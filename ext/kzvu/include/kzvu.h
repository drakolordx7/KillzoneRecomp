// kzvu - PCSX2's VU1 (microVU x86-64 recompiler, VU interpreter fallback) as a static library.
//
// One VU1 per process. All functions must be called from the same host thread (the game thread); nothing in kzvu
// is thread-safe and there is no MTVU thread.
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
	// loads I-bit immediates from micro memory at run time instead of baking them into the code, and leaves the lower
	// words holding I immediates, and the lower words of IADDI/IADDIU/ISUBIU/ILW/ISW/LQ/SQ, out of the "has this
	// program changed" comparison. A program that differs only in those words reuses the already-compiled code, which
	// for the IADDI..SQ group means the OLD immediates keep running (a PCSX2 hack; Killzone is listed as fine with it).
	bool iBitHack = true;

	// PCSX2 speedhack "mVU flag hack" (on by default in PCSX2): skips status flag updates nobody reads.
	bool flagHack = true;

	// PCSX2 gamefix XgKickHack (off by default; Killzone does not use it). Cycle-timed XGKICK transfers.
	bool xgkickHack = false;

	KzvuXgkickFn xgkick = nullptr;
	void* xgkickUser = nullptr;
};

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
