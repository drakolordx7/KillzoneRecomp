#pragma once

// Issue #15 spike: a test build for the PAL disc (SCES-52893, boot ELF SCES_528.93), not shipped.
// CMake -DKZ_PAL_SPIKE=ON (on by default when KZ_GENERATED_DIR names generated_pal) defines KZ_PAL_SPIKE for our code.
// Such a build boots only the PAL disc: the boot files go to their own folder, the override registers for the PAL
// ELF, and only the hooks whose PAL addresses were checked (work/pal/hookmap.csv: SIF/lgkbm bindings, movie fixes,
// frame timing) are installed. Mouse aim, ultrawide, button prompts, post-process and look-acceleration hooks are off.
// Without KZ_PAL_SPIKE every value below is the US one and nothing changes.

#include <cstdint>

#if defined(KZ_PAL_SPIKE)
inline constexpr bool kKzPal = true;
#define KZ_GADDR(us, pal) (pal)
#else
inline constexpr bool kKzPal = false;
#define KZ_GADDR(us, pal) (us)
#endif
