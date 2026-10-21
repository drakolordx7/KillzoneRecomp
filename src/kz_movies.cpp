// Optional skip of the boot intro movie (GitHub issue #5).
//
// FUN_0026ECC8 is the game's blocking movie player (a0 = movie name string object, *a0 + 0x10 = the characters).
// It first checks that the file exists and returns 0 without playing when it does not; callers then carry on as
// after the movie. With the option on, the hook takes that path for the intro movie only ("Movies/Intro/Intro").
// Story cutscenes, the attract loop and the menu background movies are untouched.
// KZ_MOVIE_LOG=1 logs every movie the player is asked to play.

#include "kz_movies.h"

#include "kz_config.h"
#include "ps2_runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
    constexpr uint32_t kPlayMovie = 0x0026ECC8u;
    constexpr uint32_t kRamMask = 0x01FFFFFFu;
    constexpr const char *kIntroMovie = "Movies/Intro/Intro";

    PS2Runtime::RecompiledFunction g_origPlayMovie = nullptr;
    bool g_skipIntro = false;
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
        if (ctx->pc == kPlayMovie)
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
}

void kzMoviesInstall(PS2Runtime &runtime)
{
    g_skipIntro = kzConfig().skipIntro;
    if (const char *v = std::getenv("KZ_SKIP_INTRO"); v && *v)
        g_skipIntro = std::atoi(v) != 0;
    g_log = std::getenv("KZ_MOVIE_LOG") != nullptr;
    if (!g_skipIntro && !g_log)
        return;
    g_origPlayMovie = runtime.hasFunction(kPlayMovie) ? runtime.lookupFunction(kPlayMovie) : nullptr;
    if (!g_origPlayMovie || !runtime.replaceFunction(kPlayMovie, &playMovie))
        std::printf("[kz] movie hook FAILED\n");
}
