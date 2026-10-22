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
    constexpr uint32_t kAfterLookProbe = 0x00218710u; // first call the player update makes after the scope block
    constexpr uint32_t kVkKeyboard = 0x001B0DF0u;
    constexpr uint32_t kVkAddChar = 0x001B17A0u;
    constexpr uint32_t kVkBackspace = 0x001B18D8u;

    constexpr float kRadPerCount = 0.05f * 3.14159265f / 180.0f; // at sensitivity 1.0: 0.05 degrees per count
    constexpr double kStaleSeconds = 0.25; // mouse motion older than this (menus, loading, pause) is dropped

    PS2Runtime::RecompiledFunction g_origCtrlUpdate = nullptr;
    PS2Runtime::RecompiledFunction g_origApplyLook = nullptr;
    PS2Runtime::RecompiledFunction g_origPitchAutoCentre = nullptr;
    PS2Runtime::RecompiledFunction g_origAfterLook = nullptr;
    uint32_t g_modeSwapCtl = 0; // controller whose aim mode 2 was shown to the player update as 1 (restored at 0x218710)
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
        // A gamepad keeps the original toggle on R3: the setting is for the mouse/keyboard zoom key only. (Applied to a
        // pad it pulsed R3 off again right after the player toggled zoom on, so the pad had to hold R3: issue #6.)
        if (!kzConfig().holdAim || !kzInputUsingKeyboardMouse())
            return;
        const uint32_t mode = rd32(rdram, ctl + 0x454);
        if (mode > 2u)
            return; // unknown mode: leave alone
        // Mode 1 is the zoom of ordinary weapons, mode 2 the zoom of scope-type weapons (sniper rifle).
        const bool zoomed = mode != 0u;
        const bool want = kzInputZoomHeld();
        if (want == zoomed)
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

    // KZ_AIM_LOG=3: scope/camera probe (research aid). Current weapon = slot set (ctl+0x1CC array, index ctl+0x1DC)
    // -> first weapon (set+0x18 array) -> definition (weapon+8: +0x10 type, +0x20 can zoom, +0x178/+0x17C zoom min/max,
    // +0x180 zoom time). Camera = player+0x3C: +0x6C far plane, +0x70 field of view (degrees), +0x74 aspect.
    void probeScope(const uint8_t *rdram, uint32_t player, uint32_t ctl)
    {
        static const bool on = std::getenv("KZ_AIM_LOG") && std::getenv("KZ_AIM_LOG")[0] == '3';
        static const bool fakeScope = std::getenv("KZ_FAKE_SCOPE") != nullptr;
        if (!on && !fakeScope)
            return;
        static int n = 0;
        static uint32_t lastKey = 0;
        const uint32_t cam = rd32(rdram, player + 0x3C);
        uint32_t def = 0;
        uint32_t weaponObj = 0;
        const uint32_t slotIdx = rd32(rdram, ctl + 0x1DC);
        if (slotIdx < rd32(rdram, ctl + 0x1C4))
        {
            const uint32_t set = rd32(rdram, rd32(rdram, ctl + 0x1CC) + slotIdx * 4);
            if (set && rd32(rdram, set + 0x10))
                if (const uint32_t w = rd32(rdram, rd32(rdram, set + 0x18)))
                {
                    weaponObj = w;
                    def = rd32(rdram, w + 8);
                }
        }
        // KZ_FAKE_SCOPE=2: give the current weapon the game's real sniper-rifle definition (type 0x15, found by a RAM sweep
        // for definitions with the current one's vtable), so the scope code path runs with its real data.
        static const bool realDef = std::getenv("KZ_FAKE_SCOPE") && std::getenv("KZ_FAKE_SCOPE")[0] == '2';
        if (realDef && def && weaponObj)
        {
            static uint32_t sniperDef = 0;
            static int tries = 0;
            if (!sniperDef && (tries++ % 240) == 0)
            {
                const uint32_t vt = rd32(rdram, def);
                for (uint32_t a = 0x100000; a < 0x02000000 && !sniperDef; a += 4)
                    if (rd32(rdram, a) == vt && rd32(rdram, a + 0x10) == 0x15u && rdf(rdram, a + 0x17C) > 5.0f)
                        sniperDef = a;
                if (sniperDef)
                    std::printf("[kz] fake scope: sniper definition 0x%x (zoom %.2f..%.2f)\n", sniperDef, rdf(rdram, sniperDef + 0x178),
                                rdf(rdram, sniperDef + 0x17C));
            }
            if (sniperDef && def != sniperDef)
            {
                wr32(const_cast<uint8_t *>(rdram), weaponObj + 8, sniperDef);
                def = sniperDef;
            }
        }
        else if (def && fakeScope)
        {
            // Test aid: turn the current weapon into a scope-type weapon (type 0x15, zoom 2.5..9) to exercise aim mode 2.
            wr32(const_cast<uint8_t *>(rdram), def + 0x10, 0x15u);
            wrf(const_cast<uint8_t *>(rdram), def + 0x178, 2.5f);
            wrf(const_cast<uint8_t *>(rdram), def + 0x17C, 9.0f);
            wrf(const_cast<uint8_t *>(rdram), def + 0x180, 1.0f);
        }
        if (!on)
            return;
        static bool swept = false;
        if (def && !swept && std::getenv("KZ_SWEEP_WEAPONS"))
        {
            swept = true;
            const uint32_t vt = rd32(rdram, def);
            std::printf("[kz-weapdefs] vtable 0x%x, current def 0x%x\n", vt, def);
            for (uint32_t a = 0x100000; a < 0x02000000; a += 4)
                if (rd32(rdram, a) == vt)
                {
                    const uint32_t t = rd32(rdram, a + 0x10);
                    if (t < 64)
                    {
                        char name[24] = {};
                        const uint32_t np = rd32(rdram, a + 0x8);
                        std::printf("[kz-weapdefs] def 0x%x type=%u canZoom=%u zmin=%.2f zmax=%.2f ztime=%.2f w4=0x%x w8=0x%x wC=0x%x w14=0x%x name=%.20s\n", a, t,
                                    rd32(rdram, a + 0x20) & 0xFF, rdf(rdram, a + 0x178), rdf(rdram, a + 0x17C), rdf(rdram, a + 0x180),
                                    rd32(rdram, a + 4), np, rd32(rdram, a + 0xC), rd32(rdram, a + 0x14),
                                    (np > 0x1000 && np < 0x02000000) ? reinterpret_cast<const char *>(rdram + np) : "");
                    }
                }
        }
        const uint32_t key = rd32(rdram, ctl + 0x454) * 1000003u + def;
        if ((++n % 60) != 0 && key == lastKey)
            return;
        lastKey = key;
        std::printf("[kz-scope] mode=%u zoom=%.3f prev=%.3f camFov=%.3f camAspect=%.4f far=%.1f off164=%.4f off168=%.4f def=0x%x type=%u canZoom=%u zmin=%.3f zmax=%.3f ztime=%.3f\n",
                    rd32(rdram, ctl + 0x454), rdf(rdram, ctl + 0x44C), rdf(rdram, ctl + 0x450), cam ? rdf(rdram, cam + 0x70) : 0.0f,
                    cam ? rdf(rdram, cam + 0x74) : 0.0f, cam ? rdf(rdram, cam + 0x6C) : 0.0f, rdf(rdram, player + 0x164),
                    rdf(rdram, player + 0x168), def, def ? rd32(rdram, def + 0x10) : 0u, def ? (rd32(rdram, def + 0x20) & 0xFF) : 0u,
                    def ? rdf(rdram, def + 0x178) : 0.0f, def ? rdf(rdram, def + 0x17C) : 0.0f, def ? rdf(rdram, def + 0x180) : 0.0f);
    }

    void ctrlUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        if (g_modeSwapCtl) // safety: the restore at 0x218710 did not run (the player update ended early)
        {
            wr32(rdram, g_modeSwapCtl + 0x454, 2u);
            g_modeSwapCtl = 0;
        }
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
        probeScope(rdram, player, ctl);
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
                std::printf("[kz-mtx] mode=%u zoom=%.3f dt=%.5f turn=%.4f rateA8=%.4f heading=%.5f off164=%.5f off168=%.5f pitch=%.5f\n",
                            rd32(rdram, ctl + 0x454), rdf(rdram, ctl + 0x44C), dt, rdf(rdram, g_ctrl + 0x3C), rate,
                            rdf(rdram, ctl + 0x78), rdf(rdram, g_player + 0x164), rdf(rdram, g_player + 0x168),
                            rdf(rdram, g_player + 0x14C));
            }
        }
        // Scope-type weapons (aim mode 2, e.g. the sniper rifle): right after ApplyLook the player update runs a reticle
        // block that treats the turn value as a stick deflection, keeps a reticle offset (player+0x164/0x168) that tracks
        // the stick, swallows the turn inside 45 % of the half field of view and rescales it beyond. A mouse turn command
        // is not a stick position, so small moves vanished into the offset and the view sprang back, and big moves were
        // clamped and rescaled. With keyboard/mouse the block is skipped: the player update sees mode 1 from here on
        // (reticle offsets are then written as 0 and the turn value passes through like unzoomed), and the real mode is
        // put back at the next call it makes after the block (0x218710). The swap is made before ApplyLook, not after it:
        // when the guest yields inside ApplyLook this wrapper returns early and the rest of the update runs without it, so
        // a swap after the call was skipped for that frame (reticle offset back for a frame under load). ApplyLook itself
        // reads the mode only to choose "no camera lag", which the cleared lag flag above already selects with the mouse.
        static const bool fix = !(std::getenv("KZ_SCOPE_FIX") && std::getenv("KZ_SCOPE_FIX")[0] == '0');
        if (fix && kbm)
        {
            const uint32_t ctl = rd32(rdram, g_player + 0x31C);
            if (ctl && rd32(rdram, ctl + 0x454) == 2u)
            {
                wr32(rdram, ctl + 0x454, 1u);
                g_modeSwapCtl = ctl;
            }
        }
        g_origApplyLook(rdram, ctx, rt);
        if (kbm)
            rdram[lagFlag] = savedFlag;
    }

    // First call after the scope block of the player update: put aim mode 2 back (see applyLook).
    void afterLook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        if (g_modeSwapCtl)
        {
            wr32(rdram, g_modeSwapCtl + 0x454, 2u);
            g_modeSwapCtl = 0;
        }
        g_origAfterLook(rdram, ctx, rt);
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
                    hook(runtime, kPitchAutoCentre, &pitchAutoCentre, g_origPitchAutoCentre) &&
                    hook(runtime, kAfterLookProbe, &afterLook, g_origAfterLook);
    std::printf("[kz] mouse aim patch: %s\n", ok ? "installed" : "FAILED (mouse drives the right stick)");
}
