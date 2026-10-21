# Killzone PC port: status (2026-10-21)

## Done
- Public pre-releases on GitHub: v0.1.0-alpha (player build of 10-01), v0.1.1-alpha (2026-10-21). Packaging: tools/scripts/make_release.ps1 -Version x.y.z -From <build dir> (VC++ runtime app-local, README from tools/release/README.txt).
- Issues fixed in v0.1.1: #3 sniper scope (aim mode 2 reticle block, src/kz_aim.cpp), #5 Skip intro movies (src/kz_movies.cpp, hooks 0x26ECC8), #6 controller zoom toggle (hold-to-aim is mouse/keyboard only), boot hang "[IOP] module arena exhausted" (sceSifLoadModule/StopModule wrappers set pc=ra).
- Level 2 (any level) reachable headless: work/saves/level2 (profile "Shooterman" = unlock all), .weaver/lv2fast.ps1.
- dist\Killzone (the owner's folder) = v0.1.1 build.

## In progress
- #6 sprint on a controller (helper agent, build\agentw).
- #7 21:9 / ultrawide (helper agent, build\agentb).

## Open
- #4 lock-up on level 2 near the second sniper: not reproduced (25+22 min level-2 soaks clean); asked the reporter for killzone.log and their disc\mc0.
- #8 non-AVX2 CPUs: deferred. Owner: only if it stays one build total (runtime dispatch), no promises.
- Frame-rate drops and stutter in heavy fights; menu jitter (old, see HANDOFF.md).

## Decisions that still matter
- Untested 2026-10-01 WIP (adaptive pacing, KZ_PACING) parked on branch wip/2026-10-01-stopped-agents (super 6884b86, submodule 3723a7d).
- build\RelWithDebInfo fails with C1041 PDB errors at -j>1 since 2026-10-21; build\agentv builds with -j 1. Releases built from build\agentv.
- Commits no longer carry the "Made by drakolord and assisted with Claude Code." line (from 2026-10-21).
- Release notes must not @mention people (GitHub lists them as contributors).
