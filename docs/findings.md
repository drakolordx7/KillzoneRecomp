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
