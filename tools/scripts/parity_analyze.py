"""Analysis helpers for the speed-parity logs (src/kz_parity.cpp for the port, parity_pcsx2.py for PCSX2).

Game time G of a row = sum over game frames of elapsed ticks * seconds per tick (a frame = a change of the frame timer's
"now" tick), i.e. the time the simulation has been advanced by, independent of the host clock. Emulated seconds
(vsync / R) are given too.
"""
import csv, struct
import numpy as np


def hexf(s):
    return struct.unpack('<f', struct.pack('<I', int(s, 16)))[0]


class Run:
    def __init__(self, path, rate):
        self.path, self.rate = path, rate
        rows, self.events = [], []
        with open(path) as f:
            rd = csv.reader(f)
            self.header = next(rd)
            for r in rd:
                if r and r[0].startswith('#ev'):
                    self.events.append((int(r[1]), r[2]))
                elif r and len(r) >= 11:
                    rows.append(r)
        a = np.array([[float(x) for x in r[:5]] for r in rows])
        self.wall, self.vs, self.now, self.el, self.spt = a.T
        self.x = np.array([float(r[7]) for r in rows])
        self.y = np.array([float(r[8]) for r in rows])
        self.z = np.array([float(r[9]) for r in rows])
        self.h = np.unwrap(np.array([float(r[10]) for r in rows]))
        self.words = {}
        for k, name in enumerate(self.header[11:]):
            self.words[name] = np.array([int(r[11 + k], 16) for r in rows], dtype=np.uint32)
        # A frame boundary is a change of the frame timer's "now" tick (+0x68, stored at the end of the wait loop). The
        # simulation step is the ticks since the last boundary times seconds per tick, capped like the game's clamp
        # (0.1333 s); the "elapsed" field itself can be read stale by a sampler, so it is not used.
        G = np.zeros(len(rows))
        g, last = 0.0, None
        for i in range(len(rows)):
            if last is not None and self.now[i] != last:
                g += min((self.now[i] - last) * self.spt[i], 0.1337)
            last = self.now[i]
            G[i] = g
        self.G = G
        self.sec = self.vs / rate  # emulated seconds from the vsync counter

    def word(self, name):
        return self.words[name]

    def frame_times(self):
        """Game time and emulated time of each distinct game frame start."""
        idx = np.nonzero(np.diff(self.now, prepend=-1) != 0)[0]
        return idx

    def at_G(self, arr, g):
        _, ui = np.unique(self.G, return_index=True)
        # last row of each distinct G
        last = np.r_[ui[1:] - 1, len(self.G) - 1]
        return np.interp(g, self.G[last], arr[last])

    def vs_to_G(self, vs):
        i = np.searchsorted(self.vs, vs)
        i = min(max(i, 0), len(self.G) - 1)
        return self.G[i]

    def first_change(self, name, after_vs=0, value=None):
        w = self.words[name]
        base = w[np.searchsorted(self.vs, after_vs)] if value is None else None
        for i in range(np.searchsorted(self.vs, after_vs), len(w)):
            if (value is None and w[i] != base) or (value is not None and w[i] == value):
                return i
        return None

    def speed_series(self, g0, g1, step=0.25):
        gs = np.arange(g0, g1 + 1e-9, step)
        px, py = self.at_G(self.x, gs), self.at_G(self.y, gs)
        v = np.hypot(np.diff(px), np.diff(py)) / step
        return gs[:-1] - g0, v

    def turn_series(self, g0, g1, step=0.25):
        gs = np.arange(g0, g1 + 1e-9, step)
        h = self.at_G(self.h, gs)
        return gs[:-1] - g0, np.diff(h) / step, h - h[0]
