#pragma once

// VU1 through kzvu (PCSX2 microVU recompiler, ext/kzvu). Must be installed before PS2Runtime::initialize() so the
// runtime's VU1 memory is kzvu's; bind the runtime afterwards so XGKICK packets reach its GIF arbiter as PATH1.
// KZ_VU=builtin keeps PS2Recomp's interpreter (debugging); KZ_VU=interp uses PCSX2's interpreter.

#include <string>

class PS2Runtime;

bool kzVuInstall(std::string *error);
void kzVuBindRuntime(PS2Runtime &runtime);
