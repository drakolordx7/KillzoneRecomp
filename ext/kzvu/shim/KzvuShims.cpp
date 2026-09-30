// kzvu shim: the emulator state and subsystems PCSX2's VU code references, reduced to what VU1 needs.
// SPDX-License-Identifier: GPL-3.0+

#include "kzvu_prefix.h"

#include "common/CrashHandler.h"
#include "common/RedtapeWindows.h"

#include <intrin.h>
#include <string>
#include <vector>

// ---- emulator globals (all renamed by kzvu_rename.h) --------------------------------------------------------------------
alignas(__pagealignsize) u8 eeHw[Ps2MemSize::Hardware];   // only vif0Regs/vif1Regs (TOP/ITOP/STAT) are used
alignas(16) cpuRegistersPack _cpuRegistersPack;           // cpuRegs.cycle, and the JIT's text pointer (&cpuRegs.GPR.r[9])
alignas(16) VURegs vuRegs[2];

KzvuEmuConfig kzvu_EmuConfig;
KzvuGifUnit kzvu_gifUnit;
KzvuVuThreadStub kzvu_vu1Thread;

BaseVUmicroCPU* CpuVU0 = nullptr;
BaseVUmicroCPU* CpuVU1 = nullptr;

VURegs kzvu_vu0_vu1thread;
thread_local VURegs* kzvu_vu0 = &vuRegs[0];

// ---- CPU features (PCSX2 fills this from cpuinfo in GS/MultiISA.cpp) ------------------------------------------------------
static ProcessorFeatures DetectProcessorFeatures()
{
	ProcessorFeatures f = {};
	int r[4];
	__cpuid(r, 0);
	const int max_leaf = r[0];
	__cpuid(r, 1);
	const bool osxsave = (r[2] >> 27) & 1;
	const bool avx = (r[2] >> 28) & 1;
	f.hasFMA = (r[2] >> 12) & 1;
	bool ymm_os = false;
	if (osxsave)
		ymm_os = (_xgetbv(0) & 6) == 6;
	bool avx2 = false;
	if (max_leaf >= 7)
	{
		__cpuidex(r, 7, 0);
		avx2 = (r[1] >> 5) & 1;
		f.hasBMI2 = (r[1] >> 8) & 1;
	}
	if (avx && ymm_os)
		f.vectorISA = avx2 ? ProcessorFeatures::VectorISA::AVX2 : ProcessorFeatures::VectorISA::AVX;
	else
		f.vectorISA = ProcessorFeatures::VectorISA::SSE4;
	f.hasSlowGather = false;
	return f;
}
const ProcessorFeatures g_cpu = DetectProcessorFeatures();

// ---- interrupts / EE events ---------------------------------------------------------------------------------------------
static thread_local u32 s_pending_irq = 0; // per thread: VU0 (EE thread) and VU1 (worker) each see their own

void hwIntcIrq(int n)
{
	// VU0/VU1 raise INTC_VU0 (6) / INTC_VU1 (7) when they stop on a D or T bit; VPU_STAT says which bit.
	s_pending_irq |= 1u << n;
}

u32 kzvu::TakeInterrupts(u32 mask)
{
	const u32 r = s_pending_irq & mask;
	s_pending_irq &= ~mask;
	return r;
}

void CPU_INT(EE_EventType n, s32 ecycle)
{
	// The interpreter schedules a VIF1 wake-up when an XGKICK finishes while VIF1 waits on the GIF (VGW). The host's
	// VIF1 is not stalled by kzvu, so there is nothing to wake.
}

// ---- JIT code memory --------------------------------------------------------------------------------------------------
static u8* s_code = nullptr;
static u32 s_code_size = 0;

bool kzvu::AllocCodeCache(u32 bytes, std::string* err)
{
	if (s_code)
		return true;
	// Anywhere in the address space is fine: the recompiled code reaches kzvu's globals (VU registers, VU memory,
	// microVU state, all inside the executable image) through the text pointer register, and calls far functions
	// indirectly, the same way PCSX2 places its code cache.
	s_code = static_cast<u8*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
	if (!s_code)
	{
		if (err)
			*err = "VirtualAlloc of the microVU code cache failed (error " + std::to_string(GetLastError()) + ")";
		return false;
	}
	s_code_size = bytes;
	return true;
}

void kzvu::FreeCodeCache()
{
	if (s_code)
		VirtualFree(s_code, 0, MEM_RELEASE);
	s_code = nullptr;
	s_code_size = 0;
}

u8* kzvu::CodeCacheBase()
{
	return s_code;
}

u8* SysMemory::GetCodePtr(size_t offset)
{
	// Only microVU0 and microVU1 ask for code memory: [mVU0recOffset, mVU1recOffset + mVU1recSize], which PCSX2 lays
	// out back to back and kzvu allocates as one region.
	static_assert(HostMemoryMap::mVU1recOffset == HostMemoryMap::mVU0recOffset + HostMemoryMap::mVU0recSize);
	pxAssertRel(offset >= HostMemoryMap::mVU0recOffset &&
					offset <= HostMemoryMap::mVU1recOffset + HostMemoryMap::mVU1recSize,
		"kzvu only provides microVU0/microVU1 code memory");
	return s_code + (offset - HostMemoryMap::mVU0recOffset);
}

// ---- GIF PATH1 (XGKICK) -------------------------------------------------------------------------------------------------
static kzvu::XgkickFn s_xgkick_fn = nullptr;
static void* s_xgkick_user = nullptr;
static std::vector<u8> s_gif_buf;   // bytes of the packet being collected
static u32 s_gif_start_qw = 0;      // VU1 data address (qwords) the collected packet started at
static u64 s_gif_packets = 0;
static thread_local FPControlRegister s_host_fpcr = FPControlRegister::GetCurrent();

void kzvu::SetHostFPCR(FPControlRegister fpcr)
{
	s_host_fpcr = fpcr;
}

void kzvu::SetXgkickCallback(XgkickFn fn, void* user)
{
	s_xgkick_fn = fn;
	s_xgkick_user = user;
}

void kzvu::GifReset()
{
	s_gif_buf.clear();
}

u64 kzvu::GifPacketCount()
{
	return s_gif_packets;
}

static void GifAppend(const u8* p, u32 size)
{
	if (size == 0)
		return;
	if (s_gif_buf.empty())
	{
		const u8* mem = vuRegs[1].Mem;
		if (p >= mem && p < mem + 0x4000)
			s_gif_start_qw = static_cast<u32>(p - mem) / 16;
	}
	s_gif_buf.insert(s_gif_buf.end(), p, p + size);
}

// Emits every complete packet (tags up to and including the one with EOP) at the front of the buffer.
static void GifEmitComplete()
{
	size_t pos = 0;
	size_t packet_start = 0;
	while (pos + 16 <= s_gif_buf.size())
	{
		Gif_Tag tag(&s_gif_buf[pos]);
		const size_t end = pos + 16 + tag.len;
		if (end > s_gif_buf.size())
			break;
		pos = end;
		if (tag.tag.EOP)
		{
			if (s_xgkick_fn)
			{
				// VU code runs with the VU rounding mode (chop, DAZ/FTZ); the host's GIF/GS code gets its own MXCSR back.
				const FPControlRegister vu_fpcr = FPControlRegister::GetCurrent();
				FPControlRegister::SetCurrent(s_host_fpcr);
				s_xgkick_fn(s_xgkick_user, &s_gif_buf[packet_start], static_cast<u32>(pos - packet_start), s_gif_start_qw);
				FPControlRegister::SetCurrent(vu_fpcr);
			}
			s_gif_packets++;
			packet_start = pos;
		}
	}
	if (packet_start)
	{
		s_gif_buf.erase(s_gif_buf.begin(), s_gif_buf.begin() + packet_start);
		s_gif_start_qw = (s_gif_start_qw + static_cast<u32>(packet_start / 16)) & 0x3ff;
	}
}

void KzvuGifPath::CopyGSPacketData(u8* pMem, u32 size, bool aligned)
{
	GifAppend(pMem, size);
}

u32 KzvuGifUnit::TransferGSPacketData(GIF_TRANSFER_TYPE tranType, u8* pMem, u32 size, bool aligned)
{
	GifAppend(pMem, size);
	GifEmitComplete();
	return size;
}

// Same as Gif_Unit::GetGSPacketSize (Gif_Unit.h), with REC_VU1 meaning "microVU is the active VU1 core".
u32 KzvuGifUnit::GetGSPacketSize(GIF_PATH pathIdx, u8* pMem, u32 offset, u32 size, bool flush)
{
	u32 memMask = pathIdx ? ~0u : 0x3fffu;
	u32 curSize = 0;
	for (;;)
	{
		Gif_Tag gifTag(&pMem[offset & memMask]);
		incTag(offset, curSize, 16 + gifTag.len); // Tag + Data length
		if (pathIdx == GIF_PATH_1 && curSize >= 0x4000)
		{
			DevCon.Warning("Gif Unit - GS packet size exceeded VU memory size!");
			return 0;
		}
		if (curSize >= size)
			return size;
		if (((flush && gifTag.tag.EOP) || !flush) && (CHECK_XGKICKHACK || !REC_VU1))
			return curSize | ((u32)gifTag.tag.EOP << 31);
		if (gifTag.tag.EOP)
			return curSize;
	}
}

// ---- host hooks used by the pcsx2/common slice (renamed to KzvuHost / KzvuCrashHandler) --------------------------------
void CrashHandler::WriteDumpForCaller()
{
}
