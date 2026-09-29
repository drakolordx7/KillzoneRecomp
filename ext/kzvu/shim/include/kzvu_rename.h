// kzvu: forced include for every kzvu translation unit (see CMakeLists.txt).
//
// kzvu is linked into the same executable as kzgs, which carries its own copy of pcsx2/common and its own stubs for
// PCSX2's emulator globals. Everything kzvu *defines* that kzgs (or any other PCSX2-derived library) could also define
// is renamed here, before any PCSX2 header declares it, so the two libraries never define the same symbol:
//   - emulator state kzvu owns (eeHw, cpuRegs, VU registers, the microVU instances, g_cpu, SysMemory, ...)
//   - host hooks the pcsx2/common slice calls back into (Host::, CrashHandler::), implemented in shim/KzvuShims.cpp
// The pcsx2/common objects kzvu carries (Console.cpp, Assertions.cpp, ...) keep their original names: they are
// compiled from the same sources as kzgs' copies, so whichever copy the linker picks defines the same symbol set.
// SPDX-License-Identifier: GPL-3.0+
#pragma once

// ---- emulator state ---------------------------------------------------------------------------------------------------
#define eeHw kzvu_eeHw
#define _cpuRegistersPack kzvu_cpuRegistersPack
#define vuRegs kzvu_vuRegs
#define microVU0 kzvu_microVU0
#define microVU1 kzvu_microVU1
#define g_cpu kzvu_g_cpu
#define SysMemory KzvuSysMemory
#define SaveStateBase KzvuSaveStateBase
#define hwIntcIrq kzvu_hwIntcIrq
#define CPU_INT kzvu_CPU_INT
#define CpuVU0 kzvu_CpuVU0
#define CpuVU1 kzvu_CpuVU1
#define CpuIntVU0 kzvu_CpuIntVU0
#define CpuIntVU1 kzvu_CpuIntVU1
#define CpuMicroVU0 kzvu_CpuMicroVU0
#define CpuMicroVU1 kzvu_CpuMicroVU1
#define vu1branch kzvu_vu1branch
#define vu0branch kzvu_vu0branch
#define mVUdebugNow kzvu_mVUdebugNow
#define EmuFolders KzvuEmuFolders

// ---- EE recompiler state referenced by microVU_Macro.inl (COP2 macro mode, never run by kzvu; see KzvuEeRecStubs.cpp)
#define pc kzvu_pc
#define x86regs kzvu_x86regs
#define s_saveX86regs kzvu_s_saveX86regs
#define xmmregs kzvu_xmmregs
#define s_saveXMMregs kzvu_s_saveXMMregs
#define g_pCurInstInfo kzvu_g_pCurInstInfo
#define g_x86AllocCounter kzvu_g_x86AllocCounter
#define s_nBlockInterlocked kzvu_s_nBlockInterlocked
#define g_cpuConstRegs kzvu_g_cpuConstRegs
#define g_cpuHasConstReg kzvu_g_cpuHasConstReg
#define g_cpuFlushedConstReg kzvu_g_cpuFlushedConstReg

// ---- host hooks used by the pcsx2/common slice -------------------------------------------------------------------------
#define Host KzvuHost
#define CrashHandler KzvuCrashHandler
