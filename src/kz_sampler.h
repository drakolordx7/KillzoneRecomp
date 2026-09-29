#pragma once

// Starts a background thread that appends stack samples of all other threads to `path` every `intervalSeconds`.
void kzStartStackSampler(int intervalSeconds, int maxFrames, const char *path);
