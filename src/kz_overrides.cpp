// Killzone (SCUS-97402) runtime overrides.
//
// Bindings made here take effect at load time without regenerating the recompiled code: every guest call goes
// through PS2Runtime::dispatchGuestBranch, which looks up the function table that replaceFunction() patches.
// Use this for library functions the ps2_analyzer signature DB misses. Evidence for each binding: docs/findings.md.

#include "game_overrides.h"
#include "kz_sif_modules.h"
#include "kz_ipu.h"
#include "ps2_runtime.h"

#include <cstdint>
#include <iostream>

namespace
{
    struct Binding
    {
        uint32_t address;
        const char *handler;
    };

    // Stripped-ELF library functions identified by hand (docs/findings.md#library-bindings).
    constexpr Binding kBindings[] = {
        {0x002B6060u, "sceSifLoadModule"},
        {0x002B5AF0u, "SifStopModule"}, // sceSifStopModule(modid, arg_len, args, *result)
    };

    // Library functions the runtime has no handler for; implemented in kz_sif_modules.cpp.
    struct CustomBinding
    {
        uint32_t address;
        PS2Runtime::RecompiledFunction fn;
        const char *name;
    };
    // Without the IPU (KZ_IPU=off or kzipu init failure) video playback cannot work: the movie object's open method
    // (vtable 0x52EA78 +0x10, FUN_002ded00) reports failure so every caller skips its video (attract loop, cutscenes).
    void kzMovieOpenUnavailable(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ctx->pc = getRegU32(ctx, 31);
        setReturnS32(ctx, 0);
    }

    const CustomBinding kCustom[] = {

        {0x002B5D88u, &kzSceSifSearchModuleByName, "sceSifSearchModuleByName"},
        {0x002B5CF8u, &kzSceSifUnloadModule, "sceSifUnloadModule"},
        {0x003D7490u, &kzLgkbmInit, "lgkbm init"},
    };

    void applyKillzone(PS2Runtime &runtime)
    {
        for (const Binding &b : kBindings)
        {
            if (!ps2_game_overrides::bindAddressHandler(runtime, b.address, b.handler))
                std::cerr << "[kz] failed to bind " << b.handler << " @0x" << std::hex << b.address << std::dec << std::endl;
        }
        if (!kzIpuActive() && !runtime.replaceFunction(0x002DED00u, &kzMovieOpenUnavailable))
            std::cerr << "[kz] failed to bind movie-open skip" << std::endl;
        for (const CustomBinding &c : kCustom)
        {
            if (!runtime.replaceFunction(c.address, c.fn))
                std::cerr << "[kz] failed to bind " << c.name << " @0x" << std::hex << c.address << std::dec << std::endl;
        }
    }
}

// ELF CRC32 (zlib) of SCUS_974.02 v1.00 = 0x114E85F8 (PCSX2-style XOR CRC is CAAEC49C).
PS2_REGISTER_GAME_OVERRIDE("Killzone SCUS-97402", "SCUS_974.02", 0x00146F30u, 0x114E85F8u, applyKillzone)
