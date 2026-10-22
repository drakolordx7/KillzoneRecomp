#include "kz_widescreen.h"

#include "kzgs.h"
#include "ps2_runtime.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Game functions (SCUS-97402), found with Ghidra (docs/findings.md "Ultrawide"):
//   0x1C1960 renders the 3D world for one view slot (a0 game, a1 render slot, a2 camera object, a3 render camera). The camera
//            (class at 0x3C8B78 ...) has near +0x68, far +0x6C, vertical field of view in degrees +0x70, aspect +0x74. The
//            player update 0x21E0A0 sets aspect = 4/3 * (16:9 flag ? 4/3 : 1) = 16/9 and fov = 75 * wide / aspect = 56.25.
//            The matrices and culling planes are built from the camera by 0x3C8E70, so widening +0x74 around the call
//            gives a wider view with the same vertical field of view.
//   0x220018 first-person weapon: its own frustum 0x16E2D8(f12 fov, f13 aspect, f14 near, a3 far, a0 out) with
//            aspect = camera+0x74 and fov = 60 * wide / aspect. The aspect passed to 0x16E2D8 is widened there.
//   0x16E420 orthographic matrix (f12 left, f13 right, f14 bottom, f15 top, f16 near, f17 far, a0 out), called by 0x151288
//            whenever the render context switches to 2D mode (screen pixels -> clip space). Widening [left, right] around
//            the centre compresses everything 2D into the middle of the frame.
//   0x167718 text (f12 x, f13 y, a0 -> float x/y scale, a1 string, a2 font, a3 colour): glyph positions are computed on the CPU in pixels.
//   0x15CF60 screen rectangle fill (f12 x, f13 y, f14 width, f15 height): full-width rectangles are extended over the widened area.
//   0x150090 the frame kick (D1 CHCR store).
namespace
{
    constexpr uint32_t kRamMask = 0x01FFFFFFu;
    constexpr uint32_t kWorld = 0x001C1960u;
    constexpr uint32_t kViewmodel = 0x00220018u;
    constexpr uint32_t kFrustum = 0x0016E2D8u;
    constexpr uint32_t kOrtho = 0x0016E420u;
    constexpr uint32_t kKick = 0x00150090u;
    constexpr uint32_t kLimiter = 0x001BFF10u;
    constexpr uint32_t kBufferIndex = 0x0055A6B4u;
    constexpr uint32_t kText = 0x00167718u;
    constexpr uint32_t kRect = 0x0015CF60u;
    constexpr uint32_t kViewBuild = 0x003C8E70u; // builds the render camera's planes/matrices from the view struct that 0x1C1960 filled (trace only)
    constexpr float kFrameWidth = 512.0f; // viewport of the render context (0x559118 -> scratchpad 0x70003A00, +0x10..+0x1C)
    constexpr float kNativeAspect = 16.0f / 9.0f;

    std::atomic<float> g_target{0.0f};
    bool g_log = false;
    bool g_worldWide = false; // EE thread: a widened world view was drawn in the frame being built
    // The game builds frame N into command buffer idx (*(0x55A6B4)), the vblank handler kicks that buffer later (flipping idx) while the game
    // already builds frame N+1, and it also kicks every vblank when nothing new was built. The frame type is therefore recorded per buffer
    // when the frame's build ends (the frame limiter 0x1BFF10 runs once per main-loop pass) and read when that buffer is kicked.
    bool g_bufWide[2] = {false, false};
    bool g_bufValid[2] = {false, false};
    bool g_built = false; // something was drawn since the last limiter call
    std::atomic<bool> g_latched{false};
    bool g_inViewmodel = false;
    // Wrappers that change something around a call of the original and undo it afterwards are only correct when the original ran to its end.
    // The guest yields at call checkpoints (vblank handler, thread switches: 100+ per second), the original then returns at once with
    // ctx->pc somewhere inside the function (or at a callee it was about to call) and continues later through the function table, not through
    // the wrapper. Undoing the change right then lets the rest of the call run unmodified: 0x1C1960 yielded before it had built the view
    // from the camera in a few percent of the frames, which were then built with the 16:9 camera and presented stretched (GitHub issue
    // #10, a flicker between the widened and the stretched 16:9 world). A change is therefore only undone at once when the call completed
    // (ctx->pc == $ra) and otherwise stays pending until the next hook that runs after the call has finished (flushPending).
    uint32_t g_pendCam = 0; // camera object whose aspect (+0x74) is still widened
    float g_pendCamAspect = 0.0f;
    uint32_t g_pendScale = 0; // text scale pointer whose x scale is still divided
    float g_pendScaleValue = 0.0f;
    uint32_t g_yields = 0;
    bool g_defer = true; // KZ_WIDE_DEFER=0: undo at once even after a yield (the behaviour before the issue #10 fix, for A/B runs)
    float g_k = 1.0f; // aspect factor of the widened view, valid while g_worldWide

    PS2Runtime::RecompiledFunction g_origViewBuild = nullptr, g_origWorld = nullptr, g_origViewmodel = nullptr, g_origFrustum = nullptr, g_origOrtho = nullptr, g_origKick = nullptr, g_origText = nullptr, g_origLimiter = nullptr, g_origRect = nullptr;

    uint32_t rd32(const uint8_t *rdram, uint32_t a)
    {
        uint32_t v;
        std::memcpy(&v, rdram + (a & kRamMask), 4);
        return v;
    }
    float rdF(const uint8_t *rdram, uint32_t a)
    {
        float v;
        std::memcpy(&v, rdram + (a & kRamMask), 4);
        return v;
    }
    void wrF(uint8_t *rdram, uint32_t a, float v) { std::memcpy(rdram + (a & kRamMask), &v, 4); }
    bool ramPtr(uint32_t a) { return a >= 0x00100000u && a < 0x02000000u; }

    void flushPending(uint8_t *rdram)
    {
        if (g_pendCam)
        {
            wrF(rdram, g_pendCam + 0x74, g_pendCamAspect);
            g_pendCam = 0;
        }
        if (g_pendScale)
        {
            wrF(rdram, g_pendScale, g_pendScaleValue);
            g_pendScale = 0;
        }
        g_inViewmodel = false;
    }

    float factor()
    {
        const float t = g_target.load(std::memory_order_relaxed);
        return t > kNativeAspect * 1.01f ? t / kNativeAspect : 1.0f;
    }

    void logEv(const char *what, uint32_t a = 0, float f = 0.0f)
    {
        if (g_log)
            std::printf("[kz-wide] %s %08x %.4f\n", what, a, f);
    }

    void world(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const bool entry = ctx->pc == kWorld;
        const uint32_t cam = getRegU32(ctx, 6);
        float saved = 0.0f;
        bool widened = false;
        if (entry)
        {
            flushPending(rdram);
            logEv("world", rd32(rdram, 0x0055A6B4u), 0.0f);
            const float k = factor();
            if (k > 1.0f && ramPtr(cam))
            {
                saved = rdF(rdram, cam + 0x74);
                if (std::fabs(saved - kNativeAspect) < 0.02f)
                {
                    wrF(rdram, cam + 0x74, saved * k);
                    widened = true;
                    g_worldWide = true;
                    g_k = k;
                }
            }
        }
        const uint32_t ra = getRegU32(ctx, 31);
        g_origWorld(rdram, ctx, rt);
        if (widened)
        {
            if (ctx->pc == ra || !g_defer)
                wrF(rdram, cam + 0x74, saved);
            else // the guest yielded inside: the camera stays widened until the call has really finished
            {
                g_pendCam = cam;
                g_pendCamAspect = saved;
                ++g_yields;
                logEv("world-yield", ctx->pc, 1.0f);
            }
        }
    }

    void viewBuild(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t s = getRegU32(ctx, 5);
        if (g_log && ctx->pc == kViewBuild && getRegU32(ctx, 31) == 0x001C1A64u && ramPtr(s))
        {
            // struct +0x30..0x3C = left, right, top, bottom of the view window at the near plane: (right - left) / (top - bottom) is the aspect
            const float w = rdF(rdram, s + 0x34) - rdF(rdram, s + 0x30), h = rdF(rdram, s + 0x38) - rdF(rdram, s + 0x3C);
            std::printf("[kz-wide] viewbuild wide=%d aspect=%.4f\n", g_worldWide ? 1 : 0, h != 0.0f ? w / h : 0.0f);
        }
        g_origViewBuild(rdram, ctx, rt);
    }

    void viewmodel(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const bool entry = ctx->pc == kViewmodel;
        if (entry)
        {
            flushPending(rdram);
            logEv("viewmodel");
            g_inViewmodel = g_worldWide;
        }
        const uint32_t ra = getRegU32(ctx, 31);
        g_origViewmodel(rdram, ctx, rt);
        if (entry && (ctx->pc == ra || !g_defer)) // when it yielded inside, the flag stays set until the next flush
            g_inViewmodel = false;
    }

    void frustum(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        if (ctx->pc == kFrustum)
        {
            logEv("frustum", 0, ctx->f[13]);
            if (g_inViewmodel)
                ctx->f[13] *= g_k;
        }
        g_origFrustum(rdram, ctx, rt);
    }

    void ortho(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        if (ctx->pc == kOrtho)
        {
            flushPending(rdram);
            g_built = true;
            // The render context lives in scratchpad (0x70003A00), so the viewport width is not read: the frame is 512 wide.
            const float vw = kFrameWidth;
            if (g_log)
                std::printf("[kz-wide] ortho f12=%.2f f13=%.2f wide=%d\n", ctx->f[12], ctx->f[13], g_worldWide ? 1 : 0);
            if (g_worldWide && vw > 0.0f && std::fabs(ctx->f[12]) < 0.5f && std::fabs(ctx->f[13] - vw) < 0.5f)
            {
                const float pad = vw * (g_k - 1.0f) * 0.5f;
                ctx->f[12] = -pad;
                ctx->f[13] = vw + pad;
            }
        }
        g_origOrtho(rdram, ctx, rt);
    }

    // Text is built on the CPU in screen pixels (not through the ortho matrix): start x and the glyph x scale are compressed here.
    void text(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t scalePtr = getRegU32(ctx, 4);
        const uint32_t ra = getRegU32(ctx, 31);
        if (ctx->pc == kText && g_pendScale)
            flushPending(rdram); // a previous string yielded in the middle: its scale is put back first
        const bool apply = ctx->pc == kText && g_worldWide && ramPtr(scalePtr);
        float savedScale = 0.0f;
        if (apply)
        {
            const float c = kFrameWidth * 0.5f;
            savedScale = rdF(rdram, scalePtr);
            ctx->f[12] = c + (ctx->f[12] - c) / g_k;
            wrF(rdram, scalePtr, savedScale / g_k);
        }
        g_origText(rdram, ctx, rt);
        if (apply)
        {
            if (ctx->pc == ra || !g_defer)
                wrF(rdram, scalePtr, savedScale);
            else // yielded inside the string: the rest of it still needs the compressed scale
            {
                g_pendScale = scalePtr;
                g_pendScaleValue = savedScale;
            }
        }
    }

    void limiter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        if (ctx->pc == kLimiter)
            flushPending(rdram);
        if (ctx->pc == kLimiter)
            logEv("limiter", (rd32(rdram, kBufferIndex) & 1u) | (g_built ? 0x10u : 0u) | (g_worldWide ? 0x100u : 0u));
        if (ctx->pc == kLimiter && g_built)
        {
            g_built = false;
            const uint32_t idx = rd32(rdram, kBufferIndex) & 1u;
            g_bufWide[idx] = g_worldWide;
            g_bufValid[idx] = true;
            g_worldWide = false;
        }
        g_origLimiter(rdram, ctx, rt);
    }

    // Screen rectangle fill (f12 x, f13 y, f14 width, f15 height, f16/f17 uv, a0 colour): a rectangle that covers the whole frame width
    // (fades, dimming, letterbox bars) is extended over the widened 2D area so it still covers the whole screen.
    void rect(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        if (ctx->pc == kRect && g_worldWide && std::fabs(ctx->f[12]) < 0.5f && std::fabs(ctx->f[14] - kFrameWidth) < 0.5f)
        {
            logEv("fullrect", getRegU32(ctx, 4), ctx->f[15]);
            const float pad = kFrameWidth * (g_k - 1.0f) * 0.5f;
            ctx->f[12] = -pad;
            ctx->f[14] = kFrameWidth + 2.0f * pad;
        }
        g_origRect(rdram, ctx, rt);
    }

    void kick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        if (ctx->pc == kKick)
        {
            const uint32_t idx = rd32(rdram, kBufferIndex) & 1u;
            if (g_bufValid[idx])
            {
                g_latched.store(g_bufWide[idx], std::memory_order_relaxed);
                g_bufValid[idx] = false;
            }
            logEv("kick", idx, g_latched.load(std::memory_order_relaxed) ? 1.0f : 0.0f);
        }
        g_origKick(rdram, ctx, rt);
    }

    // KZ_WIDE_LOG: trace-only wrappers
    struct Trace
    {
        uint32_t addr = 0;
        const char *name = "";
        PS2Runtime::RecompiledFunction orig = nullptr;
    };
    template <int N>
    struct TraceWrap
    {
        static inline Trace t;
        static void fn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
        {
            if (ctx->pc == t.addr)
                logEv(t.name, rd32(rdram, 0x0055A6B4u), 0.0f);
            t.orig(rdram, ctx, rt);
        }
    };
    template <int N>
    void traceInstall(PS2Runtime &rt, uint32_t addr, const char *name)
    {
        TraceWrap<N>::t.addr = addr;
        TraceWrap<N>::t.name = name;
        TraceWrap<N>::t.orig = rt.hasFunction(addr) ? rt.lookupFunction(addr) : nullptr;
        if (TraceWrap<N>::t.orig)
            rt.replaceFunction(addr, &TraceWrap<N>::fn);
    }

    bool hook(PS2Runtime &rt, uint32_t addr, PS2Runtime::RecompiledFunction fn, PS2Runtime::RecompiledFunction &orig)
    {
        orig = rt.hasFunction(addr) ? rt.lookupFunction(addr) : nullptr;
        return orig && rt.replaceFunction(addr, fn);
    }
}

float kzWideTargetAspect() { return g_target.load(std::memory_order_relaxed); }

void kzWideConfigure(KzAspect aspect, int windowWidth, int windowHeight)
{
    float target = 0.0f;
    switch (aspect)
    {
    case KzAspect::UltrawideAuto:
        if (windowWidth > 0 && windowHeight > 0)
            target = static_cast<float>(windowWidth) / static_cast<float>(windowHeight);
        break;
    case KzAspect::Ultrawide21x9: target = 43.0f / 18.0f; break; // 3440x1440
    case KzAspect::Ultrawide32x9: target = 32.0f / 9.0f; break;
    default: break;
    }
    if (const char *v = std::getenv("KZ_WIDE_ASPECT")) // automation override
        target = static_cast<float>(std::atof(v));
    const float before = kzWideTargetAspect();
    kzWideSetTargetAspect(target);
    if (kzWideTargetAspect() != before)
        std::printf("[kz] ultrawide: view aspect %.4f (window %dx%d)\n",kzWideTargetAspect(), windowWidth, windowHeight);
}

void kzWideSetTargetAspect(float aspect)
{
    const float a = aspect > kNativeAspect * 1.01f ? aspect : 0.0f;
    g_target.store(a, std::memory_order_relaxed);
    kzgsSetWideRatio(a);
}

bool kzWideTakeFrameFlag() { return g_latched.load(std::memory_order_relaxed); }

void kzWideInstall(PS2Runtime &runtime)
{
    g_log = std::getenv("KZ_WIDE_LOG") != nullptr;
    g_defer = !(std::getenv("KZ_WIDE_DEFER") && std::getenv("KZ_WIDE_DEFER")[0] == '0');
    if (const char *v = std::getenv("KZ_WIDE_ASPECT")) // automation: view aspect ratio of the widened frames
        kzWideSetTargetAspect(static_cast<float>(std::atof(v)));
    const bool ok = hook(runtime, kWorld, &world, g_origWorld) && hook(runtime, kViewmodel, &viewmodel, g_origViewmodel) &&
                    hook(runtime, kFrustum, &frustum, g_origFrustum) && hook(runtime, kOrtho, &ortho, g_origOrtho) &&
                    hook(runtime, kKick, &kick, g_origKick) && hook(runtime, kLimiter, &limiter, g_origLimiter) && hook(runtime, kText, &text, g_origText) && hook(runtime, kRect, &rect, g_origRect);
    if (g_log)
    {
        hook(runtime, kViewBuild, &viewBuild, g_origViewBuild);
        traceInstall<0>(runtime, 0x001C0148u, "renderframe");
        traceInstall<1>(runtime, 0x0026B340u, "hud");
        traceInstall<2>(runtime, 0x001EF640u, "postfx");
        traceInstall<3>(runtime, 0x001C3040u, "c3040");
    }
    std::printf("[kz] ultrawide hooks: %s\n", ok ? "installed" : "FAILED");
}
