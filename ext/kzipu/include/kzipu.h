// kzipu: PCSX2's IPU (the PS2 MPEG-2 macroblock decoder) as a static library.
//
// The host emulates the EE side of the hardware and forwards to kzipu:
//   - EE accesses to the IPU registers    0x10002000 IPU_CMD, 0x10002010 IPU_CTRL, 0x10002020 IPU_BP, 0x10002030 IPU_TOP
//   - EE accesses to the IPU FIFO ports   0x10007000 IPU_out_FIFO (128-bit read), 0x10007010 IPU_in_FIFO (128-bit write)
//   - EE accesses to the IPU DMA channels 0x1000B000 ch3 IPU_FROM, 0x1000B400 ch4 IPU_TO (CHCR/MADR/QWC/TADR)
// and gets back the IPU interrupt (INTC bit 8) and the ch3/ch4 DMA-end interrupts (D_STAT bits 3/4) as callbacks.
// kzipu runs PCSX2's own IPU_TO chain / IPU_FROM normal-mode DMA logic (IPUdma.cpp); the host only translates DMA
// addresses into pointers. A host that runs its own DMA engine can instead move data with kzipuWriteInFifo /
// kzipuReadOutFifo (see KzipuConfig::hostDrainsOutput and README.md).
//
// Threading: single-threaded. Call everything from one host thread (the game's EE thread). Callbacks run synchronously
// on that thread, from inside the kzipu call that caused them.
//
// GPL-3.0+ (PCSX2 code). Anything that links kzipu is a GPL-3 derivative work.
#pragma once

#include <cstdint>

struct KzipuConfig
{
	void* user = nullptr;

	// INTC bit 8 (INTC_IPU) was raised: a command finished, or the IPU was reset (IPU_CTRL.RST).
	void (*intc)(void* user) = nullptr;

	// A DMAC channel raised its D_STAT CIS bit: 3 = IPU_FROM finished, 4 = IPU_TO finished, 15 = bus error (BEIS).
	// CHCR.STR of that channel is already 0 when this is called.
	void (*dmacIrq)(void* user, int channel) = nullptr;

	// DMA address translation. `addr` is a DMAC physical address from MADR or TADR, 16-byte aligned; bit 31 set means
	// scratchpad (offset addr & 0x3FF0). Return a pointer valid for `bytes` bytes (at most 128), or nullptr for a bus
	// error. `write` is true for IPU_FROM (kzipu writes the memory), false for IPU_TO data and chain tags.
	uint8_t* (*dmaPtr)(void* user, uint32_t addr, uint32_t bytes, bool write) = nullptr;

	// Optional log sink for PCSX2's warnings/errors. level: 0 info, 1 warning, 2 error.
	void (*log)(void* user, int level, const char* msg) = nullptr;

	// false (default): the output FIFO is drained by DMA ch3, which the game programs through kzipuWriteReg32.
	// true: the host drains it with kzipuReadOutFifo; ch3 is treated as permanently armed and its registers are unused.
	bool hostDrainsOutput = false;

	// true (default): IDEC's QSC field is a quantiser_scale_code, mapped through IPU_CTRL.QST (linear: 2*QSC,
	// non-linear: MPEG-2 table) exactly as PCSX2 does for BDEC. false: PCSX2's IDEC behaviour, which uses QSC as the
	// quantiser_scale itself until a macroblock carries macroblock_quant. See README "IDEC quantiser scale".
	bool mapIdecQsc = true;
};

struct KzipuStatus
{
	bool busy;              // IPU_CTRL.BUSY: a command is executing (possibly stalled on data)
	uint32_t command;       // current/last command number (IPU_CMD bits 31:28): 0 BCLR 1 IDEC 2 BDEC 3 VDEC 4 FDEC 5 SETIQ 6 SETVQ 7 CSC 8 PACK 9 SETTH
	uint32_t inFifoQwc;     // IPU_CTRL.IFC: qwords in the input FIFO (0-8)
	uint32_t outFifoQwc;    // IPU_CTRL.OFC: qwords in the output FIFO (0-8)
	uint32_t bitPointer;    // IPU_BP.BP
	bool waitingForInput;   // the command is stalled until more input arrives
	bool waitingForOutput;  // the command is stalled until the output FIFO is drained
	bool inputRequest;      // IPU_TO DMA request line: the IPU wants input and the input FIFO has room
	bool outputRequest;     // IPU_FROM request line: the output FIFO holds data
	bool toIpuActive;       // ch4 CHCR.STR
	bool fromIpuActive;     // ch3 CHCR.STR
	uint32_t pendingEvents; // bitmask of scheduled internal events (1<<3 IPU_FROM, 1<<4 IPU_TO, 1<<19 IPU process)
	uint64_t cycle;         // kzipu's virtual EE cycle counter
	uint32_t intcCount;     // INTC_IPU raised since kzipuInit
	uint32_t fromIpuEnds;   // ch3 DMA-end interrupts since kzipuInit
	uint32_t toIpuEnds;     // ch4 DMA-end interrupts since kzipuInit
};

bool kzipuInit(const KzipuConfig& cfg);  // resets everything; call once before any other function
void kzipuShutdown();
void kzipuReset();                       // hardware reset: IPU registers, FIFOs, decoder state, ch3/ch4, events

// True for the addresses kzipu owns: 0x10002000-0x10002FFF (IPU registers, mirrored every 0x100), 0x10007000-0x1000701F, 0x1000B000-0x1000B0FF,
// 0x1000B400-0x1000B4FF.
bool kzipuHandlesAddress(uint32_t addr);

// IPU registers and DMA ch3/ch4 registers, as the EE sees them.
//   IPU_CMD  (0x10002000) read: DATA (FDEC/VDEC result, otherwise the next 32 bits of the stream); 64-bit read adds
//            BUSY in bit 63. Write: starts a command.
//   IPU_CTRL (0x10002010) read/write (bit 30 RST resets the IPU).
//   IPU_BP   (0x10002020) read: BP | IFC << 8 | FP << 16.
//   IPU_TOP  (0x10002030) read: next 32 bits of the stream; 64-bit read adds BUSY in bit 63.
//   D3/D4 CHCR (+0x00) write with STR=1 starts the DMA; MADR (+0x10), QWC (+0x20), TADR (+0x30).
uint32_t kzipuReadReg32(uint32_t addr);
uint64_t kzipuReadReg64(uint32_t addr);
void kzipuWriteReg32(uint32_t addr, uint32_t value);
void kzipuWriteReg64(uint32_t addr, uint64_t value);

// FIFO access, 16 bytes per qword. These are the EE's 128-bit accesses to 0x10007010 / 0x10007000, and also the data
// path for a host that runs its own IPU_TO / IPU_FROM DMA.
// Returns how many qwords were accepted (input FIFO room is 8 - IFC) or read (at most OFC).
uint32_t kzipuWriteInFifo(const void* qwords, uint32_t count);
uint32_t kzipuReadOutFifo(void* qwords, uint32_t count);

// D_CTRL.DMAE && !D_ENABLER.CPND. While false, no IPU DMA starts or advances (starts are queued) and no IPU event runs,
// as in PCSX2. Default true.
void kzipuSetDmacEnabled(bool enabled);

// Advances kzipu's virtual EE clock by eeCycles and runs every IPU/DMA event that falls due (PCSX2 timing).
// Returns the number of events run.
uint32_t kzipuRun(uint32_t eeCycles);

// Runs pending events immediately, regardless of when they are due, until nothing is pending or maxEvents ran.
// Use it when the guest polls (IPU_CTRL/IPU_CMD/CHCR/D_STAT reads) and the host has no cycle count to give.
uint32_t kzipuStep(uint32_t maxEvents = 100000);

KzipuStatus kzipuGetStatus();
