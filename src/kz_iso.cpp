#include "kz_iso.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

namespace
{
    constexpr uint32_t kSector = 2048;

    uint32_t le32(const uint8_t *p)
    {
        return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }

    std::string upper(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return s;
    }

    bool readAt(const std::filesystem::path &path, uint64_t offset, void *dst, size_t size)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return false;
        f.seekg(static_cast<std::streamoff>(offset));
        f.read(static_cast<char *>(dst), static_cast<std::streamsize>(size));
        return static_cast<size_t>(f.gcount()) == size;
    }
}

bool KzIso::open(const std::filesystem::path &path, std::string *error)
{
    uint8_t pvd[kSector];
    if (!readAt(path, 16ull * kSector, pvd, sizeof(pvd)))
    {
        if (error) *error = "cannot read the file";
        return false;
    }
    if (pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0)
    {
        if (error) *error = "not an ISO9660 disc image";
        return false;
    }
    const uint8_t *root = pvd + 156;
    m_rootLba = le32(root + 2);
    m_rootSize = le32(root + 10);
    m_path = path;
    return true;
}

std::optional<KzIsoFile> KzIso::find(const std::string &rawPath) const
{
    std::string path = upper(rawPath);
    std::replace(path.begin(), path.end(), '\\', '/');
    uint32_t dirLba = m_rootLba, dirSize = m_rootSize;
    size_t start = 0;
    while (true)
    {
        const size_t slash = path.find('/', start);
        const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        const bool last = slash == std::string::npos;
        std::vector<uint8_t> dir(dirSize);
        if (dirSize == 0 || !readAt(m_path, static_cast<uint64_t>(dirLba) * kSector, dir.data(), dir.size()))
            return std::nullopt;
        bool found = false;
        for (size_t off = 0; off < dir.size();)
        {
            const uint8_t len = dir[off];
            if (len == 0)
            {
                off = (off / kSector + 1) * kSector; // records never straddle sectors
                continue;
            }
            const uint8_t nameLen = dir[off + 32];
            std::string name(reinterpret_cast<const char *>(&dir[off + 33]), nameLen);
            if (const size_t semi = name.find(';'); semi != std::string::npos)
                name.resize(semi);
            if (upper(name) == part)
            {
                const uint32_t lba = le32(&dir[off + 2]);
                const uint32_t size = le32(&dir[off + 10]);
                const bool isDir = (dir[off + 25] & 2) != 0;
                if (last)
                    return isDir ? std::nullopt : std::optional<KzIsoFile>(KzIsoFile{lba, size});
                if (!isDir)
                    return std::nullopt;
                dirLba = lba;
                dirSize = size;
                found = true;
                break;
            }
            off += len;
        }
        if (!found)
            return std::nullopt;
        start = slash + 1;
    }
}

bool KzIso::read(const KzIsoFile &file, std::vector<uint8_t> &out) const
{
    out.resize(file.size);
    return file.size == 0 || readAt(m_path, static_cast<uint64_t>(file.lba) * kSector, out.data(), out.size());
}

KzDiscCheck kzCheckDisc(const std::filesystem::path &isoPath)
{
    KzDiscCheck r;
    if (isoPath.empty())
    {
        r.message = "No disc image selected.";
        return r;
    }
    KzIso iso;
    std::string err;
    if (!iso.open(isoPath, &err))
    {
        r.message = "Can't use this file: " + err + ".";
        return r;
    }
    std::vector<uint8_t> cnf;
    const auto cnfFile = iso.find("SYSTEM.CNF");
    if (!cnfFile || !iso.read(*cnfFile, cnf))
    {
        r.message = "Not a PlayStation 2 disc (no SYSTEM.CNF).";
        return r;
    }
    const std::string text(cnf.begin(), cnf.end());
    if (text.find("SCUS_974.02") == std::string::npos)
    {
        r.message = "This is a different game or region. Killzone NTSC-U (SCUS-97402) is required.";
        return r;
    }
    std::vector<uint8_t> elf;
    const auto elfFile = iso.find("SCUS_974.02");
    if (!elfFile || !iso.read(*elfFile, elf))
    {
        r.message = "The game executable could not be read from the disc image.";
        return r;
    }
    elf.resize((elf.size() + 3) & ~size_t(3), 0);
    uint32_t crc = 0;
    for (size_t i = 0; i < elf.size(); i += 4)
        crc ^= le32(&elf[i]);
    if (crc != 0xCAAEC49Cu)
    {
        r.message = "Killzone NTSC-U found, but a different revision than v1.00; it is not supported.";
        return r;
    }
    r.ok = true;
    r.message = "Killzone (USA) v1.00 - SCUS-97402 - OK";
    return r;
}
