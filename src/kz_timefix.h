#pragma once

// Frame-rate independence fixes for game code that counts frames instead of using the frame time. The original runs at
// 30 fps, the port at the display rate; see docs/findings.md "Speed parity with the original".

class PS2Runtime;

void kzTimeFixInstall(PS2Runtime &runtime);
