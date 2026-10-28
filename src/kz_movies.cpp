// Optional skip of the boot intro movie (GitHub issue #5).
//
// FUN_0026ECC8 (PAL 0x26F338, kzAddrs().playMovie) is the game's blocking movie player (a0 = movie name string object, *a0 + 0x10 = the characters).
// It first checks that the file exists and returns 0 without playing when it does not; callers then carry on as
// after the movie. With the option on, the hook takes that path for the intro movie only ("Movies/Intro/Intro").
// Story cutscenes, the attract loop and the menu background movies are untouched.
// KZ_MOVIE_LOG=1 logs every movie the player is asked to play.
//
// Optional skip of the boot logo screens (issue #24). They are a front-end script page, "Logos" (Lua, FILES03.DAT): its
// LogosUpdate fades the SCEA/SCEE logo, the legal screen and the Guerrilla logo on a timeline and at 21 s calls
// SwitchToMenu("MemoryCardTest"), which leads on to the intro movie and the main menu. FUN_001A8958 (PAL 0x1A8B30,
// kzAddrs().menuPageUpdate) runs the current page's Update each frame, with the script clock nowSeconds = menu+0x170
// (milliseconds) x 0.001. While the page is "Logos", the hook moves that counter forward by kLogoStepMs per frame, so the
// script plays its own timeline to the end in a few frames. The page is still created (LogosInit, and the KILLZONE logo
// resource loaded for the main menu), the memory card check and everything after it run unchanged.
// KZ_SKIP_LOGOS=0/1 overrides the setting; KZ_LOGO_LOG=1 logs the counter on each Logos frame.

#include "kz_movies.h"

#include "kz_config.h"
#include "kz_addrs.h"
#include "ps2_runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
    constexpr uint32_t kRamMask = 0x01FFFFFFu;
    constexpr const char *kIntroMovie = "Movies/Intro/Intro";

    PS2Runtime::RecompiledFunction g_origPlayMovie = nullptr;
    bool g_skipIntro = false;
    bool g_skipLogos = false;
    bool g_logoLog = false;
    constexpr uint32_t kLogoStepMs = 2000; // 21 s timeline in about 11 frames
    PS2Runtime::RecompiledFunction g_origMenuPageUpdate = nullptr;
    bool g_log = false;

    uint32_t r32(const uint8_t *rdram, uint32_t a)
    {
        uint32_t v;
        std::memcpy(&v, rdram + (a & kRamMask), 4);
        return v;
    }

    std::string movieName(const uint8_t *rdram, uint32_t stringObject)
    {
        if (stringObject < 0x00100000u || stringObject >= 0x02000000u)
            return {};
        const uint32_t buffer = r32(rdram, stringObject);
        if (buffer < 0x00100000u || buffer >= 0x02000000u)
            return {};
        const char *chars = reinterpret_cast<const char *>(rdram + ((buffer + 0x10u) & kRamMask));
        return std::string(chars, strnlen(chars, 128));
    }

    void playMovie(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        if (ctx->pc == kzAddrs().playMovie)
        {
            const std::string name = movieName(rdram, getRegU32(ctx, 4));
            const bool skip = g_skipIntro && name == kIntroMovie;
            if (g_log || skip)
                std::printf("[kz] movie \"%s\"%s\n", name.c_str(), skip ? ": skipped (Skip intro movies)" : "");
            if (skip)
            {
                ctx->pc = getRegU32(ctx, 31);
                setReturnS32(ctx, 0);
                return;
            }
        }
        g_origPlayMovie(rdram, ctx, rt);
    }

    void menuPageUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t menu = getRegU32(ctx, 4);
        if (ctx->pc == kzAddrs().menuPageUpdate && menu >= 0x00100000u && menu < 0x02000000u && movieName(rdram, menu + 8) == "Logos")
        {
            static int frames = 0;
            uint32_t ms = r32(rdram, menu + 0x170) + kLogoStepMs;
            std::memcpy(rdram + ((menu + 0x170) & kRamMask), &ms, 4);
            if (frames++ == 0)
                std::printf("[kz] boot logos: skipping (Skip logos)\n");
            if (g_logoLog)
                std::printf("[kz] logos frame %d: menu+0x170 = %u ms\n", frames, ms);
        }
        g_origMenuPageUpdate(rdram, ctx, rt);
    }
}

void kzMoviesInstall(PS2Runtime &runtime)
{
    g_skipIntro = kzConfig().skipIntro;
    if (const char *v = std::getenv("KZ_SKIP_INTRO"); v && *v)
        g_skipIntro = std::atoi(v) != 0;
    g_log = std::getenv("KZ_MOVIE_LOG") != nullptr;
    g_skipLogos = kzConfig().skipLogos;
    if (const char *v = std::getenv("KZ_SKIP_LOGOS"); v && *v)
        g_skipLogos = std::atoi(v) != 0;
    g_logoLog = std::getenv("KZ_LOGO_LOG") != nullptr;
    if (g_skipLogos)
    {
        const uint32_t kUpdate = kzAddrs().menuPageUpdate;
        g_origMenuPageUpdate = runtime.hasFunction(kUpdate) ? runtime.lookupFunction(kUpdate) : nullptr;
        if (!g_origMenuPageUpdate || !runtime.replaceFunction(kUpdate, &menuPageUpdate))
            std::printf("[kz] logo skip hook FAILED\n");
    }
    if (!g_skipIntro && !g_log)
        return;
    const uint32_t kPlayMovie = kzAddrs().playMovie;
    g_origPlayMovie = runtime.hasFunction(kPlayMovie) ? runtime.lookupFunction(kPlayMovie) : nullptr;
    if (!g_origPlayMovie || !runtime.replaceFunction(kPlayMovie, &playMovie))
        std::printf("[kz] movie hook FAILED\n");
}
