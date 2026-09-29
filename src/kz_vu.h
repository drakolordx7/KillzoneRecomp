#pragma once

// VU1 and VU0 micro mode through kzvu (PCSX2 microVU recompiler, ext/kzvu). Must be installed before
// PS2Runtime::initialize() so the runtime's VU0/VU1 memory is kzvu's; bind the runtime afterwards so XGKICK packets
// reach its GIF arbiter as PATH1.
// KZ_VU=builtin keeps PS2Recomp's VU1 interpreter (debugging); KZ_VU=interp uses PCSX2's VU1 interpreter.
// KZ_VU0=interp keeps PS2Recomp's interpreter for VCALLMS/VCALLMSR; KZ_VU0=pcsx2interp uses PCSX2's VU0 interpreter.
// KZ_VU0_DUMP=<dir> captures VU0 calls for ext/kzvu/test/kzvu0_test; KZ_VU0_STATS=1 logs VU0 load (see kz_vu.cpp).

#include <string>

class PS2Runtime;

bool kzVuInstall(std::string *error);
void kzVuBindRuntime(PS2Runtime &runtime);
