# kzipu: PCSX2's IPU as a static library

kzipu compiles PCSX2's IPU (the PS2's MPEG-2 macroblock decoder unit) into one static library, `kzipu.lib`. It also
contains PCSX2's IPU_TO / IPU_FROM DMA logic (`IPUdma.cpp`), so a host only has to forward register accesses, translate
DMA addresses and deliver two kinds of interrupt.

kzipu exists because the recompiled game's runtime (PS2Recomp) has no IPU. Its register block at 0x10002000 is a stub,
and DMA channels 3/4 are not processed, so Killzone's FMV player waits forever for decoded data.

- PCSX2 source: `ext/pcsx2`, a depth-1 clone of PCSX2 master, verified against commit `646df006a4`. kzipu does not
  modify anything in `ext/pcsx2`.
- No external dependencies. kzipu does not even compile `pcsx2/common` (see "What is compiled").
- The test executable uses the FFmpeg build under `build/RelWithDebInfo/ThirdParty/ffmpeg-prefix/...` as the reference
  decoder. Only the test links FFmpeg.

## Build

The only supported toolchain is MSVC x64 (VS 2026 / 14.51), using CMake 3.24 or newer with Ninja. Keep `-j` at 4 on
this machine.

```sh
cmd //c "tools\scripts\vsenv.bat cmake -S ext\kzipu -B ext\kzipu\build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo"
cmd //c "tools\scripts\vsenv.bat cmake --build ext\kzipu\build -j 4"
ext/kzipu/build/kzipu_test.exe          # exit code 0 = pass; --iso <path> --out <dir> --videos N --frames N --es-mb N
```

- A clean build is 8 translation units for the library and 1 for the test. It takes a few seconds.
- The result is `kzipu.lib`: about 1.7 MB with debug info (RelWithDebInfo), 0.7 MB in Release.
- Both RelWithDebInfo (`build/`) and Release (`build-release/`) were built, and the test passes in both.

Using it from the game's CMake:

```cmake
add_subdirectory(ext/kzipu)          # KZIPU_BUILD_TEST defaults to OFF when not top-level
target_link_libraries(kz_port PRIVATE kzipu)
```

A consumer inherits only the public include directory. All compile flags are private: C++20, `/permissive-`,
`/Zc:preprocessor`, `/arch:AVX2` (the same as kzgs/kzvu), PCSX2's defines and the forced include.

CMake options:

| Option | Default | Purpose |
|---|---|---|
| `KZIPU_PCSX2_DIR` | `ext/pcsx2` | PCSX2 source tree |
| `KZIPU_FFMPEG_DIR` | `build/RelWithDebInfo/ThirdParty/ffmpeg-prefix/src/ffmpeg_external` | FFmpeg for `kzipu_test`: headers in `include/`, import libs and DLLs in `bin/`. The DLLs are copied next to the test. |
| `KZIPU_KZVU_LIB`, `KZIPU_KZGS_LIB` | `ext/kzvu/build/kzvu.lib`, `ext/kzgs/build/kzgs.lib` | When both exist, `kzipu_linkcheck_ab` / `_ba` link all three libraries into one exe |
| `KZIPU_BUILD_TEST` | on when top-level | Builds `kzipu_test` and the link checks |

### What is compiled

- **PCSX2 IPU code, unmodified:**
  - `IPU/IPU.cpp`: registers and the command dispatcher
  - `IPU/IPU_Fifo.cpp`: the 8-qword input and output FIFOs
  - `IPU/IPUdma.cpp`: the IPU_TO chain DMA and the IPU_FROM normal DMA
  - `IPU/IPU_MultiISA.cpp`: the decoder (IDEC, BDEC, VDEC, FDEC, SETIQ, SETVQ, CSC, PACK), VLC, IDCT
  - `IPU/IPUdither.cpp`, `IPU/yuv2rgb.cpp`: RGB16 dither and YCbCr to RGB32

  The per-ISA files are compiled once, as the single `isa_native` build, with `/arch:AVX2`.
- **Shims (`shim/`):**
  - `kzipu_rename.h` is force-included first. It renames every symbol kzipu defines to a kzipu-private name:
    - emulator state: `eeHw`, `_cpuRegistersPack`
    - the DMAC/INTC helpers: `hwIntcIrq`, `hwDmacIrq`, `dmaGetAddr`, `hwDmacSrcChain`, `CPU_INT`, ...
    - all IPU globals and functions: `ipu_fifo`, `g_BP`, `decoder`, `ipuRead32`, `IPU1dma`, `FMVstarted`, the
      `isa_native` namespace, ...
    - the three `pcsx2/common` entry points the IPU uses: `pxOnAssertFail`, `StringUtil::StdStringFromFormat`,
      `SaveStateBase`
  - `kzipu_prefix.h` pre-includes the PCSX2 headers. It then turns `IPU_LOG`/`DMA_LOG` into no-ops and redirects
    `Console`/`DevCon` to the host log callback.
  - `KzipuShims.cpp` provides:
    - the EE hardware page (`eeHw`)
    - a 3-event slice of PCSX2's EE event scheduler: `CPU_INT`, and `cpuRegs.interrupt`/`sCycle`/`eCycle`/`cycle`
      for IPU_FROM, IPU_TO and IPU_PROCESS
    - `DmaExec` for the IPU channels (CHCR write semantics, QWC=0 means 0x10000, queuing while the DMAC is disabled)
    - chain tag handling copied from PCSX2's `Hw.cpp`
    - DMA address translation through the host callback
    - the INTC/DMAC interrupt lines

**Symbol isolation.** `dumpbin` lists no externally defined kzipu symbol without a kzipu prefix, apart from inline
COMDAT member functions from PCSX2 headers (`tIPU_BP::Advance`, `tDMA_CHCR::set`, ...). The linker merges those.
`kzipu_linkcheck_ab` and `kzipu_linkcheck_ba` link kzipu + kzvu + kzgs in both orders, and both link and run cleanly.

## API (`include/kzipu.h`)

```cpp
struct KzipuConfig {
    void* user;
    void (*intc)(void* user);                                       // INTC bit 8 (INTC_IPU) raised
    void (*dmacIrq)(void* user, int channel);                       // D_STAT CIS: 3 IPU_FROM end, 4 IPU_TO end, 15 BEIS
    uint8_t* (*dmaPtr)(void* user, uint32_t addr, uint32_t bytes, bool write);  // DMA address -> host pointer
    void (*log)(void* user, int level, const char* msg);           // optional
    bool hostDrainsOutput;                                          // false: ch3 DMA drains the output FIFO
    bool mapIdecQsc = true;                                         // see "IDEC quantiser scale"
};
bool kzipuInit(const KzipuConfig&);  void kzipuShutdown();  void kzipuReset();
bool kzipuHandlesAddress(uint32_t addr);   // 0x10002000-0x10002FFF, 0x10007000-0x1000701F, 0x1000B000-B0FF, 0x1000B400-B4FF

uint32_t kzipuReadReg32(uint32_t addr);   uint64_t kzipuReadReg64(uint32_t addr);   // IPU regs + D3/D4 regs
void kzipuWriteReg32(uint32_t addr, uint32_t v);   void kzipuWriteReg64(uint32_t addr, uint64_t v);

uint32_t kzipuWriteInFifo(const void* qwords, uint32_t count);  // -> qwords accepted (room = 8 - IFC)
uint32_t kzipuReadOutFifo(void* qwords, uint32_t count);        // -> qwords read (<= OFC)

void kzipuSetDmacEnabled(bool);          // D_CTRL.DMAE && !D_ENABLER.CPND
uint32_t kzipuRun(uint32_t eeCycles);    // advance the virtual EE clock, run due events (PCSX2 timing)
uint32_t kzipuStep(uint32_t maxEvents = 100000);   // run pending events now, regardless of due time
KzipuStatus kzipuGetStatus();            // busy, command, IFC/OFC, BP, waiting-for-input/output,
                                         // DMA request lines, ch3/ch4 active, pending events, cycle, IRQ counters
```

### Registers, as the hardware exposes them

| Address | Access | Meaning |
|---|---|---|
| `0x10002000` IPU_CMD | write 32/64 | starts a command (bits 31:28 = BCLR/IDEC/BDEC/VDEC/FDEC/SETIQ/SETVQ/CSC/PACK/SETTH) |
| | read 32 | DATA: the FDEC/VDEC result. After other commands, the next 32 bits of the stream. |
| | read 64 | DATA, plus BUSY in bit 63. `0x10002004` read 32 = BUSY word. |
| `0x10002010` IPU_CTRL | read/write 32 | IFC, OFC, CBP, ECD, SCD, IDP, AS, IVF, QST, MP1, PCT, RST (bit 30 resets), BUSY (bit 31) |
| `0x10002020` IPU_BP | read 32 | BP \| IFC << 8 \| FP << 16 |
| `0x10002030` IPU_TOP | read 32 / 64 | next 32 bits of the stream; bit 63 = BUSY |
| `0x10007000` IPU_out_FIFO | read 128 | `kzipuReadOutFifo(dst, 1)` |
| `0x10007010` IPU_in_FIFO | write 128 | `kzipuWriteInFifo(src, 1)` |
| `0x1000B000`+ D3 (IPU_FROM) | CHCR/MADR/QWC/TADR | normal mode only, as on hardware |
| `0x1000B400`+ D4 (IPU_TO) | CHCR/MADR/QWC/TADR | normal and source-chain mode (REFE/CNT/NEXT/REF/REFS/END) |

Writing a CHCR with STR=1 starts the DMA, as in PCSX2's `DmaExec`:
- While a channel is running, only STR=0 (force stop) is accepted.
- MOD=3 is treated as chain.
- A normal-mode start with QWC=0 transfers 0x10000 qwords.

A write to IPU_BP or IPU_TOP lands in the register block, as it does in PCSX2.

### Two data paths

1. **Built-in DMA (default, recommended).** The game programs ch3/ch4 through `kzipuWriteReg32`. kzipu runs PCSX2's
   `dmaIPU0/dmaIPU1/IPU0dma/IPU1dma` with the same FIFO-driven pacing, reading and writing guest memory through
   `dmaPtr`:
   - IPU_TO refills when the IPU drains its FIFO.
   - IPU_FROM moves each macroblock as soon as it is in the output FIFO.

   Chain tags are handled as in PCSX2: `hwDmacSrcChain`, TIE+IRQ, and TADR bookkeeping.

   `dmaPtr` gets an exact byte count: 16 for a tag, and up to 128 (8 qwords, the FIFO depth) for data. Addresses with
   bit 31 set are scratchpad (`addr & 0x3FF0`). Everything else is RDRAM (`addr & 0x1FFFFFF0`). Return nullptr for a
   bus error; kzipu then raises BEIS through `dmacIrq(15)`.
2. **Host FIFOs.** A host with its own DMA engine sets `hostDrainsOutput = true` and moves data with
   `kzipuWriteInFifo` / `kzipuReadOutFifo`. It uses `KzipuStatus::inputRequest` / `outputRequest` as the DMA request
   lines.

   PCSX2's decoder only emits a macroblock while the IPU_FROM channel is running. In this mode, kzipu holds ch3
   "armed" internally and ignores the ch3 registers.

   Reading the output FIFO wakes a decoder that waits for room. PCSX2 only does that from its IPU_FROM DMA; kzipu adds
   it for direct reads, which also makes EE reads of `0x10007000` work.

### Time

PCSX2 runs IPU work as EE events: `CPU_INT(IPU_PROCESS/DMAC_TO_IPU/DMAC_FROM_IPU, cycles)`. kzipu keeps those events
and gives them a private 64-bit cycle counter. There are two ways to advance it:

- `kzipuRun(eeCycles)` advances the clock and fires the events that come due in order. This is PCSX2's timing, e.g.
  IDEC/BDEC wait 64 cycles per macroblock before output.
- `kzipuStep()` fires everything pending immediately, in due order, until the IPU and its DMA are idle or waiting for
  the guest. This suits a recompiled game that has no cycle count.

The test decodes identical pictures in both modes.

Nothing runs by itself. Command execution that needs no data happens inside the `IPU_CMD` write: BCLR, SETTH,
VDEC/FDEC/SETIQ/SETVQ/CSC/PACK when their data is in the FIFO. Everything else happens in `kzipuRun` / `kzipuStep`.

### Threading

- There is one IPU per process.
- Call kzipu from one host thread (the game's EE thread). There is no internal locking.
- Callbacks run synchronously inside the kzipu call that caused them. They may call kzipu again (e.g. re-arm ch3 from
  `dmacIrq`), because STR is already cleared when `dmacIrq` fires. The simpler pattern is to queue the interrupt and
  dispatch guest handlers later, which PS2Recomp does anyway.

## Integration sketch for PS2Recomp's `PS2Memory` (not applied; `ext/PS2Recomp` belongs to the main build)

```cpp
// init (next to kzvuInit/kzgsOpen)
KzipuConfig ic;
ic.user = &m_memory;
ic.dmaPtr = [](void* u, uint32_t a, uint32_t n, bool) -> uint8_t* {
    auto* m = static_cast<PS2Memory*>(u);
    if (a & 0x80000000u) return m->getScratchpad() + (a & 0x3FF0u);
    a &= 0x1FFFFFF0u;  return (a + n <= PS2_RAM_SIZE) ? m->getRDRAM() + a : nullptr;
};
ic.intc    = [](void* u) { static_cast<PS2Memory*>(u)->queueIntcCause(8); };          // new: INTC_IPU
ic.dmacIrq = [](void* u, int ch) { static_cast<PS2Memory*>(u)->raiseDmacCause(ch); }; // new: public wrapper around
                                                                                       // completeDmacChannel's D_STAT
kzipuInit(ic);                                                                        // + queueCompletedDmacCause
```

Route these in `ps2_memory.cpp`:

| Where | What |
|---|---|
| `writeIORegister`, the `0x10002000..0x10002030` stub (about line 1213) | `kzipuWriteReg32(addr, value); kzipuStep(); return true;` |
| `readIORegister`, the IPU stub (about line 2350) | `kzipuStep(); return kzipuReadReg32(addr);` |
| `write64` / `read64` for `0x10002000` | `kzipuWriteReg64` / `kzipuReadReg64`, so IPU_CMD/IPU_TOP BUSY (bit 63) is read atomically |
| `writeIORegister`, **before** the generic `0x10008000..0x1000F000` CHCR block (about line 1325) | if `kzipuHandlesAddress(addr)` (0x1000B000-0x1000B4FF): `kzipuWriteReg32(addr, value); kzipuStep(); return true;` |
| `readIORegister`, **before** the generic CHCR read (about line 2392, which clears STR) | `kzipuStep(); return kzipuReadReg32(addr);` |
| `write128` / `read128`, next to `VIF0_FIFO`/`VIF1_FIFO` | `0x10007010`: `kzipuWriteInFifo(&value, 1)`; `0x10007000`: `kzipuReadOutFifo(&out, 1)` |
| writes to `D_CTRL` (0x1000E000) and `D_ENABLEW` (0x1000F590) | `kzipuSetDmacEnabled((D_CTRL & 1) && !(D_ENABLEW & 0x10000))` |
| D_STAT reads and the runtime's periodic interrupt/vblank poll | `kzipuStep()` (or `kzipuRun(elapsedEeCycles)` if the runtime tracks cycles) |

Interrupt delivery:
- `raiseDmacCause(ch)` sets D_STAT bit `ch` (and bit 31 per the CIM mask), as `completeDmacChannel` does today, then
  calls `queueCompletedDmacCause(ch)`. `PS2Runtime::drainCompletedDmacHandlers` then runs the game's DMAC handlers.
- `queueIntcCause(8)` needs the matching INTC path: `eeScheduler().dispatchIrq(false, 8)` from the same drain point.
- Also stop `Kernel/Stubs/IPU.cpp` from writing its fake FIFO data.

What the recompiled game does, from `generated/` (read-only observation):
- Killzone's player code (`FUN_002e0008` and its `entry_002e0xxx` pieces) does the following:
  - resets the IPU (`IPU_CTRL = 0x40000000`) and issues BCLR
  - issues FDEC (`0x40000008`) and reads IPU_CMD
  - polls IPU_CTRL (`0x2E0470`, `0x2E0498`, `0x2E04D0`)
  - programs ch3 (`D3_MADR`, then `D3_CHCR = 0x100`)
  - stops ch4 under `D_ENABLEW = 0x10000`
- The functions at `0x3CC4D8`-`0x3D6878` access the same registers. They are probably the SDK's MPEG library; that has
  not been verified.
- All of these accesses are covered by the table above.

## Test (`test/kzipu_test.cpp`)

The test runs on the real disc image, `C:\Users\drakolord\Downloads\Killzone (USA)\Killzone (USA).iso`.

1. It reads the ISO9660 root and scans `FILES*.DAT` for PSS videos. A video starts with an MPEG-2 pack header
   directly followed by a system header. There are 66 videos across `FILES.DAT`-`FILES03.DAT`, alternating 512x448
   (NTSC) and 512x512 (PAL) versions.
2. It demuxes video PES 0xE0 from the first 2 videos. That gives about 6.7 MB of ES each, and the first 20 I-frames are
   decoded. The ES is cut at a picture boundary and ended with a sequence_end_code.
3. **IDEC, built-in DMA.** The ES is placed in emulated RDRAM behind the MPEG-2 default intra matrix and a flat
   non-intra matrix. It is fed through ch4 in chain mode (REF tags of 0x8000 qwords, REFE last). The whole stream is
   parsed *through the IPU*:
   - FDEC reads start codes and header fields.
   - SETIQ loads the default matrices and any matrix the sequence header carries, straight from the bitstream.
   - IPU_CTRL is set from the picture coding extension.
   - VDEC decodes each slice's first macroblock_address_increment.
   - IDEC decodes each slice, with RGB32 output, CSC included.
   - The output leaves through ch3 in normal mode, re-armed on every ch3 DMA-end interrupt.
   - P/B pictures are skipped with FDEC.
4. **Reference.** FFmpeg (libavcodec mpeg2video) decodes the same ES. Its I-frames are converted to RGB with the IPU's
   own integer BT.601 CSC. The k-th IPU I-frame is compared with FFmpeg's k-th I-frame. **Pass: every I-frame is at
   least 30 dB PSNR.**
5. **IDEC, host FIFOs.** Video 0 is decoded again without DMA (`hostDrainsOutput`, `kzipuWriteInFifo` /
   `kzipuReadOutFifo`). The pictures must be bit-identical to step 3.
6. **IDEC, timed.** Video 0 is decoded again, advancing only with `kzipuRun(256)`. The pictures must be bit-identical.
   One more IDEC run uses `mapIdecQsc = false` (PCSX2 unchanged) and is only reported.
7. **BDEC.** Video 0 is decoded per macroblock with BDEC. VDEC parses the macroblock address and type, and FDEC reads
   quantiser_scale_code/dct_type. The RAW16 YCbCr output is compared with FFmpeg's planes (Y/Cb/Cr PSNR at least
   30 dB).
8. **CSC.** Each BDEC picture is converted to RAW8 and sent through the **CSC** command: ch4 in normal mode in, ch3
   out. It must be at least 30 dB against FFmpeg and bit-identical to the IDEC picture. The test also requires one ch4
   DMA-end interrupt per run.
9. **Output images.** For the most detailed I-frame of each video, the test writes `test/out/videoN_iframeK_ipu_ffmpeg_diffx8.png`
   with three panels: IPU | FFmpeg | |difference| x8.

Result (RelWithDebInfo and Release): **PASS**, 20 I-frames per video.

| Pass | Result |
|---|---|
| Video 0 (512x448), IDEC vs FFmpeg | RGB PSNR 55.3-65 dB. Max per-channel difference 4. 0-0.8% of pixels differ by more than 2. The 3 black frames are identical. |
| Video 1 (512x512), IDEC vs FFmpeg | RGB PSNR 55.6-59 dB. Max difference 4. |
| Video 0, BDEC vs FFmpeg | Y 60.6-68 dB, Cb 61.7-73 dB, Cr 62.1-73 dB. |
| Video 0, CSC(BDEC) | Bit-identical to IDEC in 20/20 I-frames. |
| Host-FIFO mode | Bit-identical to DMA mode, 20/20. |
| Timed mode | Bit-identical, 20/20. 10.9 M virtual EE cycles. |
| `mapIdecQsc = false` | 3 of 20 I-frames differ, with a max difference of 20 against FFmpeg (see Known gaps). |
| Video 0 run | 2.39 M IPU commands (560 IDEC, 17,920 BDEC in the BDEC pass), 140 ch3 transfers with 140 DMA-end interrupts, about 70-90 ms. |

A wider manual run (`--videos 8 --frames 20 --es-mb 10`) passes. Minimum RGB PSNR per video is 54.6-55.6 dB across 8
videos, 160 I-frames in total.

The remaining difference is IDCT rounding: PCSX2 uses the mpeg2dec integer IDCT, FFmpeg its own, and PCSX2 does no MPEG-2
mismatch control. The images are `test/out/video0_iframe14_ipu_ffmpeg_diffx8.png` and
`test/out/video1_iframe16_ipu_ffmpeg_diffx8.png`; `test/out/` is not committed.

**Stream facts measured by the test.** Killzone's FMVs were encoded by TMPGEnc 2.521 as MPEG-2 frame pictures with:
- the default intra matrix and a custom non-intra matrix
- `q_scale_type=1`, `intra_vlc_format=1`
- `frame_pred_frame_dct=1`, `alternate_scan=0`, `intra_dc_precision=0`
- slice quantiser_scale_code 1-9, mostly 1-4

## License

GPL-3.0+. kzipu consists of PCSX2 code (the IPU decoder files are GPL-2.0+/GPL-3.0+) plus GPL shims. Anything that
links kzipu is a GPL-3 derivative work.

## Known gaps

- **IDEC quantiser scale (deliberate deviation from PCSX2, on by default).** PCSX2's `ipuIDEC` stores the command's
  QSC field as `quantizer_scale` unmapped; `ipuBDEC` maps it through `IPU_CTRL.QST`. As a result, PCSX2's IDEC
  dequantizes the macroblocks before the first macroblock_quant with the wrong scale: half for `q_scale_type=0`, and
  wrong for codes >= 9 with `q_scale_type=1`.
  - With `mapIdecQsc = true` (default), kzipu corrects `decoder.quantizer_scale` right after the IDEC command write,
    before any decoding. IDEC and CSC(BDEC) then agree bit for bit.
  - Without the fix, 3 of the first 20 I-frames of video 0 differ, with a max per-channel error of 20 against FFmpeg
    instead of 4. In the 8-video sweep, the minimum PSNR of 3 videos drops by 1.5-1.9 dB.
  - That real hardware maps IDEC's QSC like BDEC's is an inference, from the field having the same name and meaning in
    both commands and from MPEG-2 conformance. It was not checked on a PS2. `mapIdecQsc = false` restores PCSX2's
    behaviour.
- **Not exercised by the test:**
  - SETVQ, PACK, SETTH, RGB16/dither output, VDEC tables 2/3 (motion code, DMV)
  - MPEG-1 streams and field pictures
  - `kzipuSetDmacEnabled(false)` queuing

  The code paths are PCSX2's. P/B pictures need motion compensation on the EE, which the IPU does not do; only their
  BDEC part would pass through kzipu.
- **Stall control is not supported.** With D_CTRL.STS = fromIPU, PCSX2's IPU_FROM updates D_STADR. kzipu never sees
  the host's D_CTRL.STS, so this never happens.
- **Not provided:** savestates (the freeze functions are compiled but stubbed), and PCSX2's game-specific DMA-busy
  hack.
- **Timing:** only PCSX2's model. Its IPU_PROCESS delays are tuned for games (e.g. "64 cycles, Myst 3"), not measured
  hardware.
- **Configurations:** MSVC x64 Release/RelWithDebInfo only. Debug is untested.
- **Game integration:** not done. The routing above is a sketch; wiring it into `ps2_memory.cpp`, and running the game's
  own player through it, is the next step.
