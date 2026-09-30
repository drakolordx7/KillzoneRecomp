# KillzoneRecomp

Native PC port of **Killzone (PS2, 2004)** via static recompilation (PS2Recomp) — GPL-3.

No game code or data is in this repo. You supply your own disc image.

## Target build
| | |
|---|---|
| Serial | SCUS-97402 (NTSC-U, v1.00) |
| Boot ELF | `SCUS_974.02`, PCSX2 CRC `CAAEC49C`, sha1 prefix `8aedc3e29418` |
| Scope | Offline game only. `LOADER.ELF` + `MODULES/*.REL` are Killzone Online (DNAS/network, servers dead) — not recompiled. |

## Layout
- `ext/PS2Recomp` — recompiler + runtime (submodule-style clone)
- `config/killzone.toml` — PS2Recomp config (from Ghidra export)
- `src/` — our code: game overrides, PC features
- `tools/scripts/` — `build_ps2recomp.bat`, `build.bat`, `capture_window.ps1`, `vsenv.bat` (VS 2026 / MSVC 14.51)
- gitignored: `game/` (extracted disc files), `generated/` (recompiled C++), `work/` (logs, Ghidra export, reference shots), `ghidra/`, `tools/` downloads

## Pipeline
1. Extract `SCUS_974.02` from the ISO into `game/`.
2. Ghidra 12.1.3 + emotionengine-reloaded headless import → `ExportPS2Functions.java` → `work/config.toml` + `work/functions.csv` (16,931 functions).
3. `ps2_recomp config/killzone.toml` → `generated/` (~82k files).
4. `tools\scripts\build.bat` → `build/RelWithDebInfo/killzone.exe`.
   Optional clang-cl build (opt-in; about 12 % less EE time per frame, see `docs/findings.md` "clang-cl build"): `tools\scripts\build_clang.ps1` → `build/clang/killzone.exe`.
5. Reference: portable PCSX2 v2.8.2 in `tools/pcsx2` (BIOS supplied by the user).
