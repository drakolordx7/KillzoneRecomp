// kzgs: glue between the public API (src/kzgs.cpp) and the host shims (shim/*.cpp).
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace kzgs
{
	// Window handed to PCSX2 through Host::AcquireRenderWindow(). nullptr = surfaceless (no swap chain).
	void SetRenderWindow(void* hwnd);

	// Last error reported by the GS through Host::ReportErrorAsync / ReportFormattedErrorAsync (thread-safe).
	void ClearLastError();
	std::string GetLastError();

	// Debug/tooling: runs fn on the GS thread after everything queued so far, and waits for it.
	void RunOnGSThread(std::function<void()> fn);

	// SMAA 1x on the D3D11 device (src/kz_smaa11.cpp). SmaaInstall() swaps the device's FXAA hook; call it after GSopen and
	// after every GSUpdateConfig (a renderer switch creates a new device).
	// Measurement: while a sink is set, every Direct3D 11 Present first hands the finished back buffer (RGBA8) to it (src/kz_present11.cpp).
	using PresentSink = void (*)(const uint8_t* rgba, int width, int height);
	bool PresentCapture(PresentSink sink);

	// GSState::m_scanmask_used (src/kz_inspect.cpp): frames left in which PCSX2 treats the output as scan-masked (anti-blur is off while > 0).
	int ScanmaskUsed();
	std::string DescribeTargets(); // one line per render target of PCSX2's texture cache (src/kz_inspect.cpp)

	void SmaaSetEnabled(bool on);
	void SmaaInstall();
} // namespace kzgs
