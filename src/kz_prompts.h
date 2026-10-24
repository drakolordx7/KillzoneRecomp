#pragma once

// Keyboard and mouse button prompts (GitHub issue #16). The game draws PS2 button pictures inside its text; while the last input
// came from the keyboard or mouse they are replaced by the key or mouse button bound to that action. docs/findings.md "Button prompts".

class PS2Runtime;

void kzPromptsInstall(PS2Runtime &runtime);
