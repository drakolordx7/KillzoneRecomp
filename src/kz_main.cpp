// Killzone runner entry point. Replaces ps2xRuntime/src/main.cpp so we control boot paths.
//
// Usage: killzone.exe [--elf <path to SCUS_974.02>] [--iso <path to Killzone ISO>] [--sample <seconds>]
// Defaults: --elf game/SCUS_974.02 (relative to the working directory); --iso unset.
// The ELF's directory is mounted as cdrom0:. The ISO, when given, backs raw sceCdRead sector reads.

#include "ps2_runtime.h"
#include "kz_sampler.h"
#if defined(PS2X_ENABLE_DEBUG_UI)
#include "ps2_debug_panel.h"
#endif

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace
{
    struct Options
    {
        std::filesystem::path elf = "game/SCUS_974.02";
        std::filesystem::path iso;
        int sampleSeconds = 0; // --sample N: dump all thread stacks to work/stacks.txt every N seconds
    };

    Options parseArgs(int argc, char *argv[])
    {
        Options opts;
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg = argv[i];
            if (arg == "--elf" && i + 1 < argc)
                opts.elf = argv[++i];
            else if (arg == "--iso" && i + 1 < argc)
                opts.iso = argv[++i];
            else if (arg == "--sample" && i + 1 < argc)
                opts.sampleSeconds = std::atoi(argv[++i]);
            else
                std::cerr << "[kz] ignoring unknown argument: " << arg << std::endl;
        }
        return opts;
    }
}

int main(int argc, char *argv[])
{
    try
    {
        const Options opts = parseArgs(argc, argv);
        const std::filesystem::path elf = std::filesystem::absolute(opts.elf).lexically_normal();
        if (!std::filesystem::exists(elf))
        {
            std::cerr << "[kz] ELF not found: " << elf.string() << std::endl;
            return 1;
        }

        if (opts.sampleSeconds > 0)
            kzStartStackSampler(opts.sampleSeconds, 24, "work/stacks.txt");

        PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
        if (!opts.iso.empty())
        {
            paths.cdImage = std::filesystem::absolute(opts.iso).lexically_normal();
            std::cout << "[kz] CD image: " << paths.cdImage.string() << std::endl;
        }
        PS2Runtime::setIoPaths(paths);

        PS2Runtime runtime;
#if defined(PS2X_ENABLE_DEBUG_UI)
        PS2DebugPanel debugPanel;
        runtime.setDebugUiCallbacks(
            [](PS2Runtime &, void *userData) { static_cast<PS2DebugPanel *>(userData)->initialize(); },
            [](PS2Runtime &rt, void *userData) { static_cast<PS2DebugPanel *>(userData)->draw(rt); },
            [](PS2Runtime &, void *userData) { static_cast<PS2DebugPanel *>(userData)->shutdown(); },
            &debugPanel);
#endif
        if (!runtime.initialize("Killzone"))
        {
            std::cerr << "[kz] failed to initialize PS2 runtime" << std::endl;
            return 1;
        }
        if (!runtime.loadELF(elf.string()))
        {
            std::cerr << "[kz] failed to load ELF: " << elf.string() << std::endl;
            return 1;
        }

        runtime.run();

        std::cout.flush();
        std::cerr.flush();
        std::_Exit(0);
    }
    catch (const std::exception &e)
    {
        std::cerr << "[kz] fatal exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[kz] fatal exception: unknown" << std::endl;
    }
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(1);
}
