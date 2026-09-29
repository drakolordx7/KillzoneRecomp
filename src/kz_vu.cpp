#include "kz_vu.h"

#include "kzvu.h"

#include "ps2_runtime.h"
#include "runtime/ps2_host_vu.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace
{
    PS2Runtime *g_runtime = nullptr;
    uint64_t g_codeGeneration = ~0ull;
    constexpr uint32_t kBudget = 1u << 24; // run programs to their E bit (the runtime's VIF1 is synchronous)

    void onXgkick(void *, const uint8_t *packet, uint32_t bytes, uint32_t)
    {
        if (g_runtime)
            g_runtime->memory().submitGifPacket(GifPathId::Path1, packet, bytes);
    }

    void prepare(uint32_t top, uint32_t itop, uint32_t fbrst, uint64_t codeGeneration)
    {
        if (codeGeneration != g_codeGeneration)
        {
            kzvuMicroWritten(0, kKzvuCodeSize);
            g_codeGeneration = codeGeneration;
        }
        kzvuSetFBRST(fbrst);
        kzvuSetTop(top, itop);
    }

    void finish(uint32_t *vpuStat)
    {
        for (int i = 0; i < 16 && kzvuRunning() && (kzvuVpuStat() & 0x0600u) == 0u; ++i)
            kzvuContinue(kBudget);
        if (vpuStat)
            *vpuStat = (*vpuStat & ~0x0600u) | (kzvuVpuStat() & 0x0600u);
    }

    void onMscal(uint32_t startPc, uint32_t top, uint32_t itop, uint32_t fbrst, uint64_t gen, uint32_t *vpuStat)
    {
        prepare(top, itop, fbrst, gen);
        kzvuExecute(startPc, kBudget);
        finish(vpuStat);
    }

    void onMscnt(uint32_t top, uint32_t itop, uint32_t fbrst, uint64_t gen, uint32_t *vpuStat)
    {
        prepare(top, itop, fbrst, gen);
        kzvuExecute(0xFFFFFFFFu, kBudget);
        finish(vpuStat);
    }
}

bool kzVuInstall(std::string *error)
{
    const char *mode = std::getenv("KZ_VU");
    if (mode && std::strcmp(mode, "builtin") == 0)
    {
        std::cout << "[kz] VU1: PS2Recomp interpreter (KZ_VU=builtin)" << std::endl;
        return true;
    }
    KzvuConfig vc; // Killzone GameIndex: vuClampMode 0, IbitHack on (the kzvu defaults)
    vc.useJit = !(mode && std::strcmp(mode, "interp") == 0);
    vc.xgkick = &onXgkick;
    if (!kzvuInit(vc, error))
        return false;
    PS2HostVu1 hooks;
    hooks.codeMem = kzvuCodeMem();
    hooks.dataMem = kzvuDataMem();
    hooks.mscal = &onMscal;
    hooks.mscnt = &onMscnt;
    ps2SetHostVu1(hooks);
    std::cout << "[kz] VU1: " << (vc.useJit ? "microVU recompiler" : "PCSX2 interpreter") << std::endl;
    return true;
}

void kzVuBindRuntime(PS2Runtime &runtime)
{
    g_runtime = &runtime;
}
