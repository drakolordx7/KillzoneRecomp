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
