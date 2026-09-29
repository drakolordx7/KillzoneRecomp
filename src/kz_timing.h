#pragma once

// High refresh rate support (docs/findings.md#frame-timing-key-for-high-refresh).
//
// Killzone flips display buffers in its vblank handler and paces its main loop on the vblank counter, so the port
// raises the guest vblank rate R to the target frame rate and corrects the game state that assumes 59.94 Hz:
//   - seconds per vsync (0x55A6E4) = 1/R, which the frame timer multiplies by elapsed vsyncs (instruction patches in
//     config/killzone.toml make it count every vsync instead of every other one);
//   - frame-time clamp (game object +0x70, in vsyncs) scaled by R/30 so the maximum simulated step stays the same;
//   - screen fade (0x55A7C8, stepped per vblank by FUN_0018f0d0) scaled back to 60 Hz speed.

#include <cstdint>

class PS2Runtime;

// fpsLimit 0 = display refresh rate. Returns the chosen guest vblank rate.
int kzTimingInit(int fpsLimit, int displayRefreshHz);
int kzTimingRate();

// Called on the game thread at every guest vblank (before the game's vblank handlers run).
void kzTimingOnVsync(uint8_t *rdram);
