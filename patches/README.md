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
