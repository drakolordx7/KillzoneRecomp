// kzspu2: PCSX2's SPU2 (PS2 sound processor) as a static library, without any audio backend.
//
// The host forwards the IOP's SPU2 register accesses and IOP DMA channels 4/7, and advances the SPU2 with IOP time.
// kzspu2 mixes 48 kHz stereo into a ring buffer, which an audio thread drains with kzspu2ReadSamples(). Interrupts come
// back through callbacks: SPU2 IRQ (IOP INTC 9) and DMA end (IOP DMA 4 / 7).
//
// Threading: everything except kzspu2ReadSamples / kzspu2AvailableFrames runs on one thread (the IOP's). The ring is
// single-producer (that thread) / single-consumer (the audio thread). Callbacks run synchronously inside the kzspu2 call
// that caused them; the host should queue them and dispatch guest interrupt handlers afterwards.
//
// Time: the IOP clock is 36.864 MHz, and the SPU2 outputs one sample per 768 IOP cycles (48 kHz). Every call takes the
// host's current IOP cycle, which must not go backwards.
//
// GPL-3.0+ (PCSX2 code).
#pragma once

#include <cstddef>
#include <cstdint>

struct KzSpu2Config
{
	void* user = nullptr;
	void (*irq)(void* user) = nullptr;                   // SPU2 IRQ raised (IOP INTC line 9)
	void (*dmaComplete)(void* user, int core) = nullptr; // DMA finished: core 0 = IOP DMA ch4, core 1 = IOP DMA ch7
	// Optional tap on the emulation thread: every mixed block (e.g. a WAV writer). Called before the ring write.
	void (*samples)(void* user, const int16_t* stereo, uint32_t frames) = nullptr;
	void (*log)(void* user, int level, const char* msg) = nullptr; // 0 info, 1 warning, 2 error
	uint32_t ringFrames = 16384; // output ring capacity (frames); when full, new samples are dropped and counted
	bool dcFilter = true;        // PCSX2's DC-blocking filter on the final output
};

struct KzSpu2Status
{
	uint64_t iopCycle;       // last cycle the SPU2 was advanced to
	uint64_t framesMixed;    // total output frames mixed
	uint64_t framesDropped;  // frames the ring had no room for
	uint32_t irqCount;       // SPU2 IRQs raised
	uint32_t dmaCount[2];    // DMA-end interrupts raised per core
	uint32_t keyedOnVoices[2]; // voices with an active envelope, per core
	uint32_t coreAttr[2];    // ATTR register per core
	int16_t peak[2];         // max |sample| (L, R) since the last kzspu2GetStatus(true)
};

bool kzspu2Init(const KzSpu2Config& config); // resets the SPU2 to its power-on state (PCSX2's SPU2::Reset)
void kzspu2Shutdown();
void kzspu2Reset(uint64_t iopCycle);

// Optional: IOP RAM, used only if a PCSX2 fallback path needs a DMA pointer it was not given.
void kzspu2SetIopRam(uint8_t* base, uint32_t size);

// ---- time -------------------------------------------------------------------------------------------------------------------
void kzspu2Advance(uint64_t iopCycle);   // mix every whole sample up to iopCycle and run DMA progress
uint64_t kzspu2NextEventCycle();         // absolute IOP cycle at which DMA progress is due; UINT64_MAX if nothing is

// ---- registers: IOP physical 0x1F900000-0x1F9007FF (any KSEG mirror accepted), 16-bit ---------------------------------------
bool kzspu2HandlesAddress(uint32_t iopAddr);
uint16_t kzspu2Read16(uint32_t iopAddr, uint64_t iopCycle);
void kzspu2Write16(uint32_t iopAddr, uint16_t value, uint64_t iopCycle);

// ---- DMA (PCSX2's psxDma4 / psxDma7) ----------------------------------------------------------------------------------------
// core 0 = IOP DMA channel 4, core 1 = channel 7. `iopRam` points at MADR inside IOP RAM, `halfwords` = BCR words * 2.
// The memory must stay valid until dmaComplete: transfers progress over IOP time, and auto-DMA (ADMA) reads the source
// while it plays. `madr` is only used to report MADR progress (kzspu2DmaMadr).
void kzspu2DmaWrite(int core, uint16_t* iopRam, uint32_t halfwords, uint32_t madr, uint64_t iopCycle); // IOP -> SPU2
void kzspu2DmaRead(int core, uint16_t* iopRam, uint32_t halfwords, uint32_t madr, uint64_t iopCycle);  // SPU2 -> IOP
bool kzspu2DmaBusy(int core);        // started and not yet completed
uint32_t kzspu2DmaMadr(int core);    // MADR as the SPU2 advances it (TADR once complete)

// ---- output (audio thread) -----------------------------------------------------------------------------------------------
uint32_t kzspu2ReadSamples(int16_t* stereo, uint32_t frames); // interleaved L/R; returns frames copied (<= frames)
uint32_t kzspu2AvailableFrames();
uint32_t kzspu2SkipFrames(uint32_t frames);                   // consumer-side discard; returns frames skipped

// ---- inspection ------------------------------------------------------------------------------------------------------------
KzSpu2Status kzspu2GetStatus(bool resetPeak = false);
const uint16_t* kzspu2Memory(); // SPU2 RAM, 1M halfwords (2 MB)
