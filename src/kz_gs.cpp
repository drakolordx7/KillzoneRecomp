#include "kz_gs.h"
#include "kz_timing.h"
#include "kz_patches.h"
#include "kz_gpu.h"

#include "kzgs.h"

#include "ps2_runtime.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/ps2_host_gs.h"
#include "runtime/ps2_timeline.h"

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
        if (const char *r = std::getenv("KZ_RENDERER")) // automation override: d3d11 | d3d12 | vulkan
            k.renderer = _stricmp(r, "d3d12") == 0 ? KzgsRenderer::D3D12 : _stricmp(r, "vulkan") == 0 ? KzgsRenderer::Vulkan : KzgsRenderer::D3D11;
        // Auto: smallest multiple of 448 lines that covers the window height.
        k.upscale = c.upscale > 0 ? c.upscale : std::clamp((std::max(windowHeight, 448) + 447) / 448, 1, 8);
        if (const char *u = std::getenv("KZ_UPSCALE")) // automation override
            k.upscale = std::clamp(std::atoi(u), 1, 8);
        k.textureFiltering = c.bilinear ? KzgsTextureFilter::PS2 : KzgsTextureFilter::Nearest;
        k.anisotropy = c.anisotropy;
        k.fxaa = c.fxaa;
        k.sharpPresent = c.sharpScaling;
        k.casSharpness = c.sharpen;
        if (const char *v = std::getenv("KZ_FXAA")) // automation overrides
            k.fxaa = std::atoi(v) != 0;
        if (const char *v = std::getenv("KZ_SHARP_SCALING"))
            k.sharpPresent = std::atoi(v) != 0;
        if (const char *v = std::getenv("KZ_SHARPEN"))
            k.casSharpness = std::clamp(std::atoi(v), 0, 100);
        switch (c.aspect)
        {
        case KzAspect::Widescreen16x9: k.aspect = KzgsAspect::Ratio16_9; break;
        case KzAspect::Classic4x3: k.aspect = KzgsAspect::Ratio4_3; break;
        case KzAspect::Stretch: k.aspect = KzgsAspect::Stretch; break;
        }
        k.vsync = c.vsync;
        // The game's GameIndex setting (halfPixelOffset=Native) leaves the top/left half native pixel of an upscaled frame
        // unwritten, so it shows stale VRAM as a dotted line (docs/findings.md, "Display edge artifacts"). The picture
        // starts 2 lines below the top edge (DISPLAY.DY), so the rim is on merged line 2 and column 0. Crop 4 columns
        // each side and 3/4 lines top/bottom: 8:7 in total, the 512:448 aspect, so the presented picture keeps its
        // shape and no letterbox row appears. KZ_CROP=l,t,r,b overrides (KZ_CROP=0,0,0,0 shows the uncropped frame).
        k.crop[0] = 4;
        k.crop[1] = 3;
        k.crop[2] = 4;
        k.crop[3] = 4;
        if (const char *cr = std::getenv("KZ_CROP"))
            std::sscanf(cr, "%d,%d,%d,%d", &k.crop[0], &k.crop[1], &k.crop[2], &k.crop[3]);
        // PCSX2's vertex-shader sprite expansion corrupts frames on NVIDIA under D3D11; the CPU path costs nothing measurable.
        k.vertexShaderExpand = k.renderer != KzgsRenderer::D3D11;
        if (const char *ve = std::getenv("KZ_VS_EXPAND")) // automation: force 0/1
            k.vertexShaderExpand = std::atoi(ve) != 0;
        static const std::string gpu = kzPreferredGpuName();
        k.adapter = gpu;
        return k;
    }

    // Runtime GSRegisters -> PCSX2 GSPrivRegSet layout (see kzgs.h), into `out` (0x2000 bytes).
    void snapshotPrivRegs(GsState &s, uint8_t *out)
    {
        const GSRegisters &r = s.runtime->memory().gs();
        auto put = [out](uint32_t off, uint64_t v) { std::memcpy(out + off, &v, 8); };
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

    bool savePng(const char *path, std::vector<uint8_t> &rgba, int w, int h)
    {
        for (size_t i = 3; i < rgba.size(); i += 4)
            rgba[i] = 0xFF;
        SDL_Surface *surf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, rgba.data(), w * 4);
        if (!surf)
            return false;
        const bool ok = SDL_SavePNG(surf, path);
        SDL_DestroySurface(surf);
        return ok;
    }

    // KZ_SHOT_DIR frames. gs_*.png = the GS output at internal resolution. KZ_SHOT_PRESENT=1 also writes gp_*.png = the
    // image as presented in the window (aspect-corrected and scaled to the window size, KZ_WINDOW_SIZE=WxH in automation).
    void saveShot(GsState &s, long long seconds)
    {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        char name[96];
        const int index = s.shotIndex++;
        if (kzgsReadback(rgba, w, h) && w > 0 && h > 0)
        {
            std::snprintf(name, sizeof(name), "/gs_%03d_t%llds_f%llu.png", index, seconds, static_cast<unsigned long long>(s.frames.load(std::memory_order_relaxed)));
            savePng((s.shotDir + name).c_str(), rgba, w, h);
        }
        static const bool present = std::getenv("KZ_SHOT_PRESENT") && std::getenv("KZ_SHOT_PRESENT")[0] == '1';
        if (present)
        {
            int ww = 1280, wh = 896;
            if (const char *v = std::getenv("KZ_WINDOW_SIZE"))
                std::sscanf(v, "%dx%d", &ww, &wh);
            if (kzgsReadback(rgba, w, h, ww, wh) && w > 0 && h > 0)
            {
                std::snprintf(name, sizeof(name), "/gp_%03d_t%llds_f%llu.png", index, seconds, static_cast<unsigned long long>(s.frames.load(std::memory_order_relaxed)));
                savePng((s.shotDir + name).c_str(), rgba, w, h);
            }
        }
    }

    // GIF packets since the last presented vsync (runVsync skips presenting unchanged frames, see there).
    std::atomic<uint64_t> g_packetsSinceVsync{0};
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

    // KZ_GS_HASH=1: one line per vsync with the number of GIF packets per path and an order-sensitive FNV-1a hash of
    // every packet since the previous vsync (path, size, bytes). Used to compare the packet stream of the synchronous
    // and the threaded VIF1 path (same game state -> same hashes). Only touched from the thread that feeds kzgs.
    struct FrameHash
    {
        bool enabled = std::getenv("KZ_GS_HASH") && std::getenv("KZ_GS_HASH")[0] == '1';
        uint64_t h = 0xcbf29ce484222325ull;
        uint32_t packets[4] = {};
        void add(const void *p, size_t n)
        {
            const uint8_t *b = static_cast<const uint8_t *>(p);
            for (size_t i = 0; i < n; ++i)
                h = (h ^ b[i]) * 0x100000001b3ull;
        }
    };
    FrameHash &frameHash()
    {
        static FrameHash f;
        return f;
    }

    void onGifPacket(int path, const uint8_t *data, uint32_t sizeBytes)
    {
        if (FrameHash &fh = frameHash(); fh.enabled)
        {
            const uint32_t hdr[2] = {static_cast<uint32_t>(path), sizeBytes};
            fh.add(hdr, sizeof(hdr));
            fh.add(data, sizeBytes);
            if (path >= 1 && path <= 3)
                ++fh.packets[path];
        }
        if (trace().active(gs().frames.load(std::memory_order_relaxed)))
            traceRecord(1, static_cast<uint32_t>(path), data, sizeBytes);
        if (path >= 1 && path <= 3)
        {
            // One thread feeds kzgs at a time (the VIF1 worker, or the EE thread with the worker idle): plain
            // load+store instead of a locked add.
            g_pathPackets[path].store(g_pathPackets[path].load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
            g_pathQwords[path].store(g_pathQwords[path].load(std::memory_order_relaxed) + sizeBytes / 16u, std::memory_order_relaxed);
            static const bool dbg = std::getenv("KZ_GS_DEBUG") && std::getenv("KZ_GS_DEBUG")[0] == '1';
            if (dbg)
                inspectGif(data, sizeBytes);
        }
        kzgsGifTransfer(path, data, sizeBytes / 16u);
        g_packetsSinceVsync.store(g_packetsSinceVsync.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
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

    // The GS half of a guest vblank. With the runtime's VIF1 worker thread it runs on that thread, after every GIF packet
    // queued before the vblank (PS2HostGs::vsyncOrdered); otherwise inline. `job` carries the privileged registers as the
    // EE saw them at the vblank.
    struct VsyncJob
    {
        int field = 0;
        alignas(16) uint8_t regs[0x2000] = {};
    };

    // kzgs trace hook -> timeline recorder (KZ_TIMELINE); see ext/kzgs/include/kzgs.h for the ids.
    void onKzgsTrace(int id, uint32_t a)
    {
        using namespace ps2tl;
        static const Id map[] = {Count, GS_IDLE_B, GS_IDLE_E, GS_VSYNC_B, GS_VSYNC_E, GS_THROTTLE_B, GS_THROTTLE_E, GS_RING_B, GS_RING_E, GS_QUEUED};
        if (id >= 1 && id <= 9)
            rec(map[id], a);
    }

    void runVsyncBody(VsyncJob *rawJob);

    void runVsync(void *arg)
    {
        ps2tl::Span tl(ps2tl::VSYNC_B, ps2tl::VSYNC_E);
        runVsyncBody(static_cast<VsyncJob *>(arg));
    }

    void runVsyncBody(VsyncJob *rawJob)
    {
        std::unique_ptr<VsyncJob> job(rawJob);
        GsState &s = gs();
        if (!s.attached.load(std::memory_order_acquire))
            return;
        const int field = job->field;
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
        std::memcpy(s.privRegs, job->regs, sizeof(s.privRegs));
        if (trace().active(s.frames.load(std::memory_order_relaxed)))
        {
            traceRecord(2, static_cast<uint32_t>(field), s.privRegs, sizeof(s.privRegs));
            std::fflush(trace().f);
        }
        else if (trace().f && s.frames.load(std::memory_order_relaxed) == trace().start + trace().frames)
            std::fflush(trace().f), std::fprintf(stderr, "[kz] GS trace complete\n");
        if (FrameHash &fh = frameHash(); fh.enabled)
        {
            std::fprintf(stderr, "[gshash] f=%llu p1=%u p2=%u p3=%u h=%016llx\n",
                         static_cast<unsigned long long>(s.frames.load(std::memory_order_relaxed)), fh.packets[1],
                         fh.packets[2], fh.packets[3], static_cast<unsigned long long>(fh.h));
            fh.h = 0xcbf29ce484222325ull;
            fh.packets[1] = fh.packets[2] = fh.packets[3] = 0;
        }
        // At high guest vblank rates most vblanks show a frame the GS already presented (no new GIF packets, same
        // display registers). Presenting it again costs a full GS vsync and blocks the VIF1 worker whenever the GS has
        // maxQueuedFrames queued, which is what capped high vblank rates. KZ_GS_PRESENT_ALL=1 presents every vblank.
        static const bool presentAll = std::getenv("KZ_GS_PRESENT_ALL") && std::getenv("KZ_GS_PRESENT_ALL")[0] == '1';
        static uint8_t lastDisplayRegs[0xF0] = {};
        static bool presentedOnce = false;
        const bool newPackets = g_packetsSinceVsync.exchange(0, std::memory_order_relaxed) != 0;
        const bool regsChanged = std::memcmp(lastDisplayRegs, s.privRegs, sizeof(lastDisplayRegs)) != 0;
        if (presentAll || !presentedOnce || newPackets || regsChanged)
        {
            std::memcpy(lastDisplayRegs, s.privRegs, sizeof(lastDisplayRegs));
            presentedOnce = true;
            kzgsVsync(field, regsChanged || !presentedOnce);
        }
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

    void onVsync(uint64_t, int field)
    {
        GsState &s = gs();
        kzTimingOnVsync(s.runtime->memory().getRDRAM());
        kzPatchesApply(s.runtime->memory().getRDRAM(), kzConfig());
        auto *job = new VsyncJob;
        job->field = field;
        snapshotPrivRegs(s, job->regs);
        ps2RunAfterQueuedGif(&runVsync, job);
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
    if (const char *tl = std::getenv("KZ_TIMELINE"); tl && *tl) // the hook costs a clock read per GS idle wait: only when recording
        kzgsSetTraceHook(&onKzgsTrace);
    s.windowHeight = windowHeight;
    snapshotPrivRegs(s, s.privRegs);
    KzgsConfig kc = toKzgs(cfg, windowHeight);
    if (!kzgsOpen(hwnd, kc, s.privRegs, error))
        return false;
    runtime.gs().setRasterBackend(std::make_unique<SkipDrawBackend>());
    PS2HostGs hooks;
    hooks.gifPacket = &onGifPacket;
    hooks.vsync = &onVsync;
    hooks.vsyncOrdered = true; // onVsync sends its GS half through ps2RunAfterQueuedGif
    // Killzone never reads GS memory back (traces: only host->local transfers), so kzgs alone consumes the GIF
    // stream. KZ_SW_GS=1 keeps the runtime's software GS processing in parallel (debug).
    hooks.exclusive = !(std::getenv("KZ_SW_GS") && std::getenv("KZ_SW_GS")[0] == '1');
    ps2SetHostGs(hooks);
    s.attached = true;
    static const char *kRendererNames[] = {"D3D11", "D3D12", "Vulkan"};
    const int rendererIndex = static_cast<int>(kc.renderer);
    std::cout << "[kz] hardware GS attached (" << (rendererIndex >= 0 && rendererIndex < 3 ? kRendererNames[rendererIndex] : "?")
              << ", upscale " << kc.upscale << "x)" << std::endl;
    return true;
}

void kzGsDetach()
{
    GsState &s = gs();
    if (!s.attached.load())
        return;
    ps2FlushQueuedGif(); // vsync jobs still queued on the VIF1 worker use kzgs
    s.attached = false;
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
