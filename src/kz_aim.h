#pragma once

// Mouse aim as an engine patch: raw mouse motion is added to the local player's yaw/pitch deltas inside the game's own
// player update, so it is 1:1 with no stick acceleration, deadzone or turn-rate cap. See docs/findings.md "Mouse aim".

class PS2Runtime;

void kzAimInstall(PS2Runtime &runtime);

#include <cstdint>

// Local player 0's entity (0 until its controller has run once). EE thread only. Its camera object is player+0x3C.
uint32_t kzAimPlayer();
