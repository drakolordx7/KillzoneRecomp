#pragma once

// The PAL generated code (generated_pal, PS2Recomp with symbol_prefix = "kzpal_" and
// table_register_function = "kzPalRegisterFunctionTable") does not define its own function table. Its
// register_functions.cpp defines the two symbols below: the function nulls every slot of the given table and then
// fills the PAL entries; End is one past the last guest address it fills. kz_region.cpp calls it on the live table
// (g_ps2RecompiledFunctionTable, defined by the US generated code) when the PAL disc is chosen.
// Only declared when the PAL set is linked in (CMake defines KZ_HAVE_PAL for kz_port).

#include "ps2_runtime.h"

#include <cstdint>

#if defined(KZ_HAVE_PAL)
void kzPalRegisterFunctionTable(PS2Runtime::RecompiledFunction *table, uint32_t base, uint32_t slotCount);
extern const uint32_t kzPalRegisterFunctionTableEnd;
#endif
