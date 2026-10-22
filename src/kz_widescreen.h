#pragma once

// Ultrawide (21:9, 32:9, match-window) support. The engine's native 16:9 mode renders a 512x448 frame that the display
// stretches to 16:9. For a wider display the 3D view is rendered with a wider aspect ratio (same vertical field of view,
// so more at the sides), the 2D layer (HUD, text, fades) is compressed horizontally into the central 16:9 part, and
// frames without a 3D view (menus, movies) are drawn as before and pillarboxed. docs/findings.md "Ultrawide".

#include "kz_config.h"

#include <cstdint>

class PS2Runtime;

void kzWideInstall(PS2Runtime &runtime);

// Target aspect ratio (width / height) of the 3D view; <= 16/9 turns the feature off. Thread-safe.
void kzWideSetTargetAspect(float aspect);
float kzWideTargetAspect(); // 0 = off

// Derives the target from the aspect setting and the window size (UltrawideAuto: the window's own ratio; 21:9 and 32:9: fixed).
void kzWideConfigure(KzAspect aspect, int windowWidth, int windowHeight);

// Called on the guest vblank: true when the frame being presented has a widened 3D view.
bool kzWideTakeFrameFlag();
