#include "kz_post.h"

#include "kz_config.h"

#include "ps2_runtime.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Game functions (SCUS-97402), found from a GS trace of first-mission gameplay and the PostProcessPreset reflection data
// (docs/findings.md "Post-process passes"). All three are called with the render context and draw directly into the
// frame's GIF packet:
//   0x15DFC0 glow pass: the glow mask in the frame's alpha channel goes through the glow palette, is blurred down to
//            128x112 in the Z-buffer memory and added back to the frame (additive). Called from the post-process render
//            0x1EF640 only. (The heat-vision palette variant is a direct call of 0x15E028, which is left alone.)
//   0x15F768 frame blend (f12 = alpha): blends the *previous* frame buffer over the current one with alpha f12*255, the
//            "motion blur" (PostProcessPreset "Motion blur" Strength -> PostProcess object +0x280). Also called by
//            0x1A8D30 with 0.02 after a screen-capture overlay.
//   0x1A9308 lens blur: only when the camera has a lens texture (+0x140); eight half-resolution ping-pong blur
//            passes plus the overlay.
// View distance (docs/findings.md "Draw distance and LOD"):
//   0x339C38 RenderZoneManager blend (a0 manager, a1 camera, a2 out): picks the zone around the camera (blending two
//            at a boundary) and writes out[0] far plane (min of far plane and fog end when fog is on), out+4 byte fog
//            enabled, out[2] fog start, out[3] fog end, out[4]/[5] densities, out[6] colour. Returns 1 when a zone
//            was found. The hook scales out[0], out[2], out[3] by the render distance factor.
//   0x1C2168 LOD scale from the camera's field of view (f12 = fov, a0 = game object): ctx+0x1AC = obj[0x1C] *
//            tan(refFov/2) / tan(fov/2), the divisor of every distance the LOD selectors compare with the LOD tables
//            (static: 0x3F4E70, skinned: 0x3F4F10, multi-mesh: 0x3EFAD8 ...). The hook scales obj[0x1C] around the call.
// The colour grade (0x167050 + the palette composite) and the film grain (the noise texture strips at the end of
// 0x1EF640) are not touched.
namespace
{
    constexpr uint32_t kGlowPass = 0x0015DFC0u;
    constexpr uint32_t kFrameBlend = 0x0015F768u;
    constexpr uint32_t kLensBlur = 0x001A9308u;
    constexpr uint32_t kPostRender = 0x001EF640u;      // post-process render (a0 = PostProcess object)
    constexpr uint32_t kNoiseStrengthOffset = 0x284u;  // film grain alpha; the pass is skipped below 2/255
    constexpr uint32_t kZoneParams = 0x00339C38u;
    constexpr uint32_t kLodScale = 0x001C2168u;
    constexpr uint32_t kRamMask = 0x01FFFFFFu;

    bool g_motionBlur = false;
    bool g_glow = false;
    bool g_lensBlur = false;
    bool g_grain = false;
    bool g_log = false;

    PS2Runtime::RecompiledFunction g_origGlow = nullptr;
    PS2Runtime::RecompiledFunction g_origFrameBlend = nullptr;
    PS2Runtime::RecompiledFunction g_origLensBlur = nullptr;
    PS2Runtime::RecompiledFunction g_origPostRender = nullptr;
    PS2Runtime::RecompiledFunction g_origZoneParams = nullptr;
    PS2Runtime::RecompiledFunction g_origLodScale = nullptr;
    float g_renderDistance = 1.0f;
    float g_lodScale = 1.0f;
    std::atomic<int> g_zoneCalls{0}, g_lodCalls{0};

    std::atomic<int> g_glowCalls{0}, g_blendCalls{0}, g_lensCalls{0};
    std::atomic<int> g_glowSkipped{0}, g_blendSkipped{0}, g_lensSkipped{0};

    void logCall(const char *name, std::atomic<int> &calls, std::atomic<int> &skipped, bool skip)
    {
        const int n = ++calls;
        if (skip)
            ++skipped;
        if (g_log && (n == 1 || n % 600 == 0))
            std::printf("[kz-post] %s: %d calls, %d skipped\n", name, n, skipped.load());
    }

    // A call enters at the function's own address; a resume at an inner label (after a yield) has another pc and must
    // run the original.
    bool entering(const R5900Context *ctx, uint32_t addr) { return ctx->pc == addr; }

    void glowPass(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        if (entering(ctx, kGlowPass))
        {
            logCall("glow", g_glowCalls, g_glowSkipped, !g_glow);
            if (!g_glow)
            {
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
        }
        g_origGlow(rdram, ctx, rt);
    }

    void frameBlend(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        if (entering(ctx, kFrameBlend))
        {
            logCall("frame blend", g_blendCalls, g_blendSkipped, !g_motionBlur);
            if (!g_motionBlur)
            {
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
        }
        g_origFrameBlend(rdram, ctx, rt);
    }

    void lensBlur(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        if (entering(ctx, kLensBlur))
        {
            logCall("lens blur", g_lensCalls, g_lensSkipped, !g_lensBlur);
            if (!g_lensBlur)
            {
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
        }
        g_origLensBlur(rdram, ctx, rt);
    }

    float rdF(const uint8_t *rdram, uint32_t a)
    {
        float v;
        std::memcpy(&v, rdram + (a & kRamMask), 4);
        return v;
    }
    void wrF(uint8_t *rdram, uint32_t a, float v) { std::memcpy(rdram + (a & kRamMask), &v, 4); }

    void zoneParams(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const bool entry = entering(ctx, kZoneParams);
        const uint32_t out = getRegU32(ctx, 6);
        g_origZoneParams(rdram, ctx, rt);
        // A normal return (pc == ra) with v0 = 1: a zone was found and out[] is filled in.
        if (entry && getRegU32(ctx, 2) == 1u && out >= 0x00100000u && out < 0x02000000u)
        {
            const int n = ++g_zoneCalls;
            if (g_log && (n == 1 || n % 600 == 0))
                std::printf("[kz-post] zone params: far %.1f fog %d start %.1f end %.1f (x%.2f)\n", rdF(rdram, out),
                            rdram[(out + 4) & kRamMask], rdF(rdram, out + 8), rdF(rdram, out + 12), g_renderDistance);
            wrF(rdram, out, rdF(rdram, out) * g_renderDistance);
            if (rdram[(out + 4) & kRamMask])
            {
                wrF(rdram, out + 8, rdF(rdram, out + 8) * g_renderDistance);
                wrF(rdram, out + 12, rdF(rdram, out + 12) * g_renderDistance);
            }
        }
    }

    void lodScale(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t obj = getRegU32(ctx, 4) + 0x1Cu;
        const bool entry = entering(ctx, kLodScale) && obj >= 0x00100000u && obj < 0x02000000u;
        float saved = 0.0f;
        if (entry)
        {
            saved = rdF(rdram, obj);
            wrF(rdram, obj, saved * g_lodScale);
            const int n = ++g_lodCalls;
            if (g_log && (n == 1 || n % 600 == 0))
                std::printf("[kz-post] lod scale call %d: base %.3f x%.2f\n", n, saved, g_lodScale);
        }
        g_origLodScale(rdram, ctx, rt);
        if (entry)
            wrF(rdram, obj, saved);
    }

    // Film grain off: the grain strips at the end of the post-process render are drawn only when the object's noise
    // strength (+0x284, from the active preset) is above 2/255, so it is zeroed on entry.
    void postRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        if (ctx->pc == kPostRender && !g_grain)
        {
            const uint32_t obj = getRegU32(ctx, 4);
            if (obj >= 0x00100000u && obj < 0x02000000u)
                wrF(rdram, obj + kNoiseStrengthOffset, 0.0f);
        }
        g_origPostRender(rdram, ctx, rt);
    }

    bool hook(PS2Runtime &rt, uint32_t addr, PS2Runtime::RecompiledFunction fn, PS2Runtime::RecompiledFunction &orig)
    {
        orig = rt.hasFunction(addr) ? rt.lookupFunction(addr) : nullptr;
        return orig && rt.replaceFunction(addr, fn);
    }

    bool envFlag(const char *name, bool value)
    {
        if (const char *v = std::getenv(name); v && *v)
            return std::atoi(v) != 0;
        return value;
    }
}

void kzPostInstall(PS2Runtime &runtime)
{
    const KzConfig &cfg = kzConfig();
    g_motionBlur = envFlag("KZ_MOTION_BLUR", cfg.motionBlur);
    g_glow = envFlag("KZ_GLOW", cfg.glow);
    g_grain = envFlag("KZ_GRAIN", cfg.noiseFilter);
    if (!hook(runtime, kPostRender, &postRender, g_origPostRender))
        std::printf("[kz] film grain switch: hook FAILED\n");
    g_lensBlur = envFlag("KZ_LENS_BLUR", cfg.lensBlur);
    g_log = std::getenv("KZ_POST_LOG") != nullptr;
    g_renderDistance = cfg.renderDistance;
    g_lodScale = cfg.lodScale;
    if (const char *v = std::getenv("KZ_RENDER_DISTANCE"); v && *v)
        g_renderDistance = static_cast<float>(std::atof(v));
    if (const char *v = std::getenv("KZ_LOD_SCALE"); v && *v)
        g_lodScale = static_cast<float>(std::atof(v));
    if (g_renderDistance != 1.0f || g_lodScale != 1.0f)
    {
        const bool viewOk = hook(runtime, kZoneParams, &zoneParams, g_origZoneParams) &&
                            hook(runtime, kLodScale, &lodScale, g_origLodScale);
        std::printf("[kz] view distance: render x%.2f, LOD x%.2f (%s)\n", g_renderDistance, g_lodScale,
                    viewOk ? "hooks installed" : "HOOK FAILED");
    }
    // Nothing to hook when every pass stays on.
    if (g_motionBlur && g_glow && g_lensBlur)
    {
        std::printf("[kz] post-process passes: all on (original look)\n");
        return;
    }
    const bool ok = hook(runtime, kGlowPass, &glowPass, g_origGlow) &&
                    hook(runtime, kFrameBlend, &frameBlend, g_origFrameBlend) &&
                    hook(runtime, kLensBlur, &lensBlur, g_origLensBlur);
    std::printf("[kz] post-process passes: motion blur %s, glow %s, lens blur %s (%s)\n", g_motionBlur ? "on" : "off",
                g_glow ? "on" : "off", g_lensBlur ? "on" : "off", ok ? "hooks installed" : "HOOK FAILED");
}
