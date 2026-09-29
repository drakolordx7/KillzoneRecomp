// kzgs shim: ImGuiManager / FullscreenUI replacements.
// The GS devices always call ImGui::Render() when presenting, so a live ImGui context is required. kzgs creates a
// minimal one (default ProggyClean font via stb_truetype, no OSD, no fullscreen UI, no input) so presents are cheap.
// SPDX-License-Identifier: GPL-3.0+

#include "ImGui/FullscreenUI.h"
#include "ImGui/ImGuiManager.h"
#include "GS/Renderers/Common/GSDevice.h"

#include "common/Timer.h"

#include "imgui.h"
#include "imgui_freetype.h"
#include "imgui_internal.h"

// GImGui is renamed to KzGImGui by kzgs_imgui_rename.h, which makes imgui.cpp skip its own definition.
ImGuiContext* GImGui = nullptr;

static Common::Timer s_last_render_time;

static void UpdateDisplaySize()
{
	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = ImVec2(g_gs_device ? static_cast<float>(g_gs_device->GetWindowWidth()) : 0.0f,
		g_gs_device ? static_cast<float>(g_gs_device->GetWindowHeight()) : 0.0f);
}

bool ImGuiManager::Initialize()
{
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.LogFilename = nullptr;
	io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
	io.DisplayFramebufferScale = ImVec2(1, 1);
	UpdateDisplaySize();
	io.Fonts->AddFontDefault();
	s_last_render_time.Reset();
	NewFrame();
	return true;
}

bool ImGuiManager::InitializeFullscreenUI()
{
	return false;
}

void ImGuiManager::Shutdown(bool clear_state)
{
	if (g_gs_device)
		g_gs_device->DestroyImGuiTextures();
	if (ImGui::GetCurrentContext())
		ImGui::DestroyContext();
}

void ImGuiManager::WindowResized()
{
	if (ImGui::GetCurrentContext())
		UpdateDisplaySize();
}

void ImGuiManager::RequestScaleUpdate() {}
void ImGuiManager::ReloadFonts() {}

void ImGuiManager::NewFrame()
{
	ImGuiIO& io = ImGui::GetIO();
	io.DeltaTime = std::max(static_cast<float>(s_last_render_time.GetTimeSecondsAndReset()), 1.0f / 1000.0f);
	ImGui::NewFrame();
}

void ImGuiManager::SkipFrame()
{
	ImGui::EndFrame();
	NewFrame();
}

void ImGuiManager::RenderOSD() {}

void FullscreenUI::Render() {}

// PCSX2's imconfig.h enables IMGUI_ENABLE_FREETYPE; kzgs_imconfig.h adds stb_truetype and this routes the default
// font loader to it, so imgui_freetype.cpp / FreeType are not linked.
const ImFontLoader* ImGuiFreeType::GetFontLoader()
{
	return ImFontAtlasGetFontLoaderForStbTruetype();
}
