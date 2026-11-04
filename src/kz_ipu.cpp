#include "kz_ipu.h"
#include "kz_addrs.h"

#include "kzipu.h"

#include "ps2_runtime.h"
#include "runtime/ps2_host_ipu.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_mpeg_pacing.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <array>
#include <utility>
#include <vector>

namespace
{
    PS2Runtime *g_runtime = nullptr;
    bool g_active = false;

    uint8_t *dmaPtr(void *, uint32_t addr, uint32_t bytes, bool)
    {
        if (!g_runtime)
            return nullptr;
        PS2Memory &mem = g_runtime->memory();
        if (addr & 0x80000000u) // SPR bit: scratchpad
        {
            const uint32_t off = addr & 0x3FF0u;
            return off + bytes <= PS2_SCRATCHPAD_SIZE ? mem.getScratchpad() + off : nullptr;
        }
        addr &= 0x1FFFFFF0u;
        return addr + bytes <= PS2_RAM_SIZE ? mem.getRDRAM() + addr : nullptr;
    }

    void onIntc(void *)
    {
        if (g_runtime)
            g_runtime->eeScheduler().dispatchIrq(false, 8u); // INTC_IPU
    }

    void onDmacIrq(void *, int channel)
    {
        ps2HostCompleteDmac(static_cast<uint32_t>(channel));
        if (g_runtime)
            g_runtime->drainCompletedDmacHandlers(g_runtime->memory().getRDRAM());
    }

    void onLog(void *, int level, const char *msg)
    {
        if (level <= 1)
            std::cerr << "[kzipu] " << msg << std::endl;
    }

    bool handles(uint32_t a) { return kzipuHandlesAddress(a); }
    uint32_t read32(uint32_t a)
    {
        kzipuStep();
        return kzipuReadReg32(a);
    }
    void write32(uint32_t a, uint32_t v)
    {
        kzipuWriteReg32(a, v);
        kzipuStep();
    }
    uint64_t read64(uint32_t a)
    {
        kzipuStep();
        return kzipuReadReg64(a);
    }
    void write64(uint32_t a, uint64_t v)
    {
        kzipuWriteReg64(a, v);
        kzipuStep();
    }
    void fifoWrite(const void *qw)
    {
        kzipuWriteInFifo(qw, 1);
        kzipuStep();
    }
    void fifoRead(void *qw)
    {
        kzipuStep();
        if (kzipuReadOutFifo(qw, 1) == 0)
            std::memset(qw, 0, 16);
    }
    void dmacEnable(bool enabled)
    {
        kzipuSetDmacEnabled(enabled);
        kzipuStep();
    }
    void poll() { kzipuStep(); }
}


// ---- KZ_IPU_TRACE=1: logs calls of the video player's stream/file functions (debug aid) -----------------------------
namespace
{
    struct TraceFn
    {
        uint32_t addr;
        const char *name;
        PS2Runtime::RecompiledFunction orig;
        uint64_t calls;
    };
    std::vector<TraceFn> g_trace;

    uint32_t guest32(uint8_t *rdram, uint32_t addr)
    {
        uint32_t v = 0;
        addr &= 0x01FFFFFFu;
        if (addr + 4 <= PS2_RAM_SIZE)
            std::memcpy(&v, rdram + addr, 4);
        return v;
    }

    template <size_t I>
    void traceThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        TraceFn &t = g_trace[I];
        const uint64_t n = ++t.calls;
        const bool log = n <= 40 || (n & (n - 1)) == 0;
        const uint32_t a0 = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5), a2 = getRegU32(ctx, 6), a3 = getRegU32(ctx, 7);
        const uint32_t ra = getRegU32(ctx, 31);
        char extra[160] = "";
        if (t.addr == kzAddrs().fileReadAsync || t.addr == kzAddrs().filePoll)
        {
            const uint32_t dev = guest32(rdram, a0 + 8), vt = guest32(rdram, dev + 4);
            std::snprintf(extra, sizeof(extra), " dev=0x%x vt=0x%x +28=0x%x +38=0x%x h=0x%x", dev, vt,
                          guest32(rdram, vt + 0x28), guest32(rdram, vt + 0x38), guest32(rdram, a0 + 0xC));
        }
        t.orig(rdram, ctx, rt);
        if (t.addr == kzAddrs().stRead) // stream read: log the total delivered when it first returns 0 (EOF or stall)
        {
            static uint32_t s_stream = 0, s_total = 0, s_lastLogged = ~0u;
            if (a0 != s_stream) { s_stream = a0; s_total = 0; s_lastLogged = ~0u; }
            const uint32_t got = getRegU32(ctx, 2);
            s_total += static_cast<int32_t>(got) > 0 ? got : 0;
            if (got == 0 && s_lastLogged != s_total)
            {
                s_lastLogged = s_total;
                std::fprintf(stderr, "[ipu-trace] stream 0x%x returned 0 after 0x%x bytes\n", a0, s_total);
            }
        }
        if (log)
        {
            const bool returned = ctx->pc == ra;
            std::fprintf(stderr, "[ipu-trace] %s#%llu(a0=0x%x a1=0x%x a2=0x%x a3=0x%x) %s v0=0x%x ra=0x%x%s\n", t.name,
                         static_cast<unsigned long long>(n), a0, a1, a2, a3, returned ? "->" : "(yield)",
                         getRegU32(ctx, 2), ra, extra);
        }
    }

    template <size_t... Is>
    constexpr std::array<PS2Runtime::RecompiledFunction, sizeof...(Is)> makeThunks(std::index_sequence<Is...>)
    {
        return {&traceThunk<Is>...};
    }

    void installTrace(PS2Runtime &runtime)
    {
        const char *spec = std::getenv("KZ_IPU_TRACE");
        if (!spec || !*spec)
            return;
        const KzAddrs &k = kzAddrs();
        const std::pair<uint32_t, const char *> kDefault[] = {
            {k.movieOpen, "movieOpen"}, {k.stOpen, "stOpen"}, {k.stRead, "stRead"},
            {k.fileReadAsync, "fileReadAsync"}, {k.filePoll, "filePoll"}, {k.movieFeed, "feed"},
            {k.movieInputEnd, "inputEnd"}, {k.audioFull, "audioFull"}, {k.audioFeed, "audioFeed"},
        };
        std::vector<std::pair<uint32_t, const char *>> fns(std::begin(kDefault), std::end(kDefault));
        // extra addresses: KZ_IPU_TRACE=0x1234,0x5678
        static std::vector<std::string> names;
        for (const char *p = spec; *p;)
        {
            char *end = nullptr;
            const unsigned long v = std::strtoul(p, &end, 16);
            if (end == p)
                break;
            if (v > 1)
            {
                names.push_back("fn_" + std::to_string(v));
                fns.push_back({static_cast<uint32_t>(v), nullptr});
            }
            p = (*end == ',') ? end + 1 : end;
        }
        static const auto thunks = makeThunks(std::make_index_sequence<32>{});
        g_trace.reserve(thunks.size());
        size_t extraName = 0;
        for (const auto &[addr, name] : fns)
        {
            if (g_trace.size() >= thunks.size())
                break;
            PS2Runtime::RecompiledFunction orig = runtime.hasFunction(addr) ? runtime.lookupFunction(addr) : nullptr;
            if (!orig)
                continue;
            char buf[16];
            std::snprintf(buf, sizeof(buf), "0x%x", addr);
            const char *n = name ? name : (names[extraName++] = buf).c_str();
            g_trace.push_back({addr, n, orig, 0});
            runtime.replaceFunction(addr, thunks[g_trace.size() - 1]);
        }
        std::cerr << "[ipu-trace] tracing " << g_trace.size() << " functions" << std::endl;
    }
}

bool kzIpuInstall()
{
    // On by default; KZ_IPU=off skips every video instead (movie-open override in kz_overrides.cpp).
    const char *mode = std::getenv("KZ_IPU");
    if (mode && std::strcmp(mode, "off") == 0)
    {
        std::cout << "[kz] IPU: off (KZ_IPU=off), videos are skipped" << std::endl;
        return false;
    }
    KzipuConfig cfg;
    cfg.dmaPtr = &dmaPtr;
    cfg.intc = &onIntc;
    cfg.dmacIrq = &onDmacIrq;
    cfg.log = &onLog;
    if (!kzipuInit(cfg))
    {
        std::cerr << "[kz] IPU init failed; videos are skipped" << std::endl;
        return false;
    }
    PS2HostIpu hooks;
    hooks.handles = &handles;
    hooks.read32 = &read32;
    hooks.write32 = &write32;
    hooks.read64 = &read64;
    hooks.write64 = &write64;
    hooks.fifoWrite = &fifoWrite;
    hooks.fifoRead = &fifoRead;
    hooks.dmacEnable = &dmacEnable;
    hooks.poll = &poll;
    ps2SetHostIpu(hooks);
    std::cout << "[kz] IPU: PCSX2 IPU (kzipu), videos enabled" << std::endl;
    g_active = true;
    return true;
}

// End of a movie file. The player's read callback (player +0x64 = FUN_002deb98, reading the disc stream object,
// vtable 0x52EB28) returns 0 both for "no data yet" and at the end of the file, and the feed loop (FUN_002e0ac0) retries
// a blocking read forever on 0; its only way out is a negative read, which also flags end of input (player +0x160).
// On the PS2 the decoder reaches the sequence end code before the player asks for data past the end. The runtime's
// libmpeg HLE decodes with more lookahead and can ask once more at the very end. So:
//  - a read at the end of the file returns -1 (the feed loop stops), and
//  - the end-of-input check (FUN_002df800) ignores that one +0x160, leaving the decision to sceMpegIsEnd, as on the
//    PS2; the player then ends or loops the movie normally. A second end-of-file read for the same player without new
//    data keeps the flag (the stream really is stuck).
namespace
{
    // kzAddrs().movieCurrentPlayer (DAT_0055b3d8): the movie player being serviced
    PS2Runtime::RecompiledFunction g_movieReadOrig = nullptr;
    PS2Runtime::RecompiledFunction g_inputEndOrig = nullptr;
    uint32_t g_eofPlayer = 0u;
    int g_eofReads = 0;

    void movieReadAtEofFails(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t stream = getRegU32(ctx, 6);
        const uint32_t ra = getRegU32(ctx, 31);
        g_movieReadOrig(rdram, ctx, rt);
        if (ctx->pc != ra)
            return; // yielded mid-read (the result is produced later)
        const uint32_t player = guest32(rdram, kzAddrs().movieCurrentPlayer);
        if (getRegU32(ctx, 2) != 0u)
        {
            if (player == g_eofPlayer)
                g_eofReads = 0;
            return;
        }
        if (guest32(rdram, stream + 4) != kzAddrs().discStreamVtable || guest32(rdram, stream + 0x20) != 0u)
            return; // no data yet, not the end
        if (player != g_eofPlayer)
        {
            g_eofPlayer = player;
            g_eofReads = 0;
        }
        ++g_eofReads;
        setReturnS32(ctx, -1);
    }

    void movieInputEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t player = guest32(rdram, kzAddrs().movieCurrentPlayer);
        if (player != 0u && player == g_eofPlayer && g_eofReads == 1)
        {
            const uint32_t flag = (player + 0x160u) & 0x01FFFFFFu;
            if (flag + 4 <= PS2_RAM_SIZE)
                std::memset(rdram + flag, 0, 4);
        }
        g_inputEndOrig(rdram, ctx, rt);
    }
}

// Movie picture pacing (issues #25, #26, #32). FUN_002df080 (PAL 0x2DFB80) is the movie object's per-frame step (vtable
// +0x20; a0 = movie object, the player and its sceMpeg handle at a0 + 0x28). Its third argument says whether to show the
// current picture again (non-zero, once there is one) or fetch a new one (0: FUN_002df6e8 -> sceMpegGetPicture). Both
// callers, the blocking player FUN_0026ECC8 and the front end's background movie (0x1A8EBC), pass "vsync counter parity
// differs from the parity latched at the first call", i.e. a new picture every other vblank:
// 29.97 pictures per second at the PS2's 59.94 Hz. Here the guest vblank runs at the frame-rate setting, so they asked
// for 30..120 pictures per second (60 at 120 Hz, 120 at 240 Hz). The libmpeg HLE then parked the thread inside
// GetPicture until each picture's presentation tick, so the loop that also feeds the movie audio to the SPU2 and draws
// the menu missed vblanks (intro at 120 Hz: ~24 waits per second, gaps between pictures up to 19 ticks instead of 4,
// 28.9 pictures per second). The flag now comes from the HLE's movie clock (the MPEG stream's own timestamps on the vsync tick):
// a new picture exactly when it is due, otherwise the current one, and GetPicture no longer has to wait.
// KZ_MOVIE_PACE=0 restores the game's own flag.
namespace
{
    PS2Runtime::RecompiledFunction g_movieStepOrig = nullptr;
    constexpr uint32_t kMovieStepFromInputEnd = 0x780u; // FUN_002df800 - FUN_002df080 (PAL 0x2E0300 - 0x2DFB80)
    constexpr uint32_t kMoviePlayerOffset = 0x28u;

    void moviePacedStep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t movie = getRegU32(ctx, 4);
        const bool due = ps2_stubs::mpegPictureDue(movie + kMoviePlayerOffset, rt->eeScheduler().currentVSyncTick());
        ctx->r[6] = _mm_set_epi64x(0, due ? 0 : 1); // $a2: 0 = fetch a new picture, 1 = keep the current one
        g_movieStepOrig(rdram, ctx, rt);
    }

    void installMoviePacing(PS2Runtime &runtime)
    {
        const char *v = std::getenv("KZ_MOVIE_PACE");
        if (v && *v == '0')
        {
            std::cout << "[kz] movie pacing: the game's own (KZ_MOVIE_PACE=0)" << std::endl;
            return;
        }
        const uint32_t step = kzAddrs().movieInputEnd - kMovieStepFromInputEnd;
        if (!runtime.hasFunction(step))
        {
            std::cerr << "[kz] movie pacing hook FAILED" << std::endl;
            return;
        }
        g_movieStepOrig = runtime.lookupFunction(step);
        runtime.replaceFunction(step, &moviePacedStep);
    }
}

void kzIpuBindRuntime(PS2Runtime &runtime)
{
    g_runtime = &runtime;
    if (g_active)
        installMoviePacing(runtime);
    const uint32_t kMovieRead = kzAddrs().movieRead;
    const uint32_t kInputEnd = kzAddrs().movieInputEnd;
    if (g_active && runtime.hasFunction(kMovieRead) && runtime.hasFunction(kInputEnd))
    {
        g_movieReadOrig = runtime.lookupFunction(kMovieRead);
        runtime.replaceFunction(kMovieRead, &movieReadAtEofFails);
        g_inputEndOrig = runtime.lookupFunction(kInputEnd);
        runtime.replaceFunction(kInputEnd, &movieInputEnd);
    }
    installTrace(runtime); // last, so it wraps the shims above
}

bool kzIpuActive()
{
    return g_active;
}
