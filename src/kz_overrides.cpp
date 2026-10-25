// Killzone runtime overrides, for the US (SCUS-97402) and the PAL (SCES-52893) boot ELF. The guest addresses come from
// kzAddrs() (kz_addrs.h), the table of the version chosen at boot (kz_region.h).
//
// Bindings made here take effect at load time without regenerating the recompiled code: every guest call goes
// through PS2Runtime::dispatchGuestBranch, which looks up the function table that replaceFunction() patches.
// Use this for library functions the ps2_analyzer signature DB misses. Evidence for each binding: docs/findings.md.

#include "game_overrides.h"
#include "kz_sif_modules.h"
#include "kz_ipu.h"
#include "kz_devtools.h"
#include "kz_addrs.h"
#include "kz_region.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_timeline.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace
{
    struct Binding
    {
        uint32_t address;
        const char *handler;
    };

    // Stripped-ELF library functions identified by hand (docs/findings.md#library-bindings). sceSifLoadModule (US 0x2B6060) and
    // sceSifStopModule (US 0x2B5AF0, (modid, arg_len, args, *result)) are bound in customBindings() below (they must set
    // ctx->pc = $ra).
    constexpr Binding kBindings[] = {
        {0u, nullptr},
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

    // Each PAL function matches the US one apart from relocations (kz_addrs.cpp).
    struct CustomBindings
    {
        CustomBinding items[5];
    };
    CustomBindings customBindings()
    {
        const KzAddrs &a = kzAddrs();
        return {{
            {a.sifSearchModuleByName, &kzSceSifSearchModuleByName, "sceSifSearchModuleByName"},
            {a.sifUnloadModule, &kzSceSifUnloadModule, "sceSifUnloadModule"},
            {a.lgkbmInit, &kzLgkbmInit, "lgkbm init"},
            {a.sifLoadModule, &kzSifLoadModuleAt, "sceSifLoadModule"},
            {a.sifStopModule, &kzSifStopModuleAt, "sceSifStopModule"},
        }};
    }

    // KZ_TIMELINE: wrappers that log entry/exit of the frame pipeline's guest functions (patch 0025). The wrapper takes the
    // function's table slot (a call, or a scheduler dispatch, enters it), logs, and calls the original. A resume at an inner
    // label goes through its own table slot and does not pass here. "Exit" is only logged for a plain return (ctx->pc == $ra).
    struct TlHook
    {
        uint32_t addr = 0;
        PS2Runtime::RecompiledFunction orig = nullptr;
    };
    template <int N>
    struct TlWrap
    {
        static inline TlHook hook;
        static uint32_t extra(const uint8_t *rdram, uint32_t ra)
        {
            auto rd = [rdram](uint32_t a) { uint32_t v; std::memcpy(&v, rdram + (a & 0x01FFFFFFu), 4); return v; };
            const KzAddrs &k = kzAddrs();
            if (hook.addr == k.frameLimiter) // limiter: vsync counter
                return rd(k.vsyncCounter);
            if (hook.addr == k.kickCheck) // kick check: flag | next buffer state<<8 | current index<<16
            {
                const uint32_t cur = rd(k.bufferIndex);
                return (rdram[k.kickFlag]) | (static_cast<uint32_t>(rdram[k.bufferState + ((cur ^ 1u) & 1u)]) << 8) | ((cur & 1u) << 16);
            }
            if (hook.addr == k.vblankHandler)
                return rd(k.vsyncCounter);
            return ra;
        }
        static void fn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
        {
            const uint32_t ra = getRegU32(ctx, 31);
            const bool tl = ps2tl::on();
            if (tl)
                ps2tl::rec(ps2tl::G_ENTER, hook.addr, extra(rdram, ra), static_cast<uint32_t>(rt->eeScheduler().eeCycleForTrace()));
            hook.orig(rdram, ctx, rt);
            if (tl && ctx->pc == ra)
                ps2tl::rec(ps2tl::G_EXIT, hook.addr, extra(rdram, ra), static_cast<uint32_t>(rt->eeScheduler().eeCycleForTrace()));
        }
    };
    template <int N>
    void tlInstall(PS2Runtime &runtime, uint32_t addr)
    {
        TlWrap<N>::hook.addr = addr;
        TlWrap<N>::hook.orig = runtime.lookupFunction(addr);
        if (!TlWrap<N>::hook.orig || !runtime.replaceFunction(addr, &TlWrap<N>::fn))
            std::cerr << "[kz] timeline hook failed @0x" << std::hex << addr << std::dec << std::endl;
    }

    void applyKillzone(PS2Runtime &runtime)
    {
        const KzAddrs &a = kzAddrs();
        kzDevHooksInstall(runtime);
        if (const char *tl = std::getenv("KZ_TIMELINE"); tl && *tl)
        {
            tlInstall<0>(runtime, a.frameLimiter);  // frame limiter / delta
            tlInstall<1>(runtime, a.waitD1Idle);    // wait for D1 idle, then set the kick flag
            tlInstall<2>(runtime, a.waitKicked);    // wait until the vblank handler kicked
            tlInstall<3>(runtime, a.kick);          // the kick (D1 CHCR store)
            tlInstall<4>(runtime, a.kickCheck);     // vblank handler: kick check
            tlInstall<5>(runtime, a.vblankHandler); // vblank handler
        }
        for (const Binding &b : kBindings)
        {
            if (!b.handler)
                continue;
            if (!ps2_game_overrides::bindAddressHandler(runtime, b.address, b.handler))
                std::cerr << "[kz] failed to bind " << b.handler << " @0x" << std::hex << b.address << std::dec << std::endl;
        }
        if (!kzIpuActive() && !runtime.replaceFunction(a.movieOpen, &kzMovieOpenUnavailable))
            std::cerr << "[kz] failed to bind movie-open skip" << std::endl;
        for (const CustomBinding &c : customBindings().items)
        {
            if (!runtime.replaceFunction(c.address, c.fn))
                std::cerr << "[kz] failed to bind " << c.name << " @0x" << std::hex << c.address << std::dec << std::endl;
        }
    }

    // The override that matched the loaded ELF must be the version chosen at boot (kz_main.cpp, kzSelectRegion): the
    // function table and every hook address belong to that version.
    void applyFor(PS2Runtime &runtime, KzRegion elfRegion)
    {
        if (kzRegion() != elfRegion)
        {
            std::cerr << "[kz] the loaded ELF is " << kzRegionBootElf(elfRegion) << " but the game version chosen at boot is "
                      << kzRegionSerial(kzRegion()) << "; overrides not applied" << std::endl;
            return;
        }
        applyKillzone(runtime);
    }
    void applyKillzoneUs(PS2Runtime &runtime) { applyFor(runtime, KzRegion::Us); }
    void applyKillzonePal(PS2Runtime &runtime) { applyFor(runtime, KzRegion::Pal); }
}

// ELF CRC32 (zlib) of SCUS_974.02 v1.00 = 0x114E85F8 (PCSX2-style XOR CRC is CAAEC49C).
PS2_REGISTER_GAME_OVERRIDE("Killzone SCUS-97402", "SCUS_974.02", 0x00146F30u, 0x114E85F8u, applyKillzoneUs)
// SCES_528.93 (PAL, En/Ru/El): ELF CRC32 (zlib) 0x39B8D221 (XOR CRC 69FE9BB6).
PS2_REGISTER_GAME_OVERRIDE("Killzone SCES-52893", "SCES_528.93", 0x00146F10u, 0x39B8D221u, applyKillzonePal)
