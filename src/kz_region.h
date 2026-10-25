#pragma once

// Issue #15: one exe runs either the US disc (SCUS-97402, boot ELF SCUS_974.02) or the PAL disc (SCES-52893, En/Ru/El,
// boot ELF SCES_528.93). The region is chosen once, from the disc (SYSTEM.CNF BOOT2 + boot ELF CRC, kz_iso.cpp) or from
// the --elf file, before the ELF is loaded, and never changes afterwards. Guest addresses that differ between the two
// ELFs are in kz_addrs.h.

#include <cstdint>
#include <string>

enum class KzRegion : uint8_t
{
    Us,
    Pal,
};

KzRegion kzRegion();
inline bool kzIsPal() { return kzRegion() == KzRegion::Pal; }

const char *kzRegionTitle(KzRegion r);      // "Killzone (USA)" / "Killzone (Europe)"
const char *kzRegionSerial(KzRegion r);     // "SCUS-97402" / "SCES-52893"
const char *kzRegionBootElf(KzRegion r);    // "SCUS_974.02" / "SCES_528.93"
const char *kzRegionBootFolder(KzRegion r); // boot files and memory card next to the exe: "disc" / "disc_pal"
uint32_t kzRegionBootElfXorCrc(KzRegion r); // PCSX2-style XOR CRC of the boot ELF

// PAL needs the PAL generated code linked in (CMake KZ_GENERATED_PAL_DIR, KZ_HAVE_PAL); US is always there.
bool kzRegionSupported(KzRegion r);

// Sets the region and the guest address table (kz_addrs.h). For PAL it also fills the live function table with the PAL
// generated code (kz_pal_table.h). Call once, before the PS2Runtime is created and before anything writes a table
// entry (kzIpuBindRuntime, loadELF's overrides, the hooks). Returns false with a message when that is not possible.
bool kzSelectRegion(KzRegion r, std::string *error);
