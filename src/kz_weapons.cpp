// Next / previous weapon (bindings NextWeapon / PrevWeapon in killzone.ini [Bindings], the mouse wheel by default).
//
// The game's own weapon button (action "selectweapon") only cycles forward. Its handling is at the end of the player
// controller update (JoystickControllerPlayerImp, kzAddrs().ctrlUpdate): on release of the button, if the character
// controller has at least two weapon slots, is not in the middle of a change (ctl+0x278 bit 2 clear) and is not on slot 0
// while slot 0 is a type 0xD weapon, it writes the wanted slot to ctrl+0x58: (current + 1) % count, or with a type 0xD
// weapon in slot 0 current + 1 wrapping to 1. The player update then changes weapon through the character controller's
// own virtual (animation, sound, ammo display). This hook runs right after that update and writes the same field for a
// step from the input layer, with the same conditions, so a step behaves exactly like the button: no synthetic input.
// Details: docs/findings.md "Weapon switching".
//
// The structure offsets are the same on the US and the European disc; the controller update address comes from kzAddrs().

#include "kz_addrs.h"
#include "kz_input.h"
#include "kz_region.h"

#include "game_overrides.h"
#include "ps2_runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    PS2Runtime::RecompiledFunction g_origCtrlUpdate = nullptr;
    bool g_log = false;

    uint32_t rd32(const uint8_t *rdram, uint32_t addr)
    {
        uint32_t v = 0;
        addr &= 0x01FFFFFFu;
        if (addr + 4 <= PS2_RAM_SIZE)
            std::memcpy(&v, rdram + addr, 4);
        return v;
    }
    void wr32(uint8_t *rdram, uint32_t addr, uint32_t v)
    {
        addr &= 0x01FFFFFFu;
        if (addr + 4 <= PS2_RAM_SIZE)
            std::memcpy(rdram + addr, &v, 4);
    }
    bool ramPtr(uint32_t p) { return p >= 0x00100000u && p < 0x02000000u; }

    // Weapon-slot type of slot 0 is 0xD: the game's FUN_001F8178 on the character controller.
    bool slotZeroIsType13(const uint8_t *rdram, uint32_t ctl)
    {
        if (static_cast<int32_t>(rd32(rdram, ctl + 0x1C4)) <= 0)
            return false;
        const uint32_t set = rd32(rdram, rd32(rdram, ctl + 0x1CC));
        if (!ramPtr(set))
            return false;
        const uint32_t list = rd32(rdram, set + 12);
        if (!ramPtr(list) || rd32(rdram, list + 16) == 0u)
            return false;
        const uint32_t first = rd32(rdram, rd32(rdram, list + 24));
        return ramPtr(first) && rd32(rdram, first + 16) == 0xDu;
    }

    // Try to take the oldest queued step. Returns true when the step is finished (taken or hopeless), false to keep it queued.
    bool applyStep(uint8_t *rdram, uint32_t ctrl, int step)
    {
        const uint32_t player = rd32(rdram, ctrl + 0x18);
        if (!ramPtr(player))
            return false;
        const uint32_t ctl = rd32(rdram, player + 0x31C);
        if (!ramPtr(ctl) || (rdram[(ctl + 116) & 0x01FFFFFFu]) == 0)
            return false; // the player is not under control (loading, dead, cutscene)
        const int32_t count = static_cast<int32_t>(rd32(rdram, ctl + 0x1C4));
        const int32_t cur = static_cast<int32_t>(rd32(rdram, ctl + 0x1DC));
        if (count < 2)
            return true; // nothing to switch to
        if (cur < 0 || cur >= count)
            return false;
        if (rd32(rdram, ctl + 0x278) & 4u)
            return false; // a weapon change (or another action that blocks it) is running: wait for it, like the button
        const bool skipZero = slotZeroIsType13(rdram, ctl);
        if (skipZero && cur == 0)
            return true;
        int32_t target;
        if (step > 0)
        {
            target = cur + 1;
            if (skipZero)
            {
                if (target >= count)
                    target = 1;
            }
            else
                target %= count;
        }
        else
        {
            target = cur - 1;
            if (skipZero ? target < 1 : target < 0)
                target = count - 1;
        }
        wr32(rdram, ctrl + 0x58, static_cast<uint32_t>(target));
        if (g_log)
            std::printf("[kz-weapons] %s: slot %d -> %d (of %d)\n", step > 0 ? "next" : "previous", cur, target, count);
        return true;
    }

    void ctrlUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t ra = getRegU32(ctx, 31);
        const uint32_t ctrl = getRegU32(ctx, 4);
        const bool local = ctx->pc == kzAddrs().ctrlUpdate && rd32(rdram, ctrl + 0x1C) == 0u;
        const uint32_t wantedBefore = local ? rd32(rdram, ctrl + 0x58) : 0u;
        g_origCtrlUpdate(rdram, ctx, rt);
        if (!local || ctx->pc != ra) // not player 0, or the update yielded (it finishes later in guest code)
            return;
        if (g_log) // KZ_WEAPON_LOG=1: every change of the current slot, whatever caused it
        {
            static int32_t lastCur = -2;
            const uint32_t ctl = rd32(rdram, rd32(rdram, ctrl + 0x18) + 0x31C);
            if (ramPtr(ctl))
            {
                const int32_t cur = static_cast<int32_t>(rd32(rdram, ctl + 0x1DC));
                if (cur != lastCur)
                    std::printf("[kz-weapons] current slot %d -> %d (ctrl+0x58 = %u, flags %08x)\n", lastCur, cur, rd32(rdram, ctrl + 0x58),
                                rd32(rdram, ctl + 0x278));
                lastCur = cur;
            }
        }
        const int step = kzInputPeekWeaponStep();
        if (step == 0)
            return;
        if (rd32(rdram, ctrl + 0x58) != wantedBefore)
            return; // the game's own button asked for a weapon this frame: this step waits for the next one
        if (applyStep(rdram, ctrl, step))
            kzInputPopWeaponStep();
    }

    void install(PS2Runtime &runtime, KzRegion region)
    {
        if (kzRegion() != region)
            return; // kz_overrides.cpp reports the mismatch
        const uint32_t addr = kzAddrs().ctrlUpdate;
        g_log = std::getenv("KZ_WEAPON_LOG") != nullptr;
        g_origCtrlUpdate = runtime.hasFunction(addr) ? runtime.lookupFunction(addr) : nullptr;
        const bool ok = g_origCtrlUpdate && runtime.replaceFunction(addr, &ctrlUpdate);
        std::printf("[kz] next/previous weapon: %s\n", ok ? "installed" : "FAILED (the weapon button still works)");
    }
    void installUs(PS2Runtime &runtime) { install(runtime, KzRegion::Us); }
    void installPal(PS2Runtime &runtime) { install(runtime, KzRegion::Pal); }
}

// Same ELF identity as the descriptors in kz_overrides.cpp; the runtime applies every descriptor that matches. Installed
// while the ELF loads, so the aim patch (installed after) wraps this hook.
PS2_REGISTER_GAME_OVERRIDE("Killzone SCUS-97402 weapons", "SCUS_974.02", 0x00146F30u, 0x114E85F8u, installUs)
PS2_REGISTER_GAME_OVERRIDE("Killzone SCES-52893 weapons", "SCES_528.93", 0x00146F10u, 0x39B8D221u, installPal)
