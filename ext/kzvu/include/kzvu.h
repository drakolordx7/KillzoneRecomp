// kzvu - PCSX2's VU1 (microVU x86-64 recompiler, VU interpreter fallback) as a static library.
//
// One VU1 per process. Nothing in kzvu is thread-safe, but the two VUs may run on two host threads: every VU1 function
// (kzvuExecute, kzvuContinue, kzvuSetTop, kzvuSetFBRST for VU1, kzvuMicroWritten, the XGKICK callback, ...) must be
// called from ONE thread that has called kzvuBindVu1Thread() first, and every VU0 function (kzvu0*) from another one.
// Without kzvuBindVu1Thread() all functions belong to the same thread, as before. Once VU1 code has been compiled on a
// thread, VU1 must stay on that thread (the JIT embeds per-thread state addresses). There is no MTVU thread in kzvu.
//
// GPL-3.0+ (PCSX2 code).
#pragma once

#include <cstdint>
#include <string>

// XGKICK: called once per complete GIF packet (all tags up to and including the one with EOP), synchronously from
// inside kzvuExecute()/kzvuContinue(). `packet` is a contiguous copy of the bytes (a packet that wraps past the end of
// the 16 KB data memory is already unwrapped) and is only valid during the call. `startQw` is the VU1 data address
// (in qwords, 0..0x3FF) the packet was read from.
using KzvuXgkickFn = void (*)(void* user, const uint8_t* packet, uint32_t bytes, uint32_t startQw);

struct KzvuConfig
{
	// true: microVU recompiler. false: PCSX2's VU interpreter (slow; for debugging / differential testing).
	bool useJit = true;

	// VU1 clamp mode as in PCSX2's GameIndex (vuClampMode): 0 none, 1 normal, 2 extra, 3 extra + preserve sign.
	// Killzone's GameIndex entry uses 0. Only the recompiler clamps; the interpreter always behaves the same.
	int clampMode = 0;

	// PCSX2 gamefix IbitHack ("reduces VU recompilation"); Killzone's GameIndex entry enables it. With it, microVU
	// reads the I-bit immediates and the immediates of IADDI/IADDIU/ISUBIU/ILW/ISW/LQ/SQ from micro memory at run
	// time instead of baking them into the code, and leaves those lower words out of the "has this program changed"
	// comparison. A program that differs only in those words reuses the compiled code and sees the new immediates.
	// The catch: a change to the opcode/register fields of such a word is not detected either.
	bool iBitHack = true;

	// PCSX2 speedhack "mVU flag hack" (on by default in PCSX2): skips status flag updates nobody reads.
	bool flagHack = true;

	// PCSX2 gamefix XgKickHack (off by default; Killzone does not use it). Cycle-timed XGKICK transfers.
	bool xgkickHack = false;

	KzvuXgkickFn xgkick = nullptr;
	void* xgkickUser = nullptr;

	// VU0 micro mode (VCALLMS/VCALLMSR): true: microVU0 recompiler. false: PCSX2's VU0 interpreter.
	// iBitHack and flagHack apply to VU0 as well.
	bool vu0UseJit = true;
	// VU0 clamp mode (same meaning as clampMode); -1 = use clampMode.
	int vu0ClampMode = -1;
};

// Makes the calling thread the VU1 thread (see the top of this file). Call it on that thread before its first VU1 call,
// and before any VU1 program has run anywhere. VU0/VU1 then use separate VPU_STAT / FBRST words and separate
// pending-interrupt and FBRST state, so a VU0 call on the EE thread can overlap a VU1 program on this one.
void kzvuBindVu1Thread();

// Allocates the 64 MB code cache, resets all VU1 state and applies `cfg`. Calling it again re-applies the config.
bool kzvuInit(const KzvuConfig& cfg, std::string* err = nullptr);
void kzvuShutdown();
bool kzvuIsInit();

// Changes settings at runtime (switching JIT <-> interpreter is allowed between programs). Settings that change the
// generated code (clampMode, iBitHack, flagHack, xgkickHack) flush the recompiler cache.
void kzvuSetConfig(const KzvuConfig& cfg);
const KzvuConfig& kzvuGetConfig();

// Resets VU1 registers, pipeline state and the recompiler cache. VU1 code/data memory is left untouched.
void kzvuReset();

// ---- memory -------------------------------------------------------------------------------------------------------------
// kzvu owns VU1 micro (code) memory and data memory: 16 KB each, 64-byte aligned, at fixed addresses inside the
// executable image for the life of the process (the recompiled code addresses data memory directly). The host's VIF1
// should write straight into these buffers (UNPACK -> kzvuDataMem(), MPG -> kzvuWriteMicro()).
uint8_t* kzvuCodeMem();
uint8_t* kzvuDataMem();
constexpr uint32_t kKzvuCodeSize = 0x4000;
constexpr uint32_t kKzvuDataSize = 0x4000;

// MPG: copies `size` bytes to micro memory at `offset` (bytes; a write past the end wraps to 0, like VIF1 MPG) and
// marks the recompiled program for re-validation, unless the bytes were already identical.
void kzvuWriteMicro(uint32_t offset, const void* src, uint32_t size);
// Call after writing kzvuCodeMem() directly. Cheap: it only marks the current program for re-validation; the next
// kzvuExecute() looks the program up by content among already-compiled programs before compiling anything.
void kzvuMicroWritten(uint32_t offset, uint32_t size);

// ---- VIF1 / VU0 control registers VU1 reads ---------------------------------------------------------------------------
// TOP/ITOP as XTOP/XITOP see them (the host's VIF1 double-buffering state at the time of MSCAL/MSCNT).
void kzvuSetTop(uint32_t top, uint32_t itop);
// VU0 FBRST: bit 10 (DE1) / bit 11 (TE1) enable stopping VU1 on the D / T bit.
void kzvuSetFBRST(uint32_t fbrst);

// ---- execution ----------------------------------------------------------------------------------------------------------
// MSCAL/MSCALF: starts a program at `startPcBytes` (byte address in micro memory, multiple of 8) and runs until the
// E bit (plus its delay slot) or until at least `maxCycles` VU cycles have run. Returns VU cycles used. If a previous
// program is still running it is first run to completion (as PCSX2's vu1ExecMicro does); those cycles are included.
uint32_t kzvuExecute(uint32_t startPcBytes, uint32_t maxCycles);
// MSCNT / budget resume: continues a program that stopped because its cycle budget ran out. Returns cycles used
// (0 if VU1 is idle).
uint32_t kzvuContinue(uint32_t maxCycles);
// VBS1: a program is running (started and has not reached its E bit, i.e. it ran out of budget).
bool kzvuRunning();
// VU1 half of VU0's VPU_STAT: 0x100 VBS1 busy, 0x200 VDS1 stopped on D bit, 0x400 VTS1 stopped on T bit,
// 0x1000 XGKICK in progress (interpreter only).
uint32_t kzvuVpuStat();
// True if VU1 raised its INTC interrupt (D/T bit stop) since the last call.
bool kzvuTakeInterrupt();
// Total VU1 cycles executed since kzvuInit.
uint64_t kzvuCycles();

// ---- diagnostics: microVU1's recompiler state (read only; call from the VU1 thread) -------------------------------------
struct KzvuVu1CodeStats
{
	uint64_t codeBytes;       // JIT code emitted so far in the current cache (drops back to ~0 when the cache is reset)
	uint64_t cacheBytes;      // size of the cache before it resets
	uint32_t programsCreated; // microPrograms created since the last cache reset
	uint32_t programsCached;  // microPrograms currently in the per-startPC lists
	// Program-search cost (only counted when kzvu is built with KZVU_SEARCH_STATS, else 0): memcmp calls made by
	// microVU's program lookup (one per compiled range of every candidate program), bytes compared, TSC cycles spent.
	uint64_t cmpCalls;
	uint64_t cmpBytes;
	uint64_t cmpCycles;
	// Code-state memo (KZVU_STATE_MEMO, see KzvuMicroVU.cpp): micro-memory changes announced since start, how many of them
	// restored a known state's program table, found a known state whose table was stale, met a new state, entries restored,
	// and (KZVU_STATE_MEMO_VERIFY=1) restored/saved entries that failed the range check.
	uint64_t memoSwitches, memoHits, memoStale, memoNew, memoRestored, memoVerifyBad, memoStates, memoCycles; // memoCycles: TSC cycles spent in kzvuVu1CodeChanged
};
void kzvuVu1CodeStats(KzvuVu1CodeStats* out); // walks all program lists: call rarely
void kzvuVu1CodeCounters(uint64_t* codeBytes, uint32_t* programsCreated); // the two cheap counters, for per-call use
struct KzvuVu1ProgDiff
{
	uint32_t programs;  // programs cached for this start PC
	uint32_t ranges;    // compiled ranges of the compared program
	uint32_t wordIndex; // first micro-memory word (32-bit index) that differs from the cached program, ~0u = identical
	uint32_t oldWord;
	uint32_t newWord;
};
// Compares VU1 micro memory with cached program number `which` (0 = most recently used) for `startPcBytes`, over the
// ranges it was compiled for. False if there is no such program.
// VU1 micro memory changed (used by kzvuMicroWritten/kzvuWriteMicro): invalidates microVU1's current-program table, or
// swaps in the table remembered for the same memory content. kzvuVu1MemoFlush() forgets the remembered tables (microVU1's
// programs were freed: reset, config change, shutdown).
void kzvuVu1CodeChanged(uint32_t offset, uint32_t size);
void kzvuVu1MemoFlush();
bool kzvuVu1DiffCachedProgram(uint32_t startPcBytes, uint32_t which, KzvuVu1ProgDiff* out);
// Number of GIF packets delivered to the XGKICK callback since kzvuInit.
uint64_t kzvuXgkickCount();

// ---- registers --------------------------------------------------------------------------------------------------------
// VI index as in PCSX2/VU0 CFC2 numbering: 0-15 integer registers, 16 status, 17 MAC, 18 clip, 20 R, 21 I, 22 Q,
// 23 P, 26 TPC (in 8-byte units).
enum : int
{
	kKzvuStatus = 16,
	kKzvuMac = 17,
	kKzvuClip = 18,
	kKzvuR = 20,
	kKzvuI = 21,
	kKzvuQ = 22,
	kKzvuP = 23,
	kKzvuTPC = 26,
};
uint32_t kzvuGetVI(int index);
void kzvuSetVI(int index, uint32_t value);
void kzvuGetVF(int index, uint32_t out[4]); // x, y, z, w as raw IEEE bits
void kzvuSetVF(int index, const uint32_t in[4]);
void kzvuGetACC(uint32_t out[4]);
void kzvuSetACC(const uint32_t in[4]);

// ---- VU0 micro mode -----------------------------------------------------------------------------------------------------
// VU0 runs the microprograms the EE starts with VCALLMS/VCALLMSR. Its register file is the EE's COP2 register file, so
// the host copies its COP2 state in with kzvu0SetRegs() before kzvu0Execute() and back out with kzvu0GetRegs().
// Same threading rule as VU1: call from the game thread only.
//
// Memory: 4 KB micro memory and 4 KB data memory, owned by kzvu (static, 64-byte aligned, same reason as VU1's). Loads
// and stores from VU0 micro code to data addresses 0x4000-0x43FF (qword 0x400+) reach VU1's VF/VI registers, as on the
// PS2 (microVU maps them onto kzvu's VU1 state).
uint8_t* kzvu0CodeMem();
uint8_t* kzvu0DataMem();
constexpr uint32_t kKzvu0CodeSize = 0x1000;
constexpr uint32_t kKzvu0DataSize = 0x1000;
// Call after VU0 micro memory changed (VIF0 MPG or EE stores). Same semantics as kzvuMicroWritten().
void kzvu0MicroWritten(uint32_t offset, uint32_t size);

// VU0 registers as the EE's COP2 sees them (CFC2/CTC2 numbering in the comments). Raw IEEE bits for floats.
struct Kzvu0Regs
{
	uint32_t vf[32][4]; // VF0 is ignored on write (always 0,0,0,1)
	uint32_t vi[16];    // 16-bit values; VI0 is ignored on write
	uint32_t acc[4];
	uint32_t status;    // vi16
	uint32_t mac;       // vi17
	uint32_t clip;      // vi18
	uint32_t r;         // vi20 (23-bit mantissa; the exponent bits are forced to 0x3F800000 by RINIT/RXOR)
	uint32_t i;         // vi21
	uint32_t q;         // vi22
};
void kzvu0SetRegs(const Kzvu0Regs& in);
void kzvu0GetRegs(Kzvu0Regs& out);

// kzvu0Call: the whole VCALLMS in one step, for hosts whose COP2 file is plain memory (the game's R5900Context).
// Equivalent to kzvu0SetRegs + kzvuSetFBRST + kzvu0Execute + kzvu0GetRegs, but the registers are copied straight
// between the host's storage and VU0's, with no Kzvu0Regs in between.
struct Kzvu0Host
{
	uint32_t (*vf)[4]; // 32 x 4 raw bits; VF0 is not read, and is written back as (0, 0, 0, 1)
	uint16_t* vi;      // 16 entries; VI0 is not read, and is written back as 0
	uint32_t* acc;     // 4 lanes
	uint16_t* status;
	uint32_t* mac;
	uint32_t* clip;
	uint32_t* clip2;   // written with the same value as clip
	uint32_t* r;       // 4 lanes: lane 0 is read, all four are written
	uint32_t* i;
	uint32_t* q;
};
struct Kzvu0CallOut
{
	uint32_t tpc;     // TPC in bytes, as kzvu0TPC()
	uint32_t vpuStat; // as kzvu0VpuStat()
};
Kzvu0CallOut kzvu0Call(const Kzvu0Host& host, uint32_t startPcBytes, uint32_t maxCycles, uint32_t fbrst);

// VCALLMS: starts VU0 at `startPcBytes` (byte address, multiple of 8; 0xFFFFFFFF = continue at TPC) and runs it until
// the E bit (plus its delay slot), a D/T-bit stop (if enabled in FBRST, see kzvuSetFBRST: DE0 bit 2 / TE0 bit 3), or at
// least `maxCycles` VU cycles. M-bit pauses (where a real VU0 lets an interlocked COP2 transfer through) are run past:
// the whole program runs synchronously, as if the EE issued its next interlocking COP2 instruction right away. If a
// program is still running from before (budget ran out), it is first run to completion. Returns VU cycles used.
uint32_t kzvu0Execute(uint32_t startPcBytes, uint32_t maxCycles);
bool kzvu0Running();        // VBS0
uint32_t kzvu0VpuStat();    // VU0 half of VPU_STAT: 0x1 VBS0 busy, 0x2 VDS0 D stop, 0x4 VTS0 T stop
uint32_t kzvu0TPC();        // TPC in bytes
uint64_t kzvu0Cycles();     // total VU0 cycles executed since kzvuInit
uint64_t kzvu0Calls();      // kzvu0Execute calls since kzvuInit
