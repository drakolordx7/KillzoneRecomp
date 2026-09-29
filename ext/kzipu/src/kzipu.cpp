// kzipu public API (include/kzipu.h) on top of PCSX2's IPU sources and the shims in shim/.
// SPDX-License-Identifier: GPL-3.0+

#include "kzipu.h"

static bool s_open = false;
static bool s_mapIdecQsc = true;

// PCSX2's ipuIDEC stores the command's QSC as decoder.quantizer_scale unmapped (BDEC maps it). IDEC decoding only
// starts from the IPU_PROCESS event scheduled by the command write, so the scale can be corrected right after it.
static void AfterCmdWrite(uint32_t value)
{
	if (s_mapIdecQsc && (value >> 28) == SCE_IPU_IDEC)
	{
		const uint32_t qsc = (value >> 16) & 0x1F;
		decoder.quantizer_scale = decoder.q_scale_type ? non_linear_quantizer_scale[qsc] : static_cast<int>(qsc << 1);
	}
}

// The IPU registers repeat every 0x100 bytes across the 0x10002000 page.
static bool IsIpuReg(uint32_t addr) { return (addr & ~0xFFFu) == 0x10002000u; }
static uint32_t IpuReg(uint32_t addr) { return 0x10002000u | (addr & 0xFFu); }
static bool IsFifoPort(uint32_t addr) { return (addr & ~0x1Fu) == 0x10007000u; }
static int DmaChannel(uint32_t addr)
{
	if ((addr & ~0xFFu) == 0x1000B000u)
		return 3;
	if ((addr & ~0xFFu) == 0x1000B400u)
		return 4;
	return -1;
}

bool kzipuHandlesAddress(uint32_t addr)
{
	return IsIpuReg(addr) || IsFifoPort(addr) || DmaChannel(addr) >= 0;
}

void kzipuReset()
{
	std::memset(&eeHw[0x2000], 0, 0x100);
	std::memset(&eeHw[0xB000], 0, 0x100);
	std::memset(&eeHw[0xB400], 0, 0x100);
	std::memset(&eeHw[0xE000], 0, 0x100);
	dmacRegs.ctrl.DMAE = 1;
	kzipu::ResetEvents();
	ipuReset();
	kzipu::SetHostDrainsOutput(kzipu::HostDrainsOutput());
}

bool kzipuInit(const KzipuConfig& cfg)
{
	kzipu::g_host.user = cfg.user;
	kzipu::g_host.intc = cfg.intc;
	kzipu::g_host.dmacIrq = cfg.dmacIrq;
	kzipu::g_host.dmaPtr = cfg.dmaPtr;
	kzipu::g_host.log = cfg.log;
	std::memset(eeHw, 0, sizeof(eeHw));
	std::memset(&_cpuRegistersPack, 0, sizeof(_cpuRegistersPack));
	kzipu::SetDmacEnabled(true);
	kzipu::SetHostDrainsOutput(cfg.hostDrainsOutput);
	s_mapIdecQsc = cfg.mapIdecQsc;
	kzipuReset();
	s_open = true;
	return true;
}

void kzipuShutdown()
{
	s_open = false;
	kzipu::g_host = {};
}

// ---- registers -----------------------------------------------------------------------------------------------------------
uint32_t kzipuReadReg32(uint32_t addr)
{
	if (IsIpuReg(addr))
		return ipuRead32(IpuReg(addr));

	const int ch = DmaChannel(addr);
	if (ch >= 0)
	{
		if (ch == 3 && kzipu::HostDrainsOutput())
			return 0;
		const DMACh& reg = (ch == 3) ? ipu0ch : ipu1ch;
		switch (addr & 0xF0)
		{
			case 0x00: return reg.chcr._u32;
			case 0x10: return reg.madr;
			case 0x20: return reg.qwc & 0xFFFF;
			case 0x30: return reg.tadr;
			case 0x40: return reg.asr0;
			case 0x50: return reg.asr1;
			case 0x80: return reg.sadr;
			default: return 0;
		}
	}
	return 0;
}

uint64_t kzipuReadReg64(uint32_t addr)
{
	if (IsIpuReg(addr))
		return ipuRead64(IpuReg(addr));
	return kzipuReadReg32(addr);
}

void kzipuWriteReg32(uint32_t addr, uint32_t value)
{
	if (IsIpuReg(addr))
	{
		// ipuWrite32 handles IPU_CMD and IPU_CTRL; PCSX2 writes anything else straight into the register block.
		addr = IpuReg(addr);
		if (ipuWrite32(addr, value))
			psHu32(addr & 0xFFFF) = value;
		else if ((addr & 0xFF) == 0x00)
			AfterCmdWrite(value);
		return;
	}

	const int ch = DmaChannel(addr);
	if (ch < 0 || (ch == 3 && kzipu::HostDrainsOutput()))
		return;
	DMACh& reg = (ch == 3) ? ipu0ch : ipu1ch;
	switch (addr & 0xF0)
	{
		case 0x00: kzipu::DmaExec(ch, value); break;
		case 0x10: reg.madr = value; break;
		case 0x20: reg.qwc = static_cast<u16>(value); break;
		case 0x30: reg.tadr = value; break;
		case 0x40: reg.asr0 = value; break;
		case 0x50: reg.asr1 = value; break;
		case 0x80: reg.sadr = value; break;
		default: break;
	}
}

void kzipuWriteReg64(uint32_t addr, uint64_t value)
{
	if (IsIpuReg(addr))
	{
		addr = IpuReg(addr);
		if (ipuWrite64(addr, value))
			psHu64(addr & 0xFFFF) = value;
		else if ((addr & 0xFF) == 0x00)
			AfterCmdWrite(static_cast<uint32_t>(value));
		return;
	}
	kzipuWriteReg32(addr, static_cast<uint32_t>(value));
}

// ---- FIFOs ----------------------------------------------------------------------------------------------------------------
uint32_t kzipuWriteInFifo(const void* qwords, uint32_t count)
{
	const u8* p = static_cast<const u8*>(qwords);
	uint32_t n = 0;
	for (; n < count; n++)
	{
		if (g_BP.IFC >= 8)
			break;
		alignas(16) mem128_t q;
		std::memcpy(&q, p + 16 * n, 16);
		WriteFIFO_IPUin(&q);  // wakes a command that waits on input (IPU_INT_PROCESS)
	}
	return n;
}

uint32_t kzipuReadOutFifo(void* qwords, uint32_t count)
{
	u8* p = static_cast<u8*>(qwords);
	uint32_t n = 0;
	for (; n < count && ipuRegs.ctrl.OFC > 0; n++)
	{
		alignas(16) mem128_t q;
		ReadFIFO_IPUout(&q);
		std::memcpy(p + 16 * n, &q, 16);
	}
	// PCSX2 only wakes a decoder that waits on output from its IPU_FROM DMA (IPU0dma); do the same for direct reads.
	if (n && ipuRegs.ctrl.BUSY && IPUCoreStatus.WaitingOnIPUFrom)
	{
		IPUCoreStatus.WaitingOnIPUFrom = false;
		IPU_INT_PROCESS(n * BIAS);
	}
	return n;
}

// ---- scheduling -----------------------------------------------------------------------------------------------------------
void kzipuSetDmacEnabled(bool enabled)
{
	dmacRegs.ctrl.DMAE = enabled ? 1 : 0;
	kzipu::SetDmacEnabled(enabled);
}

uint32_t kzipuRun(uint32_t eeCycles)
{
	return kzipu::RunEvents(kzipu::Now() + eeCycles, 0xFFFFFFFFu, false);
}

uint32_t kzipuStep(uint32_t maxEvents)
{
	return kzipu::RunEvents(0, maxEvents, true);
}

KzipuStatus kzipuGetStatus()
{
	KzipuStatus s = {};
	s.busy = ipuRegs.ctrl.BUSY != 0;
	s.command = ipu_cmd.CMD;
	s.inFifoQwc = g_BP.IFC;
	s.outFifoQwc = ipuRegs.ctrl.OFC;
	s.bitPointer = g_BP.BP;
	s.waitingForInput = s.busy && IPUCoreStatus.WaitingOnIPUTo;
	s.waitingForOutput = s.busy && IPUCoreStatus.WaitingOnIPUFrom;
	s.inputRequest = IPUCoreStatus.DataRequested && g_BP.IFC < 8;
	s.outputRequest = ipuRegs.ctrl.OFC > 0;
	s.toIpuActive = ipu1ch.chcr.STR != 0;
	s.fromIpuActive = !kzipu::HostDrainsOutput() && ipu0ch.chcr.STR != 0;
	s.pendingEvents = kzipu::PendingEventMask();
	s.cycle = kzipu::Now();
	s.intcCount = kzipu::IntcCount();
	s.fromIpuEnds = kzipu::DmacIrqCount(3);
	s.toIpuEnds = kzipu::DmacIrqCount(4);
	return s;
}
