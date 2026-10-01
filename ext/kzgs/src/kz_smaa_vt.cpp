// SPDX-License-Identifier: GPL-3.0+
// kzgs: finds the vtable slot of GSDevice::DoFXAA. GSDevice declares it protected, so this one translation unit sees the
// class with access macro-defined away; it only takes a pointer to the virtual member, which the compiler resolves to a
// local vcall thunk (no symbol of ext/pcsx2 is referenced).
#define private public
#define protected public
#include "GS/Renderers/Common/GSDevice.h"
#undef protected
#undef private

#include <cstdint>
#include <cstring>

namespace kzgs
{
	int FxaaVtableIndex()
	{
		auto pm = &GSDevice::DoFXAA;
		static_assert(sizeof(pm) >= sizeof(void*), "the first word of the member pointer is the thunk address");
		const uint8_t* p;
		std::memcpy(&p, &pm, sizeof(p));
		for (int hop = 0; hop < 4 && p[0] == 0xE9; hop++) // incremental-link jump stub
		{
			int32_t rel;
			std::memcpy(&rel, p + 1, 4);
			p += 5 + rel;
		}
		// MSVC x64 virtual-call thunk: mov rax,[rcx]; jmp [rax+disp]
		if (p[0] != 0x48 || p[1] != 0x8B || p[2] != 0x01 || p[3] != 0xFF)
			return -1;
		if (p[4] == 0x20)
			return 0;
		if (p[4] == 0x60)
			return p[5] / 8;
		if (p[4] == 0xA0)
		{
			int32_t disp;
			std::memcpy(&disp, p + 5, 4);
			return disp / 8;
		}
		return -1;
	}
} // namespace kzgs
