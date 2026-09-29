// Replays a KZ_GS_TRACE recording through PS2Recomp's software GS frontend (reference for kz_gs_replay).
// Usage: kz_gs_replay_cpu <trace> <outdir> [--every N]
#include "ps2_runtime.h"
#include "runtime/gs/gs_frontend.h"
#include "raylib.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3)
        return 2;
    int every = 50;
    for (int i = 3; i < argc; ++i)
        if (std::string(argv[i]) == "--every" && i + 1 < argc)
            every = std::atoi(argv[++i]);
    std::filesystem::create_directories(argv[2]);
    FILE *f = std::fopen(argv[1], "rb");
    if (!f)
        return 1;
    static GSRegisters regs{};
    std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0);
    auto gs = std::make_unique<GS>();
    gs->init(vram.data(), static_cast<uint32_t>(vram.size()), &regs);
    uint32_t hdr[3];
    std::vector<uint8_t> buf;
    uint64_t frame = 0;
    while (std::fread(hdr, sizeof(hdr), 1, f) == 1)
    {
        if (hdr[0] < 1 || hdr[0] > 2 || hdr[2] > (64u << 20))
            break;
        buf.resize(hdr[2]);
        if (hdr[2] && std::fread(buf.data(), hdr[2], 1, f) != 1)
            break;
        if (hdr[0] == 1)
            gs->processGIFPacket(buf.data(), hdr[2]);
        else
        {
            auto get = [&](uint32_t off) { uint64_t v; std::memcpy(&v, buf.data() + off, 8); return v; };
            regs.pmode = get(0x00); regs.smode1 = get(0x10); regs.smode2 = get(0x20);
            regs.dispfb1 = get(0x70); regs.display1 = get(0x80); regs.dispfb2 = get(0x90); regs.display2 = get(0xA0);
            regs.bgcolor = get(0xE0);
            ++frame;
            if (every > 0 && frame % every == 0)
            {
                gs->latchHostPresentationFrame();
                std::vector<uint8_t> px;
                uint32_t w = 0, h = 0;
                if (gs->copyLatchedHostPresentationFrame(px, w, h))
                {
                    size_t nb = 0;
                    for (size_t i = 0; i + 3 < px.size(); i += 4)
                        nb += (px[i] + px[i + 1] + px[i + 2]) > 30;
                    std::printf("frame %llu: %ux%u nonblack %.1f%%\n", static_cast<unsigned long long>(frame), w, h,
                                100.0 * nb / (px.size() / 4 ? px.size() / 4 : 1));
                    for (size_t i = 3; i < px.size(); i += 4)
                        px[i] = 0xFF;
                    Image img{px.data(), static_cast<int>(w), static_cast<int>(h), 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                    char name[64];
                    std::snprintf(name, sizeof(name), "/cpu_%04llu.png", static_cast<unsigned long long>(frame));
                    ExportImage(img, (std::string(argv[2]) + name).c_str());
                    std::snprintf(name, sizeof(name), "/vram_%04llu.bin", static_cast<unsigned long long>(frame));
                    if (FILE *vf = std::fopen((std::string(argv[2]) + name).c_str(), "wb"))
                    {
                        std::fwrite(vram.data(), 1, vram.size(), vf);
                        std::fclose(vf);
                    }
                }
            }
        }
    }
    std::printf("frames %llu\n", static_cast<unsigned long long>(frame));
    std::_Exit(0);
}
