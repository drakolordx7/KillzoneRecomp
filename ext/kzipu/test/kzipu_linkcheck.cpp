// Links kzipu, kzvu and kzgs into one executable (CMake builds it in two library orders). Building it is the test: a
// duplicate symbol between the libraries fails the link. Running it initializes kzipu and kzvu only (no GS device).
// SPDX-License-Identifier: GPL-3.0+
#include "kzipu.h"
#include "kzvu.h"
#include "kzgs.h"
#include <cstdio>
#include <string>
int main(int argc, char** argv)
{
	KzipuConfig icfg;
	const bool ipuOk = kzipuInit(icfg);
	kzipuWriteReg32(0x10002010, 0x40000000); // IPU reset
	const KzipuStatus st = kzipuGetStatus();
	std::string err;
	KzvuConfig vcfg;
	const bool vuOk = kzvuInit(vcfg, &err);
	if (argc > 99) // never true; keeps the kzgs API referenced so kzgs.lib is pulled into the link
	{
		KzgsConfig gcfg;
		kzgsOpen(nullptr, gcfg, nullptr, &err);
		kzgsClose();
	}
	std::printf("kzipu init %s (INTC x%u), kzvu init %s, kzgs open=%d\n", ipuOk ? "ok" : "failed", st.intcCount,
		vuOk ? "ok" : err.c_str(), kzgsIsOpen() ? 1 : 0);
	kzvuShutdown();
	kzipuShutdown();
	return ipuOk && vuOk && st.intcCount == 1 ? 0 : 1;
}
