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

## VIF1 worker thread (2026-09-29, patch 0016)

VIF1 DMA, VU1 (microVU1), XGKICK and the GIF arbiter run on a worker thread. The design and every EE <-> worker interaction
point are in patches/README.md (0016). This section has the measurements.

**Method.** The `tools/scripts/run_headless.ps1` scene (`KZ_FPS=60`, `KZ_IPU=off`, the task's `KZ_INPUT_SCRIPT`), 235 s;
in-game rate = increase of the heartbeat's `vif=` counter per second between t=170 s and t=230 s. The scene is not
deterministic (tutorial events fire on wall-clock time), so single runs of one configuration differ by about +-10 %. The
machine load of every run was logged once a second (`work/<run>/load.csv`: total CPU %, `cl.exe` and `killzone*`
process counts). No other game instance or compile ran during any run listed below; the background CPU load (other
agents' python/ghidra/steam processes, not game instances) was 32-50 %, higher for the runs marked `*` (> 40 %), which
read low. "Before" = `build/vifth_base`, the synchronous build from just before this change (same tree otherwise,
including the IOP thread code, which is selectable with `PS2X_IOP_THREAD` in both).

| VIF1/VU1/GIF | IOP thread on (default): frames/s | IOP thread off (`PS2X_IOP_THREAD=0`): frames/s |
|---|---|---|
| synchronous, before this change | 31.3, 24.4* | 23.9*, 15.9* |
| synchronous, this change (`PS2X_VIF1_THREAD=0`) | 32.0, 30.7* | 27.9, 29.9 |
| threaded (default) | 46.0, 44.9 | 33.4*, 34.2 |

More clean runs of the final code, before the IOP switch was set explicitly (IOP thread state as the tree had it at the
time): threaded 43.9, 45.4, 50.8, 48.0 (worker not pinned to P-cores), 44.4 (vblank waits for the worker); synchronous
before the change 32.8, 33.1, 28.8. Threaded is about 1.45x the synchronous path of the same build and 1.5x the old
build (IOP thread on); the synchronous fallback is not slower than the old code.

**What the worker costs** (`PS2X_VIF1_STATS=1`, gameplay, ~45 frames/s): the worker is busy 23-60 % of a core (VU1 ~80 %
of that, including XGKICK; the host-GS hook ~10 %). The EE thread pays ~1.9 % for the chain walk and snapshot copy
(~900 MB of chains per 10 s), <0.1 % for dispatch, and barriers wait < 60 ms per 10 s in total (the one VIF1 register
write per job found the worker idle in more than 98 % of the cases). The guest vblank never waits for the worker. A job
takes about 10 ms on the worker on average, so a D1 completion interrupt arrives that long after the CHCR store instead of inside it.

**Things that did not matter.** Waiting for the worker at every vblank (`PS2X_VIF1_VSYNC_SYNC=1`) instead of ordering
the GS half of the vsync behind the worker's queue: 44.4 vs 43.9-50.8 (the ordered form is the default: it costs
nothing and never stalls the EE). Restricting the worker to P-cores on the i9-12900K (`PS2X_VIF1_CORES=perf`): 43.9 with,
48.0 without; off by default. An early threaded run measured 20 frames/s; it overlapped a full rebuild by another agent
(12 `cl.exe`) and is not comparable.

**Checks.** 6 + 6 boots of 50 s (`dma=` advanced in every 10 s interval, exit code 0), more than 10 full 235 s threaded
gameplay runs, 60 s boots with `KZ_VU=interp` (PCSX2's interpreter on the worker) and `KZ_VU=builtin` (falls back to synchronous, as designed), stress runs
with a slow worker (`PS2X_VIF1_DELAY_US=700`: gameplay reached, frames intact; 4000 us busy-waits stretched the loading screen
because the game waits for each GIF DMA to go idle before starting the next), kzvu_test 575 checks and kzvu0_test 303
checks (0 failures, both after the VU0/VU1 split), kzvu_linkcheck_ab/ba link. Rendering compared with the old build at
the same timestamps (menus t=10-130, weapon/HUD/level/explosions t=140-230): same screens, same artifacts (the
banded highlight bars in the profile and character menus are in the old build too), no flicker, missing geometry or
corruption. The GIF stream cannot be compared byte for byte across runs (the game is not deterministic): with
`KZ_GS_HASH=1`, 493 menu frames have byte-identical packet streams in a synchronous and a threaded run, the others are
animation frames that differ between any two runs; packets per frame have the same distribution, but threaded runs had more
empty vsyncs (390 of 4500 vs 90) and more vsyncs with two chains (147 vs 58): the game's kicks fall differently between
the vblanks when the completion interrupt is late.

**VU1 recompiles** (asked for: `mVUcompileJIT<1>` ~6 % in steady-state gameplay). `KZ_VU_STATS=1`, threaded, first level,
per 10 s:

| t (s) | 100 | 120 | 130 | 140 | 150 | 160 | 170 | 180 | 190 | 200 | 210 | 220 | 230 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| MSCAL (k) | 45 | 41 | 176 | 178 | 353 | 412 | 441 | 406 | 410 | 327 | 375 | 240 | 71 |
| MSCALs that emitted JIT code | 1 | 0 | 38 | 203 | 230 | 166 | 121 | 70 | 47 | 19 | 19 | 293 | 128 |
| JIT emitted (KB) | 13 | 0 | 1408 | 5358 | 3857 | 2372 | 1720 | 706 | 445 | 125 | 216 | 5014 | 981 |
| new microPrograms | 3 | 0 | 311 | 1080 | 535 | 289 | 219 | 74 | 57 | 16 | 21 | 607 | 164 |
| MPG code-generation changes (k) | 0 | 0 | 9.1 | 13.1 | 29.6 | 35.7 | 36.2 | 32.7 | 32.8 | 28.1 | 31.3 | 23.3 | 4.7 |

Cache resets: 0 in 235 s (22 of 61 MB used, 3700 programs). Cause: microVU keeps one program list per MSCAL start PC,
and Killzone enters its skinning/lighting programs at ~540 different start PCs (entry addresses 0x30 bytes apart, which looks like an
unrolled loop entered `6*n` instructions before its end), times several microcode versions. Every (start PC, code) pair is
compiled once, on first use, from scratch, so the cost comes in bursts whenever new content appears (level start at
t=130-160, new effects at t=220) and is near zero in between (19 compile events in a whole 10 s at t=200). It is not repeated
compilation: of 951 new programs logged with the first differing micro-memory word, 540 were the first program for their
start PC and the other 411 all differ from the older program for the same PC inside its compiled ranges (different microcode
uploaded to the same addresses); none matched a cached program's content. The ~3300 MPG uploads per second do force
a program search per start PC after each change, which is cheap (`recMicroVU1::Clear` 0.2-0.7 % of the worker's busy
time, the search itself is inlined into `mVUexecute<1>`, 1 %); only ~5 % of the uploads are byte-identical to the code already in VU1 memory (those no longer invalidate
anything; `PS2X_MPG_SKIP_SAME=0` restores the old behaviour). With the worker, the compile bursts are off the EE thread
(`mVUcompileJIT<1>` ~4 % of a core in the profile window). Not fixed: a long session may fill the 61 MB cache, and
microVU then resets and recompiles everything (not seen yet); sharing compiled blocks between start PCs would need a change
inside microVU.

**Render-feeding costs on the EE thread, before -> after** (asked for: vector insert ~7 %, `vif1UnpackPcsx2` ~6 %):
- DMA chain walk + snapshot (`std::vector::insert` per tag into a fresh ~3 MB buffer per frame, freed on another thread):
  measured with `PS2X_VIF1_STATS=1` in the same binary, `PS2X_CHAIN_POOL=0` vs default, ~800-900 MB of chains per 10 s:
  616-671 ms per 10 s (6.2-6.7 % of the EE thread) -> 181-202 ms (1.8-2.0 %). Most of the old cost was page faults and
  reallocation copies, not visible as `_Insert_counted_range` self time (0.2 % in the profile); with the pool the worker's
  `NtFreeVirtualMemory` (0.4 %) is gone too.
- `vif1UnpackPcsx2` (`KZ_PROFILE=190,30`, share of the thread's wall time): synchronous EE thread 5.0 % -> 1.0 %; threaded worker
  4.9 % of a core (16 % of its busy time) -> 0.9 % (2.6 % of busy). Plain UNPACK (no mask, STMOD 0, WL <= CL) takes one SSE
  load/convert/store per vector; masked UNPACKs are 13 % of Killzone's ~8700 per frame. `PS2X_VIF_UNPACK_CHECK=4` ran every
  4th UNPACK of a boot + gameplay run both ways (5.8 M checked): 0 differences in VU1 memory and VIF registers.
- Heap traffic on the EE thread (`RtlAllocateHeap` 2.5-3.2 %, `RtlFreeHeap` 1.4-1.5 %) is not from these buffers; unchanged.

## EE thread frame budget (2026-09-30, patch 0019)
Measured in-process (`PS2X_SCHED_STATS=1`: counters and rdtsc, no sampling) in the gameplay window t=150..200 s, `KZ_FPS=120`.
The machine was shared with 2-3 other game instances and builds, so absolute numbers are 10-20 % worse than a quiet
machine (the least loaded run of the final binary measured 73 fps with the sampling profiler on).
- **The EE thread never idles.** `idleWaits=0` in every 10 s window. Per second of wall time: ~70-80 % in guest functions,
  ~5-6 % in the scheduler loop around them (~9 % before patch 0019), **10-25 % (100-240 ms, ~100 waits/s) asleep in
  `EeScheduler::processDueDeadlines`**: the guest clock reached a vblank's cycle deadline before the host reached its
  time, so the scheduler sleeps until the vblank's host deadline. That sleep is what the 32 % "wait site" in the earlier
  profile was.
- **Frame time is a staircase.** The finished frame list is kicked at a vblank, so a frame whose EE work ends at 14 ms
  starts its successor at the next vblank (16.7 ms) and the EE sleeps the difference. EE host time per displayed frame,
  (1000 ms - pacing sleep) / fps: 9.6-13.8 ms in sequential runs (70.8-58.6 fps), 12.1-15.3 ms in concurrent pairs (57-66 fps). fps only moves when
  frames cross a vblank boundary: 6 concurrent pairs of "all patch-0019 switches off" against "on" gave EE ms/frame -0.83,
  -0.83, -0.60, -0.48, -0.47, +0.06 (median -0.54 ms, -3.8 %) and fps 60.3 -> 59.6 (median of six, noise). Getting from
  ~14 ms to under 8.33 ms needs the guest-code side (about 80 % of the thread), not more fixed-cost trimming. On a
  quieter machine the EE has more slack: the least loaded run (73 fps, profiler on) had the EE thread asleep in
  `processDueDeadlines` for 47 % of all samples (busy 51.5 %, ~7 ms per frame including the wait loops), and the sequential
  runs of the final binary measured 9.6, 11.2 and 13.8 ms/frame at 70.8, 66.6 and 58.6 fps as the other instances on the
  machine came and went (pre-patch binary, same sequence: 68.3, 65.8, 64.6 fps; final: 70.8, 66.6, 58.6).
- **The VIF1 worker is at least as much of a limiter.** `PS2X_VIF1_STATS=1` (68 fps, t=150..200): one job per frame
  (601-673 per 10 s), worker busy 5.84-6.37 s per 10 s = 9.1-10.0 ms per frame, of which VU1 (microVU1 run + JIT compile)
  4.88-5.30 s = 7.6-8.3 ms and the host-GS hook 0.64-0.70 s; the EE's chain walk is 0.32-0.36 s per 10 s. The game kicks the
  next frame list only when D1 is idle (`FUN_00151fc8`, docs/perf_research.md section 2.6; not re-checked here), so a worker that needs more than one vblank period (8.33 ms) per frame gives
  2-vblank frames whatever the EE does. 120 fps needs the worker's per-frame time (mostly VU1) well under 8.3 ms too,
  not only the EE thread.
- **The guest cycle estimate is not the limiter.** (Superseded once the EE was no longer the busiest stage: it is, see "Frame pipeline timeline".) `PS2X_EE_CYCLE_SCALE=0.5` (charge half the cycles per call/back edge; a
  temporary knob, not kept) cut the estimated cycles from 215 to 135 M/s and left fps at 57.5 vs 58.8 in a concurrent pair.
- **Where the ~1 M scheduler dispatches/s come from** (`PS2X_SCHED_STATS=2` histograms; every dispatch is an entry into
  a guest function from the scheduler loop): tail jumps between `entry_*` fragments and the returns after a
  non-local unwind. `ps2CgAfterCallSlow` (ps2_cg.h) sets `g_ps2GuestUnwinding` when a callee returns with `ctx->pc` != the
  return address, so every enclosing guest frame returns to the scheduler, which dispatches the target and then, one
  dispatch per level, each caller's resume label. Top dispatch pcs (per second, one window): 0x17f8e0 31k, 0x185e20 30k,
  0x33cd84 29k, 0x3fb18c/3fb184/3fb108 20k each, 0x401750/4017a4/401720 17k each, 0x3a3c18/3a3ee4 12k each, and, when the
  game sits in its flag-wait loop `FUN_0014fd90` (spins on a byte cleared by an interrupt handler, reading COP0 Count),
  0x14fe28/14fe38/14fe54/14fec0/14fec8 at 47-152k each. Untried because it edits ps2_cg.h (full rebuild): let
  `ps2CgAfterCallSlow` call a table function at `ctx->pc` in a loop (a trampoline) while the pc is a table entry other than
  `fall`, instead of unwinding. That turns each of those dispatches into a plain C call and keeps the caller frames alive.
- **Locks.** Before the DMAC-drain fast path (another agent's change to `consumeCompletedDmacCauses`, PS2X_DMAC_DRAIN_FAST)
  `RtlAcquire/ReleaseSRWLockExclusive` + `Mtx_lock/unlock` were 5.0 % of the EE thread, all under `PS2Runtime::Store32` ->
  `drainCompletedDmacHandlers`. A profile of the final binary (`work/profile_pa2.txt` style, gameplay) shows no SRW/mutex
  entry above 0.1 % on the EE thread except `IopSubsystem::lockExec` in `handleRpc` (0.2 %, RPC path). Nothing else takes a
  lock per checkpoint, per dispatch or per call.
- **VU0 calls in gameplay:** ~470 k/s (`KZ_VU0_STATS=1`), start PCs by share of calls: 0x520 21 %, 0x270 15 %, 0x020 14 %,
  0xD18 12 %, 0x870 9 %, 0xC80 9 %, 0x910 5 %, 0x7B0 4 %, 0x778 4 %. The three programs of `FUN_00505a78`
  (0x870/0x778/0x7B0) are 17 % of the calls in this window, not the 84 % of the first measurement (that was a different
  scene), so hand-translating them would not remove most of the cost.
  `kzvu0_test --bench` (host cost of one call incl. the context copies, min of 3 runs, 11 captured start PCs): original
  path 136-194 ns (median 160), `kzvu0Call` 64-117 ns (median 79). Where the rest goes: ~45 ns is microVU's fixed
  dispatch cost (an E-bit-only program at 0x000 takes ~45 ns to run), ~10 ns the MXCSR switch, the copies ~20 ns.
- **EE timers.** `advanceEeTimers` was called at every checkpoint (~0.8 M/s): 28-30 ns each (rdtsc pair, ~10 ns of it
  instrumentation); lazy: 14-15 ns. Verified with `PS2X_LAZY_TIMERS=verify` (eager shadow copy compared at every flush and
  register read; an interrupt found by the shadow must be raised by the same call): 102 508 comparisons, 0 mismatches,
  2 014 interrupts, 200 M early returns checked in a 235 s run. `KZ_VU0_VERIFY=8` in the same run: 5.04 M calls checked
  (both paths from the same state, all registers/flags/TPC/VPU_STAT and VU0 data memory), 0 mismatches.
- **Boot flake (not from these changes).** About 1 boot in 30-40 under load ends in `[IOP] module arena exhausted` (the
  game loads `MCSERV.IRX` again and again until the IOP module arena is gone, then exits): 3 of ~115 boots, with the
  patch-0019 switches on (2) and off (1), always when several instances were booting at once. Log signature: a
  `load-emulated id=N path=...MCSERV.IRX` line repeating in stdout, hundreds of thousands of `module arena exhausted`
  lines in stderr. IOP module loading is not part of this patch.

## IOP RPC offload (2026-09-30, patch 0021)

The EE thread used to run the IOP's RPC server functions inline (`SifCallRpc` -> `handleIopRpc` -> `IopEmulator::handleRpc` ->
`callFunction` -> interpreter). Design and switches: patches/README.md, 0021. Measurements:

**What the game calls** (`PS2X_IOP_RPC_STATS=1`, before the patch, headless gameplay scene at 120 Hz vblank, 235 s, 40 326 calls
in the run; wall time on the EE thread; `insns` counts every IOP instruction executed during the call, including other IOP
threads that run while the server function waits on a semaphore or event flag). Gameplay window t=150..200 s (differences of the
cumulative tables):

| sid / function | mode | calls | wall in window | avg per call | insns per call |
|---|---|---|---|---|---|
| 0x06662012 fn 0x600 (sound driver, per-vblank command list, send 16-64 B, no end function) | nowait | 3 219 | 3 422 ms (6.8 % of the window) | 1 063 us | ~46 500 |
| 0x66662012 (PFILE_R) fn 0x400 | nowait | 3 219 | 34 ms | 11 us | ~215 |
| 0x66662012 fn 0x320 (read request) | nowait | 0 in the window, 623 in the run (load time) | | 16 us | 200 |
| all of `SifCallRpc` on the EE thread | | 6 438 | 3 752 ms (7.5 %) | 627 us (incl. the two above) | |

Blocking calls are 32 in the whole run (sid 0x06662012 fn 0x110 once, 5-8 ms at boot; sid 0x66662012 fn 0x100, 0x200 and 0x210 once
each; fn 0x600 28 times at 4 us): ~7 ms in total, so only nowait calls matter. Waiting for the IOP lock was 67 ms of 21.6 s: the cost is
interpretation (IOP thread profile, gameplay: `executeInstruction` 27 %, SPU2 mix 13 %, `IopMemory::read32` 13 %, `runCpu` 13 %,
`IopImportRegistry::decode` 6 %, `takeDmaStart` 5 %). The sound RPC also advances IOP time by ~1.2 ms per call (46 k instructions at
36.9 MHz), which is where the "IOP lead" of ~30 s per 235 s in `PS2X_IOP_THREAD_STATS` comes from (patches 0013/0015). Whole run:
24.9 s of the EE thread in `SifCallRpc`, 22.0 s of it inside `handleRpc`.

**After** (nowait calls to emulated servers queued to the IOP thread): EE-thread time in `SifCallRpc` 0.32 s per 235 s (8 us per
call). Without the `selectRpcAbi` change (it took the IOP lock on every call, and the IOP thread now holds it for ~1 ms per sound
command) the EE waited 3.2 s in the lock, 1 233 waits > 1 ms. EE-side IOP lock acquisitions per run: 80 584 -> 75. Offloaded calls wait
0.7 ms on average in the queue (max 40 ms, on a machine with 10 other game instances). IOP thread busy 7-10 % -> 17-19 % of wall.

**Frame cost, same-time pairs.** The machine was shared with 4-12 other game instances (50-85 % CPU load) for every run, so absolute
rates moved between 35 and 65 fps and only runs made at the same time compare. Two copies of one binary (offload default, and
`PS2X_IOP_RPC_ASYNC=0`) started together, roles swapped between pairs; CPU time per frame of the game process's threads between t=150 s
and t=200 s (`Process.Threads`, divided by the frames of that window; the EE thread is `GameThread`):

| pair | load | EE thread offload / inline | IOP thread offload / inline | fps offload / inline |
|---|---|---|---|---|
| 1 | 57 % | 11.18 / 11.67 ms | 3.04 / 1.93 ms | 59.1 / 60.0 |
| 2 | 64 % | 10.63 / 12.29 ms | 2.82 / 1.89 ms | 61.9 / 59.7 |
| 3 | 60 % | 12.29 / 12.16 ms | 3.17 / 1.84 ms | 54.5 / 57.5 |
| 4 | 85 % | 12.91 / 14.99 ms | 4.22 / 3.28 ms | 36.9 / 34.2 |

(Pairs 1 and 2 were logged before thread names were captured: their EE column is the busiest thread of the process, which is the game thread in the pairs with names; their IOP column is the fourth busiest.) The IOP thread gains 0.9-1.3 ms per frame,
the work that left the EE thread; the EE thread's own difference is -0.5, -1.7, +0.1, -2.1 ms (mean -1.0, noise about +-1). The frame
rate does not change measurably: at ~60 fps the frames are quantised by the 120 Hz vblank, and this machine state never let the EE
thread reach 8.3 ms per frame. Frame rates of the older interleaved runs, for the record (the baseline binary was built before other
agents' changes to `ps2_memory.cpp`/`ps2_runtime.cpp` that the later builds include, so these mix their work in and are not a clean A/B):
baseline binary 44.5 / 38.1 / 44.0, 45.0 / 48.1 / 59.3, 59.7 / 64.3 / 57.4 (median 48.1); offload builds 51.0 / 20.2 / 55.1,
42.3 / 61.2 / 65.4, 63.6 / 58.3 / 60.9 (median 58.3); the newest binary with `PS2X_IOP_RPC_ASYNC=0`: 64.4 / 57.8 / 64.0.

**Client reuse.** `PS2X_IOP_RPC_STATS=1` also counts nowait calls that arrive while the client's previous call has not completed (a
call the inline path could never see): 0 of 27 684 calls in a full run at 80 % machine load, where the queue latency averaged 1.5 ms
and reached 112 ms. Killzone waits for a call to complete before it reuses the client.

**Checks.** 35 headless boots of 50-60 s (12 with the flag set, 12 default, 8 at `KZ_FPS=120`, 3 with movies on; one baseline with
movies on), three instances at a time on a loaded machine: `dma=` advanced in every 5 s interval of all of them, no `TIMED OUT` from the
outside-thread semaphore wait. Movies: the intro plays in both builds (work/vid_base, work/vid_async montages). Audio, `KZ_AUDIO_WAV`,
gameplay scene, first 235 s, two runs each of baseline and offload (a baseline and an offload run at the same time, twice):

| segment | baseline vs offload | baseline vs baseline |
|---|---|---|
| 0-20 s | silent in all runs | silent |
| 20-130 s (menu music) RMS / peak | within 0.1-0.3 dB / 0.0-0.7 dB; correlation 1.000 (0.997 at 80-90 s, 0.62-0.64 in the 90-100 s segment, where a menu button press lands differently) at a constant offset that differs per pair of runs | the same figures |
| 130-235 s (gameplay) | correlation 0.03-0.3 | correlation 0.04-0.27: two baseline runs differ as much |
| overall RMS | -14.8 / -14.8 (baseline), -14.6 / -14.8 dBFS (offload) | |
| silent gaps > 20 ms inside sound | 2 in each of the four runs | |
| `kz_audio` underruns | 0 (headless drops the SDL output; real-time factor 1.00-1.02x in both) | |

The gameplay segments are not deterministic between any two runs (scripted input, wall-clock events), which is why the baseline pair
correlates as badly as the baseline/offload pair; the menu segments are deterministic and match.

**Not changed / left.** (1) The 46 k-instruction sound command still costs ~1 ms of one core per call, now on the IOP thread; the
hot spots are the interpreter (`takeDmaStart`/`schedulePendingDma` after every instruction, `IopMemory::read32` not inlined, import
decode per instruction) and the SPU2 register hooks; speeding those up would shrink the IOP thread's share (not attempted, not
measured). (2) Blocking calls (32 per run) and HLE-served RPCs stay inline. (3) A game that calls a nowait RPC and reads the reply
before the completion (`sceSifCheckStatRpc`, end function, semaphore) now sees the reply up to ~1 ms late, as on hardware; no
symptom of that was seen in Killzone (menus, loading, movies, gameplay, sound).

## Guest memory/IO and DMA chain copies (2026-09-30, patch 0020)
Measured with the switches below (same binary, A/B by environment). The machine was shared with 2-3 other game instances and
builds, so absolute numbers are 10-20 % worse than a quiet machine; every comparison is a concurrent pair. "EE ms/frame" is
the EE thread's non-sleeping host time per displayed frame (`tools/scripts/perf_metric.py`: (1000 ms - pacing sleep) / fps,
`PS2X_SCHED_STATS=1`), or, for the profile pairs, EE busy samples / frames in the 40 s window.

- **What leaves the inline RAM window** (`KZ_MEMSTATS=1`: every 10 s the loads/stores that reach `PS2Runtime::Load*/Store*`,
  by class and the 40 hottest addresses). Gameplay window, 4 windows of a 235 s run: 7.9-9.0 M accesses/s (with the stats
  mutex, ~50 fps), of which **99.93-99.96 % scratchpad** (0x70000000..0x70003FFF: the guest's stack and hot working set;
  hottest 0x70003D3C, then 0x70000000..0x7000006F and 0x700002AC..0x700002BC, 8-bit loads at 0x70000220..223; the 40 hottest
  addresses are 32-47 % of the accesses, and of those 82-94 % are loads (62-73 % 32-bit, 8-32 % 8-bit) and 6-18 % 32-bit
  stores). Everything else together is < 0.06 %: DMA channel registers 0.03-0.05 % (~4 k/s),
  GS privileged registers 0.009 % (~730/s), timers 0.003 % (~250/s), VIF/DMAC control 0.001 %. So there is no hot IO register
  worth an array or a hash-free dispatch: `m_ioRegisters` lookups are ~5 k/s in total. (Accesses that take the out-of-line
  `ps2CgRd*Slow/Wr*Slow` but are not special, i.e. kseg0/uncached RAM mirrors, cannot be counted from the runtime; the
  `ps2CgWr32Slow` profile entries are the scratchpad stores.)
- **Completed-DMAC drain (the big one).** `PS2Runtime::Store32` ran `drainCompletedDmacHandlers` after every special-address
  store, and `consumeCompletedDmacCauses` took `m_completedDmacMutex` twice each time (at least 0.5 M scratchpad stores/s):
  `PS2Runtime::Store32 -> consumeCompletedDmacCauses` was **5.2 % of the EE thread's busy samples** (Mtx_lock/unlock 2.2/2.1 %,
  SRW lock acquire/release 1.6/1.7 % self). Now a global `g_ps2CompletedDmacPending` (set by `queueCompletedDmacCause`, cleared
  by the consumer, both under the mutex) lets the consumer return without the lock while nothing is queued
  (`PS2X_DMAC_DRAIN_FAST=0` = old). After: no lock entry above 0.1 % on the EE thread. A cause queued at the moment of the check is
  drained by the next store or scheduler event, as if the drain had run a moment earlier.
- **Scratchpad fast path in `PS2Runtime::Load*/Store*`.** Aligned accesses inside 0x70000000..0x70003FFF go straight to the host
  scratchpad pointer (`PS2X_SPR_FAST=0` = old path through `PS2Memory::read*/write*`: GS-priv test, scratchpad test twice,
  `translateAddress`, `loadScalar` range checks). Profile self shares (concurrent pair, same window): `PS2Memory::read32` 1.4 %
  -> 0, `translateAddress` 0.5 -> 0.1, `write32` 0.4 -> 0, `Load32` 0.6 -> 1.2, `Store32` 0.3 -> 0.7 (the work moved into
  them). Alone it is below the noise of a paired run (EE ms/frame -1.08, +0.41, +0.01 for `PS2X_SPR_FAST=0` -> on).
- **Together** (`PS2X_SPR_FAST=0 PS2X_DMAC_DRAIN_FAST=0` -> defaults), 3 concurrent pairs with the sched-stats metric: EE ms/frame
  15.46 / 13.38 / 14.53 -> 14.15 / 12.24 / 12.92 (median -1.31 ms, -9 %; 3 of 3 pairs lower), fps 55.2 / 62.4 / 57.3 ->
  55.6 / 62.0 / 57.3. 3 more pairs with the profiler on, made when the pooled-memcpy chain copy below was still switched on (EE busy ms/frame): 11.37 / 13.28 / 12.57 -> 9.96 / 11.23 / 11.86
  (median -1.41 ms), fps 64.3 / 59.3 / 57.4 -> 66.6 / 64.7 / 60.4. The EE thread sleeps more (pacing sleep 147-167 ->
  213-259 ms/s in the first set) but fps only moves when frames cross a vblank boundary, see the patch 0019 section above.
  Drain alone with the scratchpad path on (`PS2X_DMAC_DRAIN_FAST=0` only): 14.52 / 14.51 / 13.64 -> n/a (boot flake) / 14.35 /
  13.63, i.e. not resolvable on its own; the profile share above is the direct evidence for it.
- **DMA chain snapshot copy** (`KZ_CHAINSTATS=1`; per VIF1 kick in gameplay: 2.86 MB in ~3 600 segments: REF (tag id 3)
  2.06 MB in 1 807 segments of ~1.1 KB, RET (id 6) 0.73 MB in 880, CNT (id 1) 0.18 MB in 921, tag words 8 B each).
  The walk costs 0.55-0.8 ms per kick (`PS2X_VIF1_STATS=1` walk time / jobs; 3-4 % of the EE thread). What did **not** help (each measured as walk ms per job in concurrent runs, or EE ms/frame):
  a pre-sized pooled buffer written with `memcpy` and no zero-fill (0.668 -> 0.660 and 1.117 -> 1.020 ms/job; EE ms/frame
  12.18 / 11.24 / 12.96 -> 12.52 / 11.80 / 12.70), non-temporal AVX2 stores (0.72-0.87 vs 0.71-0.82), a two-phase walk
  that collects the segments and copies them with software prefetch 3 segments ahead (0.79 vs 0.67, worse), both together
  (0.90). Skipping only the copy of REF segments (measurement hack, the pooled buffer then holds an older chain) took the
  walk from 0.533 to 0.211 ms/job, so REF is ~60 % of the walk and the rest is tag parsing plus the small hot segments. The
  copy is memory bound, and none of the kept switches changes it: nothing was kept.
- **Zero-copy is not free of hazards** (`PS2X_CHAIN_VERIFY=1`: the walk registers each segment's source pointer; when the job's
  buffer is recycled the copy is compared with the live source). IPU off, boot + menus + gameplay, 235 s: 24 670 jobs, **0
  with a changed source**, kick-to-done latency avg 0.1-12 ms (max 209 ms under load). IPU on (movies), 10 013 jobs: 170 jobs
  (1.7 %) had changed sources, 3 737 REF segments / 107 MB in total, all REF (tag id 3), none in CNT/RET/REFE: the FMV player
  overwrites a frame buffer that is still referenced by an unfinished job. So handing the worker pointers into RDRAM would
  corrupt movie frames unless the movie path waits for completion, and `PS2Memory::processVIF1Data` needs one contiguous
  chain (an UNPACK command in a CNT segment and its data in the next REF segment straddle segment borders; only DIRECT
  continuation is resumable), so a scatter list also needs interpreter work. Neither was attempted.
- **Inline scratchpad window in `ps2_cg.h`: prototyped, no gain, not adopted.** After the fixes above the guest scratchpad
  accesses still go through `ps2CgRd32Slow`/`ps2CgWr32Slow`/... (noinline, `isSpecialAddress`) and `PS2Runtime::Load*/Store*`:
  6.7 % of the EE thread's busy samples in one gameplay profile (`ps2CgWr32Slow` 1.1, `Load32` 1.2, `ps2CgWr128Slow` 1.0,
  `Store32` 0.7, `ps2CgRd32Slow` 0.6, `ps2CgRd128Slow` 0.4, the rest < 0.4). A second inline window
  `(a - 0x70000000) <= 0x4000 - size` with an alignment check, reading `ps2GetScratchpadHostPtr()`, added to `PS2CG_RD/PS2CG_WR`
  and `ps2CgRd128/Wr128` (a private copy of `ps2_cg.h` placed in a copy of `generated/`, whose quoted include wins over the
  runtime's, built in its own build dir so nobody's incremental build was touched;
  `patches/ps2recomp/experimental/ps2_cg-scratchpad-window.diff`) takes those entries down to 1.7 % (`ps2CgWr32Slow` 0.7 and
  `ps2CgWr128Slow` 0.7 remain: stores that are neither in the RAM window nor scratchpad, presumably kseg0/uncached RAM
  aliases, which the runtime cannot count). But the paired EE ms/frame did not move: prototype vs current build, 3 concurrent
  pairs, 12.79 / 11.17 / 10.89 -> 12.16 / 11.78 / 11.04 (differences -0.63, +0.61, +0.15 ms, fps 64.1 / 73.6 / 71.4 -> 68.5 /
  71.1 / 71.8), and a profiled pair 11.36 -> 11.99 ms/frame (fps 69.8 vs 68.2). The extra branches in ~100 k generated
  functions cost about what the calls saved; a full rebuild for this is not worth it.
- **What is left on the EE thread in this area.** The guest-code side (about 80 % of the thread) and the scheduler/VU0 work
  owned elsewhere. The remaining memory/IO items are each below 1.5 % of the EE thread: chain walk + snapshot (3-4 %, memory
  bound, see above), the two slow-path store entries above (1.4 %), `PS2Runtime::Load32/Store32` themselves (1.2 / 0.7 %).
  Worker-side note from the same runs (`PS2X_VIF1_STATS=1`): the VIF1 worker is busy 4.2-5.2 s per 10 s at 45-55 fps
  (about 10 ms per job, of which VU1 ~8 ms), so at 120 fps the worker, not only the EE thread, is a limit.

## Guest control flow without the scheduler (2026-09-30, patch 0023)
The follow-up to "EE thread frame budget": ~1 M scheduler dispatches per second in gameplay, each ~50 ns plus a table probe and a
cold switch at the target. Counted first, then removed. All numbers: headless, `KZ_FPS=120`, scripted input, gameplay window
t=150..200 s, on a machine shared with other builds and game instances (other agents' regens and 3-5 game runs at the same time), so
absolute frame rates are 56-64 and only concurrent pairs compare.
- **Why each dispatch happened** (`PS2X_SCHED_STATS=2`, `[sched-stats] dispatch reasons`, classified in `EeScheduler::run` from how the
  previous guest entry ended; mean of the five 10 s windows, per second and per displayed frame):

  | build | dispatches/s | per frame | scheduler-originated | yield | return-resume | jump | tail jumps followed | local recursive returns |
  |---|---|---|---|---|---|---|---|---|
  | before (generated code of `build\RelWithDebInfo`, instrumented runtime) | 922 657 | 16 041 | 364 | 166 | 365 255 | 556 872 | 0 | 0 |
  | call-site trampolines + local recursive returns (no scheduler follow, no folding) | 214 483 | 3 675 | 355 | 180 | 9 486 | 204 462 | 370 593 | 125 329 |
  | + scheduler follow, no folding (`PS2X_CODEGEN_FOLD=0`; this run at 73 fps on a quiet machine) | 9 947 | 136 | 358 | 198 | 9 391 | 0 | 726 864 | 158 136 |
  | final (+ scheduler follow + fragment folding) | 8 440 | 146 | 347 | 186 | 7 907 | 0 | 1 190 | 123 720 |

  *Yield* = a checkpoint gave control back (166-186/s). *Return-resume* = the guest returned through
  `$ra` to a call's return address after everything above it had been unwound. *Jump* = any other transfer: tail jump into another
  function or `entry_*` fragment, fall-through into the next fragment, indirect jump. Top pairs before: 0x17f8e0 and 0x185e20
  returning to themselves (self-recursive functions: `jal` to the own function is a `goto`, so its `jr $ra` left the function) 25-30 k/s
  each; fragment loops 0x401720/401750/4017a4 (18 k/s each), 0x3fb108/170/184/18c (13-19 k/s each), 0x3b0c48/6c (7-14 k/s); the
  pair 0x341648 -> 0x33cd84 (jump) and 0x33cd84 -> 0x341648 / 0x341674 (return-resume), 12 k/s each: the loop of `FUN_00341548`
  (virtual calls at 0x341640 and 0x34166c; none of these pairs is left after the change);
  the flag-wait loop `FUN_0014fd90` (0x14fe28/38/54/c0/c8: 7-28 k/s each in gameplay windows, 400-700 k/s each while the game
  waits in the menu, because after one yield the function table resumed it in a nested `entry_*` copy of its tail and the loop
  never came back into the function). The 7.9 k/s return-resumes left follow the yields: 7.9 k / 186 = ~42 unwound frames resumed
  per yield (top pairs after: 0x2e67e4 -> 0x2e6944 -> 0x2ed824 -> 0x2e67e4, 1.0-1.2 k/s each).
- **Returns.** A callee that returns normally (`pc == fall`) stays on the direct path (ps2CgCall / ps2DspCall: budget decrement,
  table call, one compare); verified by reading the code and by the counts: no scheduler dispatch in either build is a normal return.
  Return-resumes only follow yields and tail jumps.
- **What changed in the generated code** (details in patches/README.md, 0023): tail jumps set `ctx->pc = target | 1` and return; the
  caller loops (`ps2DspAfterCallSlow`, and the scheduler for the outermost frame) instead of unwinding; `jr $ra` back into a
  self-recursive function's own return labels is a `goto`; 56 212 `entry_*` blocks nested inside real functions and 5 695 chained
  standalone blocks are folded into their functions (82 652 -> 20 745 functions/files, `killzone.exe` 102.2 -> 74.1 MB,
  compile CPU 6 864 -> 3 543 s); the function table maps every old block address to the owning function, whose resume switch has
  a case for it; the switch is skipped when a call enters at the function start.
- **fps and EE ms/frame** (concurrent pairs of two build dirs made from the same runtime sources: `dsp0` = the generated code of
  `generated/` before this patch, `dsp2` = after; `PS2X_SCHED_STATS=3` in both, EE ms/frame = (1000 ms - pacing sleep) / fps as in
  perf_metric.py; the machine was quieter than for the counts, 70-75 fps):

  | pair | fps before / after | EE ms/frame before / after | change |
  |---|---|---|---|
  | 1 | 73.6 / 73.8 | 10.38 / 10.06 | -0.32 |
  | 2 | 75.1 / 73.8 | 9.85 / 9.56 | -0.29 |
  | 3 | 71.6 / 71.8 | 10.83 / 10.24 | -0.59 |
  | 4 | 69.8 / 70.0 | 11.45 / 11.14 | -0.31 |

  Second set, three builds run at once for 4 rounds (before = `dsp0`, final = `dsp2`, no folding = `dsp3`, `PS2X_CODEGEN_FOLD=0`;
  all three relinked from the same runtime sources right before):

  | round | fps before / final / no folding | EE ms/frame before / final / no folding |
  |---|---|---|
  | 1 | 69.3 / 70.3 / 71.2 | 11.01 / 10.68 / 10.61 |
  | 2 | 66.5 / 66.9 / 64.9 | 11.96 / 11.28 / 11.93 |
  | 3 | 65.7 / 67.9 / 66.7 | 12.04 / 11.28 / 11.29 |
  | 4 | 68.8 / 70.7 / 72.1 | 11.81 / 11.23 / 10.92 |

  Median EE ms/frame 11.89 / 11.26 / 11.11 (final -0.63 ms = -5 %). Folding is not measurably faster than the trampolines alone
  (the rounds differ by more than the two builds do, +-0.3 ms); what it removes is the 0.73 M followed tail jumps per second, and it is kept
  on for the code size (-27 % exe), compile time (-48 % CPU) and because a resume can no longer land in a fragment that loops
  through the scheduler. Over all 8 comparisons of the final build with the old code: -0.32 / -0.29 / -0.59 / -0.31 (first set) and
  -0.33 / -0.68 / -0.76 / -0.58 ms (second), median -0.45 ms (-4 %); fps moves by 0-2 (the frame rate steps with the vblank).

  First set: median fps 72.6 -> 72.8 (no change: the frame list is kicked at a vblank, so the frame rate only moves when a frame crosses a
  vblank boundary; docs/findings.md "EE thread frame budget"), EE ms/frame median -0.31 ms, mean -0.38 ms (-3 %). Profile of the EE
  thread without stats (KZ_PROFILE=160,40, one pair of runs): `EeScheduler::run` self 3.6 % -> 0.1 %. What is left in the profile is
  spread thinly (no guest function above ~1.2 %) plus the memory paths outside the RAM window (`ps2CgWr32Slow`, `ps2CgRd32Slow`,
  `ps2CgWr128Slow`, `PS2Runtime::Load32/Store32`, `writeIORegister`, ~5-6 % of the samples; patch 0020 works on those) and `kzvu0Call` /
  `Vu0Execute` / `mVUexecute<0>` (~5 %). 30 % of the EE thread's samples in the earlier profile were the vblank pacing sleep.
  Not used: a first set of pairs against `build\RelWithDebInfo` with `PS2X_SCHED_STATS=1` showed -0.67 ms median; that binary predates
  patches 0020 and 0021 and `=1` costs ~40 ns per dispatch (host clock read + rdtsc), ~4.5 % of the EE thread in the baseline's
  profile (`RtlQueryPerformanceCounter` 2.9 %, `schedStatsReport` 1.5 %) and nothing in the new build, so it flattered the patch.
  `PS2X_SCHED_STATS=3` (pacing sleep only) exists for this reason. Getting under the 8.33 ms vblank period needs the guest code
  itself to get ~20-25 % cheaper (EE 10-11 ms here) and the VIF1 worker to fit as well; dispatch removal is worth 0.3-0.6 ms.
- **Correctness evidence.**
  - `PS2X_CODEGEN_DSP=0 PS2X_CODEGEN_FOLD=0` regenerates the patch-0017 output byte for byte (82 652 files, 0 differences).
  - `tools/scripts/verify_resume_table.py` on the folded tree: 144 623 table entries, the same addresses as before (0 lost, 0 new), 123 881
    of them inside a function that starts elsewhere, every one has a resume `case` in that function, no `goto` without its label.
    (A first version lost 58 code-pointer entries that had been registered under removed blocks; found by diffing the tables.)
  - Headless gameplay runs of the final build (one `=2` run, 7 runs of the pairs, 2 profiled runs): menu -> profile -> level -> difficulty -> character -> mission,
    HUD, weapon, explosions, mission-failed screen at the end, in the same order and looking like the baseline (frames compared side by
    side: work/dsp_c_hist vs work/dsp_b0_hist); no `guest-branch` / `sched-trace` / `[error]` line in any run, `missing tail targets=0`
    in all 23 windows of every run.
  - The `PS2X_CODEGEN_FOLD=0` variant (`dsp3`) passed the same checks in its `=2` run and the 4 rounds above (no error line, no missing tail target).
  - With `KZ_IPU` unset (movies): Guerrilla logo, intro cinematic, menu with the movie background, mission (work/dspChk_ipu).
  - `PS2X_STRESS_YIELD=97` (a checkpoint yield every 97th check, unwinding and resuming the whole stack at every kind of label):
    the final build reaches the main menu in 9 of 9 boots (3 in pairs with the baseline, run to t=100 s: also the campaign submenu
    after the scripted Start press; 6 as six concurrent boots next to 6 baseline boots, run to t=75 s), the baseline in 9 of 9. A tenth boot of the final build, the very
    first stress run (started together with a regen and a movie run), showed the known `[IOP] module arena exhausted` MCSERV.IRX loop
    (276 079 lines, stuck at `sceSifLoadModule`; "Boot flake" above: 1 in 30-40 boots under load, always with several instances
    booting); it is IOP module loading, but 1 of 10 against 0 of 9 is too few runs to rule out a link to this change.

## VIF1 worker hot path (2026-09-30, patch 0022)

The VIF1/VU1/GIF worker needed 9-10 ms per frame (one job per frame) against an 8.33 ms vblank period at 120 Hz. What the game feeds it
per frame, from `PS2X_DMA_STATS=1` (52-58 frames/s under load, 500-550 chains per 10 s): ~4000 XGKICK packets (average 1.2 KB, ~5 MB),
~415 DIRECT (1.75 MB of PATH2), ~6200 UNPACK, ~900 MSCAL, ~80 VU1 code changes (MPG: 4000-5000 a second). UNPACK formats by count: V4-32 45 %,
V2-16 16 %, V3-8 9 %, S-16 and S-8 8 % each, V4-8 7 %, V3-16 and V4-5 5 % each, V4-16 4 %; 15 % are masked, none is in fill mode (WL <= CL),
and STMOD is never written (mode 0 always).

**Method.** `tools/scripts/vif_run.ps1` / `vif_pairs.ps1` / `vif_metric.py` (new): the task's scene with `PS2X_VIF1_STATS=1`, concurrent pairs of
the same binary with env switches, run from a snapshot dir (`mkrun.ps1`; the build can relink meanwhile: the exe is linked with
`/PDBALTPATH:%_PDB%`, set as `-DCMAKE_EXE_LINKER_FLAGS` in the `build\pd` cache, so a run does not hold the build's PDB). Metric: worker ms per
job (= per frame) over t=150..200 s, next to the vif-counter fps. The worker time is what `[vif1-thread]` reports; it includes waits for
the GS thread (ring space, frame throttle). `KZ_VU_STATS=1` was itself expensive (it walked all 2048 program lists on every MSCAL: 12 % of the
worker; now it only reads two counters on the hot path and walks the lists when it prints), so the numbers below are from runs without it
unless stated.

**Where the time went** (worker profile, `KZ_PROFILE=160,40`; JIT frames break the stack walk, so inclusive shares are approximate).
- XGKICK -> GS hand-off: a packet was copied VU memory -> kzvu buffer (`std::vector::insert`) -> arbiter queue (heap vector) -> kzgs ring:
  three copies, an allocation and a free, a sort pass and an atomic notify (`WakeByAddress`) per packet; ~18 % of the worker.
- **microVU "compile" was not compilation.** `mVUcompileJIT<1>` (17 % inclusive, 12.7 % self) is the routine that JIT code calls for every
  JR/JALR. microVU treats an indirect jump target as a new program start, so after each micro-memory change every program entry (an MSCAL or a JR
  target) searches its program list again, comparing every range each candidate was compiled from with the current micro memory. Counted with a
  diagnostic build (memcmp wrapper): 15 M `memcmp` calls per second, 17 bytes on average, ~270 TSC cycles each (cache misses on the candidates'
  16 KB code copies), 3.7-4.1 G TSC cycles per 10 s. That stayed the same from t=150 to t=220 s while the code emitted fell from 4.5 MB to
  0.1-0.4 MB per 10 s. New programs per 10 s, t=130..220: 311, 814, 698, 272, 239, 66, 55, 14, 314, 610: the level-start burst decays
  within ~60 s to 15-60 per 10 s, and new content (t=210-220 here) starts another. Real compilation is a burst cost, the search was the steady
  one. (This corrects the "first-use compile cost" reading under "VU1 recompiles" above; the counts there are right.)
  Two things that did not help: an inline SSE range compare instead of the CRT `memcmp` (same TSC per call, cache-miss bound; worker
  14.0 / 14.5 -> 14.3 / 14.8 ms in two pairs), and treating JR/JALR as part of the same program (`doJumpAsSameProgram`; memcmp calls
  unchanged, worker 14.05 -> 14.64 ms). Both were removed again.
- `vif1UnpackPcsx2` 6 % (the masked UNPACKs went through the per-element loop), `forwardVif1DirectData` 4.6 %, `processVIF1Data` itself ~2 %.

**Changes and measurements.** Same binary, env switches, worker ms per job, concurrent pairs, t=150..200 s; the machine is shared, the first
pair of each series was the quietest.
- Hand-off (`PS2X_GIF_DIRECT`, `KZVU_XGKICK_INPLACE`, `KZGS_NOTIFY_ALWAYS`; plus a cheaper `packetWritesGsEvent` and plain load+store
  counters in `onGifPacket`, no switch): 3 pairs 14.01 / 11.80 / 14.12 -> 12.78 / 10.38 / 12.23 ms (median -13 %), fps 54.9 / 58.7 / 55.6 ->
  57.9 / 59.8 / 57.9. A packet is now copied once (VU memory -> kzgs ring).
- Code-state memo (`KZVU_STATE_MEMO`): first version (512 states, full content compare) 2 pairs 12.19 / 12.85 -> 9.74 / 11.98 ms (-20 %, -7 %).
  Final version (unbounded, 128-bit content key): `memcmp` calls 15 M -> 1.3-2.6 M per 10 s; 85-90 % of the ~50 k code changes per 10 s find
  their table, 3-10 % are new contents, the rest have been compiled into since (the saved table is dropped). Memo cost 0.45 G TSC cycles per
  10 s (1.2 G before the AES hash and the single pass over the 2048 slots).
- Masked UNPACK SSE path (`PS2X_VIF_UNPACK_MASKED_FAST`): 2 pairs (a third one was lost to a rebuild) 8.15 / 9.26 -> 7.98 / 9.03 ms (-2 %).
- **All together** (`PS2X_GIF_DIRECT=0 KZVU_XGKICK_INPLACE=0 KZGS_NOTIFY_ALWAYS=1 KZVU_STATE_MEMO=0 PS2X_VIF_UNPACK_MASKED_FAST=0` against the
  defaults, series `g1`, 3 concurrent pairs): worker 7.82 / 12.86 / 10.07 -> 6.20 / 9.82 / 7.74 ms per frame (-21 %, -24 %, -23 %; median
  10.07 -> 7.74), fps 68.4 / 58.1 / 65.9 -> 70.8 / 59.0 / 67.7 (median 65.9 -> 67.7). Without the stats (`g2`, 2 pairs) fps 67.4 / 72.9 ->
  72.5 / 70.4: not resolvable, the run-to-run spread is +-10 %. One quiet run of the final build (`p3`): 6.29 ms worker per frame, 74.8 fps.

**What limits fps now.** The worker is no longer the limiter (6.2-7.7 ms per frame, 45 % busy at 75 fps). Threads in the `p3` profile (t=160..200,
74.8 fps): EE thread 65 % busy (~8.7 ms per frame); GS thread (PCSX2's hardware renderer: `GSState::GIFPackedRegHandler*`, `GSRendererHW::Draw`, D3D11)
46.7 % busy (~6.2 ms per frame; 57.6 % at 69 fps in another run, ~8.4 ms); worker 45 %. 120 fps needs all three under 8.33 ms. The EE thread
is over it, and the GS thread is close. The worker's remaining time is thin: `mVUcompileJIT<1>` self 9 % of its busy samples (~0.6 ms per frame:
the JR/JALR call and its jump-cache lookups), `Push` 6.6 % (~0.4 ms, the ring memcpy of ~5 MB per frame), the spin-wait between jobs 6 %,
memo 2.7 %, UNPACK/DIRECT ~4 %, and the VU1 code itself as many unresolved JIT addresses of at most ~1.5 % each.

**Correctness evidence.** One 235 s run with `KZVU_XGKICK_CHECK=1 KZVU_STATE_MEMO_VERIFY=1 PS2X_VIF_UNPACK_CHECK=4 PS2X_GIF_SCAN_CHECK=1`
(`chk1`): 23.0 M XGKICK transfers compared with a second `Gif_Tag` walk (packet boundaries and unfinished tail), 0 differences; 9.6 M UNPACKs
run both ways (masked SSE path included; VU1 memory and VIF registers compared), 0 differences; every saved and every restored memo entry
(~500 k code changes) checked with microVU's own range comparison against the content it belongs to, 0 bad (also in an earlier 235 s run);
the fast A+D scan against the plain loop on every packet, 0 differences (and 3 M random packets in a standalone test, 945 k of them with a
SIGNAL/FINISH/LABEL write, 0 differences). GIF ordering: the direct emit only happens when the arbiter queue is empty and the packet is
PATH1/PATH2. There the drain comparator puts it first among anything submitted before the next drain (a PATH3 IMAGE never sorts ahead of PATH1, and a
PATH2 is drained at its own submit), so the emitted order is the old one; with a PATH3 packet queued (job start) everything queues as before. kzvu
tests: 1776 checks (575 + the new memo test), 0 failures, also with `KZVU_STATE_MEMO=0` and `KZVU_STATE_MEMO_VERIFY=1`; kzvu0_test 509 checks.
Frames of the final build (`ipu1` with `KZ_IPU` unset, `v1_b1`, `chk1`): intro movie, menus with the movie backgrounds, loading, level, weapon,
HUD, explosions, death screen, as in the old build. A byte comparison of the GIF stream between old and new switches (`KZ_GS_HASH=1`, 130 s)
is not possible: the game's timing makes almost every frame differ between any two runs (308 of 15598 frame hashes equal at the same frame
number, 597 of ~14.8 k distinct hashes shared).
Runs note: both runs of one pair and one run of another died when a build re-staged `build\pd\resources` under them (their run dirs used a junction
to it); `mkrun.ps1` copies it now.

## clang-cl build (2026-09-30)

Opt-in second toolchain: LLVM 23.1.2 `clang-cl` (`D:\LLVM\bin`) with `lld-link`, same sources, same options. MSVC stays the default (`tools\scripts\build.bat`).
`tools\scripts\build_clang.ps1 [-Name clang] [-Cmake '<extra cmake args>']` configures and builds `build\<Name>`; it reuses `build\RelWithDebInfo`'s FFmpeg prefix,
shader cache and FetchContent sources, so nothing is downloaded and nothing is written to C:.

**What had to change** (no behaviour change, no `generated/` or `ext/PS2Recomp` edit, so there is no `patches/ps2recomp/0024-*.patch`):
- `CMakeLists.txt`: a `KZ_CLANG_CL` branch of the RelWithDebInfo flags: `/Zi /O2 /Oi /Gy /Gw /GS- /clang:-march=x86-64-v3 /clang:-ffp-contract=off /DNDEBUG`
  (`/O2` is `-O3` in clang-cl; no `/Ob3`, no `/Qspectre-`, no `/arch:AVX2` because `-march=x86-64-v3` covers it). **Never `/fp:fast`.** `-ffp-contract=off` is
  explicit because clang-cl defaults to `-ffp-contract=on`, which would fuse `a*b+c` into FMA (x86-64-v3 has it) and change PS2 float results. The kz* libraries
  keep their own `/fp:contract /arch:AVX2` (PCSX2 code, as under MSVC). Checked in the object code: 0 `vfmadd*` in 8 sampled generated unity objects
  (hundreds of scalar `vmulss/vaddss/vsubss` in them); every compile line of the build carries `-ffp-contract=off`.
- `ext/kzvu/shim/unity/KzvuMicroVU.cpp`: `memoHash` uses `_mm_aesenc_si128`; MSVC accepts it anywhere, clang needs the target feature, so the function has
  `__attribute__((target("aes")))` under `__clang__`. That was the **only** compile error in the whole tree (runtime, IOP, kzgs, kzvu, kzipu, kzspu2, src/, 20 743
  generated files, PCH + unity as configured). The `(__m128i)(x)` casts in the generated code and macros compile as they are; the MSVC-only guards (`if(MSVC)`)
  are true for clang-cl. Warnings only: `-Wdeprecated-declarations` (fopen/strerror), the same as MSVC's C4996.
- Aliasing: **`-fno-strict-aliasing` is not needed on the command line, because clang-cl already implies it.** It passes `-relaxed-aliasing` and `-fwrapv` by default (MSVC
  compatibility), so the typed `*(uint32_t*)(rdram+a)` / `*(uint64_t*)` guest memory accesses of `ps2_cg.h` (a 32-bit store followed by a 64-bit load of the same guest address
  would be a TBAA violation) keep MSVC's semantics. Strict aliasing was not tried; it would need `may_alias` access types first.
- Opt-in extras in `CMakeLists.txt`: `-DKZ_CLANG_LTO=thin|full` (also pass `-DCMAKE_AR=<llvm>/bin/llvm-lib.exe`, MSVC's `lib.exe` does not index bitcode),
  `-DKZ_CLANG_PGO_GEN=ON` (instrumented; `src/kz_main.cpp` calls `__llvm_profile_write_file()` before its `std::_Exit`, which skips the runtime's atexit writer; set
  `LLVM_PROFILE_FILE=<dir>\kz_%p.profraw`, then `llvm-profdata merge -output=kz.profdata *.profraw`), `-DKZ_CLANG_PGO_USE=<file.profdata>`.
- `tools/scripts/vif_run.ps1` bug found on the way: it set `KZ_PROFILE` to the empty string when not profiling, and an empty variable still counts as set (kz_main.cpp
  `getenv("KZ_PROFILE")`), so every such run had the 20 s sampling profiler running from t=0. It now removes the variable. That profiler is what crashed twice (below).
  `perf_run.ps1` has the same line and was left alone.

**Build cost.** Clang, all 532 objects (generated code in 163 unity batches, PCH), `-j12`: 100 s wall, 1574 CPU-s (MSVC: "compile CPU 3 543 s" for the generated code alone,
~15 min, see "Guest control flow without the scheduler"). The thin-LTO and PGO+LTO builds take the same wall time (99 / 97 s), the code generation moves into the link. Two builds
of the same tree have the same size to the byte (87 265 792). `killzone.exe`: MSVC 74.1 MB, clang 87.3 MB, +LTO 82.9, +PGO 81.2, +PGO+LTO 79.6.

**Correctness** (all runs headless, `KZ_FPS=120`, scripted input as in the perf runs):
- Boot, menus, intro movie (`KZ_IPU` unset, 150 s, frames every 5 s: Sony/Guerrilla logos, the FMV; `f_ipu_clang`, `f_ipu_msvc`, `f2_ipu_pgolto`), level, weapon,
  HUD, explosions, "mission objective" text, death and FAILED screen: the same scenes as the MSVC build, in every build variant (frame montages; not byte comparable, the timing
  differs between any two runs).
- `PS2X_STRESS_YIELD=97` boots to the menu (clang 120 s, PGO+LTO 100 s) with `KZ_CRASH_TRACE=1`, no crash.
- `KZ_VU0_VERIFY=8 PS2X_LAZY_TIMERS=verify`, 235 s gameplay: clang 5.11 M VU0 calls, PGO 5.33 M, LTO 5.72 M, PGO+LTO 5.45 M checked, **0 mismatches** each; timer shadow 80-93 k
  comparisons, 0 mismatches, 214 M early returns checked each. (Both paths of the VU0 check are clang-compiled: this shows they agree, not that they equal MSVC's results.)
- No `missing` / `exception` / `no function` line in the stderr of any of the runs listed here (`unhandled import cdvdman:78` is in the MSVC log too).
- Tests built with clang-cl in `build\clang_tests` (`-DKZVU_BUILD_TEST=ON -DKZGS_BUILD_TEST=ON -DKZIPU_BUILD_TEST=ON -DKZSPU2_BUILD_TEST=ON`): kzvu_test 1776 checks, 0 failures (as in patch 0022);
  kzvu0_test 509 checks, 0 failures (103 captures, JIT != in-game 0, JIT != PCSX2 interpreter 0); kzgs_test PASS (0 failures); kzipu_test PASS (min 55.6 dB PSNR); kzspu2_test PASS.
- **A crash, but not in the game:** 2 of 44 runs with the sampling profiler on (the `vif_run.ps1` bug above) died with 0xC0000005 after 10-35 s. The one with `KZ_CRASH_TRACE=1` died in
  the `kz_profiler.cpp` thread inside `RtlVirtualUnwind` (unwinding another thread's stack), the other had no trace. 0 crashes in the ~50 runs without the profiler (20 x 30 s boots, ~30 x 235 s,
  verify and stress runs). Whether an MSVC build dies the same way under this profiler was not measured. The profiler (`KZ_PROFILE`) is a diagnostic, not part of the game.

**Speed.** `PS2X_SCHED_STATS=3`, the task's input script, `KZ_IPU=off`, `KZ_FPS=120`, `run_headless.ps1`, 235 s, fps = (vif counter at t=200 - at t=150) / 50, EE ms/frame =
(1000 - pacing sleep) / fps as in `perf_metric.py`, no other killzone.exe running. The machine's speed drifts a lot between hours (the MSVC build measured 7.4-8.4 ms/frame in the first
set and 5.4-5.8 ms in the second), so only runs interleaved in one series are comparable:

| series | build | fps (runs) | EE ms/frame (runs) | median fps | median EE ms |
|---|---|---|---|---|---|
| A: 3 alternating pairs | MSVC | 73.4 / 76.4 / 78.4 | 8.38 / 7.60 / 7.42 | 76.4 | 7.60 |
| | clang | 77.1 / 73.6 / 73.3 | 6.54 / 7.05 / 7.11 | 73.6 | 7.05 |
| B: 4 rounds x 5 builds, order rotated each round | MSVC | 82.8 / 78.0 / 77.7 / 77.7 | 5.41 / 5.82 / 5.80 / 5.78 | 77.8 | 5.79 |
| | clang | 79.2 / 79.4 / 77.7 / 77.3 | 5.06 / 5.03 / 5.09 / 5.08 | 78.5 | 5.07 |
| | clang + ThinLTO | 80.6 / 82.1 / 79.0 / 82.8 | 4.90 / 4.74 / 5.09 / 4.67 | 81.3 | 4.82 |
| | clang + PGO | 86.0 / 80.3 / 80.1 / 80.0 | 4.22 / 4.52 / 4.67 / 4.58 | 80.2 | 4.55 |
| | clang + PGO + ThinLTO | 80.3 / 77.5 / 77.8 / 77.7 | 4.53 / 4.68 / 4.72 / 4.61 | 77.8 | 4.64 |

Per-round difference to the MSVC run of the same round, EE ms/frame: clang -0.35 / -0.79 / -0.71 / -0.70 (median -0.71 ms, -12 %); ThinLTO -0.51 / -1.08 / -0.71 / -1.11 (-0.90, -16 %);
PGO -1.19 / -1.30 / -1.13 / -1.20 (-1.20, -21 %); PGO+LTO -0.88 / -1.14 / -1.08 / -1.17 (-1.11, -19 %). The PGO profile was trained on this same scenario (one 250 s run of the
instrumented build), so its number is the best case for this scene.
- **fps did not move measurably** (medians 77.8 MSVC; 78.5 / 81.3 / 80.2 / 77.8 for the four clang builds; the spread inside one build is 3-6 fps): at 5-6 ms per frame the EE thread is
  no longer the limit, the pacing sleep is 55-64 % of the second. The VIF1 worker (`PS2X_VIF1_STATS=1`, `vif_run.ps1`, 3 rounds, worker ms per frame): MSVC 5.53 / 5.51 / 5.46,
  clang 5.23 / 5.30 / 5.47, PGO 5.35 / 5.45 / 5.27 (medians 5.51 / 5.30 / 5.35, -3 %): it is mostly microVU-generated code, which no host compiler changes. fps in those runs: 77.1 / 79.9 / 78.8 MSVC,
  78.0 / 82.9 / 79.8 clang, 82.4 / 78.2 / 82.2 PGO. So the toolchain shaves the EE part (guest code plus runtime), which any later register-locals/fiber work builds on, but
  120 fps also needs the worker, the GS thread and the frame-list hand-off ("What limits fps now" above) under 8.33 ms.

## Frame pipeline timeline (2026-09-30, patch 0025)

Question: gameplay at `KZ_FPS=120` ran ~80 fps although every stage fits in one 8.33 ms vblank (EE ~5.5 ms, VIF1/VU1/GIF worker ~4.5 ms,
GS thread ~3.3 ms), i.e. frames took ~1.5 vblanks. Answer: nothing in the EE -> D1 -> worker -> GS hand-off serialised them. The EE
thread's *estimated cycle clock* did (details and the fix below): 81.8 -> 118.1 fps (medians of 3 interleaved runs each).

**Recorder.** `KZ_TIMELINE=<file.csv>` (+ `KZ_TL_START`/`KZ_TL_END`, default 160..200 s of run time) writes one CSV row per event, QPC ns
clock (`include/runtime/ps2_timeline.h`); `tools/scripts/tl_analyze.py <csv> [--frames N [K] | --dump T0_MS D_MS]` makes the tables below.
The event list is in patches/README.md (0025). Runs: the task scene (`KZ_FPS=120 KZ_IPU=off`, scripted input), 235 s, build `build\tl`
(MSVC, RelWithDebInfo), snapshot with `mkrun.ps1`; `tl_run.ps1`, `tl_ab.ps1`, `tl_boots.ps1` drive them. The recorder costs nothing
measurable (81.4 fps in the first baseline run with it on, 79.0-80.8 fps in the A/B runs without it).

**What the game does per frame** (from the generated code, confirmed by the hooks): the EE builds a frame list; `FUN_001759c0` spins until
D1_CHCR.STR is clear (`FUN_00175948(0, 0x10009000, 0x100)`) and then sets the byte `0x55A795` (previous kick finished, the next may start);
the vblank handler `FUN_00152018` -> `FUN_00151fc8` kicks only when that byte is set *and* the next buffer's state byte
(`0x55A6E8 + (cur^1)`) is 2 (list finished); the kick `FUN_00150090` clears the byte, flips the buffer index and stores CHCR=0x145 (when D1 is
still busy it calls an object method with a message first, then kicks anyway); `FUN_001759f8` waits, with a timeout, until the byte is set.
So a frame list can only be kicked at a vblank.

**Baseline timeline** (`work/tl2.csv`, window t=160..200 s, 81.4 fps by the vif counter, 82.1 D1 kicks/s; 4800 vblanks = 120.0/s):

| candidate | measured | verdict |
|---|---|---|
| D1 completion reaches the EE late | kick -> worker job start 0.01 ms (median); job 4.49 ms (p90 5.48, max 11.5); kick -> completion posted 4.54 ms; posted -> applied on the EE 0.01 ms (max 0.27); no busy read of D1/D2 CHCR at all (0 `CHCR_BUSY` events), so the game's D1-idle spin never waits; completion later than one vblank after its kick in 3 of the 1432 frames that took >= 2 vblanks | not it |
| game waits for the previous frame's DMA | the line above: `FUN_001759c0` finds STR clear on its first read | not it |
| the kick only happens at a vblank | kick check at 4800 vblanks, (flag, next-buffer state): (1,2) 3208 = kick, **(1,0) 1554 = D1 free but the frame list not finished**, (0,*) 38 | the symptom: the EE's list is late for 32 % of the vblanks, see below |
| kzgs `maxQueuedFrames=2` | frames queued after each push: always 1; frame-throttle waits 0; ring-full waits 0 | not it |
| barriers | 2757 waits, 421 ms = 1.05 % of the window, all "VIF register write" in the vblank handler waiting for the 0.005 ms GS-half job | not it |
| present-skip logic | 4710 of 4800 vblanks present (the game flips DISPFB every vblank, so the registers differ); `GSvsync` 0.13 ms | not it |
| worker / GS capacity | worker busy 36.8 % (DMA jobs), GS thread busy 39.4 % = 3.3 ms per vblank | fits |
| EE thread | asleep in `processDueDeadlines` (pacing) 55.2 % of the window; idle waits (no runnable guest thread, i.e. blocked on a semaphore / event flag) 0.0 % | see below |

**Per-frame breakdown** (kick to kick; the game kicks once per frame, 0.38 ms after its vblank; "EE awake" = frame length minus the EE's
sleeps; ms):

| class | frames | length | EE awake | EE pacing sleep | D1 done after kick | worker job |
|---|---|---|---|---|---|---|
| 1 vblank | 1850 | 8.42 | 3.78 | 4.64 | 4.28 | 4.27 |
| 2 vblanks | 1409 | 16.66 | **7.40** | **9.26** | 4.77 | 4.76 |
| 3+ vblanks | 23 | 41.01 | 22.32 | 18.69 | 11.21 | 5.03 |

In 1292 of the 1432 frames that took two or more vblanks the EE's awake time was under one vblank (8.33 ms). Twelve consecutive frames from
t=19999 ms of the window: vblanks per frame 2 2 2 1 2 2 (19: a screenshot stall, below) 2 2 2 2 2 2, length 16-18 ms, EE awake 6.8-9.4 ms, D1 done 4.4-5.3 ms after the kick.

**Critical path** (one 2-vblank frame, `work/tl2.csv` frame 1002; ms after its kick; clock = the EE cycle clock relative to the kick):
```
0.000  D1_KICK (its vblank was 0.4 ms ago); the worker starts the job 0.005 ms later
3.473  EE goes to sleep, clock = +2 457 360 cycles: one vblank's cycles (2.46 M) are used up after 3.5 ms of host time
5.475  worker job done, completion posted; the EE is woken by the event, applies it, sleeps on
7.500  EE wakes at the host deadline of V1: VBLANK; kick check (flag 1, next buffer state 0): list not finished, no kick
7.650  another sleep, 0.9 ms (the VBlankEnd event, 0.5 ms of guest time after the vblank)
9.370  FUN_001bff10 entered, clock = +3 376 462 cycles
12.310 EE sleeps again, clock = V1 + 2.46 M: the second vblank's cycles are used up
16.444 VBLANK V2; kick check (1, 2); D1_KICK at 16.793
```
The worker (4.8 ms), its completion and the GS thread were done long before V1. The list was late because the EE thread's *guest cycle clock*
reached each vblank's cycle deadline early and the scheduler put the thread to sleep until the host deadline (`EeScheduler::processDueDeadlines`:
a scheduled event is due only when both its cycle deadline and its host deadline have passed). The cycles are an estimate charged per call /
back edge (`ps2_cg.h`) and run 2-2.4x faster than wall time here (one vblank's 2.46 M cycles in 3.5-4.2 ms of EE time). A frame whose estimated
work is over one vblank period's cycles (this one: 3.4 M; half the frames) takes two vblanks in the guest's time, however little host time it
needed, and the EE sleeps for the rest (55 % of the window). In this frame the EE was awake for ~4.5 ms up to the limiter; without the sleeps the list
would have been ready before V1 and kicked there. At 60 Hz the budget is 4.9 M cycles per vblank and every frame fits. So the critical path of
a frame is the EE's estimated cycles rounded up to whole vblanks, not a wait on D1, the worker, kzgs or the GS. This corrects "The guest cycle
estimate is not the limiter" in "EE thread frame budget": that test (a temporary `PS2X_EE_CYCLE_SCALE=0.5`) ran when the EE was the busiest
stage (~100 % busy), where halving the charge could not show.

**Fix: `PS2X_EE_CYCLE_SCALE`** (default `auto`, `=1` = old behaviour). `EeScheduler::checkpointDue` multiplies the cycles it charges by a
factor; `auto` = vblank period / 60 Hz period, at most 1 (0.5 at 120 Hz, 0.417 at 144, 0.25 at 240, 1.0 at <= 60 Hz, where nothing changes). The
game then gets the same estimated cycles per vblank as at its native 60 Hz (timers, IOP time and the iterations of guest spin loops per vblank are
what they are at 60 Hz), like the frame-timer and fade corrections in `kz_timing`; it is the EE cycle rate setting of PCSX2. The order of DMA, VIF,
GIF and GS accesses is decided by the guest program and the worker's FIFO, not by the clock, so the guest sees the same DMA/VIF/GS state; only
the amount of guest code that runs between two vblanks changes. The wall-clock floor of the clock stays (`accountCycles`).

**Measured.** fps = (vif counter at t=200 - at t=150) / 50, `tl_ab.ps1`: one instance at a time, order rotated per round, machine otherwise idle (CPU 8-12 %).

| variant | runs (fps) | median |
|---|---|---|
| `PS2X_EE_CYCLE_SCALE=1` (old), final binary (`r_tl3`) | 82.1 / 81.8 / 81.2 | **81.8** |
| default (auto = 0.5), final binary | 117.4 / 118.1 / 118.8 | **118.1** |
| `=1` (old), first set (`r_tl2`, before removing an always-on kzgs trace hook) | 79.0 / 80.8 / 79.8 | 79.8 |
| default (auto = 0.5), first set | 119.8 / 119.2 / 117.8 | 119.2 |
| 0.75, 0.6 (one run each, `r_tl2`) | 105.6, 116.6 | |

(Earlier old-clock runs of the same scene: 79.9 / 81.2 / 75.1 and 77.3 / 77.6 / 78.2, so the old clock spreads 75-82.) `KZ_FPS=240`, scale 0.25, one
run: 178.7 fps. Timeline with the fix (`work/pm_tl.csv`, 117.7 kicks/s): 4680 of 4707 frames take one vblank (the others: 6 with two kicks in
one vblank, 16 with two vblanks, 5 longer), kick check (1,0) 17 times (1554 before), EE awake 5.09 ms per frame (p90 7.2; over one vblank in 11
frames = 0.2 %), EE pacing sleep 40 %, D1 job 3.8 ms, GS thread 38.6 % busy (3.2 ms per vblank); consecutive frames are 7.3-9.3 ms long, one
vblank each. The two timelines are different runs of a non-deterministic scene (in the montages the fixed run reached the death screen earlier); the A/B table is
the comparison, the timelines explain it.

**Correctness.** 12 boots of 50 s, 3 at a time (`tl_boots.ps1`): all exit 0, `dma=` advances in every 10 s heartbeat (e.g. 1 -> 1438 -> 2930 ->
4289 -> 5610; the boots, frames, WAV and verify runs are on the build before the last trace-hook change, and 12 more boots on the final binary gave the same); the same at `KZ_FPS=60` (no scale applied: the log has no `[sched] EE cycle clock scale` line and the code path is the old
one), 144 (0.417) and 240 (0.25), 2 boots each; 2 boots of 100 s with `PS2X_STRESS_YIELD=97 KZ_CRASH_TRACE=1`; the only warning line is the
known `unhandled import cdvdman:78`. `KZ_VU0_VERIFY=8 PS2X_LAZY_TIMERS=verify`, 235 s gameplay (118 fps): 6.7 M VU0 calls checked, 101 753 timer
comparisons and 423 M early returns, 0 mismatches. Frames of the default vs the old clock (montages of every 10 s with `KZ_IPU=off`, every 5 s
with `KZ_IPU` unset): Sony / Guerrilla logos, the intro movie with its shots at the same timestamps, the menu video backgrounds, profile /
level / difficulty / character screens, the loading screen (further along at the same time with the faster clock), level, weapon, HUD, explosions
and smoke, death screen: the same, no flicker, missing geometry or new artifacts. SPU2 WAV over 130 s with `KZ_IPU` unset: 130.01 s (old) vs
130.00 s, per-10 s RMS and peaks equal (e.g. -15.5 / -15.3 dBFS, peaks 23 726 / 22 977 in both), same music (correlation 0.95 at a constant 5 ms
offset; the movie segment matches at a 383 ms start offset, correlation 0.97), `kz_audio` reports 1.000x real time. No `TIMED OUT` lines. The
game's simulation speed is unchanged: the vsync counter runs at 120.0/s in both and the frame timer counts vblank ticks; the scripted menu /
tutorial steps reach the same scenes at the same times. What the scale can change is guest code with a fixed-iteration timeout that waits for
something on host time (CD, IOP): it has less host time at 0.5. None was seen; loading screens, IOP RPC waits and the movie start passed in every run.

**What limits fps now.** At 120 Hz nothing in the pipeline: EE awake 5.1 ms, worker 3.8-4.5 ms and GS thread 3.2 ms per vblank average against
8.33 ms. The ~2 % missing to 120 are (a) the headless screenshot every 10 s (`KZ_SHOT_INTERVAL`): `saveShot` runs on the worker thread inside
`runVsync` and stalls the pipeline for 130-175 ms (four such jobs in every 40 s window, t=160/170/180/190 s; ~0.6 s = 1.5 %; it is in the old
numbers too), and (b) heavy frames, where the EE has the least headroom (awake p90 7.2 ms; 0.2 % of the frames are over one vblank). At 240 Hz
(4.17 ms vblank) the stages no longer fit: 2-vblank frames have EE awake 6.4 ms and a 5.0 ms worker job (178.7 fps). Further gains at higher rates
are the EE's guest code (clang-cl PGO took ~1.2 ms per frame off it, see "clang-cl build") and the VU1 side of the worker, not the hand-off.

## Play-test fixes: mouse yaw, button order, profile name, menu keys (2026-09-30)
User report on the packaged build: mouse look only moved vertically and felt smoothed, C fired, no way to type the profile
name, no keyboard input in menus. All four reproduced headless and fixed.
- **Mouse yaw.** `FUN_0021c550` (ApplyLook) adds the yaw delta to player+0x148, but in normal walking (entity flag +0x31)
  the body heading is rebuilt from the look *rate* and +0x148 is reset to the lag/sway offset at the end of the call, so
  a yaw added to f13 is lost. Measured: a scripted 90-degree mouse turn (`mx=1800`) left the view unchanged while `rx=1`
  turned it. Mouse yaw now goes in as a rate on the controller (ctrl+0xA8 and its copy ctrl+0x3C): rate = yaw /
  (maxYaw * dt). maxYaw is measured from every ApplyLook with a non-zero rate (f13 / (rate * dt)); 2.793 rad/s unzoomed.
  dt is the frame timer at the controller update (*(0x559178)+0x64 ticks * +0x50 s/tick), which equalled ApplyLook's
  f12 in all 1468 logged frames (1-tick, 2-tick and clamped 0.108-0.133 s hitch frames). Result: `mx=1800/-1800/900` ->
  applied 1.5705 / -1.5708 / 0.7854 rad, the view turns 90 / back / 45 degrees in the captures. Pitch stays on f14.
- **Smoothing.** ApplyLook runs a camera lag (player+0x2A8/0x2AC: the view trails the aim by up to 10 degrees and eases
  back) and idle sway (+0x170/0x174), both gated by byte player+0x16C. With keyboard/mouse the byte is cleared for the
  duration of the call (view follows the mouse 1:1); pad play keeps both.
- **C fired / Mouse1 crouched.** Runtime `fillPadStatus` wrote the DS2 pressure bytes 16..19 as L1, L2, R1, R2; the real
  order is L1, R1, L2, R2. Killzone reads every button from the pressure bytes (`FUN_001b33b8`: byte pad+0x34+id), so R1
  and L2 were swapped for keyboard *and* gamepad. `KZ_PAD_LOG=1` (hooks 0x1B33B8): scripted R1/L2/R2/L1/triangle read as
  ids 10/9/11/8/4 before, R1 -> 9 (fire) after. Runtime patch 0026. Controller map in RAM (settings+0xEC): walk -101,
  strafe 100, pitch 102, turn -103, allowlook -1, sprint 14, fire 9, secondary 11, action 6, switch 5, grenade 8,
  special 7, reload 4, stance 10, zoom mode 15, zoom 105.
- **Profile name.** The on-screen keyboard (`FUN_001b0ee0`, state obj+0x21C: 1 open, 2 confirm, 4 cancel) is preceded
  every frame by `FUN_001b0df0`, the game's own USB keyboard reader (HID 0x28 enter -> state 2, 0x29 escape -> 4, 0x2A
  backspace -> `FUN_001b18d8`, else `FUN_001b17a0(obj, ch)` add char); no USB keyboard is emulated. That function is
  hooked: while state == 1, typed keys (letters with Shift/Caps, digits, space, - _ .) go to the game's add-char /
  backspace as tail calls (a yield inside resumes in guest code), Enter/Escape set the state. Keys used for typing do
  not reach the pad (held keys are marked consumed until released). Checked on an empty card (`KZ_MC_ROOT`): No profiles
  -> Create -> Edit -> "Drako" typed -> Create profile -> saved (BASCUS-97402655FB312 written) -> level, difficulty,
  character -> gameplay.
- **Menus.** The front end navigates with the D-pad only (left stick `ly=1` did not move the Create Profile selection;
  D-pad down did). Arrow keys and WASD now also press the D-pad (bindings Version=2; older [Bindings] sections get the
  additions appended). No D-pad id is read during gameplay (`KZ_PAD_LOG`, each direction held 2 s).
- Test hooks: `KZ_MC_ROOT=<dir>` memory card folder; `KZ_INPUT_SCRIPT` items `t:type=text`, `t:textenter|textback|
  textcancel`, `b<t>:...` (dropped once the name is entered) and `v<t>:...` (seconds after the name is entered).

## Display edge artifacts and doubled menus (2026-09-30, play-test report)

Report: "a pixelly line along the top and left edge" and "menus look doubled up and a bit bugged", 16:9, auto upscale.
Both were reproduced with headless runs (hidden window of the monitor's size, `KZ_WINDOW_SIZE=2560x1440`) and traced
with `kzgs_replay` on recorded GS traces. The existing `gs_*.png` captures read the merged output at internal
resolution, which is not the presented image. `KZ_SHOT_PRESENT=1` now also writes `gp_*.png`: the frame scaled and
aspect-corrected to the window (`kzgsReadback(.., presentW, presentH)`). Both file names carry the GS frame number.
Other automation switches: `KZ_UPSCALE=n`, `KZ_RENDERER=d3d11|d3d12|vulkan`, `KZ_CROP=l,t,r,b`, `KZ_VS_EXPAND=0|1`,
`KZGS_GS_OPTS=Name=0|1,..`; `run_headless.ps1 -ExeDir -EnvSet "A=1|B=2"`.

### 1. Dotted line on the top and left edge

- **Reproduced.** Live run, 2560x1440 window (auto upscale 4x, FXAA on), mission-failed screen, `gs_049_t250s.png`
  (2048x1792): columns x=0 and x=1 differ from the background colour in 1791 and 1788 of 1792 rows (column 2 in 104),
  and rows 7..9 are garbage across the width; rows 0..6 are the black band. The garbage is multicoloured 1 px dots.
  In gameplay the same strips are a brighter 1-px rim instead of noise (the stale content happens to be similar).
  Crops: `work/fix_proof/edge_FAILED_screen_BEFORE_garbage_2560x1440.png`.
- **Geometry.** PCSX2's display circuit (`kzgs_replay --pcrtc`): `displayRect (0,2,512,450)` (the game programs DY=52), so
  the merged 512x448 output has 2 black lines on top, and game frame row 0 / column 0 sit at merged line 2 / column 0.
  At 4x the bad strip is 2 px wide (x 0..1) and 2 lines high (merged rows 8..9): half a native pixel.
- **Root cause (measured).** `halfPixelOffset=4 (Native)`, the Killzone GameIndex setting kzgs applies. Replay of a
  gameplay frame at 4x, first rows after the black band, mean level: Native `39 39 31 31 31`, Off/Normal/SpecialAggressive
  `31 31 31 31 32`; `nativeScaling` 0..4 changes nothing; at 1x there is no rim at all. So Native leaves the top/left half
  native pixel of an upscaled frame holding whatever the target held before (stale VRAM); which pass does not cover it was
  not traced to a draw. The other hpo values are not a fix: post effects (glow) move by half a pixel, which is why the
  GameIndex entry exists.
- **Fix.** Presentation crop, which is PCSX2's own `Crop` option and does not touch rendering: 4 columns each side and
  3 top / 4 bottom lines (`toKzgs`). Top 3 = the 2 black lines + game row 0; left 4 covers column 0. The total is 8 x 7,
  i.e. the 512:448 aspect, so the picture keeps its shape; a smaller crop such as 1,3,0,0 changes the aspect slightly
  and PCSX2 then letterboxes 3 px bars on a 16:9 window (seen in the replay), which would have been a new line.
  Measured on the presented image (replay, FXAA, 16:9): before the first rows are `0 0 0 0 0 0 21 37 33 31` (2560x1440,
  2x) or `0 x9 15` (3840x2160, 4x) and the left columns `63 63 63 64`/`62 63 63 63`; after `31 31 31 ...` and
  `64 64 64 ...` from pixel 0, at 2x/3x/4x (1440p) and 4x/5x/6x (2160p), no bars.
  Live before/after (gameplay): `work/fix_proof/edge_gameplay_top_left_before_after_2560x1440.png`,
  `work/fix_proof/edge_gameplay_top_left_after_3840x2160.png`.
- Cost: 1.6 % of each dimension (2 of the 7 lines were the black band); a CRT's overscan hides far more, the HUD boxes lose a few pixels.

### 2. Doubled / bugged menus

- **Reproduced.** Front-end menus (Game > Campaign/Battlefields, profile list, level/character select), live capture at 4x:
  frames where the text is doubled 1-2 lines apart, the picture is dimmer and white dashes appear, next to clean frames.
  The same frames appear at 1x, so it is not an upscaling effect. Replaying the recorded menu trace
  (`KZ_GS_TRACE`, 6000 frames) through PCSX2's software renderer gives the correct picture and is deterministic
  (0 of 41 frames differ between two runs).
- **Root cause (measured): PCSX2's D3D11 renderer on NVIDIA with vertex-shader sprite expansion.** Same trace, frames 4400-4440,
  compared with the software renderer, RTX 3080: D3D11 3 to 7 wrong frames in each of 4 runs (the wrong window moves between
  runs); D3D12 and Vulkan 0 of 41 in 2+2 runs; D3D11 on the Intel UHD 770 0 of 41. The menu's glow composite is drawn as
  SPRITE strips, the primitive type the vertex-shader expansion handles (a plausible link, not shown draw by draw). With it off, D3D11 had 0 wrong frames
  in 8 paced runs (2/4/8/16 ms between vsyncs, 16 frames each) and 3 of 3 at 20 ms, while the default D3D11 was wrong in
  7 of 8 and 3 of 3. Other switches did not help (`DisableFramebufferFetch`, `UserHacks_DisablePartialInvalidation`,
  `UserHacks_DisableRenderFixes`, `UserHacks_DisableDepthSupport`); `UserHacks_DisableSafeFeatures` did but is broader.
  At 4x with FXAA (the shipped settings) D3D11 with VS expand was wrong in 1 of 3 runs and differed from D3D12 by a mean
  1.3 (per channel, 0..255) even in the "clean" runs; with it off it equals D3D12 exactly (0.0 in 3 of 3).
  The failure only shows while the GS thread has idle time between frames (a replay that keeps it saturated, or
  captures taken every frame from frame 1, did not show it), which is why it is intermittent in play.
  Proof montage (rows: D3D11 default, D3D11 fixed, D3D12; columns: 3 frames of the same trace, 4x FXAA):
  `work/fix_proof/menu_d3d11_default_vs_fixed_vs_d3d12_4x.png`.
- **Not shown / caveats.** The mechanism inside the NVIDIA driver is not proven. Every readback changes GS timing, so a
  live capture cannot see the bug directly: after the readback was changed to a download texture (below) six live runs (1x/2x/4x D3D11, 4x D3D12, 1x D3D12,
  1x Vulkan, about 50 captures each, compared with the software renderer on the matching trace frame) showed no wrong frame, and the wrong-frame windows last only a few frames at menu
  transitions. The evidence is the paced GS trace replay, which uses the same code and GPU as the game.
  Also measured: `GSSaveSnapshotToMemory` (the old readback) allocates and recycles a pooled render target and made
  ~25 % of later live frames wrong even with VS expand off (13 of 46 captures), so the earlier screenshots were partly
  measuring their own capture. `kzgsReadback` now copies the output through a download texture (no pooled target).
- **Fix.** `toKzgs`: D3D11 renders with `vertexShaderExpand = false`. No measurable cost (300 gameplay frames at 4x replay in
  ~1.0 s with and without). D3D12/Vulkan keep it.
- Checked and unrelated: PCRTC anti-blur is active (both circuits read the same lines, `fbRect (0,0,512,448)` and
  `(0,0,512,447)`), turning deinterlacing off gives a bit-identical bad frame, FXAA is not the cause (wrong frames also without it), and the
  present-skip logic in `kz_gs.cpp` is not involved (the replay calls `kzgsVsync` for every recorded vsync and still shows it).
