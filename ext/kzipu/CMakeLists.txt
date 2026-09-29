# kzipu - PCSX2's IPU (PS2 MPEG-2 decoder unit) as a standalone static library (Windows x64, MSVC).
#
# Standalone:   cmake -S ext/kzipu -B ext/kzipu/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
# As a subdir:  add_subdirectory(ext/kzipu) ; target_link_libraries(game PRIVATE kzipu)
#
# GPL-3.0+: this links PCSX2 code, so anything that links kzipu is a GPL-3 derivative work.

cmake_minimum_required(VERSION 3.24)
project(kzipu C CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "kzipu only supports MSVC x64.")
endif()

set(KZIPU_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(KZIPU_PCSX2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pcsx2" CACHE PATH "PCSX2 source tree")
set(KZIPU_FFMPEG_DIR
	"${CMAKE_CURRENT_SOURCE_DIR}/../../build/RelWithDebInfo/ThirdParty/ffmpeg-prefix/src/ffmpeg_external"
	CACHE PATH "FFmpeg install (include/, bin/*.lib, bin/*.dll); used by kzipu_test only, as the reference decoder")
set(KZIPU_KZVU_LIB "${CMAKE_CURRENT_SOURCE_DIR}/../kzvu/build/kzvu.lib" CACHE FILEPATH
	"Prebuilt kzvu.lib; with KZIPU_KZGS_LIB, kzipu_linkcheck links kzipu + kzvu + kzgs to prove there are no duplicate symbols")
set(KZIPU_KZGS_LIB "${CMAKE_CURRENT_SOURCE_DIR}/../kzgs/build/kzgs.lib" CACHE FILEPATH "Prebuilt kzgs.lib (see KZIPU_KZVU_LIB)")
option(KZIPU_BUILD_TEST "Build the kzipu_test executable (and kzipu_linkcheck_* when kzvu/kzgs are built)" ${PROJECT_IS_TOP_LEVEL})

get_filename_component(KZIPU_PCSX2_DIR "${KZIPU_PCSX2_DIR}" ABSOLUTE)
set(P2 "${KZIPU_PCSX2_DIR}")
set(P2SRC "${P2}/pcsx2")
set(P3 "${P2}/3rdparty")

if(NOT EXISTS "${P2SRC}/IPU/IPU.cpp")
	message(FATAL_ERROR "PCSX2 source not found at ${P2}")
endif()

# ---------------------------------------------------------------------------------------------------------------------
# PCSX2 IPU sources (unmodified) + shims + API. Every translation unit gets the forced include kzipu_prefix.h, which
# renames every symbol kzipu defines (kzipu_rename.h), pre-includes the PCSX2 headers and redirects logging.
# No pcsx2/common source is compiled: the handful of common functions the IPU references are implemented, under
# kzipu-private names, in shim/KzipuShims.cpp. kzipu therefore shares no symbol with kzgs or kzvu.
# The per-ISA IPU code (IPU_MultiISA.cpp, IPUdither.cpp, yuv2rgb.cpp) is compiled once, for AVX2 like kzgs/kzvu.
# ---------------------------------------------------------------------------------------------------------------------
set(KZIPU_PCSX2_SRCS
	${P2SRC}/IPU/IPU.cpp
	${P2SRC}/IPU/IPU_Fifo.cpp
	${P2SRC}/IPU/IPUdma.cpp
	${P2SRC}/IPU/IPU_MultiISA.cpp
	${P2SRC}/IPU/IPUdither.cpp
	${P2SRC}/IPU/yuv2rgb.cpp
)
add_library(kzipu STATIC
	${KZIPU_PCSX2_SRCS}
	"${KZIPU_ROOT}/shim/KzipuShims.cpp"
	"${KZIPU_ROOT}/src/kzipu.cpp"
	"${KZIPU_ROOT}/include/kzipu.h"
)
target_compile_features(kzipu PRIVATE cxx_std_20)
target_compile_definitions(kzipu PRIVATE
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
target_compile_options(kzipu PRIVATE
	/Zc:externConstexpr /Zc:__cplusplus /permissive- /Zc:preprocessor
	/utf-8 /Zo /GS- /fp:contract /W1 /wd4005 /wd4996 /arch:AVX2
	"/FI${KZIPU_ROOT}/shim/include/kzipu_prefix.h"
)
target_include_directories(kzipu PRIVATE
	"${KZIPU_ROOT}/shim/include"
	"${P2}"
	"${P2SRC}"
	"${P3}/include"
	"${P3}/fmt/include"
	"${P3}/fast_float/include"
)
target_include_directories(kzipu PUBLIC "${KZIPU_ROOT}/include")

if(KZIPU_BUILD_TEST)
	set(FF "${KZIPU_FFMPEG_DIR}")
	if(NOT EXISTS "${FF}/include/libavcodec/avcodec.h")
		message(FATAL_ERROR "kzipu_test needs FFmpeg at KZIPU_FFMPEG_DIR (${FF}); build the game once or set the path")
	endif()
	add_executable(kzipu_test "${KZIPU_ROOT}/test/kzipu_test.cpp")
	target_link_libraries(kzipu_test PRIVATE kzipu "${FF}/bin/avcodec.lib" "${FF}/bin/avutil.lib")
	target_include_directories(kzipu_test PRIVATE "${FF}/include")
	target_compile_features(kzipu_test PRIVATE cxx_std_20)
	target_compile_definitions(kzipu_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
	target_compile_options(kzipu_test PRIVATE /utf-8 /W3 /wd4244 /wd4267)
	# The FFmpeg DLLs (and the DLLs they import) are copied next to the test.
	file(GLOB KZIPU_FF_DLLS "${FF}/bin/*.dll")
	add_custom_command(TARGET kzipu_test POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different ${KZIPU_FF_DLLS} "$<TARGET_FILE_DIR:kzipu_test>"
		COMMENT "Copying FFmpeg DLLs next to kzipu_test")

	# Link check: kzipu + kzvu + kzgs in one executable, in two library orders. Fails to link on any duplicate symbol.
	if(EXISTS "${KZIPU_KZVU_LIB}" AND EXISTS "${KZIPU_KZGS_LIB}")
		set(_kzgs_deps "${KZIPU_ROOT}/../kzgs/deps/deps/lib")
		set(_kzgs_libs "${KZIPU_KZGS_LIB}"
			"${_kzgs_deps}/zstd.lib" "${_kzgs_deps}/libpng16.lib" "${_kzgs_deps}/libwebp.lib" "${_kzgs_deps}/jpeg.lib"
			"${_kzgs_deps}/dxcompiler.lib" "${_kzgs_deps}/DirectX-Guids.lib"
			d3d11.lib d3d12.lib dxgi.lib d3dcompiler.lib dxguid.lib winmm.lib pathcch.lib shlwapi.lib dwmapi.lib
			PowrProf.lib OneCore.lib comsuppw.lib delayimp.lib)
		foreach(_order ab ba)
			add_executable(kzipu_linkcheck_${_order} "${KZIPU_ROOT}/test/kzipu_linkcheck.cpp")
			target_include_directories(kzipu_linkcheck_${_order} PRIVATE "${KZIPU_ROOT}/../kzvu/include" "${KZIPU_ROOT}/../kzgs/include")
			target_compile_features(kzipu_linkcheck_${_order} PRIVATE cxx_std_20)
			if(_order STREQUAL "ab")
				target_link_libraries(kzipu_linkcheck_${_order} PRIVATE kzipu "${KZIPU_KZVU_LIB}" ${_kzgs_libs})
			else()
				target_link_libraries(kzipu_linkcheck_${_order} PRIVATE ${_kzgs_libs} "${KZIPU_KZVU_LIB}" kzipu)
			endif()
			target_link_options(kzipu_linkcheck_${_order} PRIVATE /DELAYLOAD:zstd.dll /DELAYLOAD:libpng16.dll
				/DELAYLOAD:libwebp.dll /DELAYLOAD:jpeg62.dll /DELAYLOAD:dxcompiler.dll)
		endforeach()
	endif()
endif()
