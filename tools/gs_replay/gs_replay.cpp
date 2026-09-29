// Replays a GS trace recorded by the game (KZ_GS_TRACE, see src/kz_gs.cpp) through kzgs and saves output frames.
//
// Usage: kz_gs_replay <trace> <outdir> [--every N] [--upscale N] [--renderer d3d11|d3d12|vulkan] [--stats]
//   --stats  also prints a histogram of GS registers written (PACKED A+D and PACKED reg descriptors) and the last
//            value seen for the interesting ones.

#include "kzgs.h"

#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace
{
    struct Stats
    {
        std::map<uint32_t, uint64_t> adWrites;  // A+D register address -> count
        std::map<uint32_t, uint64_t> lastAd;    // last value per A+D register
        std::map<uint32_t, uint64_t> packedRegs; // PACKED descriptor -> count
        uint64_t tags[4] = {};
        uint32_t zMin = 0xFFFFFFFFu, zMax = 0;
        std::map<uint32_t, std::map<uint64_t, uint64_t>> distinct; // A+D reg -> value -> count (TEST/ZBUF/FRAME/ALPHA)
    };

    void scan(const uint8_t *data, uint32_t size, Stats &st)
    {
        uint32_t off = 0;
        while (off + 16 <= size)
        {
            uint64_t lo, hi;
            std::memcpy(&lo, data + off, 8);
            std::memcpy(&hi, data + off + 8, 8);
            off += 16;
            const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFF);
            const bool eop = (lo >> 15) & 1;
            const bool pre = (lo >> 46) & 1;
            const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3);
            uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xF);
            if (nreg == 0)
                nreg = 16;
            st.tags[flg]++;
            if (pre)
            {
                st.adWrites[0x00]++;
                st.lastAd[0x00] = (lo >> 47) & 0x7FF;
            }
            if (flg == 0)
            {
                for (uint32_t l = 0; l < nloop && off + 16 <= size; ++l)
                    for (uint32_t r = 0; r < nreg && off + 16 <= size; ++r, off += 16)
                    {
                        const uint32_t reg = static_cast<uint32_t>((hi >> (4 * r)) & 0xF);
                        st.packedRegs[reg]++;
                        if (reg == 0x4 || reg == 0x5) // XYZF2 / XYZ2: Z at bits 68..91 (XYZF2, 24-bit) or 64..95
                        {
                            uint64_t v1;
                            std::memcpy(&v1, data + off + 8, 8);
                            const uint32_t z = reg == 0x4 ? static_cast<uint32_t>((v1 >> 4) & 0xFFFFFF) : static_cast<uint32_t>(v1 & 0xFFFFFFFF);
                            st.zMin = std::min(st.zMin, z);
                            st.zMax = std::max(st.zMax, z);
                        }
                        if (reg == 0xE)
                        {
                            uint64_t val, addr;
                            std::memcpy(&val, data + off, 8);
                            std::memcpy(&addr, data + off + 8, 8);
                            st.adWrites[static_cast<uint32_t>(addr & 0xFF)]++;
                            st.lastAd[static_cast<uint32_t>(addr & 0xFF)] = val;
                            const uint32_t a = static_cast<uint32_t>(addr & 0xFF);
                            if (a == 0x47 || a == 0x48 || a == 0x4E || a == 0x4F || a == 0x4C || a == 0x42 || a == 0x06)
                                st.distinct[a][val]++;
                        }
                    }
            }
            else if (flg == 1)
                off += ((nloop * nreg + 1) / 2) * 16;
            else
                off += nloop * 16;
            if (eop)
                break;
        }
    }

    bool savePng(const std::vector<uint8_t> &rgbaIn, int w, int h, const std::string &path)
    {
        std::vector<uint8_t> rgba = rgbaIn;
        for (size_t i = 3; i < rgba.size(); i += 4)
            rgba[i] = 0xFF;
        SDL_Surface *surf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, rgba.data(), w * 4);
        if (!surf)
            return false;
        const bool ok = SDL_SavePNG(surf, path.c_str());
        SDL_DestroySurface(surf);
        return ok;
    }
}

static LONG WINAPI crashFilter(EXCEPTION_POINTERS *ep)
{
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, nullptr, TRUE);
    auto describe = [&](DWORD64 addr) {
        static char out[600];
        alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 256] = {};
        auto *sym = reinterpret_cast<SYMBOL_INFO *>(symBuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        HMODULE mod = nullptr;
        char modName[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(addr), &mod))
            GetModuleFileNameA(mod, modName, MAX_PATH);
        const char *base = std::strrchr(modName, 92);
        std::snprintf(out, sizeof(out), "%s!%s+0x%llx", base ? base + 1 : modName,
                      SymFromAddr(proc, addr, &disp, sym) ? sym->Name : "?", static_cast<unsigned long long>(disp));
        return out;
    };
    std::fprintf(stderr, "CRASH code=%08lx at %s thread=%lu\n", ep->ExceptionRecord->ExceptionCode,
                 describe(reinterpret_cast<DWORD64>(ep->ExceptionRecord->ExceptionAddress)), GetCurrentThreadId());
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip; frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp; frame.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 24 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &frame, &ctx, nullptr,
                                          SymFunctionTableAccess64, SymGetModuleBase64, nullptr) && frame.AddrPC.Offset; ++i)
        std::fprintf(stderr, "  #%02d %s\n", i, describe(frame.AddrPC.Offset));
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetUnhandledExceptionFilter(crashFilter);
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: kz_gs_replay <trace> <outdir> [--every N] [--upscale N] [--renderer d3d11|d3d12|vulkan] [--stats]\n");
        return 2;
    }
    const std::string tracePath = argv[1];
    const std::filesystem::path outDir = argv[2];
    int every = 30, upscale = 2;
    bool stats = false, fixDy = false;
    KzgsConfig cfg;
    for (int i = 3; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--every" && i + 1 < argc) every = std::atoi(argv[++i]);
        else if (a == "--upscale" && i + 1 < argc) upscale = std::atoi(argv[++i]);
        else if (a == "--stats") stats = true;
        else if (a == "--aniso" && i + 1 < argc) cfg.anisotropy = std::atoi(argv[++i]);
        else if (a == "--fxaa") cfg.fxaa = true;
        else if (a == "--adapter" && i + 1 < argc) cfg.adapter = argv[++i];
        else if (a == "--fix-dy") fixDy = true;
        else if (a == "--renderer" && i + 1 < argc)
        {
            const std::string r = argv[++i];
            cfg.renderer = r == "vulkan" ? KzgsRenderer::Vulkan : r == "d3d12" ? KzgsRenderer::D3D12 : KzgsRenderer::D3D11;
        }
    }
    cfg.upscale = upscale;
    cfg.vsync = false;
    std::filesystem::create_directories(outDir);

    FILE *f = std::fopen(tracePath.c_str(), "rb");
    if (!f)
    {
        std::fprintf(stderr, "cannot open %s\n", tracePath.c_str());
        return 1;
    }
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window *win = SDL_CreateWindow("gs_replay", 1280, 896, SDL_WINDOW_HIDDEN);
    void *hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(win), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    alignas(16) static uint8_t regs[0x2000] = {};
    std::string err;
    kzgsSetLogCallback([](int level, const char *msg) {
        if (level <= 1)
            std::fprintf(stderr, "[kzgs] %s\n", msg);
    });
    if (!kzgsOpen(hwnd, cfg, regs, &err))
    {
        std::fprintf(stderr, "kzgsOpen failed: %s\n", err.c_str());
        return 1;
    }

    Stats st;
    uint64_t frame = 0, packets[4] = {};
    std::vector<uint8_t> buf;
    uint32_t hdr[3];
    uint64_t records = 0;
    while (std::fread(hdr, sizeof(hdr), 1, f) == 1)
    {
        ++records;
        if (hdr[0] < 1 || hdr[0] > 2 || hdr[2] > (64u << 20) || (hdr[0] == 1 && (hdr[2] % 16) != 0))
        {
            std::fprintf(stderr, "stopping at corrupt/truncated record %llu (type %u size %u)\n",
                         static_cast<unsigned long long>(records), hdr[0], hdr[2]);
            break;
        }
        buf.resize(hdr[2]);
        if (hdr[2] && std::fread(buf.data(), hdr[2], 1, f) != 1)
            break;
        if (hdr[0] == 1)
        {
            if (hdr[1] <= 3) packets[hdr[1]]++;
            if (stats) scan(buf.data(), hdr[2], st);
            kzgsGifTransfer(static_cast<int>(hdr[1]), buf.data(), hdr[2] / 16u);
        }
        else if (hdr[0] == 2)
        {
            std::memcpy(regs, buf.data(), std::min<size_t>(sizeof(regs), buf.size()));
            if (fixDy)
                for (uint32_t off : {0x80u, 0xA0u}) // DISPLAY1/2: force DY to the NTSC default when out of range
                {
                    uint64_t d;
                    std::memcpy(&d, regs + off, 8);
                    const uint64_t dy = (d >> 12) & 0x7FF;
                    if (dy > 0x100)
                        d = (d & ~(0x7FFull << 12)) | (0x32ull << 12) | ((off == 0xA0u) ? 0 : 0);
                    std::memcpy(regs + off, &d, 8);
                }
            kzgsVsync(static_cast<int>(hdr[1]), true);
            ++frame;
            if (every > 0 && frame % every == 0)
            {
                std::vector<uint8_t> px;
                int w = 0, h = 0;
                if (kzgsReadback(px, w, h))
                {
                    size_t nonBlack = 0;
                    for (size_t i = 0; i + 3 < px.size(); i += 4)
                        nonBlack += (px[i] + px[i + 1] + px[i + 2]) > 30;
                    char name[64];
                    std::snprintf(name, sizeof(name), "replay_%04llu.png", static_cast<unsigned long long>(frame));
                    savePng(px, w, h, (outDir / name).string());
                    std::printf("frame %llu: %dx%d nonblack %.1f%%\n", static_cast<unsigned long long>(frame), w, h,
                                100.0 * nonBlack / (px.size() / 4 ? px.size() / 4 : 1));
                }
            }
        }
    }
    std::fclose(f);
    std::printf("frames %llu, packets p1=%llu p2=%llu p3=%llu\n", static_cast<unsigned long long>(frame),
                static_cast<unsigned long long>(packets[1]), static_cast<unsigned long long>(packets[2]),
                static_cast<unsigned long long>(packets[3]));
    if (stats)
    {
        std::printf("GIF tags: packed=%llu reglist=%llu image=%llu\n", static_cast<unsigned long long>(st.tags[0]),
                    static_cast<unsigned long long>(st.tags[1]), static_cast<unsigned long long>(st.tags[2] + st.tags[3]));
        std::printf("PACKED descriptors:");
        for (auto &[r, n] : st.packedRegs)
            std::printf(" %X:%llu", r, static_cast<unsigned long long>(n));
        std::printf("\nA+D registers (count, last value):\n");
        for (auto &[r, n] : st.adWrites)
            std::printf("  %02X  %8llu  %016llx\n", r, static_cast<unsigned long long>(n), static_cast<unsigned long long>(st.lastAd[r]));
    }
    if (stats)
    {
        std::printf("vertex Z range: %u .. %u\n", st.zMin, st.zMax);
        for (auto &[reg, vals] : st.distinct)
        {
            std::printf("reg %02X distinct values:", reg);
            int n = 0;
            for (auto &[v, c] : vals)
                if (n++ < 8) std::printf(" %llx(x%llu)", static_cast<unsigned long long>(v), static_cast<unsigned long long>(c));
            std::printf("\n");
        }
    }
    kzgsClose();
    SDL_DestroyWindow(win);
    return 0;
}
