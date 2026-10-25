#include "kz_prompts.h"

#include "kz_config.h"
#include "kz_input.h"

#include "ps2_runtime.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// Keyboard and mouse button prompts (docs/findings.md "Button prompts").
//
// How the game draws a button picture: the text markup <buttonimage name=cross|triangle|...|R3> (tag handler 0x26C4F0; <buttonfunction
// function=primaryfire|...> resolves to the same through the player's controller map) appends ONE character, code 32 + button index, to the
// text and draws it through the normal text routine 0x167718 with a special "button font" (a font object whose glyph cells are the pictures of
// one 256x64 8-bit atlas, 20 cells of 20..25 x 28 pixels: cross triangle square circle left right up down L1 L2 L3 R1 R2 R3 updown ...).
// The font's texture object T (font+68) points at the atlas the way the game stores every texture: pre-built upload packets in RAM that the
// texture streamer sends through the frame's DMA chain whenever the texture is not resident in the VRAM ring any more:
//   T+16  palette resource: +4 byte qword count (0x40), +12 VRAM cookie, +16 the 16x16 RGBA palette (1 KB, CSM1 order)
//   T+32  number of images (1)            T+40  list of {RAM address, qword count} chunks of the image   T+44  VRAM cookie
//   T+48  transfer size 0x20080 (128x32 PSMCT32: the 256x64 PSMT8 atlas pre-swizzled into GS memory order)
// The streamer (0x15A5E8) treats a cookie outside the ring window as "not resident" and uploads the RAM data again at the next draw.
// While the last input was keyboard/mouse the atlas and palette in RAM are therefore replaced by a version whose cells hold the key or
// mouse button bound to that action and the cookies are cleared, so the very draw that needs the picture uploads the new one. Switching
// back restores the original bytes. The cell layout comes from the font's own glyph table, so every font that uses the atlas works.
namespace
{
    constexpr uint32_t kRamMask = 0x01FFFFFFu;
    constexpr uint32_t kText = 0x00167718u;
    constexpr uint32_t kAtlasCrc = 0xD22EB2FDu;    // the stock button atlas (256x64 PSMT8, pre-swizzled)
    constexpr uint32_t kPaletteCrc = 0x062BBA16u;  // and its palette
    constexpr uint32_t kAtlasBytes = 16384, kPaletteBytes = 1024;
    constexpr int kAtlasW = 256, kAtlasH = 64;
    constexpr int kButtons = 20; // glyph cells in the atlas (codes 32..51)

    PS2Runtime::RecompiledFunction g_origText = nullptr;
    bool g_log = false;
    bool g_enabled = true;
    int g_forceMode = -1; // KZ_PROMPT_MODE=kbm|pad forces the device (automation)

    uint32_t rd32(const uint8_t *rdram, uint32_t a)
    {
        uint32_t v;
        std::memcpy(&v, rdram + (a & kRamMask), 4);
        return v;
    }
    uint16_t rd16(const uint8_t *rdram, uint32_t a)
    {
        uint16_t v;
        std::memcpy(&v, rdram + (a & kRamMask), 2);
        return v;
    }
    void wr32(uint8_t *rdram, uint32_t a, uint32_t v) { std::memcpy(rdram + (a & kRamMask), &v, 4); }

    // The streamer (0x15A5E8) calls a texture resident when its VRAM cookie (ring position | ring number 2 or 3) lies within 64 KB below that ring's
    // head. Position 0 of ring 2 or 3 is far below the head, so the texture is uploaded again at its next use. (Cookie 0 means ring 0, whose
    // head is small: the texture then counts as resident and keeps sampling VRAM address 0.)
    void evict(uint8_t *rdram, uint32_t cookieAddr)
    {
        uint32_t ring = rd32(rdram, cookieAddr) & 3u;
        if (ring < 2)
            ring = 3;
        wr32(rdram, cookieAddr, ring);
    }
    bool ramPtr(uint32_t a) { return a >= 0x00100000u && a < 0x02000000u; }

    uint32_t crc32(const uint8_t *p, size_t n)
    {
        static uint32_t table[256];
        static bool init = false;
        if (!init)
        {
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                table[i] = c;
            }
            init = true;
        }
        uint32_t c = 0xFFFFFFFFu;
        for (size_t i = 0; i < n; ++i)
            c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }

    // ---- GS memory layout (tables from PCSX2's GSTables.cpp) ----
    // The atlas is a PSMT8 texture 256 wide (TBW 4). The game uploads it as a PSMCT32 image 128x32 (DBW 2): both views address the same
    // 16 KB of GS memory, so the uploaded stream is the PSMT8 pixels run through the PSMT8 address function and read back through PSMCT32's.
    const uint8_t kBlock32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23}, {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
    const uint8_t kCol32[8][8] = {{0, 1, 4, 5, 8, 9, 12, 13}, {2, 3, 6, 7, 10, 11, 14, 15}, {16, 17, 20, 21, 24, 25, 28, 29}, {18, 19, 22, 23, 26, 27, 30, 31},
                                  {32, 33, 36, 37, 40, 41, 44, 45}, {34, 35, 38, 39, 42, 43, 46, 47}, {48, 49, 52, 53, 56, 57, 60, 61}, {50, 51, 54, 55, 58, 59, 62, 63}};
    const uint8_t kCol8[16][16] = {
        {0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54},         {8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62},
        {33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23},         {41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31},
        {96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86}, {104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94},
        {65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119}, {73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127},
        {128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182}, {136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190},
        {161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151}, {169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159},
        {224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214}, {232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222},
        {193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247}, {201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255}};

    uint32_t addr8(int x, int y) // byte address of PSMT8 pixel (x, y) in a 256 wide texture
    {
        const uint32_t page = static_cast<uint32_t>((y / 64) * 2 + x / 128);
        return page * 8192u + kBlock32[(y / 16) % 4][(x / 16) % 8] * 256u + kCol8[y % 16][x % 16];
    }
    uint32_t word32(int x, int y) // word index of PSMCT32 pixel (x, y) in a 128 wide image (DBW 2)
    {
        const uint32_t page = static_cast<uint32_t>((y / 32) * 2 + x / 64);
        return page * 2048u + kBlock32[(y / 8) % 4][(x / 8) % 8] * 64u + kCol32[y % 8][x % 8];
    }
    // palette entry i sits at slot i with bits 3 and 4 swapped (CSM1)
    int clutSlot(int i) { return (i & ~0x18) | ((i & 8) << 1) | ((i & 16) >> 1); }

    void streamToIndices(const uint8_t *stream, uint8_t *idx) // 16384 bytes of upload stream -> 256*64 indices
    {
        uint8_t mem[kAtlasBytes];
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 128; ++x)
                std::memcpy(mem + word32(x, y) * 4, stream + (y * 128 + x) * 4, 4);
        for (int y = 0; y < kAtlasH; ++y)
            for (int x = 0; x < kAtlasW; ++x)
                idx[y * kAtlasW + x] = mem[addr8(x, y)];
    }
    void indicesToStream(const uint8_t *idx, uint8_t *stream)
    {
        uint8_t mem[kAtlasBytes];
        for (int y = 0; y < kAtlasH; ++y)
            for (int x = 0; x < kAtlasW; ++x)
                mem[addr8(x, y)] = idx[y * kAtlasW + x];
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 128; ++x)
                std::memcpy(stream + (y * 128 + x) * 4, mem + word32(x, y) * 4, 4);
    }

    // ---- images ----
    struct Px
    {
        uint8_t r = 0, g = 0, b = 0, a = 0; // straight alpha, a 0..255
    };
    struct Image
    {
        int w = 0, h = 0;
        std::vector<Px> px;
        Image() = default;
        Image(int w_, int h_) : w(w_), h(h_), px(static_cast<size_t>(w_) * h_) {}
        Px &at(int x, int y) { return px[static_cast<size_t>(y) * w + x]; }
        const Px &at(int x, int y) const { return px[static_cast<size_t>(y) * w + x]; }
    };

    // Premultiplied "over": dst = src over dst.
    void blendOver(Px &dst, float sr, float sg, float sb, float sa)
    {
        const float da = dst.a / 255.0f;
        const float oa = sa + da * (1.0f - sa);
        if (oa <= 0.0f)
        {
            dst = Px{};
            return;
        }
        auto ch = [&](float s, uint8_t d) { return std::clamp((s * sa + d * da * (1.0f - sa)) / oa, 0.0f, 255.0f); };
        dst.r = static_cast<uint8_t>(ch(sr, dst.r) + 0.5f);
        dst.g = static_cast<uint8_t>(ch(sg, dst.g) + 0.5f);
        dst.b = static_cast<uint8_t>(ch(sb, dst.b) + 0.5f);
        dst.a = static_cast<uint8_t>(std::clamp(oa * 255.0f, 0.0f, 255.0f) + 0.5f);
    }

    // Area-average resample (premultiplied) of src into an n x n image.
    Image resample(const Image &src, int n)
    {
        Image out(n, n);
        const float sx = static_cast<float>(src.w) / n, sy = static_cast<float>(src.h) / n;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
            {
                const float x0 = x * sx, x1 = (x + 1) * sx, y0 = y * sy, y1 = (y + 1) * sy;
                float r = 0, g = 0, b = 0, a = 0, wsum = 0;
                for (int yy = static_cast<int>(y0); yy < std::min(src.h, static_cast<int>(std::ceil(y1))); ++yy)
                    for (int xx = static_cast<int>(x0); xx < std::min(src.w, static_cast<int>(std::ceil(x1))); ++xx)
                    {
                        const float wx = std::min<float>(xx + 1, x1) - std::max<float>(xx, x0);
                        const float wy = std::min<float>(yy + 1, y1) - std::max<float>(yy, y0);
                        const float wgt = wx * wy;
                        const Px &p = src.at(xx, yy);
                        const float pa = p.a / 255.0f;
                        r += p.r * pa * wgt;
                        g += p.g * pa * wgt;
                        b += p.b * pa * wgt;
                        a += pa * wgt;
                        wsum += wgt;
                    }
                Px &o = out.at(x, y);
                if (a > 0.0f && wsum > 0.0f)
                {
                    o.r = static_cast<uint8_t>(std::clamp(r / a, 0.0f, 255.0f) + 0.5f);
                    o.g = static_cast<uint8_t>(std::clamp(g / a, 0.0f, 255.0f) + 0.5f);
                    o.b = static_cast<uint8_t>(std::clamp(b / a, 0.0f, 255.0f) + 0.5f);
                    o.a = static_cast<uint8_t>(std::clamp(a / wsum, 0.0f, 1.0f) * 255.0f + 0.5f);
                }
            }
        return out;
    }

    std::string resourceDir()
    {
        static std::string dir = []() {
            const char *base = SDL_GetBasePath();
            return std::string(base ? base : "") + "resources/prompts/";
        }();
        return dir;
    }

    bool loadIcon(const std::string &name, Image &out)
    {
        const std::string path = resourceDir() + name + ".png";
        SDL_Surface *s = SDL_LoadPNG(path.c_str());
        if (!s)
            return false;
        SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(s);
        if (!c)
            return false;
        out = Image(c->w, c->h);
        for (int y = 0; y < c->h; ++y)
            std::memcpy(&out.px[static_cast<size_t>(y) * c->w], static_cast<const uint8_t *>(c->pixels) + static_cast<size_t>(y) * c->pitch, static_cast<size_t>(c->w) * 4);
        SDL_DestroySurface(c);
        return true;
    }

    // The key pictures are white shapes with the letter cut out. Under the picture goes a dark backing (the filled silhouette, grown by one
    // pixel) so the letter reads dark and the key keeps a dark rim on bright backgrounds.
    Image makeKey(const Image &src, int n)
    {
        // silhouette at full resolution: everything the outside cannot reach through transparent pixels
        Image sil(src.w, src.h);
        {
            std::vector<uint8_t> outside(static_cast<size_t>(src.w) * src.h, 0);
            std::vector<int> stack;
            auto push = [&](int x, int y) {
                if (x < 0 || y < 0 || x >= src.w || y >= src.h)
                    return;
                const size_t i = static_cast<size_t>(y) * src.w + x;
                if (outside[i] || src.px[i].a > 40)
                    return;
                outside[i] = 1;
                stack.push_back(static_cast<int>(i));
            };
            for (int x = 0; x < src.w; ++x)
                push(x, 0), push(x, src.h - 1);
            for (int y = 0; y < src.h; ++y)
                push(0, y), push(src.w - 1, y);
            while (!stack.empty())
            {
                const int i = stack.back();
                stack.pop_back();
                const int x = i % src.w, y = i / src.w;
                push(x + 1, y), push(x - 1, y), push(x, y + 1), push(x, y - 1);
            }
            for (size_t i = 0; i < sil.px.size(); ++i)
                sil.px[i] = outside[i] ? Px{} : Px{0, 0, 0, 255};
        }
        const int inner = n - 2;
        const Image icon = resample(src, inner);
        const Image shape = resample(sil, inner);
        Image out(n, n);
        // dark backing: silhouette dilated by one pixel
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
            {
                float a = 0.0f;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        if (dx && dy)
                            continue; // plus-shaped
                        const int sx = x - 1 + dx, sy = y - 1 + dy;
                        if (sx >= 0 && sy >= 0 && sx < inner && sy < inner)
                            a = std::max(a, shape.at(sx, sy).a / 255.0f);
                    }
                if (a > 0.0f)
                    blendOver(out.at(x, y), 14.0f, 14.0f, 18.0f, a * 0.92f);
            }
        for (int y = 0; y < inner; ++y)
            for (int x = 0; x < inner; ++x)
            {
                const Px &p = icon.at(x, y);
                if (p.a)
                    blendOver(out.at(x + 1, y + 1), p.r, p.g, p.b, p.a / 255.0f);
            }
        return out;
    }

    // ---- palette quantiser (median cut over straight RGBA, index 0 reserved for fully transparent) ----
    struct Colour
    {
        int c[4];
        uint32_t count;
    };
    void quantise(const Image &img, uint8_t *indices, Px *palette /*256*/)
    {
        std::map<uint32_t, uint32_t> hist;
        for (const Px &p : img.px)
            if (p.a)
                ++hist[static_cast<uint32_t>(p.r) | (p.g << 8) | (p.b << 16) | (static_cast<uint32_t>(p.a) << 24)];
        std::vector<Colour> cols;
        for (const auto &[k, n] : hist)
            cols.push_back({{static_cast<int>(k & 255), static_cast<int>((k >> 8) & 255), static_cast<int>((k >> 16) & 255), static_cast<int>(k >> 24)}, n});
        struct Box
        {
            std::vector<Colour> v;
        };
        std::vector<Box> boxes(1);
        boxes[0].v = cols;
        while (boxes.size() < 255)
        {
            size_t best = SIZE_MAX;
            double bestScore = 0.0;
            int bestCh = 0;
            for (size_t i = 0; i < boxes.size(); ++i)
            {
                if (boxes[i].v.size() < 2)
                    continue;
                for (int ch = 0; ch < 4; ++ch)
                {
                    int lo = 255, hi = 0;
                    for (const Colour &c : boxes[i].v)
                        lo = std::min(lo, c.c[ch]), hi = std::max(hi, c.c[ch]);
                    uint64_t n = 0;
                    for (const Colour &c : boxes[i].v)
                        n += c.count;
                    const double score = static_cast<double>(hi - lo) * (ch == 3 ? 1.3 : 1.0) * std::sqrt(static_cast<double>(n));
                    if (score > bestScore)
                        bestScore = score, best = i, bestCh = ch;
                }
            }
            if (best == SIZE_MAX || bestScore <= 0.0)
                break;
            Box &b = boxes[best];
            std::sort(b.v.begin(), b.v.end(), [&](const Colour &x, const Colour &y) { return x.c[bestCh] < y.c[bestCh]; });
            uint64_t total = 0;
            for (const Colour &c : b.v)
                total += c.count;
            uint64_t acc = 0;
            size_t cut = 1;
            for (size_t i = 0; i < b.v.size(); ++i)
            {
                acc += b.v[i].count;
                if (acc * 2 >= total)
                {
                    cut = std::clamp<size_t>(i + 1, 1, b.v.size() - 1);
                    break;
                }
            }
            Box nb;
            nb.v.assign(b.v.begin() + static_cast<long>(cut), b.v.end());
            b.v.resize(cut);
            boxes.push_back(std::move(nb));
        }
        palette[0] = Px{};
        size_t n = 1;
        for (const Box &b : boxes)
        {
            double sum[4] = {0, 0, 0, 0}, w = 0;
            for (const Colour &c : b.v)
            {
                const double a = c.c[3] / 255.0;
                sum[0] += c.c[0] * a * c.count, sum[1] += c.c[1] * a * c.count, sum[2] += c.c[2] * a * c.count, sum[3] += c.c[3] * static_cast<double>(c.count);
                w += c.count * a;
            }
            uint64_t cnt = 0;
            for (const Colour &c : b.v)
                cnt += c.count;
            if (!cnt)
                continue;
            Px p;
            const double a = sum[3] / cnt;
            p.a = static_cast<uint8_t>(a + 0.5);
            p.r = static_cast<uint8_t>(w > 0 ? sum[0] / w + 0.5 : 0);
            p.g = static_cast<uint8_t>(w > 0 ? sum[1] / w + 0.5 : 0);
            p.b = static_cast<uint8_t>(w > 0 ? sum[2] / w + 0.5 : 0);
            palette[n++] = p;
        }
        for (; n < 256; ++n)
            palette[n] = Px{};
        // nearest entry per distinct colour
        std::unordered_map<uint32_t, uint8_t> cache;
        for (size_t i = 0; i < img.px.size(); ++i)
        {
            const Px &p = img.px[i];
            if (!p.a)
            {
                indices[i] = 0;
                continue;
            }
            const uint32_t key = static_cast<uint32_t>(p.r) | (p.g << 8) | (p.b << 16) | (static_cast<uint32_t>(p.a) << 24);
            auto it = cache.find(key);
            if (it != cache.end())
            {
                indices[i] = it->second;
                continue;
            }
            int bestIdx = 1;
            double bestD = 1e30;
            for (int k = 1; k < 256; ++k)
            {
                const Px &q = palette[k];
                if (!q.a)
                    continue;
                const double dr = p.r - q.r, dg = p.g - q.g, db = p.b - q.b, da = p.a - q.a;
                const double d = (dr * dr + dg * dg + db * db) * (p.a / 255.0) + da * da * 1.5;
                if (d < bestD)
                    bestD = d, bestIdx = k;
            }
            cache[key] = static_cast<uint8_t>(bestIdx);
            indices[i] = static_cast<uint8_t>(bestIdx);
        }
    }

    // ---- bindings -> pictures ----
    struct Bindings
    {
        std::vector<std::pair<std::string, std::string>> pairs; // source name, target name
        bool loaded = false;
    };
    Bindings g_bindings;

    // The first source bound to the pad target (file order).
    std::string sourceFor(const char *target)
    {
        if (!g_bindings.loaded)
        {
            g_bindings.pairs = kzInputBindingPairs(kzConfigPath());
            g_bindings.loaded = true;
        }
        for (const auto &[src, dst] : g_bindings.pairs)
            if (_stricmp(dst.c_str(), target) == 0)
                return src;
        return {};
    }

    std::string iconForSource(const std::string &src)
    {
        if (src.empty())
            return {};
        if (_strnicmp(src.c_str(), "Mouse", 5) == 0 && src.size() == 6)
        {
            static const char *names[] = {"", "mouse_left", "mouse_right", "mouse_scroll", "mouse_side_back", "mouse_side_forward"};
            if (src[5] >= '1' && src[5] <= '5')
                return names[src[5] - '0'];
        }
        if (_stricmp(src.c_str(), "WheelUp") == 0)
            return "mouse_scroll_up";
        if (_stricmp(src.c_str(), "WheelDown") == 0)
            return "mouse_scroll_down";
        const SDL_Scancode sc = SDL_GetScancodeFromName(src.c_str());
        if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
            return std::string("keyboard_") + static_cast<char>('a' + (sc - SDL_SCANCODE_A));
        if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
            return std::string("keyboard_") + static_cast<char>('1' + (sc - SDL_SCANCODE_1));
        if (sc == SDL_SCANCODE_0)
            return "keyboard_0";
        if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12)
            return "keyboard_f" + std::to_string(sc - SDL_SCANCODE_F1 + 1);
        switch (sc)
        {
        case SDL_SCANCODE_UP: return "keyboard_arrow_up";
        case SDL_SCANCODE_DOWN: return "keyboard_arrow_down";
        case SDL_SCANCODE_LEFT: return "keyboard_arrow_left";
        case SDL_SCANCODE_RIGHT: return "keyboard_arrow_right";
        case SDL_SCANCODE_SPACE: return "keyboard_space_icon";
        case SDL_SCANCODE_LSHIFT:
        case SDL_SCANCODE_RSHIFT: return "keyboard_shift_icon";
        case SDL_SCANCODE_LCTRL:
        case SDL_SCANCODE_RCTRL: return "keyboard_ctrl";
        case SDL_SCANCODE_LALT:
        case SDL_SCANCODE_RALT: return "keyboard_alt";
        case SDL_SCANCODE_TAB: return "keyboard_tab_icon";
        case SDL_SCANCODE_ESCAPE: return "keyboard_escape";
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER: return "keyboard_return";
        case SDL_SCANCODE_BACKSPACE: return "keyboard_backspace_icon";
        case SDL_SCANCODE_CAPSLOCK: return "keyboard_capslock_icon";
        case SDL_SCANCODE_DELETE: return "keyboard_delete";
        case SDL_SCANCODE_INSERT: return "keyboard_insert";
        case SDL_SCANCODE_HOME: return "keyboard_home";
        case SDL_SCANCODE_END: return "keyboard_end";
        case SDL_SCANCODE_PAGEUP: return "keyboard_page_up";
        case SDL_SCANCODE_PAGEDOWN: return "keyboard_page_down";
        case SDL_SCANCODE_MINUS: return "keyboard_minus";
        case SDL_SCANCODE_EQUALS: return "keyboard_equals";
        case SDL_SCANCODE_COMMA: return "keyboard_comma";
        case SDL_SCANCODE_PERIOD: return "keyboard_period";
        case SDL_SCANCODE_SLASH: return "keyboard_slash_forward";
        case SDL_SCANCODE_BACKSLASH: return "keyboard_slash_back";
        case SDL_SCANCODE_SEMICOLON: return "keyboard_semicolon";
        case SDL_SCANCODE_APOSTROPHE: return "keyboard_apostrophe";
        case SDL_SCANCODE_GRAVE: return "keyboard_tilde";
        case SDL_SCANCODE_LEFTBRACKET: return "keyboard_bracket_open";
        case SDL_SCANCODE_RIGHTBRACKET: return "keyboard_bracket_close";
        case SDL_SCANCODE_KP_PLUS: return "keyboard_numpad_plus";
        case SDL_SCANCODE_PRINTSCREEN: return "keyboard_printscreen";
        case SDL_SCANCODE_PAUSE: return "keyboard_pause";
        case SDL_SCANCODE_SCROLLLOCK: return "keyboard_scroll_lock";
        case SDL_SCANCODE_NUMLOCKCLEAR: return "keyboard_numlock";
        default: return "keyboard_any";
        }
    }

    // Button index (glyph code - 32) -> pad target whose binding the picture shows; nullptr leaves the original picture.
    const char *targetOfButton(int idx)
    {
        static const char *t[kButtons] = {"Cross", "Triangle", "Square", "Circle", nullptr, nullptr, "Up", "Down", "L1", "L2", "L3", "R1", "R2", "R3", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
        return idx >= 0 && idx < kButtons ? t[idx] : nullptr;
    }

    // ---- the replacement atlas ----
    struct Cell
    {
        int u, v, w, h;
    };
    struct Atlas
    {
        std::array<Cell, kButtons> cells{};
        std::array<bool, kButtons> valid{};
        std::vector<uint8_t> stream;  // 16384
        std::vector<uint8_t> palette; // 1024, CSM1 order
        uint32_t streamCrc = 0, paletteCrc = 0;
    };

    std::vector<uint8_t> g_origStream, g_origPalette; // the stock atlas, taken from RAM the first time it is seen
    std::vector<Atlas> g_atlases;

    bool sameCells(const std::array<Cell, kButtons> &a, const std::array<bool, kButtons> &av, const Atlas &b)
    {
        for (int i = 0; i < kButtons; ++i)
        {
            if (av[i] != b.valid[i])
                return false;
            if (av[i] && std::memcmp(&a[i], &b.cells[i], sizeof(Cell)) != 0)
                return false;
        }
        return true;
    }

    Image decodeOriginal()
    {
        uint8_t idx[kAtlasW * kAtlasH];
        streamToIndices(g_origStream.data(), idx);
        Image img(kAtlasW, kAtlasH);
        for (int i = 0; i < kAtlasW * kAtlasH; ++i)
        {
            const uint8_t *e = &g_origPalette[static_cast<size_t>(clutSlot(idx[i])) * 4];
            Px p;
            p.r = e[0], p.g = e[1], p.b = e[2];
            p.a = e[3]; // the game's palettes use the full 0..255 range (opaque = 0xFF)
            img.px[static_cast<size_t>(i)] = p;
        }
        return img;
    }

    Atlas buildAtlas(const std::array<Cell, kButtons> &cells, const std::array<bool, kButtons> &valid)
    {
        Atlas at;
        at.cells = cells;
        at.valid = valid;
        Image img = decodeOriginal();
        std::map<std::string, Image> keyCache;
        bool changed = false;
        for (int i = 0; i < kButtons; ++i)
        {
            const char *target = targetOfButton(i);
            if (!target || !valid[i])
                continue;
            const std::string icon = iconForSource(sourceFor(target));
            Image src;
            if (icon.empty() || !loadIcon(icon, src))
            {
                std::printf("[kz] button prompts: no picture for %s (%s in resources/prompts), keeping the controller button\n", target, icon.empty() ? "no key bound" : icon.c_str());
                continue;
            }
            const Cell &c = cells[i];
            const int n = std::min(std::min(c.w, 24), c.h);
            if (n < 10)
                continue;
            const Image key = makeKey(src, n);
            for (int y = 0; y < c.h; ++y)
                for (int x = 0; x < c.w; ++x)
                    img.at(c.u + x, c.v + y) = Px{};
            const int ox = c.u + (c.w - n) / 2, oy = c.v + (c.h - n) / 2;
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x)
                    img.at(ox + x, oy + y) = key.at(x, y);
            changed = true;
            if (g_log)
                std::printf("[kz-prompt] button %d (%s) -> %s (%s), %dx%d in cell %d,%d %dx%d\n", i, target, sourceFor(target).c_str(), icon.c_str(), n, n, c.u, c.v, c.w, c.h);
        }
        if (!changed) // nothing to draw: the stock data stands in, no re-quantising (CRCs stay 0, so it never matches a texture)
        {
            at.stream = g_origStream;
            at.palette = g_origPalette;
            return at;
        }
        std::vector<uint8_t> idx(kAtlasW * kAtlasH);
        Px palette[256];
        quantise(img, idx.data(), palette);
        at.stream.assign(kAtlasBytes, 0);
        indicesToStream(idx.data(), at.stream.data());
        at.palette.assign(kPaletteBytes, 0);
        for (int i = 0; i < 256; ++i)
        {
            uint8_t *e = &at.palette[static_cast<size_t>(clutSlot(i)) * 4];
            e[0] = palette[i].r, e[1] = palette[i].g, e[2] = palette[i].b;
            e[3] = palette[i].a;
        }
        if (const char *dump = std::getenv("KZ_PROMPT_DUMP")) // debug: the atlas as the GS will see it, decoded back from the upload stream and palette
        {
            uint8_t back[kAtlasW * kAtlasH];
            streamToIndices(at.stream.data(), back);
            SDL_Surface *sf = SDL_CreateSurface(kAtlasW, kAtlasH, SDL_PIXELFORMAT_RGBA32);
            if (sf)
            {
                for (int i = 0; i < kAtlasW * kAtlasH; ++i)
                {
                    const uint8_t *e = &at.palette[static_cast<size_t>(clutSlot(back[i])) * 4];
                    uint8_t *d = static_cast<uint8_t *>(sf->pixels) + static_cast<size_t>(i / kAtlasW) * sf->pitch + (i % kAtlasW) * 4;
                    d[0] = e[0], d[1] = e[1], d[2] = e[2], d[3] = e[3];
                }
                SDL_SavePNG(sf, dump);
                SDL_DestroySurface(sf);
            }
        }
        at.streamCrc = crc32(at.stream.data(), at.stream.size());
        at.paletteCrc = crc32(at.palette.data(), at.palette.size());
        return at;
    }

    // ---- per texture state ----
    struct TexState
    {
        uint32_t list = 0, data = 0, res = 0;
        bool button = false;
        int applied = -1; // -1 unknown, 0 original data, 1 keyboard/mouse atlas
        int atlas = -1;   // index into g_atlases of the applied replacement
    };
    std::unordered_map<uint32_t, TexState> g_tex;
    int g_mode = -1; // last mode seen (for the log line)

    // KZ_PROMPT_SCHEDULE="110:pad,114:kbm" switches the device at those seconds after start (automation: the live switch).
    std::vector<std::pair<double, int>> g_schedule;

    bool wantKeyboard()
    {
        if (!g_schedule.empty())
        {
            const double t = SDL_GetTicks() / 1000.0;
            int mode = 1;
            for (const auto &[at, m] : g_schedule)
                if (t >= at)
                    mode = m;
            return mode == 1;
        }
        if (g_forceMode >= 0)
            return g_forceMode == 1;
        return kzInputUsingKeyboardMouse();
    }

    // Cells of the font's glyphs 32..51 (single part each).
    void readCells(const uint8_t *rdram, uint32_t font, std::array<Cell, kButtons> &cells, std::array<bool, kButtons> &valid)
    {
        valid.fill(false);
        const uint32_t table = rd32(rdram, font + 36), mask = rd32(rdram, font + 20), shift = rd32(rdram, font + 24), layers = rd32(rdram, font + 48);
        if (!ramPtr(table) || !ramPtr(layers) || shift > 12)
            return;
        for (int i = 0; i < kButtons; ++i)
        {
            const uint32_t c = static_cast<uint32_t>(i);
            const uint32_t page = rd32(rdram, table + 4 * (c >> shift));
            if (!ramPtr(page))
                continue;
            const uint32_t e = rd32(rdram, page + 4 * (c & mask));
            const uint32_t n = e >> 28, li = (e >> 16) & 0xFFF;
            if (n != 1)
                continue;
            const uint32_t a = layers + 12 * li;
            Cell cell{rd16(rdram, a + 4), rd16(rdram, a + 6), rdram[(a + 8) & kRamMask], rdram[(a + 9) & kRamMask]};
            if (cell.w < 1 || cell.h < 1 || cell.u + cell.w > kAtlasW || cell.v + cell.h > kAtlasH)
                continue;
            cells[i] = cell;
            valid[i] = true;
        }
    }

    // Is the texture the stock button atlas (or the replacement of a sibling that shares its buffers)? Fills the state.
    void classify(uint8_t *rdram, uint32_t T, TexState &s)
    {
        const uint32_t list = s.list, data = s.data, res = s.res;
        s.button = false;
        s.applied = -1;
        s.atlas = -1;
        if (rd32(rdram, list + 4) != 0x400 || rd32(rdram, T + 32) != 1 || rd32(rdram, T + 48) != 0x20080 || !ramPtr(data) || !ramPtr(data + kAtlasBytes) ||
            !ramPtr(res + 16 + kPaletteBytes) || rdram[(res + 4) & kRamMask] != 0x40)
            return;
        const uint32_t dcrc = crc32(rdram + (data & kRamMask), kAtlasBytes), pcrc = crc32(rdram + ((res + 16) & kRamMask), kPaletteBytes);
        if (dcrc == kAtlasCrc && pcrc == kPaletteCrc)
        {
            s.button = true;
            s.applied = 0;
            if (g_origStream.empty())
            {
                g_origStream.assign(rdram + (data & kRamMask), rdram + (data & kRamMask) + kAtlasBytes);
                g_origPalette.assign(rdram + ((res + 16) & kRamMask), rdram + ((res + 16) & kRamMask) + kPaletteBytes);
            }
        }
        else
        {
            for (size_t i = 0; i < g_atlases.size(); ++i)
                if (g_atlases[i].streamCrc == dcrc && g_atlases[i].paletteCrc == pcrc)
                    s.button = true, s.applied = 1, s.atlas = static_cast<int>(i); // shared buffers already switched by another font
        }
        if (g_log)
            std::printf("[kz-prompt] texture %08x list %08x data %08x res %08x crc %08x/%08x -> %s\n", T, list, data, res, dcrc, pcrc, s.button ? "button atlas" : "not the button atlas");
        if (s.applied == 1)
        {
            evict(rdram, T + 44), evict(rdram, res + 12); // this texture's own VRAM copy is the stock one
        }
    }

    // Does the RAM still hold what this texture state says? (A texture reloaded in place puts the stock data back.)
    bool stillHolds(const uint8_t *rdram, const TexState &s)
    {
        const std::vector<uint8_t> &sd = s.applied == 1 && s.atlas >= 0 ? g_atlases[static_cast<size_t>(s.atlas)].stream : g_origStream;
        const std::vector<uint8_t> &pd = s.applied == 1 && s.atlas >= 0 ? g_atlases[static_cast<size_t>(s.atlas)].palette : g_origPalette;
        if (sd.size() != kAtlasBytes || pd.size() != kPaletteBytes)
            return true;
        const uint8_t *d = rdram + (s.data & kRamMask), *p = rdram + ((s.res + 16) & kRamMask);
        return std::memcmp(d, sd.data(), 32) == 0 && std::memcmp(d + 8192, sd.data() + 8192, 32) == 0 && std::memcmp(d + kAtlasBytes - 32, sd.data() + kAtlasBytes - 32, 32) == 0 &&
               std::memcmp(p, pd.data(), 32) == 0 && std::memcmp(p + kPaletteBytes - 32, pd.data() + kPaletteBytes - 32, 32) == 0;
    }

    // Called at the entry of every text draw: brings the atlas the font draws with in line with the last input device.
    void ensureFont(uint8_t *rdram, uint32_t font)
    {
        const uint32_t T = rd32(rdram, font + 68);
        if (!ramPtr(T))
            return;
        const uint32_t list = rd32(rdram, T + 40), res = rd32(rdram, T + 16);
        if (!ramPtr(list) || !ramPtr(res))
            return;
        const uint32_t data = rd32(rdram, list);
        TexState &s = g_tex[T];
        const bool want = wantKeyboard();
        if (s.list != list || s.data != data || s.res != res || (s.button && s.applied >= 0 && !stillHolds(rdram, s)))
        {
            // a new or reloaded texture: is it the button atlas?
            s = TexState{};
            s.list = list, s.data = data, s.res = res;
            classify(rdram, T, s);
        }
        if (!s.button)
            return;
        if (g_mode != (want ? 1 : 0))
        {
            g_mode = want ? 1 : 0;
            std::printf("[kz] button prompts: %s\n", want ? "keyboard and mouse" : "controller");
        }
        if ((want ? 1 : 0) == s.applied)
            return;
        if (want)
        {
            std::array<Cell, kButtons> cells{};
            std::array<bool, kButtons> valid{};
            readCells(rdram, font, cells, valid);
            int ai = -1;
            for (size_t i = 0; i < g_atlases.size(); ++i)
                if (sameCells(cells, valid, g_atlases[i]))
                    ai = static_cast<int>(i);
            if (ai < 0)
            {
                g_atlases.push_back(buildAtlas(cells, valid));
                ai = static_cast<int>(g_atlases.size()) - 1;
            }
            const Atlas &a = g_atlases[static_cast<size_t>(ai)];
            std::memcpy(rdram + (s.data & kRamMask), a.stream.data(), kAtlasBytes);
            std::memcpy(rdram + ((s.res + 16) & kRamMask), a.palette.data(), kPaletteBytes);
            s.atlas = ai;
            if (g_log)
                std::printf("[kz-prompt] applied atlas %d to texture %08x (data %08x res %08x)\n", ai, T, s.data, s.res);
        }
        else
        {
            std::memcpy(rdram + (s.data & kRamMask), g_origStream.data(), kAtlasBytes);
            std::memcpy(rdram + ((s.res + 16) & kRamMask), g_origPalette.data(), kPaletteBytes);
        }
        evict(rdram, T + 44); // not resident: the next draw uploads the data again
        evict(rdram, s.res + 12);
        s.applied = want ? 1 : 0;
        // other textures that share the buffers are brought along when they are next drawn
        for (auto &[t2, st] : g_tex)
            if (t2 != T && st.button && st.data == s.data)
                st.applied = -1;
    }

    // ---- button names in words ----
    // Two tutorial lines name a stick in words ("Use the Right Analog Stick to look around."). While the keyboard and mouse are in use the
    // words are overwritten in place with shorter ones (the string ends earlier; the draw stops at the terminator) and the pen is moved by the
    // width that was saved: the tutorial panel right-aligns its lines (all of them end at the same x), so the line keeps its right edge.
    struct WordSwap
    {
        std::string original, replaced;
        float delta = 0.0f; // original width - new width, in glyph units
    };
    std::unordered_map<uint32_t, WordSwap> g_wordSwaps; // string buffer address -> swap

    float glyphAdvance(const uint8_t *rdram, uint32_t font, uint32_t code)
    {
        const uint32_t table = rd32(rdram, font + 36), mask = rd32(rdram, font + 20), shift = rd32(rdram, font + 24);
        if (!ramPtr(table) || code < 32 || shift > 12)
            return 0.0f;
        const uint32_t c = code - 32;
        const uint32_t page = rd32(rdram, table + 4 * (c >> shift));
        return ramPtr(page) ? static_cast<float>(rd32(rdram, page + 4 * (c & mask)) & 0xFFFF) / 256.0f : 0.0f;
    }
    float textWidth(const uint8_t *rdram, uint32_t font, const std::string &s)
    {
        float w = 0.0f;
        for (unsigned char ch : s)
            w += glyphAdvance(rdram, font, ch);
        return w;
    }

    std::string movementKeys()
    {
        std::string out;
        for (const char *t : {"MoveForward", "StrafeLeft", "MoveBack", "StrafeRight"})
        {
            const std::string k = sourceFor(t);
            if (k.empty() || k.size() > 5)
                return {};
            out += (out.empty() ? "" : " ") + k;
        }
        return out + " keys";
    }

    // Returns the pen shift (glyph units) for this string: 0 when it is not (or no longer) swapped.
    float swapWords(uint8_t *rdram, uint32_t strObj, uint32_t font, bool keyboard)
    {
        if (!ramPtr(strObj))
            return 0.0f;
        const uint32_t obj = rd32(rdram, strObj);
        if (!ramPtr(obj))
            return 0.0f;
        const uint32_t buf = obj + 16;
        char *p = reinterpret_cast<char *>(rdram + (buf & kRamMask));
        auto it = g_wordSwaps.find(buf);
        if (it != g_wordSwaps.end())
        {
            const WordSwap &w = it->second;
            const bool intact = std::strlen(p) == w.replaced.size() && std::memcmp(p, w.replaced.data(), w.replaced.size()) == 0;
            if (keyboard && intact)
                return w.delta;
            if (intact)
                std::memcpy(p, w.original.c_str(), w.original.size() + 1);
            g_wordSwaps.erase(it);
            return 0.0f;
        }
        if (!keyboard)
            return 0.0f;
        size_t len = 0;
        while (len < 200 && p[len])
            ++len;
        if (len < 20 || len >= 200 || std::memchr(p, 'A', len) == nullptr || len < 30) // cheap filter before the searches
            return 0.0f;
        const std::string line(p, len);
        struct Rule
        {
            const char *words;
            std::string repl;
        };
        const Rule rules[2] = {{"Right Analog Stick", "mouse"}, {"Left Analog Stick", ""}};
        for (const Rule &r : rules)
        {
            const size_t at = line.find(r.words);
            if (at == std::string::npos)
                continue;
            const std::string repl = r.repl.empty() ? movementKeys() : r.repl; // built only for a matched line
            if (repl.empty())
                continue;
            std::string out = line;
            out.replace(at, std::strlen(r.words), repl);
            if (out.size() > len) // never write past the original string in guest memory
                continue;
            WordSwap w{line, out, textWidth(rdram, font, line) - textWidth(rdram, font, out)};
            std::memcpy(p, out.c_str(), out.size() + 1);
            if (g_log)
                std::printf("[kz-prompt] words: \"%s\" -> \"%s\" (pen +%.1f)\n", line.c_str(), out.c_str(), w.delta);
            const float d = w.delta;
            g_wordSwaps[buf] = std::move(w);
            return d;
        }
        return 0.0f;
    }

    void text(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        if (ctx->pc == kText)
        {
            const uint32_t font = getRegU32(ctx, 6);
            if (g_enabled && ramPtr(font))
            {
                ensureFont(rdram, font);
                if (const float shift = swapWords(rdram, getRegU32(ctx, 5), font, wantKeyboard()); shift != 0.0f)
                {
                    float xs = 1.0f;
                    if (const uint32_t sc = getRegU32(ctx, 4); ramPtr(sc))
                        std::memcpy(&xs, rdram + (sc & kRamMask), 4);
                    ctx->f[12] += shift * xs;
                }
            }
            if (g_log && g_enabled && ramPtr(font)) // survey: fonts that draw a single glyph code 32..51 (button pictures)
            {
                static std::set<uint32_t> reported;
                const uint32_t str = getRegU32(ctx, 5);
                if (ramPtr(str) && ramPtr(rd32(rdram, str)))
                {
                    const uint8_t *p = rdram + ((rd32(rdram, str) + 16) & kRamMask);
                    if (p[0] >= 32 && p[0] <= 51 && p[1] == 0)
                    {
                        const uint32_t T = rd32(rdram, font + 68);
                        auto it = g_tex.find(T);
                        if (reported.insert(font).second)
                            std::printf("[kz-prompt] single glyph %u font %08x texture %08x button=%d\n", p[0] - 32u, font, T, it != g_tex.end() && it->second.button ? 1 : 0);
                    }
                }
            }
        }
        g_origText(rdram, ctx, rt);
    }

    bool hook(PS2Runtime &rt, uint32_t addr, PS2Runtime::RecompiledFunction fn, PS2Runtime::RecompiledFunction &orig)
    {
        orig = rt.hasFunction(addr) ? rt.lookupFunction(addr) : nullptr;
        return orig && rt.replaceFunction(addr, fn);
    }
}

void kzPromptsInstall(PS2Runtime &runtime)
{
    g_log = std::getenv("KZ_PROMPT_LOG") != nullptr;
    g_enabled = kzConfig().buttonPrompts;
    if (const char *v = std::getenv("KZ_PROMPTS"); v && v[0] == '0')
        g_enabled = false;
    if (const char *v = std::getenv("KZ_PROMPT_MODE"))
        g_forceMode = _stricmp(v, "kbm") == 0 ? 1 : (_stricmp(v, "pad") == 0 ? 0 : -1);
    if (const char *v = std::getenv("KZ_PROMPT_SCHEDULE"))
    {
        std::string all(v);
        size_t pos = 0;
        while (pos < all.size())
        {
            size_t end = all.find(',', pos);
            const std::string item = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? all.size() : end + 1;
            const size_t c = item.find(':');
            if (c != std::string::npos)
                g_schedule.emplace_back(std::atof(item.c_str()), _stricmp(item.c_str() + c + 1, "kbm") == 0 ? 1 : 0);
        }
    }
    const bool ok = hook(runtime, kText, &text, g_origText);
    std::printf("[kz] prompt hooks: %s\n", ok ? "installed" : "FAILED");
}
