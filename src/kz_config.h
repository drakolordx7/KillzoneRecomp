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
    bool fxaa = false;       // post-process blur AA; off by default (supersampling via Upscale is the sharp AA)
    bool smaa = true;        // SMAA 1x post-process (Direct3D 11 renderer only; measured: no frame-rate cost)
    bool textureReplacement = true; // load textures from textures/SCUS-97402/replacements (PCSX2 texture replacement)
    bool bilinear = true;    // texture filtering
    bool sharpScaling = true; // final image -> window: sharp bilinear (crisp pixel edges) instead of smooth
    int sharpen = 0;         // Contrast Adaptive Sharpening, 0 = off, 1..100
    bool noiseFilter = false; // PS2 film-grain overlay (off by default on PC)
    // The game's own soft post passes (docs/findings.md "Post-process passes"); the colour grade is always kept.
    bool motionBlur = false; // blends the previous frame over the current one
    bool glow = false;       // blurred additive glow of emissive surfaces
    bool lensBlur = false;   // blur + overlay while the camera has a lens texture (rain)
    // View distance (docs/findings.md "Draw distance and LOD"). 1 = original.
    float renderDistance = 1.0f; // multiplies each render zone's far plane and fog start/end
    float lodScale = 1.0f;       // multiplies the LOD detail scale (higher = detailed meshes farther away)

    // Input
    float mouseSensitivity = 1.0f;
    bool invertY = false;
    bool rawMouse = true;
    bool holdAim = true;     // hold the zoom key to aim (the game's own zoom is a toggle)
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
