#include "kz_ipu.h"

#include "kzipu.h"

#include "ps2_runtime.h"
#include "runtime/ps2_host_ipu.h"
#include "runtime/ee_scheduler.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace
{
    PS2Runtime *g_runtime = nullptr;
    bool g_active = false;

    uint8_t *dmaPtr(void *, uint32_t addr, uint32_t bytes, bool)
    {
        if (!g_runtime)
            return nullptr;
        PS2Memory &mem = g_runtime->memory();
        if (addr & 0x80000000u) // SPR bit: scratchpad
        {
            const uint32_t off = addr & 0x3FF0u;
            return off + bytes <= PS2_SCRATCHPAD_SIZE ? mem.getScratchpad() + off : nullptr;
        }
        addr &= 0x1FFFFFF0u;
        return addr + bytes <= PS2_RAM_SIZE ? mem.getRDRAM() + addr : nullptr;
    }

    void onIntc(void *)
    {
        if (g_runtime)
            g_runtime->eeScheduler().dispatchIrq(false, 8u); // INTC_IPU
    }

    void onDmacIrq(void *, int channel)
    {
        ps2HostCompleteDmac(static_cast<uint32_t>(channel));
        if (g_runtime)
            g_runtime->drainCompletedDmacHandlers(g_runtime->memory().getRDRAM());
    }

    void onLog(void *, int level, const char *msg)
    {
        if (level <= 1)
            std::cerr << "[kzipu] " << msg << std::endl;
    }

    bool handles(uint32_t a) { return kzipuHandlesAddress(a); }
    uint32_t read32(uint32_t a)
    {
        kzipuStep();
        return kzipuReadReg32(a);
    }
    void write32(uint32_t a, uint32_t v)
    {
        kzipuWriteReg32(a, v);
        kzipuStep();
    }
    uint64_t read64(uint32_t a)
    {
        kzipuStep();
        return kzipuReadReg64(a);
    }
    void write64(uint32_t a, uint64_t v)
    {
        kzipuWriteReg64(a, v);
        kzipuStep();
    }
    void fifoWrite(const void *qw)
    {
        kzipuWriteInFifo(qw, 1);
        kzipuStep();
    }
    void fifoRead(void *qw)
    {
        kzipuStep();
        if (kzipuReadOutFifo(qw, 1) == 0)
            std::memset(qw, 0, 16);
    }
    void dmacEnable(bool enabled)
    {
        kzipuSetDmacEnabled(enabled);
        kzipuStep();
    }
    void poll() { kzipuStep(); }
}

bool kzIpuInstall()
{
    // Opt-in until the game's video stream feed works end to end (KZ_IPU=on).
    const char *mode = std::getenv("KZ_IPU");
    if (!mode || std::strcmp(mode, "on") != 0)
    {
        std::cout << "[kz] IPU: off (set KZ_IPU=on to enable), videos are skipped" << std::endl;
        return false;
    }
    KzipuConfig cfg;
    cfg.dmaPtr = &dmaPtr;
    cfg.intc = &onIntc;
    cfg.dmacIrq = &onDmacIrq;
    cfg.log = &onLog;
    if (!kzipuInit(cfg))
    {
        std::cerr << "[kz] IPU init failed; videos are skipped" << std::endl;
        return false;
    }
    PS2HostIpu hooks;
    hooks.handles = &handles;
    hooks.read32 = &read32;
    hooks.write32 = &write32;
    hooks.read64 = &read64;
    hooks.write64 = &write64;
    hooks.fifoWrite = &fifoWrite;
    hooks.fifoRead = &fifoRead;
    hooks.dmacEnable = &dmacEnable;
    hooks.poll = &poll;
    ps2SetHostIpu(hooks);
    std::cout << "[kz] IPU: PCSX2 IPU (kzipu), videos enabled" << std::endl;
    g_active = true;
    return true;
}

void kzIpuBindRuntime(PS2Runtime &runtime)
{
    g_runtime = &runtime;
}

bool kzIpuActive()
{
    return g_active;
}
