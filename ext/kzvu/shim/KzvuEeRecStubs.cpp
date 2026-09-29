// kzvu shim: EE-side symbols referenced by code kzvu compiles but never runs.
//
// x86/microVU.cpp is one translation unit that also contains microVU_Macro.inl, PCSX2's COP2 "macro mode" recompiler
// for the EE (VU0 instructions issued by the R5900 recompiler), and microVU_IR.h's register allocator has a COP2 mode
// that shares registers with the EE recompiler. kzvu only runs VU1 micro mode, so none of this is reachable; the
// definitions below exist only to satisfy the linker and fail loudly if something does reach them.
// SPDX-License-Identifier: GPL-3.0+

#include "kzvu_prefix.h"

#include "vtlb.h"

[[noreturn]] static void Unreachable(const char* what)
{
	pxFailRel(what);
	std::abort();
}
#define KZVU_UNREACHABLE() Unreachable(__FUNCTION__)

// ---- EE recompiler state (renamed to kzvu_* by kzvu_rename.h) -----------------------------------------------------------
u32 pc;
bool s_nBlockInterlocked;
alignas(16) GPR_reg64 g_cpuConstRegs[32];
u32 g_cpuHasConstReg, g_cpuFlushedConstReg;
_x86regs x86regs[iREGCNT_GPR], s_saveX86regs[iREGCNT_GPR];
_xmmregs xmmregs[iREGCNT_XMM], s_saveXMMregs[iREGCNT_XMM];
EEINST* g_pCurInstInfo;
u16 g_x86AllocCounter;

namespace EmuFolders
{
	std::string Logs; // microVU program dumps (mVUlogProg, off)
}

// ---- EE recompiler register allocation (iCore.cpp / iR5900.cpp) ---------------------------------------------------------
int _allocX86reg(int type, int reg, int mode) { KZVU_UNREACHABLE(); }
bool _hasX86reg(int type, int reg, int required_mode) { KZVU_UNREACHABLE(); }
void _freeX86reg(const x86Emitter::xRegister32& x86reg) { KZVU_UNREACHABLE(); }
void _freeX86reg(int x86reg) { KZVU_UNREACHABLE(); }
void _freeX86regWithoutWriteback(int x86reg) { KZVU_UNREACHABLE(); }
int _allocTempXMMreg(XMMSSEType type) { KZVU_UNREACHABLE(); }
int _allocGPRtoXMMreg(int gprreg, int mode) { KZVU_UNREACHABLE(); }
void _reallocateXMMreg(int xmmreg, int newtype, int newreg, int newmode, bool writeback) { KZVU_UNREACHABLE(); }
int _checkXMMreg(int type, int reg, int mode) { KZVU_UNREACHABLE(); }
void _freeXMMreg(int xmmreg) { KZVU_UNREACHABLE(); }
void _freeXMMregWithoutWriteback(int xmmreg) { KZVU_UNREACHABLE(); }
int _allocVFtoXMMreg(int vfreg, int mode) { KZVU_UNREACHABLE(); }
int _allocIfUsedGPRtoX86(int gprreg, int mode) { KZVU_UNREACHABLE(); }
int _allocIfUsedVItoX86(int vireg, int mode) { KZVU_UNREACHABLE(); }
int _allocIfUsedGPRtoXMM(int gprreg, int mode) { KZVU_UNREACHABLE(); }
bool TrySwapDelaySlot(u32 rs, u32 rt, u32 rd, bool allow_loadstore) { KZVU_UNREACHABLE(); }
void iFlushCall(int flushtype) { KZVU_UNREACHABLE(); }
void recCall(void (*func)()) { KZVU_UNREACHABLE(); }
u32 scaleblockcycles_clear() { KZVU_UNREACHABLE(); }
void R5900::Dynarec::recDoBranchImm(u32 branchTo, u32* jmpSkip, bool isLikely, bool swappedDelaySlot) { KZVU_UNREACHABLE(); }
void _eeMoveGPRtoR(const x86Emitter::xRegister32& to, int fromgpr, bool allow_preload) { KZVU_UNREACHABLE(); }
void _eeMoveGPRtoM(uptr to, int fromgpr) { KZVU_UNREACHABLE(); }
void _eeFlushAllDirty() { KZVU_UNREACHABLE(); }
void _deleteEEreg128(int reg) { KZVU_UNREACHABLE(); }

// ---- EE memory access (vtlb.cpp) ----------------------------------------------------------------------------------------
int vtlb_DynGenReadQuad(u32 bits, int addr_reg, vtlb_ReadRegAllocCallback dest_reg_alloc) { KZVU_UNREACHABLE(); }
int vtlb_DynGenReadQuad_Const(u32 bits, u32 addr_const, vtlb_ReadRegAllocCallback dest_reg_alloc) { KZVU_UNREACHABLE(); }
void vtlb_DynGenWrite(u32 sz, bool xmm, int addr_reg, int value_reg) { KZVU_UNREACHABLE(); }
void vtlb_DynGenWrite_Const(u32 bits, bool xmm, u32 addr_const, int value_reg) { KZVU_UNREACHABLE(); }

// ---- EE-side VU0/VU1 control (VU0.cpp, VU0micro.cpp, VU1micro.cpp, VUmicro.cpp) ---------------------------------------
void BaseVUmicroCPU::ExecuteBlockJIT(BaseVUmicroCPU* cpu, bool interlocked) { KZVU_UNREACHABLE(); }
void vu0ResetRegs() { KZVU_UNREACHABLE(); }
void _vu0WaitMicro() { KZVU_UNREACHABLE(); }
void _vu0FinishMicro() { KZVU_UNREACHABLE(); }
void vu1Finish(bool add_cycles) { KZVU_UNREACHABLE(); }
void vu1ResetRegs() { KZVU_UNREACHABLE(); }
void vu1ExecMicro(u32 addr) { KZVU_UNREACHABLE(); }
void VCALLMS() { KZVU_UNREACHABLE(); }
void VCALLMSR() { KZVU_UNREACHABLE(); }
