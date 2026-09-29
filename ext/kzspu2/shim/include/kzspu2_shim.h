// kzspu2: the shim objects the PCSX2 SPU2 code is redirected to, plus the glue between src/kzspu2.cpp and
// shim/Kzspu2Shims.cpp.
// SPDX-License-Identifier: GPL-3.0+
#pragma once

#include <cstdarg>
#include <cstdint>
#include <string>

// ---- Console / DevCon: forwarded to the host log callback ----------------------------------------------------------------
struct Kzspu2Console
{
	void WriteLn() const {}
	void WriteLn(const std::string& s) const;
	void WriteLn(const char* fmt, ...) const;
	void Warning(const char* fmt, ...) const;
	void Error(const char* fmt, ...) const;
};
extern const Kzspu2Console kzspu2_Console;

namespace kzspu2
{
	struct HostHooks
	{
		void* user = nullptr;
		void (*irq)(void* user) = nullptr;
		void (*dmaComplete)(void* user, int core) = nullptr;
		void (*log)(void* user, int level, const char* msg) = nullptr;
	};
	extern HostHooks g_host;

	// IOP RAM as seen by iopPhysMem (only used by PCSX2's savestate fallback paths; normal DMAs pass pointers).
	void SetIopRam(uint8_t* base, uint32_t size);

	// Counters for the API's status query.
	extern uint32_t g_irqCount;
	extern uint32_t g_dmaIrqCount[2];
} // namespace kzspu2
