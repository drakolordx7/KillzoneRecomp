#pragma once

// IPU (MPEG decoder) through kzipu (PCSX2 IPU, ext/kzipu), so the game's own .pss player works.
// Opt-in for now: KZ_IPU=on. Otherwise videos are skipped via the movie-open override in kz_overrides.cpp.

class PS2Runtime;

// Returns true when the IPU is active.
bool kzIpuInstall();
void kzIpuBindRuntime(PS2Runtime &runtime);
bool kzIpuActive();
