cmake_minimum_required(VERSION 3.21)
project(KillzoneRecomp CXX C)

# Shipping build is RelWithDebInfo (symbols for crash reports). CMake's default for it is /O2 /Ob1 (no automatic
# inlining) with security cookies and incremental linking, which cost the hot generated code measurably (out-of-line
# memory helpers, __security_check_cookie in profiles). Keep strict float semantics (no /fp:fast: PS2 float
# emulation depends on it). KZ_FAST_FLAGS=OFF restores CMake's defaults.
option(KZ_FAST_FLAGS "Release-grade optimisation flags for RelWithDebInfo" ON)
if(MSVC AND KZ_FAST_FLAGS)
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "/Zi /O2 /Ob3 /Oi /Gy /Gw /GS- /Qspectre- /arch:AVX2 /DNDEBUG")
    set(CMAKE_C_FLAGS_RELWITHDEBINFO "/Zi /O2 /Ob3 /Oi /Gy /Gw /GS- /Qspectre- /arch:AVX2 /DNDEBUG")
    set(CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO "/DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF")
endif()

# PS2Recomp runtime + IOP only (recompiler/analyzer are built separately by tools/scripts/build_ps2recomp.bat)
set(PS2X_BUILD_RECOMP OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_ANALYZER OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_STUDIO OFF CACHE BOOL "" FORCE)
set(PS2X_ENABLE_AGRESSIVE_LOGS OFF CACHE BOOL "" FORCE)  # per-call enter/exit trace: ~500MB per 30s
add_subdirectory(ext/PS2Recomp)
add_subdirectory(ext/kzgs)   # PCSX2 hardware GS as a library (ext/kzgs/README.md)
add_subdirectory(ext/kzvu)   # PCSX2 microVU (VU1 recompiler) as a library (ext/kzvu/README.md)
add_subdirectory(ext/kzipu)  # PCSX2 IPU (MPEG decoder) as a library (ext/kzipu/README.md)
add_subdirectory(ext/kzspu2) # PCSX2 SPU2 (sound) as a library (ext/kzspu2/README.md)

# Generated code lives outside the repo (derived from the game ELF). Point KZ_GENERATED_DIR at it.
set(KZ_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated" CACHE PATH "PS2Recomp output for SCUS_974.02")
file(GLOB KZ_GENERATED_SRC CONFIGURE_DEPENDS "${KZ_GENERATED_DIR}/*.cpp")
list(LENGTH KZ_GENERATED_SRC KZ_GENERATED_COUNT)
message(STATUS "Killzone: ${KZ_GENERATED_COUNT} generated sources")

file(GLOB KZ_SRC CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")

# Our code lives in its own library so its dependencies (SDL3, ImGui, ...) never change the compile flags of the
# ~650 generated-code unity batches (a flag change there means a full 15-minute rebuild).
include(FetchContent)
FetchContent_GetProperties(imgui SOURCE_DIR KZ_IMGUI_DIR)
find_package(SDL3 CONFIG REQUIRED PATHS "${CMAKE_SOURCE_DIR}/ext/sdl3/SDL3-3.4.16/cmake" NO_DEFAULT_PATH)
add_library(kz_port STATIC
    ${KZ_SRC}
    "${KZ_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp"
    "${KZ_IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp")
target_include_directories(kz_port PRIVATE "${CMAKE_SOURCE_DIR}/src" "${KZ_IMGUI_DIR}" "${KZ_IMGUI_DIR}/backends")
target_link_libraries(kz_port PRIVATE ps2_runtime SDL3::SDL3 imgui kzgs kzvu kzipu kzspu2)
target_compile_definitions(kz_port PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

add_executable(killzone
    ${KZ_GENERATED_SRC}
)
target_include_directories(killzone PRIVATE "${KZ_GENERATED_DIR}" "${CMAKE_SOURCE_DIR}/src")
# kz_port holds main() and the game override registration; link it whole so static registrars are kept.
target_link_libraries(killzone PRIVATE ps2_runtime "$<LINK_LIBRARY:WHOLE_ARCHIVE,kz_port>")
set_target_properties(killzone PROPERTIES
    UNITY_BUILD ON
    UNITY_BUILD_BATCH_SIZE 128
)
target_precompile_headers(killzone PRIVATE
    ext/PS2Recomp/ps2xRuntime/include/ps2_runtime_macros.h
    ext/PS2Recomp/ps2xRuntime/include/ps2_runtime.h
    "${KZ_GENERATED_DIR}/ps2_recompiled_functions.h"
    "${KZ_GENERATED_DIR}/ps2_recompiled_stubs.h"
    ext/PS2Recomp/ps2xRuntime/include/ps2_syscalls.h
    ext/PS2Recomp/ps2xRuntime/include/ps2_stubs.h
)
if(MSVC)
    target_compile_options(killzone PRIVATE /bigobj /FS)
endif()

# Copy FFmpeg runtime DLLs next to killzone.exe (helper from ps2xRuntime).
ps2x_stage_ffmpeg_runtime_dlls(killzone)
kzgs_stage_resources(killzone)
add_custom_command(TARGET killzone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:killzone>)

# Offline GS trace replay (tools/gs_replay): renders a KZ_GS_TRACE recording through kzgs.
add_executable(kz_gs_replay tools/gs_replay/gs_replay.cpp)
target_link_libraries(kz_gs_replay PRIVATE kzgs SDL3::SDL3)
target_compile_features(kz_gs_replay PRIVATE cxx_std_20)
add_executable(kz_gs_replay_cpu tools/gs_replay/gs_replay_cpu.cpp)
target_link_libraries(kz_gs_replay_cpu PRIVATE ps2_runtime)
target_compile_features(kz_gs_replay_cpu PRIVATE cxx_std_20)
