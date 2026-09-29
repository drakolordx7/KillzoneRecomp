#include "kz_gs.h"
#include "kz_timing.h"

#include "kzgs.h"

#include "ps2_runtime.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/ps2_host_gs.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
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
        return k;
    }

    // Runtime GSRegisters -> PCSX2 GSPrivRegSet layout (see kzgs.h).
    void snapshotPrivRegs(GsState &s)
    {
        const GSRegisters &r = s.runtime->memory().gs();
        auto put = [&s](uint32_t off, uint64_t v) { std::memcpy(s.privRegs + off, &v, 8); };
        put(0x000, r.pmode);
        put(0x010, r.smode1);
        put(0x020, r.smode2);
        put(0x030, r.srfsh);
        put(0x040, r.synch1);
        put(0x050, r.synch2);
        put(0x060, r.syncv);
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

    void onGifPacket(int path, const uint8_t *data, uint32_t sizeBytes)
    {
        kzgsGifTransfer(path, data, sizeBytes / 16u);
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
        kzgsVsync(field, true);
        s.frames.fetch_add(1, std::memory_order_relaxed);
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
        if (level <= 1)
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
