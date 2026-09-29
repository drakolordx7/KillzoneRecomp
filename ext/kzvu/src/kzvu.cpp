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
static u32 s_fbrst = 0;
static u64 s_total_cycles = 0;

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

static u32 RunCycles(u32 budget)
{
	VU0.VI[REG_FBRST].UL = s_fbrst & 0xFF00;
	cpuRegs.cycle = VU1.cycle;
	const u64 start = VU1.cycle;
	CpuVU1->Execute(budget);
	const u64 used = VU1.cycle - start;
	s_total_cycles += used;
	return static_cast<u32>(used);
}

bool kzvuInit(const KzvuConfig& cfg, std::string* err)
{
	if (s_init)
	{
		kzvuSetConfig(cfg);
		return true;
	}

	x86Emitter::use_avx = g_cpu.vectorISA >= ProcessorFeatures::VectorISA::AVX;
	if (!kzvu::AllocCodeCache(HostMemoryMap::mVU1recSize, err))
		return false;

	ResetRegs();
	ApplyConfig(cfg);
	CpuMicroVU1.Reserve();
	CpuMicroVU1.Reset();
	CpuIntVU1.Reset();
	s_total_cycles = 0;
	s_init = true;
	return true;
}

void kzvuShutdown()
{
	if (!s_init)
		return;
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
	const bool codegen_changed = cfg.clampMode != s_cfg.clampMode || cfg.iBitHack != s_cfg.iBitHack ||
								 cfg.flagHack != s_cfg.flagHack || cfg.xgkickHack != s_cfg.xgkickHack;
	ApplyConfig(cfg);
	if (s_init && codegen_changed)
		CpuMicroVU1.Reset();
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
	const FPControlRegisterBackup fpcr(EmuConfig.Cpu.VU1FPCR);

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
	const FPControlRegisterBackup fpcr(EmuConfig.Cpu.VU1FPCR);
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
	return (kzvu::TakeInterrupts() & (1u << INTC_VU1)) != 0;
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
