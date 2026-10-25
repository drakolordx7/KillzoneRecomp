cmake_minimum_required(VERSION 3.21)
project(KillzoneRecomp CXX C)

# Shipping build is RelWithDebInfo (symbols for crash reports). CMake's default for it is /O2 /Ob1 (no automatic
# inlining) with security cookies and incremental linking, which cost the hot generated code measurably (out-of-line
# memory helpers, __security_check_cookie in profiles). Keep strict float semantics (no /fp:fast: PS2 float
# emulation depends on it). KZ_FAST_FLAGS=OFF restores CMake's defaults.
option(KZ_FAST_FLAGS "Release-grade optimisation flags for RelWithDebInfo" ON)
# CPU baseline of the whole build (issue #8). Every library below takes its /arch flag from KZ_ARCH_FLAG.
#   MULTI (default): one exe for any x64 CPU with SSE4.1 (Intel Core 2 Penryn / AMD Bulldozer and newer), as PCSX2's
#         multi-ISA build: everything is compiled for the baseline (MSVC: no /arch, so SSE2 code generation with SSE4.1
#         intrinsics; clang-cl: x86-64 + SSE4.1), and PCSX2's per-ISA GS / IPU / SPU2 files are compiled three times
#         (SSE4.1, AVX, AVX2) and picked at run time from CPUID (ext/kzgs/CMakeLists.txt, "MULTI").
#   AVX2: everything for AVX2 (x86-64-v3: Haswell / Ryzen and newer).
#   SSE4: everything for SSE4.2 + POPCNT (x86-64-v2: Intel Nehalem / AMD Bulldozer and newer).
set(KZ_ISA "MULTI" CACHE STRING "CPU baseline: MULTI, AVX2 or SSE4")
set_property(CACHE KZ_ISA PROPERTY STRINGS MULTI SSE4 AVX2)
if(KZ_ISA STREQUAL "SSE4")
    set(KZ_ARCH_FLAG "/arch:SSE4.2")
    set(KZ_CLANG_ARCH "/clang:-march=x86-64-v2")
elseif(KZ_ISA STREQUAL "AVX2")
    set(KZ_ARCH_FLAG "/arch:AVX2")
    set(KZ_CLANG_ARCH "/clang:-march=x86-64-v3")
elseif(KZ_ISA STREQUAL "MULTI")
    set(KZ_ARCH_FLAG "")
    set(KZ_CLANG_ARCH "/clang:-march=x86-64 /clang:-msse4.1")
else()
    message(FATAL_ERROR "KZ_ISA must be MULTI, AVX2 or SSE4 (got '${KZ_ISA}')")
endif()
set(KZGS_ISA "${KZ_ISA}" CACHE STRING "" FORCE)
# clang-cl (opt-in: configure with -DCMAKE_C_COMPILER=<llvm>/clang-cl.exe -DCMAKE_CXX_COMPILER=<llvm>/clang-cl.exe, see
# docs/perf_research.md "clang-cl build"): same option set, spelled for clang. Float semantics stay strict:
# -ffp-contract=off (no FMA fusing of a*b+c: PS2 float emulation is bit-exact), never /fp:fast. The kz* libraries
# (PCSX2 code) add their own /fp:contract ${KZ_ARCH_FLAG} after these, as they do with MSVC.
if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(KZ_CLANG_CL ON)
else()
    set(KZ_CLANG_CL OFF)
endif()
# Optional clang-cl extras (all OFF by default):
#   -DKZ_CLANG_LTO=thin|full   link-time optimisation. Needs llvm-lib as the archiver (MSVC lib.exe does not index
#                              bitcode): also pass -DCMAKE_AR=<llvm>/bin/llvm-lib.exe -DCMAKE_LINKER=<llvm>/bin/lld-link.exe
#   -DKZ_CLANG_PGO_GEN=ON      instrumented build (writes <cwd>/kz_%p.profraw at exit; run a gameplay session,
#                              then llvm-profdata merge -output=kz.profdata *.profraw)
#   -DKZ_CLANG_PGO_USE=<file>  optimise with that .profdata
set(KZ_CLANG_LTO "" CACHE STRING "clang-cl LTO mode: empty, thin or full")
option(KZ_CLANG_PGO_GEN "clang-cl: build an instrumented (profile-generating) binary" OFF)
set(KZ_CLANG_PGO_USE "" CACHE FILEPATH "clang-cl: .profdata to optimise with")
if(KZ_CLANG_CL AND KZ_FAST_FLAGS)
    set(_kz_clang_opt "/Zi /O2 /Oi /Gy /Gw /GS- ${KZ_CLANG_ARCH} /clang:-ffp-contract=off /DNDEBUG")
    set(_kz_clang_link "/DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF")
    if(KZ_CLANG_LTO)
        string(APPEND _kz_clang_opt " /clang:-flto=${KZ_CLANG_LTO}")
        string(APPEND _kz_clang_link " /lldltocache:${CMAKE_BINARY_DIR}/lto-cache")
    endif()
    if(KZ_CLANG_PGO_GEN)
        string(APPEND _kz_clang_opt " /clang:-fprofile-instr-generate")
        get_filename_component(_kz_llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
        file(GLOB _kz_prof_rt "${_kz_llvm_bin}/../lib/clang/*/lib/windows/clang_rt.profile-x86_64.lib")
        list(GET _kz_prof_rt 0 _kz_prof_rt)
        string(APPEND _kz_clang_link " \"${_kz_prof_rt}\"")
    elseif(KZ_CLANG_PGO_USE)
        string(APPEND _kz_clang_opt " /clang:-fprofile-instr-use=${KZ_CLANG_PGO_USE} /clang:-Wno-profile-instr-unprofiled /clang:-Wno-profile-instr-out-of-date /clang:-Wno-backend-plugin")
    endif()
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${_kz_clang_opt}")
    set(CMAKE_C_FLAGS_RELWITHDEBINFO "${_kz_clang_opt}")
    set(CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO "${_kz_clang_link}")
elseif(MSVC AND KZ_FAST_FLAGS)
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "/Zi /O2 /Ob3 /Oi /Gy /Gw /GS- /Qspectre- ${KZ_ARCH_FLAG} /DNDEBUG")
    set(CMAKE_C_FLAGS_RELWITHDEBINFO "/Zi /O2 /Ob3 /Oi /Gy /Gw /GS- /Qspectre- ${KZ_ARCH_FLAG} /DNDEBUG")
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

# Issue #15: one exe for the US disc (SCUS-97402, the set above) and the PAL disc (SCES-52893, En/Ru/El). The PAL code
# (from SCES_528.93) is generated with symbol_prefix = "kzpal_" and table_register_function = "kzPalRegisterFunctionTable"
# (src/kz_pal_table.h), so it links next to the US code; it is its own object library below (own include directory and
# PCH, so its ps2_recompiled_functions.h is the PAL one). The game picks the version from the disc at run time
# (src/kz_region.h). Empty or missing directory: a US-only build (the PAL disc is then reported as unsupported).
set(KZ_GENERATED_PAL_DIR "${CMAKE_SOURCE_DIR}/generated_pal" CACHE PATH "PS2Recomp output for SCES_528.93 (PAL); empty = US only")
set(KZ_HAVE_PAL OFF)
if(KZ_GENERATED_PAL_DIR AND EXISTS "${KZ_GENERATED_PAL_DIR}/register_functions.cpp")
    # A PAL set generated without the table function (e.g. the old PAL-only test build) defines the US table symbols
    # and would not link: it is left out.
    file(STRINGS "${KZ_GENERATED_PAL_DIR}/register_functions.cpp" _kz_pal_register REGEX "kzPalRegisterFunctionTable" LIMIT_COUNT 1)
    if(_kz_pal_register)
        # The PAL set fills the US live table at boot, so the US table must reach past the end of the PAL code
        # (config/killzone.toml table_end_min). Checked here so a PAL regen that grew can't ship a PAL disc that fails at boot.
        file(STRINGS "${KZ_GENERATED_PAL_DIR}/register_functions.cpp" _kz_pal_end REGEX "kzPalRegisterFunctionTableEnd = 0x" LIMIT_COUNT 1)
        file(STRINGS "${KZ_GENERATED_DIR}/register_functions.cpp" _kz_us_end REGEX "g_ps2RecompiledFunctionTableEnd = 0x" LIMIT_COUNT 1)
        string(REGEX MATCH "0x[0-9a-fA-F]+" _kz_pal_end "${_kz_pal_end}")
        string(REGEX MATCH "0x[0-9a-fA-F]+" _kz_us_end "${_kz_us_end}")
        if(_kz_pal_end AND _kz_us_end)
            math(EXPR _kz_pal_end_n "${_kz_pal_end}")
            math(EXPR _kz_us_end_n "${_kz_us_end}")
            if(_kz_pal_end_n GREATER _kz_us_end_n)
                message(FATAL_ERROR "Killzone: the PAL code ends at ${_kz_pal_end} but the US function table ends at ${_kz_us_end}: "
                                    "raise table_end_min in config/killzone.toml to at least ${_kz_pal_end} and regenerate the US set")
            endif()
        endif()
        file(GLOB KZ_GENERATED_PAL_SRC CONFIGURE_DEPENDS "${KZ_GENERATED_PAL_DIR}/*.cpp")
        list(LENGTH KZ_GENERATED_PAL_SRC KZ_GENERATED_PAL_COUNT)
        set(KZ_HAVE_PAL ON)
        message(STATUS "Killzone: ${KZ_GENERATED_PAL_COUNT} PAL generated sources (SCES-52893 supported)")
    else()
        message(WARNING "Killzone: ${KZ_GENERATED_PAL_DIR} was not generated with table_register_function = "
                        "kzPalRegisterFunctionTable (and symbol_prefix = kzpal_); building US only, the PAL disc is not supported")
    endif()
else()
    message(STATUS "Killzone: no PAL generated code (KZ_GENERATED_PAL_DIR = '${KZ_GENERATED_PAL_DIR}'); building US only, "
                   "the PAL disc (SCES-52893) is not supported")
endif()

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
target_link_libraries(kz_port PRIVATE ps2_runtime SDL3::SDL3 imgui kz_pcsx2_libs) # kzgs kzvu kzipu kzspu2, see below
target_compile_definitions(kz_port PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(KZ_HAVE_PAL)
    target_compile_definitions(kz_port PRIVATE KZ_HAVE_PAL=1) # src/kz_region.cpp fills the function table from the PAL set
endif()
# The CPU check runs before the C runtime on any x64 CPU, so it is built without AVX2 (src/kz_cpucheck.cpp, issue #14).
# KZ_CPUCHECK_ISA tells it which instructions this build needs: 0 = MULTI, 1 = SSE4, 2 = AVX2.
if(KZ_ISA STREQUAL "MULTI")
    set(_kz_cpucheck_isa 0)
elseif(KZ_ISA STREQUAL "SSE4")
    set(_kz_cpucheck_isa 1)
else()
    set(_kz_cpucheck_isa 2)
endif()
set_source_files_properties("${CMAKE_SOURCE_DIR}/src/kz_cpucheck.cpp" PROPERTIES COMPILE_DEFINITIONS "KZ_CPUCHECK_ISA=${_kz_cpucheck_isa}")
if(KZ_CLANG_CL)
    set_source_files_properties("${CMAKE_SOURCE_DIR}/src/kz_cpucheck.cpp" PROPERTIES COMPILE_OPTIONS "/arch:SSE2;/clang:-march=x86-64")
elseif(MSVC)
    set_source_files_properties("${CMAKE_SOURCE_DIR}/src/kz_cpucheck.cpp" PROPERTIES COMPILE_OPTIONS "/arch:SSE2")
endif()
if(KZ_CLANG_CL AND KZ_CLANG_PGO_GEN)
    target_compile_definitions(kz_port PRIVATE KZ_PGO_GEN=1)
endif()

# The PCSX2 libraries as the exe links them: kz_pcsx2_libs (kzgs, kzvu, kzipu, kzspu2) for the game, kz_gs_libs (kzgs) for
# kz_gs_replay.
# KZ_ISA=MULTI: the AVX/AVX2 copies of the per-ISA files emit their own out-of-line copies of inline functions from the
# shared headers (STL, fmt, GSVector, ...) under the same names as the baseline code, and the linker keeps one copy of
# each (COMDAT "select any"). link.exe keeps the copy from the object it loads first; /WHOLEARCHIVE members are loaded
# like objects named on the command line, in command-line order, before any library member pulled in on demand. So the
# shared kzgs, kzipu and kzspu2 libraries are whole-archived first, then every SSE4.1 copy, then every AVX copy, then
# every AVX2 copy: a function the baseline code of those libraries uses resolves to a baseline build of it. (kz_port is
# whole-archived by the exe ahead of these.) kzvu cannot be whole-archived (it carries the same pcsx2/common objects as
# kzgs, LNK2005), so it, ps2_runtime and imgui are still pulled on demand after the per-ISA copies; a function only they
# and an AVX copy emit out of line could still resolve to the AVX copy, which a scan of the linked exe catches.
add_library(kz_pcsx2_libs INTERFACE)
add_library(kz_gs_libs INTERFACE)
if(KZ_ISA STREQUAL "MULTI")
    set(_kz_isa_libs "")
    set(_kz_gs_isa_libs "")
    foreach(_isa sse4 avx avx2)
        get_property(_libs GLOBAL PROPERTY KZ_MULTI_ISA_LIBS_${_isa})
        list(APPEND _kz_isa_libs ${_libs})
        list(FILTER _libs INCLUDE REGEX "^kzgs_")
        list(APPEND _kz_gs_isa_libs ${_libs})
    endforeach()
    list(JOIN _kz_isa_libs "," _kz_isa_libs)
    list(JOIN _kz_gs_isa_libs "," _kz_gs_isa_libs)
    target_link_libraries(kz_pcsx2_libs INTERFACE "$<LINK_LIBRARY:WHOLE_ARCHIVE,kzgs,kzipu,kzspu2,${_kz_isa_libs}>" kzvu)
    target_link_libraries(kz_gs_libs INTERFACE "$<LINK_LIBRARY:WHOLE_ARCHIVE,kzgs,${_kz_gs_isa_libs}>")
else()
    target_link_libraries(kz_pcsx2_libs INTERFACE kzgs kzvu kzipu kzspu2)
    target_link_libraries(kz_gs_libs INTERFACE kzgs)
endif()

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
    # Start in the CPU check (src/kz_cpucheck.cpp), which then calls the normal C runtime start-up.
    target_link_options(killzone PRIVATE /ENTRY:kzCpuCheckEntry)
endif()

# The PAL generated code (see KZ_GENERATED_PAL_DIR above): same flags, unity batches and PCH layout as the US code, but its
# own include directory, so `#include <ps2_recompiled_functions.h>` in its sources and its PCH is generated_pal's header.
if(KZ_HAVE_PAL)
    add_library(kz_generated_pal OBJECT ${KZ_GENERATED_PAL_SRC})
    target_include_directories(kz_generated_pal PRIVATE "${KZ_GENERATED_PAL_DIR}")
    target_link_libraries(kz_generated_pal PRIVATE ps2_runtime)
    set_target_properties(kz_generated_pal PROPERTIES
        UNITY_BUILD ON
        UNITY_BUILD_BATCH_SIZE 128
    )
    target_precompile_headers(kz_generated_pal PRIVATE
        ext/PS2Recomp/ps2xRuntime/include/ps2_runtime_macros.h
        ext/PS2Recomp/ps2xRuntime/include/ps2_runtime.h
        "${KZ_GENERATED_PAL_DIR}/ps2_recompiled_functions.h"
        "${KZ_GENERATED_PAL_DIR}/ps2_recompiled_stubs.h"
        ext/PS2Recomp/ps2xRuntime/include/ps2_syscalls.h
        ext/PS2Recomp/ps2xRuntime/include/ps2_stubs.h
    )
    if(MSVC)
        target_compile_options(kz_generated_pal PRIVATE /bigobj /FS)
    endif()
    # Only its objects: linking the object library would add its ps2_runtime dependency again at the end of the link line,
    # which moves raylib behind OneCore.lib/winmm.lib (LNK2005 CloseWindow / PlaySound).
    target_sources(killzone PRIVATE $<TARGET_OBJECTS:kz_generated_pal>)
endif()

# Copy FFmpeg runtime DLLs next to killzone.exe (helper from ps2xRuntime).
ps2x_stage_ffmpeg_runtime_dlls(killzone)
kzgs_stage_resources(killzone)
# Keyboard and mouse button prompt pictures (Kenney Input Prompts, CC0; the licence file stays next to them): src/kz_prompts.cpp
add_custom_command(TARGET killzone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/resources/prompts" "$<TARGET_FILE_DIR:killzone>/resources/prompts"
    COMMENT "Staging the button prompt pictures"
    VERBATIM)
add_custom_command(TARGET killzone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:killzone>)

# Offline GS trace replay (tools/gs_replay): renders a KZ_GS_TRACE recording through kzgs.
add_executable(kz_gs_replay tools/gs_replay/gs_replay.cpp)
target_link_libraries(kz_gs_replay PRIVATE kz_gs_libs SDL3::SDL3)
target_compile_features(kz_gs_replay PRIVATE cxx_std_20)
add_executable(kz_gs_replay_cpu tools/gs_replay/gs_replay_cpu.cpp)
target_link_libraries(kz_gs_replay_cpu PRIVATE ps2_runtime)
target_compile_features(kz_gs_replay_cpu PRIVATE cxx_std_20)
