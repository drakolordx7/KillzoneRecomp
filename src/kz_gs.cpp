#include "kz_gs.h"
#include "kz_timing.h"
#include "kz_gpu.h"

#include "kzgs.h"

#include "ps2_runtime.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/ps2_host_gs.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>

namespace
{
    // Delegates to the CPU backend for uploads/VRAM access but skips drawing and presentation, which kzgs does.
    class SkipDrawBackend final : public GSRasterBackend
    {
    public:
        void Initialize(uint8_t *vram, uint32_t vramSize) override { m_cpu.Initialize(vram, vramSize); }
        void Reset() override { m_cpu.Reset(); }
        void Submit(const GSPrimitiveBatch &) override {}
        void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override { m_cpu.LoadClut(tex0, texclut); }
        void BeginTransfer(const GSTransferCommand &command) override { m_cpu.BeginTransfer(command); }
        void UploadImage(const uint8_t *data, uint32_t sizeBytes) override { m_cpu.UploadImage(data, sizeBytes); }
        void Flush() override {}
        void TextureFlush() override {}
        void Sync(GSSyncReason reason) override { m_cpu.Sync(reason); }
        PresentationFrame Present(const GSPresentationRequest &) override { return {}; }
        bool ClearFramebuffer(const GSContext &, uint32_t) override { return true; }
        uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override { return m_cpu.ConsumeLocalToHostBytes(dst, maxBytes); }
        uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override { return m_cpu.ReadVram(psm, base, bw, x, y); }
        void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override { m_cpu.WriteVram(psm, base, bw, x, y, value); }
        void SnapshotVram(std::vector<uint8_t> &out) const override { m_cpu.SnapshotVram(out); }
        GSTransferSnapshot GetTransferSnapshot() const override { return m_cpu.GetTransferSnapshot(); }

    private:
        GSCpuBackend m_cpu;
    };

    struct GsState
    {
        PS2Runtime *runtime = nullptr;
        alignas(16) uint8_t privRegs[0x2000] = {};
        std::atomic<bool> attached{false};
        std::atomic<uint64_t> frames{0};

        std::mutex pendingMutex; // main thread -> game thread
        bool resizePending = false;
        int resizeW = 0, resizeH = 0;
        bool configPending = false;
        KzgsConfig pendingConfig;
        int windowHeight = 0;

        std::string shotDir;
        int shotInterval = 0;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point nextShot = std::chrono::steady_clock::now();
        int shotIndex = 0;
    };

    GsState &gs()
    {
        static GsState s;
        return s;
    }

    KzgsConfig toKzgs(const KzConfig &c, int windowHeight)
    {
        KzgsConfig k;
        switch (c.renderer)
        {
        case KzRenderer::D3D11: k.renderer = KzgsRenderer::D3D11; break;
        case KzRenderer::D3D12: k.renderer = KzgsRenderer::D3D12; break;
        case KzRenderer::Vulkan: k.renderer = KzgsRenderer::Vulkan; break;
        }
        // Auto: smallest multiple of 448 lines that covers the window height.
        k.upscale = c.upscale > 0 ? c.upscale : std::clamp((std::max(windowHeight, 448) + 447) / 448, 1, 8);
        k.textureFiltering = c.bilinear ? KzgsTextureFilter::PS2 : KzgsTextureFilter::Nearest;
        k.anisotropy = c.anisotropy;
        k.fxaa = c.fxaa;
        switch (c.aspect)
        {
        case KzAspect::Widescreen16x9: k.aspect = KzgsAspect::Ratio16_9; break;
        case KzAspect::Classic4x3: k.aspect = KzgsAspect::Ratio4_3; break;
        case KzAspect::Stretch: k.aspect = KzgsAspect::Stretch; break;
        }
        k.vsync = c.vsync;
        static const std::string gpu = kzPreferredGpuName();
        k.adapter = gpu;
        return k;
    }

    // Runtime GSRegisters -> PCSX2 GSPrivRegSet layout (see kzgs.h).
    void snapshotPrivRegs(GsState &s)
    {
        const GSRegisters &r = s.runtime->memory().gs();
        auto put = [&s](uint32_t off, uint64_t v) { std::memcpy(s.privRegs + off, &v, 8); };
        // sceGsResetGraph is an HLE stub in the runtime and never programs the CRTC sync registers. PCSX2 derives the
        // video mode from SMODE1.CMOD/LC (0 = VESA, wrong display rules) and field flipping from SYNCV.VFP, so fall
        // back to the NTSC values libgraph writes.
        constexpr uint64_t kNtscSmode1 = 0x0000000740834504ull; // CMOD=2 (NTSC), LC=32
        constexpr uint64_t kNtscSyncv = 0x00C7800601A01801ull;  // VFP=1
        put(0x000, r.pmode);
        put(0x010, r.smode1 ? r.smode1 : kNtscSmode1);
        put(0x020, r.smode2);
        put(0x030, r.srfsh);
        put(0x040, r.synch1);
        put(0x050, r.synch2);
        put(0x060, r.syncv ? r.syncv : kNtscSyncv);
        put(0x070, r.dispfb1);
        put(0x080, r.display1);
        put(0x090, r.dispfb2);
        put(0x0A0, r.display2);
        put(0x0B0, r.extbuf);
        put(0x0C0, r.extdata);
        put(0x0D0, r.extwrite);
        put(0x0E0, r.bgcolor);
        put(0x1000, r.csr.load(std::memory_order_acquire));
        put(0x1010, r.imr);
        put(0x1040, r.busdir);
        put(0x1080, r.siglblid);
    }

    void saveShot(GsState &s, long long seconds)
    {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        if (!kzgsReadback(rgba, w, h) || w <= 0 || h <= 0)
            return;
        for (size_t i = 3; i < rgba.size(); i += 4)
            rgba[i] = 0xFF;
        SDL_Surface *surf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, rgba.data(), w * 4);
        if (!surf)
            return;
        char name[64];
        std::snprintf(name, sizeof(name), "/gs_%03d_t%llds.png", s.shotIndex++, seconds);
        SDL_SavePNG(surf, (s.shotDir + name).c_str());
        SDL_DestroySurface(surf);
    }

    std::atomic<uint64_t> g_pathPackets[4]{};
    // Debug: last A+D writes of interest and primitive count (PACKED/REGLIST parsing, first 64 KB of each packet).
    std::atomic<uint64_t> g_lastFrame1{0}, g_lastFrame2{0}, g_lastPrim{0}, g_primWrites{0}, g_imageTransfers{0}, g_lastBitblt{0};

    void inspectGif(const uint8_t *data, uint32_t size)
    {
        uint32_t off = 0;
        while (off + 16 <= size)
        {
            uint64_t lo, hi;
            std::memcpy(&lo, data + off, 8);
            std::memcpy(&hi, data + off + 8, 8);
            off += 16;
            const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFF);
            const bool eop = (lo >> 15) & 1;
            const bool pre = (lo >> 46) & 1;
            const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3);
            uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xF);
            if (nreg == 0)
                nreg = 16;
            if (pre)
                g_primWrites.fetch_add(1, std::memory_order_relaxed), g_lastPrim.store((lo >> 47) & 0x7FF);
            if (flg == 0) // PACKED
            {
                for (uint32_t l = 0; l < nloop && off + 16 <= size; ++l)
                    for (uint32_t r = 0; r < nreg && off + 16 <= size; ++r, off += 16)
                    {
                        const uint32_t reg = static_cast<uint32_t>((hi >> (4 * r)) & 0xF);
                        if (reg != 0xE)
                            continue;
                        uint64_t val, addr;
                        std::memcpy(&val, data + off, 8);
                        std::memcpy(&addr, data + off + 8, 8);
                        switch (addr & 0xFF)
                        {
                        case 0x4C: g_lastFrame1.store(val); break;
                        case 0x4D: g_lastFrame2.store(val); break;
                        case 0x00: g_primWrites.fetch_add(1, std::memory_order_relaxed); g_lastPrim.store(val); break;
                        case 0x50: g_lastBitblt.store(val); break;
                        case 0x53: g_imageTransfers.fetch_add(1, std::memory_order_relaxed); break;
                        default: break;
                        }
                    }
            }
            else if (flg == 1) // REGLIST
                off += ((nloop * nreg + 1) / 2) * 16;
            else // IMAGE
                off += nloop * 16;
            if (eop)
                break;
        }
    }
    std::atomic<uint64_t> g_pathQwords[4]{};

    // KZ_GS_TRACE=<file>: records GIF packets and the privileged register block at each vsync, from vsync
    // KZ_GS_TRACE_START for KZ_GS_TRACE_FRAMES frames (default 3000 / 120). Replayed by tools/gs_replay.
    struct Trace
    {
        FILE *f = nullptr;
        uint64_t start = 3000, frames = 120;
        bool active(uint64_t frame) const { return f && frame >= start && frame < start + frames; }
    };
    Trace &trace()
    {
        static Trace t = []() {
            Trace t;
            if (const char *path = std::getenv("KZ_GS_TRACE"))
            {
                t.f = std::fopen(path, "wb");
                if (const char *v = std::getenv("KZ_GS_TRACE_START")) t.start = std::strtoull(v, nullptr, 10);
                if (const char *v = std::getenv("KZ_GS_TRACE_FRAMES")) t.frames = std::strtoull(v, nullptr, 10);
            }
            return t;
        }();
        return t;
    }
    void traceRecord(uint32_t type, uint32_t arg, const void *data, uint32_t size)
    {
        Trace &t = trace();
        const uint32_t hdr[3] = {type, arg, size};
        std::fwrite(hdr, sizeof(hdr), 1, t.f);
        if (size)
            std::fwrite(data, size, 1, t.f);
    }

    void onGifPacket(int path, const uint8_t *data, uint32_t sizeBytes)
    {
        if (trace().active(gs().frames.load(std::memory_order_relaxed)))
            traceRecord(1, static_cast<uint32_t>(path), data, sizeBytes);
        if (path >= 1 && path <= 3)
        {
            g_pathPackets[path].fetch_add(1, std::memory_order_relaxed);
            g_pathQwords[path].fetch_add(sizeBytes / 16u, std::memory_order_relaxed);
            static const bool dbg = std::getenv("KZ_GS_DEBUG") && std::getenv("KZ_GS_DEBUG")[0] == '1';
            if (dbg)
                inspectGif(data, sizeBytes);
        }
        kzgsGifTransfer(path, data, sizeBytes / 16u);
    }

    // KZ_GS_DEBUG=1: one line per ~second with display registers, fade level and GIF traffic.
    void debugLine(GsState &s)
    {
        static const bool enabled = std::getenv("KZ_GS_DEBUG") && std::getenv("KZ_GS_DEBUG")[0] == '1';
        if (!enabled || (s.frames.load() % 60u) != 0u)
            return;
        const GSRegisters &r = s.runtime->memory().gs();
        const uint8_t *ram = s.runtime->memory().getRDRAM();
        uint32_t fade = 0, vs = 0, countAcc = 0, countLast = 0;
        std::memcpy(&fade, ram + 0x0055A7C8u, 4);
        std::memcpy(&vs, ram + 0x0055A6E0u, 4);
        std::memcpy(&countAcc, ram + 0x0055F0E8u, 4);
        std::memcpy(&countLast, ram + 0x0055F0F0u, 4);
        std::fprintf(stderr, "[gsdbg] smode1=%llx synch1=%llx synch2=%llx syncv=%llx srfsh=%llx\n",
                     static_cast<unsigned long long>(r.smode1), static_cast<unsigned long long>(r.synch1),
                     static_cast<unsigned long long>(r.synch2), static_cast<unsigned long long>(r.syncv),
                     static_cast<unsigned long long>(r.srfsh));
        std::fprintf(stderr, "[gsdbg] count acc=%u last=%u\n", countAcc, countLast);
        std::fprintf(stderr,
                     "[gsdbg] f=%llu vs=%u fade=%u pmode=%llx smode2=%llx dispfb1=%llx display1=%llx dispfb2=%llx display2=%llx bg=%llx "
                     "p1=%llu/%llu p2=%llu/%llu p3=%llu/%llu\n",
                     static_cast<unsigned long long>(s.frames.load()), vs, fade,
                     static_cast<unsigned long long>(r.pmode), static_cast<unsigned long long>(r.smode2),
                     static_cast<unsigned long long>(r.dispfb1), static_cast<unsigned long long>(r.display1),
                     static_cast<unsigned long long>(r.dispfb2), static_cast<unsigned long long>(r.display2),
                     static_cast<unsigned long long>(r.bgcolor),
                     static_cast<unsigned long long>(g_pathPackets[1].load()), static_cast<unsigned long long>(g_pathQwords[1].load()),
                     static_cast<unsigned long long>(g_pathPackets[2].load()), static_cast<unsigned long long>(g_pathQwords[2].load()),
                     static_cast<unsigned long long>(g_pathPackets[3].load()), static_cast<unsigned long long>(g_pathQwords[3].load()));
        std::fprintf(stderr, "[gsdbg]   frame1=%llx frame2=%llx prims=%llu lastprim=%llx trxdir=%llu bitblt=%llx\n",
                     static_cast<unsigned long long>(g_lastFrame1.load()), static_cast<unsigned long long>(g_lastFrame2.load()),
                     static_cast<unsigned long long>(g_primWrites.load()), static_cast<unsigned long long>(g_lastPrim.load()),
                     static_cast<unsigned long long>(g_imageTransfers.load()), static_cast<unsigned long long>(g_lastBitblt.load()));
    }

    void onVsync(uint64_t, int field)
    {
        GsState &s = gs();
        kzTimingOnVsync(s.runtime->memory().getRDRAM());
        {
            std::lock_guard<std::mutex> lock(s.pendingMutex);
            if (s.resizePending)
            {
                kzgsResize(s.resizeW, s.resizeH);
                s.resizePending = false;
            }
            if (s.configPending)
            {
                kzgsUpdateConfig(s.pendingConfig);
                s.configPending = false;
            }
        }
        snapshotPrivRegs(s);
        if (trace().active(s.frames.load(std::memory_order_relaxed)))
        {
            traceRecord(2, static_cast<uint32_t>(field), s.privRegs, sizeof(s.privRegs));
            std::fflush(trace().f);
        }
        else if (trace().f && s.frames.load(std::memory_order_relaxed) == trace().start + trace().frames)
            std::fflush(trace().f), std::fprintf(stderr, "[kz] GS trace complete\n");
        kzgsVsync(field, true);
        s.frames.fetch_add(1, std::memory_order_relaxed);
        debugLine(s);
        if (!s.shotDir.empty())
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= s.nextShot)
            {
                s.nextShot = now + std::chrono::seconds(s.shotInterval);
                saveShot(s, std::chrono::duration_cast<std::chrono::seconds>(now - s.start).count());
            }
        }
    }

    void onLog(int level, const char *msg)
    {
        static const bool verbose = std::getenv("KZ_GS_DEBUG") && std::getenv("KZ_GS_DEBUG")[0] == '1';
        if (level <= 1 || verbose)
            std::cerr << "[kzgs] " << msg << std::endl;
    }
}

bool kzGsAttach(PS2Runtime &runtime, void *hwnd, int windowHeight, const KzConfig &cfg, std::string *error)
{
    GsState &s = gs();
    s.runtime = &runtime;
    kzgsSetLogCallback(&onLog);
    s.windowHeight = windowHeight;
    snapshotPrivRegs(s);
    KzgsConfig kc = toKzgs(cfg, windowHeight);
    if (!kzgsOpen(hwnd, kc, s.privRegs, error))
        return false;
    runtime.gs().setRasterBackend(std::make_unique<SkipDrawBackend>());
    PS2HostGs hooks;
    hooks.gifPacket = &onGifPacket;
    hooks.vsync = &onVsync;
    ps2SetHostGs(hooks);
    s.attached = true;
    std::cout << "[kz] hardware GS attached (upscale " << kc.upscale << "x)" << std::endl;
    return true;
}

void kzGsDetach()
{
    GsState &s = gs();
    if (!s.attached.exchange(false))
        return;
    ps2SetHostGs(PS2HostGs{});
    kzgsClose();
}

void kzGsResize(int width, int height)
{
    GsState &s = gs();
    std::lock_guard<std::mutex> lock(s.pendingMutex);
    s.resizePending = true;
    s.resizeW = width;
    s.resizeH = height;
    s.windowHeight = height;
}

void kzGsApplyConfig(const KzConfig &cfg)
{
    GsState &s = gs();
    std::lock_guard<std::mutex> lock(s.pendingMutex);
    s.pendingConfig = toKzgs(cfg, s.windowHeight);
    s.configPending = true;
}

void kzGsSetScreenshotSchedule(const std::string &dir, int intervalSeconds)
{
    GsState &s = gs();
    s.shotDir = dir;
    s.shotInterval = std::max(1, intervalSeconds);
    s.start = std::chrono::steady_clock::now();
    s.nextShot = s.start + std::chrono::seconds(s.shotInterval);
    if (!dir.empty())
        std::filesystem::create_directories(dir);
}

uint64_t kzGsFrameCount()
{
    return gs().frames.load(std::memory_order_relaxed);
}
