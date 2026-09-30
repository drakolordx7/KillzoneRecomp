# kzvu: PCSX2's VU1 (and VU0 micro mode) as a static library

kzvu compiles PCSX2's VU1 execution, and VU0 micro mode (VCALLMS/VCALLMSR programs), into one static library,
`kzvu.lib`. It contains two cores for each VU:

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
  - `x86/microVU.cpp`, which includes all `microVU_*.inl` files as one translation unit (compiled through
    `shim/unity/KzvuMicroVU.cpp`, which `#include`s it unmodified and adds read-only diagnostics)
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

When no partial packet is buffered, the tags of an XGKICK transfer are walked where they are and the callback gets a
pointer straight into VU1 data memory (no copy through kzvu's buffer; only the unfinished tail of a transfer, e.g. the
first half of a packet that wraps VU memory, is buffered). `KZVU_XGKICK_INPLACE=0` restores the copy;
`KZVU_XGKICK_CHECK=1` walks every in-place transfer a second time with `Gif_Tag` and compares packet boundaries and the
tail (23 M Killzone XGKICK transfers: 0 differences).

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

**Code-state memo** (`kzvuVu1CodeChanged`, `shim/unity/KzvuMicroVU.cpp`; `KZVU_STATE_MEMO=0` = microVU's own
invalidation). `kzvuMicroWritten`/`kzvuWriteMicro` used to call microVU1's `Clear`, which forgets its whole
current-program table (`prog.quick`), so every program entry afterwards (an MSCAL, and every JR/JALR, which microVU
treats as a new program start) searches its program list again: for each candidate program it compares every range it was
compiled from with micro memory. Killzone changes VU1 microcode ~4000 times a second between a few thousand distinct
contents, and that search was 15 M `memcmp` calls per second (17 bytes on average, ~270 TSC cycles each: cache misses on
the candidates' copies of the code), ~10-15 % of the VU1 thread; it is not recompilation. The memo keeps, per 16 KB micro
memory content (128-bit key), the table that was in use for it: when the code changes, the entries of the table in use are
saved under the outgoing content, and if the incoming content was seen before its saved entries are put back (they are
the programs the search would have found). Saved entries are only reused while nothing was compiled since (microVU's code
pointer is unchanged: compiling can add ranges to a program), and everything is forgotten when microVU resets its cache,
so the result is what the search would have produced. The rest of `mVUclear` (pipeline state cleared, `cleared` flag) is
done as before. Measured: 15 M -> ~2 M `memcmp` calls per 10 s in gameplay (85-90 % of code changes find their table),
memo cost ~450 M TSC cycles per 10 s. `KZVU_STATE_MEMO_VERIFY=1` keeps the contents and checks every saved and every
restored entry with the same range comparison microVU uses (Killzone, 235 s: ~500 k code changes, ~10 M entries, 0 bad).

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
- kzvu has no internal locking and no MTVU machinery. By default call everything from one host thread.
- VU1 may instead run on its own thread while VU0 micro mode runs on another (the game's VIF1 worker thread does this):
  call `kzvuBindVu1Thread()` on the VU1 thread before its first VU1 call and before any VU1 program has run anywhere,
  then call every VU1 function (`kzvuExecute`, `kzvuContinue`, `kzvuSetTop`, `kzvuSetFBRST` for VU1, `kzvuMicroWritten`,
  the XGKICK callback, the register accessors) from that thread and every `kzvu0*` function from the other one. VU1 must
  stay on that thread once VU1 code has been compiled.
- What makes that safe: VU1 code reads and writes VU0's VPU_STAT and FBRST registers (busy and D/T-stop bits, D/T
  enables) with unlocked read-modify-writes, and both VUs use them. `kzvu_prefix.h` redefines `VU0` (PCSX2's
  `static VURegs& VU0 = vuRegs[0]`) as a per-thread pointer; the VU1 thread gets a private register file there, so the
  two VUs never share those words (the JIT embeds the address at compile time on the compiling thread). The FBRST
  value, the pending D/T interrupt bits and the host FPCR are `thread_local`. This replaces what PCSX2's THREAD_VU1
  special cases do (VU1 skips those writes). Other state the two VUs touch concurrently is per-VU, or written by only
  one of them (`cpuRegs.cycle` is written by both but nothing reads it).
- Not synchronised: a VU0 program that reads or writes VU1's registers through VU0 memory (0x4000+). None of the
  captured Killzone VU0 calls does (`kzvu0_test` reports it).
- The emitter keeps its write pointer in `thread_local` variables, which microVU re-seats on every call.
- `shim/unity/KzvuMicroVU.cpp` compiles PCSX2's `x86/microVU.cpp` unmodified (through `#include`) and adds
  `kzvuVu1CodeStats` / `kzvuVu1DiffCachedProgram`: read-only views of microVU1's JIT cache use and program lists, used by
  the game's `KZ_VU_STATS=1` diagnostics.

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
| code-state memo | 600 alternations between three micro-memory contents with two entry points each: every run must execute its own content's program, A and B compile once (4 programs, 6 with C), most code changes restore a remembered table |
| host state | MXCSR preserved and handed to the callback; shutdown and re-init |

The integer loop decrements its counter right before `IBNE`. A branch reads the value from before that instruction,
so the loop runs count + 1 times and ends with vi1 = -1. All three implementations model this quirk.

Result:
- RelWithDebInfo gives 1776 checks (575 before the code-state memo test below), 0 failures and exit code 0; Release
  was last run with the 575-check version.
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

## VU0 micro mode

VU0 runs the microprograms the EE starts with VCALLMS/VCALLMSR (PS2Recomp's old path: `VU1Interpreter` in VU0 mode,
reset and run with a 4096-cycle budget on every call). kzvu runs them on microVU0 (`vu0UseJit`, default) or PCSX2's
VU0 interpreter (`VU0microInterp.cpp`, now compiled in).

```cpp
uint8_t* kzvu0CodeMem();  uint8_t* kzvu0DataMem();       // 4 KB each, kzvu-owned, same reason as VU1's
void kzvu0MicroWritten(uint32_t offset, uint32_t size);   // after VIF0 MPG / EE stores to micro memory
void kzvu0SetRegs(const Kzvu0Regs&);  void kzvu0GetRegs(Kzvu0Regs&);  // VF, VI, ACC, status/MAC/clip, R, I, Q
uint32_t kzvu0Execute(uint32_t startPcBytes, uint32_t maxCycles);   // VCALLMS; 0xFFFFFFFF = continue at TPC
bool kzvu0Running();  uint32_t kzvu0VpuStat();  uint32_t kzvu0TPC();  uint64_t kzvu0Cycles();  uint64_t kzvu0Calls();
Kzvu0CallOut kzvu0Call(const Kzvu0Host&, uint32_t startPcBytes, uint32_t maxCycles, uint32_t fbrst);  // all of the above in one step
```

- **`kzvu0Call`:** SetRegs + `kzvuSetFBRST` + Execute + GetRegs for a host whose COP2 file is plain memory (`Kzvu0Host`
  holds pointers to the VF/VI/ACC/flag/R/I/Q storage; VF0 and VI0 are written back as the constants). No `Kzvu0Regs`
  staging copy, and the per-thread `VU0` pointer is bound once per call. The game uses it (`src/kz_vu.cpp`,
  `KZ_VU0_DIRECT=0` = the staged path, `KZ_VU0_VERIFY=<n>` = compare both in-game).

- **Registers:** VU0's register file is the EE's COP2 register file, so the host copies it in before each call and
  out afterwards (`Kzvu0Regs`). `kzvu0SetRegs` seeds the flags the way PCSX2's `vu0ExecMicro` does: the interpreter's
  copies, and microVU's four flag instances (status in microVU's internal bit layout). Q is written to both of
  microVU's Q instances.
- **M bit:** microVU0 ends a block after an M-bit instruction (the point where a real VU0 lets an interlocked COP2
  transfer through) with VBS0 still set. `kzvu0Execute` resumes until the E bit, so a call always runs the whole
  program, as the old path did. Killzone's VU0 micro memory contains no M bits in any captured state.
- **VU1 registers at 0x4000+:** VU0 loads/stores to qwords 0x400-0x43F reach VU1's VF/VI (microVU and PCSX2's
  interpreter both map them onto kzvu's VU1 state; PS2Recomp's interpreter wraps them into VU0 memory instead). No
  captured Killzone program touches them.
- **Clamping:** `vu0ClampMode` (-1 = `clampMode`). The game uses 3 (`src/kz_vu.cpp`): once a level loads, NaN/Inf
  values reach VU0 inputs through memory from the EE side (VU0 never made one from finite inputs in the measured
  runs), which a PS2 cannot produce, and with mode 0 they propagate through VU0 results (measured:
  gameplay drops from ~9-10 to ~1.3 frames/s). Mode 3 clamps them to +-max with the sign kept, which is what the old
  interpreter path did and how the PS2 treats exponent 255. On finite inputs the clamp mode makes no difference in
  the captured programs.
- **Code cache:** microVU0 and microVU1 share one 128 MB allocation (PCSX2's two regions, back to back).
- **D/T bits:** DE0/TE0 (FBRST bits 2/3, via `kzvuSetFBRST`) stop VU0 as in PCSX2; `kzvu0VpuStat` says which.

### Test (`test/kzvu0_test.cpp`)

`kzvu0_test [--clamp N] [--bench] [capture dir ...]` (default: `test/vu0_captures`, clamp 3). Exit code 0 = pass.
`--bench` only times one call of each captured start PC (200 000 iterations): the old path (context -> `Kzvu0Regs`,
`kzvu0SetRegs`, `kzvu0Execute`, `kzvu0GetRegs`, back) and `kzvu0Call`, ns per call.

1. Synthetic programs with known results on the JIT and PCSX2's interpreter: FMAC + LQ/SQ with registers passed in,
   the 4 KB data wrap, VU1 VF/VI read and written through 0x4000+, M bits in the middle of a program, ITOF0 of
   integer bit patterns, and a 2000-iteration loop (6005 cycles). The old path stops that loop at its 4096-cycle
   budget (vf2.x = 1024 instead of 2001); no captured game program comes close to 4096 cycles.
   `kzvu0Call` is also compared with SetRegs/Execute/GetRegs (all registers, flags, TPC, VPU_STAT, data memory, VU1
   registers) on every captured call and on 16 randomized inputs each (random VF bits including NaN/Inf/denormals).
2. Captured game calls (`Kzvu0Capture`, `include/kzvu0_capture.h`), written by the game with `KZ_VU0_DUMP=<dir>`
   (`KZ_VU0_DUMP_AFTER`, `_PER`, `_MAX`, `_FINITE=1`; see `src/kz_vu.cpp`). Each is replayed from its input state on
   the JIT, PCSX2's interpreter and PS2Recomp's interpreter run exactly like the runtime's old path. Fails if the JIT
   replay does not reproduce the in-game result bit for bit; everything else is reported.

`test/vu0_captures`: 103 calls from the first mission (gameplay, t > 100 s), one or three per (program, EE call
site), taken with `KZ_VU0_DUMP_FINITE=1`, so no input holds a NaN/Inf. Programs 0x000, 0x020, 0x270, 0x2c0, 0x4f0,
0x520, 0x6c8, 0x7b0, 0x870, 0xc80, 0xd18 (0x7b0 and 0x870 are FUN_00505a78's). Result (RelWithDebInfo):

- 303 checks, 0 failures. JIT replay == in-game result: 103/103.
- JIT vs PS2Recomp (old path): bit-identical VF/VI/ACC/Q/memory in 91 of 103. The 12 others are all program 0xd18
  (EE callers 0x3BD50C/0x3BD540/0x3BD588/0x3BE5AC): its `FMAND vi1, vi3` reads the MAC flag of an FMAC issued a few
  instructions earlier, PS2Recomp's flag pipeline gives vi1 = 0x10 where microVU gives 0, the following IBEQ goes the
  other way and the results differ completely (e.g. Q 0.667 vs 0.499). PCSX2's interpreter takes the same path as
  microVU (it differs from microVU only in float lanes there).
- JIT vs PCSX2 interpreter: 26 captures differ, all float lanes: 1-54 ulp in ACC/MADD chains, and in three captures a
  lane that is the tiny difference of two nearly equal values (e.g. 0 vs 1.2e-7). Where they differ, the JIT and PS2Recomp
  agree bit for bit (except 0xd18), so these are PCSX2 interpreter rounding, not JIT errors.
- No captured program reads or writes VU1 registers, contains M bits, and the longest takes 92 cycles (95 on PS2Recomp).

On 80 calls captured at clamp mode 0 (72 of them with NaN/Inf in their inputs; not in the repo), the JIT and PS2Recomp
differ in 28 at `--clamp 0` and in 12 at `--clamp 3` (programs 0x870, 0xc80, 0x020, 0xd18: NaN operands handled
differently by the two clamping schemes, plus 0xd18's branch).

## License

GPL-3.0+. kzvu consists of PCSX2 code plus GPL shims. Anything that links kzvu is a GPL-3 derivative work.

## Known gaps

- **No COP2 macro mode.** VU0 micro mode is provided (see above); COP2 macro instructions stay in the host's
  generated code (microVU's macro recompiler is compiled but stubbed out).
- **VU0 runs synchronously.** A VCALLMS runs to its E bit before the EE continues. EE code that feeds a running VU0
  program through interlocked transfers at M-bit points would not work; Killzone's VU0 code has no M bits.
- **`kzvuSetVI(16)`** (VU1 status) seeds microVU's status instances with the architectural value, not microVU's
  internal layout (`kzvu0SetRegs` does it right). Nothing in the game sets VU1's status flag, so it is unfixed.
- **Cycle counts** are PCSX2's model. They are close to PS2Recomp's but not identical: in the tests the transform
  takes 538 vs 568 cycles, and XGKICK 9 (JIT) / 13 (interpreter) vs 12. Budget accounting in slices can differ by a
  cycle.
- **Flags:** with `flagHack` on, the status-flag values the program never reads are not computed. `kzvuGetVI(16..18)`
  reflects microVU's end-of-program flag state. MAC/status/clip flags are not part of the differential comparison.
- **D bit:** ignored by the JIT, as in PCSX2 (see the findings above). The branch VI-delay edge case differs between
  the JIT and the interpreter.
- **Memory:** VU1 memory must be kzvu's own buffers. See Memory ownership.
- **Not provided:** no savestate support, and no MTVU as such: VU1 can be moved to a host thread by the caller (see
  Threading), kzvu does not run one itself.
- **Configurations:** only Release and RelWithDebInfo are tested. A Debug build keeps references to code that the
  optimizer normally drops. Those references are stubbed, but Debug has not been exercised.
- **Test data:** `kzvu_test` (VU1) uses synthetic microprograms only. VU0 has captured Killzone calls
  (`kzvu0_test`); recording VU1 MSCAL states the same way is the next check worth doing.
