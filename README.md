# Killzone for Windows (native PC port)

A native Windows port of Killzone (PlayStation 2, 2004) by [drakolordx7](https://github.com/drakolordx7). The game's code is recompiled to C++ with [PS2Recomp](https://github.com/ran-j/PS2Recomp), so it runs as a PC program with mouse and keyboard aim, high frame rates, higher resolutions and ultrawide support.

[Download the latest build](https://github.com/drakolordx7/KillzoneRecomp/releases/latest)

No game files are included; you need your own disc image of Killzone, US version (SCUS-97402). Not affiliated with Sony or Guerrilla Games.

## Requirements

- Windows 10 or 11, 64-bit
- CPU with AVX2: Intel 4th gen (Haswell), AMD Ryzen or newer
- GPU with Direct3D 11, Direct3D 12 or Vulkan

## Usage

1. Unzip the build into a folder and run `killzone.exe`.
2. In the launcher, browse to your Killzone `.iso`.
3. Pick your settings and press Play.

Settings are in `killzone.ini` and saves in `disc\mc0`, both next to `killzone.exe`. To update, unzip a new build over the old one; settings and saves stay.

Mouse and keyboard work out of the box, and so do controllers with the original PS2 layout. The launcher's Controls tab lists the keys; change them in `killzone.ini` under `[Bindings]`. Ultrawide, intro skip, toggle sprint and the graphics options are in the launcher too; `README.txt` in the zip lists the controls.

## Reporting problems

[Open an issue](https://github.com/drakolordx7/KillzoneRecomp/issues) with `killzone.log` from the game folder (the next start overwrites it; the one before is kept as `killzone.prev.log`). Say which level you were on and what you were doing.

## Contributing

- `master` is the main branch and what releases are built from. Open pull requests against it.
- `ext/PS2Recomp` is the `killzone` branch of [drakolordx7/PS2Recomp](https://github.com/drakolordx7/PS2Recomp). General runtime fixes also go upstream as pull requests.
- Building needs Windows x64, Visual Studio 2022 or later, CMake, Ninja and Python 3:
  1. `git clone --recursive https://github.com/drakolordx7/KillzoneRecomp`
  2. Put the PCSX2 source in `ext/pcsx2` (commit `646df006a4`), the [PCSX2 Windows dependencies](https://github.com/PCSX2/pcsx2-windows-dependencies/releases/tag/latest-windows-dependencies) in `ext/kzgs/deps` and the SDL3 VC development package in `ext/sdl3`.
  3. Copy `SCUS_974.02` from your disc into `game/`.
  4. `tools\scripts\build_ps2recomp.bat`, then `python tools\scripts\regen.py` (writes about 20,000 C++ files to `generated/`), then `tools\scripts\build.bat RelWithDebInfo`.
  5. `tools\scripts\make_release.ps1 -Version x.y.z -From build\RelWithDebInfo` makes the release zip.
- Reverse-engineering notes and measurements are in [`docs/findings.md`](docs/findings.md); runtime changes are described in [`patches/README.md`](patches/README.md).

## Credits

Port by [drakolordx7](https://github.com/drakolordx7). Built on [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j and contributors, and on [PCSX2](https://github.com/PCSX2/pcsx2)'s GS renderer, microVU, IPU and SPU2, linked in as libraries.

Testing and reports: mcdermott87, Ash01Live, Lockefinder1, VGEsoterica, Mister-Mannered and everyone who sent logs.

Third-party: PCSX2, SDL3, Dear ImGui, FFmpeg and PCSX2's Windows dependencies.

## License

GPL-3.0, see [LICENSE](LICENSE). PCSX2 is GPL-3.0-or-later and PS2Recomp is GPL-3.0. Killzone is a trademark of Sony Interactive Entertainment.
