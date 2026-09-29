// kzspu2 shadow of pcsx2/IopDma.h: the three interrupt lines the SPU2 raises.
// SPDX-License-Identifier: GPL-3.0+
#pragma once

void spu2Irq();     // SPU2 IRQ -> IOP INTC 9
void spu2DMA4Irq(); // DMA ch4 (core 0) finished
void spu2DMA7Irq(); // DMA ch7 (core 1) finished
