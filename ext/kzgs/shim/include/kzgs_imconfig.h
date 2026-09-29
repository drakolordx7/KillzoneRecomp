// kzgs: extra Dear ImGui configuration (pulled in via IMGUI_USER_CONFIG before PCSX2's imconfig.h).
// PCSX2's imconfig.h enables the FreeType font loader; kzgs also enables stb_truetype and routes
// ImGuiFreeType::GetFontLoader() to it (shim/ImGuiStubs.cpp) so FreeType is not a dependency.
#pragma once
#define IMGUI_ENABLE_STB_TRUETYPE
