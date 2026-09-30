# Performance: fresh ideas (22 fps -> 120 fps)

Research pass, 2026-09-29. Web survey plus a read of our own profile (`work/profile.txt`, one 30 s sample, KZ_PROFILE) and the generated code.
Nothing here was built or run. Effort: S = days or less, M = about a week, L = weeks. Payoff is a gut feel.

## What our own profile says (this frames everything below)

- Game/EE thread is 100% busy. The GS thread is only **12% busy**. The GPU is idle. So the GS side has about 8x headroom.
- Inside the game thread (inclusive):
  - **~24%** is `EeScheduler::checkpointDue`. That is IOP interpreter (~8%), SPU2 mixing (~7%), event queue with malloc and locks (~6%), EE timers (2.4% self).
  - **~72%** is `dispatchGuestBranch` and everything under it, meaning recompiled guest code. It is spread thin over hundreds of `FUN_*`, with no single hot spot.
  - Hardware emulation is small: microVU1 4%, VIF1 unpack ~2%, DMA/IO store path ~7%.
- Generated code style (checked in `generated/FUN_00505a78_0x505a78.cpp` and `ps2_runtime_macros.h`):
  - Every instruction stores `ctx->pc`.
  - GPRs live in `__m128i ctx->r[32]` and are extracted per use.
  - Every load/store is a lambda plus an `isSpecialAddress` test.
  - Every call goes through `dispatchGuestBranch`, which writes a branch-history ring, does a checkpoint check, runs `hasFunction`, then `lookupFunction`, then an indirect call.
- No amount of micro-optimising gets 5.5x. Codegen and offload fixes plausibly give 1.5-2x. **Only decoupling the presented rate from the EE rate multiplies.** So the plan is: make the EE loop as fast as it will go, then present 3-4 frames per EE frame.

---

## Most promising (read these first)

1. **GS-stream render interpolation, RT64 / Ship-of-Harkinian style (#1 below).** Buffer two frames of GIF packets, and replay N in-between frames with matched draws' vertex positions lerped. The EE runs at 30 Hz (or 60), the GS thread has the spare CPU, and you present 120 Hz. This is the only idea with a 4x lever.
2. **Get IOP and SPU2 off the EE thread (#2).** About a quarter of the maxed core is not guest code at all. It is a well-bounded, low-risk job that gives about 1.3x by itself.
3. **Lean codegen and lean calls (#3).** XenonRecomp gets several ms per frame from keeping registers in locals and dropping per-instruction context traffic. Our output is the maximally literal form of this.
4. **Late-latch camera / reprojection (#4).** Mouse-look smoothness is a different problem from fps. Apply the newest yaw/pitch at present time, as NVIDIA Reflex 2 Frame Warp and VR timewarp do. It makes 30-60 sim fps feel right immediately.
5. **Check the compiler flags and add PGO (#5).** The profile shows `__security_check_cookie` (so /GS is on). CMake's `RelWithDebInfo` default is `/Ob1` (limited inlining). That is a cheap thing to verify and it could be free 10-20%.

---

## All ideas

### 1. GS-stream render interpolation (draw-call matching, RT64-style)  [TOP]
- **Idea:** Record the GIF/GS command stream of two consecutive EE frames. Match draws by index order, prim type, vertex count and texture/state. Then emit K extra frames to the GS with XYZ2 positions (and optionally STQ/RGBA) linearly interpolated. The EE never sees the extra frames.
- **Source:** RT64 does exactly this for N64 (draw-call matching, tracked velocities, "generating new frames and modifying them in 3D space"): https://github.com/rt64/rt64. Ship of Harkinian and 2Ship interpolate transforms to get 60 fps from 20 fps game logic: https://www.videogameschronicle.com/news/zelda-ocarina-of-times-pc-port-now-supports-60fps-save-states-linux-and-more/. RT64's own README warns that matching is heuristic and can warp geometry when games compose matrices oddly: https://github.com/rt64/rt64.
- **Why here:** We already own the full stream (`kzgs` receives `GSgifTransfer` packets on its own thread). The GS thread is 12% busy. The game is dt-scaled, so the sim can run at its native 30 Hz. PS2 VU1 output is already screen-space fixed point, so lerp is trivial. The weak spot is clipped/varying vertex counts. The fallback there is "don't interpolate this draw, repeat it".
- **Effort:** L (matching heuristics, per-draw fallbacks, post-process/sprite passes, testing on fast camera motion).
- **Payoff:** Very high. 30 sim fps to 120 presented, at about 1 frame (33 ms) added latency unless combined with #4.
- **Notes:** Replaying the whole stream K times (textures, CLUT, render-to-texture) is the simple, correct way. Only the vertex writes differ. Cost is roughly K x the GS thread's current load, so about 50% busy at K=4.

### 2. Move IOP + SPU2 off the EE thread  [TOP]
- **Idea:** Give the IOP interpreter and the SPU2 mixer their own thread(s), synchronised by batched cycle budgets (`PS2X_IOP_BATCH` already exists). SPU2 needs almost no synchronisation: it is write-only from the EE side, except for IRQ/position reads, which can be predicted from the clock. Also fast-forward the IOP's idle thread instead of interpreting it.
- **Source:** Our profile (IOP ~8% + SPU2 ~7% + event queue ~6% on the game thread). Prior art for threaded audio/IOP: PCSX2's separate SPU2/IOP handling (https://github.com/PCSX2/pcsx2). ps2xIOP already has HLE fallbacks (https://github.com/ran-j/PS2Recomp).
- **Why here:** It is guest-independent work with well-defined edges, and the evidence is measured.
- **Effort:** M (S for SPU2 alone; M for IOP because of RPC/DMA ordering and the determinism fixes seen around `PS2X_IOP_BATCH`).
- **Payoff:** About 1.25-1.35x on the EE thread. Certain, but only one factor of the total.

### 3. Lean codegen: locals, no per-instruction ctx stores, direct calls  [TOP]
- **Idea:** Change what the recompiler emits. (a) Callee-saved and temp GPRs become C++ locals inside a function (spill only around calls/syscalls). (b) Drop `ctx->pc =` on every instruction; keep it only before things that can leave the function. (c) A JAL to a known static target becomes a direct C++ call, with the checkpoint counter decremented inline (no branch-history write, no `hasFunction`+`lookupFunction`). (d) Keep the 64-bit lo half in `uint64_t` and touch the upper half only for MMI/`lq`/`sq`. (e) Skip the link register.
- **Source:** XenonRecomp documents this: non-volatile and other registers as locals, "frame times are reduced by several milliseconds" and 20 MB smaller binary in Unleashed Recompiled: https://github.com/hedge-dev/XenonRecomp/blob/main/README.md. Same source: mid-asm hooks and weak-symbol function aliasing for patching.
- **Why here:** Our profile shows `dispatchGuestBranch`, `hasFunction`, `lookupFunction`, `pushDispatchPc`, `generatedFunctionTableSlot` all in the top self-time list, and each guest call pays a lot of overhead. Games compiled with GCC keep to the MIPS ABI, so the same assumptions mostly hold. Caveats: setjmp/longjmp, kernel thread context switches, and exception-style unwinding (`g_ps2GuestUnwinding`) need a checked escape hatch.
- **Effort:** L (recompiler changes plus a differential test against the current output, which the ps2xTest harness could drive).
- **Payoff:** 1.3-2x on the guest 72%. The largest single EE-side win.

### 4. Late-latch camera / reprojection for mouse-look  [TOP]
- **Idea:** Decouple camera rotation from the sim. When presenting, apply the newest mouse yaw/pitch as an image-space warp (or a view-matrix patch in the GS stream for world geometry, leaving the weapon and HUD alone). Uses the same trick as VR async timewarp.
- **Source:** NVIDIA Reflex 2 "Frame Warp": the CPU computes the next camera from the latest input and the frame is warped right before display, with in-painting for holes: https://www.nvidia.com/en-us/geforce/news/reflex-2-even-lower-latency-gameplay-with-frame-warp/. Independent demo that works on older GPUs: https://videocardz.com/newz/puredark-releases-free-demo-of-nvidia-reflex-2-frame-warp-works-on-rtx-20-gpus.
- **Why here:** We already have a mouse-aim patch (`kz_aim.cpp`) that writes yaw/pitch into the game. At 30 fps sim, that yaw only updates every 33 ms. A rotation-only warp for a ~1-3 degree delta is tiny, so holes stay at the screen edge and can be stretched. It also hides #1's added latency.
- **Effort:** M (simple whole-screen shift/rotation shader is S; the HUD/weapon-excluded version is M).
- **Payoff:** High for feel, since "smoothness" of aiming is usually what people perceive. Zero effect on fps numbers.

### 5. Compiler flags, PGO, and inlining across the unity batches  [TOP]
- **Idea:** Confirm what the generated code and runtime are built with. Look for `/Ob1` (the RelWithDebInfo default), `/GS` (cookie checks show up in the profile), CFG (`/guard:cf`, which taxes every indirect call), `/JMC`. Then try `/Ob2`-`/Ob3`, `/GS-`, `/Gw /Gy`, `/GL /LTCG`, and MSVC PGO or clang-cl `-fprofile-use` trained on a gameplay run. Try clang-cl: XenonRecomp says its optimisations "depend on Clang's code generation".
- **Source:** https://github.com/hedge-dev/XenonRecomp/blob/main/README.md (clang recommended for recompiler output). N64Recomp emits one function per file by default, which blocks inlining, and notes grouping as an option: https://github.com/N64Recomp/N64Recomp.
- **Why here:** 82k generated functions, unity batches of 128, huge switch-dispatch prologues. PGO helps precisely this shape (branch layout, hot/cold split, inlining of tiny leaf functions).
- **Effort:** S to M (a 15-minute full rebuild per experiment, per `CMakeLists.txt`).
- **Payoff:** 10-25% if flags are currently untuned. Verify first; not asserted.

### 6. Idle-loop / spin-wait skipping
- **Idea:** Find the guest's wait loops (vsync waits, `sceSifCheckStatRpc`, DMA-done polls, the IOP-read spin already seen at `FUN_00149f30`) and turn them into host-side blocking waits or a cycle fast-forward to the next event.
- **Source:** Dolphin's idle skipping (detect the loop, bump the cycle counter, also sync the CPU and GPU threads): https://dolphin-emu.org/blog/2016/11/01/dolphin-progress-report-october-2016/. PCSX2's INTC-spin and wait-loop detection: https://github.com/PCSX2/pcsx2/issues/10798.
- **Why here:** If the EE thread ever waits on something it produced itself (vblank, IOP), it burns real time as well as emulated time. If the loop is already blocking, the payoff is nil. So measure the fraction of time in the top few spinning functions first.
- **Effort:** S per loop, once identified (the recompiler config already supports instruction patches and stubs).
- **Payoff:** Unknown, 0-15%. It is also the safest way to make timing deterministic.

### 7. Fast memory: cheap RAM path, no per-access checks
- **Idea:** Map guest RAM at a fixed host base so RAM loads/stores are one instruction, and take special-address handling out of the common path. Statically classify access sites: `$sp`/`$gp`-relative and known RAM-range bases skip the `isSpecialAddress` test entirely; only sites whose base register comes from a `lui 0x10xx-0x12xx` constant get the slow path. Remove `ps2TraceGuestWrite` from release builds if it is not already a no-op.
- **Source:** Dolphin's "fastmem": map the whole guest address space, let MMIO access fault, and back-patch: https://dolphin-emu.org/blog/2026/03/12/dolphin-progress-report-release-2603/. XenonRecomp uses a base pointer plus 32-bit guest address: https://github.com/hedge-dev/XenonRecomp/blob/main/README.md.
- **Why here:** Every load and store currently pays a lambda, a range check, and a trace hook. `Ps2FastRead32` appears in the profile; `PS2Memory::write32` to `writeIORegister` is in the ~7% inclusive. Static (compile-time) classification avoids the fault-handler complexity, which is the point of static recompilation.
- **Effort:** M.
- **Payoff:** 5-15%.

### 8. Lazy timers and an allocation-free scheduler
- **Idea:** `advanceEeTimers` (2.4% self), the checkpoint accounting, and `processPendingEvents` (a `std::string`/`operator new` plus SRW lock in the hot path, about 6%) should be lazy. Compute timer counters on read from the cycle clock. Use a pre-sized ring or heap for events. Raise the checkpoint quantum so accounting runs per N thousand cycles, not per call.
- **Source:** PCSX2's counters use "next event cycle" scheduling rather than per-tick work: https://github.com/PCSX2/pcsx2 (`pcsx2/Counters.cpp`). Our profile.
- **Why here:** It is pure overhead, with no emulation value.
- **Effort:** S.
- **Payoff:** 5-10%.

### 9. Ahead-of-time recompile the VU0/VU1 microprograms
- **Idea:** Dump the game's VU1 (and hot VU0) microprograms once, and compile each to C++ ahead of time. Optionally inline the VU0 macro calls (`VCALLMS` is about 2M calls per 10 s, in `FUN_00505a78` and a loop) into the EE-side code, with no state marshalling.
- **Source:** BT3-Recomp did this and found the interpreter was their bottleneck: https://github.com/z3xox/BT3-Recomp/blob/main/docs/BOTTLENECK-INVESTIGATION.md, with the generator in `ps2xRuntime/tools/gen_vu1.py`. ICO-recomp statically recompiles its five VU1 programs: https://github.com/nathanialf/ico-recomp.
- **Why here:** We already replaced the interpreter with microVU. This removes the microVU JIT (3.5% in the profile, `mVUcompileJIT`, plus first-use stutter) and lets the compiler optimise across program boundaries. Killzone reuses a handful of programs.
- **Effort:** M (S to reuse a generator if one exists for the microVU program format).
- **Payoff:** Small on average (VU1 is about 4% now), but it kills JIT hitches and enables inlining of VU0.

### 10. MTVU-style asynchronous VIF1 / VU1 / DMA thread
- **Idea:** The EE thread copies each VIF1 DMA chunk into a ring and returns immediately. Another thread does the unpack, the VU1 run and XGKICK to GIF. Sync only when the EE reads VU1 memory or polls DMA status.
- **Source:** PCSX2's original threaded-VU1 design: https://pcsx2.net/blog/2011/threading-vu1/ and https://github.com/PCSX2/pcsx2/commit/ac9bf45. Known pitfall: VIF1 DMA transfers can still block the EE thread: https://github.com/PCSX2/pcsx2/issues/3024.
- **Why here:** Our DMAC/VIF1/VU1 path is synchronous on the game thread today (`Store32`, `writeIORegister`, `processVIF1Data` about 7% inclusive, plus microVU1 4%, plus unpack ~2%).
- **Effort:** M-L (memory-ordering care around chain data reused by the EE).
- **Payoff:** 8-12% off the EE thread.

### 11. RT64-style deferred geometry: VU1 output on the GPU, re-transformed per frame
- **Idea:** Reimplement Killzone's few VU1 programs as GPU compute or vertex shaders that produce the vertices for a frame. Keep the object matrices and camera as separate data. Extra frames are then just "re-run the transform with interpolated matrices", without the GS packet round trip.
- **Source:** RT64 defers all RSP vertex transforms to a compute shader so it can "patch transformations of the objects in the scene and the camera and produce a new frame very quickly": https://github.com/rt64/rt64. OpenGOAL replaces PS2 rendering paths with native renderers rather than emulating VU1: https://opengoal.dev/docs/porting-info/graphics/.
- **Why here:** It is the accurate, long-term version of #1: matrix-level interpolation, with no draw-call heuristics. It also gives real motion vectors (#13). Killzone's programs are finite.
- **Effort:** L-XL (understand each program, reproduce clipping and lighting quirks, and validate against microVU output).
- **Payoff:** Very high but slow. Treat as phase 2 after #1.

### 12. Un-patch the 60 Hz sim tick and decouple sim from vblank
- **Idea:** The port currently drops the game's `>>1` 30 Hz tick, so the whole EE frame (sim, culling, render list) runs every vblank at rate R. Go the other way: keep the guest sim at its native 30 Hz tick (or 20-30 Hz), run the vblank/present side at the display rate, and let #1/#4 fill in.
- **Source:** `docs/findings.md` (frame-timing section); the pattern is fix-your-timestep with render interpolation, as in LBA2's plan: https://github.com/LBALab/lba2-classic-community/blob/main/docs/plan/RENDER_INTERP_PLAN.md and Bevy-based ports: https://github.com/Normal99/skate-3-rust-engine-android/blob/main/docs/RENDER_INTERPOLATION.md. OpenGOAL has had to fix many rate-dependent mechanics when going above 60: https://opengoal.dev/blog/progress-report-q4-2025/.
- **Why here:** At 120 Hz today the EE would need 4x the original per-second work. At a 30 Hz sim it needs 1x, which the current 22 fps is already near. Also removes the need to validate physics at 120/144/240.
- **Effort:** S-M (it is mostly a config revert plus making vsync-driven parts, such as the fade counter, independent).
- **Payoff:** It is the prerequisite for #1; on its own it saves 2x or more of EE work compared with a 60/120 Hz sim.

### 13. Frame generation with real motion vectors (FSR 3 / DLSS-G / XeSS-FG)
- **Idea:** Give a vendor frame-generation SDK exact motion vectors and depth from our own data, instead of letting it guess optical flow. Motion vectors come free from #1's matched vertices or #11's matrices.
- **Source:** FSR 3.1 frame generation accepts "render-resolution motion vectors and depth" from any renderer and has a proxy swapchain that does pacing: https://gpuopen.com/fidelityfx-super-resolution-3/ and https://wccftech.com/amd-fidelityfx-sdk-v1-1-fsr-3-1-support-enhanced-upscaling-quality-decoupled-frame-generation-dlss-xess/. Emulator frame-gen is currently optical-flow based and smears on camera motion: https://heldgames.com/guides/dlss-5-emulators-pcsx2 (the article is about a leaked-library hack, so only the "optical flow is the weak point" argument is used here).
- **Why here:** It can replace the hand-written interpolation shader with better hole and disocclusion handling. It needs a depth buffer (kzgs has one) and a base rate of about 60 fps, per AMD.
- **Effort:** L.
- **Payoff:** Medium. Nicer image than a plain lerp. Not the first thing to build.

### 14. Zero-code stopgap: driver / third-party frame generation
- **Idea:** Try Lossless Scaling frame generation, AMD AFMF, or NVIDIA Smooth Motion on the window as-is, to see what 22-30 to 60-120 looks and feels like before building anything.
- **Source:** Lossless Scaling guides: minimum 30 fps, 40-60 preferred, x2 recommended over x4: https://videocardz.com/newz/lossless-scaling-3-released-with-frame-generation-up-to-x20-no-shrooms-required and https://www.tomshardware.com/video-games/pc-gaming/lossless-scaling-3-update-touts-greatly-improved-latency-and-performance-universal-frame-gen-tool-boasts-24-percent-reduced-latency.
- **Why here:** It is a 10-minute experiment that gives a floor for what #1 must beat. Latency and smearing are worse than in-engine, and 4x from 30 is at the edge of what these tools are meant for.
- **Effort:** S (user-side settings only).
- **Payoff:** Low-medium as a product, but it is a useful benchmark for the feel target.

### 15. Native replacements for hot guest leaf routines (HLE with mid-hooks)
- **Idea:** Use the sampler to rank the hottest guest routines (memcpy/memset/strlen-like loops, matrix/quaternion math, frustum-vs-AABB tests such as the RenderZone code) and replace them with hand-written native C++ (AVX2) bound by address, with differential tests against the recompiled version. `readPs2CStringBounded` and `memchr` already show in the profile.
- **Source:** Function hooking/override patterns in PS2Recomp game overrides (https://github.com/ran-j/PS2Recomp) and mid-asm hooks/weak aliases in XenonRecomp (https://github.com/hedge-dev/XenonRecomp/blob/main/README.md). OpenGOAL and the ICO decomp show that matching decompilation of hot code is realistic: https://github.com/nathanialf/ico-recomp.
- **Why here:** The profile is flat, so this pays only if a few dozen routines really account for a large chunk. Worth ranking with an inclusive-time listing of the guest functions before committing.
- **Effort:** M per handful of routines.
- **Payoff:** 5-20% if the top of the guest profile is concentrated, else little.

### 16. AI agent perf loop over the codegen, with a differential harness
- **Idea:** Give an agent the profile, the codegen templates and a strict differential test (the existing kzvu/interpreter comparisons, ps2xTest, savestate checksums). Let it propose and measure codegen variants and hook implementations, keeping only bit-identical wins.
- **Source:** Other recomp ports are now built this way (Wave Race 64, Pilotwings 64: https://github.com/elliotttate/wave-race-64-recomp, https://github.com/danielgomesvieira2000/pilotwings-64-recomp). ICO-recomp's Rust translator ships a reference interpreter specifically for this kind of checking: https://github.com/nathanialf/ico-recomp.
- **Why here:** The bottleneck in #3, #5, #7 and #15 is careful, tedious, verifiable work, which suits agents. The risk is silent behaviour drift, so the differential harness is non-negotiable.
- **Effort:** M (harness) plus ongoing.
- **Payoff:** Multiplies the other items; not a gain by itself.

### 17. OS and CPU placement tuning
- **Idea:** Pin the EE thread to the fastest P-core (or the best CCD), set it out of EcoQoS, raise timer resolution, use a high-resolution waitable timer for pacing, and consider large pages or a pre-touched guest RAM block. Keep IOP/SPU2/GS threads off the EE thread's SMT sibling.
- **Source:** Windows hybrid scheduling notes and `SetThreadInformation`/EcoQoS: https://aloiskraus.wordpress.com/2024/02/08/hybrid-cpu-performance-on-windows-10-and-11/ and https://comcomponent.com/en/blog/2026/06/03/000-windows-app-cpu-priority-affinity-power/.
- **Why here:** One core is the limit, so which core, and whether its sibling is busy, matters. The machine has had hard freezes with heavy load (see memory notes), so keep changes conservative.
- **Effort:** S.
- **Payoff:** 0-10%.

### 18. Keep the GS side cheap for K-times replay (paraLLEl-GS or a leaner GS thread)
- **Idea:** If #1 replays the stream 3-4 times, the GS thread's CPU-side cost (GSdx hardware renderer: texture hash cache, draw setup, D3D vertex buffer maps) grows 3-4x. paraLLEl-GS renders through Vulkan compute with very little CPU work per draw. Failing that, cache texture hashes across sub-frames, and batch draws.
- **Source:** https://github.com/Arntzen-Software/parallel-gs and the design blog https://themaister.net/blog/2024/07/03/playstation-2-gs-emulation-the-final-frontier-of-vulkan-compute-emulation/. ICO-recomp ships it as its default: https://github.com/nathanialf/ico-recomp. BT3-Recomp retired it on Windows after an AMD Vulkan driver crash, so this needs a driver check: https://github.com/z3xox/BT3-Recomp/blob/main/docs/BOTTLENECK-INVESTIGATION.md.
- **Why here:** Today the GS thread shows `HashTextureLevel`, `IAMapVertexBuffer`, `ClearGSLocalMemory` and NVAPI thunks. All of that is fine at 22 fps and worth measuring at 4x.
- **Effort:** M-L.
- **Payoff:** An enabler for #1, not a standalone gain.

### 19. Present path: decoupled present thread, flip model, VRR
- **Idea:** Run presentation on its own thread at the display rate, always presenting the newest composed frame, using a flip-model waitable swap chain (and VRR where available). The EE rate then stops dictating the display cadence.
- **Source:** RT64's design runs two renderers at different rates and offers "draw as early as possible" latency options: https://github.com/rt64/rt64. Fix-your-timestep pacing per LBA2's plan: https://github.com/LBALab/lba2-classic-community/blob/main/docs/plan/RENDER_INTERP_PLAN.md (which found frame caps and mismatched refresh give 2:3 judder).
- **Why here:** Interpolation only looks smooth if pacing is right, and 120 Hz makes 30 Hz sim cadence a clean 4:1.
- **Effort:** S-M.
- **Payoff:** Smoothness, not throughput.

### 20. Async, prefetched disc I/O
- **Idea:** Load `FILES.DAT`/`FILES0x.DAT` into memory (or memory-map them) and complete CDVD reads instantly, with decompression on a worker thread. This removes any IOP-side file-read spin or blocking from the EE thread.
- **Source:** Our own note: the boot hang was a game thread spinning at `FUN_00149f30` waiting on an IOP file read (`docs/findings.md`). Prior art for prefetch-style HLE CDVD is common in PS2 tooling and PCSX2's "fast CDVD".
- **Why here:** Cheap, and it removes a category of level-streaming hitches.
- **Effort:** S.
- **Payoff:** Small on average frame time, real on streaming hitches.

---

## Considered and rejected (do not spend time here)

- **EE/VU cycle-rate underclock or overclock hacks.** They apply to PCSX2's cycle-accurate loop. We have no such loop to trade, and they break logic and audio timing: https://github.com/PCSX2/pcsx2/issues/153.
- **Speculative run-ahead (RetroArch-style) for speed.** It is a latency trick that costs more CPU, and we are CPU-bound.
- **Trace JIT / dynamic binary translation.** Static recompilation already has the JIT-free advantage; the remaining gap is codegen shape, not compile strategy.
- **DLSS-5 "feeder" style hacks.** They depend on leaked libraries and optical-flow guesses (see #13 for the legitimate version).
- **GPU-ising the recompiled EE code itself.** Branchy scalar game logic does not map to a GPU.

## Suggested order

1. #5 flags check, #17 placement and #14 (measure feel), all under a day.
2. #2 (IOP/SPU2 offload) and #8 (lazy timers, scheduler), then re-profile.
3. #12 (sim at native 30 Hz) together with #4 (late-latch camera).
4. #1 (GS-stream interpolation) with #19 pacing and #18 as needed.
5. #3 (lean codegen) and #7 (fast memory), driven by #16's differential harness, in parallel with the above.
6. #11 and #13 only if #1's heuristics prove too fragile.
