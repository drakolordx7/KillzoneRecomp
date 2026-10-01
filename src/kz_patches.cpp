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
    constexpr uint32_t kNoiseFilter = 0x0055DF6Cu;    // byte
    constexpr uint32_t kNoiseGate = 0x0057BA88u;      // halfword; 4 = state where the filter byte must be 0

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
    // pnach "Disable noise filter in gameplay": the byte is cleared while the gate halfword reads 4 (in a level).
    // The byte is the alpha of the engine's default 2D material (word 13 of the render state at 0x55DF38), not only the
    // grain overlay's: the front end draws its background movie quad and the pause menu panels with it, and the game
    // writes it once (0xFF at start-up) and never again. Left at 0 after a level, the menu's background movie was
    // drawn fully transparent and the light bars piled up additively into a white screen. So the value the game had
    // before the first override is put back as soon as the gate leaves 4 (measured: gate 0 in the menus, 4 in a level
    // including its pause menu; byte 0xFF all along without this patch).
    static bool overridden = false;
    static uint8_t savedNoise = 0xFFu;
    const bool inLevel = r16(rdram, kNoiseGate) == 0x0004u;
    if (!cfg.noiseFilter && inLevel)
    {
        if (!overridden)
        {
            savedNoise = rdram[kNoiseFilter & kRamMask];
            overridden = true;
        }
        w8(rdram, kNoiseFilter, 0x00u);
    }
    else if (overridden)
    {
        w8(rdram, kNoiseFilter, savedNoise);
        overridden = false;
    }
}
