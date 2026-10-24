#include "kz_devtools.h"

#include "ps2_runtime.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace
{
    constexpr uint32_t kRamMask = 0x01FFFFFFu;
    const auto g_t0 = std::chrono::steady_clock::now();
    double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count(); }

    struct Poke
    {
        double t;
        bool repeat;
        uint32_t addr, value;
        int size;
        bool done = false;
    };
    struct Peek
    {
        double t;
        uint32_t addr;
        int words;
        bool done = false;
    };

    std::vector<Poke> &pokes()
    {
        static std::vector<Poke> v = []
        {
            std::vector<Poke> out;
            const char *e = std::getenv("KZ_POKE");
            if (!e)
                return out;
            std::string all(e);
            size_t pos = 0;
            while (pos < all.size())
            {
                size_t end = all.find(';', pos);
                std::string it = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                pos = end == std::string::npos ? all.size() : end + 1;
                size_t c = it.find(':');
                size_t eq = it.find('=');
                if (c == std::string::npos || eq == std::string::npos)
                    continue;
                Poke p{};
                p.repeat = it[c - 1] == '+';
                p.t = std::atof(it.c_str());
                p.addr = static_cast<uint32_t>(std::strtoul(it.c_str() + c + 1, nullptr, 16));
                p.value = static_cast<uint32_t>(std::strtoul(it.c_str() + eq + 1, nullptr, 0));
                p.size = 4;
                if (size_t sl = it.find('/', eq); sl != std::string::npos)
                    p.size = std::atoi(it.c_str() + sl + 1);
                out.push_back(p);
            }
            return out;
        }();
        return v;
    }
    std::vector<Peek> &peeks()
    {
        static std::vector<Peek> v = []
        {
            std::vector<Peek> out;
            const char *e = std::getenv("KZ_PEEK");
            if (!e)
                return out;
            std::string all(e);
            size_t pos = 0;
            while (pos < all.size())
            {
                size_t end = all.find(';', pos);
                std::string it = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                pos = end == std::string::npos ? all.size() : end + 1;
                size_t c1 = it.find(':');
                size_t c2 = it.find(':', c1 == std::string::npos ? 0 : c1 + 1);
                if (c1 == std::string::npos || c2 == std::string::npos)
                    continue;
                Peek p{};
                p.t = std::atof(it.c_str());
                p.addr = static_cast<uint32_t>(std::strtoul(it.c_str() + c1 + 1, nullptr, 16));
                p.words = std::atoi(it.c_str() + c2 + 1);
                out.push_back(p);
            }
            return out;
        }();
        return v;
    }

    // ---- function hook logging
    struct HookSlot
    {
        uint32_t addr = 0;
        PS2Runtime::RecompiledFunction orig = nullptr;
    };
    std::atomic<int> g_hookLines{0};
    int hookMax()
    {
        static int m = std::getenv("KZ_HOOKLOG_MAX") ? std::atoi(std::getenv("KZ_HOOKLOG_MAX")) : 300;
        return m;
    }
    template <int N>
    struct HookWrap
    {
        static inline HookSlot slot;
        static void fn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
        {
            const uint32_t ra = getRegU32(ctx, 31);
            const bool log = g_hookLines.load() < hookMax();
            if (log)
            {
                g_hookLines++;
                std::fprintf(stderr, "[hook] t=%.2f %06x(a0=%x a1=%x a2=%x a3=%x) ra=%x\n", nowSec(), slot.addr, getRegU32(ctx, 4),
                             getRegU32(ctx, 5), getRegU32(ctx, 6), getRegU32(ctx, 7), ra);
            }
            slot.orig(rdram, ctx, rt);
            if (log && ctx->pc == ra)
                std::fprintf(stderr, "[hook] t=%.2f %06x ret v0=%x\n", nowSec(), slot.addr, getRegU32(ctx, 2));
        }
    };
    template <int N>
    void install(PS2Runtime &rt, uint32_t addr)
    {
        HookWrap<N>::slot.addr = addr;
        HookWrap<N>::slot.orig = rt.lookupFunction(addr);
        if (!HookWrap<N>::slot.orig || !rt.replaceFunction(addr, &HookWrap<N>::fn))
            std::cerr << "[kz] hooklog failed @0x" << std::hex << addr << std::dec << std::endl;
    }
}

namespace
{
    // KZ_CPDEBUG=1: wraps FUN_0017d318(world, nameObj, outVec) (object-list query used by the checkpoint scan, FUN_001c5330)
    // and logs the name object's words and the resulting list (count, first objects with their +0xB0 byte), at most 40 lines.
    uint32_t rdw(const uint8_t *ram, uint32_t a);
    void killAll(uint8_t *ram);
    // KZ_KILLALL_SYNC=1: KZ_KILLALL runs on the guest thread at the frame limiter (0x1BFF10, once per main-loop pass) instead of on
    // the vsync thread, so the health pokes cannot land in the middle of a guest update (issue #4: rules out a test artifact).
    PS2Runtime::RecompiledFunction g_origSyncKill = nullptr;
    void listWatch(uint8_t *ram);
    void syncKillWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        if (ctx->pc == 0x001BFF10u)
            listWatch(rdram);
        if (ctx->pc == 0x001BFF10u && std::getenv("KZ_KILLALL_SYNC"))
        {
            static const char *k = std::getenv("KZ_KILLALL");
            static const double start = k ? std::atof(k) : 1e9;
            static const double period = [] { const char *c = k ? std::strchr(k, ',') : nullptr; return c ? std::atof(c + 1) : 2.0; }();
            static double next = start;
            const double t = nowSec();
            if (t >= next)
            {
                next = t + period;
                killAll(rdram);
            }
        }
        g_origSyncKill(rdram, ctx, rt);
    }
    // KZ_VECHOOK="func,vecoff,ownervt" (hex): logs every call of guest function func whose a0 is the vector at offset vecoff of an object whose
    // first word is ownervt (issue #4: who erases / clears the player controller's camera vector 0x1C4): a0..a2, ra, the vector's count before
    // and after the call, and the return-address-like words near sp.
    PS2Runtime::RecompiledFunction g_origVecHook = nullptr;
    uint32_t g_vecOff = 0, g_vecVt = 0;
    int g_vecLines = 0;
    void vecHookWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t a0 = getRegU32(ctx, 4), ra = getRegU32(ctx, 31), sp = getRegU32(ctx, 29);
        const bool hit = a0 > g_vecOff && g_vecLines < 400 && rdw(rdram, a0 - g_vecOff) == g_vecVt;
        uint32_t before = 0;
        if (hit)
        {
            before = rdw(rdram, a0);
            std::fprintf(stderr, "[vechook] t=%.2f vec=%x owner=%x a1=%x a2=%x ra=%x count=%u idx=%d stack:", nowSec(), a0, a0 - g_vecOff, getRegU32(ctx, 5), getRegU32(ctx, 6), ra, before, static_cast<int>(rdw(rdram, a0 - g_vecOff + 0x1dc)));
            int n = 0;
            for (uint32_t o = 0; o < 0x200 && n < 8; o += 4)
            {
                const uint32_t w = rdw(rdram, sp + o);
                if (w >= 0x140000u && w < 0x55a674u && (w & 3u) == 0u)
                {
                    std::fprintf(stderr, " +%x=%x", o, w);
                    ++n;
                }
            }
            std::fprintf(stderr, "\n");
            ++g_vecLines;
        }
        g_origVecHook(rdram, ctx, rt);
        if (hit && ctx->pc == ra)
            std::fprintf(stderr, "[vechook]   -> count=%u idx=%d\n", rdw(rdram, a0), static_cast<int>(rdw(rdram, a0 - g_vecOff + 0x1dc)));
    }
    // KZ_NULLRET="addr" (hex): logs every return of that guest function with v0 == 0 (a0..a3, ra, t seconds), at most 100 lines (issue #4:
    // allocator failures; the dlmalloc entry is 0x1727F8).
    PS2Runtime::RecompiledFunction g_origNullRet = nullptr;
    int g_nullRetLines = 0;
    void nullRetWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t ra = getRegU32(ctx, 31), a0 = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5), a2 = getRegU32(ctx, 6), a3 = getRegU32(ctx, 7);
        g_origNullRet(rdram, ctx, rt);
        if (ctx->pc == ra && getRegU32(ctx, 2) == 0u && g_nullRetLines < 100)
        {
            ++g_nullRetLines;
            std::fprintf(stderr, "[nullret] t=%.2f a0=%x a1=%x a2=%x a3=%x ra=%x\n", nowSec(), a0, a1, a2, a3, ra);
        }
    }
    PS2Runtime::RecompiledFunction g_origGetList = nullptr;
    int g_cpLines = 0;
    void getListWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t nameObj = getRegU32(ctx, 5), out = getRegU32(ctx, 6), ra = getRegU32(ctx, 31);
        g_origGetList(rdram, ctx, rt);
        if (ctx->pc == ra && nameObj == 0x564b48u && out)
        {
            static const double force = std::getenv("KZ_CPFORCE") ? std::atof(std::getenv("KZ_CPFORCE")) : -1.0;
            static int forced = 0;
            const uint32_t n0 = rdw(rdram, out), a0 = rdw(rdram, out + 8);
            if (force >= 0.0 && nowSec() >= force && n0 > 0 && forced < 3 && (nowSec() - force) > forced * 20.0)
            {
                const uint32_t o = rdw(rdram, a0);
                rdram[(o + 0xb0) & kRamMask] = 1;
                ++forced;
                std::fprintf(stderr, "[cpforce] t=%.2f object %x flag set\n", nowSec(), o);
            }
        }
        if (ctx->pc == ra && nameObj == 0x564b48u && g_cpLines < 40 && out)
        {
            ++g_cpLines;
            uint32_t cnt = rdw(rdram, out), arr = rdw(rdram, out + 8);
            std::fprintf(stderr, "[cplist] t=%.2f out=%x count=%u arr=%x", nowSec(), out, cnt, arr);
            for (uint32_t i = 0; i < cnt && i < 16; ++i)
            {
                const uint32_t o = rdw(rdram, arr + i * 4);
                float x, y, z;
                const uint32_t wx = rdw(rdram, o + 0x50), wy = rdw(rdram, o + 0x54), wz = rdw(rdram, o + 0x58);
                std::memcpy(&x, &wx, 4), std::memcpy(&y, &wy, 4), std::memcpy(&z, &wz, 4);
                std::fprintf(stderr, " [%x vt=%x b0=%02x pos=%.1f,%.1f,%.1f]", o, rdw(rdram, o), rdram[(o + 0xb0) & kRamMask], x, y, z);
            }
            std::fprintf(stderr, "\n");
        }
    }
}

void kzDevHooksInstall(PS2Runtime &runtime)
{
    if (const char *v = std::getenv("KZ_NULLRET"))
    {
        const uint32_t f = static_cast<uint32_t>(std::strtoul(v, nullptr, 16));
        g_origNullRet = runtime.lookupFunction(f);
        if (!g_origNullRet || !runtime.replaceFunction(f, &nullRetWrap))
            std::cerr << "[kz] nullret hook failed" << std::endl;
    }
    if (const char *v = std::getenv("KZ_VECHOOK"))
    {
        unsigned f = 0, o = 0, vt = 0;
        if (std::sscanf(v, "%x,%x,%x", &f, &o, &vt) == 3)
        {
            g_vecOff = o;
            g_vecVt = vt;
            g_origVecHook = runtime.lookupFunction(f);
            if (!g_origVecHook || !runtime.replaceFunction(f, &vecHookWrap))
                std::cerr << "[kz] vechook failed" << std::endl;
        }
    }
    if ((std::getenv("KZ_KILLALL_SYNC") && std::getenv("KZ_KILLALL")) || std::getenv("KZ_LISTWATCH"))
    {
        g_origSyncKill = runtime.lookupFunction(0x001BFF10u);
        if (!g_origSyncKill || !runtime.replaceFunction(0x001BFF10u, &syncKillWrap))
            std::cerr << "[kz] killall-sync hook failed" << std::endl;
    }
    if (const char *c = std::getenv("KZ_CPDEBUG"); c && *c == '1')
    {
        g_origGetList = runtime.lookupFunction(0x0017D318u);
        if (!g_origGetList || !runtime.replaceFunction(0x0017D318u, &getListWrap))
            std::cerr << "[kz] cpdebug hook failed" << std::endl;
    }
    const char *e = std::getenv("KZ_HOOKLOG");
    if (!e || !*e)
        return;
    std::string all(e);
    int n = 0;
    size_t pos = 0;
    while (pos < all.size())
    {
        size_t end = all.find(',', pos);
        const uint32_t a = static_cast<uint32_t>(std::strtoul(all.c_str() + pos, nullptr, 16));
        pos = end == std::string::npos ? all.size() : end + 1;
        switch (n++)
        {
        case 0: install<0>(runtime, a); break;
        case 1: install<1>(runtime, a); break;
        case 2: install<2>(runtime, a); break;
        case 3: install<3>(runtime, a); break;
        case 4: install<4>(runtime, a); break;
        case 5: install<5>(runtime, a); break;
        case 6: install<6>(runtime, a); break;
        case 7: install<7>(runtime, a); break;
        default: break;
        }
    }
}

namespace
{
    // KZ_TELEPORT="t:x,y,z;..." : at wall second t the player controller (vtable 0x524B50, found through the player
    // entity 0x525710 +0x31C) gets that position (ctl+0x50/54/58). Testing aid only.
    struct Tele
    {
        double t;
        float x, y, z;
        bool done = false;
    };
    std::vector<Tele> &teles()
    {
        static std::vector<Tele> v = []
        {
            std::vector<Tele> out;
            const char *e = std::getenv("KZ_TELEPORT");
            if (!e)
                return out;
            std::string all(e);
            size_t pos = 0;
            while (pos < all.size())
            {
                size_t end = all.find(';', pos);
                std::string it = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                pos = end == std::string::npos ? all.size() : end + 1;
                Tele t{};
                float x, y, z;
                if (std::sscanf(it.c_str(), "%lf:%f,%f,%f", &t.t, &x, &y, &z) == 4)
                {
                    t.x = x, t.y = y, t.z = z;
                    out.push_back(t);
                }
            }
            return out;
        }();
        return v;
    }
    uint32_t rdw(const uint8_t *ram, uint32_t a)
    {
        uint32_t v = 0;
        std::memcpy(&v, ram + (a & kRamMask), 4);
        return v;
    }
    uint32_t findCtl(const uint8_t *ram)
    {
        for (uint32_t a = 0x100000; a + 0x320 < 0x02000000u; a += 4)
            if (rdw(ram, a) == 0x525710u)
            {
                const uint32_t c = rdw(ram, a + 0x31C);
                if (c >= 0x100000u && c < 0x01FFF000u && rdw(ram, c) == 0x524B50u)
                    return c;
            }
        return 0;
    }
}

namespace
{
    // any character entity: a game object (vtable in the game's data range) whose +0x31C is a character controller (vt 0x524B50)
    bool isChar(const uint8_t *ram, uint32_t a)
    {
        const uint32_t vt = rdw(ram, a);
        if (vt < 0x500000u || vt >= 0x580000u)
            return false;
        const uint32_t c = rdw(ram, a + 0x31C);
        return c >= 0x100000u && c < 0x01FFF000u && rdw(ram, c) == 0x524B50u;
    }
}

namespace
{
    // KZ_ACTORS=<period seconds>: every period, log every character controller (vtable 0x524B50) with position/heading.
    void logActors(const uint8_t *ram, double t)
    {
        std::fprintf(stderr, "[actors] t=%.1f", t);
        int n = 0;
        for (uint32_t a = 0x100000; a + 0x80 < 0x02000000u && n < 64; a += 4)
            if (rdw(ram, a) == 0x524B50u)
            {
                float f[4];
                const uint32_t offs[4] = {0x50, 0x54, 0x58, 0x78};
                for (int i = 0; i < 4; ++i)
                {
                    const uint32_t w = rdw(ram, a + offs[i]);
                    std::memcpy(&f[i], &w, 4);
                }
                std::fprintf(stderr, " [%06x %.1f,%.1f,%.1f h%.2f]", a, f[0], f[1], f[2], f[3]);
                ++n;
            }
        std::fprintf(stderr, "\n");
        // character entities (vt 0x525710): address, health (+0x30C), controller position
        std::fprintf(stderr, "[ents] t=%.1f", t);
        n = 0;
        for (uint32_t a = 0x100000; a + 0x320 < 0x02000000u && n < 64; a += 4)
            if (isChar(ram, a))
            {
                const uint32_t c = rdw(ram, a + 0x31C);
                float hp, x = 0, y = 0, z = 0;
                const uint32_t wh = rdw(ram, a + 0x30C);
                std::memcpy(&hp, &wh, 4);
                if (c >= 0x100000u && c < 0x01FFF000u)
                {
                    const uint32_t wx = rdw(ram, c + 0x50), wy = rdw(ram, c + 0x54), wz = rdw(ram, c + 0x58);
                    std::memcpy(&x, &wx, 4), std::memcpy(&y, &wy, 4), std::memcpy(&z, &wz, 4);
                }
                std::fprintf(stderr, " [%06x vt=%x hp=%.0f %.0f,%.0f,%.0f]", a, rdw(ram, a), hp, x, y, z);
                ++n;
            }
        std::fprintf(stderr, "\n");
    }
}

namespace
{
    // KZ_PLAYERPOKE="t:off=value;..." : from wall second t, every vsync, writes the 32-bit value (hex or float with 'f' suffix,
    // e.g. 0x42c80000 or 100.0f) at player entity (vtable 0x525710) + off. KZ_GOD=1 pins +0x30C to 100.0f (health).
    struct PP
    {
        double t;
        uint32_t off, value;
    };
    std::vector<PP> &pps()
    {
        static std::vector<PP> v = []
        {
            std::vector<PP> out;
            if (const char *g = std::getenv("KZ_GOD"); g && *g == '1')
                out.push_back({0.0, 0x30C, 0x42C80000u});
            const char *e = std::getenv("KZ_PLAYERPOKE");
            if (!e)
                return out;
            std::string all(e);
            size_t pos = 0;
            while (pos < all.size())
            {
                size_t end = all.find(';', pos);
                std::string it = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                pos = end == std::string::npos ? all.size() : end + 1;
                const size_t c = it.find(':'), eq = it.find('=');
                if (c == std::string::npos || eq == std::string::npos)
                    continue;
                PP p{};
                p.t = std::atof(it.c_str());
                p.off = static_cast<uint32_t>(std::strtoul(it.c_str() + c + 1, nullptr, 16));
                const char *vs = it.c_str() + eq + 1;
                if (it.back() == 'f')
                {
                    const float f = static_cast<float>(std::atof(vs));
                    std::memcpy(&p.value, &f, 4);
                }
                else
                    p.value = static_cast<uint32_t>(std::strtoul(vs, nullptr, 0));
                out.push_back(p);
            }
            return out;
        }();
        return v;
    }
    uint32_t g_player = 0;
    uint32_t findPlayer(const uint8_t *ram)
    {
        if (g_player && rdw(ram, g_player) == 0x525710u)
            return g_player;
        g_player = 0;
        static double nextScan = 0.0;
        const double t = nowSec();
        if (t < nextScan)
            return 0;
        nextScan = t + 1.0;
        for (uint32_t a = 0x100000; a + 0x320 < 0x02000000u; a += 4)
            if (rdw(ram, a) == 0x525710u)
            {
                const uint32_t c = rdw(ram, a + 0x31C);
                if (c >= 0x100000u && c < 0x01FFF000u && rdw(ram, c) == 0x524B50u)
                    return g_player = a;
            }
        return 0;
    }
}

namespace
{
    // KZ_KILLALL="start[,period]": from wall second start, every period seconds (default 2) sets the health (+0x30C) of every
    // character entity (vt 0x525710) except the player to 0 (testing aid: clears the waves so the level scripts advance).
    // True when `a` is the start of an in-use dlmalloc block of at least 0x320 bytes (size field at a-4, the next block's prev-in-use bit set).
    // The first version of KZ_KILLALL matched stale vtable words inside other live data (a list buffer, freed memory) and zeroed
    // a+0x30C there: the NULL list entries that looked like the issue #4 hang (found with KZ_PAGEWATCH, 2026-10-24).
    bool liveChunk(const uint8_t *ram, uint32_t a)
    {
        const uint32_t sz = rdw(ram, a - 4) & ~7u;
        if (sz < 0x328u || sz > 0x100000u || (a & 7u) != 0u)
            return false;
        const uint32_t next = a - 8u + sz;
        if (next + 8u >= 0x02000000u)
            return false;
        return (rdw(ram, next + 4u) & 1u) != 0u && (rdw(ram, a - 4u) & 2u) == 0u;
    }
    void killAll(uint8_t *ram)
    {
        const uint32_t pl = findPlayer(ram);
        int n = 0;
        const uint32_t zero = 0;
        for (uint32_t a = 0x100000; a + 0x320 < 0x02000000u; a += 4)
            if (a != pl && isChar(ram, a) && liveChunk(ram, a))
            {
                float hp;
                const uint32_t wh = rdw(ram, a + 0x30C);
                std::memcpy(&hp, &wh, 4);
                if (hp > 0.0f)
                {
                    std::memcpy(ram + ((a + 0x30C) & kRamMask), &zero, 4);
                    ++n;
                }
            }
        if (n)
            std::fprintf(stderr, "[killall] t=%.1f killed %d\n", nowSec(), n);
    }
}

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

namespace
{
    // KZ_PAGEWATCH=1 (with KZ_LISTWATCH): from wall second KZ_PAGEWATCH_T (default 130) the pointer buffer of the largest watched list is made
    // read-only; every write into it is logged with the host call stack (generated functions are named after the guest function:
    // FUN_001f9470_0x1f9470) and the value written, then the page is made writable for one instruction (issue #4: finds who puts the NULL).
    uint8_t *g_pwRam = nullptr;
    uintptr_t g_pwPage = 0, g_pwSize = 0, g_pwLo = 0, g_pwHi = 0;
    int g_pwLines = 0;
    thread_local uintptr_t t_pwStepAddr = 0;
    void pwLog(const char *s)
    {
        DWORD n;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), s, static_cast<DWORD>(std::strlen(s)), &n, nullptr);
    }
    LONG CALLBACK pwVeh(PEXCEPTION_POINTERS ep)
    {
        EXCEPTION_RECORD *er = ep->ExceptionRecord;
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->ExceptionInformation[0] == 1)
        {
            const uintptr_t a = er->ExceptionInformation[1];
            if (g_pwSize && a >= g_pwPage && a < g_pwPage + g_pwSize)
            {
                if (a >= g_pwLo && a < g_pwHi && g_pwLines < 300)
                {
                    ++g_pwLines;
                    char buf[512];
                    std::snprintf(buf, sizeof buf, "[pagewatch] t=%.2f write guest=%x slot=%d\n", nowSec(), static_cast<unsigned>(a - reinterpret_cast<uintptr_t>(g_pwRam)), static_cast<int>((a - g_pwLo) / 4));
                    pwLog(buf);
                    void *frames[24];
                    const USHORT n = CaptureStackBackTrace(0, 24, frames, nullptr);
                    static bool symInit = false;
                    if (!symInit)
                    {
                        symInit = true;
                        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
                        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
                    }
                    for (USHORT i = 1; i < n; ++i)
                    {
                        alignas(SYMBOL_INFO) char sbuf[sizeof(SYMBOL_INFO) + 256];
                        SYMBOL_INFO *sym = reinterpret_cast<SYMBOL_INFO *>(sbuf);
                        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                        sym->MaxNameLen = 255;
                        DWORD64 disp = 0;
                        if (SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(frames[i]), &disp, sym))
                            std::snprintf(buf, sizeof buf, "    %s+0x%llx\n", sym->Name, static_cast<unsigned long long>(disp));
                        else
                            std::snprintf(buf, sizeof buf, "    %p\n", frames[i]);
                        pwLog(buf);
                    }
                }
                DWORD old;
                VirtualProtect(reinterpret_cast<void *>(g_pwPage), g_pwSize, PAGE_READWRITE, &old);
                ep->ContextRecord->EFlags |= 0x100;
                t_pwStepAddr = a;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
        else if (er->ExceptionCode == EXCEPTION_SINGLE_STEP && t_pwStepAddr)
        {
            if (t_pwStepAddr >= g_pwLo && t_pwStepAddr < g_pwHi && g_pwLines < 300)
            {
                char buf[128];
                std::snprintf(buf, sizeof buf, "    -> value now %x\n", *reinterpret_cast<uint32_t *>(t_pwStepAddr & ~static_cast<uintptr_t>(3)));
                pwLog(buf);
            }
            t_pwStepAddr = 0;
            DWORD old;
            if (g_pwSize)
                VirtualProtect(reinterpret_cast<void *>(g_pwPage), g_pwSize, PAGE_READONLY, &old);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // Called once per pass with the buffer to watch (guest address, bytes); re-arms when it moves.
    void pageWatchSet(uint8_t *ram, uint32_t data, uint32_t bytes)
    {
        static bool veh = false;
        if (!veh)
        {
            veh = true;
            AddVectoredExceptionHandler(1, pwVeh);
        }
        g_pwRam = ram;
        const uintptr_t lo = reinterpret_cast<uintptr_t>(ram) + data, hi = lo + bytes;
        if (lo == g_pwLo && hi == g_pwHi)
            return;
        DWORD old;
        if (g_pwSize)
            VirtualProtect(reinterpret_cast<void *>(g_pwPage), g_pwSize, PAGE_READWRITE, &old);
        g_pwLo = lo;
        g_pwHi = hi;
        g_pwPage = lo & ~static_cast<uintptr_t>(0xfff);
        g_pwSize = ((hi + 0xfff) & ~static_cast<uintptr_t>(0xfff)) - g_pwPage;
        VirtualProtect(reinterpret_cast<void *>(g_pwPage), g_pwSize, PAGE_READONLY, &old);
        std::fprintf(stderr, "[pagewatch] t=%.2f armed guest %x..%x\n", nowSec(), data, data + bytes);
    }
}
#else
namespace
{
    void pageWatchSet(uint8_t *, uint32_t, uint32_t) {}
}
#endif

namespace
{
    // KZ_LISTWATCH="vtable[,count_off,data_off]" (hex, offsets default 0x38 / 0x40): on the guest thread once per main-loop pass, watches the
    // pointer vectors of all objects with that vtable (rescanned every 120 passes) and, the first time one holds a NULL entry, dumps the object,
    // its entries (pointer, vtable) and the buffer's allocator header (issue #4).
    void listWatch(uint8_t *ram)
    {
        static const char *spec = std::getenv("KZ_LISTWATCH");
        if (!spec)
            return;
        static unsigned vt = 0, co = 0x38, dof = 0x40;
        static std::vector<uint32_t> objs;
        static std::map<uint32_t, int> reported;
        static unsigned pass = 0;
        if (pass == 0)
            std::sscanf(spec, "%x,%x,%x", &vt, &co, &dof);
        static double nextScan = 100.0; // wall seconds; the 32 MB scan is too slow to run in the menus
        if (pass++ == 0 || nowSec() < 100.0)
            return;
        if (nowSec() >= nextScan)
        {
            nextScan = nowSec() + 3.0;
            objs.clear();
            for (uint32_t a = 0x100000; a + 0x100 < 0x02000000u; a += 4)
                if (rdw(ram, a) == vt)
                    objs.push_back(a);
        }
        if (std::getenv("KZ_PAGEWATCH") && nowSec() >= (std::getenv("KZ_PAGEWATCH_T") ? std::atof(std::getenv("KZ_PAGEWATCH_T")) : 130.0))
        {
            uint32_t best = 0, bestCnt = 0;
            for (const uint32_t o : objs)
                if (rdw(ram, o) == vt && rdw(ram, o + co) > bestCnt && rdw(ram, o + co) < 10000)
                {
                    best = o;
                    bestCnt = rdw(ram, o + co);
                }
            if (best)
            {
                const uint32_t d = rdw(ram, best + dof), cap = rdw(ram, best + co + 4);
                if (d >= 0x100000 && d < 0x02000000u && cap >= bestCnt && cap < 10000)
                    pageWatchSet(ram, d, cap * 4);
            }
        }
        for (const uint32_t o : objs)
        {
            if (rdw(ram, o) != vt)
                continue;
            const uint32_t cnt = rdw(ram, o + co), data = rdw(ram, o + dof);
            if (cnt == 0 || cnt > 10000 || data < 0x100000 || data >= 0x02000000u)
                continue;
            int nulls = 0;
            for (uint32_t i = 0; i < cnt; ++i)
                nulls += rdw(ram, data + i * 4) == 0;
            if (!nulls || reported[o]++ > 2)
                continue;
            std::fprintf(stderr, "[listwatch] t=%.2f pass=%u obj=%x count=%u cap=%u data=%x nulls=%d chunkhdr[-8]=%x [-4]=%x [-0]=%x\n", nowSec(), pass, o, cnt, rdw(ram, o + co + 4), data, nulls, rdw(ram, data - 8), rdw(ram, data - 4), rdw(ram, data));
            std::fprintf(stderr, "  obj:");
            for (uint32_t w = 0; w < 0x50; w += 4)
                std::fprintf(stderr, " %x", rdw(ram, o + w));
            std::fprintf(stderr, "\n");
            for (uint32_t i = 0; i < cnt; ++i)
            {
                const uint32_t e = rdw(ram, data + i * 4);
                std::fprintf(stderr, "  [%u]=%x vt=%x\n", i, e, e ? rdw(ram, e) : 0u);
            }
        }
    }
}

namespace
{
    // KZ_VECWATCH="vtable[,count_off,data_off]" (hex vtable; offsets default 0x38 / 0x40): twice a second finds every object whose
    // first word is that vtable and reports its pointer vector (count, data) whenever the number of NULL entries changes (issue #4).
    void vecWatch(uint8_t *ram, double t)
    {
        static const char *spec = std::getenv("KZ_VECWATCH");
        static uint32_t vt = 0, co = 0x38, dof = 0x40;
        static bool init = false;
        static double next = 0.0;
        if (!init)
        {
            init = true;
            unsigned a = 0, b = 0x38, c = 0x40;
            if (spec)
                std::sscanf(spec, "%x,%x,%x", &a, &b, &c);
            vt = a; co = b; dof = c;
        }
        if (!spec || !vt || t < next)
            return;
        next = t + 0.25;
        static std::map<uint32_t, uint64_t> lastNulls;
        for (uint32_t a = 0x100000; a + 0x100 < 0x02000000u; a += 4)
            if (rdw(ram, a) == vt)
            {
                const uint32_t cnt = rdw(ram, a + co), data = rdw(ram, a + dof);
                int nulls = 0;
                int first = -1;
                if (cnt < 100000 && data >= 0x100000 && data < 0x02000000u)
                    for (uint32_t i = 0; i < cnt; ++i)
                        if (rdw(ram, data + i * 4) == 0)
                        {
                            ++nulls;
                            if (first < 0)
                                first = static_cast<int>(i);
                        }
                const uint32_t ex1 = rdw(ram, a + 0x1d0), ex2 = rdw(ram, a + 0x1dc);
                const uint64_t key = (static_cast<uint64_t>(nulls) << 48) ^ (static_cast<uint64_t>(cnt) << 32) ^ (static_cast<uint64_t>(ex1) << 16) ^ ex2 ^ 0x1000000000000000ull;
                auto it = lastNulls.find(a);
                if (it == lastNulls.end() || it->second != key)
                {
                    std::fprintf(stderr, "[vecwatch] t=%.2f obj=%x count=%u data=%x nulls=%d first=%d  +1d0=%x +1dc=%x\n", t, a, cnt, data, nulls, first, ex1, ex2);
                    lastNulls[a] = key;
                }
            }
    }
}

void kzDevOnVsync(uint8_t *rdram)
{
    static const bool any = std::getenv("KZ_GOD") || std::getenv("KZ_PLAYERPOKE") || std::getenv("KZ_POKE") || std::getenv("KZ_PEEK") || std::getenv("KZ_TELEPORT") || std::getenv("KZ_ACTORS") || std::getenv("KZ_KILLALL") || std::getenv("KZ_VECWATCH");
    static const double actorPeriod = std::getenv("KZ_ACTORS") ? std::atof(std::getenv("KZ_ACTORS")) : 0.0;
    static double nextActors = 0.0;
    if (!any || !rdram)
        return;
    const double t = nowSec();
    vecWatch(rdram, t);
    if (!pps().empty())
        if (const uint32_t pl = findPlayer(rdram))
            for (const PP &p : pps())
                if (t >= p.t)
                    std::memcpy(rdram + ((pl + p.off) & kRamMask), &p.value, 4);
    if (const char *k = std::getenv("KZ_KILLALL"); k && !std::getenv("KZ_KILLALL_SYNC"))
    {
        static const double start = std::atof(k);
        static const double period = [k]() { const char *c = std::strchr(k, ','); return c ? std::atof(c + 1) : 2.0; }();
        static double nextKill = start;
        if (t >= nextKill)
        {
            nextKill = t + period;
            killAll(rdram);
        }
    }
    if (actorPeriod > 0.0 && t >= nextActors)
    {
        nextActors = t + actorPeriod;
        logActors(rdram, t);
    }
    for (Tele &te : teles())
    {
        if (t < te.t || te.done)
            continue;
        if (const uint32_t c = findCtl(rdram))
        {
            te.done = true;
            std::memcpy(rdram + ((c + 0x50) & kRamMask), &te.x, 4);
            std::memcpy(rdram + ((c + 0x54) & kRamMask), &te.y, 4);
            std::memcpy(rdram + ((c + 0x58) & kRamMask), &te.z, 4);
            std::fprintf(stderr, "[teleport] t=%.2f ctl=%06x -> %.2f %.2f %.2f\n", t, c, te.x, te.y, te.z);
        }
    }
    for (Poke &p : pokes())
    {
        if (t < p.t || (p.done && !p.repeat))
            continue;
        p.done = true;
        std::memcpy(rdram + (p.addr & kRamMask), &p.value, static_cast<size_t>(p.size));
    }
    for (Peek &p : peeks())
    {
        if (t < p.t || p.done)
            continue;
        p.done = true;
        std::fprintf(stderr, "[peek] t=%.2f %06x:", t, p.addr);
        for (int i = 0; i < p.words; ++i)
        {
            uint32_t v;
            std::memcpy(&v, rdram + ((p.addr + i * 4) & kRamMask), 4);
            std::fprintf(stderr, " %08x", v);
        }
        std::fprintf(stderr, "\n");
    }
}
