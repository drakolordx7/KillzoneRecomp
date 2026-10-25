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

// Field of view of the player's 3D view and of the first-person weapon, as horizontal degrees measured at 16:9 (the
// vertical field of view follows; ultrawide widens the horizontal one further). 0 = the game's own. Thread-safe.
// The same hooks as ultrawide are used, so kzWideInstall must run when either is not the game's own.
void kzWideSetFov(float worldDegrees, float weaponDegrees);
float kzFovNativeWorld();  // 87.1: 56.25 degrees vertical at 16:9
float kzFovNativeWeapon(); // 72.7: 45 degrees vertical at 16:9
