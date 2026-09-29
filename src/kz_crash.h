#pragma once

// KZ_CRASH_TRACE=1: on an unhandled exception (access violation etc.), print the exception, the faulting address and a
// symbolized host call stack (from killzone.pdb) to stderr, plus the guest pc of the current EE context if known.
void kzCrashTraceInstall();
