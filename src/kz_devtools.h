#pragma once
#include <cstdint>

class PS2Runtime;

// Test/debug helpers, all off unless their environment variable is set (never used by players).
//   KZ_POKE="t:addr=value[/size];..."  writes value (hex or dec, size 1/2/4, default 4) at wall-clock second t after start, once.
//            "t+:..." repeats it every vsync from t on (god mode style pins).
//   KZ_PEEK="t:addr:words;..."          prints words (hex) at second t to stderr.
//   KZ_TELEPORT="t:x,y,z;..."          moves the player controller to that position once at wall second t.
//   KZ_KILLALL="start[,period]"        sets the health of every character entity except the player to 0 from second start, every period (2 s).
//   KZ_HOOKLOG="addr,addr,..."         logs a0..a3, ra on entry and v0 on return of those guest functions (KZ_HOOKLOG_MAX lines, default 300).
void kzDevHooksInstall(PS2Runtime &runtime);
void kzDevOnVsync(uint8_t *rdram);
