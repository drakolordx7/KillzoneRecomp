#include "kz_devtools.h"

#include "ps2_runtime.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
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
    void killAll(uint8_t *ram)
    {
        const uint32_t pl = findPlayer(ram);
        int n = 0;
        const uint32_t zero = 0;
        for (uint32_t a = 0x100000; a + 0x320 < 0x02000000u; a += 4)
            if (a != pl && isChar(ram, a))
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

void kzDevOnVsync(uint8_t *rdram)
{
    static const bool any = std::getenv("KZ_GOD") || std::getenv("KZ_PLAYERPOKE") || std::getenv("KZ_POKE") || std::getenv("KZ_PEEK") || std::getenv("KZ_TELEPORT") || std::getenv("KZ_ACTORS") || std::getenv("KZ_KILLALL");
    static const double actorPeriod = std::getenv("KZ_ACTORS") ? std::atof(std::getenv("KZ_ACTORS")) : 0.0;
    static double nextActors = 0.0;
    if (!any || !rdram)
        return;
    const double t = nowSec();
    if (!pps().empty())
        if (const uint32_t pl = findPlayer(rdram))
            for (const PP &p : pps())
                if (t >= p.t)
                    std::memcpy(rdram + ((pl + p.off) & kRamMask), &p.value, 4);
    if (const char *k = std::getenv("KZ_KILLALL"))
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
