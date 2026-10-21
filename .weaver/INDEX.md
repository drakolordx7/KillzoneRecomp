# .weaver tools
- lv2run.ps1: headless run of level N (default 2, Vekta Evacuates) from a fresh copy of work\saves\level2 (profile "Shooterman" = unlock-all cheat); extra input script and env via -Script / -EnvSet. Gameplay at ~t=165 s.
- lv2fast.ps1: like lv2run.ps1 with a compressed menu path (gameplay at ~t=105 s; script times for the post-menu part start ~t=110).
- bootrepro.ps1: repeat short headless boots until the 'module arena exhausted' boot hang appears (KZ_HOOKLOG etc. via -EnvSet); logs in work\bootrepro_N.
- bootrepro2.ps1: same with the long time limit of the level runs, killed after -Watch seconds.
- lv2hidden.py: level N as a player runs it (real window + audio device at volume 0, private invisible desktop; log in <exe dir>\killzone.log).
