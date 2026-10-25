"""Regenerate recompiled C++ and sync only changed files into generated/ (keeps rebuilds incremental).

Steps: merge SCE library bindings from ps2_analyzer and missed function starts from find_code_pointers.py into the
config (work/killzone.merged.toml), run ps2_recomp into work/gen_tmp, then copy changed/new files to generated/ and
delete stale ones.
Usage: python tools/scripts/regen.py [--pal]
"""
import filecmp, os, re, shutil, subprocess, sys, tomllib

# Killzone ships the register-locals code generator (patch 0017); PS2X_CODEGEN=classic regenerates the old style.
os.environ.setdefault('PS2X_CODEGEN', 'locals')
# Guest control flow without the scheduler (patch 0023): tail jumps followed by the caller (ps2_cg_dsp.h) and the entry_*
# fragments folded into their functions. PS2X_CODEGEN_DSP=0 / PS2X_CODEGEN_FOLD=0 give the patch-0017 output.
os.environ.setdefault('PS2X_CODEGEN_DSP', '1')
os.environ.setdefault('PS2X_CODEGEN_FOLD', '1')
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
# --pal (or KZ_CONFIG=config/killzone_pal.toml) regenerates the PAL set (issue #15). Every input and output path comes from
# one preset per config, so a PAL run can never write into generated/ or reuse the US analyzer/code-pointer files. The
# ELF, the output folder and the function list are read from the config's input / output / ghidra_output. Each path can
# still be overridden with its env var (KZ_ELF, KZ_GEN_DIR, KZ_FUNCS_CSV, KZ_ANALYZER_TOML, KZ_CODE_POINTERS,
# KZ_CODE_POINTERS_REJECT, KZ_MERGED, KZ_GEN_TMP, KZ_RECOMP_LOG); KZ_RECOMP_EXE picks another ps2_recomp.
if "--pal" in sys.argv[1:]:
    os.environ.setdefault("KZ_CONFIG", str(ROOT / "config" / "killzone_pal.toml"))
CFG = Path(os.environ.get("KZ_CONFIG", ROOT / "config" / "killzone.toml"))
PRESETS = {
    "killzone": dict(KZ_ANALYZER_TOML="work/analyzer.toml", KZ_CODE_POINTERS="work/code_pointers.txt",
                     KZ_CODE_POINTERS_REJECT="config/code_pointers_reject.txt", KZ_MERGED="work/killzone.merged.toml",
                     KZ_GEN_TMP="work/gen_tmp", KZ_RECOMP_LOG="work/recomp.log"),
    "killzone_pal": dict(KZ_ANALYZER_TOML="work/pal/analyzer_pal.toml", KZ_CODE_POINTERS="work/pal/code_pointers_pal.txt",
                         KZ_CODE_POINTERS_REJECT="work/pal/code_pointers_reject_pal.txt",
                         KZ_MERGED="work/pal/killzone_pal.merged.toml", KZ_GEN_TMP="work/pal/gen_tmp",
                         KZ_RECOMP_LOG="work/pal/recomp_pal.log"),
}
if CFG.stem not in PRESETS:
    sys.exit(f"[regen] no path preset for {CFG.name}; add one to PRESETS in regen.py")
_gen = tomllib.loads(CFG.read_text())["general"]
for _k, _v in {**PRESETS[CFG.stem], "KZ_ELF": _gen["input"], "KZ_GEN_DIR": _gen["output"],
               "KZ_FUNCS_CSV": _gen["ghidra_output"]}.items():
    os.environ.setdefault(_k, str(ROOT / _v) if not Path(_v).is_absolute() else _v)  # find_code_pointers.py reads these too
ELF = Path(os.environ["KZ_ELF"])
ANALYZER_TOML = Path(os.environ["KZ_ANALYZER_TOML"])
CODE_POINTERS = Path(os.environ["KZ_CODE_POINTERS"])
REJECT = Path(os.environ["KZ_CODE_POINTERS_REJECT"])
MERGED = Path(os.environ["KZ_MERGED"])
TMP = Path(os.environ["KZ_GEN_TMP"])
GEN = Path(os.environ["KZ_GEN_DIR"])
RECOMP = Path(os.environ.get("KZ_RECOMP_EXE", ROOT / "build" / "ps2recomp" / "ps2xRecomp" / "ps2_recomp.exe"))
ANALYZER = ROOT / "build" / "ps2recomp" / "ps2xAnalyzer" / "ps2_analyzer.exe"


def merged_config() -> str:
    text = CFG.read_text()
    if not ANALYZER_TOML.exists():
        subprocess.run([str(ANALYZER), str(ELF), str(ANALYZER_TOML)],
                       check=True, stdout=subprocess.DEVNULL)
    have = {s.split("@")[1].upper() for s in tomllib.loads(text)["general"]["stubs"] if "@" in s}
    extra = [s for s in tomllib.loads(ANALYZER_TOML.read_text())["general"].get("stubs", [])
             if "@" in s and s.split("@")[1].upper() not in have]
    lines = "".join(f'  "{s}",\n' for s in extra)
    text = re.sub(r"(stubs = \[\n)", lambda m: m.group(1) + lines, text, count=1)
    text = re.sub(r'^output = .*$', f'output = "{TMP.as_posix()}"', text, count=1, flags=re.M)
    # The config's paths are relative to the repo root; ps2_recomp gets them absolute.
    text = re.sub(r'^input = .*$', lambda m: f'input = "{ELF.resolve().as_posix()}"', text, count=1, flags=re.M)
    text = re.sub(r'^ghidra_output = .*$', lambda m: f'ghidra_output = "{Path(os.environ["KZ_FUNCS_CSV"]).resolve().as_posix()}"',
                  text, count=1, flags=re.M)
    print(f"[regen] +{len(extra)} SCE library bindings from ps2_analyzer")
    subprocess.run([sys.executable, str(ROOT / "tools" / "scripts" / "find_code_pointers.py")], check=True)
    entries = [l.strip() for l in CODE_POINTERS.read_text().splitlines() if l.strip()]
    entry_line = "entry_points = [" + ", ".join(f'"{e}"' for e in entries) + "]\n"
    text = re.sub(r"(\[general\]\n)", lambda m: m.group(1) + entry_line, text, count=1)
    print(f"[regen] +{len(entries)} entry points from find_code_pointers")
    return text


def sync() -> None:
    GEN.mkdir(exist_ok=True)
    new = {p.name for p in TMP.iterdir()}
    changed = 0
    for name in new:
        src, dst = TMP / name, GEN / name
        if not dst.exists() or not filecmp.cmp(src, dst, shallow=False):
            shutil.copy2(src, dst)
            changed += 1
    stale = [p for p in GEN.iterdir() if p.name not in new]
    for p in stale:
        p.unlink()
    print(f"[regen] {len(new)} files: {changed} changed/new, {len(stale)} removed")


def main() -> int:
    MERGED.write_text(merged_config())
    if TMP.exists():
        shutil.rmtree(TMP)
    log = Path(os.environ["KZ_RECOMP_LOG"])
    with log.open("w") as f:
        r = subprocess.run([str(RECOMP), str(MERGED)], stdout=f, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        print(f"[regen] ps2_recomp failed ({r.returncode}); see {log}")
        return r.returncode
    reject_data_candidates(log)
    sync()
    return 0


def reject_data_candidates(log: Path) -> None:
    """Record added entry points whose bodies failed to decode (data embedded in .text) so the next regen skips them."""
    cands = {int(l, 16) for l in CODE_POINTERS.read_text().split()}
    bad = set()
    for line in log.read_text(errors="replace").splitlines():
        m = re.search(r"\[error\] unhandled-instruction function=entry_([0-9a-f]+) ", line)
        if m and int(m.group(1), 16) in cands:
            bad.add(int(m.group(1), 16))
    reject = REJECT
    old = reject.read_text().splitlines() if reject.exists() else ["# data mistaken for code; generated by regen.py"]
    have = {l.split()[0].lower() for l in old if l.strip() and not l.startswith("#")}
    new = [f"0x{v:08X}" for v in sorted(bad) if f"0x{v:08x}" not in have]
    if new:
        reject.write_text("\n".join(old + new) + "\n")
        print(f"[regen] {len(new)} entry points rejected as data (take effect next regen)")


if __name__ == "__main__":
    sys.exit(main())
