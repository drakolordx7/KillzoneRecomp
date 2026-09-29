// kzipu: the shim objects the PCSX2 IPU code is redirected to (see kzipu_prefix.h), plus the internal glue between
// src/kzipu.cpp and shim/KzipuShims.cpp.
// SPDX-License-Identifier: GPL-3.0+
#pragma once

#include <cstdarg>
#include <cstdint>
#include <string>

// ---- Console / DevCon: the calls the IPU code makes, forwarded to the host log callback ----------------------------------
struct KzipuConsole
{
	void WriteLn() const {}
	void WriteLn(const std::string& s) const;
	void WriteLn(const char* fmt, ...) const;
	void Warning(const char* fmt, ...) const;
	void Error(const char* fmt, ...) const;
};
extern const KzipuConsole kzipu_Console;

// ---- glue between the API (src/kzipu.cpp) and the shims -----------------------------------------------------------------
namespace kzipu
{
	struct HostHooks
	{
		void* user = nullptr;
		void (*intc)(void* user) = nullptr;                                     // INTC_IPU raised
		void (*dmacIrq)(void* user, int channel) = nullptr;                     // D_STAT CIS bit raised
		uint8_t* (*dmaPtr)(void* user, uint32_t addr, uint32_t bytes, bool write) = nullptr;
		void (*log)(void* user, int level, const char* msg) = nullptr;          // 0 info, 1 warning, 2 error
	};
	extern HostHooks g_host;

	// Event scheduler (PCSX2's cpuRegs.interrupt / sCycle / eCycle, reduced to the three IPU events).
	void ResetEvents();
	uint32_t RunEvents(uint64_t targetCycle, uint32_t maxEvents, bool fastForward);
	uint64_t Now();
	bool AnyEventPending();
	uint32_t PendingEventMask();

	// DMAC gating: D_CTRL.DMAE and D_ENABLER.CPND (suspend). While disabled no DMA starts or advances.
	void SetDmacEnabled(bool enabled);
	bool DmacEnabled();

	// hostDrainsOutput mode: ch3 is held "armed" (STR=1, QWC=0xFFFF) so the decoder never waits for it, and the IPU_FROM
	// DMA event is dropped instead of moving data; the host drains the output FIFO itself.
	void SetHostDrainsOutput(bool on);
	bool HostDrainsOutput();
	void ArmHostDrain();

	// Starts a channel after a CHCR write (PCSX2's DmaExec, IPU channels only).
	void DmaExec(int channel, uint32_t chcr);

	// Counters for the API's status query.
	uint32_t IntcCount();
	uint32_t DmacIrqCount(int channel);
} // namespace kzipu
