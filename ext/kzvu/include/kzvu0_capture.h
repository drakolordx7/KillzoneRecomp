// kzvu0_capture.h - file format of a captured VU0 microprogram call (VCALLMS/VCALLMSR).
//
// The game writes these with KZ_VU0_DUMP=<dir> (src/kz_vu.cpp); ext/kzvu/test/kzvu0_test.cpp replays them through
// microVU0, PCSX2's VU0 interpreter and PS2Recomp's VU1Interpreter (in VU0 mode) and compares the results.
// One file = one call: the complete VU0 input state, the VU1 registers VU0 can address at 0x4000+, and the result the
// game got. Little-endian, no padding surprises (all members are 4-byte aligned, the struct is written as-is).
#pragma once

#include "kzvu.h"

#include <cstdint>

struct Kzvu0Capture
{
	static constexpr uint32_t kMagic = 0x3056'5A4Bu; // "KZV0"
	static constexpr uint32_t kVersion = 1;

	uint32_t magic = kMagic;
	uint32_t version = kVersion;
	uint32_t startPc = 0;        // byte address the program was started at
	uint32_t callerPc = 0;       // EE address of the VCALLMS/VCALLMSR (ctx->pc)
	uint32_t fbrst = 0;          // COP2 FBRST (vi28)
	uint32_t callIndex = 0;      // number of VU0 calls before this one in the run
	uint32_t outVpuStat = 0;     // VU0 bits of VPU_STAT after the run
	uint32_t outTpc = 0;         // TPC (bytes) after the run
	uint32_t cycles = 0;         // VU0 cycles the game's run took
	uint32_t itop = 0;           // COP2 vu0_itop (what the runtime's interpreter hands XITOP)
	uint32_t reserved[2] = {};
	Kzvu0Regs in{};              // COP2 registers before the call
	Kzvu0Regs out{};             // ... and after
	uint32_t vu1VfIn[32][4] = {};
	uint32_t vu1ViIn[32] = {};   // VI0-15 + control registers, kzvuGetVI numbering
	uint32_t vu1VfOut[32][4] = {};
	uint32_t vu1ViOut[32] = {};
	uint8_t code[kKzvu0CodeSize] = {};
	uint8_t dataIn[kKzvu0DataSize] = {};
	uint8_t dataOut[kKzvu0DataSize] = {};
};
