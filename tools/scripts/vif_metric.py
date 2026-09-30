"""Per-run numbers for the VIF1 worker work, from work/<name>/stderr.log.

  fps       (vif counter at t=200 - at t=150) / 50 (the task's definition; [headless] lines)
  fps2      the same over t=200..230 (settled scene, after the microVU compile burst)
  worker    ms of worker time per job (PS2X_VIF1_STATS=1 [vif1-thread] lines whose t is in (150 s, 200 s]); one job per frame
  vu1       ms per job inside the host VU1 (MSCAL/MSCNT incl. XGKICK -> GS and JIT compile)
  gs        ms per job in the host GS packet hook (GifArbiter::drain -> kzgsGifTransfer)
  jobs/s    jobs per second in that window
Usage: vif_metric.py name [name ...]   (run with PS2X_VIF1_STATS=1 to get the worker columns)
"""
import os
import re
import statistics
import sys

WORK = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'work')


def window(rows, lo, hi):
    sel = [r for r in rows if lo < r['t'] <= hi]
    jobs = sum(r['jobs'] for r in sel)
    if not jobs:
        return None
    secs = sum(r['dt'] for r in sel)
    return (sum(r['worker'] for r in sel) / jobs, sum(r['vu1'] for r in sel) / jobs,
            sum(r['gs'] for r in sel) / jobs, jobs / secs if secs else 0.0)


for name in sys.argv[1:]:
    lines = open(os.path.join(WORK, name, 'stderr.log'), errors='replace').read().splitlines()
    vif, rows, prev = {}, [], 0.0
    for ln in lines:
        m = re.search(r'headless\] t=(\d+)s .* vif=(\d+)', ln)
        if m:
            vif[int(m.group(1))] = int(m.group(2))
        m = re.search(r'\[vif1-thread\] t=(\d+)ms jobs=(\d+) .*worker=([\d.]+)ms \(vu1 ([\d.]+)ms, hostgs ([\d.]+)ms\)', ln)
        if m:
            t = int(m.group(1)) / 1000.0
            rows.append(dict(t=t, dt=t - prev, jobs=int(m.group(2)), worker=float(m.group(3)), vu1=float(m.group(4)),
                             gs=float(m.group(5))))
            prev = t
    fps = (vif[200] - vif[150]) / 50.0 if 150 in vif and 200 in vif else float('nan')
    fps2 = (vif[230] - vif[200]) / 30.0 if 230 in vif and 200 in vif else float('nan')
    w = window(rows, 150, 200)
    w2 = window(rows, 200, 240)
    s = f'{name}: fps={fps:.1f} fps2={fps2:.1f}'
    if w:
        s += f'  worker={w[0]:.2f} ms  vu1={w[1]:.2f}  gs={w[2]:.2f}  jobs/s={w[3]:.0f}'
    if w2:
        s += f'  | t>200: worker={w2[0]:.2f} vu1={w2[1]:.2f} gs={w2[2]:.2f}'
    print(s)
