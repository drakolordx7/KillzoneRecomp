#include "kz_vu.h"
#include "kz_crash.h"

#include "kzvu.h"
#include "kzvu0_capture.h"

#include "ps2_runtime.h"
#include "runtime/ps2_host_vu.h"
#include "runtime/ps2_host_vu0.h"
#include "runtime/ps2_dma_stats.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace
{
    PS2Runtime *g_runtime = nullptr;
    uint64_t g_codeGeneration = ~0ull;
    constexpr uint32_t kBudget = 1u << 24; // run programs to their E bit (the runtime's VIF1 is synchronous)

    void onXgkick(void *, const uint8_t *packet, uint32_t bytes, uint32_t)
    {
        if (ps2DmaStatsEnabled())
        {
            PS2DmaStats &st = ps2DmaStats();
            st.vu1Xgkick.fetch_add(1, std::memory_order_relaxed);
            st.vu1XgkickBytes.fetch_add(bytes, std::memory_order_relaxed);
            if (bytes >= 16u)
            {
                uint64_t tag = 0;
                std::memcpy(&tag, packet, sizeof(tag));
                if ((tag >> 46) & 1u)
                    st.vu1XgkickPrim[(tag >> 47) & 7u].fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (g_runtime)
            g_runtime->memory().submitGifPacket(GifPathId::Path1, packet, bytes);
    }

    void prepare(uint32_t top, uint32_t itop, uint32_t fbrst, uint64_t codeGeneration)
    {
        if (codeGeneration != g_codeGeneration)
        {
            kzvuMicroWritten(0, kKzvuCodeSize);
            g_codeGeneration = codeGeneration;
        }
        kzvuSetFBRST(fbrst);
        kzvuSetTop(top, itop);
    }

    void finish(uint32_t *vpuStat)
    {
        for (int i = 0; i < 16 && kzvuRunning() && (kzvuVpuStat() & 0x0600u) == 0u; ++i)
            kzvuContinue(kBudget);
        if (vpuStat)
            *vpuStat = (*vpuStat & ~0x0600u) | (kzvuVpuStat() & 0x0600u);
    }

    void onMscal(uint32_t startPc, uint32_t top, uint32_t itop, uint32_t fbrst, uint64_t gen, uint32_t *vpuStat)
    {
        prepare(top, itop, fbrst, gen);
        const uint64_t c0 = kzvuCycles();
        kzvuExecute(startPc, kBudget);
        finish(vpuStat);
        if (ps2DmaStatsEnabled())
            ps2DmaStats().vu1Cycles.fetch_add(kzvuCycles() - c0, std::memory_order_relaxed);
    }

    void onMscnt(uint32_t top, uint32_t itop, uint32_t fbrst, uint64_t gen, uint32_t *vpuStat)
    {
        prepare(top, itop, fbrst, gen);
        kzvuExecute(0xFFFFFFFFu, kBudget);
        finish(vpuStat);
    }

    // ---- VU0 micro mode (VCALLMS / VCALLMSR) -------------------------------------------------------------------------
    // A VU0 program is at most 512 instruction pairs; 1M cycles is far beyond any real program and only stops a
    // runaway loop (kzvu force-stops it and warns).
    constexpr uint32_t kVu0Budget = 1u << 20;
    uint64_t g_vu0CodeGeneration = ~0ull;

    void contextToVu0(const R5900Context *ctx, Kzvu0Regs &r)
    {
        std::memcpy(r.vf, ctx->vu0_vf, sizeof(r.vf));
        for (int i = 0; i < 16; ++i)
            r.vi[i] = ctx->vi[i];
        std::memcpy(r.acc, &ctx->vu0_acc, sizeof(r.acc));
        r.status = ctx->vu0_status;
        r.mac = ctx->vu0_mac_flags;
        r.clip = ctx->vu0_clip_flags;
        std::memcpy(&r.r, &ctx->vu0_r, sizeof(r.r)); // lane 0
        std::memcpy(&r.i, &ctx->vu0_i, sizeof(r.i));
        std::memcpy(&r.q, &ctx->vu0_q, sizeof(r.q));
    }

    void vu0ToContext(const Kzvu0Regs &r, R5900Context *ctx)
    {
        std::memcpy(&ctx->vu0_vf[1], r.vf[1], 31 * 16);
        ctx->vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
        ctx->vi[0] = 0;
        for (int i = 1; i < 16; ++i)
            ctx->vi[i] = static_cast<uint16_t>(r.vi[i]);
        std::memcpy(&ctx->vu0_acc, r.acc, sizeof(r.acc));
        ctx->vu0_status = static_cast<uint16_t>(r.status);
        ctx->vu0_mac_flags = r.mac;
        ctx->vu0_clip_flags = r.clip;
        ctx->vu0_clip_flags2 = r.clip;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int32_t>(r.r)));
        std::memcpy(&ctx->vu0_i, &r.i, sizeof(r.i));
        std::memcpy(&ctx->vu0_q, &r.q, sizeof(r.q));
    }

    // KZ_VU0_DUMP=<dir>: writes Kzvu0Capture files (ext/kzvu/include/kzvu0_capture.h) for ext/kzvu/test/kzvu0_test.
    //   KZ_VU0_DUMP_AFTER=<s>   start capturing this many seconds after startup (default 0)
    //   KZ_VU0_DUMP_PER=<n>     calls captured per (start PC, caller PC) pair (default 2)
    //   KZ_VU0_DUMP_MAX=<n>     total files (default 64)
    //   KZ_VU0_DUMP_FINITE=1    only calls whose input VF/ACC and data memory hold no NaN/Inf
    // KZ_VU0_STATS=1: every 10 s, one line with VU0 call count, VU cycles and host time spent in VU0.
    struct Vu0Debug
    {
        std::filesystem::path dir;
        double after = 0.0;
        uint32_t perSite = 2;
        uint32_t maxFiles = 64;
        bool finiteOnly = false;
        uint32_t written = 0;
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> perSiteCount;
        bool stats = false;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point lastReport = start;
        uint64_t calls = 0;
        uint64_t hostNs = 0;
        uint64_t nanIn = 0;  // calls whose input VF/ACC held a NaN/Inf (the PS2 has neither)
        uint64_t nanNew = 0; // calls that produced a NaN/Inf from finite inputs
        int nanInLogged = 0;
        int nanNewLogged = 0;
        std::map<uint32_t, uint64_t> callsPerPc;
    };
    std::unique_ptr<Vu0Debug> g_vu0Debug;
    uint32_t g_vu0CallIndex = 0;

    void setupVu0Debug()
    {
        const char *dump = std::getenv("KZ_VU0_DUMP");
        const char *stats = std::getenv("KZ_VU0_STATS");
        if (!(dump && *dump) && !(stats && std::strcmp(stats, "1") == 0))
            return;
        g_vu0Debug = std::make_unique<Vu0Debug>();
        g_vu0Debug->stats = stats && std::strcmp(stats, "1") == 0;
        if (dump && *dump)
        {
            g_vu0Debug->dir = dump;
            std::error_code ec;
            std::filesystem::create_directories(g_vu0Debug->dir, ec);
            if (const char *v = std::getenv("KZ_VU0_DUMP_AFTER"))
                g_vu0Debug->after = std::atof(v);
            if (const char *v = std::getenv("KZ_VU0_DUMP_PER"))
                g_vu0Debug->perSite = static_cast<uint32_t>(std::atoi(v));
            if (const char *v = std::getenv("KZ_VU0_DUMP_FINITE"))
                g_vu0Debug->finiteOnly = std::strcmp(v, "1") == 0;
            if (const char *v = std::getenv("KZ_VU0_DUMP_MAX"))
                g_vu0Debug->maxFiles = static_cast<uint32_t>(std::atoi(v));
            std::cout << "[kz] VU0: capturing calls to " << g_vu0Debug->dir.string() << " after "
                      << g_vu0Debug->after << " s" << std::endl;
        }
    }

    // First VF register (1-31, 32 = ACC) holding a NaN or Inf, or -1.
    int firstNonFinite(const Kzvu0Regs &r)
    {
        for (int i = 1; i < 33; ++i)
        {
            const uint32_t *v = i < 32 ? r.vf[i] : r.acc;
            for (int l = 0; l < 4; ++l)
                if ((v[l] & 0x7F800000u) == 0x7F800000u)
                    return i;
        }
        return -1;
    }

    void onVu0CallDebug(R5900Context *ctx, uint32_t startPc)
    {
        Vu0Debug &d = *g_vu0Debug;
        const auto t0 = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(t0 - d.start).count();

        std::unique_ptr<Kzvu0Capture> cap;
        if (!d.dir.empty() && d.written < d.maxFiles && elapsed >= d.after)
        {
            bool finite = true;
            if (d.finiteOnly)
            {
                Kzvu0Regs r;
                contextToVu0(ctx, r);
                finite = firstNonFinite(r) < 0;
                const uint32_t *mem = reinterpret_cast<const uint32_t *>(kzvu0DataMem());
                for (uint32_t w = 0; finite && w < kKzvu0DataSize / 4; ++w)
                    finite = (mem[w] & 0x7F800000u) != 0x7F800000u;
            }
            uint32_t &n = d.perSiteCount[{startPc, ctx->pc}];
            if (finite && n < d.perSite)
            {
                ++n;
                cap = std::make_unique<Kzvu0Capture>();
                cap->startPc = startPc;
                cap->callerPc = ctx->pc;
                cap->fbrst = ctx->vu0_fbrst;
                cap->callIndex = g_vu0CallIndex;
                cap->itop = ctx->vu0_itop;
                contextToVu0(ctx, cap->in);
                for (int i = 0; i < 32; ++i)
                {
                    kzvuGetVF(i, cap->vu1VfIn[i]);
                    cap->vu1ViIn[i] = kzvuGetVI(i);
                }
                std::memcpy(cap->code, kzvu0CodeMem(), kKzvu0CodeSize);
                std::memcpy(cap->dataIn, kzvu0DataMem(), kKzvu0DataSize);
            }
        }

        const uint64_t c0 = kzvu0Cycles();
        Kzvu0Regs regs;
        contextToVu0(ctx, regs);
        const int badIn = d.stats ? firstNonFinite(regs) : -1;
        kzvu0SetRegs(regs);
        kzvuSetFBRST(ctx->vu0_fbrst);
        kzvu0Execute(startPc, kVu0Budget);
        kzvu0GetRegs(regs);
        vu0ToContext(regs, ctx);
        const auto t1 = std::chrono::steady_clock::now();

        if (cap)
        {
            cap->out = regs;
            cap->outVpuStat = kzvu0VpuStat();
            cap->outTpc = kzvu0TPC();
            cap->cycles = static_cast<uint32_t>(kzvu0Cycles() - c0);
            for (int i = 0; i < 32; ++i)
            {
                kzvuGetVF(i, cap->vu1VfOut[i]);
                cap->vu1ViOut[i] = kzvuGetVI(i);
            }
            std::memcpy(cap->dataOut, kzvu0DataMem(), kKzvu0DataSize);
            char name[96];
            std::snprintf(name, sizeof(name), "vu0_%03u_pc%03x_ee%06x.bin", d.written, startPc, ctx->pc);
            if (FILE *f = std::fopen((d.dir / name).string().c_str(), "wb"))
            {
                std::fwrite(cap.get(), sizeof(Kzvu0Capture), 1, f);
                std::fclose(f);
                ++d.written;
            }
        }

        if (d.stats)
        {
            const int badOut = badIn < 0 ? firstNonFinite(regs) : -1;
            if (badIn >= 0)
            {
                d.nanIn++;
                if (d.nanInLogged++ < 5)
                    std::fprintf(stderr, "[kz] VU0 NaN/Inf in input vf%d: start 0x%03x caller 0x%06x call #%u\n", badIn,
                                 startPc, ctx->pc, g_vu0CallIndex);
            }
            if (badOut >= 0)
            {
                d.nanNew++;
                if (d.nanNewLogged++ < 5)
                    std::fprintf(stderr, "[kz] VU0 made NaN/Inf in vf%d from finite input: start 0x%03x caller 0x%06x call #%u\n",
                                 badOut, startPc, ctx->pc, g_vu0CallIndex);
            }
            d.calls++;
            d.hostNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            d.callsPerPc[startPc]++;
            if (t1 - d.lastReport >= std::chrono::seconds(10))
            {
                std::string top;
                for (const auto &[pc, n] : d.callsPerPc)
                {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), " %03x:%llu", pc, static_cast<unsigned long long>(n));
                    top += buf;
                }
                std::fprintf(stderr, "[kz] VU0 t=%.0fs calls=%llu nan-in=%llu nan-made=%llu vu0cycles=%llu host=%.1fms per-pc:%s\n",
                             std::chrono::duration<double>(t1 - d.start).count(),
                             static_cast<unsigned long long>(d.calls), static_cast<unsigned long long>(d.nanIn),
                             static_cast<unsigned long long>(d.nanNew),
                             static_cast<unsigned long long>(kzvu0Cycles()), d.hostNs / 1e6, top.c_str());
                d.lastReport = t1;
                d.calls = 0;
                d.hostNs = 0;
                d.nanIn = 0;
                d.nanNew = 0;
                d.callsPerPc.clear();
            }
        }
    }

    void onVu0Call(R5900Context *ctx, uint32_t startPc, uint64_t codeGeneration)
    {
        if (codeGeneration != g_vu0CodeGeneration)
        {
            kzvu0MicroWritten(0, kKzvu0CodeSize);
            g_vu0CodeGeneration = codeGeneration;
        }
        if (g_vu0Debug)
            onVu0CallDebug(ctx, startPc);
        else
        {
            Kzvu0Regs regs;
            contextToVu0(ctx, regs);
            kzvu0SetRegs(regs);
            kzvuSetFBRST(ctx->vu0_fbrst);
            kzvu0Execute(startPc, kVu0Budget);
            kzvu0GetRegs(regs);
            vu0ToContext(regs, ctx);
        }
        ++g_vu0CallIndex;
        const uint32_t tpc = kzvu0TPC();
        ctx->vu0_tpc = tpc;
        ctx->vu0_pc = tpc;
        ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & ~0xFFu) | (kzvu0VpuStat() & 0xFFu);
    }
}

bool kzVuInstall(std::string *error)
{
    kzCrashTraceInstall(); // debugging aid (KZ_CRASH_TRACE=1), installed with the first host hooks
    const char *mode = std::getenv("KZ_VU");
    const char *mode0 = std::getenv("KZ_VU0");
    const bool vu1Builtin = mode && std::strcmp(mode, "builtin") == 0;
    const bool vu0Builtin = mode0 && std::strcmp(mode0, "interp") == 0;
    if (vu1Builtin)
        std::cout << "[kz] VU1: PS2Recomp interpreter (KZ_VU=builtin)" << std::endl;
    if (vu0Builtin)
        std::cout << "[kz] VU0: PS2Recomp interpreter (KZ_VU0=interp)" << std::endl;
    if (vu1Builtin && vu0Builtin)
        return true;

    KzvuConfig vc; // Killzone GameIndex: vuClampMode 0 (both VUs), IbitHack on (the kzvu defaults)
    vc.useJit = !(mode && std::strcmp(mode, "interp") == 0);
    vc.vu0UseJit = !(mode0 && std::strcmp(mode0, "pcsx2interp") == 0);
    // VU0 clamp mode 3 (clamp operands and results to +-max, keep the sign), not the GameIndex's 0: in this port
    // NaN/Inf values (which the PS2 cannot produce) reach VU0 programs through memory from the EE side once a level
    // loads (KZ_VU0_STATS=1 counts them; VU0 itself never made one from finite inputs). Measured: with mode 0 they
    // propagate and gameplay drops to ~1.3 frames/s; mode 3 treats them like the old interpreter path did
    // (normalizeOperand) and like the PS2 treats exponent 255.
    vc.vu0ClampMode = 3;
    if (const char *clamp0 = std::getenv("KZ_VU0_CLAMP"))
        vc.vu0ClampMode = std::atoi(clamp0);
    vc.xgkick = &onXgkick;
    if (!kzvuInit(vc, error))
        return false;
    if (!vu1Builtin)
    {
        PS2HostVu1 hooks;
        hooks.codeMem = kzvuCodeMem();
        hooks.dataMem = kzvuDataMem();
        hooks.mscal = &onMscal;
        hooks.mscnt = &onMscnt;
        ps2SetHostVu1(hooks);
        std::cout << "[kz] VU1: " << (vc.useJit ? "microVU recompiler" : "PCSX2 interpreter") << std::endl;
    }
    if (!vu0Builtin)
    {
        PS2HostVu0 hooks0;
        hooks0.codeMem = kzvu0CodeMem();
        hooks0.dataMem = kzvu0DataMem();
        hooks0.callms = &onVu0Call;
        ps2SetHostVu0(hooks0);
        setupVu0Debug();
        std::cout << "[kz] VU0: " << (vc.vu0UseJit ? "microVU recompiler" : "PCSX2 interpreter") << ", clamp mode "
                  << (vc.vu0ClampMode < 0 ? vc.clampMode : vc.vu0ClampMode) << std::endl;
    }
    return true;
}

void kzVuBindRuntime(PS2Runtime &runtime)
{
    g_runtime = &runtime;
}
