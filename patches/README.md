# Local patches to submodules

Applied on top of `ext/PS2Recomp` (upstream ran-j/PS2Recomp @ 75d729c). Re-apply after a fresh clone with:

    git -C ext/PS2Recomp apply ../../patches/ps2recomp/*.patch

- `0001-recomp-resume-points-for-standalone-entry-blocks.patch` — the recompiler skipped resume-point registration
  for every `entry_*` function, assuming they are synthetic wrappers nested in a real function. Ghidra's export names
  65k standalone code blocks `entry_*`, so a thread switched out inside a call from one (e.g. static constructor
  `entry_004186f0`, resume at 0x418724) could never be resumed. Now only `entry_*` blocks nested inside a real
  function are skipped (sorted interval lookup).
- `0002-runtime-headless-mode.patch` — `PS2X_HEADLESS=1` runs without a window or audio device (automated runs,
  locked host session). `PS2X_HEADLESS_SHOTS=<dir>` saves the presented frame to PNG every
  `PS2X_HEADLESS_INTERVAL` seconds with a heartbeat line on stderr; `PS2X_HEADLESS_SECONDS` bounds the run.
- `0003-iop-cdvd-image-search-and-trayreq.patch` — with a disc image configured, the IOP cdvdman emulation read sectors
  from the image but resolved `sceCdSearchFile` against a virtual layout built from the host folder (different LSNs),
  so IOP-side file reads (Killzone's PFILE_R.IRX streamer) could land on the wrong sectors. Search now walks the image's
  own ISO9660 directory. Adds `sceCdTrayReq` (cdvdman #14; tray never moves), called ~2.7k times per boot.
- `0004-mpeg-hle-killzone-movies.patch` — Killzone's FMV player (all movies, incl. the looping menu backgrounds) on the
  runtime's libmpeg HLE (its sceMpeg*/sceIpu* calls are HLE-bound; FFmpeg decodes). `sceMpegAddStrCallback` now
  matches packets by libmpeg's stream key (stream id + 4 sub-stream bytes, channel OR'ed in; table read from the game's
  library), so the audio callback gets its PSS channel (5 language channels per movie). `sceMpegGetPicture` with no picture
  calls the game's sceMpegCbNodata callback (Killzone only demuxes from inside it) and treats the IPU_TO data the
  callback sends as consumed; it stops asking once the video hit its sequence_end_code. Without a sceCdSt stream
  (the game reads files itself) the program end code ends a movie, `sceMpegCreate`/`sceMpegReset` replay the video
  demuxed since the latest sequence header (the game demuxes the first chunk before them), data after the end starts
  a new pass (looping), and the libmpeg end flag (`*mp->sys`, read by the game's unbound `sceMpegIsEnd`) is published.
  Also: toSPR/fromSPR interleave-mode DMA (D_SQWC; the PSS audio path deinterleaves with it), and pending invocations
  (SIF commands) may run while a thread executes an HleCall invocation (the nodata callback blocks on file reads).
- `0005-vif1-direct-image-continuation.patch`: fixes garbled text and dithered sprite edges, in both the kzgs GS and
  the CPU GS. A VIF1 DIRECT can end with a PATH2 IMAGE GIFtag whose pixel data arrives in a later DIRECT, usually
  `MARK; DIRECT n` in the next DMAtag's TTE words. The runtime used to treat the bytes right after the first DIRECT as
  that pixel data. Those bytes were the VIFcodes, so every such upload (fonts, CLUTs, UI atlases) was shifted by 8
  bytes: 2 CT32 or 16 T4 pixels. The last 8 bytes of the data were then parsed as VIFcodes. Now VIFcodes are always
  parsed. Only the payload of a DIRECT is re-wrapped in a synthesized IMAGE tag when an image is pending. Raw
  continuation is kept only for a DIRECT cut off at the end of a DMA buffer (`m_vif1PendingDirectQwc`). Verified byte
  for byte against real PCSX2's EE RAM through a PINE savestate.
- `0006-spu2-host-audio.patch`: SPU2 hook for sound (`ps2xIOP/include/ps2x/iop/iop_host_spu2.h`). With a host SPU2
  installed (`src/kz_audio.cpp` installs kzspu2), the IOP emulator routes these to it:
  - SPU2 register accesses (0x1F900000-0x1F9007FF; 32-bit accesses are split into 16-bit ones);
  - IOP DMA ch4/ch7 starts. CHCR.TR stays set until the SPU2 finishes, and MADR reads report its progress;
  - the host's SPU2 IRQ and DMA-end requests, which become IOP interrupts 9, 0x24 and 0x28.

  `runCycles` advances the SPU2 every time slice. While all threads sleep, it does not skip past the SPU2's next DMA
  event, or more than 32 samples. Without hooks, the old stub (fake DMA-end interrupt, no sound) is unchanged.

  Two IOP emulator fixes that Killzone's sound needs (independent of the hook):
  - `sceSifGetOtherData` (sifcmd #23) now copies EE memory into IOP RAM. It was a no-op, and PSOUND_R.IRX pulls
    every sound bank from EE RAM with it.
  - `WaitEventFlag` from code that runs outside an IOP thread (module start, RPC server functions) used to return at
    once. It now advances IOP time to the next pending interrupt, guest callback or SPU2 event until the flag is
    set, giving up if nothing pending could set it or after 1 s of IOP time.
    Returning early left libsd's "transfer done" flag set by the later DMA interrupt. The flag was then stale, so
    the next `sceSdVoiceTransStatus` reported a running upload as finished, and every following `sceSdVoiceTrans`
    was refused as busy.

- `0007-vu0-microvu.patch`: VU0 micro mode hook, plus a VCALLMSR fix.
  - **Host VU0 (`include/runtime/ps2_host_vu0.h`, `ps2SetHostVu0`).** When a host VU0 is installed before
    `initialize()`, the runtime's VU0 code/data memory is the host's (not freed by the runtime), and
    `executeVU0Microprogram` (VCALLMS, VCALLMSR) hands the call to it: `callms(ctx, startPc, codeGeneration)` runs
    the program to completion with the COP2 registers in `ctx` as VU0's register file. Without a hook, or while the
    runtime's VU0 memory is not the host's, the built-in interpreter runs as before. `src/kz_vu.cpp` installs
    kzvu's microVU0 (`KZ_VU0=interp` keeps the interpreter).
  - **VCALLMSR start address.** Generated code computes it from `ctx->vi[27]`, but `vi[]` only holds the 16 integer
    registers, so that read lands inside `vu0_r`. CTC2 to $vi27 writes `ctx->vu0_cmsar0`, so `vu0StartMicroProgram`
    (called only for VCALLMSR) now starts at `(vu0_cmsar0 & 0x1FF) * 8`. Killzone uses VCALLMSR at 0x31A8D8 and
    0x31A994 (FUN_0031a068). The generated code itself is unchanged (regenerating means a full rebuild).

- `0008-runtime-unwind-arena-iop-import-cache.patch`: stability and speed fixes found while getting into gameplay.
  - **Yield/return confusion (random crashes).** `dispatchGuestBranch` treated "callee came back with pc == its own
    entry" as a normal return. A checkpoint yield at a recursive call (Lua) or at a loop head that is the function's
    first instruction produces exactly that pc. The caller then resumed early and the abandoned callee left
    garbage return addresses (jumps to heap addresses like 0x7F82A0). A global `g_ps2GuestUnwinding` flag is now
    set by every yield and every non-return exit; dispatch propagates the unwind, and the scheduler clears the flag
    before it enters guest code. Reproduced with `PS2X_STRESS_YIELD=50` (a new debug env that forces a yield at
    every Nth checkpoint): every run crashed within seconds before the fix, and none did in 120 s after it.
  - **Scheduler trace.** A ring of scheduler entries/exits/interrupt invocations, printed when the scheduler hits
    a pc with no function (debug aid).
  - **Runtime arena (`include/runtime/ps2_host_arena.h`, `ps2SetRuntimeArena`).** The runtime's own guest
    allocations followed the game's `SetupHeap`, and interrupt stacks sat at the top of RAM. Killzone's dlmalloc
    owns 0x5913F8..0x1FFFFF0, so both overwrote its heap (the level-load heap walk then looped forever on a
    zeroed chunk). The host now moves them to a region the game never uses (Killzone: 0x80000-0x100000).
  - **IOP import decode cache.** `IopImportRegistry::decode` runs on every IOP instruction and scanned up to 64 KB
    back for each stub's import table; resolved stubs are now cached per pc (cleared on reset/unload).

- `0009-ps2-float-semantics.patch` (recompiler + `ps2_runtime_macros.h`, needs regen + full rebuild):
  - `RSQRT.S fd, fs, ft` was emitted as `1/sqrt(fs)`; the EE computes `fs / sqrt(ft)`. Killzone uses it in 382
    functions (vector normalisation), so normals and lighting came out wrong. VU0 macro `VRSQRT` had the same bug
    (ignored fs).
  - PS2 float semantics for EE FPU and VU0 macro ops: no Inf/NaN (exponent-255 inputs and overflow saturate to
    +-max), x/0 = +-max with sign fs xor ft (was Inf for DIV.S and 0 for VDIV), SQRT of |x|. Denormals are flushed
    by the host setting FTZ/DAZ on the game thread (src/kz_main.cpp).
  - `CVT.W.S` rounded to nearest; the EE truncates toward zero and saturates.

- `0010-vif1-unpack-dma-chains-quadword-loads-mmi.patch` (recompiler + runtime; the recompiler part needs
  `build_ps2recomp.bat`, `regen.py` and a full rebuild). Makes Killzone's gameplay 3D render. Before it, the first mission
  showed only the HUD and post-effect passes over a flat red scene buffer.
  - **LQ/SQ/LQC2/SQC2 force a quadword address** (`instruction_translator.cpp`). The EE ignores the low four address
    bits, and the generated code did not. The game loads unaligned vectors with `lq t0,0(a); lq t1,16(a); mtsab a,0;
    qfsrv t0,t1,t0` (371 QFSRV sites). With unaligned `lq` the second vector came out as neighbouring stack words. One
    effect: FUN_002c84d8 (AABB x matrix) returned max = translation, every RenderZone box was flat (zone +0xAC max
    54/-416/-17.3 vs PCSX2's 159/-270/37), the camera was in no zone, and no world object was submitted
    (108 MSCAL per frame vs 915 in PCSX2).
  - **SQRT.S reads ft** (`fpu_translator.cpp`). It read fs, which is 0 in the encoding, so ~430 sites took sqrt(f0).
  - **MMI** (`mmi_translation_helpers.cpp`). PEXEW swapped adjacent words instead of words 0 and 2. PEXEH, PEXCH, PEXCW
    and PREVH had the same kind of lane errors. PMULTH summed the products instead of storing eight of them in 128-bit
    LO/HI. PMFHL.SH ignored LO1/HI1. After the alignment and SQRT fixes, the player still fell through the floor (z
    -1.7e5 after a minute; PCSX2 2.659). With these fixed, the player stands at PCSX2's position.
  - **VIF1 UNPACK with PCSX2's semantics** (`ps2_vif1_interpreter.cpp`, `vif1UnpackPcsx2`):
    - WL=0 means 256.
    - In fill mode the source pointer only advances for CL writes.
    - V2 writes xyxy. V3 reads four elements.
    - V4-5 expands to 8 bits per channel.
    - STMOD 3 is supported.
    - Mask rows and columns follow the write cycle.
    - Source size uses PCSX2's formula.

    `PS2X_VIF_UNPACK=legacy` keeps the old code. With it, the world draws with bent and stretched geometry, and the
    game ran at ~2.5 frames/s in one run.
  - **DMA chains are no longer cut at 4096 tags** (`ps2_memory.cpp`). A gameplay frame is one VIF1 chain of ~4400
    tags. The rest of the chain was dropped and CHCR still read back as done. Symptoms: noisy textures, white screens,
    runaway VU1 programs (2.5e9 VU1 cycles in 10 s), and host crashes (access violation, heap corruption) seconds into
    gameplay.
  - **Scratchpad DMA chain mode** (toSPR source chain with TTE, fromSPR destination chain). Before, CHCR only read back
    as done and nothing was copied. Used in the menus, not in gameplay. `PS2X_SPR_CHAIN=0` disables it.
  - **SPR bit (bit 31) in MADR/TADR/DMAtag ADDR** selects the scratchpad at `addr & 0x3FF0`, as in PCSX2 `dmaGetAddr`.
    `PS2X_DMA_SPR_BIT=0` disables it.
  - **Debug aids:**
    - `PS2X_DMA_STATS=1` prints a `[dma]` line per headless heartbeat: DMA starts per channel and mode, VIF1
      chains/tags/bytes, the tag-limit count, UNPACK formats, MSCAL, and XGKICK count/bytes/first PRIM from kz_vu.
      The counters are in the new header `runtime/ps2_dma_stats.h`, which generated code does not include.
    - `PS2X_VIF1_DUMP=<dir>` (with `_AFTER=<s>` and `_MAX=<n>`) writes every VIF1 buffer, the VU1 micro memory and
      the scratchpad. Read the dumps with `tools/scripts/vifparse.py`.

- `0011-iop-batched-advance.patch`: `PS2Runtime::advanceIopEeCycles` ran the IOP on every EE checkpoint (nearly every
  guest call) with a few cycles, paying the fixed per-run cost (SPU2 advance, service checks, thread selection) each
  time; the IOP was ~50% of the game thread in-game. It now runs in quanta of `PS2X_IOP_BATCH` EE cycles (default
  4096, ~14 us). Killzone in-game: ~14 -> ~18 frames/s; menu and in-game audio unchanged (WAV capture).

- `0012-iop-scheduler-idle-fastpath-exclusive-host-gs.patch` (performance; in-game Killzone ~13 -> ~22 frames/s):
  - IOP kernel keeps a flat thread list (map walks in `beginNextReady`/`nextWakeCycle` dominated IOP time) and
    merges the wake/select passes; dead-thread cleanup is skipped when nothing died.
  - IOP idle fast path: when an idle pass finds nothing can happen before cycle X, later `runCycles` calls that stay
    before X only advance the clock; any external entry (RPC, SIF transfer, memory writes, module ops, reset)
    invalidates it. `PS2X_IOP_IDLE_FASTPATH=0` disables.
  - EE `accountCycles` samples the host clock every 64th call instead of on every checkpoint.
  - `PS2HostGs::exclusive`: with a host GS attached, the built-in software GS no longer processes GIF packets
    (except packets that write SIGNAL/FINISH/LABEL, whose CSR bits games poll).
  - `PS2X_IOP_BATCH` (patch 0011) now defaults to 1 (off): larger batches correlated with boot hangs; the
    re-entrancy of the batch counter was also fixed.

- `0013-iop-rpc-semaphore-wait.patch`: fixes the intermittent boot/loading hang (game thread spinning at 0x14A028 in
  FUN_00149f30, waiting for the FilePS2 read counters at +0x34/+0x38 to reach 0).
  - **Cause.** RPC server functions run synchronously outside any IOP thread (`IopRpcBridge::handleRpc` ->
    `callFunction`). `WaitSema` there returned at once without taking the semaphore when its count was 0.
    PFILE_R.IRX's RPC (fn 0x320/0x330, read requests) appends to its streaming queue under semaphore `0x409c`
    (FUN_00001c1c). The streaming thread (entry 0x22A4) removes finished requests under the same semaphore, and IOP
    threads are preempted wherever their slice ends. At 0x26C4..0x26D8 (module offsets) the streaming thread has
    loaded `next = cur->next` (0) but not yet run `if (next == 0) tail = 0`. An append in that window links the new
    request behind the finished one, and the streaming thread then clears head and tail: the request is dropped, no
    completion SIF command is ever sent, and the EE counter never reaches 0. The queue counters at `0x406C`/`0x4070`
    are also updated with unlocked read-modify-writes on both sides.
  - **Evidence** (`PS2X_IOP_SYNC_TRACE=1`, `PS2X_IOP_BATCH=4096`, fix disabled, 3 of 8 boots hung). In each hung boot,
    the last contended `WaitSema(4)` from the RPC found the streaming thread preempted at 0x126C8, 0x126D0 and
    0x126D0 (PFILE_R loads at 0x10000). The next once-per-second dump showed head 0x14094 = 0, tail 0x14098 = 0 and
    queued count 0x1406C = 1 in two of them, with the streaming thread idle in its DelayThread poll. In the third
    boot the count read 0; the unlocked counter update can lose that increment. In three of the five good
    boots a contended wait also landed in that window without a hang. Whether the loss happens depends on the
    loaded `next` being 0 and on a later append arriving while the orphan is still queued. The same
    boots show ~12-20 contended outside-thread waits per boot on PSOUND_R.IRX's semaphore (ra 0x71260).
  - **Fix.** `WaitSema` outside a thread on a semaphore with count 0 now runs the IOP scheduler (threads, due
    interrupts, callbacks, timers) until the semaphore is signalled, then takes it. This is what the real IOP does
    when the RPC server thread blocks. While such a wait is active, `SignalSema` keeps the count for the waiter and
    ends the signalling thread's slice, because the RPC thread outranks it. The wait is not used from interrupt
    handlers or guest callbacks, or while the scheduler is already running a thread. It gives up after 1 s of IOP
    time and prints `[iop-sync] WaitSema(n) outside a thread ... TIMED OUT`. `PS2X_IOP_OUTSIDE_SEMA_WAIT=0` restores
    the old behaviour.
  - The body of `runCycles`' loop is now `scheduleStep()`, shared with the wait (same logic).
  - **Tracing** (new private header `iop_trace.h`): `PS2X_IOP_SYNC_TRACE=1` logs contended outside-thread
    semaphore waits (with all thread states and pcs) and, once per IOP second, the thread and semaphore states plus
    the IOP words listed in `PS2X_IOP_WATCH` (comma-separated). `=2` also logs every RPC request (first 16 words)
    and every IOP->EE SIF command (for FilePS2: the EE counter address and value).
  - Measured (headless boots, 50 s, `KZ_FPS=60 KZ_IPU=off`; hung = dma constant from t=30 s to t=40 s):
    `PS2X_IOP_BATCH=1`: 5 of 12 hung before (4 at 0x14A028, 1 at 0x14A1C0), 0 of 12 after.
    `PS2X_IOP_BATCH=4096`: 3 of 8 before (plus 3 of 8 with the level-1 trace), 0 of 16 after.
    With the fix and the trace, one 4096 boot resolved 14 contended PFILE_R waits in 1-31 IOP cycles and 22
    PSOUND_R waits in 42-3840 cycles. No boot hit the 1 s timeout.

- `0014-jalr-resume-only-return.patch` (recompiler, needs regen): an unresolved indirect *call* (`jalr`) made every
  instruction of the calling function an entry point (label + switch case per instruction in ~4000 functions), as if
  it were an unresolved local jump. A call only needs its return address as a resume point. Same fix as the
  PS2Recomp DQ8 fork (PR #254). Together with release-grade compiler flags for RelWithDebInfo (top-level
  CMakeLists.txt: /Ob3 /GS- /arch:AVX2 /Qspectre- /Gy /Gw, /INCREMENTAL:NO /OPT:REF,ICF): Killzone in-game
  18.6 -> 29.4 frames/s at 60 Hz vblank (headless, same scene).

- `0015-iop-thread.patch`: the IOP emulator and SPU2 mixing run on their own host thread (`PS2X_IOP_THREAD`, default 1;
  `PS2X_IOP_THREAD=0` keeps the synchronous path of patches 0011-0013, run from `EeScheduler::accountCycles`).
  Touches only `.cpp` files and headers that generated code does not include (no full rebuild). Design:
  - **Time.** `PS2Runtime::advanceIopEeCycles` no longer runs the IOP. It sums the EE cycles `accountCycles` reports
    (already clamped to wall time) and publishes the total with one atomic store per 2048 EE cycles
    (`IopSubsystem::publishEeCycles`). The IOP thread (started by the first publish; `_beginthreadex`, 4 MB stack,
    FTZ/DAZ set like the EE thread) consumes the difference, IOP time = EE/8 as in `IopEmulator::runEeCycles`, so it
    never runs ahead of the published EE clock. It runs in chunks of 4096 IOP cycles (~110 us) with a lock, hands the
    lock to any EE-side caller that queued for it (`execWaiters`; `std::mutex` is not fair), and sleeps ~100 us
    (high-resolution waitable timer) whenever less than 2048 IOP cycles are pending.
  - **EE -> IOP.** `IopSubsystem::enableThread(true)` puts every public entry point behind one recursive lock
    (`Impl::ExecGuard`): `reset`, `loadModule`, `loadModuleBuffer`, `stopModule`, `runEeCycles`, `selectRpcAbi`,
    `canBindRpc`, `handleRpc`, `onSifTransfer`, `allocate/free/read/write/zeroMemory`, `debugSnapshot`
    (`isMemoryRange` only does arithmetic). Recursive because handleRpc -> HLE service -> host -> `readIopMemory`
    re-enters. A synchronous RPC therefore still runs the RPC server function inline on the EE thread while it holds
    the lock (including the outside-thread `WaitSema` / `WaitEventFlag` scheduler runs of patches 0006/0013, which
    advance IOP time on the EE thread; that time is not paid back, exactly as before). Lock order is always
    `PS2IopHostAdapter::m_callMutex` -> IOP lock; the IOP thread takes only the IOP lock.
  - **IOP -> EE.** The IOP emulator reaches the EE only through `IopHost`. Everything the IOP thread does there:
    `readGuest`/`writeGuest`/`zeroGuest` (SIF DMA `sceSifSetDma`, the extra data of `sceSifSendCmd`,
    `sceSifGetOtherData`) touch EE RAM directly (a plain memcpy, like the real SIF DMA engine; the adapter ignores the
    EE thread's call scope on the IOP thread); host file reads and `log` were already thread-safe; `sendSifCommand` no
    longer calls `dispatchSifCommand` (guest heap allocation + `EeScheduler::queueInvocation`, EE-thread only). It
    queues the packet in `PS2IopHostAdapter` and posts one `EeEventType::Dmac` event with id `kIopPostedWorkEventId`
    (`ps2_iop_post.h`); `EeScheduler::processEvent` calls `ps2IopDrainPosted` on the EE thread, which dispatches the
    queue in order. The IOP never raises EE interrupts or completes RPCs itself (RPC results and end callbacks stay
    on the EE thread inside `handleRpc`), so there is nothing else to post.
  - **SPU2.** `iop_host_spu2.h` hooks and `kz_audio.cpp` state now run on the IOP thread (or on the EE thread while it
    holds the lock); calls are serialised, the SDL audio thread only reads the atomic ring. `kz_audio`'s wall-clock
    pacing is unchanged.
  - **Debug.** `PS2X_IOP_THREAD_STATS=1` prints `[iop-thread]` lines every 5 s: IOP thread busy %, published-consumed lag,
    IOP time lead over EE/8, EE lock calls/wait, and EE-side drain counts.
  - **Measured** (headless, `KZ_FPS=60 KZ_IPU=off`, scripted input, in-game frames/s = `vif=` counter delta over
    t=170..230 s; the machine is shared with other builds/runs, so absolute numbers move by +-20 % between runs and only
    interleaved pairs are compared):
    - Pristine pre-patch binary vs threaded, same source otherwise, 3 pairs: 30.8 / 27.8 / 28.0 (mean 28.9) ->
      31.4 / 32.2 / 32.0 (mean 31.9), +10 %. With `KZ_FPS=240` (vblank quantisation of the frame rate is 4x finer),
      2 pairs: 28.2 / 25.9 -> 37.8 / 31.9 (mean 27.1 -> 34.9, +29 %). At 60 Hz the frame rate is quantised by the
      vblank (a frame that saved 5 ms but still misses the next vblank gains nothing), which hides part of the gain.
    - Same binary, `PS2X_IOP_THREAD=0` vs default, built with the VIF1 worker of patch 0016 also active, 3 pairs:
      41.1 / 39.1 / 39.9 -> 41.8 / 42.2 / 50.1 (mean +12 %).
    - The IOP thread itself is busy ~7-11 % of one core in gameplay (SPU2 mixing is ~29 % of that); the game thread
      keeps only the RPC server functions it runs inline (`handleRpc` 2.3 % inclusive in a 1 kHz profile).
    - Boot: 24 headless 50 s boots, none hung (`dma=` advances in every heartbeat), 8 of them with 4 instances
      running at once. Gameplay: 9 full 235 s runs across the builds, all render the level; 2 more with the final
      build (patches 0015+0016) run side by side. SPU2 WAV (`KZ_AUDIO_WAV`) vs the pre-patch build over the first
      230 s: silent until t=20 s, then music/UI as before; identical sample peaks in 10 of 11 ten-second menu
      segments (t=20..120 s), per-10 s RMS within 1 dB in the four final-build runs (the first threaded run had one UI
      sound land in the neighbouring 10 s window), overall RMS -13.7 dBFS (pre-patch) vs -13.8 / -14.1 / -13.9 / -13.9
      (four threaded runs); menu-music waveform correlation with the pre-patch run 0.998-0.999 at 5-13 ms offset (the
      synchronous path has the same offset). In-game levels differ by scene timing (explosions), as they do between
      two synchronous runs.
    - The IOP runs ~6 % ahead of EE/8 (14 s in a 230 s run, `PS2X_IOP_THREAD_STATS`): outside-thread waits inside
      RPCs and module starts advance IOP time on the EE thread and are not paid back. This was already true for the
      synchronous path (kz_audio's pacing factor exists for it).

- `0017-codegen-register-locals.patch` (recompiler + new header `ps2xRuntime/include/ps2_cg.h`; needs `build_ps2recomp.bat`,
  a regen with `PS2X_CODEGEN=locals`, and a full rebuild). An alternative code generator; the classic output is unchanged
  when `PS2X_CODEGEN` is unset (verified byte for byte against `generated/`). `tools/scripts/regen.py` takes
  `KZ_GEN_DIR` / `KZ_GEN_TMP` / `KZ_MERGED` / `KZ_RECOMP_EXE` / `KZ_RECOMP_LOG` so the output can go elsewhere, e.g.
  `PS2X_CODEGEN=locals KZ_GEN_DIR=D:/KillzoneRecomp/work/generated_cg python tools/scripts/regen.py`.
  `PS2X_CODEGEN_FUNCS=<file>` (one hex function start per line) restricts it to those functions; the rest stay classic
  (both kinds share translation units, all new macros are `L_`/`ps2Cg` prefixed).
  - **Guest GPRs are C++ locals.** Every function loads the low 64 bits of the registers it touches (`gprN`; `gprhN`
    for registers used by 128-bit ops: LQ/SQ/MMI) from `ctx->r[]` at entry (also after a resume `switch`), and writes the
    modified ones back (`PS2_FLUSH`) before anything that can observe `ctx`: calls, syscalls and stubs, returns, yields,
    exits through an external branch. `PS2_RELOAD` follows calls and stubs. The C++ optimizer removes reloads of dead
    registers. The generator emits classic macro text first, then `rewriteForLocals` (function_emitter.cpp) rewrites it
    (`GPR_U32(ctx, 4)` -> `L_GPR_U32(4)`, writes to $zero -> `PS2_DISCARD`) and records the register sets. MOVZ/MOVN move
    only the low 64 bits in this mode (as the EE does). Memory slow paths (MMIO Load/Store) and `drainCompletedDmacHandlers`
    do not read GPRs, so plain loads and stores need no sync.
  - **`ctx->pc` is no longer stored per instruction.** It is set where somebody reads it: before statements that call
    into the runtime (`runtime->`, `ps2_stubs::`, `ps2_syscalls::`), before calls, at returns and yields, and when a
    jump targets a delay slot address (the resume-from-delay-slot check reads it). The `in_delay_slot`/`branch_pc` stores
    are only emitted for delay slots that call the runtime.
  - **Direct calls with an inline budget** (`ps2CgCall`). JAL/JALR read the function table directly (hooks installed with
    `replaceFunction` still apply) and keep `dispatchGuestBranch`'s unwind protocol: `g_ps2GuestUnwinding`, the return pc
    check and the `ctx->pc == callee entry` rule (slow path `ps2CgAfterCallSlow`). Branch history, `hasFunction`,
    `lookupFunction` and the per-call `checkpointDue` are gone from the fast path. Calls and loop back edges subtract 8 and 32
    from `g_ps2EeBudget`; when it runs out the accumulated charge is passed to `eeCheckpointDue` (`ps2CgBudgetSlow`).
    Refill is 256 cycles (`PS2X_EE_BUDGET` overrides; `PS2X_STRESS_YIELD` forces 1 so every check is a scheduler
    checkpoint). 1024 hung the movie player (KZ_IPU on): the IOP file read never completed, 3 of 3 runs, while 64, 256 and
    512 booted to the menu (3 of 3 at 256 after the final build). The control build is fine with `PS2X_IOP_BATCH` up to 4096,
    so that is a coarse-EE-charge interaction with the IOP, not a guest-code bug.
  - **Memory access**: `L_READ*`/`L_WRITE*` are inline functions with one range compare (`addr <= 32 MB - size`) and a
    typed access; everything else takes the old special-address path out of line.
  - `-DPS2_CG_CLASSIC_CHECKPOINTS` builds the runtime side with the old per-call/per-back-edge scheduler charge (measurement
    variant: locals without the direct-call/budget part).
  - Compile cost: the locals output is smaller (FUN_00153d28, the largest function: 1.4 MB object vs 2.7 MB, 20 s vs 42 s to compile), and the
    executable is half the size (102 MB vs 211 MB).

- `0016-vif1-worker-thread.patch` (runtime; the rest of the change is in the main repo: `ext/kzvu`, `src/kz_vu.cpp`,
  `src/kz_gs.cpp`, `src/kz_main.cpp`): VIF1 DMA, VU1 (microVU1), XGKICK and the GIF arbiter run on their own worker
  thread, the way PCSX2's MTVU does for VU1. In-game (headless, 60 Hz vblank, same scene, no other game instance or compile
  running, `vif=` counter t=170..232 s): ~31 -> ~46 frames/s with the IOP thread on (31.3 -> 46.0, and 24.4 -> 44.9 in a second pair that ran on a busier machine); the new synchronous path measures the same as before (32.0 and 30.7 against 31.3) (details and the runs behind them: docs/findings.md, "VIF1 worker thread").
  `PS2X_VIF1_THREAD=0` keeps the old synchronous path. Threaded is the default whenever a host VU1 is installed (its JIT
  is tied to the thread that runs it, so the mode is fixed at startup; with `KZ_VU=builtin` the runtime stays synchronous).
  - **Design.** A DMA start on D1 (VIF1) or D2 (GIF) still walks the DMA chain on the EE thread. That copies the source
    data into the chain buffer, exactly as before, so the game may reuse its frame lists as soon as the CHCR store
    returns (a normal-mode MADR/QWC transfer is copied chunk by chunk at the start instead of being read later). The
    buffers go to the worker as one job (`ps2_vif1_worker.cpp`, new). Jobs run in FIFO order and do what the
    synchronous `processPendingTransfers` did: PATH3 packets first (undrained), then `processVIF1Data` per buffer (UNPACK,
    MPG, MSCAL/MSCNT on the host VU1, DIRECT -> PATH2, XGKICK -> PATH1), then the arbiter drain. So the host GS sees the
    same packet order as before. From the first job on, the worker owns: VIF1 registers/state, VU1 code+data memory,
    the host VU1, the GIF arbiter, the PATH3 mask state. VIF0 (VU0) transfers stay synchronous on the EE thread.
  - **Completion.** CHCR.STR of D1/D2 reads busy until the job is done (`readIORegister`; the old code always read STR
    as idle). The worker records the completion and posts an `EeEventType::Dmac` event (`kVif1WorkerEventId`); the EE
    applies it (STR/QWC clear, D_STAT bit, `queueCompletedDmacCause`, DMAC handlers via `drainCompletedDmacHandlers`)
    from `EeScheduler::processEvent`, or lazily on the first read of D1/D2 CHCR or D_STAT. A DMA started on a busy
    channel queues behind the old one (per-channel outstanding count).
  - **EE <-> worker interaction points.** Each of these waits until the worker is idle ("barrier"), unless stated:
    VU1 code/data access through `mapVuMemory` (all read/write8..128); VIF1 register writes (FBRST, MARK, CYCLE, MODE,
    NUM, MASK, CODE, ITOPS, BASE, OFST, TOPS, ITOP, TOP; a write to STAT or ERR only touches the register map, no
    barrier); GIF_STAT reads (M3P is the worker's PATH3 mask); VIF1_FIFO writes (the EE-side `processVIF1Data`; its MSCAL
    still runs on the worker, as a blocking call); `submitGifPacket`/`processGIFPacket` from the EE; the public
    `processPendingTransfers()` (HLE stubs, VIF0). D1/D2 CHCR and D_STAT reads only apply finished work, they do not
    wait. GS frame boundary: the scheduler used to call the host `vsync` hook at each guest vblank. Now the kz hook does
    its EE-side work (guest patches, timing) inline, and sends its GS half (`kzgsVsync`, with the private-register
    snapshot taken on the EE thread) through `ps2RunAfterQueuedGif`: it runs on the worker behind the frame's packets,
    and the EE does not wait (`PS2HostGs::vsyncOrdered`; without it the scheduler waits for the worker at every vblank).
    SIGNAL/FINISH/LABEL packets still reach the built-in GS through the arbiter (`packetWritesGsEvent`), on the worker;
    they set CSR with atomics that the EE polls.
  - **Worker thread.** MXCSR copied from the game thread (FTZ/DAZ); above-normal priority and no EcoQoS throttling
    (`ps2_vif1_worker_win.cpp`); `PS2X_VIF1_CORES=perf` also restricts it to the P-cores of a hybrid CPU (off by default:
    no measurable difference on the i9-12900K). It spins ~100 us for the next job before sleeping: loading screens start
    a GIF DMA only after the previous one reads idle (hundreds per second), so job latency matters there.
  - **kzvu split (main repo).** VU0 micro mode stays on the EE thread, so microVU0 and microVU1 now run concurrently.
    VU1 code reads and writes VU0's VPU_STAT and FBRST words, and both VUs do unlocked read-modify-writes on them.
    `kzvuBindVu1Thread()` (called by the worker through `PS2HostVu1::bindWorkerThread`) makes `VU0` (PCSX2's
    `static VURegs& VU0`) a per-thread pointer in the kzvu sources: the VU1 thread gets a private VPU_STAT/FBRST (the
    JIT embeds the address at compile time on the compiling thread, so microVU1 code compiled there uses it), and
    FBRST / pending-IRQ / host-FPCR state is thread-local. This does what the THREAD_VU1 special cases in PCSX2 do (skip
    those writes for VU1). VU1 must stay on one thread once code was compiled.
  - **Render-feeding costs on the EE thread** (measured with `PS2X_VIF1_STATS=1`, same binary, env A/B):
    the DMA chain walk + snapshot was 6.2-6.7 % of the EE thread (fresh 3 MB vector per frame, grown by `insert` per tag,
    freed on another thread: page faults and copies). Chain buffers now come from a pool (`ps2ChainBuffer*`) sized by
    the previous chain of the channel: 1.8-2.0 % (`PS2X_CHAIN_POOL=0` = old behaviour). `vif1UnpackPcsx2` had 5.0 % of
    the EE thread in synchronous mode (16 % of the worker's busy time threaded): plain UNPACK (no mask, STMOD 0,
    WL <= CL: masked UNPACKs are ~13 % of Killzone's ~8700 per frame) now does one SSE load/convert/store per vector, 1.0 %
    (`PS2X_VIF_UNPACK_FAST=0` = old loop; `PS2X_VIF_UNPACK_CHECK=n` runs every n-th UNPACK both ways and compares VU1 memory
    and VIF registers: 5.8 M UNPACKs over a boot + gameplay run, 0 differences). An MPG that does not change VU1 code
    memory no longer bumps the code generation (`PS2X_MPG_SKIP_SAME=0` = old); only ~5 % of Killzone's ~3500 uploads
    per second are identical, the rest alternate between programs.
  - **microVU1 recompiles** (`KZ_VU_STATS=1`, every 10 s: MSCAL calls, calls that emitted code and how much, cache
    resets, new programs; `KZ_VU_STATS_DIFF=n` logs the first differing micro-memory word of the first n new programs).
    Cause of the compile time seen in gameplay profiles: microVU keeps one program list per MSCAL start PC and
    Killzone's skinning/lighting programs are entered at ~540 different start PCs (entry addresses 0x30 bytes apart,
    which looks like an unrolled loop entered `6*n` instructions before its end), times several microcode versions: ~3700 programs / 22 MB of JIT in the first 100 s of
    the first level (new programs per 10 s: hundreds while new content appears, 15-60 in between; 0 cache
    resets in 235 s, 22 of 61 MB used). No program was created for content identical to a cached one: the 411 logged
    creations that had an older program for the same PC all differ in the compiled ranges. So it is first-use compile
    cost, not recompilation, and it is off the EE thread now (worker: `mVUcompileJIT<1>` ~4 % of a core in that window).
    Unresolved: a long session may fill the 61 MB cache (microVU resets and recompiles everything).
  - **Switches / debug.** `PS2X_VIF1_THREAD=0` (synchronous), `PS2X_VIF1_STATS=1` (`[vif1-thread]` line every 10 s: jobs,
    bytes, MPGs (same), EE chain-walk time, EE dispatch time, worker busy time split into VU1 and host-GS hook,
    barriers per reason with wait counts), `PS2X_VIF1_VSYNC_SYNC=1` (wait for the worker at every vblank),
    `PS2X_VIF1_CORES=perf`, `PS2X_VIF1_DELAY_US=n` (busy-wait before every job: slow-worker stress test),
    `KZ_GS_HASH=1` (per-vsync packet count + FNV hash of the GIF stream), `KZ_PROFILE_OUT=<file>`, and the A/B switches above.
  - **Known limits.** Guest FBRST (VU1 D/T-stop enables) is sampled once per vblank, and the D/T stop bits of VPU_STAT
    are not propagated to the guest in threaded mode (Killzone never enables them). VU0 microcode that reads VU1
    registers through VU0 memory (0x4000+) is not synchronised with the worker (none of the 103 captured Killzone VU0
    calls touches them). GIF/VIF1 completion interrupts now arrive ~1-15 ms after the CHCR store instead of inside it.

- `0018-scheduler-event-fastpath.patch`: `EeScheduler::processPendingEvents` runs on nearly every scheduler iteration
  and took two locks, allocated/freed a `std::deque` and read the host clock (deadline scan) even with nothing to do
  (~24% of the game thread in a 120 Hz profile). Now: an atomic queued-event count skips the lock + deque when empty,
  the checkpoint flag is recomputed lock-free (re-checked against a concurrent post), and `processDueDeadlines`
  returns immediately while the EE cycle is below the earliest deadline. Killzone at 120 Hz vblank, gameplay-only
  window, 3 runs: ~46 -> 68-76 frames/s.

- `0019-vu0-sched-locks.patch` (runtime: `EeScheduler.cpp`, `ps2_memory.cpp`; the VU0 half is in the main repo:
  `src/kz_vu.cpp`, `ext/kzvu/{include/kzvu.h,src/kzvu.cpp,test/kzvu0_test.cpp}`). Trims the EE thread's per-call and
  per-dispatch fixed costs. Generated against the working tree as it was, which already carried uncommitted changes
  to `ps2_memory.cpp` (the GIF_STAT pointer cache and the `eeCycles < kEeClockHz` shortcut in `advanceEeTimers`, the
  DMAC-drain fast path); the hunks here need those as context. No header changed, no regen.
  - **Scheduler dispatch loop** (`EeScheduler::run`). Gameplay runs ~0.9-1.2 M scheduler dispatches per second (entry
    fragments and every return after a non-local unwind go through it), so per-dispatch work matters.
    (1) `processPendingEvents` was called twice per iteration (1.5-2.8 M/s) and only has work when an event was posted,
    a deadline or EE timer interrupt is due, a reschedule is pending or the checkpoint flag is set; those are now tested
    inline (300 calls/s left; `PS2X_EVENT_PUMP_ALWAYS=1` = old). (2) The dispatch's own 8-cycle charge went through a
    full `checkpointDue` -> `accountCycles` (timers, IOP clock, wall clock, COP0 Count) each time; it is now charged
    against the guest checkpoint budget like a call is (`g_ps2EeBudget`, ps2_cg.h) and passed on when the budget runs
    out. Total cycles charged are unchanged; events are still noticed at the top of the loop
    (`PS2X_DISPATCH_BUDGET=0` = old). (3) One dense-table probe replaces `hasFunction` + `lookupFunction` (two calls;
    the latter also pushed the dispatch history ring, a missing-function diagnostic that no longer records scheduler
    hops). (4) pc/ra/sp/gp debug publish every 8th dispatch. (5) The `PS2X_WALLCLOCK` / `PS2X_STRESS_YIELD` function-local
    statics became namespace constants (MSVC's thread-safe-init check is a TLS load per call), and `accountCycles`
    inlines the cached-thread lookup.
  - **Lazy EE timers** (`PS2Memory::advanceEeTimers`, `resetEeTimers`, `cyclesUntilNextEeTimerInterrupt`, timer register
    read/write). Timer state is only observable through its registers and interrupts, so `advanceEeTimers` now collects
    cycles and applies them when a timer register is read or written, when the earliest cycle at which any timer can
    raise an interrupt flag is reached (computed once per flush, `eeTimersNextEventCycles`), or when the scheduler asks
    for the next timer deadline. The tick split carries its remainder exactly, so chunking does not change the result;
    the interrupt is raised by the same checkpoint call as before. `PS2X_LAZY_TIMERS=0` = old,
    `PS2X_LAZY_TIMERS=verify` keeps an eager shadow copy advanced every checkpoint and compares it at every flush, at
    every register read and whenever the shadow raises an interrupt (10 s summary, see below).
  - **VU0 call** (`src/kz_vu.cpp`, `kzvu0Call` in kzvu). `onVu0Call` copied the COP2 file context -> `Kzvu0Regs` ->
    VU0 and back (four 496-byte copies, plus the flag seeding, per call, 0.4-0.6 M calls/s). `kzvu0Call` copies straight
    between `R5900Context` and VU0 (AVX copies, VI/flag seeding as before), and every VU0 access on the call path binds
    the per-thread `VU0` reference once instead of reloading a TLS pointer after each store. Semantics unchanged: the
    same registers, the same VF0/VI0 write-back, same TPC/VPU_STAT. `KZ_VU0_DIRECT=0` = staged path.
    `KZ_VU0_VERIFY=<n>` runs every n-th call through both paths from the same state and compares all registers, flags,
    TPC/VPU_STAT and VU0 data memory.
  - **Tests.** `kzvu0_test` (now 509 checks, 0 failures; `--clamp 0/1/2/3` all pass) compares `kzvu0Call` with
    `SetRegs/Execute/GetRegs` on all 103 captured game calls and on 16 randomized inputs each (random VF bits including
    NaN/Inf/denormals, flags, VI, R/I/Q, FBRST); mutation-checked (dropping the Q write-back gives 75 failures).
    `kzvu_test` 575 checks, 0 failures. `kzvu0_test --bench` times the old and new call path per captured program.
  - **Debug.** `PS2X_SCHED_STATS=1` (`[sched-stats]` every 10 s: yields by reason, dispatches/s, events, rdtsc split of
    `run()` time between guest code and scheduler, host time slept in vblank pacing; `=2` adds dispatch-pc
    histograms; `-DPS2X_SCHED_STATS_HOT` adds per-checkpoint counters). Helpers used for the numbers
    below: `tools/scripts/perf_run.ps1`, `perf_pairs.ps1` (variants run concurrently) and `perf_metric.py`.
  - **Measured** (headless, `KZ_FPS=120`, scripted input, window t=150..200 s; the machine was shared with 2-3 other
    game instances and builds, so every A/B is a pair of runs started together in the same binary with the switches above;
    V0 = `PS2X_LAZY_TIMERS=0 PS2X_EVENT_PUMP_ALWAYS=1 PS2X_DISPATCH_BUDGET=0 KZ_VU0_DIRECT=0`, V1 = defaults):
    - EE-thread host time per displayed frame (1000 ms minus the vblank-pacing sleep, divided by fps), 6 pairs:
      V0 median 14.05 ms, V1 median 13.68 ms; per-pair change -0.83, -0.83, -0.60, -0.48, -0.47, +0.06 ms (median -0.54 ms,
      -3.8 %). fps (unchanged, see docs/findings.md "EE thread frame budget"): V0 median 60.3, V1 median 59.6.
    - Scheduler per dispatch (rdtsc, each figure includes ~15-20 ns of instrumentation): V0 84-99 ns, V1 51-56 ns.
    - EE timers per checkpoint (rdtsc pair around `advanceEeTimers`, ~10 ns of it instrumentation): eager 28-30 ns, lazy
      14-15 ns.
    - VU0 call, host cost per call over the 11 captured start PCs (`kzvu0_test --bench`, min of 3 runs, includes the
      context copies the old path made): original 136-194 ns (median 160), `kzvu0Call` 64-117 ns (median 79).
  - **Correctness evidence.** `PS2X_LAZY_TIMERS=verify` + `KZ_VU0_VERIFY=8` over a full 235 s run: 102 508 timer
    comparisons, 2 014 interrupts and 200 M early returns checked, 5.04 M VU0 calls compared, 0 mismatches in both. Gameplay
    frames of V0 and V1 runs look identical (first-mission scenes). 88 of 88 20 s boots with the final binary passed the
    module-load phase (a separate pre-existing IOP module-loading flake is described in docs/findings.md);
    `PS2X_STRESS_YIELD=97` boots to the menu in both V0 and V1.
  - **Not changed / left.** The EE thread sleeps 10-47 % of the time in vblank pacing, and the VIF1 worker needs
    ~9-10 ms per frame (VU1 7.6-8.3 ms of it), more than a 120 Hz vblank period (docs/findings.md "EE thread frame
    budget"): fixed-cost trimming on the EE thread does not move fps by itself. The ~1 M dispatches/s come from tail jumps
    between `entry_*` fragments and the resume of every caller after a non-local unwind; a trampoline in
    `ps2CgAfterCallSlow` (ps2_cg.h, full rebuild) would remove most of them.

- `0021-iop-rpc-offload.patch` (runtime + IOP; no header that generated code includes, so no full rebuild): nowait SIF RPC calls
  to emulated IOP servers run on the IOP thread instead of inline on the EE thread (`PS2X_IOP_RPC_ASYNC`, default 1; `=0`
  restores the inline path; only with the IOP thread of patch 0015).
  - **Why.** Measured with the new `PS2X_IOP_RPC_STATS=1` (a `[iop-rpc]` table every 10 s: calls, wall time waiting for the IOP
    lock, wall time running the server function, IOP instructions, per sid / function / nowait / end function), headless gameplay
    scene at 120 Hz vblank, 235 s, before this patch: 40 k RPCs per run, all but 32 of them nowait, and one of them is the cost.
    `sid 0x06662012 fn 0x600` (the sound driver's per-vblank command list, no end function, 16-64 byte send buffer, ~64 calls/s in
    gameplay, ~130/s in the menus) takes 0.95-1.06 ms wall and ~46 k IOP instructions per call (that count includes the other IOP
    threads that run while the server function waits on a semaphore or event flag, patches 0006/0013): 3.4 s per 50 s of gameplay =
    6.8 % of the EE thread's wall time (`SifCallRpc` as a whole 7.5 %, 627 us per call). PFILE_R (`sid 0x66662012`) fn 0x400
    (same rate, ~11 us) and fn 0x320 (623 per run, ~16 us) are cheap. Blocking calls: 32 per run, ~7 ms in total, so they stay
    inline. Lock wait was not the cost (67 ms of 21.6 s): it is interpretation, including the SPU2 register hooks.
  - **How.** `SifCallRpc` (RPC.cpp) still decodes the call and fills the client/server structures on the EE thread. For a
    nowait call, `ps2IopRpcSubmit` (`ps2_iop_async.h`, implemented in ps2_iop_host.cpp) asks `IopSubsystem::canOffloadRpc(sid)`
    (an emulated server is registered for the sid: cached per sid, positive answers only, cleared by every module operation and
    reset), queues an `IopSubsystem::AsyncRpc` with a snapshot of the send buffer (the SIF DMA of the real thing) and returns 0 at
    once. The IOP thread runs the queue in FIFO order between its time slices, holding the IOP lock and outside any IOP thread
    (the environment of the inline path, so the semaphore waits of patch 0013 and the event-flag waits of patch 0006 work
    unchanged), calls `IopRpcBridge::handleRpc` with the snapshot (the reply goes straight into the EE receive buffer, like a
    SIF DMA), and posts the completion to the EE through the posted-work queue of patch 0015 (`PS2IopHostAdapter::postWork`,
    ordered with the IOP's SIF commands). On the EE thread `finishOffloadedRpc` does what the inline path did after `handleRpc`,
    in the same order: completion-semaphore signals, reply fix-ups, the client's busy flag (`sceSifCheckStatRpc` reports busy
    until then, as on hardware), the debug event, and the end function, which now runs as a queued interrupt-time invocation
    (`RpcCallback`, caller's gp) instead of inside `sceSifCallRpc`.
  - **Ordering.** A blocking `handleRpc`, `reset` and the module operations first run everything queued before them under the
    IOP lock, so calls stay in order. Calls to HLE-only servers (mcserv, dbcman, HLE libsd: they use EE-thread state) and
    blocking calls take the old path.
  - **Also.** `IopSubsystem::selectRpcAbi` (runs on every `SifCallRpc`) took the IOP lock; with a 1 ms server function on the
    IOP thread every EE call waited for it (1 233 waits > 1 ms, 3.2 s of EE wait in one run). It only reads the constant service
    list and which HLE modules are loaded (changed only on the EE thread), so it takes no lock now (EE-side lock acquisitions
    per run: 80 k -> 75).
  - **Measured** (headless, 120 Hz vblank, scripted input, 235 s; the machine ran 4-12 other game instances at 50-85 % CPU
    load, so absolute frame rates were 35-65 and only runs made at the same time compare; docs/findings.md "IOP RPC offload"):
    EE-thread time in `SifCallRpc` 24.9 s -> 0.32 s per 235 s (avg 621 us -> 8 us per call). CPU per frame, offload vs
    `=0` on two copies of one binary run at the same time, 4 pairs: EE thread -0.5 / -1.7 / +0.1 / -2.1 ms (mean -1.0, noise
    ~1 ms), IOP thread +1.1 / +0.9 / +1.3 / +0.9 ms; frame rate unchanged within noise (~60 fps is vblank-quantised at 120 Hz).
    Offloaded calls wait 0.7 ms on average in the queue. IOP thread busy 7-10 % -> 17-19 % of wall.
  - **Checks.** 35 headless boots of 50-60 s (12 with the flag set, 12 default, 8 at 120 Hz, 3 with movies on): `dma=` advanced in
    every 5 s interval of every boot, no `TIMED OUT`; more than 20 full 235 s gameplay runs. SPU2 WAV (`KZ_AUDIO_WAV`),
    baseline vs offload: silent until t=20 s, then menu-music RMS/peak equal within 0.1-0.7 dB and waveform correlation
    0.997-1.000 per 10 s segment (a constant offset per run pair); the gameplay segments differ between offload and baseline
    exactly as two baseline runs do (scene timing); overall RMS -14.8 / -14.6 / -14.8 / -14.8 dBFS (2 baseline, 2 offload runs),
    2 silent gaps > 20 ms inside sound in all four, `kz_audio` underruns 0. Movies play. The four `ps2xIOP` test executables pass.
    Nowait calls on a client whose previous call had not completed: 0 of 27 684 in a run at 80 % machine load (queue latency
    avg 1.5 ms, max 112 ms).
  - **Debug.** `PS2X_IOP_RPC_STATS=1` (the table, plus the EE-side `SifCallRpc` totals, calls on a still-busy client and the queue
    latency of offloaded calls), `PS2X_IOP_RPC_ASYNC=0`.

- `0020-guest-io-copies.patch` (runtime library only: `ps2_memory.cpp`, `ps2_runtime.cpp`, `ps2_vif1_worker.cpp/.h`; no header that
  generated code includes, so no full rebuild; made against the tree with 0019 applied, and 0019's `ps2_memory.cpp` hunk carries
  this patch's drain comment as context, so apply the two together or use `git apply --3way`). Guest memory/IO slow paths and the
  DMA chain snapshot, measured in the gameplay window (`KZ_FPS=120`, concurrent A/B pairs; details, numbers and what did not help:
  docs/findings.md, "Guest memory/IO and DMA chain copies").
  - **Completed-DMAC drain without the mutex.** `PS2Runtime::Store32` drains completed DMAC handlers after every special-address
    store, and `PS2Memory::consumeCompletedDmacCauses` took `m_completedDmacMutex` twice per call: 5.2 % of the EE thread's busy
    samples. A global `g_ps2CompletedDmacPending` (declared in `ps2_vif1_worker.h`, defined in `ps2_memory.cpp`) lets it return
    without the lock while nothing is queued. `PS2X_DMAC_DRAIN_FAST=0` = old.
  - **Scratchpad fast path.** ~99.9 % of the guest loads/stores that leave the inline RDRAM window are scratchpad (7.9-9.0 M/s);
    aligned accesses in 0x70000000..0x70003FFF are served from the host scratchpad pointer in `PS2Runtime::Load*/Store*`.
    `PS2X_SPR_FAST=0` = old. All other IO together is < 0.06 % of those accesses (DMA channel registers ~4 k/s, GS privileged ~730/s,
    timers ~250/s), so no IO register got a special path.
  - Result: EE ms/frame (non-sleeping EE host time per displayed frame) with `PS2X_SPR_FAST=0 PS2X_DMAC_DRAIN_FAST=0` -> defaults,
    3 concurrent pairs: 15.46 / 13.38 / 14.53 -> 14.15 / 12.24 / 12.92 (median -1.31 ms, -9 %); 3 profiled pairs (that set still had the
    pooled-memcpy chain copy switched on, not kept) 11.37 / 13.28 / 12.57 -> 9.96 / 11.23 / 11.86. fps did not move (55.2 / 62.4 / 57.3 -> 55.6 / 62.0 / 57.3): frames land on vblank boundaries.
    Each half alone is below the noise of a pair; the profile shows the lock entries (`consumeCompletedDmacCauses` 5.2 % inclusive,
    SRW/Mtx 1.6-2.2 % each) gone and `PS2Memory::read32`/`translateAddress` off the top.
  - **Measurement switches** (env-gated, off by default): `KZ_MEMSTATS=1` (10 s histogram of the guest accesses that reach
    `PS2Runtime::Load*/Store*`, by class and the 40 hottest addresses), `KZ_CHAINSTATS=1` (bytes and segments per DMAtag id of the
    VIF1 chain walk), `PS2X_CHAIN_VERIFY=1` (compares every chain snapshot with the live source when the job's buffer is recycled:
    counts sources the guest overwrote between the CHCR store and job completion; 0 of 24 670 jobs in boot + menus + gameplay
    with IPU off, 170 of 10 013 with movies on, all in REF segments = FMV frame buffers, so a zero-copy chain would corrupt movies).
  - **Not kept (measured, no gain):** pre-sized pooled chain buffer + `memcpy` (walk 0.668 -> 0.660 and 1.117 -> 1.020 ms/job),
    non-temporal stores, two-phase gather with prefetch (worse: 0.79 vs 0.67 ms/job). The copy is memory bound; REF segments are
    ~60 % of the 0.55-0.8 ms per kick (skipping their copy: 0.533 -> 0.211 ms/job).
  - `experimental/ps2_cg-scratchpad-window.diff` (not applied by the `*.patch` glob): an inline scratchpad window in `ps2_cg.h`.
    Built in a private build dir (a copy of `generated/` whose own `ps2_cg.h` wins the quoted include): the slow-path entries fall
    from 6.7 % to 1.7 % of the EE thread's samples, but EE ms/frame did not move (12.79 / 11.17 / 10.89 -> 12.16 / 11.78 / 11.04),
    so it is not adopted.

- `0023-codegen-dispatch.patch` (recompiler + new header `ps2xRuntime/include/ps2_cg_dsp.h` + `EeScheduler.cpp`; needs a rebuilt
  recompiler, a regen and a full rebuild; `tools/scripts/regen.py` sets `PS2X_CODEGEN_DSP=1` and `PS2X_CODEGEN_FOLD=1` by default,
  both `=0` give the patch-0017 output, checked byte for byte: 82 652 files, 0 differences; the main repo carries the two regen.py
  defaults and `tools/scripts/verify_resume_table.py`). Cuts the ~0.9 M guest-to-guest scheduler dispatches per second of gameplay
  to ~8 k/s. `ps2_cg.h` is untouched (only generated code and the scheduler include the new header).
  - **Where the dispatches came from** (counted in `EeScheduler::run` with `PS2X_SCHED_STATS=2`, gameplay t=150..200 s, per
    second / per displayed frame, classified by how the previous guest entry ended). Same runtime, the generated code of
    `build\RelWithDebInfo`: **923 k/s (16 041 per frame)** = 364 scheduler-originated (thread start, interrupt invocation) + 166 checkpoint
    yields + 365 k *return-resumes* + 557 k *jumps*. Every return-resume and jump traced back to one mechanism: a function leaves
    through a branch or fall-through into another function or `entry_*` fragment with `ctx->pc = target; return;`, the call site sees
    `pc != return address`, treats it as a non-local return, sets `g_ps2GuestUnwinding`, every generated frame returns to the
    scheduler, the scheduler dispatches the fragment and then, one dispatch per level, every caller's resume label. Yields are 166/s
    (each unwinds ~40 frames, the ~8 k/s that is left). The rest: self-recursive functions (`jal` into the own function is a `goto`,
    its `jr $ra` returned to the scheduler: 0x17f8e0 and 0x185e20, 25-30 k/s each) and the flag-wait loop `FUN_0014fd90` (up to 706 k/s
    when the game sits in it: after a yield the function table resumed it in a nested `entry_*` copy of its tail, whose five blocks then
    ran block to block through the scheduler).
  - **Tail transfers** (`ps2_cg_dsp.h`). A generated function that leaves through a tail jump (conditional branch out of its range, `j`
    to a non-function address, unresolved `jr $reg`, falling off its end) stores `ctx->pc = target | 1` (`ps2DspTail`) and returns.
    The nearest enclosing call site (`ps2DspCall`, replaces `ps2CgCall`) recognises the mark and calls the target itself in a loop
    (`ps2DspAfterCallSlow`): the callee's frame is gone (tail jump), the caller's frames stay, so the eventual `jr $ra` back to the
    caller is an ordinary return. Only marked pcs are followed; a pc that is neither the return address nor marked is a non-local
    return (longjmp, thread switch) and takes the old unwinding path. Each followed tail is charged 8 cycles against the checkpoint
    budget like a dispatch was; when the budget runs out the checkpoint runs and a yield leaves `ctx->pc = target` (unmarked, registers
    flushed) exactly as the scheduler expects. The scheduler is the outermost call site and follows marked tails the same way
    (`ps2DspSchedulerFollow`, and clears the mark on whatever pc a function returned to it with). Guest words are 4-aligned, so
    the mark cannot be mistaken for an address; the hot path (`pc == fall`) is unchanged, no global flag is read on it.
    `PS2X_STRICT_RETURN_DIAGNOSTICS`, hooks installed with `replaceFunction` (they see `ctx->pc == entry`, unmarked) and
    `kz_ipu`/`kz_aim` wrappers (`ctx->pc != ra` after the original returns means "yielded": a marked pc counts) work unchanged.
  - **Recursion.** A `jr $ra` in a function that contains a self-recursive `jal` switches on the return address and `goto`s the
    recursive return labels directly (charged 8 cycles; yields as before), 124 k/s in gameplay.
  - **Fragment folding** (`PS2Recompiler::foldEntryFragments`, `PS2X_CODEGEN_FOLD=1`). Ghidra's export has 64 020 `entry_*`
    blocks: 56 882 lie inside a real function (a copy of its tail from an inner label on) and 7 138 are standalone, in 2 181
    contiguous chains of mean 24 instructions (largest 2 526). Each block was its own C++ function and its own table entry,
    so a resume at an inner label ran the copy and then hopped block to block through the scheduler (registers written back and
    reloaded at every hop), and the loops inside such functions never stayed inside them. Now a block nested in a real function
    (its start is a decoded instruction of it, its end within it) is dropped and becomes a resume label of that function; contiguous
    standalone blocks are merged into one function under the first block's name (up to 4 096 instructions; the others become
    resume labels). Configured entry points that had been registered under a block move to the owner. 56 212 nested + 5 695
    chained blocks folded: 82 652 -> 20 745 generated files, generated sources 556 -> 247 MB, `killzone.exe` 102.2 -> 74.1 MB, unity
    compile CPU time (ninja log, loaded machine) 6 864 -> 3 543 s. Every table address is still present (144 623 in both trees, none
    lost, none new) and `tools/scripts/verify_resume_table.py <dir>` checks that each of the 123 881 entries that now point into a
    function whose start is a different address (was 61 974) has a `case` in that function's resume switch and that every `goto`
    has its label. The first attempt lost 58 configured entry points (code pointers inside removed blocks): the check found them.
  - **Resume switch guard.** A call enters at the function start, so generated code now tests `ctx->pc != START` before the `switch`
    (a function with many resume labels no longer walks the switch on every call).
  - **Debug.** `PS2X_SCHED_STATS=1|2` also prints, per 10 s, the dispatches by reason (scheduler / yield / return-resume / jump), the tail
    jumps followed by call sites and the scheduler, the recursive returns kept local and missing tail targets (0 in every run).
    `=2` adds per-reason (previous entry -> pc) pair histograms. `=3` keeps only the pacing-sleep total and the 10 s report (nothing
    per loop iteration or dispatch): the mode to use when comparing builds with different dispatch counts.
  - **Measured** (headless, `KZ_FPS=120`, scripted input, t=150..200; machine shared with other builds and game instances, so
    concurrent pairs). Two build dirs from the same runtime sources, `dsp0` = the generated code of `generated/` (patch 0017 output)
    and `dsp2` = this patch's output, `PS2X_SCHED_STATS=3` in both (new: only the vblank pacing sleep is accumulated, nothing per
    dispatch). 4 pairs, fps base / new: 73.6 / 73.8, 75.1 / 73.8, 71.6 / 71.8, 69.8 / 70.0 (median 72.6 / 72.8: unchanged, the game is
    on a vblank step, see docs/findings.md "EE thread frame budget"); EE host time per displayed frame (1000 ms - pacing sleep) / fps:
    10.38 / 9.85 / 10.83 / 11.45 ms before, 10.06 / 9.56 / 10.24 / 11.14 ms after: -0.32 / -0.29 / -0.59 / -0.31 ms (median -0.31 ms,
    mean -0.38 ms, -3 %). Profile of the EE thread without stats (KZ_PROFILE=160,40, one pair): `EeScheduler::run` self time 3.6 % -> 0.1 %.
    Not used: a first set of pairs against `build\RelWithDebInfo` with `PS2X_SCHED_STATS=1` (-0.67 ms median). That binary predates
    patches 0020/0021 (both in the runtime of `dsp0`/`dsp2`), and `=1` reads the host clock and does rdtsc/counter work on every
    dispatch (`RtlQueryPerformanceCounter` 2.9 % + `schedStatsReport` 1.5 % of the EE thread in the profile), which taxes exactly the
    build with 1 M dispatches per second.
    Second set, three builds at once (`dsp0` before, `dsp2` final, `dsp3` = `PS2X_CODEGEN_FOLD=0`: trampolines and local returns
    but no folding), 4 rounds, all rebuilt from the same runtime sources: EE ms/frame before 11.01 / 11.96 / 12.04 / 11.81 (median
    11.89), final 10.68 / 11.28 / 11.28 / 11.23 (median 11.26: -0.63 ms, -5 %), no folding 10.61 / 11.93 / 11.29 / 10.92 (median 11.11);
    fps before 69.3 / 66.5 / 65.7 / 68.8, final 70.3 / 66.9 / 67.9 / 70.7, no folding 71.2 / 64.9 / 66.7 / 72.1. Folding is not
    measurably faster than the trampolines alone (differences within the +-0.3 ms noise of the rounds); it removes the ~0.73 M followed
    tail jumps per second (1.2 k/s left) and is kept on for the code size (-27 % exe), compile time (-48 % CPU) and because a resume
    can no longer land in a fragment that loops through the scheduler. Over all 8 comparisons with the final build: -0.32 / -0.29 / -0.59 /
    -0.31 / -0.33 / -0.68 / -0.76 / -0.58 ms, median -0.45 ms (-4 %).
  - **Correctness evidence.** See docs/findings.md "Guest control flow without the scheduler".
  - **Left.** ~190 yields/s x ~40 unwound frames = the remaining ~8 k dispatches/s (needs stack switching or fibers to remove); the
    memory slow paths (`ps2CgWr32Slow`/`ps2CgRd32Slow`/`Load32`/`Store32`, ~6 % of the EE thread in the profile: scratchpad and
    MMIO addresses) are not control flow and were not touched.

- `0022-vif-worker-hotpath.patch` (runtime: `gs/ps2_gif_arbiter.cpp`, `ps2_vif1_interpreter.cpp`; the rest is in the main repo:
  `ext/kzvu` (`shim/KzvuShims.cpp`, `shim/unity/KzvuMicroVU.cpp`, `src/kzvu.cpp`, `include/kzvu.h`, test), `ext/kzgs/src/kzgs.cpp`,
  `src/kz_gs.cpp`, `src/kz_vu.cpp`, `tools/scripts/vif_*`/`mkrun.ps1`). Makes the VIF1/VU1/GIF worker's per-frame time smaller; no header
  changed, no regen. Details, profile and every measurement: docs/findings.md "VIF1 worker hot path".
  - **XGKICK -> GS hand-off** (was ~18 % of the worker; ~4000 packets of 1.2 KB per frame). A packet used to be copied three times (VU1 memory ->
    kzvu buffer -> arbiter heap vector -> kzgs ring) with an alloc/free, a sort and an atomic notify each. (1) kzvu walks the tags in VU1 memory
    and hands the callback a pointer into it when no partial packet is buffered (`KZVU_XGKICK_INPLACE=0` = old; `KZVU_XGKICK_CHECK=1` compares
    with a `Gif_Tag` walk). (2) `GifArbiter::submit` emits a PATH1/PATH2 packet directly (host GS hook, and the built-in GS when it must see it) when
    nothing is queued (`PS2X_GIF_DIRECT=0` = old): that is where the sorted drain would have put it, and with a PATH3 packet queued (job start)
    it queues as before, so the order the GS sees is unchanged. `drain()` returns at once on an empty queue. (3) `packetWritesGsEvent` skips
    a PACKED tag's body when none of its NREG descriptor nibbles is A+D (`PS2X_GIF_SCAN_CHECK=1` compares with the plain loop). (4) kzgs
    wakes its consumer / a producer waiting for ring space only when the other side announced it sleeps (`KZGS_NOTIFY_ALWAYS=1` = old). (5)
    `onGifPacket`'s three locked adds became load+store (one feeding thread at a time), `onXgkick` reads the stats flag once.
  - **VU1 code-state memo** (`ext/kzvu`, `KZVU_STATE_MEMO=0` = old). microVU forgot its current-program table on every micro-memory change (~4000 a
    second in Killzone), so every program entry (MSCAL, JR/JALR target) searched its program list again: 15 M `memcmp` calls per second, cache-miss
    bound. That, not compilation, was most of the `mVUcompileJIT<1>` time seen in profiles. The memo remembers the table per micro-memory
    content and puts it back when the content returns; see `ext/kzvu/README.md` ("JIT invalidation") for the exact rules
    (`KZVU_STATE_MEMO_VERIFY=1` checks every saved/restored entry).
  - **Masked UNPACK** (15 % of Killzone's UNPACKs; `PS2X_VIF_UNPACK_MASKED_FAST=0` = old): STMOD 0, not fill mode: one SSE
    load/convert/select/store per vector with per-write-cycle constants (data / row / column / keep). `PS2X_VIF_UNPACK_CHECK` covers it.
  - **Measured** (concurrent pairs of one binary, all switches above off vs on, `PS2X_VIF1_STATS=1`, t=150..200 s, worker ms per frame):
    7.82 / 12.86 / 10.07 -> 6.20 / 9.82 / 7.74 (-21 %, -24 %, -23 %), fps 68.4 / 58.1 / 65.9 -> 70.8 / 59.0 / 67.7. Per change: hand-off
    14.01 / 11.80 / 14.12 -> 12.78 / 10.38 / 12.23 ms; memo 12.19 / 12.85 -> 9.74 / 11.98 ms (first version); masked UNPACK 8.15 / 9.26 ->
    7.98 / 9.03 ms. Without the stats the fps change is not resolvable (2 pairs, 67.4 / 72.9 -> 72.5 / 70.4): the EE thread (~8.7 ms per frame) and
    the GS thread (6-8 ms) limit now, the worker needs 6.3 ms on a quiet machine (74.8 fps run).
  - **Evidence.** 23 M XGKICK transfers, 9.6 M UNPACKs, all memo entries (~500 k code changes) and every packet's A+D scan compared with the
    old logic in one 235 s run: 0 differences; kzvu_test 1776 checks (new memo test), kzvu0_test 509, 0 failures. Frames and movies
    (`KZ_IPU` unset) as before.
  - **Tried and dropped.** An inline SSE compare in place of the CRT `memcmp` in microVU's program search (no change), JR/JALR as part of the
    same program (`doJumpAsSameProgram`, no change).
  - **Left.** The EE thread and the GS thread (the PCSX2 renderer) for 120 fps; in the worker `mVUcompileJIT<1>` (the JR/JALR call itself, ~9 % of its
    busy time; `doConstProp` might remove some JRs but is untested and off in PCSX2) and the kzgs ring copy of ~5 MB per frame.

- (no `0024`) clang-cl build: needed no `ext/PS2Recomp` change. The one compile fix is in `ext/kzvu` (main repo) and the flags are in the top-level `CMakeLists.txt`;
  see docs/findings.md "clang-cl build".

- `0025-frame-pipeline.patch` (runtime: `include/runtime/ps2_timeline.h` (new), `Kernel/EeScheduler.cpp`, `include/runtime/ee_scheduler.h`, `ps2_vif1_worker.cpp`,
  `ps2_runtime.cpp`; the rest is in the main repo: `ext/kzgs` (`kzgsSetTraceHook`), `src/kz_gs.cpp`, `src/kz_overrides.cpp`, `tools/scripts/tl_*`). No header that
  generated code includes changed, so no regen. Gameplay at `KZ_FPS=120`: **81.8 -> 118.1 fps** (medians of 3 interleaved sequential runs each; a first set with an earlier build of the same change: 79.8 -> 119.2). Timeline, per-frame
  breakdown, the measurements and the evidence: docs/findings.md "Frame pipeline timeline". The patch is the diff of those files against their state before this
  work (it applies to that tree). Applying 0001-0023 one after another to a fresh checkout of the pinned commit fails at 0004 and at
  0010, 0012, 0013, 0016 and 0018-0023 (not investigated: a failed patch leaves its files unpatched for the later ones), so the series has not been replay-tested.
  - **Timeline recorder** (`KZ_TIMELINE=<file.csv>`, window `KZ_TL_START`/`KZ_TL_END` seconds of run time, default 160..200; nothing is recorded or allocated without
    it, and the guest-function hooks are only installed with it). One 24-byte record per event (one relaxed `fetch_add` and a QPC read, ~30 ns), written as
    CSV when the first event after the window arrives. Events: guest vblank (with the EE cycle clock), EE sleeps (pacing sleep in `processDueDeadlines`, idle wait),
    VIF barriers that actually wait, completion apply (by scheduler event / barrier / register poll), DMAC interrupts queued, guest invocations (interrupt /
    callback start and end with their pc), D1 kick (bytes, cycle clock), first busy / first idle read of D1/D2 CHCR, worker job begin/end (DMA / MSCAL /
    GS half of the vblank), completion posted, kz `runVsync`, kzgs producer waits (frame throttle, ring full), GS thread frame begin/end and idle time, frames
    queued; and entry/exit of the guest functions FUN_001bff10 (frame limiter), FUN_00150090 (the kick), FUN_00151fc8 (kick check in the vblank handler: flag,
    next buffer state), FUN_00152018 (the vblank handler), FUN_001759c0 / FUN_001759f8 (only seen when called through the function table; the game calls them
    directly, so they do not show). `tools/scripts/tl_analyze.py` turns a CSV into the summary and per-frame tables; `tl_run.ps1`, `tl_ab.ps1` (interleaved
    sequential A/B runs, rotated order, CPU load logged) and `tl_boots.ps1` (N boots, exit code and `dma=` advance) drive the runs.
  - **EE cycle clock scale** (`PS2X_EE_CYCLE_SCALE`, default `auto`; `=1` restores the old behaviour). The vblank event needs its cycle deadline *and* its
    host deadline, and the cycles the guest code's checkpoints charge run 2-2.4x faster than wall time on this host (one vblank's 2.46 M cycles were used up in 3.5-4.2 ms of EE time), so a frame whose estimated work is more
    than one vblank period's cycles (2.46 M at 120 Hz; a measured heavy frame cost 3.4 M) spans two vblanks although the host executed it in less than one; the EE then
    sleeps in `processDueDeadlines` until the second vblank. `checkpointDue` now multiplies the charge by a Q16 factor. `auto` = 60 Hz period / vblank period,
    at most 1 (0.5 at 120 Hz, 0.417 at 144, 0.25 at 240; 1 = unchanged at 60 Hz or below): the game gets the same estimated cycles per vblank as at its native 60 Hz,
    so timers, IOP time and guest spin-loop iterations per vblank are what they are at 60 Hz (the same thing `kz_timing` does for the frame timer and the fade).
    `<f>` sets a fixed factor, `0` makes the clock follow wall time only (`accountCycles` already floors it there).
  - **Measured.** `KZ_FPS=120` scripted scene, fps = (vif at t=200 - vif at t=150) / 50, 3 rounds alternating `PS2X_EE_CYCLE_SCALE=1` / default, one instance at a
    time: 82.1 / 81.8 / 81.2 -> 117.4 / 118.1 / 118.8 (medians 81.8 -> 118.1; first set 79.0 / 80.8 / 79.8 -> 119.8 / 119.2 / 117.8). Fixed factors, one run each: 0.75 -> 105.6, 0.6 -> 116.6.
  - **Evidence.** 12 boots of 50 s (3 at a time) plus 2 each at `KZ_FPS=60` (no scale, nothing changed), 144 (0.417) and 240 (0.25) and 2 boots of 100 s with
    `PS2X_STRESS_YIELD=97 KZ_CRASH_TRACE=1`: every boot exited 0 and `dma=` advanced in every heartbeat; `KZ_VU0_VERIFY=8 PS2X_LAZY_TIMERS=verify` 235 s gameplay: 6.7 M
    VU0 calls and 101 k timer comparisons, 0 mismatches; frames (menus, intro movie and menu video backgrounds with `KZ_IPU` unset, loading screen, level, weapon,
    HUD, explosions, death screen) as in the old clock; SPU2 WAV over 130 s: 130.01 s (old clock) vs 130.00 s (new), equal per-10 s RMS and peaks, same music (correlation 0.95 at
    the usual constant offset); no `TIMED OUT` or new warning lines.
- **0026-pad-pressure-order.patch** - DualShock 2 pressure bytes 16..19 are L1, R1, L2, R2 (the runtime wrote L1, L2, R1, R2). Killzone reads every
  button through the pressure bytes (FUN_001b33b8: byte pad+0x34+id), so R1 and L2 were swapped for keyboard and gamepad: fire (id 9) came from L2
  and crouch (id 10) from R1. Measured with `KZ_PAD_LOG=1` (scripted R1, L2, R2, L1, triangle -> ids 10, 9, 11, 8, 4 before the fix).
