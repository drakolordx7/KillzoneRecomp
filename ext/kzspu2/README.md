# kzspu2: PCSX2's SPU2 as a static library

kzspu2 compiles PCSX2's SPU2 (the PS2 sound processor: 2 cores x 24 ADPCM voices, ADSR, reverb, auto-DMA input) into
one static library, `kzspu2.lib`. It has no audio backend. The mixed 48 kHz stereo output goes to a lock-free ring
buffer that the host's audio thread drains.

kzspu2 exists because the port's IOP emulator (`ext/PS2Recomp/ps2xIOP`) runs the game's real sound drivers
(`LIBSD.IRX`, `SDRDRV.IRX`, `PSOUND_R.IRX`), but its SPU2 register block (0x1F900000) was a stub. SPU2 DMA only
produced a fake completion interrupt, so the game ran without sound.

- PCSX2 source: `ext/pcsx2`, a depth-1 clone of PCSX2 master (the same tree kzgs/kzvu/kzipu use). kzspu2 does not
  modify anything in `ext/pcsx2`.
- No external dependencies.

## Build

The only supported toolchain is MSVC x64 (VS 2026 / 14.51), using CMake 3.24 or newer with Ninja. Keep `-j` at 4 on
this machine.

```sh
cmd //c "tools\scripts\vsenv.bat cmake -S ext\kzspu2 -B ext\kzspu2\build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo"
cmd //c "tools\scripts\vsenv.bat cmake --build ext\kzspu2\build -j 4"
ext/kzspu2/build/kzspu2_test.exe          # exit code 0 = pass; writes build/out/*.wav
```

- A clean build is 10 translation units for the library and 1 for the test. It takes a few seconds.
- From the game's CMake:

```cmake
add_subdirectory(ext/kzspu2)          # KZSPU2_BUILD_TEST defaults to OFF when not top-level
target_link_libraries(kz_port PRIVATE kzspu2)
```

A consumer inherits only the public include directory. All compile flags are private: C++20, `/permissive-`,
`/Zc:preprocessor`, `/arch:AVX2` (the same as kzgs/kzvu/kzipu), PCSX2's defines and the forced include.

### What is compiled

- **PCSX2 SPU2 code, unmodified:** `spu2sys.cpp` (registers, voices, DMA progress, time), `Mixer.cpp` (ADPCM decode
  and cache, voice mix, core mix), `ReadInput.cpp` (auto-DMA input), `Reverb.cpp` + `ReverbResample.cpp`, `ADSR.cpp`,
  `RegTable.cpp`, `Dma.cpp` (plain DMA and ADMA).
  The per-ISA files are compiled once, with `/arch:AVX2`.
- **Not compiled:**
  - `spu2.cpp`: audio backend and VM glue. Its register and DMA entry points (`SPU2read/write`,
    `SPU2*DMA*Mem`, `SPU2interruptDMA4/7`), reset and `spu2Output` are reimplemented in `src/kzspu2.cpp`.
  - `spu2freeze.cpp`: savestates.
  - `Debug.cpp`, `Wavedump_wav.cpp`: only used in `PCSX2_DEVBUILD` builds.
- **Shims (`shim/`):**
  - `kzspu2_rename.h` is force-included first. It renames every symbol kzspu2 defines (SPU2 globals such as
    `Cores`, `_spu2mem`, `Cycles`; functions such as `TimeUpdate`, `SPU2write`; the SPU2 types; `isa_native`;
    and the IOP state below) to a kzspu2-private name.
  - `shim/include/` comes first on the include path. It **shadows** the IOP headers the SPU2 code includes with
    minimal versions: `R3000A.h` (`psxRegs.cycle`), `IopCounters.h` (counter 6, the SPU2 event), `IopHw.h` (DMA
    ch4/ch7 registers), `IopDma.h` (the three interrupt lines), `IopMem.h`, and empty `Config.h`, `SaveState.h`,
    `Host/AudioStream.h`. So no IOP, config or backend code is compiled.
  - `Kzspu2Shims.cpp` provides that state, and routes the SPU2 IRQ and DMA-end lines to the host callbacks.

**Symbol isolation.** `dumpbin /symbols` lists no externally defined kzspu2 symbol without a kzspu2 prefix. The
exceptions are STL/CRT COMDATs and inline `GSVector8i` members from the PCSX2 header (`load`, `mul16hrs`, ...). Those
are compiled with the same `/arch:AVX2` as kzgs, and the linker merges them. The game links kzspu2 together with kzgs,
kzvu and kzipu.

## API (`include/kzspu2.h`)

```cpp
struct KzSpu2Config {
    void* user;
    void (*irq)(void* user);                            // SPU2 IRQ (IOP INTC line 9)
    void (*dmaComplete)(void* user, int core);          // core 0 = IOP DMA ch4, core 1 = ch7 finished
    void (*samples)(void* user, const int16_t* stereo, uint32_t frames);  // optional tap (emulation thread)
    void (*log)(void* user, int level, const char* msg);
    uint32_t ringFrames = 16384;                        // output ring; when full, new frames are dropped (counted)
    bool dcFilter = true;                               // PCSX2's DC blocker on the final output
};
bool kzspu2Init(const KzSpu2Config&);  void kzspu2Shutdown();  void kzspu2Reset(uint64_t iopCycle);

void kzspu2Advance(uint64_t iopCycle);        // mix every whole sample up to iopCycle (768 IOP cycles per sample)
uint64_t kzspu2NextEventCycle();              // when DMA progress is next due (UINT64_MAX: nothing pending)

bool kzspu2HandlesAddress(uint32_t iopAddr);  // 0x1F900000-0x1F9007FF, any KSEG mirror
uint16_t kzspu2Read16(uint32_t iopAddr, uint64_t iopCycle);
void kzspu2Write16(uint32_t iopAddr, uint16_t value, uint64_t iopCycle);

void kzspu2DmaWrite(int core, uint16_t* iopRam, uint32_t halfwords, uint32_t madr, uint64_t iopCycle); // IOP -> SPU2
void kzspu2DmaRead (int core, uint16_t* iopRam, uint32_t halfwords, uint32_t madr, uint64_t iopCycle); // SPU2 -> IOP
bool kzspu2DmaBusy(int core);   uint32_t kzspu2DmaMadr(int core);

uint32_t kzspu2ReadSamples(int16_t* stereo, uint32_t frames);   // audio thread
uint32_t kzspu2AvailableFrames();  uint32_t kzspu2SkipFrames(uint32_t frames);
KzSpu2Status kzspu2GetStatus(bool resetPeak = false);          // frames mixed/dropped, IRQ/DMA counts, voices, peak
const uint16_t* kzspu2Memory();                                // 2 MB SPU2 RAM
```

- **Threading:** everything except the three ring-consumer calls runs on one thread, the one that runs the IOP. The
  ring is single-producer, single-consumer.
- **Callbacks:** they run synchronously inside the kzspu2 call that caused them. Queue them, and dispatch the guest
  handlers afterwards.
- **Time:** only PCSX2's model. Every call takes the current IOP cycle, and PCSX2's `TimeUpdate` mixes the samples
  that have come due:
  - plain DMA moves data in chunks and completes after about 24 cycles per halfword;
  - auto-DMA completes when its input has been played.
  - `kzspu2NextEventCycle()` tells an IOP scheduler that skips idle time not to skip past a DMA step.
- **DMA memory:** `iopRam` must stay valid until `dmaComplete`, because ADMA reads the source while it plays.

## Test (`test/kzspu2_test.cpp`)

Everything goes through registers and DMA, the way libsd drives the hardware:

1. A 1 kHz sine is encoded to SPU2 ADPCM by a brute-force encoder (5 filters x 13 shifts per block). It is a looping
   VAG sample of 12 blocks.
2. It is uploaded to 0x5000 by DMA ch4 (core 0) and to 0x6000 by ch7 (core 1). Checks:
   - both DMA-end callbacks fire;
   - each transfer takes 24.0 IOP cycles per halfword;
   - MADR ends at TADR;
   - the SPU2 RAM holds the bytes.
3. DMA reads on both cores return the same bytes.
4. Core 0 voice 0 plays at pitch 0x1000 (left only), and core 1 voice 5 at pitch 0x0800 (right only), for 1 s of IOP
   time. Core 0's output reaches the final mix through core 1's external input.
5. IRQ: IRQA inside the sample fires the SPU2 IRQ once. It stays latched until libsd's acknowledge (ATTR.IRQE off/on),
   then fires again.
6. Key off releases the voice.
7. ADMA: 170.7 ms of 16-bit PCM (3 kHz left, 750 Hz right) is streamed through core 0's input, as
   `sceSdBlockTrans` does.

Result: **PASS**.

| Check | Result |
|---|---|
| Left (core 0, pitch 0x1000) | 1000.00 Hz by zero crossings, 98.4% of the power at 1000 Hz, RMS 7131 |
| Right (core 1, pitch 0x0800) | 500.00 Hz, 98.4% at 500 Hz, RMS 7115 |
| ADMA | 2996.6 Hz / 748.3 Hz by zero crossings, RMS about 11600. DMA end after 161.7 ms of IOP time (the data lasts 170.7 ms) |

## Game integration

- **IOP side:** `ext/PS2Recomp` patch `patches/ps2recomp/0006-spu2-host-audio.patch` adds the hook header
  `ps2x/iop/iop_host_spu2.h`. With it, `IopMemory` routes the SPU2 registers and DMA ch4/ch7 to the host, and
  `IopEmulator::runCycles` advances the SPU2 and turns its requests into IOP interrupts 9, 0x24 and 0x28.
- **Host side:** `src/kz_audio.cpp` installs kzspu2 behind those hooks and plays the ring through an SDL3 audio
  stream, with gain = `KzConfig::masterVolume` / 100.
- **Pacing:** the runtime's IOP time runs up to 7.5% faster than real time. So kz_audio gives the SPU2 its own
  clock: IOP time scaled by a factor of at most 1 that tracks wall time. In the measured runs the factor was 0.930.
- **Environment variables:** `KZ_AUDIO=off`, `KZ_AUDIO_WAV=<file>`, `KZ_AUDIO_DEVICE`, `KZ_AUDIO_PACE`,
  `KZ_AUDIO_TRACE` (see `src/kz_audio.h`).

The game runs its real drivers (`LIBSD.IRX`, `SDRDRV.IRX`, `PSOUND_R.IRX`). Two IOP emulator bugs kept them silent,
and patch 0006 fixes both:

- `sceSifGetOtherData` was a no-op. PSOUND_R pulls every sound bank from EE RAM with it, 4 KB at a time.
- A `WaitEventFlag` outside an IOP thread returned immediately. The result was a stale libsd "transfer done" flag,
  and after the first 4 KB every `sceSdVoiceTrans` was refused as busy.

Measured with `run_headless.ps1 -Config audio -Seconds 90 -Extra "--no-launcher"`, `KZ_IPU=off` (videos skipped),
`KZ_AUDIO_WAV=work\audio_final\kz_audio.wav`:

- **Boot:** libsd init does 1 + 97 DMAs, then the three front-end banks (0x5170 + 0x1AB0 + 0x39F0 bytes) upload in
  12 DMAs. Output is silent until the main menu appears, at about 26 s.
- **Menu music:** a stereo ADPCM stream on 2 voices of core 0, refilled by about 7 DMAs/s on each channel. RMS per
  5 s is -18 to -23 dBFS (-34 dBFS in one quiet passage), and peaks reach 23654. Output is 1.000x real time.
- **Analysis:** the spectral flatness median is 0.050, which is music rather than noise (white noise is about 0.56).
  There were 25 sample steps over 4000 in 60 s, so there are no clicks at buffer boundaries. L/R correlation is
  0.41, which is real stereo.

## License

GPL-3.0+. kzspu2 consists of PCSX2 code plus GPL shims. Anything that links kzspu2 is a GPL-3 derivative work.

## Known gaps

- **Savestates:** not supported.
- **PS1 mode** (the SPU at 0x1F801C00): not supported.
- **SPDIF bitstream bypass and CDDA input:** not supported. They are PCSX2's paths, and no PS2 game output is expected
  to depend on them.
- **Timing:** PCSX2's model only. For example, its DMA takes 24 cycles per halfword.
- **One deviation:** on reset, kzspu2 invalidates the ADPCM decode cache. PCSX2 keeps it, although reset clears the
  memory it caches.
- **MMIX:** as in PCSX2, core 0's output reaches the mix only after the game writes core 1's MMIX. libsd's
  `sceSdInit` does this.
- **Not verified in game:**
  - reverb: compiled and active, but no in-game A/B comparison;
  - SFX, since the headless runs have no input;
  - audio during FMVs;
  - the game's auto-DMA path;
  - audible playback on a real output device: the verification runs were headless, WAV only.
