#include "kz_config.h"
#include "kz_input.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace
{
    std::string trim(std::string s)
    {
        const auto b = s.find_first_not_of(" \t\r");
        const auto e = s.find_last_not_of(" \t\r");
        return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
    }

    template <typename E>
    struct EnumName
    {
        E value;
        const char *name;
    };

    constexpr EnumName<KzWindowMode> kWindowModes[] = {
        {KzWindowMode::Windowed, "windowed"}, {KzWindowMode::Borderless, "borderless"}, {KzWindowMode::Fullscreen, "fullscreen"}};
    constexpr EnumName<KzRenderer> kRenderers[] = {
        {KzRenderer::D3D11, "d3d11"}, {KzRenderer::D3D12, "d3d12"}, {KzRenderer::Vulkan, "vulkan"}};
    constexpr EnumName<KzAspect> kAspects[] = {
        {KzAspect::Widescreen16x9, "16:9"}, {KzAspect::Classic4x3, "4:3"}, {KzAspect::Stretch, "stretch"}};

    template <typename E, size_t N>
    const char *toName(const EnumName<E> (&table)[N], E v)
    {
        for (const auto &e : table)
            if (e.value == v)
                return e.name;
        return table[0].name;
    }

    template <typename E, size_t N>
    E fromName(const EnumName<E> (&table)[N], const std::string &s, E fallback)
    {
        for (const auto &e : table)
            if (_stricmp(e.name, s.c_str()) == 0)
                return e.value;
        return fallback;
    }

    bool parseBool(const std::string &s, bool fallback)
    {
        if (s == "1" || _stricmp(s.c_str(), "true") == 0 || _stricmp(s.c_str(), "on") == 0)
            return true;
        if (s == "0" || _stricmp(s.c_str(), "false") == 0 || _stricmp(s.c_str(), "off") == 0)
            return false;
        return fallback;
    }
}

std::filesystem::path kzConfigPath()
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return std::filesystem::path(exe).parent_path() / "killzone.ini";
}

KzConfig kzLoadConfig(const std::filesystem::path &path)
{
    KzConfig c;
    std::ifstream in(path);
    if (!in)
        return c;
    std::map<std::string, std::string> kv;
    std::string section, line;
    while (std::getline(in, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        if (line.front() == '[' && line.back() == ']')
        {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        kv[section + "." + trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    auto get = [&](const char *key) -> const std::string * {
        auto it = kv.find(key);
        return it == kv.end() ? nullptr : &it->second;
    };
    if (auto v = get("Display.WindowMode")) c.windowMode = fromName(kWindowModes, *v, c.windowMode);
    if (auto v = get("Display.Width")) c.width = std::max(0, std::atoi(v->c_str()));
    if (auto v = get("Display.Height")) c.height = std::max(0, std::atoi(v->c_str()));
    if (auto v = get("Display.VSync")) c.vsync = parseBool(*v, c.vsync);
    if (auto v = get("Display.FpsLimit")) c.fpsLimit = std::max(0, std::atoi(v->c_str()));
    if (auto v = get("Graphics.Renderer")) c.renderer = fromName(kRenderers, *v, c.renderer);
    if (auto v = get("Graphics.Upscale")) c.upscale = std::clamp(std::atoi(v->c_str()), 0, 8);
    if (auto v = get("Graphics.Aspect")) c.aspect = fromName(kAspects, *v, c.aspect);
    if (auto v = get("Graphics.Anisotropy")) c.anisotropy = std::clamp(std::atoi(v->c_str()), 0, 16);
    if (auto v = get("Graphics.FXAA")) c.fxaa = parseBool(*v, c.fxaa);
    if (auto v = get("Graphics.SMAA")) c.smaa = parseBool(*v, c.smaa);
    if (auto v = get("Graphics.TextureReplacement")) c.textureReplacement = parseBool(*v, c.textureReplacement);
    if (auto v = get("Graphics.Bilinear")) c.bilinear = parseBool(*v, c.bilinear);
    if (auto v = get("Graphics.SharpScaling")) c.sharpScaling = parseBool(*v, c.sharpScaling);
    if (auto v = get("Graphics.Sharpen")) c.sharpen = std::clamp(std::atoi(v->c_str()), 0, 100);
    // Files written before the sharp defaults (no SharpScaling key) had FXAA on by default: switch it off once.
    if (!get("Graphics.SharpScaling")) c.fxaa = false;
    if (auto v = get("Graphics.NoiseFilter")) c.noiseFilter = parseBool(*v, c.noiseFilter);
    if (auto v = get("Graphics.MotionBlur")) c.motionBlur = parseBool(*v, c.motionBlur);
    if (auto v = get("Graphics.Glow")) c.glow = parseBool(*v, c.glow);
    if (auto v = get("Graphics.LensBlur")) c.lensBlur = parseBool(*v, c.lensBlur);
    if (auto v = get("Graphics.RenderDistance")) c.renderDistance = std::clamp(static_cast<float>(std::atof(v->c_str())), 1.0f, 16.0f);
    if (auto v = get("Graphics.LodScale")) c.lodScale = std::clamp(static_cast<float>(std::atof(v->c_str())), 0.25f, 16.0f);
    if (auto v = get("Input.MouseSensitivity")) c.mouseSensitivity = std::clamp(static_cast<float>(std::atof(v->c_str())), 0.05f, 20.0f);
    if (auto v = get("Input.InvertY")) c.invertY = parseBool(*v, c.invertY);
    if (auto v = get("Input.RawMouse")) c.rawMouse = parseBool(*v, c.rawMouse);
    if (auto v = get("Input.AimMode")) c.holdAim = _stricmp(v->c_str(), "Toggle") != 0;
    if (auto v = get("Input.StickDeadzone")) c.stickDeadzone = std::clamp(static_cast<float>(std::atof(v->c_str())), 0.0f, 0.9f);
    if (auto v = get("Audio.MasterVolume")) c.masterVolume = std::clamp(std::atoi(v->c_str()), 0, 100);
    if (auto v = get("Paths.ISO")) c.isoPath = *v;
    if (auto v = get("Launcher.Show")) c.showLauncher = parseBool(*v, c.showLauncher);
    return c;
}

bool kzSaveConfig(const std::filesystem::path &path, const KzConfig &c)
{
    std::ostringstream o;
    o << "; Killzone PC settings. Edited by the launcher; safe to edit by hand.\n\n"
      << "[Display]\nWindowMode=" << toName(kWindowModes, c.windowMode) << "\nWidth=" << c.width << "\nHeight=" << c.height
      << "\nVSync=" << (c.vsync ? 1 : 0) << "\nFpsLimit=" << c.fpsLimit << "\n\n"
      << "[Graphics]\nRenderer=" << toName(kRenderers, c.renderer) << "\nUpscale=" << c.upscale
      << "\nAspect=" << toName(kAspects, c.aspect) << "\nAnisotropy=" << c.anisotropy << "\nFXAA=" << (c.fxaa ? 1 : 0) << "\nSMAA=" << (c.smaa ? 1 : 0) << "\nTextureReplacement=" << (c.textureReplacement ? 1 : 0)
      << "\nBilinear=" << (c.bilinear ? 1 : 0) << "\nSharpScaling=" << (c.sharpScaling ? 1 : 0) << "\nSharpen=" << c.sharpen
      << "\nNoiseFilter=" << (c.noiseFilter ? 1 : 0) << "\nMotionBlur=" << (c.motionBlur ? 1 : 0)
      << "\nGlow=" << (c.glow ? 1 : 0) << "\nLensBlur=" << (c.lensBlur ? 1 : 0)
      << "\nRenderDistance=" << c.renderDistance << "\nLodScale=" << c.lodScale << "\n\n"
      << "[Input]\nMouseSensitivity=" << c.mouseSensitivity << "\nInvertY=" << (c.invertY ? 1 : 0)
      << "\nRawMouse=" << (c.rawMouse ? 1 : 0) << "\nAimMode=" << (c.holdAim ? "Hold" : "Toggle")
      << "\nStickDeadzone=" << c.stickDeadzone << "\n\n"
      << "[Audio]\nMasterVolume=" << c.masterVolume << "\n\n"
      << "[Paths]\nISO=" << c.isoPath << "\n\n"
      << "[Launcher]\nShow=" << (c.showLauncher ? 1 : 0) << "\n\n";
    // Key/mouse bindings: keep the user's section (read before the file is rewritten), or write the defaults.
    o << "[Bindings]\n; Key=Target. Targets: pad buttons (Cross Circle Square Triangle L1 R1 L2 R2 L3 R3 Start Select\n"
      << "; Up Down Left Right) or MoveForward MoveBack StrafeLeft StrafeRight LookUp LookDown LookLeft LookRight.\n"
      << "; Mouse1 left, Mouse2 right, Mouse3 middle, Mouse4/Mouse5 side, WheelUp/WheelDown; keys by SDL key name.\n";
    o << "Version=3\n";
    for (const auto &[key, target] : kzInputBindingPairs(path))
        o << key << "=" << target << "\n";
    std::ofstream out(path, std::ios::trunc);
    if (!out)
        return false;
    out << o.str();
    return static_cast<bool>(out);
}

KzConfig &kzConfig()
{
    static KzConfig cfg;
    return cfg;
}
