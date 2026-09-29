// kzspu2: forced include for the PCSX2 SPU2 sources (SPU2/*.cpp) and kzspu2's own sources.
//
// It includes the standard headers first, then renames every kzspu2 symbol (kzspu2_rename.h), then pre-includes the
// PCSX2 headers the SPU2 code uses (all #pragma once, so the sources' own #includes become no-ops), then redirects
// Console / DevCon to the host log callback.
//
// The IOP-side headers the SPU2 code includes (R3000A.h, IopHw.h, IopDma.h, IopCounters.h, IopMem.h, Config.h,
// SaveState.h, Host/AudioStream.h) are *shadowed* by the minimal versions next to this file (this directory is
// searched before pcsx2/), so kzspu2 compiles no IOP, config or audio-backend code at all.
// SPDX-License-Identifier: GPL-3.0+
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#include "kzspu2_rename.h"

#include "common/Pcsx2Defs.h"
#include "common/Assertions.h"
#include "common/Console.h"
#include "GS/MultiISA.h"
#include "GS/GSVector.h"
#include "R3000A.h"
#include "IopCounters.h"
#include "IopHw.h"
#include "IopDma.h"
#include "IopMem.h"
#include "SPU2/defs.h"
#include "SPU2/regs.h"
#include "SPU2/spu2.h"
#include "SPU2/Dma.h"
#include "SPU2/Debug.h"

#include "kzspu2_shim.h"

#define Console kzspu2_Console
#define DevCon kzspu2_Console
