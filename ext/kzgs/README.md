# kzgs: PCSX2's hardware GS renderer as a static library

kzgs compiles PCSX2's `pcsx2/GS` renderer into one static library, `kzgs.lib`. It replaces the recompiled
game's CPU software rasterizer. It uses the D3D11, D3D12 and Vulkan hardware renderers. The rest of the emulator is
replaced by small stubs in `shim/`, so the library needs no BIOS, EE/IOP, VIF, DMAC or MTGS.

- PCSX2 source: `ext/pcsx2`, a depth-1 clone of PCSX2 master. This build was verified against commit `646df006a4`.
  kzgs does not modify anything in `ext/pcsx2`.
- Dependencies: the official `pcsx2-windows-dependencies.7z` from
  <https://github.com/PCSX2/pcsx2-windows-dependencies/releases/tag/latest-windows-dependencies>, extracted to
  `ext/kzgs/deps` (so the headers are in `ext/kzgs/deps/deps/include`). It is not committed.

## Build

The only supported toolchain is MSVC x64 (VS 2026 / 14.51), using CMake 3.24 or newer with Ninja.

```sh
# one-time: fetch the dependency bundle (~170 MB)
curl -L -o ext/kzgs/deps/pcsx2-windows-dependencies.7z \
  https://github.com/PCSX2/pcsx2-windows-dependencies/releases/download/latest-windows-dependencies/pcsx2-windows-dependencies.7z
"C:/Program Files/7-Zip/7z.exe" x -oext/kzgs/deps ext/kzgs/deps/pcsx2-windows-dependencies.7z

# standalone build + test (keep -j at 4 on this machine)
cmd //c "tools\scripts\vsenv.bat cmake -S ext\kzgs -B ext\kzgs\build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo"
cmd //c "tools\scripts\vsenv.bat cmake --build ext\kzgs\build -j 4"
ext/kzgs/build/kzgs_test.exe            # exit code 0 = pass; add --renderer d3d12|vulkan, --warp, --vsync, --show
```

A clean build is 165 Ninja steps (about 160 translation units). The result is `kzgs.lib` (about 87 MB with debug info).

Using it from the game's CMake:

```cmake
add_subdirectory(ext/kzgs)                 # KZGS_BUILD_TEST defaults to OFF when not top-level
target_link_libraries(killzone PRIVATE kzgs)
kzgs_stage_resources(killzone)             # copies resources/shaders + runtime DLLs next to killzone.exe
```

CMake options:

| Option | Default | Purpose |
|---|---|---|
| `KZGS_PCSX2_DIR` | `ext/pcsx2` | Location of the PCSX2 source tree |
| `KZGS_DEPS_DIR` | `ext/kzgs/deps/deps` | Location of the extracted dependency bundle |
| `KZGS_ISA` | `AVX2` | Instruction set the GS code is compiled for: `SSE4`, `AVX` or `AVX2` (see Known gaps) |
| `KZGS_BUILD_TEST` | on when top-level | Builds `kzgs_test` |
| `KZGS_BUILD_TOOLS` | on when top-level | Builds `kzgs_replay`, the trace debugger (see Debugging) |

All of kzgs' compile flags are private: `/permissive-`, `/Zc:preprocessor`, `/arch:AVX2`, C++20, PCSX2's defines and
the ImGui rename header. The only thing a consumer inherits is the include directory, plus these link libraries:
system D3D/DXGI, `delayimp`, and import libraries for `zstd`, `libpng16`, `libwebp`, `jpeg62` and `dxcompiler`. All
five of those are `/DELAYLOAD`ed.

`kzgs_stage_resources()` copies these DLLs next to the executable:
- `zstd.dll`
- `libpng16.dll` and `z.dll`
- `libwebp.dll` and `libsharpyuv.dll`
- `jpeg62.dll`
- `dxcompiler.dll` (needed by D3D12)
- `shaderc_shared.dll` (Vulkan loads it at runtime)

A D3D11-only run touches none of them, except when screenshots, dumps or texture replacements are used.

## API (`include/kzgs.h`)

```cpp
bool kzgsOpen(void* hwnd, const KzgsConfig& cfg, uint8_t* privRegs, std::string* err = nullptr);
void kzgsClose();
bool kzgsIsOpen();
void kzgsGifTransfer(int path /*1..3*/, const uint8_t* data, uint32_t qwords);
void kzgsVsync(int field, bool regsWritten = true);   // presents a frame
void kzgsWriteCSR(uint32_t csr);                      // RESET bit
void kzgsSoftReset(uint32_t mask = 7);                // GIF path soft reset
void kzgsReset();                                     // full GS reset
void kzgsResize(int width, int height);
void kzgsUpdateConfig(const KzgsConfig& cfg);         // any setting, including a renderer switch
void kzgsSync();                                      // wait until the GS thread has drained the queue
bool kzgsReadback(std::vector<uint8_t>& rgba, int& w, int& h);  // last output frame, RGBA8, internal resolution
void kzgsSetLogCallback(KzgsLogFn fn);
```

`KzgsConfig` has these settings:
- `renderer`: D3D11, D3D12, Vulkan, or Software (PCSX2's software rasterizer, useful as a reference when debugging)
- `upscale`: 1 to 8
- `textureFiltering`: PCSX2 `BiFiltering`
- `anisotropy`
- `fxaa`
- `edgeAA`: PCSX2 HW AA1
- `aspect`: 4:3, 16:9 or stretch
- `vsync`: FIFO or off
- `bilinearPresent`
- `halfPixelOffset` and `nativeScaling`: default to the Killzone GameIndex fixes, `4` (Native) and `2`
  (Aggressive). They are written to `UserHacks_HalfPixelOffset` and `UserHacks_NativeScaling`.
- `adapter`, or `useWarp` to force the WARP software adapter
- `resourcesDir`: defaults to `<exe>/resources`
- `cacheDir`: defaults to `<exe>/cache`. Set it to a writable per-user folder if the game is installed read-only.
- `disableShaderCache`
- `maxQueuedFrames`: default 2
- `selfContainedGifPackets`: default true; see GIF packet semantics below
- `debugDevice`

Pass `hwnd = nullptr` to open without a window (a surfaceless device). This still renders and supports
`kzgsReadback`, so it can be used for headless screenshot tests.

### Privileged registers

`privRegs` is the caller's own 0x2000-byte block, laid out like PCSX2's `GSPrivRegSet`:

| Offset | Registers |
|---|---|
| `+0x0000`, `+0x0010`, `+0x0020`, ... | PMODE, SMODE1, SMODE2, ... (one every 0x10 bytes) |
| `+0x0070` | DISPFB1 |
| `+0x0080` | DISPLAY1 |
| `+0x00E0` | BGCOLOR |
| `+0x1000` | CSR |
| `+0x1010` | IMR |
| `+0x1080` | SIGLBLID |

kzgs never writes to this block. The GS thread works on its own copy, which PCSX2's MTGS also does. At every
`kzgsVsync()` it snapshots 0x000-0x0EF, CSR, IMR and SIGLBLID into the command ring. The game can therefore write its
registers at any time without racing the renderer.

Mode notes that were checked with the test:
- PCSX2 identifies NTSC from `SMODE1.CMOD == 2`.
- A 448-line full-frame buffer uses `SMODE2 = INT=1, FFMD=0`.
- `FFMD=1` makes PCSX2 read a half-height field (224 lines) and line-double it.

### GIF packet semantics

By default (`selfContainedGifPackets = true`), every `kzgsGifTransfer()` call must be a self-contained sequence of GIF
packets that starts with a GIFtag. This matches what PS2Recomp's GIF arbiter forwards, and what its own CPU GS
frontend expects.

A GIFtag that is still open at the end of a call is dropped. This matters for one case in particular. When a VIF1
DIRECT ends with an IMAGE tag whose data arrives in a later DMA chunk, `ps2_vif1_interpreter.cpp` sends that data as
a new packet with its own synthesized IMAGE tag. PCSX2 keeps GIF path state across calls (MTGS semantics), so without
this mode it would read the synthesized tag as pixel data. The last data qword would then be parsed as a garbage
GIFtag, which corrupted every PATH2 texture upload and the GS state after it. That was the cause of the all-black
Killzone frames. Dropping the open tag is safe because the image transfer's own progress (TRXREG) is tracked
separately and continues with the re-wrapped data.

Set `selfContainedGifPackets = false` for a raw, hardware-exact per-path byte stream, where a packet may continue
across calls.

## Threading model

kzgs owns one GS thread. The D3D11/D3D12/Vulkan device, the ImGui context and every PCSX2 GS call live on that thread.
The public calls work as producers:

- They copy their arguments into a 32 MB single-consumer ring buffer and return immediately. GIF data is copied, so
  the caller can reuse its DMA buffer.
- Producers are serialized by a mutex. Because packets from different threads would interleave, drive kzgs from the
  one thread that owns the GIF arbiter.
- `kzgsOpen`, `kzgsClose`, `kzgsSync` and `kzgsReadback` block until the GS thread has handled them.
- `kzgsVsync` blocks only when more than `maxQueuedFrames` frames are still queued. This is frame pacing, as in
  PCSX2's MTGS.
- A transfer larger than 1 MB is split into several ring commands. That is safe because PCSX2's GIF path state
  machine resumes partial packets. The test sends every packet in 7-qword pieces to check this.

## What is stubbed (shim/)

| Area | kzgs behavior | Matters for correctness? |
|---|---|---|
| `Host::AcquireRenderWindow` / `ReleaseRenderWindow` / `BeginPresentFrame` | Returns the HWND given to `kzgsOpen`, or a surfaceless window if it was nullptr. | Yes, and it is implemented. |
| `Host::` OSD, error and translation calls | OSD text goes to the log. Errors go to the log and are returned by `kzgsOpen`'s `err`. Translation returns the input unchanged. | No |
| `Host::` settings getters | Always return the default value. kzgs is configured only through `KzgsConfig`. | No |
| `Host::IsFullscreen` / `SetFullscreen` | Always windowed; exclusive fullscreen is not supported. | No |
| `ImGuiManager`, `FullscreenUI` | A minimal ImGui context (default font, no OSD or menus), because the devices call `ImGui::Render()` on every present. | No |
| `VMManager` | Reports serial `SCUS-97402`, title `Killzone` and CRC 0. These are used only for dump and texture-replacement folder names. | No |
| `PerformanceMetrics`, `GSDumpReplayer`, `MTGS`, `SPU2`, `USB`, `Pad`, `FileMcd`, `AudioStream`, `CrashHandler` | No-ops. The MTGS entries are reached only from PCSX2 hotkeys, which kzgs never fires. | No |
| `eeHw`, `_cpuRegistersPack`, `FMVstarted`, `BuildVersion` | Dummy globals that PCSX2 headers bind references to. | No |
| `SysMemory::GetCodePtr` | Reserves only the 64 MB RWX "SWrec" region, near the exe, for the software renderer's JIT. | Only for the Software renderer |
| **SIGNAL / FINISH / LABEL, CSR interrupt bits, IMR, INTC_GS** | Not handled. PCSX2's `GSState` ignores these A+D writes too; the EE-side GIF unit handles them. **The caller's GIF arbiter must process them and raise INTC_GS.** | **Yes** |
| **CSR.FIELD / vsync field** | The caller supplies `field` to `kzgsVsync` and keeps CSR in its own block. | **Yes** |

Two parts of PCSX2 are compiled for real rather than stubbed:
- `Pcsx2Config.cpp`, which provides `GSOptions`, `EmuConfig` and `EmuFolders`.
- `SourceLog.cpp`.

### Dear ImGui isolation

The game links its own Dear ImGui (1.92.7-docking), while PCSX2 ships 1.92.9b. To keep them apart, CMake generates
`build/kzgs_generated/kzgs_imgui_rename.h`, and every kzgs C++ file force-includes it:
- The header `#define`s every `ImGui`, `Im*` and `GImGui` identifier found in PCSX2's imgui headers and sources to
  `Kz<name>`, about 1,760 identifiers.
- Identifiers that imgui uses as `#ifndef` hooks are left alone. These are the `ImDrawIdx`/`ImTextureID` typedefs and
  the inline `ImQsort`.

This was verified by linking `kzgs.lib` together with the game's fetched imgui 1.92.7 into one executable. It linked
with no duplicate symbols, and the game's `ImGui::GetCurrentContext()` was unchanged after `kzgsOpen`, a vsync and
`kzgsClose`.

## License

PCSX2 is GPL-3.0-or-later, and kzgs statically links it. `kzgs.lib`, and any executable that links it (the Killzone
PC port), is therefore a derivative work under **GPL-3.0-or-later**. Distributing binaries requires offering the
corresponding source. The shims and the kzgs sources carry `SPDX-License-Identifier: GPL-3.0+`.

The DLLs from the dependency bundle keep their own licenses (zlib, libpng, libwebp, libjpeg-turbo, zstd, DXC,
shaderc); see `deps/deps/licenses`.

## Debugging (kzgs_replay)

`kzgs_replay` replays a game GS trace recorded with `KZ_GS_TRACE`, in the same format that `tools/gs_replay` reads,
and can look inside PCSX2's GS:

```sh
ext/kzgs/build/kzgs_replay.exe work/trace_full2.bin work/kzdbg/out --every 100 [--upscale 2] [--renderer sw]
    [--pcrtc] [--vram 0:8,70:8,e0:8] [--dump-draws 1000:60] [--last N]
```

- `--pcrtc` prints PCSX2's display-circuit state: display and framebuffer rects, offsets and magnification.
- `--vram` reads the texture cache back into local memory. It then prints nonzero bytes per 256 KB of VRAM and
  writes each requested CT32 buffer (FBP:FBW) as a PNG.
- `--dump-draws F:N` turns on PCSX2's own per-draw dumps (context registers, vertices, transfers) for N draws
  starting at frame F.
- It uses the internal hook `kzgs::RunOnGSThread()`, declared in `shim/include/kzgs_internal.h`.

## Known gaps

- **Local-to-host transfers are not exposed.** These are `TRXDIR=1` reads of VRAM back through the GIF FIFO or BUSDIR
  (`GSInitAndReadFIFO`), and there is no kzgs call for them yet. The GS processes the TRXDIR write, but the data is
  never handed back to the caller.
- **Savestates are not exposed.** `GSfreeze` is compiled but there is no kzgs call for it.
- The GS code is compiled for one ISA (`KZGS_ISA`, default AVX2), not PCSX2's runtime multi-ISA dispatch. CPUs
  without AVX2 need a rebuild with `-DKZGS_ISA=SSE4`.
- D3D12 uses the system D3D12 runtime, not the Agility SDK. PCSX2 exports `D3D12SDKVersion` from its exe; kzgs does
  not. The log shows "Agility version: 616", which is the system runtime.
- No OSD, performance overlay, video capture UI or PCSX2 hotkeys. Video capture and GS dump code is compiled but has
  not been tested through kzgs.
- `kzgsUpdateConfig` changes vsync between FIFO and off only. Mailbox mode is not exposed.
- `QueryDisplayConfig() failed: Access is denied` is logged when the window's refresh rate can't be queried, for
  example in a locked or remote session. It is harmless.
- Only one kzgs instance per process, because PCSX2's GS uses globals.
- ps2xRuntime has its own global `struct GSVertex` (and `GSRegisters` and similar). PCSX2's `GSVertex` has the same
  name. No shared template instantiations were found, so there is no ODR clash today. Renaming the runtime's struct
  would remove the risk.
- In the test, putting around 16,000 full-screen 2x fills into a single frame hit the 2-second GPU watchdog on the
  Intel UHD 770 under D3D12 and Vulkan: the frame read back black, and the device was reset. No real frame comes close
  to that. The ring stress test uses IMAGE uploads for bulk data instead.
