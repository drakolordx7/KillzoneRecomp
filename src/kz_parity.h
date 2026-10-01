#pragma once

// Speed-parity instrumentation (docs/findings.md "Speed parity with the original"). KZ_PARITY_LOG=<file> writes one CSV row
// per guest vblank with wall time, the game's vsync counter and frame timer, the local player's character controller
// (position, heading) and any extra words named by KZ_PARITY_WORDS. The same fields are read from PCSX2 over PINE by
// tools/scripts/parity_pcsx2.py, so both runs are measured the same way. KZ_PARITY_DUMP=t1,t2,.. also writes the EE RAM
// at those wall-clock seconds (<log>.<t>.ram) for diffing.

#include <cstdint>

void kzParityOnVsync(uint8_t *rdram);
