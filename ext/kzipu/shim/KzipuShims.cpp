// kzipu shim: the emulator state and subsystems PCSX2's IPU code references, reduced to what the IPU needs:
// the EE hardware register block, the three IPU events of the EE event scheduler, the INTC/DMAC interrupt lines,
// DMA address translation and DMA chain tag handling (IPU_TO only supports source chain mode), plus the few
// pcsx2/common functions the IPU sources call.
// SPDX-License-Identifier: GPL-3.0+

#include "kzipu_prefix.h"

#include <cstdio>
#include <cstdlib>

// ---- emulator globals (all renamed by kzipu_rename.h) --------------------------------------------------------------------
// eeHw is the 64 KB EE hardware register page (0x10000000). kzipu uses 0x2000 (IPU registers), 0xB000/0xB400 (DMA ch3/4)
// and 0xE000 (DMAC control; only D_CTRL.STS and D_STAT copies live here, the host owns the real ones).
alignas(__pagealignsize) u8 eeHw[Ps2MemSize::Hardware];
alignas(16) cpuRegistersPack _cpuRegistersPack;   // cpuRegs.cycle / interrupt / sCycle / eCycle / dmastall

const KzipuConsole kzipu_Console;

namespace kzipu
{
	HostHooks g_host;
}

// ---- log -----------------------------------------------------------------------------------------------------------------
static void LogV(int level, const char* fmt, va_list ap)
{
	char buf[1024];
	std::vsnprintf(buf, sizeof(buf), fmt, ap);
	if (kzipu::g_host.log)
		kzipu::g_host.log(kzipu::g_host.user, level, buf);
}

void KzipuConsole::WriteLn(const std::string& s) const
{
	if (kzipu::g_host.log)
		kzipu::g_host.log(kzipu::g_host.user, 0, s.c_str());
}

void KzipuConsole::WriteLn(const char* fmt, ...) const
{
	va_list ap;
	va_start(ap, fmt);
	LogV(0, fmt, ap);
	va_end(ap);
}

void KzipuConsole::Warning(const char* fmt, ...) const
{
	va_list ap;
	va_start(ap, fmt);
	LogV(1, fmt, ap);
	va_end(ap);
}

void KzipuConsole::Error(const char* fmt, ...) const
{
	va_list ap;
	va_start(ap, fmt);
	LogV(2, fmt, ap);
	va_end(ap);
}

// ---- pcsx2/common functions the IPU code references (renamed; kzipu does not compile pcsx2/common) -----------------------
void pxOnAssertFail(const char* file, int line, const char* func, const char* msg)
{
	kzipu_Console.Error("kzipu assertion failed: %s (%s:%d, %s)", msg ? msg : "", file ? file : "", line, func ? func : "");
}

std::string StringUtil::StdStringFromFormat(const char* format, ...)
{
	va_list ap;
	va_start(ap, format);
	char buf[512];
	std::vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);
	return buf;
}

bool SaveStateBase::FreezeTag(const char* src)
{
	// Savestates are not supported by kzipu; ipuFreeze()/ipuDmaFreeze() are compiled but never called.
	return false;
}

// ---- EE event scheduler (IPU events only) ------------------------------------------------------------------------------
// PCSX2 runs DMA and IPU work as EE "interrupts": CPU_INT(n, cycles) sets bit n of cpuRegs.interrupt and the EE calls the
// handler once cpuRegs.cycle has advanced by `cycles`. kzipu keeps that model with its own virtual cycle counter, which
// the host advances with kzipuRun(); kzipuStep() fires pending events without waiting for their time to come.
static bool s_in_event = false;      // PCSX2's eeRunInterruptScan != INT_NOT_RUNNING
static bool s_dmac_enabled = true;   // D_CTRL.DMAE && !D_ENABLER.CPND
static u32 s_queued_dma = 0;         // channels started while the DMAC was disabled (PCSX2's QueuedDMA)
static bool s_host_drain = false;   // KzipuConfig::hostDrainsOutput
static u32 s_intc_count = 0;
static u32 s_dmac_count[16] = {};

static constexpr int kEvents[] = {DMAC_FROM_IPU, DMAC_TO_IPU, IPU_PROCESS};

void CPU_INT(EE_EventType n, s32 ecycle)
{
	// Same as PCSX2: an event requested with a tiny delay from inside an event handler runs in the same scan.
	if (ecycle < 4 && !(cpuRegs.dmastall & (1 << n)) && s_in_event)
	{
		cpuRegs.interrupt |= 1 << n;
		cpuRegs.sCycle[n] = cpuRegs.cycle;
		cpuRegs.eCycle[n] = 0;
		return;
	}
	cpuRegs.interrupt |= 1 << n;
	cpuRegs.sCycle[n] = cpuRegs.cycle;
	cpuRegs.eCycle[n] = ecycle;
}

void CPU_SET_DMASTALL(EE_EventType n, bool set)
{
	if (set)
		cpuRegs.dmastall |= 1 << n;
	else
		cpuRegs.dmastall &= ~(1 << n);
}

void cpuClearInt(uint n)
{
	cpuRegs.interrupt &= ~(1 << n);
	cpuRegs.dmastall &= ~(1 << n);
}

void kzipu::ResetEvents()
{
	cpuRegs.interrupt = 0;
	cpuRegs.dmastall = 0;
	for (int n : kEvents)
	{
		cpuRegs.sCycle[n] = 0;
		cpuRegs.eCycle[n] = 0;
	}
	s_queued_dma = 0;
}

uint64_t kzipu::Now()
{
	return cpuRegs.cycle;
}

uint32_t kzipu::PendingEventMask()
{
	u32 m = 0;
	for (int n : kEvents)
		m |= cpuRegs.interrupt & (1u << n);
	return m;
}

bool kzipu::AnyEventPending()
{
	return PendingEventMask() != 0;
}

uint32_t kzipu::RunEvents(uint64_t targetCycle, uint32_t maxEvents, bool fastForward)
{
	u32 count = 0;
	while (count < maxEvents && s_dmac_enabled)
	{
		int best = -1;
		u64 best_due = 0;
		for (int n : kEvents) // PCSX2's TESTINT order breaks ties: IPU_FROM, IPU_TO, IPU_PROCESS
		{
			if (!(cpuRegs.interrupt & (1u << n)))
				continue;
			const u64 due = cpuRegs.sCycle[n] + cpuRegs.eCycle[n];
			if (best < 0 || due < best_due)
			{
				best = n;
				best_due = due;
			}
		}
		if (best < 0)
			break;
		if (!fastForward && best_due > targetCycle)
			break;
		if (best_due > cpuRegs.cycle)
			cpuRegs.cycle = best_due;

		cpuClearInt(best);
		s_in_event = true;
		switch (best)
		{
			case DMAC_FROM_IPU:
				if (s_host_drain)
					kzipu::ArmHostDrain();
				else
					ipu0Interrupt();
				break;
			case DMAC_TO_IPU: ipu1Interrupt(); break;
			case IPU_PROCESS: ipuCMDProcess(); break;
		}
		s_in_event = false;
		count++;
	}
	if (!fastForward && targetCycle > cpuRegs.cycle)
		cpuRegs.cycle = targetCycle;
	return count;
}

// ---- interrupt lines ------------------------------------------------------------------------------------------------------
void hwIntcIrq(int n)
{
	psHu32(INTC_STAT) |= 1 << n;
	if (n == INTC_IPU)
	{
		s_intc_count++;
		if (kzipu::g_host.intc)
			kzipu::g_host.intc(kzipu::g_host.user);
	}
}

void hwDmacIrq(int n)
{
	psHu32(DMAC_STAT) |= 1 << n;
	if (n >= 0 && n < 16)
		s_dmac_count[n]++;
	if (kzipu::g_host.dmacIrq)
		kzipu::g_host.dmacIrq(kzipu::g_host.user, n);
}

uint32_t kzipu::IntcCount()
{
	return s_intc_count;
}

uint32_t kzipu::DmacIrqCount(int channel)
{
	return (channel >= 0 && channel < 16) ? s_dmac_count[channel] : 0;
}

// ---- DMA memory and chain tags ------------------------------------------------------------------------------------------
__fi void throwBusError(const char* s)
{
	kzipu_Console.Error("%s BUSERR", s);
	dmacRegs.stat.BEIS = true;
	if (kzipu::g_host.dmacIrq)
		kzipu::g_host.dmacIrq(kzipu::g_host.user, DMAC_BUS_ERROR);
}

__fi void setDmacStat(u32 num)
{
	dmacRegs.stat.set_flags(1 << num);
}

// The IPU DMA code asks for one address at a time and then touches at most 8 qwords from it (the FIFO depth), or 1 for a
// tag. The byte count handed to the host is exact, so the host only has to guarantee that many bytes.
tDMA_TAG* dmaGetAddr(u32 addr, bool write)
{
	if (!kzipu::g_host.dmaPtr)
		return nullptr;
	u32 bytes = 16;
	if (addr == ipu0ch.madr && write)
		bytes = 16 * std::clamp<u32>(std::min<u32>(ipu0ch.qwc, 8), 1, 8);
	else if (addr == ipu1ch.madr && !write && ipu1ch.qwc)
		bytes = 16 * std::clamp<u32>(std::min<u32>(ipu1ch.qwc, 8), 1, 8);
	return reinterpret_cast<tDMA_TAG*>(kzipu::g_host.dmaPtr(kzipu::g_host.user, addr, bytes, write));
}

bool DMACh::transfer(const char* s, tDMA_TAG* ptag)
{
	if (ptag == nullptr)
	{
		throwBusError(s);
		return false;
	}
	chcrTransfer(ptag);
	qwcTransfer(ptag);
	return true;
}

void hwDmacSrcTadrInc(DMACh& dma)
{
	// Don't touch it if in normal/interleave mode.
	if (dma.chcr.STR == 0)
		return;
	if (dma.chcr.MOD != 1)
		return;

	const u16 tagid = (dma.chcr.TAG >> 12) & 0x7;
	if (tagid == TAG_CNT)
		dma.tadr = dma.madr;
}

bool hwDmacSrcChain(DMACh& dma, int id)
{
	u32 temp;

	switch (id)
	{
		case TAG_REFE: // Refe - Transfer Packet According to ADDR field
			dma.tadr += 16;
			return true; // End the transfer.

		case TAG_CNT: // CNT - Transfer QWC following the tag.
			dma.madr = dma.tadr + 16;
			dma.tadr = dma.madr;
			return false;

		case TAG_NEXT: // Next - Transfer QWC following tag. TADR = ADDR
			temp = dma.madr;
			dma.madr = dma.tadr + 16;
			dma.tadr = temp;
			return false;

		case TAG_REF: // Ref - Transfer QWC from ADDR field
		case TAG_REFS: // Refs - Transfer QWC from ADDR field (Stall Control)
			dma.tadr += 16;
			return false;

		case TAG_END: // End - Transfer QWC following the tag
			dma.madr = dma.tadr + 16;
			return true; // Don't increment tadr (Soul Calibur II and III)

		default: // CALL/RET are not supported by the IPU_TO channel; an undefined tag ends the DMA
			return true;
	}
}

// ---- DMAC channel start (PCSX2's DmaExec, IPU channels only) -------------------------------------------------------------
static void StartChannel(int channel)
{
	if (channel == 3)
		dmaIPU0();
	else
		dmaIPU1();
}

void kzipu::DmaExec(int channel, uint32_t value)
{
	DMACh& reg = (channel == 3) ? ipu0ch : ipu1ch;
	const tDMA_CHCR chcr(value);

	// Fields other than STR can only be written while the DMA is stopped. Writing STR=0 force-stops it.
	if (reg.chcr.STR)
	{
		if (chcr.STR == 0)
		{
			reg.chcr.STR = 0;
			cpuClearInt(channel);
			s_queued_dma &= ~(1u << channel);
		}
		return;
	}

	reg.chcr.set(value);

	// MOD=3 does not exist; PCSX2 treats it as chain.
	if (reg.chcr.MOD == 0x3)
		reg.chcr.MOD = 0x1;

	// A NORMAL transfer started with QWC=0 transfers 0x10000 qwords on hardware.
	if (reg.chcr.STR && !reg.chcr.MOD && reg.qwc == 0)
		reg.qwc = 0x10000;

	if (reg.chcr.STR && s_dmac_enabled)
		StartChannel(channel);
	else if (reg.chcr.STR)
		s_queued_dma |= 1u << channel;
}

void kzipu::SetDmacEnabled(bool enabled)
{
	const bool was = s_dmac_enabled;
	s_dmac_enabled = enabled;
	if (enabled && !was)
	{
		for (int ch : {3, 4})
		{
			if (!(s_queued_dma & (1u << ch)))
				continue;
			s_queued_dma &= ~(1u << ch);
			const DMACh& reg = (ch == 3) ? ipu0ch : ipu1ch;
			if (reg.chcr.STR)
				StartChannel(ch);
		}
	}
}

void kzipu::SetHostDrainsOutput(bool on)
{
	s_host_drain = on;
	if (on)
		ArmHostDrain();
}

bool kzipu::HostDrainsOutput()
{
	return s_host_drain;
}

void kzipu::ArmHostDrain()
{
	ipu0ch.chcr.STR = 1;
	ipu0ch.chcr.MOD = NORMAL_MODE;
	ipu0ch.qwc = 0xFFFF;
}

bool kzipu::DmacEnabled()
{
	return s_dmac_enabled;
}
