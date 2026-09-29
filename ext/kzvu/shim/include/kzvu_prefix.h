// kzvu: forced include for the PCSX2 VU sources (x86/microVU.cpp, VUops.cpp, VUflags.cpp, VU1microInterp.cpp) and
// for kzvu's own sources.
//
// It first includes every PCSX2 header those files use (all of them are #pragma once, so the sources' own #includes
// become no-ops), then redirects three emulator subsystems to kzvu's shims by macro:
//   gifUnit   -> kzvu_gifUnit   (XGKICK: GIF packets go to the host callback instead of PCSX2's GIF unit / MTGS)
//   EmuConfig -> kzvu_EmuConfig (only the VU-relevant settings; filled from KzvuConfig)
//   vu1Thread -> kzvu_vu1Thread (MTVU is compiled out: THREAD_VU1 is forced to false)
// Because the macros are defined after the headers were parsed, the PCSX2 declarations keep their original types and
// only the code in the VU sources (and microVU_*.inl) is redirected.
// SPDX-License-Identifier: GPL-3.0+
#pragma once

#include "kzvu_rename.h"

#include "PrecompiledHeader.h"

#include "Common.h"
#include "Config.h"
#include "GS.h"
#include "Gif.h"
#include "Gif_Unit.h"
#include "MTVU.h"
#include "R5900.h"
#include "R5900OpcodeTables.h"
#include "VU.h"
#include "VUflags.h"
#include "VUmicro.h"
#include "VUops.h"
#include "Vif.h"
#include "Vif_Dma.h"
#include "x86/iR5900.h"
#include "GS/MultiISA.h"

#include "common/AlignedMalloc.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/Perf.h"
#include "common/StringUtil.h"
#include "common/emitter/x86emitter.h"

#include "kzvu_shim.h"

#undef THREAD_VU1
#define THREAD_VU1 false

#define EmuConfig kzvu_EmuConfig
#define gifUnit kzvu_gifUnit
#define vu1Thread kzvu_vu1Thread
