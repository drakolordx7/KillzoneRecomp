#include "kz_timing.h"

#include "runtime/ps2_host_gs.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace
{
    constexpr uint32_t kRamMask = 0x01FFFFFFu;
    constexpr uint32_t kSecondsPerVsync = 0x0055A6E4u; // float
    constexpr uint32_t kGameObjectPtr = 0x00559178u;   // DAT_00559178; frame timer at +0x50..+0x70
    constexpr uint32_t kTimerClampOffset = 0x70u;      // max vsyncs per simulated frame
    constexpr uint32_t kFadeLevel = 0x0055A7C8u;       // int, stepped -8/+4 per vblank by FUN_0018f0d0
    constexpr int kNativeHz = 60;

    int g_rate = kNativeHz;

    // clamp scaling
    uint32_t g_clampObserved = 0; // original (30 Hz tick) value we last scaled
    uint32_t g_clampWritten = 0;

    // fade correction
    bool g_fadeInit = false;
    int32_t g_fadeWritten = 0;
    double g_fadeShadow = 0.0;

    // KZ_TIMING_LOG=1: once per wall-clock second, print how much game time the simulation consumed (sum over game
    // frames of elapsed ticks * seconds per tick). 1.00 means the game runs at real-time speed.
    bool g_timingLog = false;
    uint32_t g_lastNow = 0;
    double g_gameSeconds = 0.0;
    uint32_t g_gameFrames = 0;
    std::chrono::steady_clock::time_point g_logStart;

    float rdf(const uint8_t *ram, uint32_t addr);

    uint32_t rd32(const uint8_t *ram, uint32_t addr)
    {
        uint32_t v;
        std::memcpy(&v, ram + (addr & kRamMask), 4);
        return v;
    }

    float rdf(const uint8_t *ram, uint32_t addr)
    {
        float v;
        std::memcpy(&v, ram + (addr & kRamMask), 4);
        return v;
    }

    void wr32(uint8_t *ram, uint32_t addr, uint32_t v)
    {
        std::memcpy(ram + (addr & kRamMask), &v, 4);
    }
}

int kzTimingInit(int fpsLimit, int displayRefreshHz)
{
    int rate = fpsLimit > 0 ? fpsLimit : (displayRefreshHz > 0 ? displayRefreshHz : kNativeHz);
    rate = std::clamp(rate, 30, 480);
    g_rate = rate;
    ps2SetVblankPeriodMicros(static_cast<uint32_t>(std::lround(1'000'000.0 / rate)));
    std::cout << "[kz] guest vblank / frame rate: " << rate << " Hz" << std::endl;
    const char *log = std::getenv("KZ_TIMING_LOG");
    g_timingLog = log && *log && *log != '0';
    g_logStart = std::chrono::steady_clock::now();
    return rate;
}

int kzTimingRate()
{
    return g_rate;
}

void kzTimingOnVsync(uint8_t *rdram)
{
    if (!rdram)
        return;

    // Seconds per vsync. The game writes 1/59.94 (NTSC) at init; keep it at 1/R.
    const float period = 1.0f / static_cast<float>(g_rate);
    uint32_t periodBits;
    std::memcpy(&periodBits, &period, 4);
    if (rd32(rdram, kSecondsPerVsync) != 0)
        wr32(rdram, kSecondsPerVsync, periodBits);

    // Frame-time clamp: originally in 30 Hz ticks; now ticks are 1/R s.
    const uint32_t obj = rd32(rdram, kGameObjectPtr);
    if (obj >= 0x00100000u && obj < 0x02000000u)
    {
        const uint32_t cur = rd32(rdram, obj + kTimerClampOffset);
        if (cur != 0 && cur != g_clampWritten)
        {
            g_clampObserved = cur;
            g_clampWritten = static_cast<uint32_t>(std::ceil(cur * (g_rate / 30.0)));
            wr32(rdram, obj + kTimerClampOffset, g_clampWritten);
        }
        if (g_timingLog)
        {
            const uint32_t now = rd32(rdram, obj + 0x68);
            if (now != g_lastNow)
            {
                g_lastNow = now;
                g_gameSeconds += rd32(rdram, obj + 0x64) * static_cast<double>(rdf(rdram, obj + 0x50));
                ++g_gameFrames;
            }
            const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_logStart).count();
            if (wall >= 1.0)
            {
                std::fprintf(stderr, "[kz] timing: game %.3f s per wall s, %u frames, clamp %u, tick %.6f s, elapsed %u, scale %.3f, spv %.6f\n",
                             g_gameSeconds / wall, g_gameFrames, rd32(rdram, obj + kTimerClampOffset),
                             rdf(rdram, obj + 0x50), rd32(rdram, obj + 0x64), rdf(rdram, obj + 0x54),
                             rdf(rdram, kSecondsPerVsync));
                g_gameSeconds = 0.0;
                g_gameFrames = 0;
                g_logStart = std::chrono::steady_clock::now();
            }
        }
    }

    // Fade: the vblank handler moved the level by its fixed step since our last write; keep only 60/R of it.
    if (g_rate != kNativeHz)
    {
        const int32_t observed = static_cast<int32_t>(rd32(rdram, kFadeLevel));
        if (!g_fadeInit)
        {
            g_fadeInit = true;
            g_fadeShadow = observed;
            g_fadeWritten = observed;
        }
        else if (observed != g_fadeWritten)
        {
            const int32_t delta = observed - g_fadeWritten;
            if (std::abs(delta) <= 8)
                g_fadeShadow = std::clamp(g_fadeShadow + delta * (static_cast<double>(kNativeHz) / g_rate), 0.0, 255.0);
            else
                g_fadeShadow = observed; // set directly by the game
            g_fadeWritten = static_cast<int32_t>(std::lround(g_fadeShadow));
            wr32(rdram, kFadeLevel, static_cast<uint32_t>(g_fadeWritten));
        }
    }
}
