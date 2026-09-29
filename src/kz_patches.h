#pragma once

// Per-vblank game memory patches driven by the PC settings (docs/findings.md#widescreen--noise-filter).

#include <cstdint>

struct KzConfig;

void kzPatchesApply(uint8_t *rdram, const KzConfig &cfg);
