# kzgs - PCSX2's hardware GS renderer as a standalone static library (Windows x64, MSVC).
#
# Standalone:   cmake -S ext/kzgs -B ext/kzgs/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
# As a subdir:  add_subdirectory(ext/kzgs) ; target_link_libraries(game PRIVATE kzgs) ; kzgs_stage_resources(game)
#
# GPL-3.0+: this links PCSX2 code, so anything that links kzgs is a GPL-3 derivative work.

cmake_minimum_required(VERSION 3.24)
project(kzgs C CXX ASM_MASM)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "kzgs only supports MSVC x64.")
endif()

set(KZGS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(KZGS_PCSX2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pcsx2" CACHE PATH "PCSX2 source tree")
set(KZGS_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/deps" CACHE PATH "Extracted pcsx2-windows-dependencies bundle")
set(KZGS_ISA "AVX2" CACHE STRING "Vector ISA the GS is compiled for (SSE4, AVX, AVX2)")
set_property(CACHE KZGS_ISA PROPERTY STRINGS SSE4 AVX AVX2)
option(KZGS_BUILD_TEST "Build the kzgs_test executable" ${PROJECT_IS_TOP_LEVEL})
option(KZGS_BUILD_TOOLS "Build kzgs debug tools (kzgs_replay)" ${PROJECT_IS_TOP_LEVEL})

get_filename_component(KZGS_PCSX2_DIR "${KZGS_PCSX2_DIR}" ABSOLUTE)
set(P2 "${KZGS_PCSX2_DIR}")
set(P2SRC "${P2}/pcsx2")
set(P3 "${P2}/3rdparty")
set(DEPS "${KZGS_DEPS_DIR}")

if(NOT EXISTS "${P2SRC}/GS/GS.cpp")
	message(FATAL_ERROR "PCSX2 source not found at ${P2}")
endif()
if(NOT EXISTS "${DEPS}/include/zstd.h")
	message(FATAL_ERROR "Dependency bundle not found at ${DEPS}. Download pcsx2-windows-dependencies.7z from "
		"https://github.com/PCSX2/pcsx2-windows-dependencies/releases/tag/latest-windows-dependencies and extract to ext/kzgs/deps")
endif()

# ---------------------------------------------------------------------------------------------------------------------
# Shared compile flags (private to kzgs; nothing here leaks into consumers).
# ---------------------------------------------------------------------------------------------------------------------
add_library(kzgs_flags INTERFACE)
target_compile_features(kzgs_flags INTERFACE cxx_std_20)
target_compile_definitions(kzgs_flags INTERFACE
	_M_X86=1
	__SSE4_1__=1
	WINVER=0x0A00
	_WIN32_WINNT=0x0A00
	WIN32_LEAN_AND_MEAN
	NOMINMAX
	WIL_SUPPRESS_EXCEPTIONS
	_CRT_NONSTDC_NO_WARNINGS
	_CRT_SECURE_NO_WARNINGS
	CRT_SECURE_NO_DEPRECATE
	_SCL_SECURE_NO_WARNINGS
	_UNICODE
	UNICODE
	ENABLE_VULKAN
	XBYAK_NO_EXCEPTION
	IMGUI_USER_CONFIG="kzgs_imconfig.h"
	D3D12MA_USING_DIRECTX_HEADERS
)
target_compile_options(kzgs_flags INTERFACE
	"$<$<COMPILE_LANGUAGE:CXX>:/Zc:externConstexpr;/Zc:__cplusplus;/permissive-;/Zc:preprocessor>"
	"$<$<COMPILE_LANGUAGE:C,CXX>:/utf-8;/Zo;/GS-;/fp:contract;/W1;/wd4005;/wd4996>"
)
if(KZGS_ISA STREQUAL "AVX2")
	target_compile_options(kzgs_flags INTERFACE $<$<COMPILE_LANGUAGE:C,CXX>:/arch:AVX2>)
elseif(KZGS_ISA STREQUAL "AVX")
	target_compile_options(kzgs_flags INTERFACE $<$<COMPILE_LANGUAGE:C,CXX>:/arch:AVX>)
endif()
target_include_directories(kzgs_flags INTERFACE
	"${KZGS_ROOT}/shim/include"
	"${P2}"
	"${P2SRC}"
	"${P3}/include"
	"${P3}/xbyak"
	"${P3}/fmt/include"
	"${P3}/fast_float/include"
	"${P3}/imgui/include"
	"${P3}/lzma/include"
	"${P3}/cpuinfo/include"
	"${P3}/d3d12memalloc/include"
	"${P3}/vulkan/include"
	"${P3}/winwil/include"
	"${P3}/winpixeventruntime/include"
	"${P3}/simpleini/include"
	"${DEPS}/include"
)

# ---------------------------------------------------------------------------------------------------------------------
# Dear ImGui isolation. The game links its own (different) Dear ImGui, so kzgs' private copy is renamed at the
# preprocessor level: every ImGui/Im*/GImGui identifier declared in PCSX2's imgui headers becomes Kz<name> in all kzgs
# C++ translation units (forced include), giving distinct link symbols and a separate GImGui context pointer.
# ---------------------------------------------------------------------------------------------------------------------
set(KZGS_IMGUI_RENAME_H "${CMAKE_CURRENT_BINARY_DIR}/kzgs_generated/kzgs_imgui_rename.h")
set(_kz_ids "")
set(_kz_macros "")
# Sources are scanned too: they declare file-local types with external linkage (e.g. ImTriangulator).
foreach(_h include/imgui.h include/imgui_internal.h include/imgui_freetype.h include/imgui_stdlib.h
		src/imgui.cpp src/imgui_draw.cpp src/imgui_tables.cpp src/imgui_widgets.cpp src/imgui_stdlib.cpp)
	set(_path "${P3}/imgui/${_h}")
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_path}")
	file(READ "${_path}" _content)
	# ';' and brackets have list semantics in CMake; neutralize them before regex matching.
	string(REPLACE ";" " " _content "${_content}")
	string(REPLACE "[" " " _content "${_content}")
	string(REPLACE "]" " " _content "${_content}")
	string(REPLACE "\\" " " _content "${_content}")
	string(REGEX MATCHALL "#[ \t]*define[ \t]+(GImGui|Im[A-Z][A-Za-z0-9_]*)" _defs "${_content}")
	foreach(_d IN LISTS _defs)
		string(REGEX REPLACE "#[ \t]*define[ \t]+" "" _d "${_d}")
		list(APPEND _kz_macros "${_d}")
	endforeach()
	string(REGEX MATCHALL "[^A-Za-z0-9_](GImGui[A-Za-z0-9_]*|Im[A-Z][A-Za-z0-9_]*)" _m "${_content}")
	foreach(_id IN LISTS _m)
		string(SUBSTRING "${_id}" 1 -1 _id)
		string(STRIP "${_id}" _id)
		list(APPEND _kz_ids "${_id}")
	endforeach()
endforeach()
# Names imgui tests with #ifndef/#ifdef/defined() are user-overridable hooks (typedefs like ImDrawIdx/ImTextureID and
# the inline ImQsort); renaming them would make imgui skip their definitions. They produce no link symbols, so they
# are left alone. GImGui is the exception: it is renamed and declared/defined by kzgs (see below / ImGuiShim.cpp).
foreach(_f "${P3}/imgui/include/imgui.h" "${P3}/imgui/include/imgui_internal.h" "${P3}/imgui/src/imgui.cpp"
		"${P3}/imgui/src/imgui_draw.cpp" "${P3}/imgui/src/imgui_widgets.cpp" "${P3}/imgui/src/imgui_tables.cpp")
	file(READ "${_f}" _content)
	string(REPLACE ";" " " _content "${_content}")
	string(REGEX MATCHALL "(#[ \t]*ifn?def[ \t]+|defined[ \t]*[(]?[ \t]*)Im[A-Z][A-Za-z0-9_]*" _guards "${_content}")
	foreach(_g IN LISTS _guards)
		string(REGEX REPLACE "^.*[^A-Za-z0-9_](Im[A-Z][A-Za-z0-9_]*)$" "\\1" _g "${_g}")
		list(APPEND _kz_macros "${_g}")
	endforeach()
endforeach()
list(REMOVE_DUPLICATES _kz_ids)
list(REMOVE_ITEM _kz_ids ${_kz_macros})
list(APPEND _kz_ids GImGui) # always renamed (imgui.cpp comments show "#define GImGui ..." as an example)
list(REMOVE_DUPLICATES _kz_ids)
list(SORT _kz_ids)
list(LENGTH _kz_ids _kz_count)
set(_kz_text "// Generated by ext/kzgs/CMakeLists.txt - renames kzgs' private Dear ImGui (${_kz_count} identifiers).\n#pragma once\n")
foreach(_id IN LISTS _kz_ids)
	string(APPEND _kz_text "#define ${_id} Kz${_id}\n")
endforeach()
# imgui declares/defines GImGui only when it is not a macro; declare it here, ImGuiShim.cpp defines it.
string(APPEND _kz_text "struct ImGuiContext;
extern ImGuiContext* GImGui;
")
file(CONFIGURE OUTPUT "${KZGS_IMGUI_RENAME_H}" CONTENT "${_kz_text}" @ONLY)
target_compile_options(kzgs_flags INTERFACE "$<$<COMPILE_LANGUAGE:CXX>:/FI${KZGS_IMGUI_RENAME_H}>")

# ---------------------------------------------------------------------------------------------------------------------
# 3rdparty (compiled from the in-tree copies PCSX2 ships)
# ---------------------------------------------------------------------------------------------------------------------
file(GLOB KZGS_LZMA_SRCS "${P3}/lzma/src/*.c")
add_library(kzgs_3rdparty OBJECT
	${P3}/fmt/src/format.cc
	${P3}/fmt/src/os.cc
	${P3}/imgui/src/imgui.cpp
	${P3}/imgui/src/imgui_draw.cpp
	${P3}/imgui/src/imgui_tables.cpp
	${P3}/imgui/src/imgui_widgets.cpp
	${P3}/imgui/src/imgui_stdlib.cpp
	${P3}/d3d12memalloc/src/D3D12MemAlloc.cpp
	${P3}/simpleini/src/ConvertUTF.c
	${P3}/cpuinfo/src/api.c
	${P3}/cpuinfo/src/cache.c
	${P3}/cpuinfo/src/init.c
	${P3}/cpuinfo/src/log.c
	${P3}/cpuinfo/src/x86/init.c
	${P3}/cpuinfo/src/x86/info.c
	${P3}/cpuinfo/src/x86/vendor.c
	${P3}/cpuinfo/src/x86/uarch.c
	${P3}/cpuinfo/src/x86/name.c
	${P3}/cpuinfo/src/x86/topology.c
	${P3}/cpuinfo/src/x86/isa.c
	${P3}/cpuinfo/src/x86/cache/init.c
	${P3}/cpuinfo/src/x86/cache/descriptor.c
	${P3}/cpuinfo/src/x86/cache/deterministic.c
	${P3}/cpuinfo/src/x86/windows/init.c
	${KZGS_LZMA_SRCS}
)
target_link_libraries(kzgs_3rdparty PRIVATE kzgs_flags)
target_include_directories(kzgs_3rdparty PRIVATE "${P3}/cpuinfo/src" "${P3}/cpuinfo/deps/clog/include" "${P3}/imgui/src")
target_compile_definitions(kzgs_3rdparty PRIVATE CPUINFO_LOG_LEVEL=0 CPUINFO_LOG_TO_STDIO=0 _7ZIP_ST Z7_ST)
target_compile_options(kzgs_3rdparty PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:/W0>)
set_target_properties(kzgs_3rdparty PROPERTIES C_STANDARD 11)

# ---------------------------------------------------------------------------------------------------------------------
# pcsx2/common slice
# ---------------------------------------------------------------------------------------------------------------------
add_library(kzgs_common OBJECT
	${P2}/common/AlignedMalloc.cpp
	${P2}/common/Assertions.cpp
	${P2}/common/Console.cpp
	${P2}/common/DynamicLibrary.cpp
	${P2}/common/Error.cpp
	${P2}/common/FileSystem.cpp
	${P2}/common/HostSys.cpp
	${P2}/common/Image.cpp
	${P2}/common/MD5Digest.cpp
	${P2}/common/MemorySettingsInterface.cpp
	${P2}/common/Perf.cpp
	${P2}/common/ProgressCallback.cpp
	${P2}/common/ReadbackSpinManager.cpp
	${P2}/common/Semaphore.cpp
	${P2}/common/SettingsWrapper.cpp
	${P2}/common/SmallString.cpp
	${P2}/common/StringUtil.cpp
	${P2}/common/TextureDecompress.cpp
	${P2}/common/Timer.cpp
	${P2}/common/WindowInfo.cpp
	${P2}/common/Windows/WinThreads.cpp
	${P2}/common/Windows/WinHostSys.cpp
	${P2}/common/Windows/WinMisc.cpp
	${P2}/common/FastJmp.asm
)
target_link_libraries(kzgs_common PRIVATE kzgs_flags)
target_precompile_headers(kzgs_common PRIVATE "${P2}/common/PrecompiledHeader.h")

# ---------------------------------------------------------------------------------------------------------------------
# pcsx2/GS (hardware renderers D3D11/D3D12/Vulkan; the SW renderer is compiled too because GS.cpp and the HW
# renderer's CPU sprite paths reference it, but OpenGL and Metal are left out)
# ---------------------------------------------------------------------------------------------------------------------
set(KZGS_GS_SRCS
	GS/GS.cpp
	GS/GSCapture.cpp
	GS/GSClut.cpp
	GS/GSDrawingContext.cpp
	GS/GSDrawingEnvironment.cpp
	GS/GSDump.cpp
	GS/GSLocalMemory.cpp
	GS/GSLzma.cpp
	GS/GSPerfMon.cpp
	GS/GSPng.cpp
	GS/GSRingHeap.cpp
	GS/GSState.cpp
	GS/GSTables.cpp
	GS/GSUtil.cpp
	GS/GSVector.cpp
	GS/MultiISA.cpp
	GS/Renderers/Common/GSDevice.cpp
	GS/Renderers/Common/GSDirtyRect.cpp
	GS/Renderers/Common/GSFunctionMap.cpp
	GS/Renderers/Common/GSRenderer.cpp
	GS/Renderers/Common/GSTexture.cpp
	GS/Renderers/Common/GSVertexTrace.cpp
	GS/Renderers/Null/GSRendererNull.cpp
	GS/Renderers/HW/GSHwHack.cpp
	GS/Renderers/HW/GSRendererHW.cpp
	GS/Renderers/HW/GSTextureCache.cpp
	GS/Renderers/HW/GSTextureReplacementLoaders.cpp
	GS/Renderers/HW/GSTextureReplacements.cpp
	GS/Renderers/SW/GSTextureCacheSW.cpp
	# "unshared" (per-ISA) sources, compiled once for KZGS_ISA
	GS/GSBlock.cpp
	GS/GSLocalMemoryMultiISA.cpp
	GS/GSXXH.cpp
	GS/Renderers/Common/GSVertexTraceFMM.cpp
	GS/Renderers/HW/GSRendererHWMultiISA.cpp
	GS/Renderers/SW/GSDrawScanline.cpp
	GS/Renderers/SW/GSRasterizer.cpp
	GS/Renderers/SW/GSRendererSW.cpp
	GS/Renderers/SW/GSDrawScanlineCodeGenerator.all.cpp
	GS/Renderers/SW/GSSetupPrimCodeGenerator.all.cpp
	# D3D11
	GS/Renderers/DX11/D3D.cpp
	GS/Renderers/DX11/D3D11ShaderCache.cpp
	GS/Renderers/DX11/GSDevice11.cpp
	GS/Renderers/DX11/GSTexture11.cpp
	# D3D12
	GS/Renderers/DX12/D3D12Builders.cpp
	GS/Renderers/DX12/D3D12DescriptorHeapManager.cpp
	GS/Renderers/DX12/D3D12ShaderCache.cpp
	GS/Renderers/DX12/D3D12StreamBuffer.cpp
	GS/Renderers/DX12/GSDevice12.cpp
	GS/Renderers/DX12/GSTexture12.cpp
	# Vulkan
	GS/Renderers/Vulkan/GSDeviceVK.cpp
	GS/Renderers/Vulkan/GSTextureVK.cpp
	GS/Renderers/Vulkan/vk_mem_alloc.cpp
	GS/Renderers/Vulkan/VKBuilders.cpp
	GS/Renderers/Vulkan/VKLoader.cpp
	GS/Renderers/Vulkan/VKShaderCache.cpp
	GS/Renderers/Vulkan/VKStreamBuffer.cpp
	GS/Renderers/Vulkan/VKSwapChain.cpp
	# Emulator config structs (GSOptions, EmuConfig, EmuFolders) - compiled for real
	Pcsx2Config.cpp
	SourceLog.cpp
)
list(TRANSFORM KZGS_GS_SRCS PREPEND "${P2SRC}/")

file(GLOB KZGS_SHIM_SRCS CONFIGURE_DEPENDS "${KZGS_ROOT}/shim/*.cpp")
add_library(kzgs_gs OBJECT ${KZGS_GS_SRCS} ${KZGS_SHIM_SRCS})
target_link_libraries(kzgs_gs PRIVATE kzgs_flags)
target_precompile_headers(kzgs_gs PRIVATE "${P2SRC}/PrecompiledHeader.h")

# ---------------------------------------------------------------------------------------------------------------------
# kzgs: the public static library
# ---------------------------------------------------------------------------------------------------------------------
add_library(kzgs STATIC
	"${KZGS_ROOT}/src/kzgs.cpp"
	"${KZGS_ROOT}/src/kz_smaa11.cpp"
	"${KZGS_ROOT}/src/kz_smaa_vt.cpp"
	"${KZGS_ROOT}/include/kzgs.h"
	$<TARGET_OBJECTS:kzgs_3rdparty>
	$<TARGET_OBJECTS:kzgs_common>
	$<TARGET_OBJECTS:kzgs_gs>
)
target_link_libraries(kzgs PRIVATE kzgs_flags)
target_precompile_headers(kzgs PRIVATE "${P2SRC}/PrecompiledHeader.h")
target_include_directories(kzgs PUBLIC "${KZGS_ROOT}/include")
target_include_directories(kzgs PRIVATE "${KZGS_ROOT}/third_party/smaa")

# Import libraries for the bundle DLLs + system libs. The bundle DLLs are delay-loaded so a D3D11-only run does not
# need dxcompiler.dll, and PNG/WebP/zstd are only touched by screenshots, dumps and texture replacement.
target_link_libraries(kzgs INTERFACE
	"${DEPS}/lib/zstd.lib"
	"${DEPS}/lib/libpng16.lib"
	"${DEPS}/lib/libwebp.lib"
	"${DEPS}/lib/jpeg.lib"
	"${DEPS}/lib/dxcompiler.lib"
	"${DEPS}/lib/DirectX-Guids.lib"
	d3d11.lib d3d12.lib dxgi.lib d3dcompiler.lib dxguid.lib
	winmm.lib pathcch.lib shlwapi.lib dwmapi.lib PowrProf.lib OneCore.lib comsuppw.lib
	delayimp.lib
)
target_link_options(kzgs INTERFACE
	/DELAYLOAD:zstd.dll /DELAYLOAD:libpng16.dll /DELAYLOAD:libwebp.dll /DELAYLOAD:jpeg62.dll /DELAYLOAD:dxcompiler.dll
)

set(KZGS_RUNTIME_DLLS
	"${DEPS}/bin/zstd.dll"
	"${DEPS}/bin/libpng16.dll"
	"${DEPS}/bin/z.dll"
	"${DEPS}/bin/libwebp.dll"
	"${DEPS}/bin/jpeg62.dll"
	"${DEPS}/bin/libsharpyuv.dll"
	"${DEPS}/bin/dxcompiler.dll"
	"${DEPS}/bin/shaderc_shared.dll"
	CACHE INTERNAL "")
set(KZGS_RESOURCES_DIR "${P2}/bin/resources" CACHE INTERNAL "")
set(KZGS_SMAA_SHADERS_DIR "${KZGS_ROOT}/shaders" CACHE INTERNAL "")

# kzgs_stage_resources(<target>): after <target> links, copy the GS shaders to <exe dir>/resources/shaders and the
# runtime DLLs next to the exe. kzgsOpen() looks in <exe dir>/resources unless KzgsConfig::resourcesDir is set.
function(kzgs_stage_resources target)
	add_custom_command(TARGET ${target} POST_BUILD
		COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>/resources"
		COMMAND "${CMAKE_COMMAND}" -E copy_directory "${KZGS_RESOURCES_DIR}/shaders" "$<TARGET_FILE_DIR:${target}>/resources/shaders"
		COMMAND "${CMAKE_COMMAND}" -E copy_directory "${KZGS_SMAA_SHADERS_DIR}" "$<TARGET_FILE_DIR:${target}>/resources/shaders/kzgs"
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${KZGS_RUNTIME_DLLS} "$<TARGET_FILE_DIR:${target}>"
		COMMENT "kzgs: staging GS shaders and runtime DLLs for ${target}"
		VERBATIM)
endfunction()

if(KZGS_BUILD_TEST)
	add_executable(kzgs_test "${KZGS_ROOT}/test/kzgs_test.cpp")
	target_link_libraries(kzgs_test PRIVATE kzgs)
	target_compile_features(kzgs_test PRIVATE cxx_std_20)
	target_compile_definitions(kzgs_test PRIVATE KZGS_TEST_OUT_DIR="${KZGS_ROOT}/test")
	kzgs_stage_resources(kzgs_test)
endif()

if(KZGS_BUILD_TOOLS)
	# Debug replay of game GS traces with access to PCSX2 GS internals (compiled with kzgs' private flags).
	add_executable(kzgs_replay "${KZGS_ROOT}/tools/kzgs_replay.cpp")
	target_link_libraries(kzgs_replay PRIVATE kzgs kzgs_flags)
	target_precompile_headers(kzgs_replay PRIVATE "${P2SRC}/PrecompiledHeader.h")
	kzgs_stage_resources(kzgs_replay)
endif()
