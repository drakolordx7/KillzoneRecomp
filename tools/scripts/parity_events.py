"""Print the state-change timeline of a parity log: python parity_events.py <csv> <rate> [word ...]"""
import sys
sys.path.insert(0, r'D:\KillzoneRecomp\tools\scripts')
from parity_analyze import Run
import numpy as np
r = Run(sys.argv[1], float(sys.argv[2]))
names = sys.argv[3:] or [n for n in r.words]
last = None
for i in range(len(r.vs)):
    key = tuple(int(r.words[n][i]) for n in names)
    if key != last:
        print('G=%7.3f vs=%6d wall=%8.3f' % (r.G[i], r.vs[i], r.wall[i]), ' '.join('%s=%x' % (n, k) for n, k in zip(names, key)))
        last = key
