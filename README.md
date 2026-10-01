# KillzoneRecomp

A native Windows PC port of **Killzone** (PlayStation 2, 2004), made by statically recompiling the game's PS2 code to
C++ with [PS2Recomp](https://github.com/ran-j/PS2Recomp). PCSX2's hardware renderer, microVU, IPU and SPU2 are linked in
as libraries.

**No game code or data is in this repository.** You need your own disc image of Killzone NTSC-U (SCUS-97402).

Developed with AI assistance (Claude Code). Findings, measurements and the reasoning behind each change are in
[`docs/findings.md`](docs/findings.md) and [`patches/README.md`](patches/README.md).

## Status

Test build. Boot, menus, profile creation and the first mission's gameplay are verified in automated runs; longer
play-throughs and hands-on testing are ongoing.

- **Frame rate:** runs at the monitor's refresh rate. Measured in gameplay: about 117-120 fps at 120 Hz and about
  130-140 fps at 144 Hz. The game's clock runs at real time at 120 Hz (the frame timer is rescaled, not sped up).
- **Mouse and keyboard:** mouse aim is an engine patch (raw counts go into the game's own look rate: no stick
  acceleration or dead zone), with fully rebindable keys. Controllers work through SDL3 with the original PS2 layout.
- **Graphics:** internal resolution up to 8x (supersampling), native 16:9, sharp scaling, anisotropic filtering,
  optional FXAA; D3D11, D3D12 or Vulkan.
- **Launcher:** display, graphics, controls and audio settings; checks the disc image is the supported version.
- **Not included:** Killzone Online (LOADER.ELF and the network modules); its servers are long gone.

## Default controls

| | |
|---|---|
| Mouse | aim |
| Left / right / middle mouse | fire / hold to aim / secondary fire |
| W A S D | move (menus: navigate) |
| Left Shift / C or Left Ctrl | sprint / crouch |
| R / G / Q or wheel | reload / grenade / switch weapon |
| E or F / X or Mouse4 | use / special item |
| Escape / Tab | pause / objectives |
| Arrow keys, Enter | menus |

The profile name can be typed directly on the game's on-screen keyboard. Bindings live in `killzone.ini` under
`[Bindings]`.

## Building

Windows x64, Visual Studio 2022 or later (MSVC), CMake 3.21+, Ninja, Python 3, a CPU with AVX2.

1. Clone with submodules: `git clone --recursive https://github.com/drakolordx7/KillzoneRecomp`.
   `ext/PS2Recomp` is the `killzone` branch of [drakolordx7/PS2Recomp](https://github.com/drakolordx7/PS2Recomp).
2. Dependencies that are not committed:
   - PCSX2 source in `ext/pcsx2` (verified against commit `646df006a4`), unmodified.
   - [pcsx2-windows-dependencies](https://github.com/PCSX2/pcsx2-windows-dependencies/releases/tag/latest-windows-dependencies)
     extracted to `ext/kzgs/deps`.
   - SDL3 3.4.16 (VC development package) in `ext/sdl3`.
3. Extract `SCUS_974.02` from your disc image into `game/`.
4. Build the recompiler: `tools\scripts\build_ps2recomp.bat`.
5. Recompile the game: `python tools\scripts\regen.py` (writes `generated/`, about 20k C++ files).
6. Build: `tools\scripts\build.bat RelWithDebInfo` -> `build\RelWithDebInfo\killzone.exe`.
7. Optional: `tools\scripts\package.ps1` makes a player folder in `dist\Killzone`.

`killzone.exe --selftest` runs the unit checks. `tools\scripts\run_headless.ps1` runs the game without a window, with
scripted input (`KZ_INPUT_SCRIPT`) and periodic screenshots, which is how everything here was verified.

## Layout

- `src/` - the port: game overrides and patches, input, mouse aim, timing, launcher, config, GS/VU/IPU/SPU2 glue.
- `ext/kzgs`, `ext/kzvu`, `ext/kzipu`, `ext/kzspu2` - PCSX2's GS, microVU, IPU and SPU2 built as static libraries.
- `ext/PS2Recomp` - recompiler and runtime (fork, `killzone` branch).
- `patches/` - the runtime changes as patches, with a description and measurements for each.
- `config/killzone.toml` - PS2Recomp configuration for SCUS-97402.
- `tools/` - build, test, profiling and Ghidra helper scripts.
- `docs/` - findings and performance research.

## License

GPL-3.0 (see `LICENSE`). The port links PCSX2 (GPL-3.0-or-later) and PS2Recomp (GPL-3.0).
Killzone is a trademark of Sony Interactive Entertainment; this project is not affiliated with Sony or Guerrilla Games.
