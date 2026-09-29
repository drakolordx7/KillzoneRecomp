// kzgs: glue between the public API (src/kzgs.cpp) and the host shims (shim/*.cpp).
#pragma once

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
} // namespace kzgs
