// kzspu2 shadow of pcsx2/IopCounters.h: counter 6 is the SPU2's "call SPU2async() at" event.
// SPDX-License-Identifier: GPL-3.0+
#pragma once

#include "common/Pcsx2Defs.h"

struct psxCounter
{
	u64 startCycle;
	s32 deltaCycles;
};

#define NUM_COUNTERS 8
extern psxCounter psxCounters[NUM_COUNTERS];
