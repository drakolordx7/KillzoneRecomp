# Performance research: 22 fps -> 120 fps (2026-09-29)

Scope: what other static recompilers and PS2 emulators do that this port does not, checked against the local code.
Nothing was built or run for this report. Percentages come from `work/profile.txt`: 30 s of gameplay, the game thread
100 % busy, sampled by `kz_sampler` at ~50 Hz. "Proven" means shipped and measured in another project.
"Speculative" means an estimate from reading the code.

## 0. Where the game thread goes today (work/profile.txt, inclusive %)

| Bucket | Share | Evidence |
|---|---|---|
| IOP interpreter + SPU2 mix, run synchronously in EE checkpoints | ~18 % in this profile (25-30 % in other runs) | `advanceIopEeCycles` 18.2, `kzspu2_TimeUpdate` 7.3, `IopCpuCore::executeInstruction` 3.5 |
| Call and checkpoint overhead (not counting the IOP) | ~10-15 % | `dispatchGuestBranch`/`hasFunction`/`lookupFunction`/`pushDispatchPc`/slot ~4.6 self; `checkpointDue`+`accountCycles`+`advanceEeTimers`+QPC ~5.5 self; `processPendingEvents` 6.3 incl. (mutex + `operator new`) |
| VIF1 + microVU1 + GIF, on the game thread inside the vblank handler | ~8-10 % | `FUN_00150090` 5.7 (it kicks D1_CHCR, see §4.6), `kzvu_microVU1` 4.1, `mVUcompileJIT<1>` 3.5, `processVIF1Data` 2.7 |
| Runtime helpers that should be inlined but show up as separate functions | ~4 % self | `Ps2FastRead32` 1.8, `Ps2FastRead64` 0.7, `isSpecialAddress` 0.7, `__security_check_cookie` 0.5, `ps2_fclamp` 0.4 |
| Heap and string traffic in stubs | ~3-5 % | `RtlAllocateHeap` 2.9, `ps2_stubs::strcasecmp` 2.5 (builds `std::string`s), vector insert in `writeIORegister` |
| Generated game code (and VU0 calls) | the remaining ~45-50 % | spread over thousands of `FUN_*`; no single hot spot |

A 5.5x speedup (22 -> 120) means 45 ms per frame must become 8.3 ms. Even with every overhead bucket gone, the
generated code alone is ~20 ms per frame. **So 120 fps simulation needs both: all runtime overhead off the game
thread, and generated code 2.5-3x faster.** §1-§3 cover the first part; only §2's codegen items (register locals,
TBAA, direct calls) can deliver the second. §5 covers the alternative: simulate at 60 and present at 120.

## 1. The first three things I'd do

1. **Build the hot code with release flags (hours, no regen, one full rebuild).** The build is `RelWithDebInfo`.
   `EnableFastReleaseMode` (`ps2xRuntime/cmake/ReleaseMode.cmake:9`) only applies its flags under `$<CONFIG:Release>`,
   so none of them are active. The generated units actually compile with
   `/O2 /Ob1 /Zi` (`build/RelWithDebInfo/build.ninja`, `unity_645_cxx`) and link with `/INCREMENTAL`:
   - `/Ob1` only inlines functions marked `inline`. The profile shows `Ps2FastRead32` etc. called out of line anyway,
     because MSVC's inliner gives up inside huge generated functions.
   - `/GS` is on by default: `__security_check_cookie` 0.5 % self. The 8/16-byte `wrapped[]` arrays in `Ps2FastRead64/128`
     trigger stack cookies.
   - `/INCREMENTAL` routes calls through ILT jump thunks: `ILT+...` frames appear in the profile.
   - Fix: `/Ob3` (or `/Ob2`), `/GS-`, `/Gy /Gw`, `/arch:AVX2` (kzgs/kzvu/kzspu2 already use it), and `/INCREMENTAL:NO /OPT:REF,ICF`
     for `killzone`, `ps2_runtime` and `ps2xIOP`. Also mark `Ps2FastRead*/Write*`, `Ps2IsSpecialAddress` and
     `Ps2ExtractEpi*` `__forceinline`.
   - Optionally add `/GL` + `/LTCG` on `ps2_runtime` and the IOP only, so `checkpointDue`/`accountCycles` inline into
     dispatch. `/GL` on 82k generated functions makes the link very long; skip it there.
   - Expected gain: 10-20 % (medium confidence). Risk: none functionally; build time.
2. **Stop promoting whole functions to resume points because of a JALR (one-line recompiler fix + regen).**
   - `control_flow_analyzer.cpp:190-424`: any indirect jump without a resolved table sets `needsIndirectFallback`.
     That includes every `jalr`, i.e. every virtual call and function-pointer call. Every instruction then gets a
     label and a `switch(ctx->pc)` case.
   - Measured here: 4,045 of 59,040 functions, **316k of 1.33M instructions (24 %)**, are fully promoted. Example:
     `generated/FUN_002e7160_0x2e7160.cpp`, 42 cases for 42 instructions. Every label is a merge point, which
     blocks MSVC's scheduling and dead-store elimination across instructions.
   - Proven elsewhere: the DQ8 fork of PS2Recomp made exactly this change (commit 75d5085, ran-j/PS2Recomp PR #254:
     "Restrict the fallback to JR. Promoted entries drop from 189,876 to 1,688").
   - The JALR return address is already queued as a resume target (`control_flow_analyzer.cpp:195-198`), so
     correctness is kept.
   - Expected gain: 5-15 % of generated-code time (speculative; depends on how hot these C++ methods are). Effort: low.
     Risk: low.
3. **Run the IOP + SPU2 on their own host thread (days).** The IOP is paced by wall clock (EE time = max(estimate, wall
   clock), `EeScheduler.cpp:483-513`), so its cost is a fixed ~0.2-0.3 s per second of the game thread. Moving it
   off gives a flat **1.25-1.4x** (high confidence on the size; details in §3). This is the biggest single item that
   needs no recompiler work.

After these, the next step is structural (§2.1-2.3): fibers, direct calls with an inline poll, then register
locals with clang-cl. That is what separates ~40-50 fps from 100+.

## 2. Ranked techniques

### Proven elsewhere

| # | Technique | Evidence | Expected gain here (confidence) | Effort | Risks |
|---|---|---|---|---|---|
| 2.1 | **Guest threads on host fibers; no unwinding to switch threads** | DQ8 fork commit 8b83b2b ("Run guest threads on their own stacks instead of unwinding them to switch"): the old unwind-and-rebuild transfer cost ~10 µs; a fiber round trip costs 22 ns; guest dispatches per movie frame went 82,441 -> 353. N64ModernRuntime runs every game thread on its own host thread with semaphore handoff (`ultramodern/src/threads.cpp`), so its generated code has no checks at all. | Mostly an enabler for 2.2. Directly: removes the re-entry cost after every yield and every non-local jump (`ps2_runtime.cpp:1556-1566`, `EeScheduler.cpp:265-374`). The yield rate in Killzone was not measured, so the direct gain is unknown. | Medium (the DQ8 code exists; x86-64 needs a hand-written stack switch, which DQ8 has) | Interrupt handlers need their own invocation stack (DQ8 did this). Guest setjmp/longjmp needs HLE, as XenonRecomp does. |
| 2.2 | **JAL as a direct C++ call; JALR through an inline table lookup; per-call checkpoint replaced by one inline flag test** | N64Recomp: "`jal` is recompiled directly into a function call", JALR -> `LOOKUP_FUNC` (N64Recomp README; `include/recomp.h`). PCSX2 blocks end with an inline `cycle >= nextEventCycle` compare; the full event test runs at most every `eeWaitCycles = 3072` cycles or at the next scheduled event (`pcsx2/R5900.cpp`, `_cpuEventTest_Shared`). | Removes the ~10-15 % call/checkpoint bucket. Also makes small leaves like `FUN_003da450` (18 instructions, called several times per iteration of `FUN_00505a78`'s hot loop) inlinable. Estimate 15-25 % (medium). | Medium (recompiler emitter + runtime). Needs 2.1 first, because today a `false` return from dispatch is how yields unwind. | Interrupt latency now depends on back-edge polls. That is fine: PS2 handlers only need to run "soon". Timers and vblank must post to an atomic flag from a host timer thread. The EE time-slice preemption (`EeScheduler.cpp:453-466`) is not PS2 behaviour: the EE kernel does not time-slice equal priorities. Dropping it is more faithful. |
| 2.3 | **Registers as C++ locals (and clang-cl)** | XenonRecomp README, "Optimizations": registers "can be safely converted into local variables… The local variable optimization particularly introduces the most improvements… frame times are reduced by several milliseconds" in Unleashed Recompiled. It requires Clang ("many optimization methods depend on Clang's code generation"). N64Recomp keeps `ctx->rN` as `uint64_t` fields with typed `*(int32_t*)` memory macros, so clang's TBAA keeps them in registers. | The only item that attacks the ~50 % generated-code bucket at scale. Today every guest instruction is a load/modify/store of `ctx`, and each dependent instruction pays a store-to-load forward (~4-5 cycles). Estimate **2-3x on generated code** (speculative, but it is the mechanism XenonRecomp credits). | High (recompiler: load used registers at entry; flush args and sp before calls; reload v0/v1 after calls; flush everything at syscalls, slow memory paths and exits). | ABI-violating hand-written assembly: needs a per-function opt-out. Mid-function hooks must name registers explicitly, as XenonRecomp's `midasm_hook` does. |
| 2.4 | **Macro-level codegen fixes, no regen** (a) test `addr < 0x02000000` first in `READ*/WRITE*` (today up to ~6 range compares, `ps2_address.h:53`); (b) drop the wrap/contiguity check in `Ps2FastRead*` (`ps2_runtime_macros.h:228`); aligned EE accesses cannot straddle the end of RAM, and the EE faults on misalignment; (c) make GPRs a union with `uint64_t` lanes, so `SET_GPR_S32` is one 8-byte store instead of load128/movsd/store128 (`ps2_runtime_macros.h:795-843`); (d) typed pointer loads/stores instead of `memcpy` into `uint8_t*`, so a future clang build gets TBAA | N64Recomp `MEM_W` is `*(int32_t*)(rdram + addr - base)`, with no checks and a typed access (`include/recomp.h`). MSVC has no TBAA at all (MSVC optimizer developer, simdjson issue #831), so every guest store forces every `ctx` register to be reloaded. | 5-15 % (medium). (c) adds a store-forward stall when a 128-bit read follows a 64-bit write (SQ of a freshly computed GPR). Measure it. | Low (header-only, full rebuild) | (b) must keep the slow path for LWL/LDL, which already align. Side finding: `MOVN`/`MOVZ` use `SET_GPR_VEC(GPR_VEC(...))`, which copies all 128 bits (`FUN_003da450`). The EE and PCSX2 move only the lower 64 bits. |
| 2.5 | **IOP + SPU2 on a separate thread** | Coprocessor-on-its-own-thread with loose sync is a standard emulator design: PCSX2 MTGS and MTVU, and Dolphin's "DSP LLE on thread" option. PCSX2 itself keeps the IOP on the EE thread but only syncs at scheduled events, not per block. | 1.25-1.4x flat (high confidence on the size of the IOP bucket) | Medium. `PS2IopHostAdapter` already serialises EE->IOP calls with `m_callMutex` (`ps2_iop_host.h`). | See §3. IOP->EE callbacks (`invokeGuestFunction`, `sendSifCommand`) must become queued EE events. Boot-time file waits (`FUN_00149f30`) were fragile under batching (patch 0011); a real concurrent IOP should make them less fragile, but that is untested. |
| 2.6 | **Asynchronous VIF1/VU1/GIF on a worker thread (the MTVU idea)** (implemented 2026-09-29, patch 0016: in-game ~31 -> ~46 frames/s, docs/findings.md "VIF1 worker thread") | PCSX2 MTVU. Killzone's GameIndex entry (`tools/pcsx2/resources/GameIndex.yaml:12094`) does not disable MTVU; it only sets `vuClampMode 0` + `IbitHack`. The DQ8 fork added an "ordered bounded GS worker". | 6-10 % (medium) | Medium: D1_CHCR.STR must read busy until the worker finishes, and the DMA-end interrupt is raised from there. The game already copes with real asynchronous DMA: `FUN_00151fc8` only kicks when D1 is idle and a buffer is in state 2. | VU1 memory and XGKICK ordering against PATH2/3. Frame-list reuse must wait for real completion. |
| 2.7 | **Scheduler and event-pump fixed costs** | DQ8 commits 2e538a7 and 20973e8: `selectReady()` walked 128 queues per dispatch (~7 %), `advanceEeTimers` did 64-bit divisions every safe point, `processPendingEvents` took a mutex just to ask "anything queued?". The same code is here: `EeScheduler.cpp:1734` (128-queue walk), `:1863-1894` (constructs a `std::deque`, locks twice), `ps2_memory.cpp:563` (division per armed timer per checkpoint, 2.4 % self). | 5-8 % (medium) | Low (port the DQ8 changes) | None notable |
| 2.8 | **Compiler: clang-cl + ThinLTO, then PGO** | XenonRecomp requires Clang 18+. Zelda64Recomp/N64ModernRuntime are developed on Clang. | clang-cl alone, with today's macros: small, because `memcpy` accesses and `__m128i` (declared `may_alias`) defeat TBAA. With 2.3/2.4(d): large. PGO: ~10 % typical, mostly code layout (speculative). | Medium (toolchain switch; kz* libraries use MSVC-only flags) | Build time; PGO needs a representative gameplay run. Do PGO last. |

### Speculative (reasoned from this codebase, not proven elsewhere)

| # | Technique | Evidence | Gain (confidence) | Effort | Risk |
|---|---|---|---|---|---|
| 2.9 | **Stop the per-instruction `ctx->pc` and delay-slot state stores.** Pass the pc to slow paths as an argument and store it only before calls, syscalls and exits. | Every instruction stores `ctx->pc`. Every branch stores `in_delay_slot`/`branch_pc` twice (see any generated function). Slow-path `Load32(rdram, ctx, addr)` takes `ctx`, so the stores cannot be removed as dead. | 3-8 % (low-medium). It mostly falls out of 2.3 anyway. | Medium (recompiler) | Exceptions and logging that read `ctx->pc` |
| 2.10 | **VU0 call cost.** `onVu0Call` copies the whole VF file in and out (`kz_vu.cpp:90-119, 306-330`), plus `kzvu0SetRegs`/`GetRegs` copies. That is ~2 KB of copying per call at ~0.5M calls/s (findings.md: 2M calls per 10 s at 9 fps). Copy only the registers each program uses (12 programs in total), or HLE the three hot programs 0x870/0x778/0x7B0 used by `FUN_00505a78` as native SSE. | findings.md VU0 section | 2-6 % (low-medium; the copy cost is unmeasured) | Low-medium | Program 0xD18's flag differences (findings.md) |
| 2.11 | **microVU1 recompiling in steady state.** `mVUcompileJIT<1>` is 3.5 % inclusive mid-gameplay. `prepare()` calls `kzvuMicroWritten(0, whole size)` on every code-generation change (`kz_vu.cpp:48-57`). Count compiles per second; if they don't settle, pass the real MPG range, or check that the program-cache search is hit. | profile.txt | up to 3.5 % | Low | none |
| 2.12 | **Vblank quantization check (a 5-minute experiment).** The finished frame list is kicked only from the vblank handler (`FUN_00152018` -> `FUN_00151fc8` -> `FUN_00150090`, gated by `0x55A795` and buffer state 2). `FUN_0014fd90` polls `COP0 Count` in a wait loop, so waits count as "busy". 66/3 = 22 matches the measured rate. Run the same scene at `KZ_FPS=66` vs `132` vs `240`: if fps rises, part of the "busy" time is vsync waiting, and a high internal R is free speed. | disassembly of the functions named | 0-30 % (unknown) | trivial | More vblank handler runs at high R (each is cheap) |
| 2.13 | `strcasecmp` and `printf` stubs without `std::string` (`readPs2CStringBounded`), and no vector insert per VIF FIFO write | profile.txt | 2-4 % | Low | none |

### Dead ends (for this port)

- **EE wait-loop / INTC-spin detection (PCSX2 `Speedhacks.WaitLoop`, `iR5900.cpp`).** In PCSX2, skipping an idle
  loop moves emulated time forward faster, which only helps because EE cycles *are* the clock. Here the EE clock is
  already clamped to wall time (`accountCycles`), so skipping guest cycles gains no frames. It would only save
  power. The real question is whether idle waiting inflates the busy time; that is experiment 2.12.
- **EE cycle-rate and VU cycle-stealing hacks.** Here they only change the cycle *estimate*, and wall clock
  dominates it. No effect on throughput.
- **GameIndex speed hacks for SCUS-97402.** There are none beyond `IbitHack` (already the kzvu default) and
  `vuClampMode 0` (the port uses clamp 3 for VU0 on purpose; findings.md). "Skip MPEG" only affects FMVs.
- **Guard-page fastmem with exception-driven MMIO.** PCSX2 and Dolphin can backpatch their JIT code after a fault.
  Static C++ cannot, so every I/O access would have to go through an exception handler that decodes the faulting x86
  instruction (Xenia does this for MMIO; it is fragile with compiler-generated SSE moves). A single predictable
  `addr < 32 MB` compare (2.4a) gets almost all of the benefit.
- **Bigger IOP batches (patch 0011) as the long-term IOP fix.** They hit boot hangs, and 2.5 supersedes them.

## 3. IOP/SPU2 thread: a design that should work

- **One lock, IOP thread owns the emulator.** The IOP thread loops: take the IOP lock, `runCycles` up to
  `wallclock_now + slack` (0.5-1 ms), release, and sleep or spin-yield until the next deadline. PCSX2's EE runs the
  IOP to within ~3072 EE cycles (~10 µs); the IOP modules here only need to finish RPCs and DMA in "reasonable"
  time, and SPU2 output is buffered anyway.
- **EE-side IOP entry points take the same lock**: `handleRpc`, `onSifTransfer`, `loadModule`, IOP memory access
  (`PS2Runtime::*Iop*`). A synchronous RPC then runs on the EE thread exactly as it does now, while the IOP thread
  waits on the lock.
- **IOP->EE traffic becomes posted events.** `invokeGuestFunction` (RPC end callbacks) and `sendSifCommand` become
  `postEeEvent`, which already exists and is mutex-protected (`EeScheduler.cpp:408`). Today the callback runs inline
  on the EE thread's stack. This is the part that needs care: any caller that expects the callback to have run when
  the RPC returns must wait for the event.
- **SIF registers and SIF DMA completion flags** that the EE polls (`sceSifDmaStat`, the `0x1000F2x0` registers)
  need atomics or the lock.
- **SPU2 (`kz_audio.cpp` / `iop_host_spu2.h`)** already runs inside `runCycles`, so it moves with the IOP.
- **Checks:** the boot sequence (the old `FUN_00149f30` hang), an FMV with audio (patch 0004's callback-from-inside-demux path), and a WAV
  capture compared to the current build.

## 4. Codebase-specific answers to the questions asked

1. **Can hot calls skip `dispatchGuestBranch`?** Not safely today. A `false` return from dispatch is the only way a
   yield (checkpoint, blocking syscall, thread switch) gets off the host stack; every JAL return address is a
   resume case for that reason. Once guest threads run on fibers (2.1), a yield no longer needs the host stack
   unwound, and JAL can be a direct call with an inline `if (g_eePending) eeService(ctx)` check (2.2).
   - Interim (no fibers): an inline fast path in the generated macro that increments a budget and calls the target
     from the dense table directly, falling back to `dispatchGuestBranch` only when the budget runs out or the
     pending flag is set.
   - That interim also removes `pushDispatchPc` + the branch-history ring + two `hasFunction` checks per call
     (`ps2_runtime.cpp:1544-1592`). Keep the history ring behind a debug define.
2. **Is READ32/WRITE32 inlined?** Written as lambdas around `static inline` helpers, but the profile shows
   `Ps2FastRead32`, `Ps2FastRead64` and `isSpecialAddress` as separate functions at 3.2 % self combined. Under `/Ob1`
   plus giant functions, MSVC does not inline them reliably. Fix with `__forceinline` + `/Ob3` (§1.1, 2.4).
3. **Is the per-instruction `ctx->pc` store avoidable?** Yes (2.9). Only exceptions, syscalls, slow-path I/O and
   resume points need the pc. The larger cost is that GPRs live in `ctx` with no alias information (2.3/2.4).
4. **Are interrupt checks at every backward branch needed?** A check is needed there (spin loops on the vsync counter
   `0x55A6E0` must see the handler run), but it must be an inline load-and-test, not a call. Today each back edge
   calls `eeCheckpointDue` -> `checkpointDue` -> `accountCycles` -> timers + IOP (`EeScheduler.cpp:424-513`;
   `PS2Runtime::eeCheckpointDue` is 10 % inclusive).
5. **Frame cap and limiter interaction.** Displayed rate = guest vblank rate R (`kz_timing.cpp`). The game's limiter
   (`FUN_001bff10`) spins until at least one tick has passed, so for 120 fps output R must be ≥ 120. Each frame is also
   kicked only at a vblank (§2.12), so a low R can quantize the simulation rate. Test that first.
6. **Vblank handler work.** `FUN_00150090` (in the vblank handler) starts the whole frame's VIF1 chain with a store
   to `D1_CHCR`. The runtime processes the chain synchronously inside that store: VIF unpack, microVU1, XGKICK into
   kzgs. That is why 2.6 moves real work off the game thread.

## 5. Is a 120 fps simulation the right target?

**Probably not as the first target. Aim for a 60 fps simulation and 120 Hz presentation.**

Rough budget, speculative, assuming each step delivers its mid estimate:

| Step | ms/frame on the game thread | fps |
|---|---|---|
| Today | 45 | 22 |
| + §1 flags/macros, JALR fix | ~37 | ~27 |
| + IOP thread, async VIF1/VU1, event-pump fixes | ~27 | ~37 |
| + fibers + direct calls + inline poll | ~21 | ~48 |
| + register locals + clang-cl (2-3x on generated code) | ~9-12 | ~85-110 |
| + PGO, VU0 HLE | ~8 | ~120 |

- 60 fps simulation is realistic after the runtime work plus the flag and macro fixes.
- 120 fps simulation needs the full register-locals rewrite and everything else to land. Even then, the game's
  own systems at dt = 1/120 are unvalidated (findings.md already calls for validating movement, fire rate, reload and
  animations at 60/144/240).

### Ways to get smooth 120 Hz output from a 60 Hz simulation

- **A. Driver frame generation (no code).** NVIDIA Smooth Motion supports DX11/DX12/Vulkan games and "can typically
  double the perceived frame rate" (nvidia.com, NVIDIA app news). kzgs presents through D3D11, and the profile shows an
  NVIDIA driver. AMD AFMF and Lossless Scaling are the equivalents on other hardware.
  - Needs: stable 60 fps frame pacing from the port, and a flip-model swapchain.
  - Costs: added latency, HUD and crosshair artifacts.
  - Effort: hours (a setting plus pacing checks).
- **B. Built-in FSR3/DLSS frame generation.** Needs motion vectors and depth from the renderer. The GS only receives
  screen-space vertices, so there are no per-object motion vectors. It also needs a D3D12/Vulkan GS backend. Not
  worth it.
- **C. RT64-style geometry interpolation, done at the VU1 input.** RT64 interpolates because it keeps untransformed
  vertices and matrices (the RSP transform is deferred to a GPU compute shader) and matches draw calls across
  frames (rt64/rt64 README). Zelda64Recomp adds display-list tags (extended GBI) to make matching reliable, and states
  "Changing framerate has no effect on gameplay".
  - On the PS2 the transform happens in VU1 microcode, so the equivalent hook is the VIF1 chain: per batch, record
    `MSCAL` + the matrix `UNPACK`s + the vertex source address.
  - Match batches between frame N-1 and N (by microprogram, vertex address, and object pointer where findable), then
    for the in-between frame re-run VU1 + GS with interpolated matrices: slerp rotation, lerp translation, lerp bone
    palettes for skinned meshes. HUD/2D batches are passed through.
  - Cost: VU1 + GS work doubles, but both are off the game thread after 2.6.
  - Effort: weeks. It is game-specific (needs Killzone's VU1 data layout: matrix offsets per microprogram), and
    particles and effects will need exclusions.
  - This is the only route to *true* 120 Hz motion without a 120 Hz simulation.

**Recommendation:** do §1, 2.5-2.7 and 2.12 now. Then do 2.1-2.2 (fibers + direct calls), which is the DQ8 fork's
path and has measured results. Treat 2.3 (register locals + clang-cl) as the project that decides whether a 120 Hz
simulation is reachable. Ship 60 fps simulation + option A for 120 Hz in the meantime.

## 6. Measurement gaps to close first

- The stack-walk sampler runs at ~50 Hz and cannot attribute frames without unwind info (the unexplained `0x560000`
  frame at 9 % inclusive is probably microVU JIT code). Use ETW/WPR or AMD uProf/VTune for self-time per function
  and for the share of time spent in generated code.
- Count yields per second, checkpoint calls per second and guest calls per second (a cheap counter printed with the
  headless heartbeat) to size 2.1/2.2 before building them.
- Measure time spent in the spin/wait functions (`FUN_0014fd90`, `FUN_001bff10`) per second (2.12).

## Sources

- XenonRecomp README (optimizations, local variables, Clang requirement): https://github.com/hedge-dev/XenonRecomp/blob/main/README.md
- N64Recomp README (direct `jal` calls, `LOOKUP_FUNC`): https://github.com/N64Recomp/N64Recomp/blob/main/README.md ; macros: https://github.com/N64Recomp/N64Recomp/blob/main/include/recomp.h
- N64ModernRuntime threads on host threads: https://github.com/N64Recomp/N64ModernRuntime/blob/main/ultramodern/src/threads.cpp
- Zelda64Recomp high framerate (RT64): https://github.com/Zelda64Recomp/Zelda64Recomp ; RT64 interpolation design: https://github.com/rt64/rt64
- PCSX2 event test / IOP sync (`eeWaitCycles`, `_cpuEventTest_Shared`): https://github.com/PCSX2/pcsx2/blob/master/pcsx2/R5900.cpp ; WaitLoop in `x86/ix86-32/iR5900.cpp`, `x86/iR3000A.cpp`
- DQ8 PS2Recomp fork (JALR fallback, fibers, scheduler costs): https://github.com/ran-j/PS2Recomp/pull/254 (commits 75d5085, 8b83b2b, 2e538a7, 20973e8)
- MSVC has no TBAA (MSVC optimizer developer): https://github.com/simdjson/simdjson/issues/831
- MSVC `/Ob` levels: https://learn.microsoft.com/en-us/cpp/build/reference/ob-inline-function-expansion
- NVIDIA Smooth Motion (DX11/DX12/Vulkan): https://www.nvidia.com/en-us/geforce/news/nvidia-app-global-dlss-overrides-rtx-40-series-smooth-motion/
- Dolphin DSP LLE on thread: https://bugs.dolphin-emu.org/issues/7172
