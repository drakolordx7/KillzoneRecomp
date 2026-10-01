"""Analyse KZ_SHOT_BURST captures (gb_<burst>_<seq>_fld<field>_...png written by the port, see docs/findings.md "Menu jitter and blur").

  python tools/scripts/burst_analyze.py shift <dir>            vertical shift between consecutive presented frames of static content
  python tools/scripts/burst_analyze.py text <dir> X0 Y0 X1 Y1 detail (variance of the Laplacian) of a fixed text region per frame;
                                                               frames that blend two game frames show up as a second, lower level
Capture example (2560x1440 window, back buffer at Present, 24 frames per sample, one sample every 4 s from t=76 s):
  tools\scripts\run_headless.ps1 -Name jit -Seconds 190 -Interval 4 -Extra '--no-launcher' -EnvSet "KZ_FPS=120|KZ_WINDOW_SIZE=2560x1440|KZ_SHOT_BURST=24|KZ_SHOT_BURST_PRESENT=2|KZ_SHOT_BURST_FROM=76"
"""
import glob, os, re, sys
import numpy as np
from PIL import Image


def load(path):
    return np.asarray(Image.open(path).convert("L"), dtype=np.float64)


def lap(a):
    return a[1:-1, 1:-1] * 4 - a[:-2, 1:-1] - a[2:, 1:-1] - a[1:-1, :-2] - a[1:-1, 2:]


def lk_dy(a, b, bs=16):
    """Median vertical shift (pixels) of textured blocks of b relative to a (1-D Lucas-Kanade)."""
    gy = np.gradient(a, axis=0)
    it = b - a
    h, w = a.shape
    out = []
    for y0 in range(8, h - 8 - bs, bs):
        for x0 in range(0, w - bs, bs):
            g = gy[y0:y0 + bs, x0:x0 + bs]
            t = it[y0:y0 + bs, x0:x0 + bs]
            den = (g * g).sum()
            if den > bs * bs * 40:
                out.append(-(g * t).sum() / den)
    return np.array(out)


def bursts(d):
    return sorted(set(re.match(r"gb_(\d+)_", os.path.basename(f)).group(1) for f in glob.glob(d + "/gb_*_fld*.png")))


def shift(d):
    pairs = static = 0
    worst = 0.0
    for b in bursts(d):
        prev = None
        for f in sorted(glob.glob(f"{d}/gb_{b}_*_fld*.png")):
            im = load(f)[::2, ::2]
            if prev is not None:
                pairs += 1
                if np.abs(im - prev).mean() < 0.1:
                    r = lk_dy(prev, im)
                    if len(r) >= 20:
                        static += 1
                        worst = max(worst, abs(float(np.median(r))) * 2)
            prev = im
    print(f"{d}: {pairs} consecutive pairs, {static} static pairs, max vertical shift {worst:.3f} px")


def text(d, x0, y0, x1, y1):
    for b in bursts(d):
        v = [lap(load(f)[y0:y1, x0:x1]).var() for f in sorted(glob.glob(f"{d}/gb_{b}_*_fld*.png"))]
        print("burst", b, " ".join("%d" % x for x in v))


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "shift":
        shift(sys.argv[2])
    elif len(sys.argv) >= 7 and sys.argv[1] == "text":
        text(sys.argv[2], *map(int, sys.argv[3:7]))
    else:
        print(__doc__)
