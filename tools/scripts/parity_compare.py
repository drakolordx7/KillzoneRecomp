"""Speed-parity table: PCSX2 reference vs port logs. See docs/findings.md "Speed parity with the original".

  python parity_compare.py            (uses the fixed file names under work/ listed in RUNS)

Port events come from the script times (the script clock is the vsync counter over R, equal to the log's wall column
within a tick); PCSX2 events are the '#ev' lines parity_pcsx2.py writes (vsync of the pad write).
"""
import sys, os
sys.path.insert(0, r'D:\KillzoneRecomp\tools\scripts')
from parity_analyze import Run
import numpy as np

W = r'D:\KillzoneRecomp\work'
MAG, WST, FL, CRA, CRB = 'w:0x180', 'w:0xE0', 'c:0xE0', 'c:0x374', 'c:0x370'


def ev_ref(r, label):
    return [vs for vs, t in r.events if t == label]


def g_at_wall(r, t):
    i = min(np.searchsorted(r.wall, t), len(r.G) - 1)
    return r.G[i], i


def g_at_vs(r, vs):
    i = min(np.searchsorted(r.vs, vs), len(r.G) - 1)
    return r.G[i], i


class Scen:
    """Event start of one scripted input on one run: row index and game time."""

    def __init__(self, r, port_t=None, ref_label=None, ref_nth=0):
        if port_t is not None:
            self.g, self.i = g_at_wall(r, port_t)
        else:
            self.g, self.i = g_at_vs(r, ev_ref(r, ref_label)[ref_nth])
        self.r = r


def plateau(r, g0, dur, step=0.25, top=1):
    t, v = r.speed_series(g0 - 0.05, g0 + dur, step)
    return float(np.mean(np.sort(v)[-top:])), t, v  # mean of the `top` fastest bins


def rise(r, g0, dur, vmax, frac=0.9):
    t, v = r.speed_series(g0 - 0.05, g0 + dur, 0.05)
    k = np.nonzero(v >= frac * vmax)[0]
    return float(t[k[0]] - 0.05 + 0.05) if len(k) else float('nan')


def first_row(r, name, i0, pred):
    w = r.words[name]
    for i in range(i0, len(w)):
        if pred(w[i]):
            return i
    return None


def fire_intervals(r, g0, i0, dur=2.0):
    w = r.words[MAG]
    iend = np.searchsorted(r.G, g0 + dur)
    ts, tv = [], []
    for i in range(i0 + 1, iend):
        if w[i] < w[i - 1] and w[i] > 0:
            ts.append(r.G[i]); tv.append(r.vs[i])
    ts = np.array(ts); tv = np.array(tv)
    return ts, tv


def metrics_move(r, tp, label):
    out = {}
    names = [('ly=1', 'back'), ('lx=-1', 'strafe_l'), ('lx=1', 'strafe_r')]
    for k, (lab, nm) in enumerate(names):
        s = Scen(r, port_t=tp[k]) if tp else Scen(r, ref_label=lab)
        v, t, series = plateau(r, s.g, 2.0)
        out['walk_' + nm + '_speed'] = v
        out['walk_' + nm + '_t90'] = rise(r, s.g, 2.0, v)
    return out


def turn_onset(r, g0, i0, thr=0.002):
    """Game time of the first heading change after row i0 (the frame in which the turn input took effect)."""
    h = r.h
    for i in range(i0, len(h)):
        if abs(h[i] - h[i0]) > thr:
            return r.G[i], i
    return g0, i0


def metrics_turn(r, tp):
    out = {}
    names = [('rx=1', 'rx1', 1.0), ('rx=-1', 'rx-1', 1.0), ('rx=0.5', 'rx.5', 2.0)]
    for k, (lab, nm, dur) in enumerate(names):
        # the port's script clock (vsync counter / R) and the log's wall column differ by up to ~0.1 s: search from before it
        s = Scen(r, port_t=tp[k] - 0.3) if tp else Scen(r, ref_label=lab)
        g_on, i_on = turn_onset(r, s.g, s.i)
        # angular speed series from the onset (frame in which the heading first changed), 0.1 s bins
        t, w, h = r.turn_series(g_on - 0.001, g_on + dur - 0.05, 0.1)
        w = np.abs(w)
        out['turn_%s_w@0.3s' % nm] = float(w[2])
        out['turn_%s_w@0.6s' % nm] = float(w[5])
        out['turn_%s_w@0.9s' % nm] = float(w[min(8, len(w) - 1)])
        if nm == 'rx.5':
            out['turn_rx.5_w@1.5s'] = float(w[14])
        out['turn_%s_angle_rad_%.1fs' % (nm, dur - 0.05)] = float(abs(h[-1]))
    return out


def metrics_sprint(r, tp):
    out = {}
    s = Scen(r, port_t=tp[1]) if tp else Scen(r, ref_label='l3+ly=-1')
    v, t, series = plateau(r, s.g, 2.5, 0.1, 3)
    out['sprint_speed'] = v
    out['sprint_t90'] = rise(r, s.g, 2.5, v)
    s0 = Scen(r, port_t=tp[0]) if tp else Scen(r, ref_label='rx=1')
    t, w, h = r.turn_series(s0.g - 0.05, s.g, 0.05)
    out['sprint_turn_pre_rad'] = float(abs(h[-1]))
    return out


def metrics_weapon(r, tp):
    out = {}
    fire = Scen(r, port_t=tp[0]) if tp else Scen(r, ref_label='r1')
    ts, tv = fire_intervals(r, fire.g, fire.i, 2.0)
    if len(ts) >= 3:
        out['fire_interval_s_game'] = float(np.mean(np.diff(ts)))
        out['fire_shots_per_s'] = float(1.0 / np.mean(np.diff(ts)))
        out['fire_first_shot_delay_s'] = float(ts[0] - fire.g)
        out['fire_shots_in_2s'] = int(len(ts))
    rl = Scen(r, port_t=tp[1]) if tp else Scen(r, ref_label='triangle')
    i_out = first_row(r, MAG, rl.i, lambda v: v == 0)
    if i_out is not None:
        i_in = first_row(r, MAG, i_out, lambda v: v >= 30)
        i_end = first_row(r, WST, i_out, lambda v: v == 0xE)
        out['reload_press_to_magout_s'] = float(r.G[i_out] - rl.g)
        if i_in is not None:
            out['reload_magout_to_magin_s'] = float(r.G[i_in] - r.G[i_out])
        if i_end is not None:
            out['reload_weaponstate_s'] = float(r.G[i_end] - r.G[i_out])
    sw = Scen(r, port_t=tp[2]) if tp else Scen(r, ref_label='circle')
    i_s = first_row(r, FL, sw.i, lambda v: v != 0)
    if i_s is not None:
        i_e = first_row(r, FL, i_s, lambda v: v == 0)
        out['switch_press_to_flag_s'] = float(r.G[i_s] - sw.g)
        if i_e is not None:
            out['switch_flag_s'] = float(r.G[i_e] - r.G[i_s])
    cr = Scen(r, port_t=tp[3]) if tp else Scen(r, ref_label='l2')
    iA = first_row(r, CRA, cr.i, lambda v: v == 2)
    if iA is not None:
        iB = first_row(r, CRB, iA, lambda v: v == 2)
        out['crouch_press_to_request_s'] = float(r.G[iA] - cr.g)
        if iB is not None:
            out['crouch_request_to_state_s'] = float(r.G[iB] - r.G[iA])
    return out


PFX = sys.argv[1] if len(sys.argv) > 1 else 'A'   # A = KZ_TIMEFIX=0 baseline runs, AF = with the fix

RUNS = {
    'M': (r'parity\ref_M.csv', PFX + '_%d.csv', [163, 167, 170, 173], metrics_move),
    'T': (r'parity\ref_T.csv', PFX + '_%d.csv', [177, 180, 183], metrics_turn),
    'SP': (r'parity\ref_SP.csv', 'B_%d.csv', [179, 180.5], metrics_sprint),
    'W': (r'parity\ref_W.csv', 'B_%d.csv', [163, 166, 171, 173], metrics_weapon),
}


def main():
    cols = ['pcsx2'] + [c for c in (30, 60, 120, 144, 240) if os.path.exists(os.path.join(W, 'B_%d.csv' % c))]
    table = {}
    for key, (refcsv, portcsv, tp, fn) in RUNS.items():
        for c in cols:
            try:
                if c == 'pcsx2':
                    r = Run(os.path.join(W, refcsv), 59.94)
                    m = fn(r, None) if key != 'M' else metrics_move(r, None, None)
                else:
                    r = Run(os.path.join(W, portcsv % c), c)
                    m = fn(r, tp) if key != 'M' else metrics_move(r, tp, None)
            except Exception as e:
                m = {'ERR': str(e)}
            for k, v in m.items():
                table.setdefault(k, {})[c] = v
    hdr = '%-30s' % 'quantity' + ''.join('%10s' % ('pcsx2' if c == 'pcsx2' else 'port@%d' % c) for c in cols)
    print(hdr + ' | ratio to pcsx2: ' + ' '.join('%6s' % ('@%d' % c) for c in cols[1:]))
    for k, row in table.items():
        ref = row.get('pcsx2')
        fmt = lambda x: '%10.3f' % x if isinstance(x, (int, float, np.floating)) else '%10s' % x
        rat = ''
        if isinstance(ref, (int, float, np.floating)) and ref:
            rat = ' '.join('%6.3f' % (row[c] / ref) if isinstance(row.get(c), (int, float, np.floating)) else '%6s' % '-' for c in cols[1:])
        print('%-30s%s | %s' % (k, ''.join(fmt(row.get(c)) for c in cols), rat))


if __name__ == '__main__':
    main()
