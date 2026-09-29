#pragma once

// IPU (MPEG decoder) through kzipu (PCSX2 IPU, ext/kzipu), so the game's own .pss player works. The player's
// libmpeg/libipu calls (sceMpeg*, sceIpu*) are bound to the runtime's libmpeg HLE, which decodes with FFmpeg;
// kzipu serves the player's direct IPU register and IPU DMA (ch3/ch4) accesses. kz_ipu.cpp also holds the two
// end-of-movie shims the HLE needs (see movieReadAtEofFails).
// On by default. KZ_IPU=off (or an init failure) skips videos via the movie-open override in kz_overrides.cpp.
// KZ_IPU_TRACE=1[,0xADDR,...] logs calls of the video player's stream/file functions (debug aid).

class PS2Runtime;

// Returns true when the IPU is active.
bool kzIpuInstall();
void kzIpuBindRuntime(PS2Runtime &runtime);
bool kzIpuActive();
