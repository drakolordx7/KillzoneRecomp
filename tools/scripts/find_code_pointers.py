"""Find function starts Ghidra missed, for regen.py to pass to ps2_recomp as entry_points.

Two sources:
  1. data pointers: aligned words in .data/.rodata/.sdata pointing into .text at a non-function address
     (vtables, callback tables, jump tables);
  2. prologues after a return: `addiu sp,sp,-N` that follows `jr ra` + delay slot (+ nop / `addiu v0,v0,0` padding).
     Ghidra sometimes extends a tiny function over the next one (e.g. entry_0043ffd0 swallowing the static
     constructor at 0x43FFE8, reached only via a runtime-built list), so these starts never appear as pointers.
Writes work/code_pointers.txt (one 0x... per line).

Usage: python tools/scripts/find_code_pointers.py
"""
import csv, struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ELF = ROOT / "game" / "SCUS_974.02"
CSV = ROOT / "work" / "functions.csv"
OUT = ROOT / "work" / "code_pointers.txt"
# Candidates whose recompiled body hit undecodable instructions (VU microcode / tables embedded in .text);
# written by regen.py after each recompile and excluded here.
REJECT = ROOT / "config" / "code_pointers_reject.txt"


def sections(d):
    shoff, = struct.unpack_from("<I", d, 0x20)
    shnum, shstrndx = struct.unpack_from("<HH", d, 0x30)
    sh = [struct.unpack_from("<10I", d, shoff + i * 40) for i in range(shnum)]
    stro = sh[shstrndx][4]
    for s in sh:
        name = d[stro + s[0]:d.index(b"\0", stro + s[0])].decode()
        yield name, s[1], s[3], s[4], s[5]  # name, type, addr, offset, size


def main():
    d = ELF.read_bytes()
    secs = {n: (t, a, o, sz) for n, t, a, o, sz in sections(d)}
    _, tstart, toff, tsize = secs[".text"]
    tend = tstart + tsize
    starts = set()
    with CSV.open() as f:
        for row in csv.DictReader(f):
            starts.add(int(row["Start"], 16))
    found = {}
    for name in (".data", ".rodata", ".sdata"):
        stype, addr, off, size = secs[name]
        for i in range(0, size - 3, 4):
            v, = struct.unpack_from("<I", d, off + i)
            if tstart <= v < tend and v % 4 == 0 and v not in starts:
                insn, = struct.unpack_from("<I", d, toff + (v - tstart))
                if insn == 0:
                    continue  # padding, not code
                found.setdefault(v, name)
    JR_RA = 0x03E00008
    PADDING = {0x00000000, 0x24420000}
    words = struct.unpack_from(f"<{tsize // 4}I", d, toff)
    for i, w in enumerate(words):
        if (w >> 16) != 0x27BD or not (w & 0x8000):
            continue  # not addiu sp,sp,-N
        a = tstart + i * 4
        if a in starts:
            continue
        j = i - 1
        while j >= 0 and words[j] in PADDING:
            j -= 1
        # words[j] is the delay slot (non-padding) or jr ra itself when the delay slot is a nop
        if (j >= 0 and words[j] == JR_RA) or (j >= 1 and words[j - 1] == JR_RA):
            found.setdefault(a, "prologue")
    rejected = set()
    if REJECT.exists():
        rejected = {int(l.split()[0], 16) for l in REJECT.read_text().splitlines() if l.strip() and not l.startswith("#")}
    for v in rejected:
        found.pop(v, None)
    by = {}
    for v, n in found.items():
        by[n] = by.get(n, 0) + 1
    OUT.write_text("".join(f"0x{v:08X}\n" for v in sorted(found)))
    print(f"[code-pointers] {len(found)} targets not in function map {by} ({len(rejected)} rejected as data) -> {OUT}")
    print("0x43FFE8 present:", 0x43FFE8 in found)


if __name__ == "__main__":
    main()
