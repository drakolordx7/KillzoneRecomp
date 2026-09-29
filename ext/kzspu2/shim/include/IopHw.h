// kzspu2 shadow of pcsx2/IopHw.h: the IOP DMA registers of channels 4 and 7 (MADR/BCR/CHCR/TADR). PCSX2's SPU2 code
// reads and advances them while a transfer runs. They live in a kzspu2-private copy of the IOP hardware page; the host
// reads MADR through kzspu2DmaMadr().
// SPDX-License-Identifier: GPL-3.0+
#pragma once

#include "IopMem.h"

extern u8 iopHw[0x10000];

#define psxHu16(mem) (*(u16*)&iopHw[(mem) & 0xffff])
#define psxHu32(mem) (*(u32*)&iopHw[(mem) & 0xffff])

#define HW_DMA4_MADR (psxHu32(0x10c0)) // SPU DMA
#define HW_DMA4_BCR (psxHu32(0x10c4))
#define HW_DMA4_CHCR (psxHu32(0x10c8))
#define HW_DMA4_TADR (psxHu32(0x10cc))

#define HW_DMA7_MADR (psxHu32(0x1500)) // SPU2 DMA
#define HW_DMA7_BCR (psxHu32(0x1504))
#define HW_DMA7_CHCR (psxHu32(0x1508))
#define HW_DMA7_TADR (psxHu32(0x150C))
