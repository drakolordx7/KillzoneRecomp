// kzvu: PCSX2's x86/microVU.cpp (unmodified, pulled in with #include) plus read-only access to microVU1's program and
// code-cache state, for KZ_VU_STATS (see kz_vu.cpp) and the recompile diagnostics in kzvu.h. microVU.cpp is compiled
// as part of this translation unit instead of directly, because microVU1 and the microProgram structures are only
// visible here. It also holds the VU1 code-state memo (kzvuVu1CodeChanged, at the end of the file).
// SPDX-License-Identifier: GPL-3.0+

#include "kzvu_prefix.h"

#include <cstdlib>
#include <intrin.h>
#include <memory>
#include <unordered_map>
#include <vector>
#include "x86/microVU.h"

// Diagnostic build (-DKZVU_SEARCH_STATS): every memcmp in microVU's sources, i.e. its program lookup comparing the compiled
// ranges of the candidate programs with the micro memory, is counted and timed (KZ_VU_STATS prints it). Measured this way:
// ~1.5 M calls/s of 17 bytes on average in Killzone gameplay at ~270 TSC cycles each (a cache miss on the candidate's copy
// of the code; an inline SSE compare instead of the CRT memcmp changed nothing), which the code-state memo below avoids.
#ifdef KZVU_SEARCH_STATS
static uint64_t s_cmpCalls, s_cmpBytes, s_cmpCycles;
static __forceinline int kzvuCountedMemcmp(const void* a, const void* b, size_t n)
{
	const uint64_t t0 = __rdtsc();
	const int r = std::memcmp(a, b, n);
	s_cmpCycles += __rdtsc() - t0;
	++s_cmpCalls;
	s_cmpBytes += n;
	return r;
}
#define memcmp kzvuCountedMemcmp
#endif

#include "x86/microVU.cpp"

#ifdef KZVU_SEARCH_STATS
#undef memcmp
#endif

#include "kzvu.h"

void kzvuVu1MemoStats(KzvuVu1CodeStats* out);
void kzvuVu1CodeCounters(uint64_t* codeBytes, uint32_t* programsCreated)
{
	*codeBytes = static_cast<uint64_t>(microVU1.prog.x86ptr - microVU1.prog.x86start);
	*programsCreated = static_cast<uint32_t>(microVU1.prog.total);
}

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
#ifdef KZVU_SEARCH_STATS
	out->cmpCalls = s_cmpCalls;
	out->cmpBytes = s_cmpBytes;
	out->cmpCycles = s_cmpCycles;
#else
	out->cmpCalls = out->cmpBytes = out->cmpCycles = 0;
#endif
	kzvuVu1MemoStats(out);
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

// ---- code-state memo (KZVU_STATE_MEMO, default on; 0 = microVU's own invalidation) -------------------------------------
// Killzone re-uploads VU1 microcode ~4000 times a second, alternating between a few dozen distinct micro-memory contents.
// microVU invalidates its whole current-program table (prog.quick) on every change, so each program entry (MSCAL, and
// every JR/JALR, which microVU treats as a new program start) afterwards searches its program list again, comparing every
// compiled range of every candidate program with the micro memory (~1.5 M memcmp calls per second, ~10-15 % of the VU1
// thread). Here the table is remembered per micro-memory content: when the code changes, the entries of the table in use
// are saved with the content they were validated for, and if the new content was seen before its saved entries are put
// back (they are the programs the search would have found). An entry stays valid while nothing was compiled since it was
// saved: compiling a block can add ranges to a program (and copy micro memory into it), so a saved table is only reused if
// microVU's code pointer is unchanged; otherwise it is dropped and the entries are searched again, as before.
// Everything else microVU's mVUclear does (pipeline state cleared, "cleared" flag) is done here too.
namespace
{
	struct MemoKey
	{
		u64 a, b;
		bool operator==(const MemoKey& o) const { return a == o.a && b == o.b; }
	};
	struct MemoKeyHash
	{
		size_t operator()(const MemoKey& k) const { return static_cast<size_t>(k.a ^ (k.b * 0x9E3779B97F4A7C15ull)); }
	};

	struct MemoState
	{
		std::vector<std::pair<u16, microProgram*>> entries; // (start slot = start PC / 8, program that matched this content), sorted
		const u8* epoch = nullptr;                          // microVU1 code pointer when entries was last brought up to date
		std::vector<u8> code;                               // the content itself (KZVU_STATE_MEMO_VERIFY only)
	};

	struct Memo
	{
		bool enabled = true;
		bool verify = false;
		std::unordered_map<MemoKey, std::unique_ptr<MemoState>, MemoKeyHash> states;
		MemoState* cur = nullptr;
		const u8* lastCode = nullptr;
		int lastTotal = 0;
		uint64_t switches = 0, hits = 0, stale = 0, created = 0, restored = 0, verifyBad = 0, cycles = 0;
		Memo()
		{
			const char* v = std::getenv("KZVU_STATE_MEMO");
			enabled = !(v && *v == '0');
			const char* w = std::getenv("KZVU_STATE_MEMO_VERIFY");
			verify = w && *w && *w != '0';
		}
	};
	Memo g_memo;
	constexpr size_t kMemoMaxStates = 65536;

	// 128-bit content key of the 16 KB micro memory: eight AES-round accumulators over 16-byte blocks (each block position
	// feeds one lane, every lane has its own start value, the lanes are folded with extra rounds). States are only compared
	// by this key, without keeping the content: not a cryptographic hash, but a chance of ~2^-128 per pair of different
	// contents for non-adversarial data (KZVU_STATE_MEMO_VERIFY=1 also compares the content).
	MemoKey memoHash(const u8* p)
	{
		const __m128i* q = reinterpret_cast<const __m128i*>(p);
		__m128i a0 = _mm_set_epi64x(0x243F6A8885A308D3ll, 0x13198A2E03707344ll);
		__m128i a1 = _mm_set_epi64x(0xA4093822299F31D0ll, 0x082EFA98EC4E6C89ll);
		__m128i a2 = _mm_set_epi64x(0x452821E638D01377ll, 0xBE5466CF34E90C6Cll);
		__m128i a3 = _mm_set_epi64x(0xC0AC29B7C97C50DDll, 0x3F84D5B5B5470917ll);
		__m128i a4 = _mm_set_epi64x(0x9216D5D98979FB1Bll, 0xD1310BA698DFB5ACll);
		__m128i a5 = _mm_set_epi64x(0x2FFD72DBD01ADFB7ll, 0xB8E1AFED6A267E96ll);
		__m128i a6 = _mm_set_epi64x(0xBA7C9045F12C7F99ll, 0x24A19947B3916CF7ll);
		__m128i a7 = _mm_set_epi64x(0x0801F2E2858EFC16ll, 0x636920D871574E69ll);
		for (size_t i = 0; i < 0x4000 / 16; i += 8)
		{
			a0 = _mm_aesenc_si128(a0, _mm_loadu_si128(q + i));
			a1 = _mm_aesenc_si128(a1, _mm_loadu_si128(q + i + 1));
			a2 = _mm_aesenc_si128(a2, _mm_loadu_si128(q + i + 2));
			a3 = _mm_aesenc_si128(a3, _mm_loadu_si128(q + i + 3));
			a4 = _mm_aesenc_si128(a4, _mm_loadu_si128(q + i + 4));
			a5 = _mm_aesenc_si128(a5, _mm_loadu_si128(q + i + 5));
			a6 = _mm_aesenc_si128(a6, _mm_loadu_si128(q + i + 6));
			a7 = _mm_aesenc_si128(a7, _mm_loadu_si128(q + i + 7));
		}
		__m128i x = _mm_aesenc_si128(_mm_aesenc_si128(a0, a1), _mm_aesenc_si128(a2, a3));
		__m128i y = _mm_aesenc_si128(_mm_aesenc_si128(a4, a5), _mm_aesenc_si128(a6, a7));
		x = _mm_aesenc_si128(_mm_aesenc_si128(x, y), a0);
		y = _mm_aesenc_si128(_mm_aesenc_si128(y, x), a4);
		const __m128i r = _mm_aesenc_si128(_mm_aesenc_si128(_mm_xor_si128(x, y), y), x);
		MemoKey k;
		k.a = static_cast<u64>(_mm_cvtsi128_si64(r));
		k.b = static_cast<u64>(_mm_extract_epi64(r, 1));
		return k;
	}

	// The ranges the program was compiled from equal `mem` (the program's own copy of them is prog.data).
	bool programMatches(const microProgram* p, const u8* mem)
	{
		for (const microRange& r : *p->ranges)
			if (r.start < 0 || r.end < r.start || std::memcmp(reinterpret_cast<const u8*>(p->data) + r.start, mem + r.start, r.end - r.start) != 0)
				return false;
		return true;
	}

	void memoFlush()
	{
		g_memo.states.clear();
		g_memo.cur = nullptr;
		g_memo.lastCode = nullptr;
		g_memo.lastTotal = 0;
	}
}

void kzvuVu1MemoFlush()
{
	memoFlush();
}

void kzvuVu1CodeChanged(uint32_t offset, uint32_t size)
{
	Memo& M = g_memo;
	if (!M.enabled)
	{
		CpuMicroVU1.Clear(offset, size);
		return;
	}
	const u64 tsc0 = __rdtsc();
	microVU& mVU = microVU1;
	const u8* codePtr = mVU.prog.x86ptr;
	// microVU freed all its programs (cache reset): the remembered tables point at freed memory.
	if (M.cur && (codePtr < M.lastCode || mVU.prog.total < M.lastTotal))
		memoFlush();
	++M.switches;
	const u8* micro = reinterpret_cast<const u8*>(mVU.regs().Micro);
	const u32 slots = mVU.progSize / 2;

	// 1+2. Save what the table in use holds (valid for the content it was in use for) and clear it (mVUclear: no program is
	// current). One pass over the 2048 slots; the entries are collected in a static buffer.
	static std::pair<u16, microProgram*> now[0x800];
	u32 nnow = 0;
	for (u32 i = 0; i < slots; i += 2)
	{
		// two 16-byte slots at once; the program pointer is the second qword of each
		const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&mVU.prog.quick[i]));
		const __m256i m = _mm256_set_epi64x(-1, 0, -1, 0);
		if (_mm256_testz_si256(v, m))
			continue;
		for (u32 j = i; j < i + 2; j++)
			if (microProgram* p = mVU.prog.quick[j].prog)
			{
				now[nnow++] = std::make_pair(static_cast<u16>(j), p);
				mVU.prog.quick[j].block = nullptr;
				mVU.prog.quick[j].prog = nullptr;
			}
	}
	if (MemoState* out = M.cur)
	{
		if (M.verify)
			for (u32 k = 0; k < nnow; k++)
				if (!programMatches(now[k].second, out->code.data()))
					++M.verifyBad;
		if (out->epoch != codePtr)
		{
			// Something was compiled since the older entries were saved: their ranges may have grown.
			out->entries.assign(now, now + nnow);
			out->epoch = codePtr;
		}
		else if (nnow)
		{
			// Same epoch: union (the entries of `now` win). Usually `now` is what the table already holds.
			auto& ent = out->entries;
			bool subset = true;
			size_t x = 0;
			for (u32 y = 0; y < nnow && subset; y++)
			{
				while (x < ent.size() && ent[x].first < now[y].first)
					++x;
				subset = x < ent.size() && ent[x].first == now[y].first && ent[x].second == now[y].second;
			}
			if (!subset)
			{
				std::vector<std::pair<u16, microProgram*>> merged;
				merged.reserve(ent.size() + nnow);
				size_t xi = 0;
				u32 yi = 0;
				while (xi < ent.size() || yi < nnow)
				{
					if (yi == nnow || (xi < ent.size() && ent[xi].first < now[yi].first))
						merged.push_back(ent[xi++]);
					else if (xi == ent.size() || now[yi].first < ent[xi].first)
						merged.push_back(now[yi++]);
					else
					{
						merged.push_back(now[yi++]);
						++xi;
					}
				}
				ent = std::move(merged);
			}
		}
	}
	std::memset(&mVU.prog.lpState, 0, sizeof(mVU.prog.lpState));
	mVU.prog.cleared = 0; // the first program entry sets it anyway (search or create); see mVU1clearlpStateJIT

	// 3. The state for the new content.
	if (M.states.size() >= kMemoMaxStates)
		memoFlush();
	const MemoKey key = memoHash(micro);
	auto it = M.states.find(key);
	MemoState* st;
	if (it != M.states.end())
	{
		st = it->second.get();
		if (M.verify && std::memcmp(st->code.data(), micro, 0x4000) != 0)
			++M.verifyBad; // key collision
		if (st->epoch == codePtr)
		{
			++M.hits;
			for (auto& e : st->entries)
			{
				microProgram* p = e.second;
				if (M.verify && !programMatches(p, micro))
				{
					++M.verifyBad;
					continue;
				}
				mVU.prog.quick[e.first].prog = p;
				mVU.prog.quick[e.first].block = p->block[e.first];
				++M.restored;
			}
		}
		else
		{
			++M.stale;
			st->entries.clear();
			st->epoch = codePtr;
		}
	}
	else
	{
		auto ns = std::make_unique<MemoState>();
		ns->epoch = codePtr;
		if (M.verify)
			ns->code.assign(micro, micro + 0x4000);
		st = ns.get();
		M.states.emplace(key, std::move(ns));
		++M.created;
	}
	M.cur = st;
	M.lastCode = codePtr;
	M.lastTotal = mVU.prog.total;
	M.cycles += __rdtsc() - tsc0;
}

void kzvuVu1MemoStats(KzvuVu1CodeStats* out)
{
	out->memoSwitches = g_memo.switches;
	out->memoHits = g_memo.hits;
	out->memoStale = g_memo.stale;
	out->memoNew = g_memo.created;
	out->memoRestored = g_memo.restored;
	out->memoVerifyBad = g_memo.verifyBad;
	out->memoStates = g_memo.states.size();
	out->memoCycles = g_memo.cycles;
}
