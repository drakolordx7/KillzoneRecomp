#pragma once

// PC port settings, stored in killzone.ini next to the executable. Every field has a default so a missing or partial
// file is valid. The launcher edits these; the runtime reads them at boot (and some live, e.g. sensitivity).

#include <filesystem>
#include <string>

enum class KzWindowMode
{
    Windowed,
    Borderless,
    Fullscreen,
};

enum class KzRenderer
{
    D3D11,
    D3D12,
    Vulkan,
};

enum class KzAspect
{
    Widescreen16x9, // game's native widescreen mode (FOV + HUD handled by the engine)
    Classic4x3,
    Stretch,
};

struct KzConfig
{
    // Display
    KzWindowMode windowMode = KzWindowMode::Borderless;
    int width = 0;  // 0 = desktop resolution
    int height = 0;
    bool vsync = false;
    int fpsLimit = 0; // 0 = unlimited (display refresh with vsync); otherwise a cap, e.g. 120, 144, 240

    // Graphics
    KzRenderer renderer = KzRenderer::D3D11;
    int upscale = 0; // internal resolution multiplier over 640x448; 0 = auto (match window height)
    KzAspect aspect = KzAspect::Widescreen16x9;
    int anisotropy = 16;     // 0/2/4/8/16
    bool fxaa = true;
    bool bilinear = true;    // texture filtering
    bool noiseFilter = false; // PS2 film-grain overlay (off by default on PC)

    // Input
    float mouseSensitivity = 1.0f;
    bool invertY = false;
    bool rawMouse = true;
    float stickDeadzone = 0.15f;

    // Audio
    int masterVolume = 100; // 0..100

    // Paths
    std::string isoPath; // user's Killzone (USA) disc image

    bool showLauncher = true; // show the options launcher before booting
};

std::filesystem::path kzConfigPath();
KzConfig kzLoadConfig(const std::filesystem::path &path);
bool kzSaveConfig(const std::filesystem::path &path, const KzConfig &cfg);

// Process-wide settings (loaded once in main; the launcher may replace them before boot).
KzConfig &kzConfig();
