// Killzone runner entry point. Replaces ps2xRuntime/src/main.cpp so we control boot paths.
//
// Usage: killzone.exe [--elf <path to SCUS_974.02>] [--iso <path to Killzone ISO>] [--sample <seconds>]
// Defaults: --elf game/SCUS_974.02 (relative to the working directory); --iso unset.
// The ELF's directory is mounted as cdrom0:. The ISO, when given, backs raw sceCdRead sector reads.

#include "ps2_runtime.h"
#include "kz_sampler.h"
#include "kz_config.h"
#include "kz_input.h"
#include "kz_iso.h"
#include "kz_launcher.h"
#include "kz_gs.h"
#include "kz_timing.h"
#include "kz_vu.h"
#include "kz_ipu.h"
#include "kz_audio.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>
#include "runtime/ps2_pad_provider.h"

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
        bool elfGiven = false;
        std::filesystem::path iso;
        int sampleSeconds = 0; // --sample N: dump all thread stacks to work/stacks.txt every N seconds
        bool selfTest = false;  // --selftest: run unit checks (config, input, disc, launcher render) and exit
        bool noLauncher = false; // --no-launcher: boot straight into the game
        bool forceLauncher = false; // --launcher: show the launcher even if it is turned off in killzone.ini
    };

    Options parseArgs(int argc, char *argv[])
    {
        Options opts;
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg = argv[i];
            if (arg == "--elf" && i + 1 < argc)
            {
                opts.elf = argv[++i];
                opts.elfGiven = true;
            }
            else if (arg == "--iso" && i + 1 < argc)
                opts.iso = argv[++i];
            else if (arg == "--sample" && i + 1 < argc)
                opts.sampleSeconds = std::atoi(argv[++i]);
            else if (arg == "--selftest")
                opts.selfTest = true;
            else if (arg == "--no-launcher")
                opts.noLauncher = true;
            else if (arg == "--launcher")
                opts.forceLauncher = true;
            else
                std::cerr << "[kz] ignoring unknown argument: " << arg << std::endl;
        }
        return opts;
    }
}

namespace
{
    SDL_Window *createGameWindow(const KzConfig &cfg)
    {
        const SDL_DisplayID display = SDL_GetPrimaryDisplay();
        const SDL_DisplayMode *desktop = SDL_GetDesktopDisplayMode(display);
        const int deskW = desktop ? desktop->w : 1920, deskH = desktop ? desktop->h : 1080;
        int w = cfg.width > 0 ? cfg.width : deskW;
        int h = cfg.height > 0 ? cfg.height : deskH;
        SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
        if (cfg.windowMode == KzWindowMode::Windowed)
        {
            if (cfg.width == 0)
            {
                w = deskW * 3 / 4;
                h = deskH * 3 / 4;
            }
            flags |= SDL_WINDOW_RESIZABLE;
        }
        else
        {
            flags |= SDL_WINDOW_FULLSCREEN;
        }
        SDL_Window *win = SDL_CreateWindow("Killzone", w, h, flags);
        if (!win)
            return nullptr;
        if (cfg.windowMode == KzWindowMode::Fullscreen)
        {
            SDL_DisplayMode mode{};
            if (SDL_GetClosestFullscreenDisplayMode(display, w, h, 0.0f, true, &mode))
                SDL_SetWindowFullscreenMode(win, &mode); // exclusive
        }
        else if (cfg.windowMode == KzWindowMode::Borderless)
        {
            SDL_SetWindowFullscreenMode(win, nullptr); // borderless desktop
        }
        SDL_SyncWindow(win);
        return win;
    }

    int runSelfTest()
    {
        int failures = 0;
        // config round-trip through a temp file
        KzConfig a;
        a.windowMode = KzWindowMode::Fullscreen;
        a.fpsLimit = 144;
        a.upscale = 4;
        a.aspect = KzAspect::Classic4x3;
        a.mouseSensitivity = 2.5f;
        a.invertY = true;
        a.isoPath = "C:/games/Killzone (USA).iso";
        const auto tmp = std::filesystem::temp_directory_path() / "kz_selftest.ini";
        if (!kzSaveConfig(tmp, a)) { std::cerr << "[selftest] FAIL: save config" << std::endl; ++failures; }
        const KzConfig b = kzLoadConfig(tmp);
        if (b.windowMode != a.windowMode || b.fpsLimit != 144 || b.upscale != 4 || b.aspect != a.aspect ||
            b.mouseSensitivity != 2.5f || !b.invertY || b.isoPath != a.isoPath)
        {
            std::cerr << "[selftest] FAIL: config round-trip" << std::endl;
            ++failures;
        }
        std::filesystem::remove(tmp);
        const KzConfig d = kzLoadConfig(std::filesystem::temp_directory_path() / "kz_missing.ini");
        if (d.windowMode != KzWindowMode::Borderless || d.fpsLimit != 0) { std::cerr << "[selftest] FAIL: defaults" << std::endl; ++failures; }
        kzInputInit(std::filesystem::temp_directory_path() / "kz_missing.ini");
        failures += kzInputSelfTest();
        // disc check against the user's image when present (not required for the test to pass)
        const KzConfig saved = kzLoadConfig(kzConfigPath());
        if (!saved.isoPath.empty())
            std::cout << "[selftest] disc: " << kzCheckDisc(saved.isoPath).message << std::endl;
        if (kzCheckDisc(std::filesystem::temp_directory_path() / "kz_missing.iso").ok)
        {
            std::cerr << "[selftest] FAIL: missing disc accepted" << std::endl;
            ++failures;
        }
        KzConfig shot = saved;
        const std::string png = (std::filesystem::current_path() / "work" / "launcher.png").string();
        std::cout << "[selftest] launcher render -> " << png << ": " << (kzLauncherScreenshot(shot, png.c_str()) ? "ok" : "unavailable") << std::endl;
        std::cout << "[selftest] " << (failures ? "FAILED" : "passed") << " (" << failures << " failures)" << std::endl;
        return failures ? 1 : 0;
    }
}

int main(int argc, char *argv[])
{
    try
    {
        const Options opts = parseArgs(argc, argv);
        if (opts.selfTest)
            return runSelfTest();
        kzConfig() = kzLoadConfig(kzConfigPath());
        KzConfig &cfg = kzConfig();
        if (!opts.iso.empty())
            cfg.isoPath = std::filesystem::absolute(opts.iso).string();
        const bool headless = std::getenv("PS2X_HEADLESS") && std::getenv("PS2X_HEADLESS")[0] == '1';
        if (!headless && !opts.noLauncher && (opts.forceLauncher || cfg.showLauncher || !kzCheckDisc(cfg.isoPath).ok))
        {
            if (!kzRunLauncher(cfg))
                return 0;
        }
        // Boot files: an explicit --elf (development), otherwise the small files extracted from the user's disc image
        // into <exe dir>/disc. Game data is read from the image itself.
        std::filesystem::path elf;
        if (opts.elfGiven)
            elf = std::filesystem::absolute(opts.elf).lexically_normal();
        else
        {
            std::string prepError;
            elf = kzPrepareDiscFiles(cfg.isoPath, kzConfigPath().parent_path() / "disc", &prepError);
            if (elf.empty())
            {
                const std::string msg = "Can't use the game disc image: " + prepError;
                std::cerr << "[kz] " << msg << std::endl;
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Killzone", msg.c_str(), nullptr);
                return 1;
            }
        }
        if (!std::filesystem::exists(elf))
        {
            std::cerr << "[kz] ELF not found: " << elf.string() << std::endl;
            return 1;
        }

        if (opts.sampleSeconds > 0)
            kzStartStackSampler(opts.sampleSeconds, 24, "work/stacks.txt");

        PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
        if (!cfg.isoPath.empty())
        {
            paths.cdImage = std::filesystem::path(cfg.isoPath).lexically_normal();
            std::cout << "[kz] CD image: " << paths.cdImage.string() << std::endl;
        }
        PS2Runtime::setIoPaths(paths);

        kzInputInit(kzConfigPath());
        ps2SetPadProvider(&kzPadProvider);

        // The runtime's raylib window/present loop is replaced by our SDL window + kzgs, so the runtime always runs
        // window-less. Automation (PS2X_HEADLESS=1 already set by the caller) also skips our window: kzgs renders
        // offscreen and KZ_SHOT_DIR/KZ_SHOT_INTERVAL save frames.
        const bool automation = headless;
        _putenv_s("PS2X_HEADLESS", "1");

        SDL_Window *window = nullptr;
        void *hwnd = nullptr;
        int winW = 1280, winH = 896;
        if (automation)
        {
            // Automation still renders into a real (hidden) window: kzgs is exercised the same way as in the game.
            if (SDL_InitSubSystem(SDL_INIT_VIDEO))
            {
                window = SDL_CreateWindow("Killzone (automation)", winW, winH, SDL_WINDOW_HIDDEN);
                if (window)
                    hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
            }
        }
        else
        {
            if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
            {
                std::cerr << "[kz] SDL video init failed: " << SDL_GetError() << std::endl;
                return 1;
            }
            window = createGameWindow(cfg);
            if (!window)
            {
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Killzone", SDL_GetError(), nullptr);
                return 1;
            }
            hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
            SDL_GetWindowSizeInPixels(window, &winW, &winH);
            SDL_SetWindowRelativeMouseMode(window, true);
            kzInputSetCaptured(true);
        }

        std::string vuError;
        if (!kzVuInstall(&vuError))
        {
            std::cerr << "[kz] VU1 recompiler init failed: " << vuError << std::endl;
            return 1;
        }
        kzIpuInstall();
        kzAudioInstall();
        PS2Runtime runtime;
        kzVuBindRuntime(runtime);
        kzIpuBindRuntime(runtime);
        kzAudioBindRuntime(runtime);
        static PS2Runtime *s_runtime = &runtime;
        kzInputSetScriptClock([]() -> double {
            return static_cast<double>(s_runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed)) / kzTimingRate();
        });
        if (!runtime.initialize("Killzone"))
        {
            std::cerr << "[kz] failed to initialize PS2 runtime" << std::endl;
            return 1;
        }
        std::string gsError;
        const bool cpuGs = std::getenv("KZ_GS") && std::strcmp(std::getenv("KZ_GS"), "cpu") == 0; // debug: software GS
        if (cpuGs)
            std::cout << "[kz] KZ_GS=cpu: using the runtime's software GS (PS2X_HEADLESS_SHOTS for frames)" << std::endl;
        else if (!kzGsAttach(runtime, hwnd, winH, cfg, &gsError))
        {
            const std::string msg = "Could not start the renderer: " + gsError;
            std::cerr << "[kz] " << msg << std::endl;
            if (window)
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Killzone", msg.c_str(), window);
            return 1;
        }
        if (const char *dir = std::getenv("KZ_SHOT_DIR"))
            kzGsSetScreenshotSchedule(dir, std::getenv("KZ_SHOT_INTERVAL") ? std::atoi(std::getenv("KZ_SHOT_INTERVAL")) : 5);
        if (!runtime.loadELF(elf.string()))
        {
            std::cerr << "[kz] failed to load ELF: " << elf.string() << std::endl;
            return 1;
        }

        // Guest vblank rate = target frame rate (kz_timing.h). Automation can force it with KZ_FPS.
        int displayHz = 60;
        if (window)
        {
            if (const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window)))
                displayHz = static_cast<int>(std::lround(mode->refresh_rate));
        }
        const int fps = std::getenv("KZ_FPS") ? std::atoi(std::getenv("KZ_FPS")) : cfg.fpsLimit;
        kzTimingInit(fps, displayHz);

        std::atomic<bool> runtimeDone{false};
        std::thread runtimeThread([&]() {
            runtime.run();
            runtimeDone.store(true);
        });

        bool focused = true;
        while (!runtimeDone.load())
        {
            if (window)
            {
                SDL_Event e;
                while (SDL_PollEvent(&e))
                {
                    kzInputOnEvent(e);
                    switch (e.type)
                    {
                    case SDL_EVENT_QUIT:
                    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                        runtime.requestStop();
                        break;
                    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                        kzGsResize(e.window.data1, e.window.data2);
                        break;
                    case SDL_EVENT_WINDOW_FOCUS_LOST:
                        focused = false;
                        SDL_SetWindowRelativeMouseMode(window, false);
                        kzInputSetCaptured(false);
                        break;
                    case SDL_EVENT_WINDOW_FOCUS_GAINED:
                    case SDL_EVENT_MOUSE_BUTTON_DOWN:
                        if (!focused || !SDL_GetWindowRelativeMouseMode(window))
                        {
                            focused = true;
                            SDL_SetWindowRelativeMouseMode(window, true);
                            kzInputSetCaptured(true);
                        }
                        break;
                    default:
                        break;
                    }
                }
            }
            kzInputPoll();
            SDL_Delay(1);
        }
        runtime.requestStop();
        runtimeThread.join();
        kzGsDetach();
        kzInputShutdown();
        if (window)
            SDL_DestroyWindow(window);

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
