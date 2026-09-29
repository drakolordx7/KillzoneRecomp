// kzspu2 shadow of pcsx2/SaveState.h: the two types spu2.h declares SPU2freeze() with. Savestates are not supported.
// SPDX-License-Identifier: GPL-3.0+
#pragma once

#include "common/Pcsx2Defs.h"

enum class FreezeAction
{
	Load,
	Save,
	Size,
};

struct freezeData
{
	int size;
	u8* data;
};
