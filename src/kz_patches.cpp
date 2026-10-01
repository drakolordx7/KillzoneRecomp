#include "kz_patches.h"

#include "kz_config.h"

#include <cstdlib>
#include <cstring>

namespace
{
    constexpr uint32_t kRamMask = 0x01FFFFFFu;
    // The engine's native 16:9 mode (FOV + HUD) is a word in the global game object: *(0x559178) + 0x34.
    // (The community pnach writes 0x5DA364, which is where that object sits in the PS2 heap; found with PCSX2 PINE.)
    constexpr uint32_t kGameObjectPtr = 0x00559178u;
    constexpr uint32_t kWidescreenFieldOffset = 0x34u;

    void w32(uint8_t *ram, uint32_t a, uint32_t v) { std::memcpy(ram + (a & kRamMask), &v, 4); }
    void w8(uint8_t *ram, uint32_t a, uint8_t v) { ram[a & kRamMask] = v; }
    uint16_t r16(const uint8_t *ram, uint32_t a)
    {
        uint16_t v;
        std::memcpy(&v, ram + (a & kRamMask), 2);
        return v;
    }
}

void kzPatchesApply(uint8_t *rdram, const KzConfig &cfg)
{
    static const bool disabled = std::getenv("KZ_NO_PATCHES") != nullptr; // debugging
    if (!rdram || disabled)
        return;
    // Writing the pnach's absolute heap address corrupted render state here (the runtime's heap layout differs from
    // the PS2's), so the flag is reached through the game object pointer instead.
    if (cfg.aspect == KzAspect::Widescreen16x9)
    {
        uint32_t obj;
        std::memcpy(&obj, rdram + (kGameObjectPtr & kRamMask), 4);
        if (obj >= 0x00100000u && obj < 0x02000000u)
            w32(rdram, obj + kWidescreenFieldOffset, 1u);
    }
    // The film grain is switched in kz_post.cpp (the post-process object's own noise strength). The community pnach
    // cleared byte 0x55DF6C instead, which is the alpha of the engine's default 2D material: it also hid the pause
    // menu's panels and, left at 0 after a level, the front end's background movie (white screen after Quit).
}
