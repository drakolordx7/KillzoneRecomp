# kzspu2 - PCSX2's SPU2 (PS2 sound processor) as a standalone static library (Windows x64, MSVC). No audio backend:
# the mixed 48 kHz output goes to a ring buffer the host drains (see include/kzspu2.h, README.md).
#
# Standalone:   cmake -S ext/kzspu2 -B ext/kzspu2/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
# As a subdir:  add_subdirectory(ext/kzspu2) ; target_link_libraries(game PRIVATE kzspu2)
#
# GPL-3.0+: this links PCSX2 code, so anything that links kzspu2 is a GPL-3 derivative work.

cmake_minimum_required(VERSION 3.24)
project(kzspu2 C CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "kzspu2 only supports MSVC x64.")
endif()

set(KZSPU2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(KZSPU2_PCSX2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pcsx2" CACHE PATH "PCSX2 source tree")
option(KZSPU2_BUILD_TEST "Build the kzspu2_test executable" ${PROJECT_IS_TOP_LEVEL})

get_filename_component(KZSPU2_PCSX2_DIR "${KZSPU2_PCSX2_DIR}" ABSOLUTE)
set(P2 "${KZSPU2_PCSX2_DIR}")
set(P2SRC "${P2}/pcsx2")
set(P3 "${P2}/3rdparty")

if(NOT EXISTS "${P2SRC}/SPU2/spu2sys.cpp")
	message(FATAL_ERROR "PCSX2 source not found at ${P2}")
endif()

# ---------------------------------------------------------------------------------------------------------------------
# PCSX2 SPU2 sources (unmodified) + shims + API. Not compiled from SPU2/: spu2.cpp (audio backend, VM glue; its
# register/DMA entry points are reimplemented in src/kzspu2.cpp), spu2freeze.cpp (savestates), Debug.cpp and
# Wavedump_wav.cpp (PCSX2_DEVBUILD only). Every translation unit gets the forced include kzspu2_prefix.h, which renames
# every symbol kzspu2 defines (kzspu2_rename.h). shim/include comes first on the include path and shadows the IOP,
# config, savestate and audio-stream headers with minimal versions, so no other PCSX2 code is compiled.
# The per-ISA code (Mixer.cpp, ReverbResample.cpp) is compiled once, for AVX2 like kzgs/kzvu/kzipu.
# ---------------------------------------------------------------------------------------------------------------------
set(KZSPU2_PCSX2_SRCS
	${P2SRC}/SPU2/spu2sys.cpp
	${P2SRC}/SPU2/Mixer.cpp
	${P2SRC}/SPU2/ReadInput.cpp
	${P2SRC}/SPU2/Reverb.cpp
	${P2SRC}/SPU2/ReverbResample.cpp
	${P2SRC}/SPU2/ADSR.cpp
	${P2SRC}/SPU2/RegTable.cpp
	${P2SRC}/SPU2/Dma.cpp
)
add_library(kzspu2 STATIC
	${KZSPU2_PCSX2_SRCS}
	"${KZSPU2_ROOT}/shim/Kzspu2Shims.cpp"
	"${KZSPU2_ROOT}/src/kzspu2.cpp"
	"${KZSPU2_ROOT}/include/kzspu2.h"
)
target_compile_features(kzspu2 PRIVATE cxx_std_20)
target_compile_definitions(kzspu2 PRIVATE
	_M_X86=1
	__SSE4_1__=1
	WINVER=0x0A00
	_WIN32_WINNT=0x0A00
	WIN32_LEAN_AND_MEAN
	NOMINMAX
	_CRT_NONSTDC_NO_WARNINGS
	_CRT_SECURE_NO_WARNINGS
	_SCL_SECURE_NO_WARNINGS
	_UNICODE
	UNICODE
)
target_compile_options(kzspu2 PRIVATE
	/Zc:externConstexpr /Zc:__cplusplus /permissive- /Zc:preprocessor
	/utf-8 /Zo /GS- /fp:contract /W1 /wd4005 /wd4996 /arch:AVX2
	"/FI${KZSPU2_ROOT}/shim/include/kzspu2_prefix.h"
)
target_include_directories(kzspu2 PRIVATE
	"${KZSPU2_ROOT}/shim/include"
	"${KZSPU2_ROOT}/include"
	"${P2}"
	"${P2SRC}"
	"${P3}/include"
	"${P3}/fmt/include"
	"${P3}/fast_float/include"
)
target_include_directories(kzspu2 PUBLIC "${KZSPU2_ROOT}/include")

if(KZSPU2_BUILD_TEST)
	add_executable(kzspu2_test "${KZSPU2_ROOT}/test/kzspu2_test.cpp")
	target_link_libraries(kzspu2_test PRIVATE kzspu2)
	target_compile_features(kzspu2_test PRIVATE cxx_std_20)
	target_compile_definitions(kzspu2_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
	target_compile_options(kzspu2_test PRIVATE /utf-8 /W3 /wd4244 /wd4267)
endif()
