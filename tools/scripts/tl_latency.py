#!/usr/bin/env python3
"""Input-to-present latency and queue depth from a KZ_TIMELINE recording (patch 0025; see docs/findings.md "Input latency").

  tl_latency.py <timeline.csv>

Per game frame: start = the frame limiter's exit (FUN_001bff10, address 1834768) just before the frame's D1 kick (the game polls
the pad right after it and simulates and builds the frame list in the rest of the frame); kick = the D1 kick of the finished
list (only in a vblank); done = DONE_POST (VIF1 worker finished the list); present = the end of the first GS-thread frame
(GS_VSYNC_E) of the first vsync the producer pushes after `done` (i-th push = i-th GS frame). latency = present - start (host time, ms). Queue depth = the kzgs frame count after each
push (GS_QUEUED, 1 = nothing waiting) and the time the producer waited for the GS thread (GS_THROTTLE).
"""
import bisect, statistics, sys
sys.path.insert(0, r'D:\KillzoneRecomp\tools\scripts')
from tl_analyze import load, pct

LIMITER = 1834768


def main():
    ev = load(sys.argv[1])
    exits = [t for t, l, n, a, b, c in ev if n == 'G_EXIT' and a == LIMITER]
    kicks = [t for t, l, n, a, b, c in ev if n == 'D1_KICK']
    dones = [t for t, l, n, a, b, c in ev if n == 'DONE_POST']
    gsb = [t for t, l, n, a, b, c in ev if n == 'GS_VSYNC_B']
    gse = [t for t, l, n, a, b, c in ev if n == 'GS_VSYNC_E']
    vbl = [t for t, l, n, a, b, c in ev if n == 'VBLANK']
    queued = [a for t, l, n, a, b, c in ev if n == 'GS_QUEUED']
    pushes = [t for t, l, n, a, b, c in ev if n == 'GS_QUEUED']  # the producer's kzgsVsync push; the i-th push is the i-th GS frame
    thr = []
    open_t = None
    for t, l, n, a, b, c in ev:
        if n == 'GS_THROTTLE_B':
            open_t = t
        elif n == 'GS_THROTTLE_E' and open_t is not None:
            thr.append(t - open_t)
            open_t = None
    # GS frames of pushes made before the recording window: skip them
    # (those finished before the first push, plus the ones still in flight then: queued - 1 at that push)
    skip = (bisect.bisect_left(gse, pushes[0]) + max(queued[0] - 1, 0)) if pushes else 0
    gse_p = gse[skip:]
    lat, ee_part, kick_part, worker_part, present_part = [], [], [], [], []
    for k in kicks:
        i = bisect.bisect_left(exits, k - 1.0) - 1  # last limiter exit before the kick (1 us margin)
        if i < 0:
            continue
        start = exits[i]
        if k - start > 100000.0:  # no limiter exit logged for >100 ms (a hitch or a gap in the recording): skip
            continue
        j = bisect.bisect_left(dones, k)
        if j >= len(dones):
            continue
        done = dones[j]
        m = bisect.bisect_left(pushes, done)  # the first vsync pushed after the list was done carries its picture
        if m >= len(gse_p) or m >= len(pushes):
            continue
        pe = gse_p[m]
        lat.append((pe - start) / 1000.0)
        worker_part.append((done - k) / 1000.0)
        present_part.append((pe - done) / 1000.0)
        kick_part.append((k - start) / 1000.0)
    if not lat:
        print('no frames with a limiter exit / kick / present in the recording (kicks %d, GS frames %d): wrong window?' % (len(kicks), len(gse)))
        return
    print('frames: %d (kicks %d, vblanks %d, GS frames %d, window %.1f s)' % (len(lat), len(kicks), len(vbl), len(gse), (ev[-1][0] - ev[0][0]) / 1e6))
    vp = statistics.median([b - a for a, b in zip(vbl, vbl[1:])]) / 1000.0 if len(vbl) > 2 else float('nan')
    print('vblank period %.3f ms' % vp)

    def line(name, v):
        print('%-34s n=%d mean %.2f  med %.2f  p10 %.2f  p90 %.2f  p99 %.2f  max %.2f ms' % (
            name, len(v), statistics.mean(v), statistics.median(v), pct(v, .1), pct(v, .9), pct(v, .99), max(v)))

    line('frame start -> D1 kick', kick_part)
    line('kick -> worker done', worker_part)
    line('done -> present end', present_part)
    line('INPUT -> PRESENT END (total)', lat)
    if vp == vp:
        print('  = %.2f vblank periods (median), p90 %.2f' % (statistics.median(lat) / vp, pct(lat, .9) / vp))
    if queued:
        hist = {}
        for q in queued:
            hist[q] = hist.get(q, 0) + 1
        print('kzgs frames queued after each push:', ' '.join('%d:%d' % (k, hist[k]) for k in sorted(hist)), ' max', max(queued))
    print('producer throttle waits: %d, total %.1f ms, max %.2f ms' % (len(thr), sum(thr) / 1000.0, max(thr) / 1000.0 if thr else 0))
    if len(pushes) > 10 and len(gse_p) > 10:
        n0 = min(len(pushes), len(gse_p))
        line('push -> GS thread done', [(gse_p[i] - pushes[i]) / 1000.0 for i in range(n0)])


if __name__ == '__main__':
    main()
