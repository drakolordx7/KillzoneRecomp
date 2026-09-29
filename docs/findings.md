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

## Controls (2026-09-29)
- Action enum (reflection string at 0x5494B4): walk, strafe, pitch, turn, allowlook, sprint, primaryfire, secondaryfire,
  action, selectweapon, grenadethrow, specialitem, reload, stancechange, zoommode, zoom (+2 pause/objectives entries).
- Profile settings object (found at 0x102C880 in a gameplay RAM dump) holds the controller map at +0xEC (one int per
  action, same order; getter FUN_001d6c58 returns settings+0xEC), YawSensitivity +0x138, PitchSensitivity +0x13C
  (default 0.333). Button ids use the DS2 pressure order: 0 right, 1 left, 2 up, 3 down, 4 triangle, 5 circle,
  6 cross, 7 square, 8 L1, 9 R1, 10 L2, 11 R2, 12/13 select/start, 14 L3, 15 R3; axes are 100+ (negative = inverted).
- Default map: fire R1, secondary R2, action X, switch weapon O, grenade L1, special square, reload triangle,
  crouch L2, sprint L3, zoom mode R3. No jump in Killzone 1. Buttons are read as analog values (FUN_001b33b8,
  pressed if > 0.1 in FUN_001b3548), so the pad reply fills the DS2 pressure bytes too.
- Virtual keyboard (profile name): FUN_001af620. obj+0x228 = selected 3x3 block (8 = centre), picked with the left
  stick while held; the face buttons type the block's 4 letters (triangle top, circle right, cross bottom, square left).
  Centre block: X enter, triangle cancel, square backspace, O space.

## VU0 micro mode (2026-09-29)
- VCALLMS targets in gameplay (first mission, microVU0 at clamp 3, calls per 10 s at ~9 frames/s): 0x870 ~0.84M,
  0x778/0x7B0 ~0.42M each (all three in FUN_00505a78, a loop: `cfc2.i $v0,$vi1` after 0x870 reads a test result),
  0x020/0x270/0x520/0xD18 ~75-90k, 0xC80 ~55k, 0x2C0/0x4F0/0x000/0x6C8 < 1k. About 2M VU0 calls per 10 s, ~270 ms of
  host time. Captured programs take 4-92 VU cycles; none uses M bits or VU1 registers.
- VU0 now runs on kzvu's microVU0 (src/kz_vu.cpp, runtime patch 0007). Old path (KZ_VU0=interp) vs microVU0 in the
  same headless run: ~7.0 vs ~10.0 vif frames/s in gameplay.
- NaN/Inf in guest memory: from level load on (~t=125 s headless), VU0 inputs loaded from memory contain 0x7FC00000
  (the x86 default NaN; the PS2 has no NaN). VU0 never produced one from finite inputs (KZ_VU0_STATS=1 counts both), so
  the source is on the EE side; the generated FPU macros are plain IEEE (`FPU_DIV_S` = a / b in
  ps2_runtime_macros.h, no PS2 max-value clamping). First seen in the input of program 0xC80 called at 0x3AC464
  (`lqc2 $vf1, 0($a1)`). Not traced further.
- VCALLMSR (0x31A8D8, 0x31A994 in FUN_0031a068) was generated as `ctx->vi[27]`, past the 16-entry vi[] (reads vu0_r);
  the runtime now uses vu0_cmsar0 (patch 0007). No capture came from these two sites, so the fix is untested in-game.
- PS2Recomp's interpreter vs microVU0/PCSX2 interpreter on clean captured inputs: identical except program 0xD18,
  where `FMAND vi1, vi3` sees a different MAC flag and the program branches the other way (ext/kzvu/README.md).

## Gameplay 3D (2026-09-29)
- Symptom: in the first mission only the HUD, tutorial prompts and post-effect passes rendered, over a red scene buffer.
  The EE was not submitting the world. Measured per frame: ours had 108 MSCAL, 468 UNPACK and ~250 DMA tags. PCSX2 had
  915 MSCAL, 6803 UNPACK and 4381 tags. The PCSX2 figures come from the frame chain in a gameplay savestate.
- **PCSX2 gameplay reference:** `tools/pcsx2/sstates/SCUS-97402 (CAAEC49C).09.p2s` and `.10.p2s`, taken at the first
  tutorial prompt ("Press R1"). `tools/scripts/pcsx2_drive.py` produced them, driving PCSX2 through PINE and the pnach
  pad hook.
  - Useful addresses (same in both runtimes, static data):
    - `0x559040` holds the start of the frame's VIF1 chain. `chain2buf.py` walks it and `vifparse.py` parses it.
    - RenderZoneManager vtable `0x531448`: +0x30 zone count, +0x34 zone array.
    - RenderZone world AABB: min at +0xA0, max at +0xAC.
    - Player entity vtable `0x525710`: position at +0x210.
- **Chain of causes, each fix measured in a run:**
  1. **Unaligned LQ.** FUN_003107a8 builds each RenderZone's world AABB with FUN_002c84d8, which loads the local box
     max with the unaligned `lq/lq/qfsrv` idiom. The generated LQ did not align the address, so every zone box came out
     flat and the camera was in no zone.
     - After the fix, the zone boxes match PCSX2 to 1 ulp, and the weapon plus the world batches (27k strip vertices per
       12 vsyncs) are submitted.
     - The camera was then 1.7e5 units below the level: the player fell.
  2. **SQRT.S read fs.** After the fix, the player x/y equal PCSX2's exactly (95.354, -480.852); before, they were off
     by ~0.02. The player still fell.
  3. **MMI lane bugs (PEXEW and others).** After the fix, the player stands at z 2.654 (PCSX2: 2.659), and the level,
     weapon and HUD render.
  4. **VIF1 DMA chains cut at 4096 tags.** A gameplay frame is ~4400 tags. After the fix, textures are clean and there
     are no white screens or host crashes (5 of 5 runs clean, against 3 crashes in ~6 runs before).
  5. **VIF1 UNPACK semantics.** The legacy code bends geometry.
- Before/after in-game rate (vif counter per second, t=150-230 s, `KZ_FPS=66`, `PS2X_IOP_BATCH=1`):
  - main build without these fixes, no 3D: 12.1
  - with the fixes, full scene: 10.7 and 10.3 (two runs)
- **Boot hang:** with `PS2X_IOP_BATCH` at its default (4096), 2 of 4 boots hung at dma=18. The game thread spins in
  FUN_00149f30 at 0x14a028, waiting for an IOP file read. With `PS2X_IOP_BATCH=1`, 0 of 4 boots hung.
  Root cause found and fixed 2026-09-29 (patch 0013), see "FilePS2 / PFILE_R.IRX" below; it hangs at batch 1 too.

## FilePS2 / PFILE_R.IRX (2026-09-29)
- EE side: FUN_00149f30 double-buffers a file into two halves. Each half has a pending-bytes counter, at +0x34 and
  +0x38 of the stream object. The EE sets a counter to the request size, then sends the request with
  FUN_0014bc08/0014bd00/0014bdf8: nowait sceSifCallRpc on client 0x55B710, sid 0x66662012, fn 800 (one request) or
  0x330 (two). The 0x20-byte request is {mode 0x8100000/0x1100000, device, file, offset, size, EE buffer, EE
  counter address}. The loops at 0x14a028/0x14a1c0 spin until both counters are <= 0.
- IOP side (PFILE_R.IRX, loaded at 0x10000; offsets below are module offsets):
  - The RPC server FUN_00002f00 (0x800/0x330 -> FUN_00001c1c) appends a node to the streaming queue: head 0x4094,
    tail 0x4098, queued counts 0x406C/0x4070. The queue lock is semaphore 0x409C.
  - The streaming thread FUN_000022a4 (prio 0x33) picks a request (FUN_00001e9c), reads with sceCdRead or ioman,
    and DMAs the data with sceSifSetDma.
  - Per 32 KB chunk it reports progress with `sceSifSendCmd(0, {counter, remaining, ...})` (FUN_00002c64). The
    game's EE cmd-0 handler stores `remaining` into the EE counter, and the final chunk sends 0.
  - The thread then unlinks the node under the lock.
- The hang: the emulator runs RPC server functions outside any IOP thread, where `WaitSema` did not block. An append
  that landed while the streaming thread was preempted between `v0 = cur->next` (0x26C4) and `if (v0 == 0) tail = 0`
  (0x26D8) was lost. Its completion command was never sent, so the counter stayed positive. Details, evidence and
  hang counts: patches/README.md, 0013.
- PSOUND_R.IRX's RPC also waits on a held semaphore from RPC context (seen in traces, ra 0x71260); patch 0013 covers
  it. From code reading, PFILE_R's close path (FUN_00001278, RPC 0x210) polls with DelayThread while a file has
  running requests. DelayThread outside a thread still returns at once. In traced boots it ran outside a thread only
  once per boot, from USBD at start, never from PFILE_R.
