// Test-only glue for linking PS2Recomp's VU1Interpreter (ps2xRuntime/src/lib/vu) without the rest of ps2xRuntime.
// VU1Interpreter needs a GS& and a PS2Memory*: the memory object is only used for its VU1 code pointer/generation
// (decoded-instruction cache) and for submitGifPacket (XGKICK), which this file defines to capture packets.
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <vector>

#define _ALLOW_KEYWORD_MACROS
#define private public
#include "runtime/ps2_memory.h"
#undef private
#include "runtime/gs/gs_frontend.h"

#include "ps2recomp_stubs.h"

std::vector<std::vector<uint8_t>> g_recompGifPackets;

PS2Memory::PS2Memory() {}
PS2Memory::~PS2Memory() {}

void PS2Memory::submitGifPacket(GifPathId pathId, const uint8_t* data, uint32_t sizeBytes, bool, bool)
{
	g_recompGifPackets.emplace_back(data, data + sizeBytes);
}

void GS::processGIFPacket(const uint8_t*, uint32_t)
{
	std::abort(); // never reached: VU1Interpreter prefers PS2Memory::submitGifPacket when it has a memory object
}

PS2Memory* recompCreateMemory(uint8_t* vu1Code, uint8_t* vu1Data)
{
	auto* m = new PS2Memory();
	m->m_vu1Code = vu1Code;
	m->m_vu1Data = vu1Data;
	return m;
}

void recompMarkCodeModified(PS2Memory* m)
{
	m->markVU1CodeModified();
}
