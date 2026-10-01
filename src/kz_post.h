#pragma once

// Switches for the game's own full-screen post-processing passes (docs/findings.md "Post-process passes").
// The hooks wrap the game functions that draw the passes, so a disabled pass is never submitted to the GS.

class PS2Runtime;

void kzPostInstall(PS2Runtime &runtime);
