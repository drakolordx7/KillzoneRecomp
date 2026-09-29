// Links kzvu and kzgs into one executable (CMake builds it in both library orders). Building it is the test: a
// duplicate symbol between the two libraries fails the link. Running it initializes kzvu only (no GS device).
#include "kzvu.h"
#include "kzgs.h"
#include <cstdio>
int main(int argc, char** argv)
{
	std::string err;
	KzvuConfig cfg;
	const bool ok = kzvuInit(cfg, &err);
	if (argc > 99) // never true; keeps the kzgs API referenced so kzgs.lib is pulled into the link
	{
		KzgsConfig gcfg;
		kzgsOpen(nullptr, gcfg, nullptr, &err);
		kzgsClose();
	}
	std::printf("kzvu init %s, kzgs open=%d\n", ok ? "ok" : err.c_str(), kzgsIsOpen() ? 1 : 0);
	kzvuShutdown();
	return ok ? 0 : 1;
}
