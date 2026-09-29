# kzvu: PCSX2's VU1 as a static library

kzvu compiles PCSX2's VU1 execution into one static library, `kzvu.lib`. It contains two cores:

- microVU, PCSX2's x86-64 recompiler for VU microcode. This is the default.
- PCSX2's VU interpreter, as a fallback. It also serves as a second reference in the tests.

kzvu is meant to replace PS2Recomp's cycle-accurate `VU1Interpreter`. Profiling put that interpreter at about 70% of
game-thread time. The rest of the emulator is replaced by small shims in `shim/`, so kzvu needs no EE, VIF, GIF unit,
MTGS or MTVU.

- PCSX2 source is `ext/pcsx2`, a depth-1 clone of PCSX2 master. This build was verified against commit `646df006a4`.
  kzvu does not modify anything in `ext/pcsx2`.
- kzvu has no external dependencies. It compiles the small `pcsx2/common` slice it needs, plus the x86 emitter and
  fmt, from `ext/pcsx2` itself. It does not use the `ext/kzgs/deps` bundle.

## Build

The only supported toolchain is MSVC x64 (VS 2026 / 14.51), using CMake 3.24 or newer with Ninja. Keep `-j` at 4 on
this machine.

```sh
cmd //c "tools\scripts\vsenv.bat cmake -S ext\kzvu -B ext\kzvu\build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo"
cmd //c "tools\scripts\vsenv.bat cmake --build ext\kzvu\build -j 4"
ext/kzvu/build/kzvu_test.exe          # exit code 0 = pass; --no-bench, --bench-runs N, --no-ibithack
```

Both RelWithDebInfo and Release are built and tested. A clean build is about 30 translation units and takes about 12 s at `-j 4`.
The result is `kzvu.lib`, about 12 MB with debug info.

Using it from the game's CMake:

```cmake
add_subdirectory(ext/kzvu)          # KZVU_BUILD_TEST defaults to OFF when not top-level
target_link_libraries(kz_port PRIVATE kzvu)
```

The public include directory is the only thing consumers inherit, plus `pathcch.lib`. All compile flags are private:
C++20, `/permissive-`, `/Zc:preprocessor`, `/arch:AVX2` (the same as kzgs), PCSX2's defines and the forced includes.

CMake options:

| Option | Default | Purpose |
|---|---|---|
| `KZVU_PCSX2_DIR` | `ext/pcsx2` | Location of the PCSX2 source tree |
| `KZVU_PS2RECOMP_DIR` | `ext/PS2Recomp` | Its `VU1Interpreter` is compiled into `kzvu_test` only, as a differential reference |
| `KZVU_KZGS_LIB` | `ext/kzgs/build/kzgs.lib` | If this file exists, the `kzvu_linkcheck_ab`/`_ba` targets link kzvu and kzgs together |
| `KZVU_BUILD_TEST` | on when top-level | Builds `kzvu_test` and the link checks |

### What is compiled

- **PCSX2 VU code, unmodified:**
  - `x86/microVU.cpp`, which includes all `microVU_*.inl` files as one translation unit
  - `VUops.cpp`, `VUflags.cpp` and `VU1microInterp.cpp`
- **pcsx2/common:**
  - the x86 emitter (`common/emitter/*.cpp`)
  - `Console`, `Assertions`, `Error`, `FileSystem`, `Perf`, `StringUtil`, `SmallString` and `Timer`
  - `fmt`
- **Shims in `shim/`:**
  - `kzvu_rename.h` is force-included in every translation unit. It renames every emulator global kzvu defines, such
    as `eeHw`, `cpuRegs`, `vuRegs`, `g_cpu`, `SysMemory`, `Host::` and `CrashHandler::`, to a kzvu-private name.
  - `kzvu_prefix.h` is force-included in the VU sources. It first includes all the PCSX2 headers those sources use,
    then redirects three names by macro:
    - `gifUnit` goes to `kzvu_gifUnit`, which collects XGKICK packets for the host callback.
    - `EmuConfig` goes to `kzvu_EmuConfig`, which holds only the VU settings.
    - `vu1Thread` goes to a stub, and `THREAD_VU1` is forced to `false`, so MTVU is compiled out.
  - `KzvuShims.cpp` defines:
    - the emulator state kzvu owns
    - the GIF PATH1 collector
    - the JIT code memory (`SysMemory::GetCodePtr`)
    - `hwIntcIrq`, `CPU_INT`
    - CPU feature detection
  - `KzvuEeRecStubs.cpp` holds the EE-recompiler symbols that `microVU_Macro.inl` references. That file is the COP2
    macro-mode recompiler, which is part of the microVU translation unit. kzvu never runs it, and every stub fails
    loudly if it is reached.

### Linking next to kzgs

Every symbol kzvu defines for its own emulator state has a kzvu-private name, so it cannot collide with kzgs' stubs.
The `pcsx2/common` objects are the exception: both libraries compile them from the same files, so each copy defines
the same symbol set, and the linker takes whichever copy it finds first.

This is verified in two ways:
- `kzvu_linkcheck_ab` (`kzvu.lib` before `kzgs.lib`) and `kzvu_linkcheck_ba` (the reverse) both link and run without
  duplicate-symbol errors.
- A `dumpbin` comparison of kzvu's defined externals against `ps2_runtime.lib`, `kz_port.lib`, `imgui.lib` and
  `ps2_iop.lib` finds only CRT and STL inline functions in common (COMDAT, which the linker accepts).

One overlap needs care. kzvu contains PCSX2's fmt 12.2 (`fmt::v12`), and PS2Recomp links fmt 12.1 (also `fmt::v12`).
kzgs already has exactly the same overlap in today's game build, and kzvu's copy is byte-for-byte the same source as
kzgs'.

## API (`include/kzvu.h`)

```cpp
bool kzvuInit(const KzvuConfig& cfg, std::string* err = nullptr);  // 64 MB code cache, reset, apply cfg
void kzvuShutdown();
void kzvuSetConfig(const KzvuConfig& cfg);   // runtime changes; codegen-relevant ones flush the JIT cache
void kzvuReset();                            // VU1 registers + pipeline state + JIT cache (memory untouched)

uint8_t* kzvuCodeMem();                      // 16 KB VU1 micro memory (kzvu-owned, fixed address)
uint8_t* kzvuDataMem();                      // 16 KB VU1 data memory  (kzvu-owned, fixed address)
void kzvuWriteMicro(uint32_t offset, const void* src, uint32_t size);  // MPG: copy + invalidate if changed
void kzvuMicroWritten(uint32_t offset, uint32_t size);                 // after writing kzvuCodeMem() directly

void kzvuSetTop(uint32_t top, uint32_t itop);  // what XTOP/XITOP read
void kzvuSetFBRST(uint32_t fbrst);             // VU0 FBRST; DE1 (bit 10) / TE1 (bit 11)

uint32_t kzvuExecute(uint32_t startPcBytes, uint32_t maxCycles);  // MSCAL; 0xFFFFFFFF = MSCNT (continue at TPC)
uint32_t kzvuContinue(uint32_t maxCycles);   // resume a program that ran out of budget
bool kzvuRunning();                          // VBS1
uint32_t kzvuVpuStat();                      // VPU_STAT VU1 bits: 0x100 busy, 0x200 D stop, 0x400 T stop
bool kzvuTakeInterrupt();                    // INTC_VU1 raised (T/D stop) since last call
uint64_t kzvuCycles();  uint64_t kzvuXgkickCount();

uint32_t kzvuGetVI(int);  void kzvuSetVI(int, uint32_t);  // 0-15, 16 status, 17 MAC, 18 clip, 20 R, 21 I, 22 Q, 23 P, 26 TPC
void kzvuGetVF(int, uint32_t[4]);  void kzvuSetVF(int, const uint32_t[4]);
void kzvuGetACC(uint32_t[4]);  void kzvuSetACC(const uint32_t[4]);
```

`KzvuConfig` has these settings:
- `useJit`: microVU (default) or the interpreter.
- `clampMode` (0-3): PCSX2's `vuClampMode`. Killzone's GameIndex entry uses 0, which is the default.
- `iBitHack`: default on, as in Killzone's GameIndex entry.
- `flagHack`: PCSX2's "mVU flag hack", default on as in PCSX2.
- `xgkickHack`: default off.
- `xgkick` + `xgkickUser`: the XGKICK callback.

### Execution

`kzvuExecute(pc, budget)` is the equivalent of PCSX2's `vu1ExecMicro`:
- If a program is still running, it first runs that program to completion, as PCSX2 does.
- It then sets VBS1 and starts at `pc`.
- It runs until the E bit plus its delay slot, or until at least `budget` VU cycles have passed. The recompiler only
  checks the budget at block ends.
- It returns the VU cycles used.

If `kzvuRunning()` is still true afterwards, `kzvuContinue(budget)` resumes the program with its pipeline state
intact. The test runs a 500-iteration loop in 64-cycle slices and gets a bit-identical final state to a single call.

Passing `startPcBytes = 0xFFFFFFFF` means MSCNT: start at the current TPC. Use it after an E bit, or after a T-bit
stop.

MXCSR handling:
- Inside a run, MXCSR is set to PCSX2's VU value: round toward zero, DAZ and FTZ.
- The caller's MXCSR is restored on return.
- The XGKICK callback runs with the caller's MXCSR, not the VU's.

### XGKICK

The callback receives one complete GIF packet per call: every tag up to and including the one with EOP. It is called
synchronously, from inside `kzvuExecute`/`kzvuContinue`.

- A packet that wraps past qword 0x3FF is delivered already unwrapped into one contiguous buffer.
- `startQw` is the VU1 data address the packet was read from.
- The buffer is only valid during the call.

Timing follows PCSX2:
- **microVU without XgKickHack:** the whole packet is sent after the instruction that follows the XGKICK, or at the
  end of the program.
- **Interpreter:** data is sent in cycle-timed chunks. kzvu reassembles the chunks and delivers only complete packets.

In PS2Recomp terms, the callback is where `m_memory.submitGifPacket(GifPathId::Path1, data, size)` goes.

## Memory ownership

kzvu owns VU1 micro memory and data memory, 16 KB each, 64-byte aligned. Both are static arrays inside the executable
image. They must be, for two reasons:
- microVU encodes constant VU-memory addresses (for example `LQ vf1, 0x10(vi0)`) as 32-bit displacements from its text
  pointer, `&cpuRegs.GPR.r[9]`, which is also inside the image.
- Heap buffers can sit more than 2 GB away from the image, and those displacements cannot reach them.

The host therefore has to use kzvu's buffers. PS2Recomp's `PS2Memory` currently allocates `m_vu1Code` and `m_vu1Data`
with `new[]` in `ps2_memory.cpp:378-379`. The simplest integration is:
- point those two pointers at `kzvuCodeMem()` and `kzvuDataMem()`
- don't `delete[]` them

VIF1 UNPACK, MPG and the EE's mapped access at `0x1100C000`/`0x11008000` then read and write kzvu's memory directly, and
no copies are needed.

## JIT invalidation

microVU caches compiled programs by content. Each compiled program remembers which micro-memory ranges it was compiled
from.

- `kzvuWriteMicro()` and `kzvuMicroWritten()` do the equivalent of PCSX2's `CpuVU1->Clear()`. It is cheap: it only
  marks the current program as "re-validate".
- On the next `kzvuExecute`, microVU `memcmp`s the ranges of the programs it already compiled for that start PC. It
  reuses a match, and compiles a new program only when none matches. Switching back and forth between known programs
  never recompiles.
- A write to `kzvuCodeMem()` that is not announced is not seen by the JIT: it keeps running the cached code. The test
  checks this.

The easiest hook for PS2Recomp:
- In the MSCAL/MSCNT callback, compare `PS2Memory::getVU1CodeGeneration()` with the value seen last time.
- If it changed, call `kzvuMicroWritten(0, 0x4000)`.

This covers VIF MPG and EE writes alike. `kzvuWriteMicro` also skips the invalidation entirely when the written bytes
are unchanged.

The IbitHack (on for Killzone) changes what counts as a change:
- microVU reads these immediates from micro memory at run time and leaves their words out of the program comparison:
  - I-bit immediates
  - the lower words of IADDI, IADDIU, ISUBIU, ILW, ISW, LQ and SQ
- So a program whose only difference is such an immediate reuses the compiled code and still sees the new value, with
  or without an announcement (tested).
- The flip side: a change to the opcode or register fields of one of those words is not detected. Per Killzone's
  GameIndex entry, the game is fine with this, and it is PCSX2's behaviour.

## Clamping, flags, hacks

- **`clampMode`** maps exactly as in PCSX2's GameDatabase:
  - 1 sets `vu1Overflow`
  - 2 also sets `vu1ExtraOverflow`
  - 3 also sets `vu1SignOverflow`

  Only microVU clamps.
- **`iBitHack`** turns on PCSX2's `IbitHack` gamefix. See JIT invalidation above.
- **`flagHack`** turns on PCSX2's `vuFlagHack` speedhack, which is on by default in PCSX2. microVU then skips status
  flag calculations that no later instruction reads.
- **`xgkickHack`** turns on PCSX2's `XgKickHack`. It is off for Killzone.
- **Settings kzvu fixes:**
  - MTVU is always off.
  - `vu1Instant` is on (PCSX2's default).
  - `EECycleRate` and `EECycleSkip` are 0.
  - VU0 macro mode (COP2) is compiled but unreachable.
- **Changing settings:** any change to `clampMode`, `iBitHack`, `flagHack` or `xgkickHack` through `kzvuSetConfig`
  flushes the JIT cache. Change settings only between programs, not while `kzvuRunning()`.

## Threading

- There is one VU1 per process.
- Call kzvu from one host thread: the game thread that runs the VIF1 interpreter.
- There is no MTVU thread and no internal locking.
- The emitter keeps its write pointer in `thread_local` variables, which microVU re-seats on every call.

## Host integration sketch (not applied; ps2_runtime.cpp belongs to the main build)

```cpp
KzvuConfig vc;                                   // useJit, clampMode 0, iBitHack on: Killzone GameIndex
vc.xgkick = [](void* u, const uint8_t* p, uint32_t n, uint32_t) {
    static_cast<PS2Memory*>(u)->submitGifPacket(GifPathId::Path1, p, n);
};
vc.xgkickUser = &m_memory;
kzvuInit(vc, &err);                              // then point PS2Memory's VU1 code/data at kzvuCodeMem()/kzvuDataMem()

m_memory.setVu1MscalCallback([this](uint32_t startPC, uint32_t top, uint32_t itop) {
    if (auto g = m_memory.getVU1CodeGeneration(); g != m_lastVu1Gen) { kzvuMicroWritten(0, 0x4000); m_lastVu1Gen = g; }
    kzvuSetFBRST(cpuContext->vu0_fbrst);
    kzvuSetTop(top, itop);
    kzvuExecute(startPC, 65536);                  // same budget as today; kzvuContinue() if kzvuRunning()
    cpuContext->vu0_vpu_stat = (cpuContext->vu0_vpu_stat & ~0x0600u) | (kzvuVpuStat() & 0x0600u);
});
// MSCNT: same, with kzvuExecute(0xFFFFFFFF, ...). VIF1's VU-busy check: kzvuRunning().
```

## Test (`test/kzvu_test.cpp`)

Each case is a hand-encoded VU1 microprogram. The field layouts were cross-checked against PCSX2's `VUops.cpp` opcode
tables. Each case runs on kzvu's JIT, kzvu's interpreter and PS2Recomp's `VU1Interpreter`. PS2Recomp's sources are
compiled into the test only, with `test/ps2recomp_stubs.cpp` providing a minimal `PS2Memory` and GIF capture.

Every implementation is checked against expected values. In addition, the full final states are compared:
- VF1-31, VI1-15, ACC, Q
- all 16 KB of data memory
- the XGKICK packets

The cases:

| Case | What it checks |
|---|---|
| FMAC | ADD/SUB/MUL/MADD/MULA/MADDA with dest masks and x/y/z/w broadcasts; ITOF0/12, FTOI0/4; LQ/SQ with offsets and partial masks |
| integer | IADDIU/ISUBIU/IADD/IADDI/ISUB/IAND/IOR; an IBNE loop with work in the delay slot; ISW/ILW |
| memory | LQI/SQI post-increment loop |
| fdiv | DIV/SQRT/RSQRT into Q, WAITQ, MULq/ADDq |
| xgkick | an A+D packet, and a packet that wraps qword 0x3FF to 0x000; bytes and start address |
| xtop | XTOP/XITOP against host-provided TOP/ITOP |
| transform | the benchmark program, checked against a double-precision host reference; implementations compared within 1 ulp |
| budget | the same final state from one call and from 64-cycle slices via `kzvuContinue` |
| T bit | stop, VPU_STAT VTS1, interrupt, then MSCNT resume |
| invalidation | announced and unannounced micro-memory writes; IbitHack immediates |
| host state | MXCSR preserved and handed to the callback; shutdown and re-init |

The integer loop decrements its counter right before `IBNE`. A branch reads the value from before that instruction,
so the loop runs count + 1 times and ends with vi1 = -1. All three implementations model this quirk.

Result:
- RelWithDebInfo and Release both give 575 checks, 0 failures and exit code 0.
- There are 0 differences between the JIT and PS2Recomp on the checked programs.

Two findings are printed as informational. Neither is a kzvu bug; both are PCSX2's own behaviour:

1. **Branch VI-delay edge case.** A loop counter set by `IADDIU` two instructions before the loop head puts two writes
   of vi1 inside the first branch's 4-instruction window.
   - microVU makes the branch read vi1 as it was 4 instructions earlier (0), so the loop exits after one pass. This is
     documented in `microVU_Analyze.inl`, `analyzeBranchVI`.
   - PCSX2's interpreter and PS2Recomp read the value from just before the preceding IADDI, so they keep looping.

   Killzone runs under microVU in PCSX2, so microVU's behaviour is the one the game is known to work with.
2. **D bit.** microVU ignores the D bit (`doDBitHandling = false`: "shouldn't be enabled in released versions of
   games"). The interpreter stops on it. The T bit behaves the same in both.

Benchmark: the transform program, 10,000 runs.
- Setup: 16 vertices × (LQI, MULAx/MADDAy/MADDAz/MADDw, DIV+WAITQ, MULq, FTOI4, SQI, loop), then XGKICK of a
  17-qword packet.
- That is about 220 instruction pairs per run: 538 VU cycles, or 568 on PS2Recomp. At 294.912 MHz, that is 1.8 µs of
  PS2 time.

| Implementation | ns per run (RelWithDebInfo) | vs PS2 real time |
|---|---|---|
| kzvu microVU (JIT) | 97-98 | about 18x faster |
| kzvu interpreter | 8000-8600 | 4.4-4.7x slower |
| PS2Recomp VU1Interpreter | 55000 | 30x slower |

## License

GPL-3.0+. kzvu consists of PCSX2 code plus GPL shims. Anything that links kzvu is a GPL-3 derivative work.

## Known gaps

- **VU1 only.** There is no VU0 micro mode and no COP2 macro mode (its code is compiled but stubbed out). The VU0
  structures exist only because microVU1 reads VU0's VPU_STAT and FBRST.
- **Cycle counts** are PCSX2's model. They are close to PS2Recomp's but not identical: in the tests the transform
  takes 538 vs 568 cycles, and XGKICK 9 (JIT) / 13 (interpreter) vs 12. Budget accounting in slices can differ by a
  cycle.
- **Flags:** with `flagHack` on, the status-flag values the program never reads are not computed. `kzvuGetVI(16..18)`
  reflects microVU's end-of-program flag state. MAC/status/clip flags are not part of the differential comparison.
- **D bit:** ignored by the JIT, as in PCSX2 (see the findings above). The branch VI-delay edge case differs between
  the JIT and the interpreter.
- **Memory:** VU1 memory must be kzvu's own buffers. See Memory ownership.
- **Not provided:** no savestate support, and no MTVU (VU1 on its own thread).
- **Configurations:** only Release and RelWithDebInfo are tested. A Debug build keeps references to code that the
  optimizer normally drops. Those references are stubbed, but Debug has not been exercised.
- **Test data:** the test uses synthetic microprograms. No Killzone microcode has been run through the three
  implementations yet. Extracting the game's MPG uploads, or recording them at run time, and replaying them
  differentially is the next check worth doing.
