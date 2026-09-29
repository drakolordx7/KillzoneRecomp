// kzvu: types of the shim objects the PCSX2 VU code is redirected to (see kzvu_prefix.h), plus the internal glue
// between src/kzvu.cpp and shim/KzvuShims.cpp.
// SPDX-License-Identifier: GPL-3.0+
#pragma once

#include <atomic>
#include <cstdint>

// ---- EmuConfig: only the fields the VU code reads ---------------------------------------------------------------------
struct KzvuEmuConfig
{
	struct
	{
		struct
		{
			bool EnableEE = false;
			bool EnableIOP = false;
			bool EnableVU0 = false;
			bool EnableVU1 = true; // REC_VU1: microVU (true) or the interpreter (false)
			bool vu0Overflow = true;
			bool vu0ExtraOverflow = false;
			bool vu0SignOverflow = false;
			bool vu0Underflow = false;
			bool vu1Overflow = true;
			bool vu1ExtraOverflow = false;
			bool vu1SignOverflow = false;
			bool vu1Underflow = false;
			bool fpuOverflow = true;
			bool fpuExtraOverflow = false;
			bool fpuFullMode = false;
			bool EnableEECache = false;
			bool EnableFastmem = false;
		} Recompiler;

		FPControlRegister FPUFPCR;
		FPControlRegister FPUDivFPCR;
		FPControlRegister VU0FPCR;
		FPControlRegister VU1FPCR;
	} Cpu;

	struct
	{
		bool FpuMulHack = false;
		bool VuAddSubHack = false;
		bool IbitHack = false;
		bool VUSyncHack = false;
		bool VUOverflowHack = false;
		bool XgKickHack = false;
		bool FullVU0SyncHack = false;
	} Gamefixes;

	struct
	{
		bool vuFlagHack = true;
		bool vuThread = false;
		bool vu1Instant = true;
		s8 EECycleRate = 0;
		u8 EECycleSkip = 0;
	} Speedhacks;
};
extern KzvuEmuConfig kzvu_EmuConfig;

// ---- gifUnit: XGKICK packets are collected here and handed to the host ------------------------------------------------
// microVU and the interpreter call the same three Gif_Unit members PCSX2 uses for PATH1. Data arrives in pieces (a
// 16KB wrap, or tag-by-tag from the interpreter's timed transfer); complete packets (up to and including the tag with
// EOP) are passed to the host callback in one call.
struct KzvuGifPath
{
	void CopyGSPacketData(u8* pMem, u32 size, bool aligned = false);
};

struct KzvuGifUnit
{
	KzvuGifPath gifPath[3];

	u32 GetGSPacketSize(GIF_PATH pathIdx, u8* pMem, u32 offset = 0, u32 size = ~0u, bool flush = false);
	u32 TransferGSPacketData(GIF_TRANSFER_TYPE tranType, u8* pMem, u32 size, bool aligned = false);
};
extern KzvuGifUnit kzvu_gifUnit;

// ---- vu1Thread: MTVU is compiled out (THREAD_VU1 == false); this only has to satisfy the compiler ----------------------
struct KzvuVuThreadStub
{
	alignas(16) VIFregisters vifRegs;
	u32 vuFBRST = 0;
	std::atomic<u32> mtvuInterrupts{0};

	bool IsOpen() const { return false; }
	void Open() {}
	void Close() {}
	void WaitVU() {}
	void Get_MTVUChanges() {}
	void KickStart() {}
	void ExecuteVU(u32, u32, u32, u32) {}
};
extern KzvuVuThreadStub kzvu_vu1Thread;

// ---- glue between the API (src/kzvu.cpp) and the shims ------------------------------------------------------------------
namespace kzvu
{
	using XgkickFn = void (*)(void* user, const u8* packet, u32 bytes, u32 startQw);

	void SetXgkickCallback(XgkickFn fn, void* user);
	void SetHostFPCR(FPControlRegister fpcr); // MXCSR the XGKICK callback runs with (the host's, not the VU's)
	void GifReset();                  // drop any partially collected packet
	u64 GifPacketCount();             // packets delivered since start

	u8* CodeCacheBase();              // JIT code region (allocated by AllocCodeCache)
	bool AllocCodeCache(u32 bytes, std::string* err);
	void FreeCodeCache();

	u32 TakeInterrupts(u32 mask);     // hwIntcIrq(INTC_VU0/VU1) bits in `mask` raised since the last call (D/T bit)
} // namespace kzvu
