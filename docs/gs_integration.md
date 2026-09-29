# Phase 3 plan: PCSX2 GS behind PS2Recomp's pipeline

Source: code map of ps2xRuntime (A) and pcsx2 (B), 2026-09-28.

## Keep from PS2Recomp (A)
- Synchronous DMAC (`ps2_memory.cpp` writeIORegister 1293-1570, processPendingTransfers ~1680-1906), VIF1 (`ps2_vif1_interpreter.cpp`),
  VU1 interpreter (`vu/ps2_vu1_*.cpp`, XGKICK → PATH1), GifArbiter (`gs/ps2_gif_arbiter.cpp`), EeScheduler vblank (`Kernel/EeScheduler.cpp`).

## Replace
- `GS::processGIFPacket` + `GSCpuBackend` (CPU software rasterizer) → GS-thread ring buffer calling PCSX2 `GSgifTransfer1/2/3` (B `GS/GS.cpp:420-438`).
- `GSopen(..., basemem)` with basemem = A's `GSRegisters` laid out as PCSX2 `GSPrivRegSet` (0x2000 bytes).
- VBlankStart → `GSvsync(field, regs_written)`.
- raylib present loop → Win32/SDL3 window serving PCSX2 `Host::AcquireRenderWindow` (WindowInfo/HWND). D3D11 first, Vulkan second.
- Stub ~6-8 PCSX2 modules GS touches: Host:: OSD/error/settings, VMManager (serial/CRC), PerformanceMetrics, ImGuiManager/FullscreenUI,
  EmuFolders, GSDumpReplayer, SaveState, MTGS (hotkeys). Build `common/` + fmt, imgui, xxhash, zstd/lzma, shaderc; ship `bin/resources/shaders`.

## Fix in A (Killzone-relevant)
- VU1 always clamps (`normalizeOperand`, ps2_vu1_core.cpp:109) — Killzone GameIndex wants `vuClampMode: 0` ("yellow graphics"). Add a no-clamp mode.
- VIF V4-5 unpack (ps2_vif1_interpreter.cpp:703-706) misses `<<3`/`<<7` scaling — likely bug.
- VIF0 unpack only V?-32, no MSCAL; stops at unknown opcodes (:238, :281).
- GS SIGNAL/FINISH set CSR but never raise INTC_GS / check IMR; VSINT never set (gs_frontend.cpp:1464-1486).
- GIF FIFO 0x10006000 unhandled.

## GS settings to reproduce from GameIndex
- `halfPixelOffset: 4` (Native) and `nativeScaling: 2` (Aggressive) → `UserHacks_HalfPixelOffset`, `UserHacks_NativeScaling` in GSConfig.
- IbitHack: microVU-only, irrelevant to the interpreter.
