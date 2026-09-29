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
- `0005-vif1-direct-image-continuation.patch`: fixes garbled text and dithered sprite edges, in both the kzgs GS and
  the CPU GS. A VIF1 DIRECT can end with a PATH2 IMAGE GIFtag whose pixel data arrives in a later DIRECT, usually
  `MARK; DIRECT n` in the next DMAtag's TTE words. The runtime used to treat the bytes right after the first DIRECT as
  that pixel data. Those bytes were the VIFcodes, so every such upload (fonts, CLUTs, UI atlases) was shifted by 8
  bytes: 2 CT32 or 16 T4 pixels. The last 8 bytes of the data were then parsed as VIFcodes. Now VIFcodes are always
  parsed. Only the payload of a DIRECT is re-wrapped in a synthesized IMAGE tag when an image is pending. Raw
  continuation is kept only for a DIRECT cut off at the end of a DMA buffer (`m_vif1PendingDirectQwc`). Verified byte
  for byte against real PCSX2's EE RAM through a PINE savestate.
