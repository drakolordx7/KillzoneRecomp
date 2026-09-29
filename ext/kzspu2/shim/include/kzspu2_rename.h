// kzspu2: renames every symbol kzspu2 defines, before any PCSX2 header declares it.
//
// kzspu2 is linked into the same executable as kzgs, kzvu and kzipu, which carry their own copies of PCSX2 globals.
// Everything the SPU2 sources and the shims define gets a kzspu2-private name here, so no kzspu2 symbol can collide
// with another PCSX2-derived library (checked with dumpbin, see README.md).
// SPDX-License-Identifier: GPL-3.0+
#pragma once

// ---- PCSX2 SPU2 globals and functions ---------------------------------------------------------------------------------
#define Cores kzspu2_Cores
#define Spdif kzspu2_Spdif
#define OutPos kzspu2_OutPos
#define InputPos kzspu2_InputPos
#define Cycles kzspu2_Cycles
#define spu2regs kzspu2_spu2regs
#define _spu2mem kzspu2_spu2mem
#define PlayMode kzspu2_PlayMode
#define SetIrqCall kzspu2_SetIrqCall
#define SetIrqCallDMA kzspu2_SetIrqCallDMA
#define StartVoices kzspu2_StartVoices
#define StopVoices kzspu2_StopVoices
#define CalculateADSR kzspu2_CalculateADSR
#define UpdateSpdifMode kzspu2_UpdateSpdifMode
#define regtable kzspu2_regtable
#define spu2Mix kzspu2_spu2Mix
#define GetMemPtr kzspu2_GetMemPtr
#define spu2M_Read kzspu2_spu2M_Read
#define spu2M_Write kzspu2_spu2M_Write
#define spu2Output kzspu2_spu2Output
#define pcm_cache_data kzspu2_pcm_cache_data
#define g_counter_cache_hits kzspu2_g_counter_cache_hits
#define g_counter_cache_misses kzspu2_g_counter_cache_misses
#define g_counter_cache_ignores kzspu2_g_counter_cache_ignores
#define ReverbUpsample kzspu2_ReverbUpsample
#define ReverbDownsample kzspu2_ReverbDownsample
#define DebugCores kzspu2_DebugCores
#define lClocks kzspu2_lClocks
#define TimeUpdate kzspu2_TimeUpdate
#define CounterUpdate kzspu2_CounterUpdate
#define CheckDMAProgress kzspu2_CheckDMAProgress
#define SPU2_FastWrite kzspu2_SPU2_FastWrite
#define SPU2write kzspu2_SPU2write
#define SPU2read kzspu2_SPU2read
#define SPU2async kzspu2_SPU2async
#define SPU2freeze kzspu2_SPU2freeze
#define SPU2readDMA4Mem kzspu2_SPU2readDMA4Mem
#define SPU2writeDMA4Mem kzspu2_SPU2writeDMA4Mem
#define SPU2interruptDMA4 kzspu2_SPU2interruptDMA4
#define SPU2interruptDMA7 kzspu2_SPU2interruptDMA7
#define SPU2readDMA7Mem kzspu2_SPU2readDMA7Mem
#define SPU2writeDMA7Mem kzspu2_SPU2writeDMA7Mem
#define DCFilterIn kzspu2_DCFilterIn
#define DCFilterOut kzspu2_DCFilterOut
#define isa_native kzspu2_isa
#define SPU2 kzspu2_SPU2
#define SPU2Savestate kzspu2_SPU2Savestate
#define WaveDump kzspu2_WaveDump

// ---- SPU2 types (their member functions are link symbols) --------------------------------------------------------------
#define StereoOut32 Kzspu2StereoOut32
#define V_VolumeLR Kzspu2V_VolumeLR
#define V_VolumeSlide Kzspu2V_VolumeSlide
#define V_VolumeSlideLR Kzspu2V_VolumeSlideLR
#define V_ADSR Kzspu2V_ADSR
#define V_Voice Kzspu2V_Voice
#define V_VoiceDebug Kzspu2V_VoiceDebug
#define V_CoreDebug Kzspu2V_CoreDebug
#define V_Reverb Kzspu2V_Reverb
#define V_SPDIF Kzspu2V_SPDIF
#define V_CoreRegs Kzspu2V_CoreRegs
#define V_VoiceGates Kzspu2V_VoiceGates
#define V_CoreGates Kzspu2V_CoreGates
#define VoiceMixSet Kzspu2VoiceMixSet
#define V_Core Kzspu2V_Core
#define PcmCacheEntry Kzspu2PcmCacheEntry
#define FreezeAction Kzspu2FreezeAction
#define freezeData Kzspu2freezeData

// ---- the IOP state the SPU2 code touches, implemented by shim/Kzspu2Shims.cpp (see the shadow headers) ----------------
#define psxRegs kzspu2_psxRegs
#define psxRegisters Kzspu2psxRegisters
#define psxCounters kzspu2_psxCounters
#define psxCounter Kzspu2psxCounter
#define psxNextDeltaCounter kzspu2_psxNextDeltaCounter
#define psxNextStartCounter kzspu2_psxNextStartCounter
#define iopHw kzspu2_iopHw
#define iopPhysMem kzspu2_iopPhysMem
#define spu2Irq kzspu2_spu2Irq
#define spu2DMA4Irq kzspu2_spu2DMA4Irq
#define spu2DMA7Irq kzspu2_spu2DMA7Irq
#define pxOnAssertFail kzspu2_pxOnAssertFail
