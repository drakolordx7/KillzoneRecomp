#pragma once

// Starts a background thread that appends stack samples of all other threads to `path` every `intervalSeconds`.
void kzStartStackSampler(int intervalSeconds, int maxFrames, const char *path);

// KZ_PROFILE=<start s>,<duration s>: ~1 kHz statistical profile of all threads -> path (src/kz_profiler.cpp).
void kzStartProfiler(double startSeconds, double durationSeconds, const char *path);
