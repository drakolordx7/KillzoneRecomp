#include "kz_audio.h"

#include "kz_config.h"
#include "kzspu2.h"

#include "ps2x/iop/iop_host_spu2.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>

namespace
{
    constexpr uint32_t kRate = 48000;
    constexpr uint32_t kTargetFrames = kRate * 40 / 1000;  // audio thread keeps ~40 ms queued
    constexpr uint32_t kHighFrames = kRate * 150 / 1000;   // above this (producer ahead of real time): skip to target

    bool g_active = false;
    PS2Runtime *g_runtime = nullptr;
    SDL_AudioStream *g_stream = nullptr;
    std::atomic<uint64_t> g_underrunFrames{0};
    std::atomic<uint64_t> g_feedCalls{0}, g_feedFrames{0}; // SDL audio callback activity (device diagnostics)
    std::atomic<uint64_t> g_skippedFrames{0};
    bool g_pace = true; // see "pacing" below
    double g_factor = 1.0;
    double g_paceAhead = 0; // last measured SPU2 lead over real time (s)

    // ---- WAV tap + stats (emulation thread) ---------------------------------------------------------------------------
    std::mutex g_wavMutex;
    FILE *g_wav = nullptr;
    uint64_t g_wavFrames = 0;
    uint64_t g_wavHeaderAt = 0;
    double g_statsInterval = 0;
    std::chrono::steady_clock::time_point g_start, g_statsLast;
    uint64_t g_statsFrames = 0;
    double g_sumSq[2] = {};
    int g_peak[2] = {};

    bool envFlag(const char *name, bool def)
    {
        const char *v = std::getenv(name);
        if (!v || !*v)
            return def;
        return !(std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0 || std::strcmp(v, "false") == 0);
    }

    void writeWavHeader(FILE *f, uint64_t frames)
    {
        const uint32_t data = static_cast<uint32_t>(std::min<uint64_t>(frames * 4u, 0xFFFFFFFFull - 36u));
        auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
        std::fseek(f, 0, SEEK_SET);
        std::fwrite("RIFF", 1, 4, f);
        u32(36u + data);
        std::fwrite("WAVEfmt ", 1, 8, f);
        u32(16);
        u16(1);
        u16(2);
        u32(kRate);
        u32(kRate * 4);
        u16(4);
        u16(16);
        std::fwrite("data", 1, 4, f);
        u32(data);
        std::fseek(f, 0, SEEK_END);
    }

    void closeWav()
    {
        std::lock_guard lock(g_wavMutex);
        if (!g_wav)
            return;
        writeWavHeader(g_wav, g_wavFrames);
        std::fclose(g_wav);
        g_wav = nullptr;
    }

    void printStats(double now)
    {
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_statsLast).count();
        g_statsLast = std::chrono::steady_clock::now();
        auto db = [](double rms) { return rms > 0 ? 20.0 * std::log10(rms / 32768.0) : -999.0; };
        const double n = g_statsFrames ? static_cast<double>(g_statsFrames) : 1.0;
        const double rmsL = std::sqrt(g_sumSq[0] / n), rmsR = std::sqrt(g_sumSq[1] / n);
        const KzSpu2Status st = kzspu2GetStatus();
        std::fprintf(stderr,
                     "[kz_audio] t=%.0fs frames=%llu (%.3fx real time) rms L/R %.0f/%.0f (%.1f/%.1f dBFS) peak %d/%d "
                     "voices %u/%u irq %u dma %u/%u dropped %llu underrun %llu skipped %llu pace %.3f device calls %llu frames %llu\n",
                     now, static_cast<unsigned long long>(g_statsFrames), secs > 0 ? g_statsFrames / (kRate * secs) : 0.0,
                     rmsL, rmsR, db(rmsL), db(rmsR), g_peak[0], g_peak[1], st.keyedOnVoices[0], st.keyedOnVoices[1],
                     st.irqCount, st.dmaCount[0], st.dmaCount[1], static_cast<unsigned long long>(st.framesDropped),
                     static_cast<unsigned long long>(g_underrunFrames.load()),
                     static_cast<unsigned long long>(g_skippedFrames.load()), g_pace ? g_factor : 1.0,
                     static_cast<unsigned long long>(g_feedCalls.load()), static_cast<unsigned long long>(g_feedFrames.load()));
        g_statsFrames = 0;
        g_sumSq[0] = g_sumSq[1] = 0;
        g_peak[0] = g_peak[1] = 0;
    }

    void onSamples(void *, const int16_t *stereo, uint32_t frames)
    {
        {
            std::lock_guard lock(g_wavMutex);
            if (g_wav)
            {
                std::fwrite(stereo, 4, frames, g_wav);
                g_wavFrames += frames;
                if (g_wavFrames - g_wavHeaderAt >= kRate) // keep the file valid if the process is killed
                {
                    g_wavHeaderAt = g_wavFrames;
                    writeWavHeader(g_wav, g_wavFrames);
                    std::fflush(g_wav);
                }
            }
        }
        if (g_statsInterval > 0)
        {
            for (uint32_t i = 0; i < frames; ++i)
                for (int c = 0; c < 2; ++c)
                {
                    const int v = stereo[i * 2 + c];
                    g_sumSq[c] += static_cast<double>(v) * v;
                    g_peak[c] = std::max(g_peak[c], std::abs(v));
                }
            g_statsFrames += frames;
            const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
            const double last = std::chrono::duration<double>(g_statsLast - g_start).count();
            if (now - last >= g_statsInterval)
                printStats(now);
        }
    }

    // ---- SDL audio thread ---------------------------------------------------------------------------------------------
    void SDLCALL feed(void *, SDL_AudioStream *stream, int additional, int)
    {
        ++g_feedCalls;
        g_feedFrames += static_cast<uint64_t>(std::max(additional, 0)) / 4u;
        const uint32_t avail = kzspu2AvailableFrames();
        if (avail > kHighFrames)
            g_skippedFrames += kzspu2SkipFrames(avail - kTargetFrames);

        int16_t buf[1024 * 2];
        uint32_t need = static_cast<uint32_t>(std::max(additional, 0)) / 4u;
        while (need)
        {
            const uint32_t chunk = std::min<uint32_t>(need, 1024);
            const uint32_t got = kzspu2ReadSamples(buf, chunk);
            if (got < chunk)
            {
                std::memset(buf + got * 2, 0, (chunk - got) * 4);
                g_underrunFrames += chunk - got;
            }
            SDL_PutAudioStreamData(stream, buf, static_cast<int>(chunk * 4));
            need -= chunk;
        }
    }

    bool openDevice()
    {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        {
            std::cerr << "[kz_audio] SDL audio init failed: " << SDL_GetError() << std::endl;
            return false;
        }
        const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(kRate)};
        g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, nullptr);
        if (!g_stream)
        {
            std::cerr << "[kz_audio] no audio device: " << SDL_GetError() << std::endl;
            return false;
        }
        int volume = std::clamp(kzConfig().masterVolume, 0, 100);
        if (const char *v = std::getenv("KZ_AUDIO_VOLUME")) // automation: e.g. 0 to exercise the device silently
            volume = std::clamp(std::atoi(v), 0, 100);
        SDL_SetAudioStreamGain(g_stream, volume / 100.0f);
        const bool resumed = SDL_ResumeAudioStreamDevice(g_stream);
        const SDL_AudioDeviceID device = SDL_GetAudioStreamDevice(g_stream);
        const char *name = SDL_GetAudioDeviceName(device);
        SDL_AudioSpec deviceSpec{};
        int deviceFrames = 0;
        SDL_GetAudioDeviceFormat(device, &deviceSpec, &deviceFrames);
        std::cout << "[kz_audio] output: " << SDL_GetCurrentAudioDriver() << " device '" << (name ? name : "?") << "' "
                  << deviceSpec.freq << " Hz " << deviceSpec.channels << " ch, buffer " << deviceFrames
                  << " frames; stream 48 kHz stereo, volume " << volume << "%"
                  << (resumed ? "" : " (RESUME FAILED: ") << (resumed ? "" : SDL_GetError()) << (resumed ? "" : ")")
                  << std::endl;
        return true;
    }

    // atexit: finish the WAV. (kz_main ends with std::_Exit, which skips this; the header is also rewritten every
    // second of audio, so the file stays valid either way.) The SDL stream is left to SDL / process exit.
    void shutdown()
    {
        closeWav();
    }

    // ---- pacing ---------------------------------------------------------------------------------------------------------
    // The runtime's EE clock never falls behind wall time but can run ahead of it (measured: IOP time up to 7.5% faster
    // than real time), and the SPU2 mixes by IOP time. Fed straight to a 48 kHz device, that surplus would fill the
    // queue and force periodic skips. So the SPU2 gets its own clock: it advances by IOP time scaled by a factor <= 1
    // that tracks wall time (re-estimated in the advance hook). The SPU2 still always moves with IOP time, so IOP code
    // that waits for an SPU2 DMA or IRQ keeps working; it just sees the SPU2 run at real-time speed. KZ_AUDIO_PACE=0 off.
    constexpr double kIopHz = 36864000.0;
    bool g_paceInit = false;
    uint64_t g_lastIop = 0;
    double g_spu = 0;
    std::chrono::steady_clock::time_point g_paceT0, g_paceLastAdjust;
    double g_spuAtT0 = 0;

    uint64_t toSpu(uint64_t iop)
    {
        if (!g_paceInit)
        {
            g_paceInit = true;
            g_lastIop = iop;
            g_spu = g_spuAtT0 = static_cast<double>(iop);
            g_paceT0 = g_paceLastAdjust = std::chrono::steady_clock::now();
        }
        if (iop > g_lastIop)
        {
            g_spu += static_cast<double>(iop - g_lastIop) * (g_pace ? g_factor : 1.0);
            g_lastIop = iop;
        }
        return static_cast<uint64_t>(g_spu);
    }

    void adjustPace()
    {
        if (!g_pace)
            return;
        // Called on every SPU2 advance (i.e. every IOP time slice); reading the clock each time was measurable.
        static uint32_t calls = 0;
        if ((++calls & 255u) != 0u)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (now - g_paceLastAdjust < std::chrono::milliseconds(50))
            return;
        g_paceLastAdjust = now;
        const double wall = g_spuAtT0 + std::chrono::duration<double>(now - g_paceT0).count() * kIopHz;
        const double ahead = (g_spu - wall) / kIopHz; // seconds the SPU2 is ahead of real time
        g_paceAhead = ahead;
        // Aim to remove the error over ~2 s; limit to [0.8, 1.0] and smooth the change.
        const double desired = std::clamp(1.0 - ahead / 2.0, 0.8, 1.0);
        g_factor += (desired - g_factor) * 0.25;
        if (ahead < -0.25) // the IOP fell behind real time (stall): do not try to catch up later
            g_spuAtT0 -= (-ahead - 0.25) * kIopHz;
    }

    // ---- IOP hooks ----------------------------------------------------------------------------------------------------
    // KZ_AUDIO_TRACE=<n>: log the first n register writes, and every DMA start / IRQ / KON / ATTR write (debug aid).
    long g_trace = 0;
    long g_traceWrites = 0;

    uint16_t hookRead16(uint32_t a, uint64_t c) { return kzspu2Read16(a, toSpu(c)); }
    void hookWrite16(uint32_t a, uint16_t v, uint64_t c)
    {
        if (g_trace)
        {
            const uint32_t r = a & 0x7FF, reg = r & 0x3FF;
            const bool key = r < 0x760 && (reg == 0x1A0 || reg == 0x1A2 || reg == 0x1A4 || reg == 0x1A6 || reg == 0x19A);
            if (g_traceWrites++ < g_trace || key)
                std::fprintf(stderr, "[kz_audio] w %03x = %04x @%llu\n", r, v, static_cast<unsigned long long>(c));
        }
        kzspu2Write16(a, v, toSpu(c));
    }
    void hookDmaStart(int core, uint16_t *ram, uint32_t madr, uint32_t halfwords, bool toSpu2, uint64_t c)
    {
        if (g_trace)
            std::fprintf(stderr, "[kz_audio] dma core%d %s madr %06x %u halfwords @%llu\n", core, toSpu2 ? "write" : "read",
                         madr, halfwords, static_cast<unsigned long long>(c));
        if (toSpu2)
            kzspu2DmaWrite(core, ram, halfwords, madr, toSpu(c));
        else
            kzspu2DmaRead(core, ram, halfwords, madr, toSpu(c));
    }
    uint32_t hookDmaMadr(int core) { return kzspu2DmaMadr(core); }
    void hookAdvance(uint64_t c)
    {
        kzspu2Advance(toSpu(c));
        adjustPace();
    }
    // The SPU2's next event is in its own clock; the IOP wants its cycle.
    uint64_t hookNextEvent()
    {
        const uint64_t e = kzspu2NextEventCycle();
        if (e == UINT64_MAX)
            return e;
        const double spuNow = g_spu;
        if (static_cast<double>(e) <= spuNow)
            return g_lastIop + 1;
        const double f = g_pace ? g_factor : 1.0;
        return g_lastIop + static_cast<uint64_t>(std::ceil((static_cast<double>(e) - spuNow) / f));
    }

    void onIrq(void *)
    {
        if (g_trace)
            std::fprintf(stderr, "[kz_audio] irq\n");
        ps2x::iop::iopSpu2RaiseIrq();
    }
    void onDmaComplete(void *, int core)
    {
        if (g_trace)
            std::fprintf(stderr, "[kz_audio] dma core%d done @%llu\n", core,
                         static_cast<unsigned long long>(kzspu2GetStatus().iopCycle));
        ps2x::iop::iopSpu2DmaComplete(core);
    }
    void onLog(void *, int level, const char *msg)
    {
        if (level >= 1)
            std::cerr << "[kzspu2] " << msg << std::endl;
    }
}

bool kzAudioInstall(bool outputDevice)
{
    if (!envFlag("KZ_AUDIO", true))
    {
        std::cout << "[kz_audio] KZ_AUDIO=off: no sound" << std::endl;
        return false;
    }

    g_pace = envFlag("KZ_AUDIO_PACE", true);
    if (const char *t = std::getenv("KZ_AUDIO_TRACE"); t && *t)
        g_trace = std::max(1L, std::atol(t));

    KzSpu2Config cfg;
    cfg.irq = onIrq;
    cfg.dmaComplete = onDmaComplete;
    cfg.samples = onSamples;
    cfg.log = onLog;
    cfg.ringFrames = kRate / 2; // 500 ms
    if (!kzspu2Init(cfg))
    {
        std::cerr << "[kz_audio] kzspu2Init failed" << std::endl;
        return false;
    }

    ps2x::iop::IopHostSpu2 hooks;
    hooks.read16 = hookRead16;
    hooks.write16 = hookWrite16;
    hooks.dmaStart = hookDmaStart;
    hooks.dmaMadr = hookDmaMadr;
    hooks.advance = hookAdvance;
    hooks.nextEvent = hookNextEvent;
    ps2x::iop::iopSetHostSpu2(hooks);

    g_start = g_statsLast = std::chrono::steady_clock::now();
    if (const char *wav = std::getenv("KZ_AUDIO_WAV"); wav && *wav)
    {
        g_wav = std::fopen(wav, "wb");
        if (g_wav)
        {
            writeWavHeader(g_wav, 0);
            g_statsInterval = 5;
            std::cout << "[kz_audio] writing " << wav << std::endl;
        }
        else
            std::cerr << "[kz_audio] cannot write " << wav << std::endl;
    }
    if (const char *s = std::getenv("KZ_AUDIO_STATS"); s && *s)
        g_statsInterval = std::atof(s);

    // Not derived from PS2X_HEADLESS: kz_main sets that for the runtime in every run (the runtime's own window is
    // never used), which is why the device was never opened in real play.
    if (envFlag("KZ_AUDIO_DEVICE", outputDevice))
    {
        if (!openDevice())
            std::cout << "[kz_audio] NO SOUND: the audio device could not be opened" << std::endl;
    }
    else
        std::cout << "[kz_audio] no output device (automated run or KZ_AUDIO_DEVICE=0); SPU2 still emulated" << std::endl;

    std::atexit(shutdown);
    g_active = true;
    return true;
}

void kzAudioBindRuntime(PS2Runtime &runtime)
{
    g_runtime = &runtime;
}

bool kzAudioActive()
{
    return g_active;
}
