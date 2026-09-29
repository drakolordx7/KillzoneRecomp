# Usage (as a module): p2s.read(<file.p2s>, <member>) -> bytes. PCSX2 savestates are zips with zstd members
# (eeMemory.bin, Scratchpad.bin, eeHwRegs.bin, vu1Memory.bin, Screenshot.png, ...); needs the zstandard package.
# Read a member of a PCSX2 .p2s savestate (zip with zstd members).
import zipfile, zstandard, struct, sys
def read(path, member):
    z = zipfile.ZipFile(path)
    info = z.getinfo(member)
    f = z.fp; f.seek(info.header_offset)
    hdr = f.read(30); n, e = struct.unpack_from('<HH', hdr, 26)
    f.seek(info.header_offset + 30 + n + e)
    raw = f.read(info.compress_size)
    if info.compress_type == 0: return raw
    return zstandard.ZstdDecompressor().decompress(raw, max_output_size=info.file_size)
