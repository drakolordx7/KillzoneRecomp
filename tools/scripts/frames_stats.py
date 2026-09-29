"""Print non-black % and blue-red bias for every PNG in a folder. Usage: python frames_stats.py <dir> [glob]"""
import glob, os, sys
from PIL import Image

d = sys.argv[1]
pattern = sys.argv[2] if len(sys.argv) > 2 else "*.png"
for f in sorted(glob.glob(os.path.join(d, pattern))):
    im = Image.open(f).convert("RGB")
    px = im.get_flattened_data()
    n = len(px)
    nb = sum(1 for p in px if sum(p) > 30)
    print(f"{os.path.basename(f):24} {im.size[0]}x{im.size[1]}  nonblack={100*nb/n:5.1f}%  B-R={sum(p[2]-p[0] for p in px)/n:6.2f}")
