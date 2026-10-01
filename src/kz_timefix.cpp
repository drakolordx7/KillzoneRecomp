#include "kz_timefix.h"

#include "ps2_runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Game functions (SCUS-97402):
//   0x23E8C8 FUN_0023e8c8(pad, state, float *turnSpeed, flag, float *accum, float *start): the stick look acceleration of
//            the JoystickControllerPlayerImp update (0x23EE08). state+0x18 is the turn value (stick^3 * sensitivity).
//            Every frame it adds |turn| * turnSpeed * (pi/180) to *accum, a count of "degrees turned" that is only reset
//            when the stick is released, and once *accum passes 15 degrees (0.2618) the turn value is multiplied by
//            1 + smoothstep((now - *start) / 1 s), i.e. a held stick speeds up to 2x over a second. The step is per
//            *frame*, not per second, so the delay until the acceleration starts depends on the frame rate: full stick
//            8 frames (0.27 s at the original 30 fps, 0.13 s at 60 fps, 0.07 s at 120 fps), half stick about 60 frames
//            (2.0 s at 30 fps, 0.5 s at 120 fps). The step is scaled by dt * 30 here (dt = the frame timer's elapsed
//            ticks * seconds per tick), which equals the original at 30 fps and keeps the delay the same in seconds.
//   0x21C550 ApplyLook(f12 dt, f13 yawDelta, f14 pitchDelta, a0 player): the camera lag (player+0x2A8 yaw, +0x2AC pitch: the
//            view trails the aim, pad play only, enabled by byte player+0x16C and off while mounted) is stored as
//            lag = (lag - k * lookStep) * 10 * (0.1 - dt), an Euler step of a decay with time constant 0.1 s. The
//            original runs it at 30 fps (factor 0.667 per frame), where the discrete system settles with tau 0.0822 s and
//            trails the aim by 0.0316 s at 0.5 rad/s; at 60-144 fps the same formula (factor 1 - 10 dt) trails by
//            +21..24 % (0.038-0.039 s) and settles 8-11 % slower. After the call the stored lag is multiplied by
//            exp(-dt / 0.075) / (10 * (0.1 - dt)), i.e. the decay is exponential in time: trail within -11..+12 % of the
//            original at 30..inf fps (+6 % at 120 fps), settle time 0.075 s (-9 %).
namespace
{
    constexpr uint32_t kLookAccel = 0x0023E8C8u;
    constexpr uint32_t kApplyLook = 0x0021C550u;
    constexpr float kLagTau = 0.075f;
    constexpr uint32_t kGameObjectPtr = 0x00559178u;
    constexpr uint32_t kNowSeconds = 0x0055F0FCu; // DAT_0055f0fc: the engine clock in seconds (float)
    constexpr uint32_t kRamMask = 0x01FFFFFFu;

    PS2Runtime::RecompiledFunction g_origLookAccel = nullptr;
    PS2Runtime::RecompiledFunction g_origApplyLook = nullptr; // whatever was installed first (kz_aim's wrapper, or the game's own)
    bool g_log = false;

    uint32_t rd32(const uint8_t *ram, uint32_t a)
    {
        uint32_t v;
        std::memcpy(&v, ram + (a & kRamMask), 4);
        return v;
    }
    float rdf(const uint8_t *ram, uint32_t a)
    {
        float f;
        std::memcpy(&f, ram + (a & kRamMask), 4);
        return f;
    }
    void wrf(uint8_t *ram, uint32_t a, float f) { std::memcpy(ram + (a & kRamMask), &f, 4); }

    float frameDt(const uint8_t *ram)
    {
        const uint32_t obj = rd32(ram, kGameObjectPtr);
        if (obj < 0x00100000u || obj >= 0x02000000u)
            return 0.0f;
        return static_cast<float>(rd32(ram, obj + 0x64)) * rdf(ram, obj + 0x50);
    }

    void applyLook(uint8_t *ram, R5900Context *ctx, PS2Runtime *rt)
    {
        if (ctx->pc != kApplyLook)
        {
            g_origApplyLook(ram, ctx, rt);
            return;
        }
        const uint32_t player = getRegU32(ctx, 4);
        const uint32_t ra = getRegU32(ctx, 31);
        const float dt = ctx->f[12];
        g_origApplyLook(ram, ctx, rt);
        if (ctx->pc != ra || !(dt > 0.0f && dt < 0.095f))
            return; // yielded (finishes in guest code), or the step is out of the formula's range
        const uint32_t ctl = rd32(ram, player + 0x31C);
        if (ctl < 0x00100000u || ctl >= 0x02000000u || ram[(player + 0x16C) & kRamMask] == 0 || rd32(ram, ctl + 0x454) == 2u)
            return; // no lag in this state
        const float c = std::exp(-dt / kLagTau) / (10.0f * (0.1f - dt));
        wrf(ram, player + 0x2A8, rdf(ram, player + 0x2A8) * c);
        wrf(ram, player + 0x2AC, rdf(ram, player + 0x2AC) * c);
    }

    void lookAccel(uint8_t *ram, R5900Context *ctx, PS2Runtime *rt)
    {
        if (ctx->pc != kLookAccel)
        {
            g_origLookAccel(ram, ctx, rt); // resumed after a yield: not our entry
            return;
        }
        const uint32_t state = getRegU32(ctx, 5);
        const uint32_t speedPtr = getRegU32(ctx, 6);
        const uint32_t flag = getRegU32(ctx, 7);
        const uint32_t accumPtr = getRegU32(ctx, 8);
        const uint32_t startPtr = getRegU32(ctx, 9);
        const float now = rdf(ram, kNowSeconds);
        const float turn = rdf(ram, state + 0x18);
        uint32_t turnBits;
        std::memcpy(&turnBits, &turn, 4);
        if (static_cast<int>((turnBits >> 23) & 0xFFu) - 0x7F < -0x4F)
        {
            wrf(ram, accumPtr, 0.0f);
            wrf(ram, startPtr, now);
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
        const float dt = frameDt(ram);
        const float scale = (dt > 0.0f) ? std::min(dt * 30.0f, 4.0f) : 1.0f;
        const float accum = rdf(ram, accumPtr) + std::fabs(turn) * rdf(ram, speedPtr) * 0.017453294f * scale;
        wrf(ram, accumPtr, accum);
        const bool idle = flag == 0u || (ram[(state + 8) & kRamMask] != 0) || accum <= 0.26179942f;
        if (idle)
        {
            wrf(ram, startPtr, now);
        }
        else
        {
            float x = now - rdf(ram, startPtr);
            x = x > 0.0f ? x : 0.0f;
            x = x < 1.0f ? x : 1.0f;
            const float s = x * x * (3.0f - (x + x));
            wrf(ram, state + 0x18, turn + turn * s);
        }
        if (g_log)
            std::printf("[kz-timefix] dt=%.5f scale=%.3f turn=%.4f accum=%.4f\n", dt, scale, turn, accum);
        ctx->pc = getRegU32(ctx, 31);
    }
}

void kzTimeFixInstall(PS2Runtime &runtime)
{
    if (const char *v = std::getenv("KZ_TIMEFIX"); v && std::strcmp(v, "0") == 0)
    {
        std::printf("[kz] frame-rate fixes: off (KZ_TIMEFIX=0)\n");
        return;
    }
    g_log = std::getenv("KZ_TIMEFIX_LOG") != nullptr;
    g_origLookAccel = runtime.hasFunction(kLookAccel) ? runtime.lookupFunction(kLookAccel) : nullptr;
    const bool ok = g_origLookAccel && runtime.replaceFunction(kLookAccel, &lookAccel);
    // After kzAimInstall (which wraps ApplyLook too): this wrapper calls whatever is registered now.
    g_origApplyLook = runtime.hasFunction(kApplyLook) ? runtime.lookupFunction(kApplyLook) : nullptr;
    const char *lagOff = std::getenv("KZ_TIMEFIX_LAG"); // =0: leave the camera lag as the game computes it (A/B tests)
    const bool lagWanted = !(lagOff && lagOff[0] == '0');
    const bool ok2 = g_origApplyLook && (!lagWanted || runtime.replaceFunction(kApplyLook, &applyLook));
    std::printf("[kz] frame-rate fixes: look acceleration %s, camera lag %s\n", ok ? "installed" : "FAILED",
                !lagWanted ? "off (KZ_TIMEFIX_LAG=0)" : (ok2 ? "installed" : "FAILED"));
}
