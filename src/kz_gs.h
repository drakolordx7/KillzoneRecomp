#pragma once

// Hardware GS: attaches kzgs (PCSX2's GS renderer, ext/kzgs) to the runtime.
//
// The runtime's GIF arbiter forwards every packet (all PATHs) and each guest vblank to kzgs through ps2_host_gs.h
// hooks, on the EE game thread. The built-in GS frontend keeps parsing packets for side effects (SIGNAL/FINISH/CSR,
// image uploads, VRAM reads), but its software rasterizer is skipped.

#include "kz_config.h"

#include <string>

class PS2Runtime;

// hwnd = Win32 window to render into, or nullptr for an offscreen (headless) device. windowHeight drives the
// automatic internal resolution.
bool kzGsAttach(PS2Runtime &runtime, void *hwnd, int windowHeight, const KzConfig &cfg, std::string *error);
void kzGsDetach();

// Main-thread notifications; applied on the game thread at the next vblank.
void kzGsResize(int width, int height);
void kzGsApplyConfig(const KzConfig &cfg);

// Automated screenshots: when `dir` is non-empty, the output frame is saved as <dir>/gs_<n>_t<sec>s.png every
// `intervalSeconds` of wall time (checked at vblank).
void kzGsSetScreenshotSchedule(const std::string &dir, int intervalSeconds);

// Frames presented so far (vblanks forwarded to kzgs).
uint64_t kzGsFrameCount();
