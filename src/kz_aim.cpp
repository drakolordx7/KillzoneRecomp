#include "kz_aim.h"

#include "kz_config.h"
#include "kz_input.h"

#include "ps2_runtime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Game functions (SCUS-97402):
//   0x23EE08 JoystickControllerPlayerImp update(ctrl, ...). ctrl+0x18 = player entity, ctrl+0x1C = local player index
//            (-1 = unused). Writes the stick look rates: ctrl+0xA8 yaw (copy at ctrl+0x3C), ctrl+0xAC pitch. The player
//            update reads them as its control state (ctrl+0x24, rates at +0x84/+0x88).
//   0x21C550 ApplyLook(f12 dt, f13 yawDelta, f14 pitchDelta, a0 player): yaw = player+0x148 + yawDelta,
//            pitch = player+0x14C + pitchDelta (the setters clamp pitch). Called once per sim step by the player update
//            0x21A8C8 with rate * max turn speed * dt. It also runs the camera lag (player+0x2A8/0x2AC: the view trails
//            the aim by up to 10 degrees and eases back) and idle sway (+0x170/0x174), both enabled by byte player+0x16C.
//            In normal walking player+0x148 only keeps the lag/sway offset, so a yaw added to the delta is lost.
//   Body turn (in the player update 0x21A8C8): heading += turn * T * dt, with turn = the control state's turn value
//            (ctrl+0x3C; stick right is negative, and it ramps up to about 2x while the stick is held) and
//            T = ctl[0x190] / ctl[0x44C] (vtable +0x188 of the character controller ctl = player+0x31C: 2.0944 rad/s
//            unzoomed; ctl+0x44C is the zoom factor). The heading angle is ctl+0x78, ctl+0x454 the aim mode
//            (0 normal, 1 zoomed, 2 mounted). Measured: a turn value written in frame k is applied twice, once in
//            frame k and once more in frame k+1 before that frame's player update (a latched copy), so one frame of
//            turn changes the heading by turn * T * (dt[k] + dt[k+1]) (four mouse impulses and the stick frames
//            match to 4 digits).
//   0x23E9C8 pitch auto-centre while walking without look input (a gamepad assist; gated by a profile option).
//   0x1B0DF0 on-screen keyboard: USB keyboard reader (obj). The game supports a USB keyboard for the profile name
//            (HID codes: 0x28 enter -> obj+0x21C = 2, 0x29 escape -> 4, 0x2A backspace -> FUN_001b18d8, else add char
//            FUN_001b17a0(obj, ch)), but no USB keyboard is emulated. obj+0x21C == 1 while the keyboard is open.
namespace
{
    constexpr uint32_t kCtrlUpdate = 0x0023EE08u;
    constexpr uint32_t kApplyLook = 0x0021C550u;
    constexpr uint32_t kPitchAutoCentre = 0x0023E9C8u;
    constexpr uint32_t kVkKeyboard = 0x001B0DF0u;
    constexpr uint32_t kVkAddChar = 0x001B17A0u;
    constexpr uint32_t kVkBackspace = 0x001B18D8u;

    constexpr float kRadPerCount = 0.05f * 3.14159265f / 180.0f; // at sensitivity 1.0: 0.05 degrees per count
    constexpr double kStaleSeconds = 0.25; // mouse motion older than this (menus, loading, pause) is dropped

    PS2Runtime::RecompiledFunction g_origCtrlUpdate = nullptr;
    PS2Runtime::RecompiledFunction g_origApplyLook = nullptr;
    PS2Runtime::RecompiledFunction g_origPitchAutoCentre = nullptr;
    PS2Runtime::RecompiledFunction g_origVkKeyboard = nullptr;
    PS2Runtime::RecompiledFunction g_vkAddChar = nullptr;
    PS2Runtime::RecompiledFunction g_vkBackspace = nullptr;

    uint32_t g_player = 0; // local player 0's entity
    uint32_t g_ctrl = 0;   // its controller
    bool g_announced = false;
    bool g_log = false;
    std::chrono::steady_clock::time_point g_lastTake{};

    // Mouse yaw goes in as the turn value. Because every turn value acts in two consecutive frames, the mouse keeps a
    // debt (heading change still owed) and commands half of it per frame: the two steps of one command add up to the
    // debt at equal frame times, and the debt is charged with the frame times that really applied, so the total is
    // exact and nothing drifts. The effect is a two-frame average of the mouse motion.
    float g_yawDebt = 0.0f;       // radians of heading still to apply
    float g_prevTurn = 0.0f;      // mouse turn value commanded last frame (its second step happens this frame)
    float g_mouseRate = 0.0f;     // turn value added this frame (log)
    float g_pendingPitch = 0.0f;  // radians, applied in the next ApplyLook
    float g_ctrlDt = 0.0f;        // frame timer dt read at the controller update

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
    void wrf(uint8_t *rdram, uint32_t addr, float f)
    {
        addr &= 0x01FFFFFFu;
        if (addr + 4 <= PS2_RAM_SIZE)
            std::memcpy(rdram + addr, &f, 4);
    }
    void wr32(uint8_t *rdram, uint32_t addr, uint32_t v)
    {
        addr &= 0x01FFFFFFu;
        if (addr + 4 <= PS2_RAM_SIZE)
            std::memcpy(rdram + addr, &v, 4);
    }
    void setReg(R5900Context *ctx, int reg, uint32_t v)
    {
        ctx->r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(v)));
    }

    // This frame's simulation step from the game's frame timer (docs/findings.md "Frame timer"): elapsed ticks (+0x64)
    // times seconds per tick (+0x50) in the game object *(0x559178). 0 if unavailable.
    float frameDt(const uint8_t *rdram)
    {
        const uint32_t obj = rd32(rdram, 0x00559178u);
        if (obj < 0x00100000u || obj >= 0x02000000u)
            return 0.0f;
        return static_cast<float>(rd32(rdram, obj + 0x64)) * rdf(rdram, obj + 0x50);
    }

    // Hold to aim: the game's zoom is a toggle on R3 (action "zoommode"). With [Input] AimMode=Hold the zoom key is
    // not sent to the pad; R3 is pulsed whenever the game's zoom state (ctl+0x454 == 1) differs from the key state.
    void updateHoldAim(const uint8_t *rdram, uint32_t ctl)
    {
        static std::chrono::steady_clock::time_point nextPulse{};
        static int attempts = 0;
        if (!kzConfig().holdAim)
            return;
        const uint32_t mode = rd32(rdram, ctl + 0x454);
        if (mode > 1u)
            return; // mounted weapon / other modes: leave alone
        const bool want = kzInputZoomHeld();
        if (want == (mode == 1u))
        {
            attempts = 0;
            return;
        }
        if (!want && attempts == 0 && mode == 0u)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (now < nextPulse || attempts >= 3)
            return; // give the game time to switch; a weapon that cannot zoom is not hammered while the key is held
        kzInputPulseButton(KZ_PAD_R3, 70);
        nextPulse = now + std::chrono::milliseconds(300);
        if (want)
            ++attempts;
    }

    void ctrlUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t ra = getRegU32(ctx, 31);
        const uint32_t ctrl = getRegU32(ctx, 4);
        const bool local = ctx->pc == kCtrlUpdate && rd32(rdram, ctrl + 0x1C) == 0u;
        g_origCtrlUpdate(rdram, ctx, rt);
        if (!local || ctx->pc != ra)
            return; // not player 0, or the update yielded (it finishes later in guest code)
        const uint32_t player = rd32(rdram, ctrl + 0x18);
        if (player < 0x00100000u || player >= 0x02000000u)
            return;
        if (!g_announced)
        {
            std::printf("[kz] mouse aim: local player 0x%x\n", player);
            g_announced = true;
        }
        g_player = player;
        g_ctrl = ctrl;
        kzInputSetAimPatchActive(true);

        const auto now = std::chrono::steady_clock::now();
        const double since = std::chrono::duration<double>(now - g_lastTake).count();
        g_lastTake = now;
        const KzMouseDelta d = kzInputTakeMouseDelta();
        g_mouseRate = 0.0f;
        g_pendingPitch = 0.0f;
        g_ctrlDt = frameDt(rdram);
        if (g_log)
        {
            static const bool matrix = std::getenv("KZ_AIM_LOG") && std::getenv("KZ_AIM_LOG")[0] == '2';
            if (matrix)
                std::printf("[kz-mtc] dt=%.5f heading=%.5f\n", g_ctrlDt, rdf(rdram, rd32(rdram, player + 0x31C) + 0x78));
        }
        const uint32_t ctl = rd32(rdram, player + 0x31C);
        const float zoom = rdf(rdram, ctl + 0x44C);
        const float turnSpeed = (zoom > 0.01f) ? rdf(rdram, ctl + 0x190) / zoom : 0.0f; // rad/s at turn value 1
        const float dt = g_ctrlDt;
        updateHoldAim(rdram, ctl);
        if (since >= kStaleSeconds || !(turnSpeed > 0.01f) || !(dt > 0.0f))
        {
            g_yawDebt = 0.0f; // menus, loading, pause: drop stale motion and any pending correction
            g_prevTurn = 0.0f;
            return;
        }
        // The second step of last frame's command happens in this frame, with this frame's dt.
        g_yawDebt -= g_prevTurn * turnSpeed * dt;
        // Mouse right turns right: heading decreases (stick right gives a negative turn value). Mouse down is +dy and
        // looking up is a positive pitch delta. Zoomed, both scale with the zoom factor like the stick does.
        const float scale = kRadPerCount / std::max(zoom, 1.0f);
        g_yawDebt += -d.dx * scale;
        g_pendingPitch = -d.dy * scale;
        if (std::fabs(g_yawDebt) < 1e-7f && g_prevTurn == 0.0f)
        {
            g_yawDebt = 0.0f;
            return;
        }
        g_mouseRate = g_yawDebt / (2.0f * turnSpeed * dt);
        g_yawDebt -= g_mouseRate * turnSpeed * dt; // first step, this frame
        g_prevTurn = g_mouseRate;
        wrf(rdram, ctrl + 0x3C, rdf(rdram, ctrl + 0x3C) + g_mouseRate);
    }

    void applyLook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const bool mine = ctx->pc == kApplyLook && g_player != 0u && getRegU32(ctx, 4) == g_player;
        if (!mine)
        {
            g_origApplyLook(rdram, ctx, rt);
            return;
        }
        const float dt = ctx->f[12];
        const float rate = rdf(rdram, g_ctrl + 0x24 + 0x84);
        ctx->f[14] += g_pendingPitch;
        const float mousePitch = g_pendingPitch;
        g_pendingPitch = 0.0f;

        // Keyboard/mouse: no camera lag or idle sway (the view follows the mouse 1:1). Pad play keeps them.
        const uint32_t lagFlag = (g_player + 0x16Cu) & 0x01FFFFFFu;
        const bool kbm = kzInputUsingKeyboardMouse();
        const uint8_t savedFlag = rdram[lagFlag];
        if (kbm)
            rdram[lagFlag] = 0;
        if (g_log)
        {
            static int n = 0;
            if ((++n % 3) == 0 || g_mouseRate != 0.0f || mousePitch != 0.0f)
                std::printf("[kz-aim] ctrlDt=%.5f dt=%.5f mouseTurn=%.3f debt=%.5f dPitch=%.4f pitch=%.4f\n", g_ctrlDt, dt,
                            g_mouseRate, g_yawDebt, ctx->f[14], rdf(rdram, g_player + 0x14C));
            // KZ_AIM_LOG=2: the character controller's transform (player+0x31C, matrix at +0x50) and mode (+0x454),
            // to measure the real heading change of a turn.
            static const bool matrix = std::getenv("KZ_AIM_LOG") && std::getenv("KZ_AIM_LOG")[0] == '2';
            if (matrix)
            {
                const uint32_t ctl = rd32(rdram, g_player + 0x31C);
                std::printf("[kz-mtx] mode=%u dt=%.5f turn=%.4f rateA8=%.4f heading=%.5f\n", rd32(rdram, ctl + 0x454), dt,
                            rdf(rdram, g_ctrl + 0x3C), rate, rdf(rdram, ctl + 0x78));
            }
        }
        g_origApplyLook(rdram, ctx, rt);
        if (kbm)
            rdram[lagFlag] = savedFlag;
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

    // Physical keyboard typing on the game's on-screen keyboard: one key per frame, handed to the game's own routines
    // the way its USB keyboard path does. Add char / backspace are tail calls (a0 = obj is already set and ra is the
    // caller's), so a yield inside them resumes in guest code as usual.
    void vkKeyboard(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t obj = getRegU32(ctx, 4);
        if (ctx->pc == kVkKeyboard && rd32(rdram, obj + 0x21C) == 1u)
        {
            kzInputSetTextEntryActive();
            uint16_t key = 0;
            if (kzInputTakeTextKey(key))
            {
                if (key == KZ_TEXT_ENTER)
                    wr32(rdram, obj + 0x21C, 2u);
                else if (key == KZ_TEXT_CANCEL)
                    wr32(rdram, obj + 0x21C, 4u);
                else if (key == KZ_TEXT_BACKSPACE)
                {
                    ctx->pc = kVkBackspace;
                    g_vkBackspace(rdram, ctx, rt);
                    return;
                }
                else
                {
                    setReg(ctx, 5, key);
                    ctx->pc = kVkAddChar;
                    g_vkAddChar(rdram, ctx, rt);
                    return;
                }
            }
        }
        g_origVkKeyboard(rdram, ctx, rt);
    }

    // KZ_PAD_LOG=1: log which button ids (FUN_001b33b8(pad, id): byte pad+0x34+id) read as pressed.
    PS2Runtime::RecompiledFunction g_origButtonValue = nullptr;
    void buttonValue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t ra = getRegU32(ctx, 31);
        const int32_t id = static_cast<int32_t>(getRegU32(ctx, 5));
        g_origButtonValue(rdram, ctx, rt);
        if (ctx->pc != ra)
            return;
        static uint32_t lastMask = 0;
        static uint32_t mask = 0;
        static int calls = 0;
        if (id >= 0 && id < 32 && ctx->f[0] > 0.1f)
            mask |= 1u << id;
        if (++calls >= 64)
        {
            if (mask != lastMask)
            {
                std::printf("[kz-pad] pressed ids:");
                for (int i = 0; i < 32; ++i)
                    if (mask & (1u << i))
                        std::printf(" %d", i);
                std::printf("\n");
                lastMask = mask;
            }
            mask = 0;
            calls = 0;
        }
    }

    bool hook(PS2Runtime &rt, uint32_t addr, PS2Runtime::RecompiledFunction fn, PS2Runtime::RecompiledFunction &orig)
    {
        orig = rt.hasFunction(addr) ? rt.lookupFunction(addr) : nullptr;
        return orig && rt.replaceFunction(addr, fn);
    }
}

void kzAimInstall(PS2Runtime &runtime)
{
    // Text entry does not depend on the aim patch.
    g_vkAddChar = runtime.hasFunction(kVkAddChar) ? runtime.lookupFunction(kVkAddChar) : nullptr;
    g_vkBackspace = runtime.hasFunction(kVkBackspace) ? runtime.lookupFunction(kVkBackspace) : nullptr;
    const bool vk = g_vkAddChar && g_vkBackspace && hook(runtime, kVkKeyboard, &vkKeyboard, g_origVkKeyboard);
    std::printf("[kz] keyboard text entry: %s\n", vk ? "installed" : "FAILED");
    if (std::getenv("KZ_PAD_LOG"))
        hook(runtime, 0x001B33B8u, &buttonValue, g_origButtonValue);

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
