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
- **The guest cycle estimate is not the limiter.** `PS2X_EE_CYCLE_SCALE=0.5` (charge half the cycles per call/back edge; a
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
- **fps and EE ms/frame** (concurrent pairs, `PS2X_SCHED_STATS=1`, EE ms/frame = (1000 ms - pacing sleep) / fps as in
  perf_metric.py; baseline = `build\RelWithDebInfo`, new = this patch):

  | pair | fps base / new | EE ms/frame base / new | change |
  |---|---|---|---|
  | 1 | 56.1 / 58.3 | 15.05 / 12.84 | -2.21 |
  | 2 | 60.9 / 61.1 | 12.75 / 12.08 | -0.67 |
  | 3 | 59.9 / 59.3 | 13.11 / 12.62 | -0.49 |
  | 4 (sampling profiler on in both) | 62.5 / 63.9 | 12.27 / 11.14 | -1.13 |

  Median of pairs 1-3: fps 59.9 -> 59.3 (no change: the frame list is kicked at a vblank, so 12-13 ms of EE work per frame is a
  2-vblank frame at 120 Hz whatever the EE does; docs/findings.md "EE thread frame budget"), EE ms/frame 13.11 -> 12.62 (-5 %, mean
  of pairs 1-3 -1.1 ms = -8 %). The profile of the EE thread (KZ_PROFILE=160,40) shows where it went: `EeScheduler::run` 6.1 % self
  before, not in the top 28 after; total busy 72.3 % -> 67.9 % of the samples. What is left in that profile is spread thinly
  (no guest function above ~1.2 %), plus the memory slow paths (`ps2CgWr32Slow`, `ps2CgRd32Slow`, `ps2CgWr128Slow`,
  `PS2Runtime::Load32/Store32`, `writeIORegister`: 4.7 % before, 6.4 % after, self time; addresses outside the 32 MB RAM window) and `kzvu0Call`/`Vu0Execute`
  (~3 %). Getting under the 8.33 ms vblank period needs the guest code itself to get ~35 % cheaper and the VIF1 worker to fit
  as well; dispatch removal alone is worth 0.5-1 ms per frame here.
- **Correctness evidence.**
  - `PS2X_CODEGEN_DSP=0 PS2X_CODEGEN_FOLD=0` regenerates the patch-0017 output byte for byte (82 652 files, 0 differences).
  - `tools/scripts/verify_resume_table.py` on the folded tree: 144 623 table entries, the same addresses as before (0 lost, 0 new), 123 881
    of them inside a function that starts elsewhere, every one has a resume `case` in that function, no `goto` without its label.
    (A first version lost 58 code-pointer entries that had been registered under removed blocks; found by diffing the tables.)
  - Headless gameplay runs of the final build (hist runs and 3 pairs): menu -> profile -> level -> difficulty -> character -> mission,
    HUD, weapon, explosions, mission-failed screen at the end, in the same order and looking like the baseline (frames compared side by
    side: work/dsp_c_hist vs work/dsp_b0_hist); no `guest-branch` / `sched-trace` / `[error]` line in any run, `missing tail targets=0`
    in all 23 windows of every run.
  - With `KZ_IPU` unset (movies): Guerrilla logo, intro cinematic, menu with the movie background, mission (work/dspChk_ipu).
  - `PS2X_STRESS_YIELD=97` (a checkpoint yield every 97th check, unwinding and resuming the whole stack at every kind of label):
    the final build reaches the main menu in 9 of 9 boots (3 in pairs with the baseline, run to t=100 s: also the campaign submenu
    after the scripted Start press; 6 as six concurrent boots next to 6 baseline boots, run to t=75 s), the baseline in 9 of 9. A tenth boot of the final build, the very
    first stress run (started together with a regen and a movie run), showed the known `[IOP] module arena exhausted` MCSERV.IRX loop
    (276 079 lines, stuck at `sceSifLoadModule`; "Boot flake" above: 1 in 30-40 boots under load, always with several instances
    booting); it is IOP module loading, but 1 of 10 against 0 of 9 is too few runs to rule out a link to this change.
