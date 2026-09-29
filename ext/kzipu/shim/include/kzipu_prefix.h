// kzipu: forced include for the PCSX2 IPU sources (IPU/*.cpp) and for kzipu's own sources.
//
// It first includes every PCSX2 header those files use (all are #pragma once, so the sources' own #includes become
// no-ops), then redirects by macro, only inside the code that follows:
//   IPU_LOG / DMA_LOG -> nothing        (PCSX2's trace log system is not compiled)
//   Console / DevCon  -> kzipu_Console  (warnings and errors go to the host's log callback)
// SPDX-License-Identifier: GPL-3.0+
#pragma once

#include "kzipu_rename.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <limits.h>
#include <string>

#include "common/Pcsx2Defs.h"
#include "common/StringUtil.h"
#include "Common.h"
#include "Config.h"
#include "IPU/IPU.h"
#include "IPU/IPU_Fifo.h"
#include "IPU/IPUdma.h"
#include "IPU/IPU_MultiISA.h"
#include "IPU/mpeg2_vlc.h"
#include "IPU/yuv2rgb.h"
#include "GS/MultiISA.h"
#include "ps2/HwInternal.h"

#include "kzipu_shim.h"

#undef IPU_LOG
#define IPU_LOG(...) ((void)0)
#undef DMA_LOG
#define DMA_LOG(...) ((void)0)

#define Console kzipu_Console
#define DevCon kzipu_Console
