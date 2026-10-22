# .weaver tools
- lv2run.ps1: headless run of level N (default 2, Vekta Evacuates) from a fresh copy of work\saves\level2 (profile "Shooterman" = unlock-all cheat); extra input script and env via -Script / -EnvSet. Gameplay at ~t=165 s.
- lv2fast.ps1: like lv2run.ps1 with a compressed menu path (gameplay at ~t=105 s; script times for the post-menu part start ~t=110).
- bootrepro.ps1: repeat short headless boots until the 'module arena exhausted' boot hang appears (KZ_HOOKLOG etc. via -EnvSet); logs in work\bootrepro_N.
- bootrepro2.ps1: same with the long time limit of the level runs, killed after -Watch seconds.
- lv2hidden.py: level N as a player runs it (real window + audio device at volume 0, private invisible desktop; log in <exe dir>\killzone.log).
- batch_test.ps1: pre-release test pass on a build (-ExeDir): intro skip, boot under forced yields, scope aim, level 2 soak, level-1 fps; prints one line per check.
- STATUS.md / CHANGELOG.md: project snapshot and per-batch changes.
- disasm.py: MIPS disassembly of SCUS_974.02 (capstone): python .weaver\disasm.py 0x16e420 [count]. Shows which float/int registers a function takes (Ghidra's signatures are often wrong).
- crlf_edit.py: edit()/edit_inline() helpers for exact text edits that keep a file's CRLF/LF endings (several src files are CRLF or mixed). Do not write backslash-n inside a bash heredoc string: the shell tool turns it into a real newline.
