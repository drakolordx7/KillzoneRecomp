# kzvu - PCSX2's VU1 (microVU x86-64 recompiler + VU interpreter) as a standalone static library (Windows x64, MSVC).
#
# Standalone:   cmake -S ext/kzvu -B ext/kzvu/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
# As a subdir:  add_subdirectory(ext/kzvu) ; target_link_libraries(game PRIVATE kzvu)
#
# GPL-3.0+: this links PCSX2 code, so anything that links kzvu is a GPL-3 derivative work.

cmake_minimum_required(VERSION 3.24)
project(kzvu C CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "kzvu only supports MSVC x64.")
endif()

set(KZVU_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(KZVU_PCSX2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pcsx2" CACHE PATH "PCSX2 source tree")
set(KZVU_PS2RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../PS2Recomp" CACHE PATH
	"PS2Recomp tree; its VU1Interpreter is compiled into kzvu_test only, for differential testing")
set(KZVU_KZGS_LIB "${CMAKE_CURRENT_SOURCE_DIR}/../kzgs/build/kzgs.lib" CACHE FILEPATH
	"Prebuilt kzgs.lib; when it exists kzvu_linkcheck links kzvu + kzgs together to prove there are no duplicate symbols")
option(KZVU_BUILD_TEST "Build the kzvu_test executable" ${PROJECT_IS_TOP_LEVEL})

get_filename_component(KZVU_PCSX2_DIR "${KZVU_PCSX2_DIR}" ABSOLUTE)
set(P2 "${KZVU_PCSX2_DIR}")
set(P2SRC "${P2}/pcsx2")
set(P3 "${P2}/3rdparty")

if(NOT EXISTS "${P2SRC}/x86/microVU.cpp")
	message(FATAL_ERROR "PCSX2 source not found at ${P2}")
endif()

# ---------------------------------------------------------------------------------------------------------------------
# Compile flags (private). They match ext/kzgs so the pcsx2/common objects both libraries carry are identical.
# ---------------------------------------------------------------------------------------------------------------------
add_library(kzvu_flags INTERFACE)
target_compile_features(kzvu_flags INTERFACE cxx_std_20)
target_compile_definitions(kzvu_flags INTERFACE
	_M_X86=1
	__SSE4_1__=1
	WINVER=0x0A00
	_WIN32_WINNT=0x0A00
	WIN32_LEAN_AND_MEAN
	NOMINMAX
	_CRT_NONSTDC_NO_WARNINGS
	_CRT_SECURE_NO_WARNINGS
	CRT_SECURE_NO_DEPRECATE
	_SCL_SECURE_NO_WARNINGS
	_UNICODE
	UNICODE
)
target_compile_options(kzvu_flags INTERFACE
	"$<$<COMPILE_LANGUAGE:CXX>:/Zc:externConstexpr;/Zc:__cplusplus;/permissive-;/Zc:preprocessor>"
	"$<$<COMPILE_LANGUAGE:C,CXX>:/utf-8;/Zo;/GS-;/fp:contract;/W1;/wd4005;/wd4996;/arch:AVX2>"
)
target_include_directories(kzvu_flags INTERFACE
	"${KZVU_ROOT}/shim/include"
	"${P2}"
	"${P2SRC}"
	"${P3}/include"
	"${P3}/fmt/include"
	"${P3}/fast_float/include"
)

# Every kzvu translation unit (PCSX2 VU code, the common/ slice and the shims) is compiled with a forced include that
# renames the emulator globals kzvu defines (eeHw, cpuRegs, vuRegs, g_cpu, SysMemory, ...) and the host hooks it
# implements (Host::, CrashHandler::) to kzvu-private names, so kzvu can be linked into the same executable as kzgs.
set(KZVU_RENAME_H "${KZVU_ROOT}/shim/include/kzvu_rename.h")
target_compile_options(kzvu_flags INTERFACE "$<$<COMPILE_LANGUAGE:CXX>:/FI${KZVU_RENAME_H}>")

# ---------------------------------------------------------------------------------------------------------------------
# pcsx2/common slice + x86 emitter
# ---------------------------------------------------------------------------------------------------------------------
add_library(kzvu_common OBJECT
	${P2}/common/Assertions.cpp
	${P2}/common/Console.cpp
	${P2}/common/Error.cpp
	${P2}/common/FileSystem.cpp
	${P2}/common/Perf.cpp
	${P2}/common/StringUtil.cpp
	${P2}/common/SmallString.cpp
	${P2}/common/Timer.cpp
	${P2}/common/emitter/bmi.cpp
	${P2}/common/emitter/fpu.cpp
	${P2}/common/emitter/groups.cpp
	${P2}/common/emitter/jmp.cpp
	${P2}/common/emitter/legacy.cpp
	${P2}/common/emitter/legacy_sse.cpp
	${P2}/common/emitter/movs.cpp
	${P2}/common/emitter/simd.cpp
	${P2}/common/emitter/x86emitter.cpp
	${P3}/fmt/src/format.cc
)
target_link_libraries(kzvu_common PRIVATE kzvu_flags)
target_compile_options(kzvu_common PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:/W0>)

# ---------------------------------------------------------------------------------------------------------------------
# PCSX2 VU code: microVU (one translation unit, includes all microVU_*.inl) + the VU interpreter.
# kzvu_prefix.h pre-includes every PCSX2 header these files use and then redirects gifUnit/EmuConfig/vu1Thread to the
# shims in shim/include/kzvu_shim.h. No file in ext/pcsx2 is modified.
# ---------------------------------------------------------------------------------------------------------------------
set(KZVU_VU_SRCS
	${P2SRC}/x86/microVU.cpp
	${P2SRC}/VUops.cpp
	${P2SRC}/VUflags.cpp
	${P2SRC}/VU1microInterp.cpp
	${P2SRC}/VU0microInterp.cpp
)
file(GLOB KZVU_SHIM_SRCS CONFIGURE_DEPENDS "${KZVU_ROOT}/shim/*.cpp")
add_library(kzvu_vu OBJECT ${KZVU_VU_SRCS} ${KZVU_SHIM_SRCS})
target_link_libraries(kzvu_vu PRIVATE kzvu_flags)
set_source_files_properties(${KZVU_VU_SRCS} PROPERTIES COMPILE_OPTIONS "/FI${KZVU_ROOT}/shim/include/kzvu_prefix.h")

# ---------------------------------------------------------------------------------------------------------------------
# kzvu: the public static library
# ---------------------------------------------------------------------------------------------------------------------
add_library(kzvu STATIC
	"${KZVU_ROOT}/src/kzvu.cpp"
	"${KZVU_ROOT}/include/kzvu.h"
	$<TARGET_OBJECTS:kzvu_common>
	$<TARGET_OBJECTS:kzvu_vu>
)
target_link_libraries(kzvu PRIVATE kzvu_flags)
set_source_files_properties("${KZVU_ROOT}/src/kzvu.cpp" PROPERTIES COMPILE_OPTIONS "/FI${KZVU_ROOT}/shim/include/kzvu_prefix.h")
target_include_directories(kzvu PUBLIC "${KZVU_ROOT}/include")
target_link_libraries(kzvu INTERFACE pathcch.lib)

if(KZVU_BUILD_TEST)
	# PS2Recomp's cycle-accurate VU1Interpreter, compiled into the test only (differential reference + benchmark).
	set(PS2X "${KZVU_PS2RECOMP_DIR}/ps2xRuntime")
	set(KZVU_TEST_SRCS "${KZVU_ROOT}/test/kzvu_test.cpp")
	set(KZVU_TEST_DEFS "")
	if(EXISTS "${PS2X}/src/lib/vu/ps2_vu1_core.cpp")
		list(APPEND KZVU_TEST_SRCS
			"${PS2X}/src/lib/vu/ps2_vu1_core.cpp"
			"${PS2X}/src/lib/vu/ps2_vu1_upper.cpp"
			"${PS2X}/src/lib/vu/ps2_vu1_lower.cpp"
			"${KZVU_ROOT}/test/ps2recomp_stubs.cpp")
		list(APPEND KZVU_TEST_DEFS KZVU_TEST_HAVE_PS2RECOMP=1)
	else()
		message(STATUS "kzvu: PS2Recomp VU1 sources not found; kzvu_test runs without the PS2Recomp comparison")
	endif()
	add_executable(kzvu_test ${KZVU_TEST_SRCS})
	target_link_libraries(kzvu_test PRIVATE kzvu)
	target_compile_features(kzvu_test PRIVATE cxx_std_20)
	target_compile_definitions(kzvu_test PRIVATE ${KZVU_TEST_DEFS} NOMINMAX _CRT_SECURE_NO_WARNINGS)
	target_compile_options(kzvu_test PRIVATE /utf-8 /W3 /wd4244 /wd4267 /wd4305)
	target_include_directories(kzvu_test PRIVATE "${PS2X}/include" "${PS2X}/include/runtime" "${PS2X}/src/lib/vu")

	# VU0 micro mode: synthetic programs + replay of captured game calls (test/vu0_captures, KZ_VU0_DUMP in the game).
	list(TRANSFORM KZVU_TEST_SRCS REPLACE "kzvu_test\.cpp$" "kzvu0_test.cpp" OUTPUT_VARIABLE KZVU0_TEST_SRCS)
	add_executable(kzvu0_test ${KZVU0_TEST_SRCS})
	target_link_libraries(kzvu0_test PRIVATE kzvu)
	target_compile_features(kzvu0_test PRIVATE cxx_std_20)
	target_compile_definitions(kzvu0_test PRIVATE ${KZVU_TEST_DEFS} NOMINMAX _CRT_SECURE_NO_WARNINGS)
	target_compile_options(kzvu0_test PRIVATE /utf-8 /W3 /wd4244 /wd4267 /wd4305)
	target_include_directories(kzvu0_test PRIVATE "${PS2X}/include" "${PS2X}/include/runtime" "${PS2X}/src/lib/vu")

	# Link check: kzvu + kzgs in one executable, in both library orders. Fails to link on any duplicate symbol.
	if(EXISTS "${KZVU_KZGS_LIB}")
		foreach(_order ab ba)
			add_executable(kzvu_linkcheck_${_order} "${KZVU_ROOT}/test/kzvu_linkcheck.cpp")
			target_include_directories(kzvu_linkcheck_${_order} PRIVATE "${KZVU_ROOT}/../kzgs/include")
			target_compile_features(kzvu_linkcheck_${_order} PRIVATE cxx_std_20)
			set(_kzgs_deps "${KZVU_ROOT}/../kzgs/deps/deps/lib")
			set(_kzgs_libs "${KZVU_KZGS_LIB}"
				"${_kzgs_deps}/zstd.lib" "${_kzgs_deps}/libpng16.lib" "${_kzgs_deps}/libwebp.lib" "${_kzgs_deps}/jpeg.lib"
				"${_kzgs_deps}/dxcompiler.lib" "${_kzgs_deps}/DirectX-Guids.lib"
				d3d11.lib d3d12.lib dxgi.lib d3dcompiler.lib dxguid.lib winmm.lib pathcch.lib shlwapi.lib dwmapi.lib
				PowrProf.lib OneCore.lib comsuppw.lib delayimp.lib)
			if(_order STREQUAL "ab")
				target_link_libraries(kzvu_linkcheck_${_order} PRIVATE kzvu ${_kzgs_libs})
			else()
				target_link_libraries(kzvu_linkcheck_${_order} PRIVATE ${_kzgs_libs} kzvu)
			endif()
			target_link_options(kzvu_linkcheck_${_order} PRIVATE /DELAYLOAD:zstd.dll /DELAYLOAD:libpng16.dll
				/DELAYLOAD:libwebp.dll /DELAYLOAD:jpeg62.dll /DELAYLOAD:dxcompiler.dll)
		endforeach()
	endif()
endif()
