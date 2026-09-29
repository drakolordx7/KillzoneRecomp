#include "kz_aim.h"

#include "kz_config.h"
#include "kz_input.h"

#include "ps2_runtime.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Game functions (SCUS-97402):
//   0x23EE08 JoystickControllerPlayerImp update(ctrl, ...). ctrl+0x18 = player entity, ctrl+0x1C = local player index
//            (-1 = unused). Writes the stick look rates: ctrl+0xA8 yaw, ctrl+0xAC pitch.
//   0x21C550 ApplyLook(f12 dt, f13 yawDelta, f14 pitchDelta, a0 player): yaw = player+0x148 + yawDelta,
//            pitch = player+0x14C + pitchDelta (the setters clamp pitch). Called once per sim step by the player update
//            0x21A8C8 with rate * max turn speed * dt.
//   0x23E9C8 pitch auto-centre while walking without look input (a gamepad assist; gated by a profile option).
namespace
{
    constexpr uint32_t kCtrlUpdate = 0x0023EE08u;
    constexpr uint32_t kApplyLook = 0x0021C550u;
    constexpr uint32_t kPitchAutoCentre = 0x0023E9C8u;

    constexpr float kRadPerCount = 0.05f * 3.14159265f / 180.0f; // at sensitivity 1.0: 0.05 degrees per count
    constexpr double kStaleSeconds = 0.25; // mouse motion older than this (menus, loading, pause) is dropped

    PS2Runtime::RecompiledFunction g_origCtrlUpdate = nullptr;
    PS2Runtime::RecompiledFunction g_origApplyLook = nullptr;
    PS2Runtime::RecompiledFunction g_origPitchAutoCentre = nullptr;

    uint32_t g_player = 0;  // local player 0's entity
    bool g_announced = false;
    bool g_log = false;
    std::chrono::steady_clock::time_point g_lastTake{};

    uint32_t rd32(const uint8_t *rdram, uint32_t addr)
    {
        uint32_t v = 0;
        addr &= 0x01FFFFFFu;
        if (addr + 4 <= PS2_RAM_SIZE)
            std::memcpy(&v, rdram + addr, 4);
        return v;
    }
    float rdf(const uint8_t *rdram, uint32_t addr)
    {
        const uint32_t v = rd32(rdram, addr);
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }

    void ctrlUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        if (ctx->pc == kCtrlUpdate)
        {
            const uint32_t ctrl = getRegU32(ctx, 4);
            if (rd32(rdram, ctrl + 0x1C) == 0u)
            {
                const uint32_t player = rd32(rdram, ctrl + 0x18);
                if (player >= 0x00100000u && player < 0x02000000u)
                {
                    if (!g_announced)
                    {
                        std::printf("[kz] mouse aim: local player 0x%x\n", player);
                        g_announced = true;
                    }
                    g_player = player;
                    kzInputSetAimPatchActive(true);
                }
            }
        }
        g_origCtrlUpdate(rdram, ctx, rt);
    }

    void applyLook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        if (ctx->pc == kApplyLook && g_player != 0u && getRegU32(ctx, 4) == g_player)
        {
            const auto now = std::chrono::steady_clock::now();
            const double since = std::chrono::duration<double>(now - g_lastTake).count();
            g_lastTake = now;
            const KzMouseDelta d = kzInputTakeMouseDelta();
            if (since < kStaleSeconds && (d.dx != 0.0f || d.dy != 0.0f))
            {
                // Stick right gives a positive yaw delta, stick up a positive pitch delta (the game negates the
                // stick Y axis); mouse down is +dy, so pitch goes the other way.
                ctx->f[13] += d.dx * kRadPerCount;
                ctx->f[14] -= d.dy * kRadPerCount;
            }
            if (g_log)
            {
                static int n = 0;
                if ((++n % 30) == 0 || d.dx != 0.0f || d.dy != 0.0f)
                    std::printf("[kz-aim] yaw=%.4f pitch=%.4f dYaw=%.4f dPitch=%.4f mouse=(%.1f,%.1f)\n",
                                rdf(rdram, g_player + 0x148), rdf(rdram, g_player + 0x14C), ctx->f[13], ctx->f[14],
                                d.dx, d.dy);
            }
        }
        g_origApplyLook(rdram, ctx, rt);
    }

    void pitchAutoCentre(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        // With keyboard/mouse the player holds pitch with the mouse; the pad assist would drag the view to the horizon.
        if (ctx->pc == kPitchAutoCentre && kzInputUsingKeyboardMouse())
        {
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
        g_origPitchAutoCentre(rdram, ctx, rt);
    }

    bool hook(PS2Runtime &rt, uint32_t addr, PS2Runtime::RecompiledFunction fn, PS2Runtime::RecompiledFunction &orig)
    {
        orig = rt.hasFunction(addr) ? rt.lookupFunction(addr) : nullptr;
        return orig && rt.replaceFunction(addr, fn);
    }
}

void kzAimInstall(PS2Runtime &runtime)
{
    if (const char *v = std::getenv("KZ_AIM"); v && std::strcmp(v, "off") == 0)
    {
        std::printf("[kz] mouse aim patch: off (KZ_AIM=off), mouse drives the right stick\n");
        return;
    }
    g_log = std::getenv("KZ_AIM_LOG") != nullptr;
    const bool ok = hook(runtime, kCtrlUpdate, &ctrlUpdate, g_origCtrlUpdate) &&
                    hook(runtime, kApplyLook, &applyLook, g_origApplyLook) &&
                    hook(runtime, kPitchAutoCentre, &pitchAutoCentre, g_origPitchAutoCentre);
    std::printf("[kz] mouse aim patch: %s\n", ok ? "installed" : "FAILED (mouse drives the right stick)");
}
