#include "kz_addrs.h"

// US: SCUS_974.02 (the addresses the code always used). PAL: SCES_528.93, from work/pal/hookmap.csv, each checked
// against the PAL ELF (issue #15):
//  - code: the PAL function has the same instructions as the US one apart from relocated immediates (jal targets,
//    lui/lo16 of data), or, for the three hand ports, was found from its call site:
//      viewmodel 0x2204C0: the function changed, but it is called from the same site of the world render (PAL 0x1C2074,
//        same arguments) and it calls the frustum (0x16E2B0) with f13 = camera aspect (+0x74), which is all the
//        ultrawide hook relies on;
//      vkAddChar 0x1B1980: the PAL keyboard reader (0x1B0FB0) calls it with (obj, code) like the US one; its body matches
//        the US one apart from relocations;
//      hudTrace 0x26B9B8: same call site in the world render (PAL 0x1C20DC); trace only;
//  - data: a PAL instruction that references the same object (lui/lo16), or the next object at the same +0xE00 shift.
// Both tables list every field in declaration order (designated initializers); kzSelectRegion (kz_region.cpp) refuses a
// table with a field left at 0.

const KzAddrs kKzAddrsUs = {
    .gameObjectPtr = 0x00559178u,
    .bufferIndex = 0x0055A6B4u,
    .vsyncCounter = 0x0055A6E0u,
    .secondsPerVsync = 0x0055A6E4u,
    .bufferState = 0x0055A6E8u,
    .kickFlag = 0x0055A795u,
    .fadeLevel = 0x0055A7C8u,
    .movieCurrentPlayer = 0x0055B3D8u,
    .vsyncCountAcc = 0x0055F0E8u,
    .vsyncCountLast = 0x0055F0F0u,
    .nowSeconds = 0x0055F0FCu,
    .cpNameObj = 0x00564B48u,
    .lgkbmState = 0x00590D90u,
    .imageEnd = 0x0055A674u,

    .playerVtable = 0x00525710u,
    .ctlVtable = 0x00524B50u,
    .weaponVtable = 0x0052BD20u,
    .discStreamVtable = 0x0052EB28u,

    .ctrlUpdate = 0x0023EE08u,
    .applyLook = 0x0021C550u,
    .pitchAutoCentre = 0x0023E9C8u,
    .afterLookProbe = 0x00218710u,
    .vkKeyboard = 0x001B0DF0u,
    .vkAddChar = 0x001B17A0u,
    .vkBackspace = 0x001B18D8u,
    .buttonValue = 0x001B33B8u,

    .frameLimiter = 0x001BFF10u,
    .kick = 0x00150090u,
    .kickCheck = 0x00151FC8u,
    .vblankHandler = 0x00152018u,
    .waitD1Idle = 0x001759C0u,
    .waitKicked = 0x001759F8u,
    .lookAccel = 0x0023E8C8u,

    .playMovie = 0x0026ECC8u,
    .movieOpen = 0x002DED00u,
    .movieRead = 0x002DEB98u,
    .movieInputEnd = 0x002DF800u,
    .movieFeed = 0x002E0AC0u,
    .audioFull = 0x002E1A40u,
    .audioFeed = 0x002E19C0u,
    .stOpen = 0x00496168u,
    .stRead = 0x004963B8u,
    .fileReadAsync = 0x0014D790u,
    .filePoll = 0x0014DAD0u,

    .sifSearchModuleByName = 0x002B5D88u,
    .sifUnloadModule = 0x002B5CF8u,
    .sifLoadModule = 0x002B6060u,
    .sifStopModule = 0x002B5AF0u,
    .lgkbmInit = 0x003D7490u,

    .glowPass = 0x0015DFC0u,
    .frameBlend = 0x0015F768u,
    .lensBlur = 0x001A9308u,
    .postRender = 0x001EF640u,
    .zoneParams = 0x00339C38u,
    .lodScale = 0x001C2168u,

    .world = 0x001C1960u,
    .viewmodel = 0x00220018u,
    .frustum = 0x0016E2D8u,
    .ortho = 0x0016E420u,
    .text = 0x00167718u,
    .rect = 0x0015CF60u,
    .viewBuild = 0x003C8E70u,
    .viewBuildFromWorld = 0x001C1A64u,
    .renderFrameTrace = 0x001C0148u,
    .hudTrace = 0x0026B340u,
    .c3040Trace = 0x001C3040u,

    .objListQuery = 0x0017D318u,
};

const KzAddrs kKzAddrsPal = {
    .gameObjectPtr = 0x00559F78u,
    .bufferIndex = 0x0055B4B4u,
    .vsyncCounter = 0x0055B4E0u,
    .secondsPerVsync = 0x0055B4E4u,
    .bufferState = 0x0055B4E8u,
    .kickFlag = 0x0055B595u,
    .fadeLevel = 0x0055B5C8u,
    .movieCurrentPlayer = 0x0055C1D8u,
    .vsyncCountAcc = 0x0055FEE8u,
    .vsyncCountLast = 0x0055FEF0u, // next to vsyncCountAcc, as in US
    .nowSeconds = 0x0055FEFCu,
    .cpNameObj = 0x00565948u,
    .lgkbmState = 0x00591B90u, // the PAL init (0x3D80E0) addresses it as 0x1B90(s4) where the US one uses 0x0D90(s4)
    .imageEnd = 0x0055B47Cu,   // 0x140000 + filesz 0x41B47C

    .playerVtable = 0x00526390u,
    .ctlVtable = 0x005257D0u,
    .weaponVtable = 0x0052C9A8u,
    .discStreamVtable = 0x0052F7B8u,

    .ctrlUpdate = 0x0023F3A0u,
    .applyLook = 0x0021C9F8u,
    .pitchAutoCentre = 0x0023EF60u,
    .afterLookProbe = 0x00218C50u, // called at PAL 0x21B18C, the same place in the player update as US 0x21AC4C
    .vkKeyboard = 0x001B0FB0u,
    .vkAddChar = 0x001B1980u,
    .vkBackspace = 0x001B1AB8u,
    .buttonValue = 0x001B35D0u,

    .frameLimiter = 0x001C0278u,
    .kick = 0x00150068u,
    .kickCheck = 0x00151FA0u,
    .vblankHandler = 0x00151FF0u,
    .waitD1Idle = 0x00175998u,
    .waitKicked = 0x001759D0u,
    .lookAccel = 0x0023EE60u,

    .playMovie = 0x0026F338u,
    .movieOpen = 0x002DF800u,
    .movieRead = 0x002DF698u,
    .movieInputEnd = 0x002E0300u,
    .movieFeed = 0x002E15C0u,
    .audioFull = 0x002E2540u,
    .audioFeed = 0x002E24C0u,
    .stOpen = 0x00496E18u,
    .stRead = 0x00497068u,
    .fileReadAsync = 0x0014D770u, // the same callers as US (stOpen/stRead +0xCB0)
    .filePoll = 0x0014DAB0u,

    .sifSearchModuleByName = 0x002B6888u,
    .sifUnloadModule = 0x002B67F8u,
    .sifLoadModule = 0x002B6B60u,
    .sifStopModule = 0x002B65F0u,
    .lgkbmInit = 0x003D80E0u,

    .glowPass = 0x0015DF98u,
    .frameBlend = 0x0015F740u,
    .lensBlur = 0x001A94E0u,
    .postRender = 0x001EFC00u,
    .zoneParams = 0x0033A738u,
    .lodScale = 0x001C24D0u,

    .world = 0x001C1CC8u,
    .viewmodel = 0x002204C0u,
    .frustum = 0x0016E2B0u,
    .ortho = 0x0016E3F8u,
    .text = 0x001676F0u,
    .rect = 0x0015CF38u,
    .viewBuild = 0x003C9AB8u,
    .viewBuildFromWorld = 0x001C1DCCu,
    .renderFrameTrace = 0x001C04B0u,
    .hudTrace = 0x0026B9B8u,
    .c3040Trace = 0x001C33A8u,

    .objListQuery = 0x0017D2F0u,
};

namespace kz_detail
{
    const KzAddrs *g_addrs = &kKzAddrsUs;
}
