// kzipu: forced include for every kzipu translation unit (see CMakeLists.txt).
//
// kzipu is linked into the same executable as kzgs and kzvu, which carry their own copies of PCSX2's emulator globals
// (eeHw, cpuRegs, FMVstarted, ...) and of pcsx2/common. Everything kzipu *defines* is renamed here, before any PCSX2
// header declares it, so no kzipu symbol can collide with another PCSX2-derived library:
//   - the emulator state and the DMAC/INTC/event helpers kzipu implements in shim/KzipuShims.cpp
//   - every global the PCSX2 IPU sources define (registers, FIFOs, decoder state, commands, DMA handlers)
//   - the few pcsx2/common functions the IPU code references (kzipu does not compile pcsx2/common at all)
// SPDX-License-Identifier: GPL-3.0+
#pragma once

// ---- emulator state + helpers implemented by the shim ----------------------------------------------------------------
#define eeHw kzipu_eeHw
#define _cpuRegistersPack kzipu_cpuRegistersPack
#define CPU_INT kzipu_CPU_INT
#define CPU_SET_DMASTALL kzipu_CPU_SET_DMASTALL
#define cpuClearInt kzipu_cpuClearInt
#define hwIntcIrq kzipu_hwIntcIrq
#define hwDmacIrq kzipu_hwDmacIrq
#define dmaGetAddr kzipu_dmaGetAddr
#define SPRdmaGetAddr kzipu_SPRdmaGetAddr
#define hwDmacSrcChain kzipu_hwDmacSrcChain
#define hwDmacSrcChainWithStack kzipu_hwDmacSrcChainWithStack
#define hwDmacSrcTadrInc kzipu_hwDmacSrcTadrInc
#define throwBusError kzipu_throwBusError
#define setDmacStat kzipu_setDmacStat
#define DMACh KzipuDMACh
#define SaveStateBase KzipuSaveStateBase
#define pxOnAssertFail kzipu_pxOnAssertFail
#define StdStringFromFormat kzipu_StdStringFromFormat

// ---- PCSX2 IPU globals, types and functions ----------------------------------------------------------------------------
#define isa_native kzipu_isa
#define ipu_fifo kzipu_ipu_fifo
#define IPU_Fifo KzipuIPU_Fifo
#define IPU_Fifo_Input KzipuIPU_Fifo_Input
#define IPU_Fifo_Output KzipuIPU_Fifo_Output
#define ipu_cmd kzipu_ipu_cmd
#define tIPU_cmd kzipu_tIPU_cmd
#define tIPU_CMD_IDEC kzipu_tIPU_CMD_IDEC
#define tIPU_CMD_BDEC kzipu_tIPU_CMD_BDEC
#define tIPU_CMD_CSC kzipu_tIPU_CMD_CSC
#define g_BP kzipu_g_BP
#define decoder kzipu_decoder
#define IPUCoreStatus kzipu_IPUCoreStatus
#define IPU1Status kzipu_IPU1Status
#define g_ipu_vqclut kzipu_g_ipu_vqclut
#define g_ipu_thresh kzipu_g_ipu_thresh
#define g_ipu_indx4 kzipu_g_ipu_indx4
#define coded_block_pattern kzipu_coded_block_pattern
#define non_linear_quantizer_scale kzipu_non_linear_quantizer_scale
#define eecount_on_last_vdec kzipu_eecount_on_last_vdec
#define FMVstarted kzipu_FMVstarted
#define EnableFMV kzipu_EnableFMV
#define g_idct_clip_lut kzipu_g_idct_clip_lut
#define mpeg2_scan kzipu_mpeg2_scan
#define ipuReset kzipu_ipuReset
#define ReportIPU kzipu_ReportIPU
#define ipuRead32 kzipu_ipuRead32
#define ipuRead64 kzipu_ipuRead64
#define ipuWrite32 kzipu_ipuWrite32
#define ipuWrite64 kzipu_ipuWrite64
#define ipuSoftReset kzipu_ipuSoftReset
#define IPUCMD_WRITE kzipu_IPUCMD_WRITE
#define IPUProcessInterrupt kzipu_IPUProcessInterrupt
#define ReadFIFO_IPUout kzipu_ReadFIFO_IPUout
#define WriteFIFO_IPUin kzipu_WriteFIFO_IPUin
#define ipuDmaReset kzipu_ipuDmaReset
#define ipuCMDProcess kzipu_ipuCMDProcess
#define ipu0Interrupt kzipu_ipu0Interrupt
#define ipu1Interrupt kzipu_ipu1Interrupt
#define dmaIPU0 kzipu_dmaIPU0
#define dmaIPU1 kzipu_dmaIPU1
#define IPU0dma kzipu_IPU0dma
#define IPU1dma kzipu_IPU1dma
