// kzspu2 shadow of pcsx2/R3000A.h: only the IOP cycle counter the SPU2 code reads (psxRegs.cycle).
// SPDX-License-Identifier: GPL-3.0+
#pragma once

#include "common/Pcsx2Defs.h"

struct psxRegisters
{
	u64 cycle; // IOP clock (36.864 MHz); set from the host's cycle on every kzspu2 call
};

alignas(16) extern psxRegisters psxRegs;

// PCSX2's IOP event scheduler state; CounterUpdate() lowers the next-event delta (kzspu2NextEventCycle()).
extern s32 psxNextDeltaCounter;
extern u64 psxNextStartCounter;
