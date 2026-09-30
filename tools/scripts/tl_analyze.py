#!/usr/bin/env python3
"""Frame-pipeline timeline analysis (patch 0025). Input: the CSV written with KZ_TIMELINE=<file> (t_us,lane,event,a,b,c).

  tl_analyze.py <csv>                     summary of the whole window + per-frame-class breakdown
  tl_analyze.py <csv> --frames N [K]      table of N consecutive frames (kick to kick), starting at frame K (default: middle)
  tl_analyze.py <csv> --dump T0_MS D_MS   merged event list for D ms starting T0 ms after the first event (GS_IDLE hidden)

Definitions (all times in ms of host time):
  frame     = the interval between two consecutive D1 kicks (the game kicks one frame list per frame, only in the vblank handler)
  vblanks   = how many guest vblanks lie between the two kicks (1 = one frame per vblank = 120 fps at KZ_FPS=120)
  EE awake  = frame length minus the EE thread's sleeps in the scheduler (pacing sleep in processDueDeadlines + idle waits)
  worker DMA= JOB_B..JOB_E of the DMA job that the kick queued (VU1 + GIF work, includes waits for the GS thread)
  D1 done   = DONE_POST time minus kick time (the worker posted the completion event)
  applied   = EE applied the completion (STR clear, D_STAT, DMAC handler queued) minus DONE_POST
"""
import csv, sys, statistics, bisect, collections


def load(path):
    ev = []
    with open(path) as f:
        r = csv.reader(f)
        next(r)
        for t, lane, name, a, b, c in r:
            ev.append((float(t), lane, name, int(a), int(b), int(c)))
    ev.sort(key=lambda e: e[0])
    t0 = ev[0][0]
    return [(t - t0, l, n, a, b, c) for t, l, n, a, b, c in ev]


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p))] if v else float('nan')


def stat(v):
    if not v:
        return 'n=0'
    return f'n={len(v)} mean={statistics.mean(v):.2f} med={statistics.median(v):.2f} p10={pct(v,.1):.2f} p90={pct(v,.9):.2f} max={max(v):.2f}'


def spans(ev, b, e, key=lambda a, bb: 0):
    out, open_ = [], {}
    for t, lane, name, a, bb, c in ev:
        if name == b:
            open_[key(a, bb)] = (t, a, bb)
        elif name == e:
            k = key(a, bb)
            if k in open_:
                t0, a0, b0 = open_.pop(k)
                out.append((t0, t, a0, b0))
    return out


class Cover:
    """Total length of a set of spans clipped to [t0,t1]."""
    def __init__(self, sp):
        self.sp = sorted(sp)
        self.st = [s for s, _ in self.sp]

    def total(self, t0, t1):
        i = max(0, bisect.bisect_left(self.st, t0) - 1)
        tot = 0.0
        while i < len(self.sp) and self.sp[i][0] < t1:
            s, e = self.sp[i]
            tot += max(0.0, min(e, t1) - max(s, t0))
            i += 1
        return tot


def nxt(arr, t):
    i = bisect.bisect_left(arr, t)
    return arr[i] if i < len(arr) else None


def analyse(ev):
    dur_us = ev[-1][0]
    dur = dur_us / 1e6
    vb = [t for t, l, n, a, b, c in ev if n == 'VBLANK']
    kicks = [(t, a, b, c) for t, l, n, a, b, c in ev if n == 'D1_KICK' and b & 1]
    print(f'window {dur:.1f} s, {len(ev)} events; vblanks {len(vb)} ({len(vb)/dur:.1f}/s), D1 kicks {len(kicks)} ({len(kicks)/dur:.1f}/s)')
    print('vblank interval ms:', stat([(y - x) / 1000 for x, y in zip(vb, vb[1:])]))
    kt = [k[0] for k in kicks]
    print('kick delay after its vblank ms:', stat([(t - vb[bisect.bisect_right(vb, t) - 1]) / 1000 for t in kt if bisect.bisect_right(vb, t) > 0]))
    slp = spans(ev, 'SLEEP_B', 'SLEEP_E', key=lambda a, b: a)
    pace = Cover([(s, e) for s, e, a, b in slp if a == 0])
    idle = Cover([(s, e) for s, e, a, b in slp if a == 1])
    bar = spans(ev, 'BARRIER_B', 'BARRIER_E', key=lambda a, b: a)
    jobs = spans(ev, 'JOB_B', 'JOB_E')
    dma = sorted([j for j in jobs if j[2] == 1])
    gsj = [j for j in jobs if j[2] == 3]
    mscal = [j for j in jobs if j[2] == 2]
    posts = sorted(t for t, l, n, a, b, c in ev if n == 'DONE_POST' and a == 1)
    applies = sorted(s for s, e, a, b in spans(ev, 'APPLY_B', 'APPLY_E'))
    idle_seen = sorted(t for t, l, n, a, b, c in ev if n == 'CHCR_IDLE' and a == 1)
    gsv = spans(ev, 'GS_VSYNC_B', 'GS_VSYNC_E')
    gidle_e = [(t, a / 1000.0) for t, l, n, a, b, c in ev if n == 'GS_IDLE_E']  # (time of the frame start, idle ms since the previous one)
    thr = spans(ev, 'GS_THROTTLE_B', 'GS_THROTTLE_E')
    ring = spans(ev, 'GS_RING_B', 'GS_RING_E')
    dstart = [s for s, e, a, b in dma]

    print('\n-- EE thread --')
    print('  pacing sleep (processDueDeadlines): %.1f%% of the window; idle waits %.1f%%' % (100 * sum(e - s for s, e, a, b in slp if a == 0) / dur_us, 100 * sum(e - s for s, e, a, b in slp if a == 1) / dur_us))
    print('  barrier waits (worker busy when the EE touched VIF/VU state): n=%d, %.1f ms total = %.2f%% of the window' % (len(bar), sum(e - s for s, e, a, b in bar) / 1000, 100 * sum(e - s for s, e, a, b in bar) / dur_us))
    lim = sorted(t for t, l, n, a, b, c in ev if n == 'G_ENTER' and a == 0x1bff10)
    print('  frame limiter entries (FUN_001bff10): %d; period ms: %s' % (len(lim), stat([(y - x) / 1000 for x, y in zip(lim, lim[1:])])))
    hooks = collections.Counter(a for t, l, n, a, b, c in ev if n == 'G_ENTER')
    print('  guest hook entries:', {hex(k): v for k, v in sorted(hooks.items())})
    # kick check at the vblank handler: b = flag | state<<8 | cur<<16
    chk = [(t, b) for t, l, n, a, b, c in ev if n == 'G_ENTER' and a == 0x151fc8]
    cls = collections.Counter(((b & 0xff), ((b >> 8) & 0xff)) for t, b in chk)
    print('  vblank kick check (flag, next-buffer state) -> count (kick needs flag!=0 and state==2):', dict(sorted(cls.items())))

    print('\n-- worker (VIF1/VU1/GIF) --')
    print('  DMA job ms:', stat([(e - s) / 1000 for s, e, a, b in dma]))
    print('  GS half of vblank (kz runVsync) ms:', stat([(e - s) / 1000 for s, e, a, b in gsj]))
    print('  kick -> job start ms:', stat([(nxt(dstart, t - 50) - t) / 1000 for t in kt if nxt(dstart, t - 50) is not None]))
    print('  kick -> DONE_POST ms:', stat([(nxt(posts, t) - t) / 1000 for t in kt if nxt(posts, t) is not None]))
    print('  DONE_POST -> applied on EE ms:', stat([(nxt(applies, p) - p) / 1000 for p in posts if nxt(applies, p) is not None]))
    if idle_seen:
        print('  kick -> EE poll first sees D1 idle ms:', stat([(nxt(idle_seen, t) - t) / 1000 for t in kt if nxt(idle_seen, t) is not None]))
    print('  busy: DMA jobs %.1f%%, all jobs %.1f%% of the window (jobs include waits for the GS thread)' % (100 * sum(e - s for s, e, a, b in dma) / dur_us, 100 * sum(e - s for s, e, a, b in jobs) / dur_us))
    print('  blocked in kzgs frame throttle: n=%d, %.1f ms; ring full: n=%d, %.1f ms' % (len(thr), sum(e - s for s, e, a, b in thr) / 1000, len(ring), sum(e - s for s, e, a, b in ring) / 1000))
    print('  frames queued in kzgs after each push:', dict(collections.Counter(a for t, l, n, a, b, c in ev if n == 'GS_QUEUED')))

    print('\n-- GS thread (kzgs / PCSX2 renderer) --')
    print('  GSvsync (render+present of one frame) ms:', stat([(e - s) / 1000 for s, e, a, b in gsv]))
    gi = sum(a for t, a in gidle_e) * 1000 / dur_us
    print('  idle: %.1f%% of the window => busy %.1f%% = %.2f ms per vblank (GS work is in the GIF transfers and draws, GSvsync itself is the present)' % (100 * gi, 100 * (1 - gi), (1 - gi) * dur_us / 1000 / len(vb)))

    # ---- per frame ----
    def vcount(t0, t1):
        return bisect.bisect_left(vb, t1 - 100) - bisect.bisect_left(vb, t0 - 100)

    frames = []
    for k in range(len(kt) - 1):
        t0, t1 = kt[k], kt[k + 1]
        n = vcount(t0, t1)
        awake = (t1 - t0) - pace.total(t0, t1) - idle.total(t0, t1)
        p = nxt(posts, t0)
        j = nxt(dstart, t0 - 50)
        je = None
        if j is not None:
            for s, e, a, b in dma:
                if s == j:
                    je = e
                    break
        frames.append(dict(t0=t0, t1=t1, n=n, len=(t1 - t0) / 1000, awake=awake / 1000, pace=pace.total(t0, t1) / 1000,
                           done=(p - t0) / 1000 if p else float('nan'), job=(je - j) / 1000 if je else float('nan'),
                           gsidle=sum(a for t, a in gidle_e if t0 <= t < t1), kickph=(t0 - vb[bisect.bisect_right(vb, t0) - 1]) / 1000))
    print('\n-- frames by length (kick to kick) --')
    bycl = collections.defaultdict(list)
    for f in frames:
        bycl[min(f['n'], 3)].append(f)
    print('  vblanks per frame -> count:', {k: len(v) for k, v in sorted(bycl.items())})
    hdr = '  %-9s %6s %8s %8s %9s %8s %8s %8s'
    print(hdr % ('class', 'n', 'len ms', 'EE awake', 'EE pacing', 'D1 done', 'job ms', 'GS idle'))
    for k, v in sorted(bycl.items()):
        m = lambda key: statistics.mean(x[key] for x in v if x[key] == x[key])
        print(hdr % ('%d vblank%s' % (k, '' if k == 1 else 's') + ('+' if k == 3 else ''), len(v), '%.2f' % m('len'), '%.2f' % m('awake'), '%.2f' % m('pace'), '%.2f' % m('done'), '%.2f' % m('job'), '%.2f' % m('gsidle')))
    aw = [f['awake'] for f in frames]
    print('  EE awake time per frame ms (all frames):', stat(aw))
    vp = statistics.median([(y - x) / 1000 for x, y in zip(vb, vb[1:])])   # vblank period, ms
    over = sum(1 for f in frames if f['awake'] > vp)
    print('  frames whose EE awake time exceeds one vblank (%.2f ms): %d of %d (%.1f%%)' % (vp, over, len(frames), 100 * over / max(1, len(frames))))
    two = [f for f in frames if f['n'] >= 2]
    late = sum(1 for f in two if f['done'] > vp)
    print('  of the %d frames that took >= 2 vblanks: D1 completion later than one vblank after the kick in %d, EE awake time under one vblank in %d' % (len(two), late, sum(1 for f in two if f['awake'] < vp)))
    return frames


def print_frames(ev, n, start=None):
    fr = analyse_quiet(ev)
    if start is None:
        start = len(fr) // 2
    print('%5s %9s %3s %7s %7s %7s %7s %7s %7s' % ('frame', 't ms', 'vb', 'len', 'awake', 'pacing', 'D1done', 'job', 'kickph'))
    for i, f in enumerate(fr[start:start + n], start):
        print('%5d %9.1f %3d %7.2f %7.2f %7.2f %7.2f %7.2f %7.2f' % (i, f['t0'] / 1000, f['n'], f['len'], f['awake'], f['pace'], f['done'], f['job'], f['kickph']))


def analyse_quiet(ev):
    import io, contextlib
    with contextlib.redirect_stdout(io.StringIO()):
        return analyse(ev)


if __name__ == '__main__':
    ev = load(sys.argv[1])
    if len(sys.argv) > 2 and sys.argv[2] == '--dump':
        s = float(sys.argv[3]) * 1000
        d = float(sys.argv[4]) * 1000
        for t, l, n, a, b, c in ev:
            if s <= t < s + d and n != 'GS_IDLE_E':
                print(f'{t/1000:10.3f} ms {l:6s} {n:14s} a={a:#x} b={b:#x} c={c}')
    elif len(sys.argv) > 2 and sys.argv[2] == '--frames':
        print_frames(ev, int(sys.argv[3]), int(sys.argv[4]) if len(sys.argv) > 4 else None)
    else:
        analyse(ev)
