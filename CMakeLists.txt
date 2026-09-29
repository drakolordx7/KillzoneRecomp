cmake_minimum_required(VERSION 3.21)
project(KillzoneRecomp CXX C)

# PS2Recomp runtime + IOP only (recompiler/analyzer are built separately by tools/scripts/build_ps2recomp.bat)
set(PS2X_BUILD_RECOMP OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_ANALYZER OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_STUDIO OFF CACHE BOOL "" FORCE)
set(PS2X_ENABLE_AGRESSIVE_LOGS OFF CACHE BOOL "" FORCE)  # per-call enter/exit trace: ~500MB per 30s
add_subdirectory(ext/PS2Recomp)

# Generated code lives outside the repo (derived from the game ELF). Point KZ_GENERATED_DIR at it.
set(KZ_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated" CACHE PATH "PS2Recomp output for SCUS_974.02")
file(GLOB KZ_GENERATED_SRC CONFIGURE_DEPENDS "${KZ_GENERATED_DIR}/*.cpp")
list(LENGTH KZ_GENERATED_SRC KZ_GENERATED_COUNT)
message(STATUS "Killzone: ${KZ_GENERATED_COUNT} generated sources")

file(GLOB KZ_SRC CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")

add_executable(killzone
    ${KZ_GENERATED_SRC}
    ${KZ_SRC}
)
# Our own code must not be merged into the generated-code unity batches.
set_source_files_properties(${KZ_SRC} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON SKIP_PRECOMPILE_HEADERS ON)
target_include_directories(killzone PRIVATE "${KZ_GENERATED_DIR}" "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(killzone PRIVATE ps2_runtime)
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
