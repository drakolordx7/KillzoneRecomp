# Reverse-engineering findings (SCUS-97402, CRC CAAEC49C)

Addresses are EE virtual addresses in `SCUS_974.02`. Function names are Ghidra auto-names.

## Scope
- `SCUS_974.02` = entire offline game. `cdrom0:\LOADER.ELF` is exec'd only for Killzone Online; `LOADER.ELF` loads
  `MODULES\BLGFX.REL` (online loading screen), `DNASMC.REL`, `NETWORK.REL`, `ONLINE.REL`. All online-only → out of scope.
- Game data: `FILES.DAT`, `FILES01-04.DAT`.
- ~~Native USB mouse support~~ — **wrong, corrected 2026-09-29**: `Use Mouse` / `Mouse-Look X/Y` are reflection properties of the
  engine camera classes (`Camera.Input: Use Mouse/Use Joystick/Use Keyboard`, `CamFirstPerson: Speed/Forward/Strafe/Mouse-Look X/Y/Min Pitch/Max Pitch`,
  `CamOrbit`), i.e. Guerrilla's PC tool/debug cameras. See "USB keyboard/mouse (lgkbm)" below: nothing reads mouse data.

## Frame timing (key for high refresh)
- `DAT_0055a6e0` — vsync counter (60 Hz NTSC fields).
- `DAT_0055a6e4` — float, seconds per vsync (presumably 1/60).
- `FUN_001bfe88(timer)` — timer init:
  - `+0x50` = `2 * DAT_0055a6e4` → seconds per tick (1/30 s)
  - `+0x54` = 1.0f (time scale), `+0x60` = 1, `+0x68`/`+0x6c` = `vsync >> 1`
- `FUN_001bff10(timer)` — frame limiter / delta:
  - loop: `now = vsync >> 1` (30 Hz ticks); `+0x6c = prev`; `+0x68 = now`; `elapsed = min(now - prev, +0x70)`; `+0x64 = elapsed`; spin while `elapsed < 1`.
- Community 60 fps patch: `0x1BFF3C` → `srl v0,v0,0` (drop the `>>1`, ticks become 60 Hz) and `0x1BFEAC` → `lui at,0x3F00` (0.5f scale on the tick length).
- **Conclusion:** simulation dt = `elapsed_ticks * seconds_per_tick`, clamped. Sim is dt-scaled → high refresh plan = replace the vsync-tick source
  with a finer host-clock tick and matching `+0x50`, then validate timing-sensitive systems at 60/144/240 (movement, fire rate, reload, anims).

## Widescreen / noise filter
- `0x005DA364` = 1 → native widescreen (pnach). `0x0055DF6C` byte → noise filter (only when `0x0057BA88 == 4`).
  No direct xrefs in Ghidra (accessed via base+offset) — locate owning struct later.

## USB keyboard/mouse (lgkbm)
- `FUN_002de720` loads `cdrom0:\IOP\USBD.IRX` ("usbd") and `LGKBM.IRX` ("lgkbm", arg `MaxKbd=1`), then `FUN_003d7490(1,0,1,cb)` = Logitech
  lgkbm lib init (max kbd, max mice clamped to 4; here 1 keyboard, 0 mice).
- RPC sid `0x61766973`, client @0x590D90, async with end func `0x3D9360` (iSignalSema 0x590DB8). Thread `FUN_003d8cf0`:
  cmd 3 (0x20) handshake {u16 maxKbd, u16 maxMouse, u32 ver 0x80001, u16 1} → IOP must echo ver at +0xA, 1 at +0xE, 0 at +0x10;
  cmd 1 (0x90) device list (count @+2, entries @+4 x 0x10: +2 type 0/1 kbd/2 mouse, +4 id, +0xE flags 0x8000 reportId/0x4000 wheel);
  cmd 2 (0x1D0) events (count @+2, records @+4 x 0x1C: +4 type, +6 id, +0xC err, +0x14 HID boot report); cmd 4 ack.
- Keyboard API (lock 0x590DBC, slots 0x590E18 x 0x110): `FUN_003d7708` open, `FUN_003d6f38`, `FUN_003d7058` read, `FUN_003d7918` key
  bitmap, `FUN_003d7858` close, `FUN_003d7828` changed-flag. **No mouse reader exists** (mouse ring 0x591258 x 0x68 is written, never read).
- Consequence: mouse aim must be an engine patch (player yaw/pitch); keyboard movement via the pad layer. The keyboard lib is likely online chat.

## Mouse (first pass, superseded)
- `Use Mouse` @0x5570FB ← `FUN_003c8a20`; `Mouse-Look X/Y` @0x557249/56, 0x55730A/17 ← `FUN_003ca130`, `FUN_003cb580` (options/menu construction).
- `MsgMouse` @0x5516A7 ← `FUN_00309a88` (registers 4 message handlers at 0x582B80..0x582C60 via `FUN_00172238`).
- Next: follow `MsgMouse` handler → where mouse deltas feed player aim.

## Library bindings
The ELF is stripped, so Sony library functions are identified by `ps2_analyzer`'s signature DB (131 bound automatically by
`tools/scripts/regen.py`). Functions the DB misses are bound manually in `config/killzone.toml`:
- `0x2B6060` `sceSifLoadModule(path, arg_len, args)` — thin wrapper over `0x2B5E38` `_SifLoadModule(path, arg_len, args, res, fno)`,
  which binds the IOP LOADFILE RPC (sid 0x80000006) via `0x2B5928` and spins until `client.server != 0`.
  Without the binding the boot hangs there: the runtime has no BIOS LOADFILE server. Caller: `FUN_00175548` (module loader loop).
- Loadfile neighbours already DB-matched: `sceSifLoadFileReset@0x2B5AB8`, `sceSifStopModule@0x2B5AF0`, `sceSifUnloadModule@0x2B5CF8`,
  `sceSifSearchModuleByName@0x2B5D88`.

## High refresh implementation (2026-09-29)
- Vsync counter `0x55A6E0` is written only by the two vblank handlers `FUN_00152018` and `FUN_0018f0d0`; `FUN_0018f0d0` also
  flips DISPFB1/2 every vblank → **displayed frame rate = vblank rate**, so the port raises the guest vblank rate R.
- Frame timer lives in the game object `*(0x559178)` at +0x50..+0x70, driven by main loop `FUN_001402b0`
  (`FUN_001bfe88` init once per session, `FUN_001bff10` wait+delta every frame).
- Instruction patches (config/killzone.toml [patches]): drop the `>>1` 30 Hz tick at 0x1BFEB4/0x1BFEB8/0x1BFF3C and the
  `add.s f0,f0,f0` doubling at 0x1BFEA0 → tick = 1 vsync, seconds/tick = `0x55A6E4`.
- `src/kz_timing.cpp` per vblank: `0x55A6E4` = 1/R; clamp `+0x70` scaled by R/30; fade level `0x55A7C8`
  (±8/+4 per vblank in `FUN_0018f0d0`) corrected to 60 Hz speed.
- FMV/attract playback (`FUN_0026ecc8`) times itself with `FUN_001466f8` (ms timer), only uses vsync parity for fields.
- Runtime: `ps2SetVblankPeriodMicros()` (runtime/ps2_host_gs.h) replaces the fixed 16667 us vblank period.

## Widescreen (found 2026-09-29 with PCSX2 PINE)
- Native 16:9 flag = word at `*(0x559178) + 0x34` (the global game object, which also holds the frame timer at +0x50..+0x70).
  The community pnach address 0x5DA364 is that field in PCSX2's heap layout; the runtime's heap differs, and writing
  0x5DA364 there corrupted rendering (grey, grainy menus). Game option label strings: "Aspect", "(4:3; 16:9)", "IsWideScreen".
- Noise filter pnach: clear byte 0x55DF6C while halfword 0x57BA88 == 4 (in a level). Forcing 0xFF elsewhere was wrong.
- Tool: tools/scripts/pine.py (PCSX2 PINE client; EnablePINE=true, PINESlot=28011 in tools/pcsx2/inis/PCSX2.ini).

## Mouse aim (2026-09-29)
- Profile control options are reflected properties (FUN_001d3478): `YawSensitivity` float at settings+0x138, `PitchSensitivity` +0x13C,
  (getters FUN_001d6cb8 / FUN_001d6cc8; settings = `*(*(obj+0x10))`), crouch toggle byte +0x142.
- `FUN_0023ee08` = JoystickControllerPlayerImp update (ctrl): ctrl+0x18 = player entity, ctrl+0x1C = local player index (-1 unused).
  Look rates: ctrl+0x3C = ctrl+0xA8 = (yawSens*1.5+0.5) * clamp(stickX,-1,1); ctrl+0xAC = (pitchSens*1.5+0.5) * clamp(-stickY,-1,1)
  (negated again if the invert option FUN_001d6ca8 is set). The control state handed to the player is ctrl+0x24 (rates at +0x84/+0x88).
- `FUN_0021a8c8(dt, player)` = player update: `FUN_0021c550(dt, rate84 * maxYaw(vt+0x190) * dt, rate88 * maxPitch(vt+0x194) * dt, player)`.
  `FUN_0021c550` adds the deltas: yaw = player+0x148 + d (setter FUN_00218818), pitch = player+0x14C + d (setter FUN_00218970, clamps).
  So turning is linear in the rate; no acceleration curve after the stick read.
- `FUN_0023e9c8` = pitch auto-centre while walking with no look input (gated by profile option FUN_001d6c68) - a pad assist.
- Patch (src/kz_aim.cpp): hooks 0x23EE08 (remember player of local index 0), 0x21C550 (add mouse counts * 0.05 deg * sensitivity to
  f13/f14 for that player), 0x23E9C8 (skipped while keyboard/mouse is the active device). KZ_AIM=off disables, KZ_AIM_LOG=1 logs angles.
