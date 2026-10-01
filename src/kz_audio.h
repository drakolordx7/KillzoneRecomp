#pragma once

// Game audio: PCSX2's SPU2 (ext/kzspu2) behind the IOP emulator's SPU2 registers, IOP DMA channels 4/7 and INTC line 9
// (ps2x/iop/iop_host_spu2.h). The game's own LIBSD.IRX / SDRDRV.IRX / PSOUND_R.IRX run on the emulated IOP and drive
// it like real hardware; the mixed 48 kHz output plays through SDL3, scaled by KzConfig::masterVolume.
//
// Environment:
//   KZ_AUDIO=off          no SPU2 (the IOP keeps its old stub, i.e. silence)
//   KZ_AUDIO_DEVICE=0|1   force the output device off/on (default: on, except with PS2X_HEADLESS=1)
//   KZ_AUDIO_WAV=<path>   also write the mixed output (before master volume) to a 48 kHz stereo WAV
//   KZ_AUDIO_STATS=<sec>  print level/timing stats every <sec> seconds (default 5 when KZ_AUDIO_WAV is set)
//   KZ_AUDIO_PACE=0       no real-time pacing of the SPU2 clock (see kz_audio.cpp "pacing")
//   KZ_AUDIO_TRACE=<n>    log the first n SPU2 register writes plus every KON/KOFF/ATTR write, DMA and IRQ

class PS2Runtime;

// Call before PS2Runtime::initialize(). Returns true when audio emulation is active.
// `outputDevice`: open the host audio device (false for automated runs, which only record with KZ_AUDIO_WAV).
// KZ_AUDIO_DEVICE=0/1 overrides it.
bool kzAudioInstall(bool outputDevice);
void kzAudioBindRuntime(PS2Runtime &runtime);
bool kzAudioActive();
