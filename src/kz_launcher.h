#pragma once

#include "kz_config.h"

// Options launcher shown before the game boots. Edits `cfg` in place and saves it to killzone.ini.
// Returns true when the player pressed Play (with a valid disc), false on Quit / window close.
bool kzRunLauncher(KzConfig &cfg);

// Renders one launcher frame offscreen and writes it to `pngPath` (automated screenshot / verification). Returns success.
bool kzLauncherScreenshot(KzConfig &cfg, const char *pngPath);
