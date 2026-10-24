#pragma once
#include <cstdint>

class PS2Runtime;

// Test/debug helpers, all off unless their environment variable is set (never used by players).
//   KZ_POKE="t:addr=value[/size];..."  writes value (hex or dec, size 1/2/4, default 4) at wall-clock second t after start, once.
//            "t+:..." repeats it every vsync from t on (god mode style pins).
//   KZ_PEEK="t:addr:words;..."          prints words (hex) at second t to stderr.
//   KZ_TELEPORT="t:x,y,z;..."          moves the player controller to that position once at wall second t.
//   KZ_KILLALL="start[,period]"        sets the health of every character entity except the player to 0 from second start, every period (2 s).
//   KZ_KILLALL_SYNC=1                  run KZ_KILLALL on the guest thread at the frame limiter instead of the vsync thread.
//   KZ_LISTWATCH="vt[,cnt_off,data_off]"  guest thread, from second 100: finds objects with that vtable (rescan every 3 s) and dumps the first one whose
//            pointer vector (count at +0x38, data at +0x40 by default) holds a NULL entry. KZ_PAGEWATCH=1 [KZ_PAGEWATCH_T=130] additionally makes the
//            largest such vector's buffer read-only and logs every write with the host call stack (guest function names) and the value.
//   KZ_VECHOOK="func,vecoff,ownervt"   logs calls of a guest vector function whose a0 is the vector at vecoff of an object with that vtable.
//   KZ_NULLRET="addr"                  logs every return of a guest function with v0 == 0 (e.g. 1727F8, the dlmalloc entry).
//   KZ_VECWATCH="vt[,cnt_off,data_off]" like LISTWATCH but polled from the vsync thread (racy; prefer LISTWATCH).
//   (runtime) PS2X_MISSING_DUMP=1       registers, memory behind them and stack return addresses at every call/jump to a missing target (e.g. a NULL function pointer).
//   KZ_HOOKLOG="addr,addr,..."         logs a0..a3, ra on entry and v0 on return of those guest functions (KZ_HOOKLOG_MAX lines, default 300).
void kzDevHooksInstall(PS2Runtime &runtime);
void kzDevOnVsync(uint8_t *rdram);
