// kzvu: PCSX2's x86/microVU.cpp (unmodified, pulled in with #include) plus read-only access to microVU1's program and
// code-cache state, for KZ_VU_STATS (see kz_vu.cpp) and the recompile diagnostics in kzvu.h. microVU.cpp is compiled
// as part of this translation unit instead of directly, because microVU1 and the microProgram structures are only
// visible here.
// SPDX-License-Identifier: GPL-3.0+

#include "kzvu_prefix.h"

#include "x86/microVU.cpp"

#include "kzvu.h"

void kzvuVu1CodeStats(KzvuVu1CodeStats* out)
{
	const microVU& m = microVU1;
	out->codeBytes = static_cast<uint64_t>(m.prog.x86ptr - m.prog.x86start);
	out->cacheBytes = static_cast<uint64_t>(m.prog.x86end - m.prog.x86start);
	out->programsCreated = static_cast<uint32_t>(m.prog.total);
	uint32_t entries = 0;
	for (u32 i = 0; i < m.progSize / 2; i++)
		if (m.prog.prog[i])
			entries += static_cast<uint32_t>(m.prog.prog[i]->size());
	out->programsCached = entries;
}

// The newest cached program for `startPc` (bytes): index 0 is the most recently used one. Compares the current VU1 micro
// memory against it over the ranges the program was compiled for; returns the first differing 32-bit word.
bool kzvuVu1DiffCachedProgram(uint32_t startPcBytes, uint32_t which, KzvuVu1ProgDiff* out)
{
	const u32 slot = startPcBytes / 8;
	if (slot >= microVU1.progSize / 2 || !microVU1.prog.prog[slot])
		return false;
	const auto& list = *microVU1.prog.prog[slot];
	if (which >= list.size())
		return false;
	const microProgram& p = *list[which];
	out->programs = static_cast<uint32_t>(list.size());
	out->ranges = static_cast<uint32_t>(p.ranges->size());
	const u32* cur = reinterpret_cast<const u32*>(microVU1.regs().Micro);
	for (const auto& r : *p.ranges)
	{
		for (s32 b = r.start; b < r.end; b += 4)
		{
			const u32 w = static_cast<u32>(b) / 4;
			if (p.data[w] != cur[w])
			{
				out->wordIndex = w;
				out->oldWord = p.data[w];
				out->newWord = cur[w];
				return true;
			}
		}
	}
	out->wordIndex = ~0u;
	return true; // same over all ranges
}
