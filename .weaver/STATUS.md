# Killzone PC port: status (2026-10-22, end of week)

## Done
- Public pre-releases on GitHub: v0.1.0 to v0.1.3 alpha. Packaging: tools/scripts/make_release.ps1 -Version x.y.z -From build\agentv (VC++ runtime app-local, README from tools/release/README.txt). Test pass: .weaver/batch_test.ps1; A/B: .weaver/ab_test.ps1.
- Fixed and closed: #3 sniper scope, #5 skip intro, #6 controller zoom + optional toggle sprint, #7 ultrawide (match window / 21:9 / 32:9), boot hang "[IOP] module arena exhausted".
- On master since v0.1.2: #11 controller diagonals (stick range 1.33, radial dead zone), #10 ultrawide flicker (deferred undo after a guest yield in 0x1C1960), killzone.log keeps every line on hang / kill / Alt+F4.
- Level select for tests: work/saves/level2 (profile "Shooterman" unlocks all), .weaver/lv2fast.ps1. Issue #4 reporter's save: work/saves/issue4_reporter/mc0 (run on a copy).
- GitHub text cleaned (humanizer pass); no Claude/AI credit in public text or new commits. Commit history left as is.

## Open (next steps)
- #4 level-2 hang (open, reporter active): reproduced 3 of 4 runs with the reporter's save (.weaver/i4run.ps1, i4hunt.ps1 on branch wip/2026-10-22-issues-4-9): about 15 s after a wave is killed (KZ_KILLALL health poke) the guest main thread (id 1) disappears from the thread table, DMA/VIF stop, vsync keeps ticking. Not a memory-card problem (no card write happens). Next: log guest thread exit/terminate/sleep with ra (PS2X_HEARTBEAT_THREADS=1 is on the wip submodule branch), repeat with natural kills, compare PCSX2. Checkpoint writer 0x1C54E0 (Lua OnCheckPoint), checkpoint scan 0x1C5330, vtable 0x52A350 (+0xB0 flag; KZ_CPFORCE).
- #9 hang with an external frame cap (RTSS 100 fps), open: reproduced with an emulated present cap (loading crawls/stalls: VIF1 worker blocks in kzgsVsync, EE waits in ps2Vif1Barrier). Candidate fix on branch wip/2026-10-22-issues-4-9 (kzgsVsyncTry drops a present when the GS thread is behind; KZ_GS_BLOCK=1 old path); verified in one capped run only. Next: finish the cap matrix (.weaver/capmatrix_all.py), uncapped frame-rate A/B, then merge.
- #8 non-AVX2 CPUs: deferred; only if it stays one build (runtime dispatch). Public answer: "on the radar, no promises".
- Frame-rate measurement is noisy (90-112 fps run to run, same build): needs a deterministic scene. Ultrawide untested on D3D12/Vulkan.

## Decisions that still matter
- WIP branches (not merged): wip/2026-10-01-stopped-agents (old pacing experiment), wip/2026-10-22-issues-4-9 (+ submodule wip/2026-10-22-issue4).
- build\RelWithDebInfo fails with C1041 PDB errors at -j>1; build with -j 1 (agentv for releases; agenta/agentb/agentw are helper dirs).
- No Claude/AI credit in commits, PRs, issues, releases (project CLAUDE.md). Release notes never @mention people.
- Sprint stays hold by default (original behaviour, PCSX2-verified); toggle is opt-in.
