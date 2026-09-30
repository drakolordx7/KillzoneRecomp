// kzvu: public API (include/kzvu.h) on top of PCSX2's microVU1 / VU1 interpreter.
// Compiled with the kzvu_prefix.h forced include, so EmuConfig/cpuRegs/VU1/... below are kzvu's shim objects.
// SPDX-License-Identifier: GPL-3.0+

#include "kzvu.h"

#include "common/FPControl.h"

#include <cstring>

// VU0/VU1 micro and data memory. Static so they sit inside the executable image, within +-2 GB of the recompiler's
// text pointer (&cpuRegs.GPR.r[9]): microVU encodes constant VU memory addresses as 32-bit displacements from it.
alignas(64) static u8 s_vu0_micro[VU0_PROGSIZE];
alignas(64) static u8 s_vu0_mem[VU0_MEMSIZE];
alignas(64) static u8 s_vu1_micro[VU1_PROGSIZE];
alignas(64) static u8 s_vu1_mem[VU1_MEMSIZE];

static bool s_init = false;
static KzvuConfig s_cfg;
static thread_local u32 s_fbrst = 0; // per thread: VU0 calls (EE thread) and VU1 programs (VU1 thread) set their own
static u64 s_total_cycles = 0;
static u64 s_vu0_cycles = 0;
static u64 s_vu0_calls = 0;

// Default VU FPCR, same as PCSX2 (Pcsx2Config.cpp): exceptions masked, DAZ + FTZ, round toward zero.
static constexpr FPControlRegister KZVU_VU_FPCR = FPControlRegister::GetDefault()
													  .DisableExceptions()
													  .SetDenormalsAreZero(true)
													  .SetFlushToZero(true)
													  .SetRoundMode(FPRoundMode::ChopZero);

static void ApplyConfig(const KzvuConfig& cfg)
{
	s_cfg = cfg;
	if (s_cfg.clampMode < 0)
		s_cfg.clampMode = 0;
	if (s_cfg.clampMode > 3)
		s_cfg.clampMode = 3;

	auto& rec = EmuConfig.Cpu.Recompiler;
	rec.EnableVU1 = s_cfg.useJit;
	rec.EnableVU0 = s_cfg.vu0UseJit;
	if (s_cfg.vu0ClampMode > 3)
		s_cfg.vu0ClampMode = 3;
	const int vu0Clamp = s_cfg.vu0ClampMode < 0 ? s_cfg.clampMode : s_cfg.vu0ClampMode;
	rec.vu0Overflow = vu0Clamp >= 1;
	rec.vu0ExtraOverflow = vu0Clamp >= 2;
	rec.vu0SignOverflow = vu0Clamp >= 3;
	rec.vu0Underflow = false;
	rec.vu1Overflow = s_cfg.clampMode >= 1;
	rec.vu1ExtraOverflow = s_cfg.clampMode >= 2;
	rec.vu1SignOverflow = s_cfg.clampMode >= 3;
	rec.vu1Underflow = false;
	EmuConfig.Gamefixes.IbitHack = s_cfg.iBitHack;
	EmuConfig.Gamefixes.XgKickHack = s_cfg.xgkickHack;
	EmuConfig.Speedhacks.vuFlagHack = s_cfg.flagHack;
	EmuConfig.Speedhacks.vuThread = false;
	EmuConfig.Speedhacks.vu1Instant = true;
	EmuConfig.Cpu.FPUFPCR = KZVU_VU_FPCR;
	EmuConfig.Cpu.FPUDivFPCR = KZVU_VU_FPCR;
	EmuConfig.Cpu.VU0FPCR = KZVU_VU_FPCR;
	EmuConfig.Cpu.VU1FPCR = KZVU_VU_FPCR;

	CpuVU1 = s_cfg.useJit ? static_cast<BaseVUmicroCPU*>(&CpuMicroVU1) : static_cast<BaseVUmicroCPU*>(&CpuIntVU1);
	CpuVU0 = s_cfg.vu0UseJit ? static_cast<BaseVUmicroCPU*>(&CpuMicroVU0) : static_cast<BaseVUmicroCPU*>(&CpuIntVU0);
	kzvu::SetXgkickCallback(s_cfg.xgkick, s_cfg.xgkickUser);
}

static void ResetRegs()
{
	for (int i = 0; i < 2; i++)
	{
		std::memset(static_cast<void*>(&vuRegs[i]), 0, sizeof(VURegs));
		vuRegs[i].idx = i;
		vuRegs[i].VF[0].f.w = 1.0f;
	}
	VU0.Micro = s_vu0_micro;
	VU0.Mem = s_vu0_mem;
	VU1.Micro = s_vu1_micro;
	VU1.Mem = s_vu1_mem;
	std::memset(static_cast<void*>(&_cpuRegistersPack), 0, sizeof(_cpuRegistersPack));
	vif1Regs.top = 0;
	vif1Regs.itop = 0;
	vif1Regs.stat._u32 = 0;
	kzvu::GifReset();
}

// Switches MXCSR to the VU's control register for the duration of a run (PCSX2 runs the EE thread with the same
// value, so microVU's dispatcher does not switch it itself) and back to the host's afterwards.
class ScopedVuFPCR
{
public:
	ScopedVuFPCR()
		: m_host(FPControlRegister::GetCurrent())
	{
		kzvu::SetHostFPCR(m_host);
		FPControlRegister::SetCurrent(EmuConfig.Cpu.VU1FPCR);
	}
	~ScopedVuFPCR() { FPControlRegister::SetCurrent(m_host); }

private:
	FPControlRegister m_host;
};

// FBRST bits the VUs read: DE0/TE0 (2, 3) and DE1/TE1 (10, 11). FB/RS bits are write-triggered actions.
static constexpr u32 kFbrstStopEnables = 0x0C0C;

static u32 RunCycles(u32 budget)
{
	VU0.VI[REG_FBRST].UL = s_fbrst & kFbrstStopEnables;
	cpuRegs.cycle = VU1.cycle;
	const u64 start = VU1.cycle;
	CpuVU1->Execute(budget);
	const u64 used = VU1.cycle - start;
	s_total_cycles += used;
	return static_cast<u32>(used);
}

void kzvuBindVu1Thread()
{
	// This thread runs VU1 from now on: VU0.VI[VPU_STAT/FBRST] (kzvu_prefix.h) resolve to a private register file, so
	// they do not share words with VU0 running on another thread.
	kzvu_vu0 = &kzvu_vu0_vu1thread;
}

bool kzvuInit(const KzvuConfig& cfg, std::string* err)
{
	if (s_init)
	{
		kzvuSetConfig(cfg);
		return true;
	}

	x86Emitter::use_avx = g_cpu.vectorISA >= ProcessorFeatures::VectorISA::AVX;
	// One region for both recompilers: [mVU0recOffset, mVU1recOffset + mVU1recSize) of PCSX2's code map.
	if (!kzvu::AllocCodeCache(HostMemoryMap::mVU0recSize + HostMemoryMap::mVU1recSize, err))
		return false;

	ResetRegs();
	ApplyConfig(cfg);
	CpuMicroVU0.Reserve();
	CpuMicroVU0.Reset();
	CpuIntVU0.Reset();
	CpuMicroVU1.Reserve();
	CpuMicroVU1.Reset();
	CpuIntVU1.Reset();
	s_total_cycles = 0;
	s_vu0_cycles = 0;
	s_vu0_calls = 0;
	s_init = true;
	return true;
}

void kzvuShutdown()
{
	if (!s_init)
		return;
	CpuMicroVU0.Shutdown();
	CpuMicroVU1.Shutdown();
	kzvu::FreeCodeCache();
	s_init = false;
}

bool kzvuIsInit()
{
	return s_init;
}

void kzvuSetConfig(const KzvuConfig& cfg)
{
	const bool codegen_changed = cfg.clampMode != s_cfg.clampMode || cfg.vu0ClampMode != s_cfg.vu0ClampMode ||
								 cfg.iBitHack != s_cfg.iBitHack ||
								 cfg.flagHack != s_cfg.flagHack || cfg.xgkickHack != s_cfg.xgkickHack;
	ApplyConfig(cfg);
	if (s_init && codegen_changed)
	{
		CpuMicroVU0.Reset();
		CpuMicroVU1.Reset();
	}
}

const KzvuConfig& kzvuGetConfig()
{
	return s_cfg;
}

void kzvuReset()
{
	if (!s_init)
		return;
	ResetRegs();
	CpuMicroVU0.Reset();
	CpuIntVU0.Reset();
	CpuMicroVU1.Reset();
	CpuIntVU1.Reset();
}

// ---- memory -------------------------------------------------------------------------------------------------------------
uint8_t* kzvuCodeMem()
{
	return s_vu1_micro;
}

uint8_t* kzvuDataMem()
{
	return s_vu1_mem;
}

static void WriteMicroPart(u32 offset, const u8* src, u32 size)
{
	if (size == 0 || std::memcmp(s_vu1_micro + offset, src, size) == 0)
		return;
	std::memcpy(s_vu1_micro + offset, src, size);
	CpuMicroVU1.Clear(offset, size);
}

void kzvuWriteMicro(uint32_t offset, const void* src, uint32_t size)
{
	offset &= VU1_PROGMASK;
	if (size > VU1_PROGSIZE)
		size = VU1_PROGSIZE;
	const u8* p = static_cast<const u8*>(src);
	const u32 first = std::min<u32>(size, VU1_PROGSIZE - offset);
	WriteMicroPart(offset, p, first);
	WriteMicroPart(0, p + first, size - first);
}

void kzvuMicroWritten(uint32_t offset, uint32_t size)
{
	if (s_init)
		CpuMicroVU1.Clear(offset & VU1_PROGMASK, size);
}

// ---- VIF1 / VU0 control -------------------------------------------------------------------------------------------------
void kzvuSetTop(uint32_t top, uint32_t itop)
{
	vif1Regs.top = top & 0x3ff;
	vif1Regs.itop = itop & 0x3ff;
}

void kzvuSetFBRST(uint32_t fbrst)
{
	s_fbrst = fbrst;
}

// ---- execution ----------------------------------------------------------------------------------------------------------
uint32_t kzvuExecute(uint32_t startPcBytes, uint32_t maxCycles)
{
	if (!s_init)
		return 0;
	const ScopedVuFPCR fpcr;

	// vu1ExecMicro(): a still-running program is finished first.
	u32 used = 0;
	if (VU0.VI[REG_VPU_STAT].UL & 0x100)
	{
		used += RunCycles(vu1RunCycles);
		if (VU0.VI[REG_VPU_STAT].UL & 0x100)
		{
			Console.Warning("kzvu: force-stopping VU1, it ran for too long");
			VU0.VI[REG_VPU_STAT].UL &= ~0x100;
		}
	}

	VU0.VI[REG_VPU_STAT].UL &= ~0xFF00;
	VU0.VI[REG_VPU_STAT].UL |= 0x0100;
	if (startPcBytes != 0xFFFFFFFFu)
		VU1.VI[REG_TPC].UL = (startPcBytes >> 3) & 0x7FF;
	CpuVU1->SetStartPC(VU1.VI[REG_TPC].UL << 3);
	used += RunCycles(maxCycles);
	return used;
}

uint32_t kzvuContinue(uint32_t maxCycles)
{
	if (!s_init || !(VU0.VI[REG_VPU_STAT].UL & 0x100))
		return 0;
	const ScopedVuFPCR fpcr;
	return RunCycles(maxCycles);
}

bool kzvuRunning()
{
	return (VU0.VI[REG_VPU_STAT].UL & 0x100) != 0;
}

uint32_t kzvuVpuStat()
{
	return VU0.VI[REG_VPU_STAT].UL & 0xFF00;
}

bool kzvuTakeInterrupt()
{
	return (kzvu::TakeInterrupts(1u << INTC_VU1) & (1u << INTC_VU1)) != 0;
}

uint64_t kzvuCycles()
{
	return s_total_cycles;
}

uint64_t kzvuXgkickCount()
{
	return kzvu::GifPacketCount();
}

// ---- registers ----------------------------------------------------------------------------------------------------------
uint32_t kzvuGetVI(int index)
{
	return VU1.VI[index & 31].UL;
}

void kzvuSetVI(int index, uint32_t value)
{
	index &= 31;
	if (index == 0)
		return;
	if (index < 16)
		value &= 0xffff;
	VU1.VI[index].UL = value;
	// microVU keeps four pipelined instances of each flag; seed all of them.
	if (index == REG_STATUS_FLAG)
		for (u32& f : VU1.micro_statusflags) f = value;
	else if (index == REG_MAC_FLAG)
		for (u32& f : VU1.micro_macflags) f = value;
	else if (index == REG_CLIP_FLAG)
		for (u32& f : VU1.micro_clipflags) f = value;
}

void kzvuGetVF(int index, uint32_t out[4])
{
	std::memcpy(out, VU1.VF[index & 31].UL, 16);
}

void kzvuSetVF(int index, const uint32_t in[4])
{
	if ((index & 31) == 0)
		return;
	std::memcpy(VU1.VF[index & 31].UL, in, 16);
}

void kzvuGetACC(uint32_t out[4])
{
	std::memcpy(out, VU1.ACC.UL, 16);
}

void kzvuSetACC(const uint32_t in[4])
{
	std::memcpy(VU1.ACC.UL, in, 16);
}

// ---- VU0 micro mode -----------------------------------------------------------------------------------------------------
uint8_t* kzvu0CodeMem()
{
	return s_vu0_micro;
}

uint8_t* kzvu0DataMem()
{
	return s_vu0_mem;
}

void kzvu0MicroWritten(uint32_t offset, uint32_t size)
{
	if (s_init)
		CpuMicroVU0.Clear(offset & VU0_PROGMASK, size);
}

void kzvu0SetRegs(const Kzvu0Regs& in)
{
	std::memcpy(VU0.VF[1].UL, in.vf[1], 31 * 16);
	VU0.VF[0].UL[0] = VU0.VF[0].UL[1] = VU0.VF[0].UL[2] = 0;
	VU0.VF[0].f.w = 1.0f;
	VU0.VI[0].UL = 0;
	for (int i = 1; i < 16; i++)
		VU0.VI[i].UL = in.vi[i] & 0xffff;
	std::memcpy(VU0.ACC.UL, in.acc, 16);
	VU0.VI[REG_STATUS_FLAG].UL = in.status;
	VU0.VI[REG_MAC_FLAG].UL = in.mac;
	VU0.VI[REG_CLIP_FLAG].UL = in.clip;
	// Same as PCSX2's vu0ExecMicro(): the interpreter's flag copies, and microVU's four pipelined instances of each
	// flag (its status instances are kept in microVU's internal bit layout, see mVUallocSFLAGd()). Q is seeded in both
	// of microVU's instances (current + pending): no division is in flight between programs.
	VU0.clipflag = in.clip;
	VU0.macflag = in.mac;
	VU0.statusflag = in.status;
	const u32 microStatus = ((in.status >> 3) & 0x18u) | ((in.status >> 11) & 0x1800u) | ((in.status >> 14) & 0x3cf0000u);
	for (u32& f : VU0.micro_statusflags) f = microStatus;
	for (u32& f : VU0.micro_macflags) f = in.mac;
	for (u32& f : VU0.micro_clipflags) f = in.clip;
	VU0.VI[REG_R].UL = in.r;
	VU0.VI[REG_I].UL = in.i;
	VU0.VI[REG_Q].UL = in.q;
	VU0.pending_q = in.q;
}

void kzvu0GetRegs(Kzvu0Regs& out)
{
	std::memcpy(out.vf, VU0.VF, 32 * 16);
	for (int i = 0; i < 16; i++)
		out.vi[i] = VU0.VI[i].UL & 0xffff;
	std::memcpy(out.acc, VU0.ACC.UL, 16);
	out.status = VU0.VI[REG_STATUS_FLAG].UL;
	out.mac = VU0.VI[REG_MAC_FLAG].UL;
	out.clip = VU0.VI[REG_CLIP_FLAG].UL;
	out.r = VU0.VI[REG_R].UL;
	out.i = VU0.VI[REG_I].UL;
	out.q = VU0.VI[REG_Q].UL;
}

static u32 RunCycles0(u32 budget)
{
	VU0.VI[REG_FBRST].UL = s_fbrst & kFbrstStopEnables;
	cpuRegs.cycle = VU0.cycle;
	const u64 start = VU0.cycle;
	CpuVU0->Execute(budget);
	const u64 used = VU0.cycle - start;
	s_vu0_cycles += used;
	return static_cast<u32>(used);
}

// Runs VU0 while VBS0 is set: past M-bit pauses, until the E bit, a D/T stop or the budget.
static u32 RunVu0ToEnd(u32 maxCycles)
{
	u32 used = 0;
	// Every Execute() makes progress (at least one instruction pair or one block), so this terminates; the iteration
	// cap only guards against a program that pauses on an M bit without consuming cycles.
	for (int iter = 0; iter < 1 << 20 && (VU0.VI[REG_VPU_STAT].UL & 0x1) && used < maxCycles; iter++)
		used += RunCycles0(maxCycles - used);
	return used;
}

uint32_t kzvu0Execute(uint32_t startPcBytes, uint32_t maxCycles)
{
	if (!s_init)
		return 0;
	const ScopedVuFPCR fpcr;
	s_vu0_calls++;

	// vu0ExecMicro(): a still-running program is finished first (PCSX2's vu0Finish).
	u32 used = 0;
	if (VU0.VI[REG_VPU_STAT].UL & 0x1)
	{
		used += RunVu0ToEnd(maxCycles);
		if (VU0.VI[REG_VPU_STAT].UL & 0x1)
		{
			Console.Warning("kzvu: force-stopping VU0, it ran for too long");
			VU0.VI[REG_VPU_STAT].UL &= ~0x1;
		}
	}

	VU0.VI[REG_VPU_STAT].UL &= ~0xFF;
	VU0.VI[REG_VPU_STAT].UL |= 0x01;
	if (startPcBytes != 0xFFFFFFFFu)
		VU0.VI[REG_TPC].UL = (startPcBytes >> 3) & (VU0_PROGMASK >> 3);
	CpuVU0->SetStartPC(VU0.VI[REG_TPC].UL << 3);
	used += RunVu0ToEnd(maxCycles);
	kzvu::TakeInterrupts(1u << INTC_VU0); // D/T stops are reported through VPU_STAT; the host has no VU0 INTC line
	return used;
}

bool kzvu0Running()
{
	return (VU0.VI[REG_VPU_STAT].UL & 0x1) != 0;
}

uint32_t kzvu0VpuStat()
{
	return VU0.VI[REG_VPU_STAT].UL & 0xFF;
}

uint32_t kzvu0TPC()
{
	return VU0.VI[REG_TPC].UL << 3;
}

uint64_t kzvu0Cycles()
{
	return s_vu0_cycles;
}

uint64_t kzvu0Calls()
{
	return s_vu0_calls;
}
