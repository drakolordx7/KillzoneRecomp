r"""Ultrawide flicker checker. Classifies each presented frame of KZ_SHOT_BURST_PRESENT=2 captures (gb_*.png in <dir>) taken with
KZ_ASPECT=21:9 at about 21:9 window size as
  W wide       ammo digits at their designed place (HUD compressed into the middle 16:9 part, world widened)
  S stretched  ammo digits found elsewhere (nearer the screen edge) = the 16:9 picture stretched over the wide window
  P pillarbox  black bars left and right (a 16:9 frame presented pillarboxed)
  ?            ammo digits not found (HUD hidden, menu, loading ...)
Template: .weaver/wide_check_tpl.png (ammo digits cut from a 2580x1080 gameplay frame, matched as a bright-pixel mask, scaled to the frame size).
  python .weaver\wide_check.py <dir> [--expect-wide] [-v]
--static [--thr T]: motionless scene, frames that differ from the burst median are bad (also catches a partly widened world).
--expect-wide: ignores frames before the first wide one, pass/fail line, --keep-bad moves the bad frames to <dir>/bad/;, every non-W frame counts as bad (use on gameplay captures only)."""
import glob, os, re, sys
import numpy as np
import cv2

HERE = os.path.dirname(os.path.abspath(__file__))
TPL = cv2.imread(os.path.join(HERE, "wide_check_tpl.png"), cv2.IMREAD_GRAYSCALE)
EXPECT = 1900 / 2580.0  # left edge of the template (ammo digits) in a wide frame, fraction of width
TOL = 0.012

def classify(path):
    a = cv2.imread(path, cv2.IMREAD_GRAYSCALE)
    h, w = a.shape
    if a[:, : int(w * 0.06)].max() < 10 and a[:, -int(w * 0.06):].max() < 10:
        return "P", 0.0, 0.0
    t = cv2.resize(TPL, (max(8, round(TPL.shape[1] * w / 2580.0)), max(8, round(TPL.shape[0] * h / 1080.0))), interpolation=cv2.INTER_AREA)
    y0 = int(h * 0.86)
    strip = (a[y0:, :] > 200).astype(np.float32)
    t = (t > 200).astype(np.float32)
    res = cv2.matchTemplate(strip, t, cv2.TM_CCOEFF_NORMED)
    _, score, _, loc = cv2.minMaxLoc(res)
    x = loc[0] / w
    if score < 0.6:
        return "?", x, score
    return ("W" if abs(x - EXPECT) < TOL else "S"), x, score

def static_check(d):
    """--static: the capture is of a motionless scene (no input). Per burst the pixel-wise median frame is the reference; a frame is bad when
    its mean absolute difference from it over the upper 75% of the picture (world, no weapon/HUD strip) exceeds --thr (default 3.0 grey levels).
    A wide frame whose world was partly drawn with the 16:9 camera differs by far more."""
    thr = 3.0
    if "--thr" in sys.argv:
        thr = float(sys.argv[sys.argv.index("--thr") + 1])
    files = sorted(glob.glob(os.path.join(d, "gb_*_fld*.png")))
    bursts = {}
    for f in files:
        m = re.match(r"gb_(\d+)_(\d+)_", os.path.basename(f))
        bursts.setdefault(m.group(1), []).append((int(m.group(2)), f))
    n = nb = 0
    started = False
    for b, lst in sorted(bursts.items()):
        lst = sorted(lst)
        ims = [cv2.imread(f, cv2.IMREAD_GRAYSCALE) for _, f in lst]
        if not started:
            if classify(lst[0][1])[0] != "W":
                continue
            started = True
        h = ims[0].shape[0]
        ims = [i[: int(h * 0.75)].astype(np.float32) for i in ims]
        med = np.median(np.stack(ims), axis=0)
        ds = [float(np.abs(i - med).mean()) for i in ims]
        bad = [i for i, x in enumerate(ds) if x > thr]
        n += len(ds)
        nb += len(bad)
        print(f"burst {b}: max {max(ds):.2f} mean {sum(ds)/len(ds):.2f} " + "".join("X" if x > thr else "." for x in ds))
        if "--keep-bad" in sys.argv and bad:
            os.makedirs(os.path.join(d, "bad"), exist_ok=True)
            for i in bad:
                os.replace(lst[i][1], os.path.join(d, "bad", os.path.basename(lst[i][1])))
    print(f"{d}: static check, {n} frames, BAD {nb}" + (" (PASS)" if nb == 0 and n else " (FAIL)"))

def main():
    d = sys.argv[1]
    if "--static" in sys.argv:
        return static_check(d)
    verbose = "-v" in sys.argv
    files = sorted(glob.glob(os.path.join(d, "gb_*_fld*.png")))
    bursts = {}
    for f in files:
        m = re.match(r"gb_(\d+)_(\d+)_", os.path.basename(f))
        bursts.setdefault(m.group(1), []).append((int(m.group(2)), f))
    tot = {"W": 0, "S": 0, "P": 0, "?": 0}
    started = "--expect-wide" not in sys.argv  # with --expect-wide, frames before the first wide frame (loading, menus) are not counted
    bad = []
    for b, lst in sorted(bursts.items()):
        s, info = "", []
        for seq, f in sorted(lst):
            c, x, sc = classify(f)
            if not started and c == "W":
                started = True
            if not started:
                s += "-"
                continue
            if c != "W":
                bad.append(f)
            tot[c] += 1
            s += c
            if c not in "W" or verbose:
                info.append(f"{seq}:{c}@{x:.3f}/{sc:.2f}")
        print(f"burst {b}: {s}" + ("  " + " ".join(info) if info and (verbose or "S" in s or "?" in s) else ""))
    n = sum(tot.values())
    print(f"{d}: {n} frames, wide {tot['W']}, stretched {tot['S']}, pillarbox {tot['P']}, unknown {tot['?']}")
    if "--expect-wide" in sys.argv:
        nb = n - tot["W"]
        print(f"BAD frames: {nb} of {n}" + (" (PASS)" if nb == 0 and n else " (FAIL)"))
        if "--keep-bad" in sys.argv and bad:
            os.makedirs(os.path.join(d, "bad"), exist_ok=True)
            for f in bad:
                os.replace(f, os.path.join(d, "bad", os.path.basename(f)))

main()
