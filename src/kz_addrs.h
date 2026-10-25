#pragma once

// Guest addresses our code uses, per game version (issue #15). The US values are the ones the code always used
// (SCUS_974.02); the PAL values (SCES_528.93) come from work/pal/hookmap.csv and were checked against the PAL ELF's
// instruction words (same function body apart from relocations, or a call site that calls it the same way). Only real
// guest addresses are here: RAM masks, CRCs, range bounds and struct offsets stay where they are used.
//
// kzAddrs() is the table of the region chosen at boot (kz_region.h). It is valid before the region is chosen too (US).

#include <cstdint>

struct KzAddrs
{
    // ---- global data
    uint32_t gameObjectPtr;      // DAT_00559178: the game object (frame timer +0x50..+0x70, native 16:9 flag +0x34)
    uint32_t gameObjectVtable;   // the game object's vtable (word 0 of the object): tells a live object from reused memory
    uint32_t bufferIndex;        // DAT_0055a6b4: index of the command buffer being built
    uint32_t vsyncCounter;       // DAT_0055a6e0: vblank counter
    uint32_t secondsPerVsync;    // DAT_0055a6e4: float
    uint32_t bufferState;        // DAT_0055a6e8: one byte per command buffer
    uint32_t kickFlag;           // DAT_0055a795: byte
    uint32_t fadeLevel;          // DAT_0055a7c8: screen fade, int
    uint32_t movieCurrentPlayer; // DAT_0055b3d8: the movie player being serviced
    uint32_t vsyncCountAcc;      // DAT_0055f0e8
    uint32_t vsyncCountLast;     // DAT_0055f0f0
    uint32_t nowSeconds;         // DAT_0055f0fc: engine clock in seconds, float
    uint32_t cpNameObj;          // DAT_00564b48: name object of the checkpoint object-list query (devtools)
    uint32_t lgkbmState;         // DAT_00590d90: lgkbm library state block (the layout inside it is the same)
    uint32_t imageEnd;           // end of the ELF's loaded image (0x140000 + filesz), devtools stack scan

    // ---- vtables
    uint32_t playerVtable;     // player entity
    uint32_t ctlVtable;        // character controller (player + 0x31C)
    uint32_t weaponVtable;     // weapon object
    uint32_t discStreamVtable; // disc stream object of the movie player

    // ---- mouse aim and keyboard text entry (kz_aim.cpp)
    uint32_t ctrlUpdate;      // JoystickControllerPlayerImp update
    uint32_t applyLook;       // ApplyLook (also kz_timefix.cpp)
    uint32_t pitchAutoCentre; // pad pitch auto-centre
    uint32_t afterLookProbe;  // first call the player update makes after the scope block
    uint32_t vkKeyboard;      // on-screen keyboard: USB keyboard reader
    uint32_t vkAddChar;       // on-screen keyboard: add char (obj, code)
    uint32_t vkBackspace;     // on-screen keyboard: backspace (obj)
    uint32_t buttonValue;     // pad button value (pad, id), KZ_PAD_LOG

    // ---- frame timing and the frame pipeline
    uint32_t frameLimiter; // frame limiter / delta, once per main-loop pass
    uint32_t kick;         // the frame kick (D1 CHCR store)
    uint32_t kickCheck;    // vblank handler: kick check
    uint32_t vblankHandler;
    uint32_t waitD1Idle;   // wait for D1 idle, then set the kick flag
    uint32_t waitKicked;   // wait until the vblank handler kicked
    uint32_t lookAccel;    // stick look acceleration (kz_timefix.cpp)

    // ---- movies (kz_ipu.cpp, kz_movies.cpp, kz_overrides.cpp)
    uint32_t playMovie;     // blocking movie player
    uint32_t movieOpen;     // movie object's open method
    uint32_t movieRead;     // player read callback
    uint32_t movieInputEnd; // end-of-input check
    uint32_t movieFeed;
    uint32_t audioFull;
    uint32_t audioFeed;
    uint32_t stOpen;
    uint32_t stRead;
    uint32_t fileReadAsync;
    uint32_t filePoll;

    // ---- library bindings (kz_overrides.cpp)
    uint32_t sifSearchModuleByName;
    uint32_t sifUnloadModule;
    uint32_t sifLoadModule;
    uint32_t sifStopModule;
    uint32_t lgkbmInit;

    // ---- post-process and view distance (kz_post.cpp)
    uint32_t glowPass;
    uint32_t frameBlend;
    uint32_t lensBlur;
    uint32_t postRender;
    uint32_t zoneParams;
    uint32_t lodScale;

    // ---- ultrawide (kz_widescreen.cpp) and text (kz_prompts.cpp)
    uint32_t world;             // renders the 3D world for one view slot
    uint32_t viewmodel;         // first-person weapon
    uint32_t frustum;           // perspective frustum (f12 fov, f13 aspect, ...)
    uint32_t ortho;             // orthographic matrix
    uint32_t text;              // text draw
    uint32_t rect;              // screen rectangle fill
    uint32_t viewBuild;         // builds the render camera's planes/matrices (trace)
    uint32_t viewBuildFromWorld; // return address of the viewBuild call inside world (trace)
    uint32_t renderFrameTrace;  // trace points (KZ_WIDE_LOG)
    uint32_t hudTrace;
    uint32_t c3040Trace;

    // ---- devtools
    uint32_t objListQuery; // object-list query used by the checkpoint scan (KZ_CPDEBUG)
};

extern const KzAddrs kKzAddrsUs;
extern const KzAddrs kKzAddrsPal;

namespace kz_detail
{
    extern const KzAddrs *g_addrs;
}
inline const KzAddrs &kzAddrs() { return *kz_detail::g_addrs; }

// The game object *gameObjectPtr if it is a live game object (word 0 = gameObjectVtable), else 0 (before it is built,
// or if something has overwritten it). Per-vsync writes through the pointer check this first.
uint32_t kzLiveGameObject(const uint8_t *rdram);
