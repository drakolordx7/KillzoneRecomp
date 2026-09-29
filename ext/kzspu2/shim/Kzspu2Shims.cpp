// kzspu2 shim: the IOP state PCSX2's SPU2 code references, reduced to what the SPU2 needs: the IOP cycle counter,
// the SPU2 event counter (psxCounters[6]), the IOP DMA registers of channels 4/7, IOP RAM access, and the three
// interrupt lines (SPU2 IRQ, DMA4 end, DMA7 end), which go to the host's callbacks.
// SPDX-License-Identifier: GPL-3.0+

#include "kzspu2_prefix.h"

#include <cstdio>
#include <vector>

// ---- emulator globals (all renamed by kzspu2_rename.h) ------------------------------------------------------------------
alignas(16) psxRegisters psxRegs;
psxCounter psxCounters[NUM_COUNTERS];
s32 psxNextDeltaCounter = 0;
u64 psxNextStartCounter = 0;
alignas(16) u8 iopHw[0x10000];

const Kzspu2Console kzspu2_Console;

namespace kzspu2
{
	HostHooks g_host;
	uint32_t g_irqCount = 0;
	uint32_t g_dmaIrqCount[2] = {};

	static uint8_t* s_iopRam = nullptr;
	static uint32_t s_iopRamSize = 0;

	void SetIopRam(uint8_t* base, uint32_t size)
	{
		s_iopRam = base;
		s_iopRamSize = size;
	}
} // namespace kzspu2

u8* iopPhysMem(u32 addr)
{
	// Only reached by PCSX2's "DMA pointer lost after a savestate load" fallbacks. Without a registered IOP RAM the
	// transfer reads zeros instead of crashing.
	static std::vector<u8> s_zero(8u * 1024u * 1024u);
	if (kzspu2::s_iopRam && kzspu2::s_iopRamSize)
		return kzspu2::s_iopRam + (addr % kzspu2::s_iopRamSize);
	return s_zero.data() + (addr & (8u * 1024u * 1024u - 1u));
}

// ---- interrupt lines (PCSX2's IopIrq.cpp / IopDma.cpp, with the IOP side replaced by host callbacks) ------------------------
void spu2Irq()
{
	++kzspu2::g_irqCount;
	if (kzspu2::g_host.irq)
		kzspu2::g_host.irq(kzspu2::g_host.user);
}

void spu2DMA4Irq()
{
	SPU2interruptDMA4();
	if (HW_DMA4_CHCR & 0x01000000)
	{
		HW_DMA4_CHCR &= ~0x01000000;
		++kzspu2::g_dmaIrqCount[0];
		if (kzspu2::g_host.dmaComplete)
			kzspu2::g_host.dmaComplete(kzspu2::g_host.user, 0);
	}
}

void spu2DMA7Irq()
{
	SPU2interruptDMA7();
	if (HW_DMA7_CHCR & 0x01000000)
	{
		HW_DMA7_CHCR &= ~0x01000000;
		++kzspu2::g_dmaIrqCount[1];
		if (kzspu2::g_host.dmaComplete)
			kzspu2::g_host.dmaComplete(kzspu2::g_host.user, 1);
	}
}

// ---- log --------------------------------------------------------------------------------------------------------------------
static void LogV(int level, const char* fmt, va_list ap)
{
	char buf[1024];
	std::vsnprintf(buf, sizeof(buf), fmt, ap);
	if (kzspu2::g_host.log)
		kzspu2::g_host.log(kzspu2::g_host.user, level, buf);
}

void Kzspu2Console::WriteLn(const std::string& s) const
{
	if (kzspu2::g_host.log)
		kzspu2::g_host.log(kzspu2::g_host.user, 0, s.c_str());
}

void Kzspu2Console::WriteLn(const char* fmt, ...) const
{
	va_list ap;
	va_start(ap, fmt);
	LogV(0, fmt, ap);
	va_end(ap);
}

void Kzspu2Console::Warning(const char* fmt, ...) const
{
	va_list ap;
	va_start(ap, fmt);
	LogV(1, fmt, ap);
	va_end(ap);
}

void Kzspu2Console::Error(const char* fmt, ...) const
{
	va_list ap;
	va_start(ap, fmt);
	LogV(2, fmt, ap);
	va_end(ap);
}

void pxOnAssertFail(const char* file, int line, const char* func, const char* msg)
{
	kzspu2_Console.Error("kzspu2 assertion failed: %s (%s:%d, %s)", msg ? msg : "", file ? file : "", line, func ? func : "");
}
