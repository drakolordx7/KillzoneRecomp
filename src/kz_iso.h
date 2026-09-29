#pragma once

// Minimal ISO9660 reader: enough to validate the user's disc image and read files from its root/IOP directories.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct KzIsoFile
{
    uint32_t lba = 0;
    uint32_t size = 0;
};

class KzIso
{
public:
    bool open(const std::filesystem::path &path, std::string *error = nullptr);
    // Path like "SYSTEM.CNF" or "IOP/PADMAN.IRX" (case-insensitive, ";1" version suffix optional).
    std::optional<KzIsoFile> find(const std::string &path) const;
    bool read(const KzIsoFile &file, std::vector<uint8_t> &out) const;

private:
    std::filesystem::path m_path;
    uint32_t m_rootLba = 0;
    uint32_t m_rootSize = 0;
};

// Result of checking that an image is Killzone NTSC-U v1.00 (SCUS-97402, boot ELF CRC CAAEC49C).
struct KzDiscCheck
{
    bool ok = false;
    std::string message; // human-readable status for the launcher
};
KzDiscCheck kzCheckDisc(const std::filesystem::path &iso);

// Extracts the small boot files (SYSTEM.CNF, SCUS_974.02, IOP/*.IRX) from the disc image into `dir` (skipping files that
// already exist with the right size). The large data files stay in the image and are read by sector.
// Returns the path of the boot ELF, or empty on failure (message in *error).
std::filesystem::path kzPrepareDiscFiles(const std::filesystem::path &iso, const std::filesystem::path &dir, std::string *error);
