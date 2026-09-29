// kzspu2 API, plus the parts of PCSX2's SPU2/spu2.cpp that are not audio-backend code: the register and DMA entry
// points, the DMA-end hooks, reset, and spu2Output(), which here feeds a lock-free ring instead of an AudioStream.
// SPDX-License-Identifier: GPL-3.0+

#include "kzspu2_prefix.h"

#include "kzspu2.h"

#include <atomic>
#include <vector>

const StereoOut32 StereoOut32::Empty(0, 0);

u64 lClocks = 0;
float DCFilterIn[2], DCFilterOut[2];

namespace
{
	KzSpu2Config s_cfg;
	bool s_init = false;

	// Output ring: interleaved stereo s16, positions count frames and only grow.
	std::vector<int16_t> s_ring;
	uint32_t s_ringFrames = 0;
	std::atomic<uint64_t> s_ringWrite{0};
	std::atomic<uint64_t> s_ringRead{0};
	uint64_t s_framesMixed = 0;
	uint64_t s_framesDropped = 0;

	// Staging block filled by spu2Output(), flushed to the tap and the ring.
	constexpr uint32_t kBlockFrames = 256;
	int16_t s_block[kBlockFrames * 2];
	uint32_t s_blockFrames = 0;
	int16_t s_peak[2] = {};

	void FlushBlock()
	{
		if (!s_blockFrames)
			return;
		const uint32_t frames = s_blockFrames;
		s_blockFrames = 0;
		s_framesMixed += frames;
		for (uint32_t i = 0; i < frames * 2; ++i)
		{
			const int v = s_block[i] < 0 ? -s_block[i] : s_block[i];
			int16_t& p = s_peak[i & 1];
			if (v > p)
				p = static_cast<int16_t>(std::min(v, 32767));
		}
		if (s_cfg.samples)
			s_cfg.samples(s_cfg.user, s_block, frames);
		if (!s_ringFrames)
			return;

		const uint64_t w = s_ringWrite.load(std::memory_order_relaxed);
		const uint64_t r = s_ringRead.load(std::memory_order_acquire);
		const uint32_t room = s_ringFrames - static_cast<uint32_t>(w - r);
		const uint32_t n = std::min(room, frames);
		for (uint32_t i = 0; i < n; ++i)
		{
			const uint32_t slot = static_cast<uint32_t>((w + i) % s_ringFrames);
			s_ring[slot * 2 + 0] = s_block[i * 2 + 0];
			s_ring[slot * 2 + 1] = s_block[i * 2 + 1];
		}
		s_ringWrite.store(w + n, std::memory_order_release);
		s_framesDropped += frames - n;
	}

	// PCSX2 SPU2 time runs off psxRegs.cycle. The host's cycle never goes backwards; tolerate it anyway.
	void SetClock(uint64_t iopCycle)
	{
		if (iopCycle > psxRegs.cycle)
			psxRegs.cycle = iopCycle;
	}

	void InternalReset()
	{
		// PCSX2's SPU2::InternalReset(false)
		spu2Mix = MULTI_ISA_SELECT(spu2Mix);
		ReverbDownsample = MULTI_ISA_SELECT(ReverbDownsample);
		ReverbUpsample = MULTI_ISA_SELECT(ReverbUpsample);

		std::memset(spu2regs, 0, 0x010000);
		std::memset(_spu2mem, 0, 0x200000);
		std::memset(_spu2mem + 0x2800, 7, 0x10); // from BIOS reversal. Locks the voices so they don't run free.
		std::memset(_spu2mem + 0xe870, 7, 0x10); // Loop which gets left over by the BIOS.
		// kzspu2: PCSX2 keeps stale ADPCM cache entries across a reset; the memory was just cleared, so drop them.
		for (auto& e : pcm_cache_data)
			e.Validated = false;

		std::memset(DCFilterIn, 0, sizeof(DCFilterIn));
		std::memset(DCFilterOut, 0, sizeof(DCFilterOut));

		Spdif.Info = 0;
		Cores[0].Init(0);
		Cores[1].Init(1);

		std::memset(iopHw, 0, sizeof(iopHw));
		std::memset(psxCounters, 0, sizeof(psxCounters));
		psxCounters[6].startCycle = psxRegs.cycle;
		psxCounters[6].deltaCycles = 0x7fffffff;
		psxNextStartCounter = psxRegs.cycle;
		psxNextDeltaCounter = 0x7fffffff;
		lClocks = psxRegs.cycle;
		s_blockFrames = 0;
	}

	u32 ToPhys(u32 addr) { return addr & 0x1FFFFFFFu; }
} // namespace

// ---- spu2.cpp: callbacks from the IOP DMA (sizes in 16-bit units) ---------------------------------------------------------------
void SPU2readDMA4Mem(u16* pMem, u32 size)
{
	TimeUpdate(psxRegs.cycle);
	Cores[0].DoDMAread(pMem, size);
}

void SPU2writeDMA4Mem(u16* pMem, u32 size)
{
	TimeUpdate(psxRegs.cycle);
	Cores[0].DoDMAwrite(pMem, size);
}

void SPU2interruptDMA4()
{
	if (Cores[0].DmaMode)
		Cores[0].Regs.STATX |= 0x80;
	Cores[0].Regs.STATX &= ~0x400;
	Cores[0].TSA = Cores[0].ActiveTSA;
}

void SPU2interruptDMA7()
{
	if (Cores[1].DmaMode)
		Cores[1].Regs.STATX |= 0x80;
	Cores[1].Regs.STATX &= ~0x400;
	Cores[1].TSA = Cores[1].ActiveTSA;
}

void SPU2readDMA7Mem(u16* pMem, u32 size)
{
	TimeUpdate(psxRegs.cycle);
	Cores[1].DoDMAread(pMem, size);
}

void SPU2writeDMA7Mem(u16* pMem, u32 size)
{
	TimeUpdate(psxRegs.cycle);
	Cores[1].DoDMAwrite(pMem, size);
}

u32 SPU2::GetConsoleSampleRate()
{
	return SAMPLE_RATE;
}

bool SPU2::IsRunningPSXMode()
{
	return false; // PS1 mode is not supported
}

void SPU2async()
{
	TimeUpdate(psxRegs.cycle);
}

u16 SPU2read(u32 rmem)
{
	u16 ret = 0xDEAD;
	u32 core = 0;
	const u32 mem = rmem & 0xFFFF;
	u32 omem = mem;

	if (mem & 0x400)
	{
		omem ^= 0x400;
		core = 1;
	}

	if (omem == 0x1f9001AC) // (never true: omem is masked to 16 bits; kept as in PCSX2)
	{
		Cores[core].ActiveTSA = Cores[core].TSA;
		for (int i = 0; i < 2; i++)
		{
			if (Cores[i].IRQEnable && (Cores[i].IRQA == Cores[core].ActiveTSA))
				SetIrqCall(i);
		}
		ret = Cores[core].DmaRead();
	}
	else
	{
		TimeUpdate(psxRegs.cycle);

		if (rmem >> 16 == 0x1f80)
			ret = Cores[0].ReadRegPS1(rmem);
		else if (mem >= 0x800)
			ret = spu2Ru16(mem);
		else
			ret = *(regtable[(mem >> 1)]);
	}

	return ret;
}

void SPU2write(u32 rmem, u16 value)
{
	TimeUpdate(psxRegs.cycle);

	if (rmem >> 16 == 0x1f80)
		Cores[0].WriteRegPS1(rmem, value);
	else
		SPU2_FastWrite(rmem, value);
}

static void DCFilter(float* input)
{
	// PCSX2's DC blocking high-pass filter (http://peabody.sapp.org/class/dmp2/lab/dcblock/)
	float output[2];
	output[0] = (input[0] - DCFilterIn[0] + ((0.995f * DCFilterOut[0])));
	output[1] = (input[1] - DCFilterIn[1] + ((0.995f * DCFilterOut[1])));

	DCFilterIn[0] = input[0];
	DCFilterIn[1] = input[1];
	DCFilterOut[0] = output[0];
	DCFilterOut[1] = output[1];

	input[0] = output[0];
	input[1] = output[1];
}

void spu2Output(StereoOut32 out)
{
	int16_t l = static_cast<int16_t>(clamp_mix(out.Left));
	int16_t r = static_cast<int16_t>(clamp_mix(out.Right));
	if (s_cfg.dcFilter)
	{
		float conv[2] = {static_cast<float>(l), static_cast<float>(r)};
		DCFilter(conv);
		l = static_cast<int16_t>(std::clamp(std::lround(conv[0]), -32768l, 32767l));
		r = static_cast<int16_t>(std::clamp(std::lround(conv[1]), -32768l, 32767l));
	}
	s_block[s_blockFrames * 2 + 0] = l;
	s_block[s_blockFrames * 2 + 1] = r;
	if (++s_blockFrames == kBlockFrames)
		FlushBlock();
}

// ---- API ------------------------------------------------------------------------------------------------------------------
bool kzspu2Init(const KzSpu2Config& config)
{
	s_cfg = config;
	kzspu2::g_host.user = config.user;
	kzspu2::g_host.irq = config.irq;
	kzspu2::g_host.dmaComplete = config.dmaComplete;
	kzspu2::g_host.log = config.log;

	s_ringFrames = std::max<uint32_t>(config.ringFrames, 0);
	s_ring.assign(static_cast<size_t>(s_ringFrames) * 2, 0);
	s_ringWrite.store(0);
	s_ringRead.store(0);
	s_framesMixed = 0;
	s_framesDropped = 0;
	kzspu2::g_irqCount = 0;
	kzspu2::g_dmaIrqCount[0] = kzspu2::g_dmaIrqCount[1] = 0;

	psxRegs.cycle = 0;
	InternalReset();
	s_init = true;
	return true;
}

void kzspu2Shutdown()
{
	s_init = false;
	s_ring.clear();
	s_ring.shrink_to_fit();
	s_ringFrames = 0;
	kzspu2::g_host = {};
}

void kzspu2Reset(uint64_t iopCycle)
{
	SetClock(iopCycle);
	InternalReset();
}

void kzspu2SetIopRam(uint8_t* base, uint32_t size)
{
	kzspu2::SetIopRam(base, size);
}

void kzspu2Advance(uint64_t iopCycle)
{
	if (!s_init)
		return;
	SetClock(iopCycle);
	TimeUpdate(psxRegs.cycle);
	FlushBlock();
}

uint64_t kzspu2NextEventCycle()
{
	uint64_t next = UINT64_MAX;
	if (!s_init)
		return next;
	for (int c = 0; c < 2; ++c)
	{
		const V_Core& core = Cores[c];
		if (core.DMAICounter > 0)
			next = std::min<uint64_t>(next, core.LastClock + static_cast<uint64_t>(core.DMAICounter));
	}
	return next;
}

bool kzspu2HandlesAddress(uint32_t iopAddr)
{
	const u32 phys = ToPhys(iopAddr);
	return phys >= 0x1F900000u && phys < 0x1F900800u;
}

uint16_t kzspu2Read16(uint32_t iopAddr, uint64_t iopCycle)
{
	if (!s_init)
		return 0;
	SetClock(iopCycle);
	const u16 v = SPU2read(ToPhys(iopAddr));
	FlushBlock();
	return v;
}

void kzspu2Write16(uint32_t iopAddr, uint16_t value, uint64_t iopCycle)
{
	if (!s_init)
		return;
	SetClock(iopCycle);
	SPU2write(ToPhys(iopAddr), value);
	FlushBlock();
}

static void StartDma(int core, uint16_t* iopRam, uint32_t halfwords, uint32_t madr, uint64_t iopCycle, bool write)
{
	if (!s_init || core < 0 || core > 1)
		return;
	SetClock(iopCycle);
	const u32 words = halfwords / 2;
	u32& MADR_ = core ? HW_DMA7_MADR : HW_DMA4_MADR;
	u32& BCR_ = core ? HW_DMA7_BCR : HW_DMA4_BCR;
	u32& CHCR_ = core ? HW_DMA7_CHCR : HW_DMA4_CHCR;
	MADR_ = madr;
	BCR_ = (std::max<u32>(words, 1u) << 16) | 1u;
	CHCR_ = write ? 0x01000201u : 0x01000200u;
	if (write)
		(core ? SPU2writeDMA7Mem : SPU2writeDMA4Mem)(iopRam, halfwords);
	else
		(core ? SPU2readDMA7Mem : SPU2readDMA4Mem)(iopRam, halfwords);
	FlushBlock();
}

void kzspu2DmaWrite(int core, uint16_t* iopRam, uint32_t halfwords, uint32_t madr, uint64_t iopCycle)
{
	StartDma(core, iopRam, halfwords, madr, iopCycle, true);
}

void kzspu2DmaRead(int core, uint16_t* iopRam, uint32_t halfwords, uint32_t madr, uint64_t iopCycle)
{
	StartDma(core, iopRam, halfwords, madr, iopCycle, false);
}

bool kzspu2DmaBusy(int core)
{
	return ((core ? HW_DMA7_CHCR : HW_DMA4_CHCR) & 0x01000000u) != 0;
}

uint32_t kzspu2DmaMadr(int core)
{
	return core ? HW_DMA7_MADR : HW_DMA4_MADR;
}

uint32_t kzspu2AvailableFrames()
{
	return static_cast<uint32_t>(s_ringWrite.load(std::memory_order_acquire) - s_ringRead.load(std::memory_order_relaxed));
}

uint32_t kzspu2ReadSamples(int16_t* stereo, uint32_t frames)
{
	if (!s_ringFrames)
		return 0;
	const uint64_t r = s_ringRead.load(std::memory_order_relaxed);
	const uint64_t w = s_ringWrite.load(std::memory_order_acquire);
	const uint32_t n = std::min<uint32_t>(frames, static_cast<uint32_t>(w - r));
	for (uint32_t i = 0; i < n; ++i)
	{
		const uint32_t slot = static_cast<uint32_t>((r + i) % s_ringFrames);
		stereo[i * 2 + 0] = s_ring[slot * 2 + 0];
		stereo[i * 2 + 1] = s_ring[slot * 2 + 1];
	}
	s_ringRead.store(r + n, std::memory_order_release);
	return n;
}

uint32_t kzspu2SkipFrames(uint32_t frames)
{
	const uint64_t r = s_ringRead.load(std::memory_order_relaxed);
	const uint64_t w = s_ringWrite.load(std::memory_order_acquire);
	const uint32_t n = std::min<uint32_t>(frames, static_cast<uint32_t>(w - r));
	s_ringRead.store(r + n, std::memory_order_release);
	return n;
}

KzSpu2Status kzspu2GetStatus(bool resetPeak)
{
	KzSpu2Status s{};
	s.iopCycle = psxRegs.cycle;
	s.framesMixed = s_framesMixed;
	s.framesDropped = s_framesDropped;
	s.irqCount = kzspu2::g_irqCount;
	s.dmaCount[0] = kzspu2::g_dmaIrqCount[0];
	s.dmaCount[1] = kzspu2::g_dmaIrqCount[1];
	for (int c = 0; c < 2; ++c)
	{
		u32 n = 0;
		for (const V_Voice& v : Cores[c].Voices)
			n += v.ADSR.Phase != V_ADSR::PHASE_STOPPED ? 1u : 0u;
		s.keyedOnVoices[c] = n;
		s.coreAttr[c] = Cores[c].Regs.ATTR;
	}
	s.peak[0] = s_peak[0];
	s.peak[1] = s_peak[1];
	if (resetPeak)
		s_peak[0] = s_peak[1] = 0;
	return s;
}

const uint16_t* kzspu2Memory()
{
	return reinterpret_cast<const uint16_t*>(_spu2mem);
}
