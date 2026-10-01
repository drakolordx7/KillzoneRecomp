#include "kz_parity.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    constexpr uint32_t kRamSize = 0x02000000u;
    constexpr uint32_t kPlayerVtable = 0x525710u; // player entity
    constexpr uint32_t kCtlVtable = 0x524B50u;    // character controller (player+0x31C)
    constexpr uint32_t kWeaponVtable = 0x52BD20u; // weapon (+0xC owner ctl, +0x180 magazine, +0x184 reserve, +0xE0 state)

    struct Word
    {
        char base;                   // p player, c ctl, g game object (*0x559178), w rifle weapon, a absolute
        std::vector<uint32_t> chain; // first = offset from base, the rest = offsets after dereferencing
        std::string text;
    };

    bool g_init = false;
    bool g_on = false;
    FILE *g_file = nullptr;
    std::vector<Word> g_words;
    std::vector<double> g_dumps;
    size_t g_nextDump = 0;
    std::string g_dumpPrefix;
    double g_start = 0.0;
    std::chrono::steady_clock::time_point g_t0;
    uint32_t g_player = 0;
    uint32_t g_weapon = 0; // the rifle: weapon object (vtable 0x52BD20, owner = ctl) with the most reserve ammo
    std::chrono::steady_clock::time_point g_nextScan{};
    std::chrono::steady_clock::time_point g_nextWeaponScan{};

    uint32_t rd32(const uint8_t *ram, uint32_t a)
    {
        a &= 0x01FFFFFFu;
        uint32_t v = 0;
        if (a + 4 <= kRamSize)
            std::memcpy(&v, ram + a, 4);
        return v;
    }

    bool valid(uint32_t p)
    {
        return p >= 0x00100000u && p < kRamSize - 0x1000u;
    }

    void init()
    {
        g_init = true;
        const char *path = std::getenv("KZ_PARITY_LOG");
        if (!path || !*path)
            return;
        g_file = std::fopen(path, "w");
        if (!g_file)
            return;
        g_on = true;
        g_dumpPrefix = path;
        if (const char *s = std::getenv("KZ_PARITY_START"))
            g_start = std::atof(s);
        if (const char *d = std::getenv("KZ_PARITY_DUMP"))
        {
            for (const char *p = d; *p;)
            {
                g_dumps.push_back(std::atof(p));
                while (*p && *p != ',')
                    ++p;
                if (*p == ',')
                    ++p;
            }
        }
        if (const char *w = std::getenv("KZ_PARITY_WORDS"))
        {
            // "p:0x148,c:0x454,a:0x55a6e0,p:0x31c>0x104" (offsets after '>' follow a pointer)
            std::string all = w;
            size_t pos = 0;
            while (pos < all.size())
            {
                size_t end = all.find(',', pos);
                if (end == std::string::npos)
                    end = all.size();
                std::string item = all.substr(pos, end - pos);
                pos = end + 1;
                if (item.size() < 3 || item[1] != ':')
                    continue;
                Word word;
                word.base = item[0];
                word.text = item;
                size_t q = 2;
                while (q <= item.size())
                {
                    size_t e = item.find('>', q);
                    if (e == std::string::npos)
                        e = item.size();
                    word.chain.push_back(static_cast<uint32_t>(std::strtoul(item.substr(q, e - q).c_str(), nullptr, 0)));
                    q = e + 1;
                }
                g_words.push_back(word);
            }
        }
        std::fprintf(g_file, "wall_s,vsync,now,elapsed,sec_per_tick,player,ctl,x,y,z,heading");
        for (const Word &w : g_words)
            std::fprintf(g_file, ",%s", w.text.c_str());
        std::fprintf(g_file, "\n");
        g_t0 = std::chrono::steady_clock::now();
    }

    // One 32 MB sweep for the player entity and the rifle, spread over many vblanks (128 K words each, ~0.15 ms) so the
    // vblank thread is never held up long enough to disturb the timing being measured. A finished sweep that found nothing
    // starts over; a found object is re-verified every vblank by the caller.
    constexpr uint32_t kChunkWords = 128 * 1024;
    uint32_t g_sweepPos = 0x100000u / 4;
    uint32_t g_sweepPlayer = 0, g_sweepBest = 0, g_sweepBestReserve = 0;

    void sweep(const uint8_t *ram, uint32_t ctl)
    {
        const uint32_t *w = reinterpret_cast<const uint32_t *>(ram);
        const uint32_t end = std::min<uint32_t>(g_sweepPos + kChunkWords, kRamSize / 4 - 0x300);
        for (uint32_t i = g_sweepPos; i < end; ++i)
        {
            const uint32_t v = w[i];
            if (v == kPlayerVtable && !g_sweepPlayer)
            {
                const uint32_t c = rd32(ram, i * 4 + 0x31C);
                if (valid(c) && rd32(ram, c) == kCtlVtable)
                    g_sweepPlayer = i * 4;
            }
            else if (v == kWeaponVtable && ctl && w[i + 3] == ctl)
            {
                const uint32_t reserve = w[i + 0x184 / 4];
                if (reserve >= g_sweepBestReserve && reserve < 100000u)
                {
                    g_sweepBestReserve = reserve;
                    g_sweepBest = i * 4;
                }
            }
        }
        g_sweepPos = end;
        if (g_sweepPos >= kRamSize / 4 - 0x300)
        {
            if (g_sweepPlayer)
                g_player = g_sweepPlayer;
            if (g_sweepBest)
                g_weapon = g_sweepBest;
            g_sweepPos = 0x100000u / 4;
            g_sweepPlayer = g_sweepBest = g_sweepBestReserve = 0;
        }
    }
}

void kzParityOnVsync(uint8_t *ram)
{
    if (!g_init)
        init();
    if (!g_on || !ram)
        return;
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
    while (g_nextDump < g_dumps.size() && wall >= g_dumps[g_nextDump])
    {
        const std::string path = g_dumpPrefix + "." + std::to_string(static_cast<int>(g_dumps[g_nextDump])) + ".ram";
        if (FILE *f = std::fopen(path.c_str(), "wb"))
        {
            std::fwrite(ram, 1, kRamSize, f);
            std::fclose(f);
        }
        ++g_nextDump;
    }
    if (wall < g_start)
        return;
    if (g_player && rd32(ram, g_player) != kPlayerVtable)
        g_player = 0;
    if (g_weapon && rd32(ram, g_weapon) != kWeaponVtable)
        g_weapon = 0;
    const uint32_t obj = rd32(ram, 0x00559178u);
    uint32_t ctl = 0;
    if (g_player)
        ctl = rd32(ram, g_player + 0x31C);
    if (!valid(ctl))
        ctl = 0;
    sweep(ram, ctl); // incremental: refreshes the player and the rifle once per sweep
    float x = 0, y = 0, z = 0, h = 0, spt = 0;
    uint32_t el = 0, nowTick = 0;
    if (valid(obj))
    {
        const uint32_t s = rd32(ram, obj + 0x50);
        std::memcpy(&spt, &s, 4);
        el = rd32(ram, obj + 0x64);
        nowTick = rd32(ram, obj + 0x68);
    }
    if (ctl)
    {
        const uint32_t v[4] = {rd32(ram, ctl + 0x50), rd32(ram, ctl + 0x54), rd32(ram, ctl + 0x58), rd32(ram, ctl + 0x78)};
        std::memcpy(&x, &v[0], 4);
        std::memcpy(&y, &v[1], 4);
        std::memcpy(&z, &v[2], 4);
        std::memcpy(&h, &v[3], 4);
    }
    std::fprintf(g_file, "%.5f,%u,%u,%u,%.7f,%x,%x,%.4f,%.4f,%.4f,%.5f", wall, rd32(ram, 0x0055A6E0u), nowTick, el, spt, g_player,
                 ctl, x, y, z, h);
    for (const Word &w : g_words)
    {
        uint32_t base = w.base == 'p' ? g_player : w.base == 'c' ? ctl : w.base == 'g' ? obj : w.base == 'w' ? g_weapon : 0;
        uint32_t a = base + w.chain[0];
        for (size_t i = 1; i < w.chain.size(); ++i)
        {
            a = rd32(ram, a);
            if (!valid(a))
            {
                a = 0;
                break;
            }
            a += w.chain[i];
        }
        std::fprintf(g_file, ",%x", a ? rd32(ram, a) : 0u);
    }
    std::fprintf(g_file, "\n");
    static int n = 0;
    if ((++n & 255) == 0)
        std::fflush(g_file);
}
