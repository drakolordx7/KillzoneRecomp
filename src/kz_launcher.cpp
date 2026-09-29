#include "kz_launcher.h"

#include "kz_iso.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    constexpr int kWidth = 760;
    constexpr int kHeight = 620;

    struct Resolution
    {
        int w, h;
    };

    std::vector<Resolution> desktopResolutions()
    {
        std::vector<Resolution> out;
        int count = 0;
        SDL_DisplayID display = SDL_GetPrimaryDisplay();
        if (SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(display, &count))
        {
            for (int i = 0; i < count; ++i)
            {
                Resolution r{modes[i]->w, modes[i]->h};
                if (std::none_of(out.begin(), out.end(), [&](const Resolution &o) { return o.w == r.w && o.h == r.h; }))
                    out.push_back(r);
            }
            SDL_free(modes);
        }
        if (out.empty())
            out = {{1920, 1080}, {2560, 1440}, {3840, 2160}, {1280, 720}};
        return out;
    }

    // Charcoal + Helghast red accent.
    void applyStyle()
    {
        ImGuiStyle &s = ImGui::GetStyle();
        s.WindowRounding = 0.0f;
        s.FrameRounding = 4.0f;
        s.GrabRounding = 4.0f;
        s.FramePadding = ImVec2(10, 6);
        s.ItemSpacing = ImVec2(10, 8);
        ImVec4 *c = s.Colors;
        const ImVec4 bg(0.105f, 0.105f, 0.11f, 1.0f), panel(0.15f, 0.15f, 0.16f, 1.0f), accent(0.78f, 0.20f, 0.14f, 1.0f),
            accentHi(0.90f, 0.28f, 0.20f, 1.0f), text(0.92f, 0.91f, 0.89f, 1.0f), dim(0.60f, 0.59f, 0.57f, 1.0f);
        c[ImGuiCol_WindowBg] = bg;
        c[ImGuiCol_ChildBg] = panel;
        c[ImGuiCol_PopupBg] = panel;
        c[ImGuiCol_Text] = text;
        c[ImGuiCol_TextDisabled] = dim;
        c[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.20f, 0.21f, 1.0f);
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.25f, 0.25f, 0.26f, 1.0f);
        c[ImGuiCol_FrameBgActive] = ImVec4(0.28f, 0.28f, 0.29f, 1.0f);
        c[ImGuiCol_Button] = ImVec4(0.22f, 0.22f, 0.23f, 1.0f);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.30f, 0.31f, 1.0f);
        c[ImGuiCol_ButtonActive] = accent;
        c[ImGuiCol_Header] = ImVec4(0.24f, 0.24f, 0.25f, 1.0f);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.30f, 0.30f, 0.31f, 1.0f);
        c[ImGuiCol_HeaderActive] = accent;
        c[ImGuiCol_CheckMark] = accentHi;
        c[ImGuiCol_SliderGrab] = accent;
        c[ImGuiCol_SliderGrabActive] = accentHi;
        c[ImGuiCol_Tab] = panel;
        c[ImGuiCol_TabHovered] = ImVec4(0.30f, 0.30f, 0.31f, 1.0f);
        c[ImGuiCol_TabSelected] = accent;
        c[ImGuiCol_Separator] = ImVec4(0.25f, 0.25f, 0.26f, 1.0f);
    }

    struct LauncherState
    {
        KzConfig &cfg;
        std::vector<Resolution> resolutions;
        KzDiscCheck disc;
        std::string checkedPath = std::string(1, ''); // sentinel: forces a check on the first frame
        std::mutex dialogMutex;
        std::string pickedPath; // set by the async file dialog callback
        bool play = false;
        bool quit = false;
    };

    void onFileChosen(void *userdata, const char *const *files, int)
    {
        auto *st = static_cast<LauncherState *>(userdata);
        if (files && files[0])
        {
            std::lock_guard<std::mutex> lock(st->dialogMutex);
            st->pickedPath = files[0];
        }
    }

    template <typename E>
    bool enumCombo(const char *label, E &value, const std::vector<std::pair<E, const char *>> &items)
    {
        const char *current = items.front().second;
        for (const auto &[v, n] : items)
            if (v == value)
                current = n;
        bool changed = false;
        if (ImGui::BeginCombo(label, current))
        {
            for (const auto &[v, n] : items)
                if (ImGui::Selectable(n, v == value))
                {
                    value = v;
                    changed = true;
                }
            ImGui::EndCombo();
        }
        return changed;
    }

    void drawUi(LauncherState &st, SDL_Window *window)
    {
        KzConfig &cfg = st.cfg;
        {
            std::lock_guard<std::mutex> lock(st.dialogMutex);
            if (!st.pickedPath.empty())
            {
                cfg.isoPath = st.pickedPath;
                st.pickedPath.clear();
            }
        }
        if (cfg.isoPath != st.checkedPath)
        {
            st.disc = kzCheckDisc(cfg.isoPath);
            st.checkedPath = cfg.isoPath;
        }

        const ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::Begin("launcher", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

        ImGui::SetWindowFontScale(1.6f);
        ImGui::TextUnformatted("KILLZONE");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::SameLine();
        ImGui::TextDisabled("  PC port - settings");
        ImGui::Separator();

        // Disc
        ImGui::TextUnformatted("Game disc (your Killzone NTSC-U image)");
        char buf[1024];
        std::snprintf(buf, sizeof(buf), "%s", cfg.isoPath.c_str());
        ImGui::SetNextItemWidth(-110);
        if (ImGui::InputText("##iso", buf, sizeof(buf)))
            cfg.isoPath = buf;
        ImGui::SameLine();
        if (ImGui::Button("Browse...", ImVec2(100, 0)))
        {
            static const SDL_DialogFileFilter filters[] = {{"PS2 disc image", "iso"}, {"All files", "*"}};
            SDL_ShowOpenFileDialog(onFileChosen, &st, window, filters, 2, nullptr, false);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, st.disc.ok ? ImVec4(0.45f, 0.80f, 0.45f, 1) : ImVec4(0.90f, 0.45f, 0.35f, 1));
        ImGui::TextWrapped("%s", st.disc.message.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();

        if (ImGui::BeginTabBar("tabs"))
        {
            if (ImGui::BeginTabItem("Display"))
            {
                enumCombo<KzWindowMode>("Window mode", cfg.windowMode,
                                        {{KzWindowMode::Borderless, "Borderless fullscreen"},
                                         {KzWindowMode::Fullscreen, "Exclusive fullscreen"},
                                         {KzWindowMode::Windowed, "Windowed"}});
                std::string resLabel = cfg.width == 0 ? "Desktop" : std::to_string(cfg.width) + " x " + std::to_string(cfg.height);
                if (ImGui::BeginCombo("Resolution", resLabel.c_str()))
                {
                    if (ImGui::Selectable("Desktop", cfg.width == 0))
                        cfg.width = cfg.height = 0;
                    for (const Resolution &r : st.resolutions)
                    {
                        const std::string l = std::to_string(r.w) + " x " + std::to_string(r.h);
                        if (ImGui::Selectable(l.c_str(), cfg.width == r.w && cfg.height == r.h))
                        {
                            cfg.width = r.w;
                            cfg.height = r.h;
                        }
                    }
                    ImGui::EndCombo();
                }
                static const int kFps[] = {0, 60, 120, 144, 165, 240, 360};
                std::string fpsLabel = cfg.fpsLimit == 0 ? "Unlimited" : std::to_string(cfg.fpsLimit);
                if (ImGui::BeginCombo("Frame rate limit", fpsLabel.c_str()))
                {
                    for (int f : kFps)
                        if (ImGui::Selectable(f == 0 ? "Unlimited" : std::to_string(f).c_str(), cfg.fpsLimit == f))
                            cfg.fpsLimit = f;
                    ImGui::EndCombo();
                }
                ImGui::Checkbox("V-Sync", &cfg.vsync);
                ImGui::TextDisabled("High refresh rate: the game runs its simulation on real frame time, so any\n"
                                    "limit above 60 renders and plays at that rate.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Graphics"))
            {
                enumCombo<KzRenderer>("Renderer", cfg.renderer,
                                      {{KzRenderer::D3D11, "Direct3D 11"}, {KzRenderer::D3D12, "Direct3D 12"}, {KzRenderer::Vulkan, "Vulkan"}});
                const char *scales[] = {"Auto (match display)", "1x (native 640x448)", "2x", "3x (~1080p)", "4x (~1440p)",
                                        "5x", "6x (~4K)", "7x", "8x"};
                ImGui::Combo("Internal resolution", &cfg.upscale, scales, IM_ARRAYSIZE(scales));
                enumCombo<KzAspect>("Aspect ratio", cfg.aspect,
                                    {{KzAspect::Widescreen16x9, "16:9 widescreen (native)"},
                                     {KzAspect::Classic4x3, "4:3 original"},
                                     {KzAspect::Stretch, "Stretch to window"}});
                const int anisoValues[] = {0, 2, 4, 8, 16};
                const char *aniso[] = {"Off", "2x", "4x", "8x", "16x"};
                int anisoIdx = static_cast<int>(std::find(std::begin(anisoValues), std::end(anisoValues), cfg.anisotropy) - std::begin(anisoValues));
                if (anisoIdx >= 5) anisoIdx = 4;
                if (ImGui::Combo("Anisotropic filtering", &anisoIdx, aniso, 5))
                    cfg.anisotropy = anisoValues[anisoIdx];
                ImGui::Checkbox("Bilinear texture filtering", &cfg.bilinear);
                ImGui::Checkbox("FXAA anti-aliasing", &cfg.fxaa);
                ImGui::Checkbox("Film-grain noise filter (original look)", &cfg.noiseFilter);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Controls"))
            {
                ImGui::SliderFloat("Mouse sensitivity", &cfg.mouseSensitivity, 0.1f, 10.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
                ImGui::Checkbox("Invert mouse Y", &cfg.invertY);
                ImGui::Checkbox("Raw mouse input", &cfg.rawMouse);
                ImGui::SliderFloat("Controller stick deadzone", &cfg.stickDeadzone, 0.0f, 0.5f, "%.2f");
                ImGui::Spacing();
                ImGui::TextDisabled("Keyboard / mouse bindings live in killzone.ini, [Bindings] section\n"
                                    "(Key=Action, e.g. Mouse1=R1, W=MoveForward). Controllers are detected automatically.");
                if (ImGui::Button("Open killzone.ini"))
                {
                    kzSaveConfig(kzConfigPath(), cfg);
                    const std::string url = "file:///" + kzConfigPath().generic_string();
                    SDL_OpenURL(url.c_str());
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Audio"))
            {
                ImGui::SliderInt("Master volume", &cfg.masterVolume, 0, 100, "%d%%");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        // Footer
        const float footerY = ImGui::GetWindowHeight() - 56.0f;
        ImGui::SetCursorPosY(footerY);
        ImGui::Separator();
        ImGui::Checkbox("Show this launcher at startup", &cfg.showLauncher);
        ImGui::SameLine(ImGui::GetWindowWidth() - 250);
        if (ImGui::Button("Quit", ImVec2(110, 32)))
            st.quit = true;
        ImGui::SameLine();
        ImGui::BeginDisabled(!st.disc.ok);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.78f, 0.20f, 0.14f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f, 0.28f, 0.20f, 1));
        if (ImGui::Button("PLAY", ImVec2(110, 32)))
            st.play = true;
        ImGui::PopStyleColor(2);
        ImGui::EndDisabled();
        ImGui::End();
    }

    struct Session
    {
        SDL_Window *window = nullptr;
        SDL_Renderer *renderer = nullptr;
        bool ok = false;
    };

    Session openSession(bool hidden)
    {
        Session s;
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
            return s;
        SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY | (hidden ? SDL_WINDOW_HIDDEN : 0);
        s.window = SDL_CreateWindow("Killzone - Launcher", kWidth, kHeight, flags);
        if (!s.window)
            return s;
        s.renderer = SDL_CreateRenderer(s.window, hidden ? "software" : nullptr);
        if (!s.renderer)
            return s;
        SDL_SetRenderVSync(s.renderer, 1);
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        // System UI font; ImGui's built-in bitmap font is the fallback.
        if (SDL_GetPathInfo("C:/Windows/Fonts/segoeui.ttf", nullptr))
            ImGui::GetIO().Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", 18.0f);
        applyStyle();
        ImGui_ImplSDL3_InitForSDLRenderer(s.window, s.renderer);
        ImGui_ImplSDLRenderer3_Init(s.renderer);
        s.ok = true;
        return s;
    }

    void closeSession(Session &s)
    {
        if (s.ok)
        {
            ImGui_ImplSDLRenderer3_Shutdown();
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();
        }
        if (s.renderer)
            SDL_DestroyRenderer(s.renderer);
        if (s.window)
            SDL_DestroyWindow(s.window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }

    void renderFrame(LauncherState &st, Session &s)
    {
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        drawUi(st, s.window);
        ImGui::Render();
        SDL_SetRenderDrawColor(s.renderer, 27, 27, 28, 255);
        SDL_RenderClear(s.renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), s.renderer);
    }
}

bool kzRunLauncher(KzConfig &cfg)
{
    Session s = openSession(false);
    if (!s.ok)
    {
        SDL_Log("[launcher] could not open window: %s", SDL_GetError());
        closeSession(s);
        return !cfg.isoPath.empty(); // fall through to the game with saved settings
    }
    LauncherState st{cfg};
    st.resolutions = desktopResolutions();
    while (!st.play && !st.quit)
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                st.quit = true;
        }
        renderFrame(st, s);
        SDL_RenderPresent(s.renderer);
    }
    kzSaveConfig(kzConfigPath(), cfg);
    closeSession(s);
    return st.play;
}

bool kzLauncherScreenshot(KzConfig &cfg, const char *pngPath)
{
    Session s = openSession(true);
    bool ok = false;
    if (s.ok)
    {
        LauncherState st{cfg};
        st.resolutions = desktopResolutions();
        for (int i = 0; i < 3; ++i) // let ImGui settle layout
            renderFrame(st, s);
        if (SDL_Surface *surf = SDL_RenderReadPixels(s.renderer, nullptr))
        {
            ok = SDL_SavePNG(surf, pngPath);
            SDL_DestroySurface(surf);
        }
    }
    closeSession(s);
    return ok;
}
