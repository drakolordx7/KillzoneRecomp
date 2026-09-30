"""Per-run numbers from work/<name>/stderr.log over the gameplay window t=150..200 s.

  fps         (vif counter at t=200 - at t=150) / 50, the task's definition
  sleep       EE-thread time slept in vblank pacing (EeScheduler::processDueDeadlines), ms per second, mean of the 10 s
              [sched-stats] windows covering t=150..200 (run with PS2X_SCHED_STATS=1)
  busy/frame  (1000 - sleep) / fps: EE-thread host ms per displayed frame, guest code + scheduler. The EE thread never
              blocks otherwise (idleWaits=0 in every window), so this does not depend on which vblank a frame lands on.
  sched       scheduler share of run() outside the pacing sleep, ns per dispatch (rdtsc; includes ~15-20 ns of its own
              instrumentation, so compare between runs, do not read as absolute)
Usage: perf_metric.py name [name ...]
"""
import re
import statistics
import sys
import os

WORK = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'work')

for name in sys.argv[1:]:
    lines = open(os.path.join(WORK, name, 'stderr.log'), errors='replace').read().splitlines()
    vif, sleeps, disp, around = {}, [], [], []
    for ln in lines:
        m = re.search(r'headless\] t=(\d+)s .* vif=(\d+)', ln)
        if m:
            vif[int(m.group(1))] = int(m.group(2))
        m = re.search(r'slept for vblank pacing ([\d.]+) ms', ln)
        if m:
            sleeps.append(float(m.group(1)))
        m = re.search(r'\[sched-stats\] [\d.]+s per second: .*dispatch=(\d+)', ln)
        if m:
            disp.append(int(m.group(1)))
        m = re.search(r'scheduler around wait-loop dispatches ([\d.]+)%, scheduler around other dispatches ([\d.]+)%', ln)
        if m:
            around.append(float(m.group(1)) + float(m.group(2)))
    fps = (vif[200] - vif[150]) / 50.0 if 150 in vif and 200 in vif else float('nan')
    w = slice(15, 20)
    if not sleeps[w] or not (fps == fps and fps > 0):
        print(f'{name}: fps={fps:.1f} (no usable [sched-stats] windows)')
        continue
    sl = statistics.mean(sleeps[w])
    idx = [i for i in range(15, min(20, len(around), len(disp), len(sleeps))) if disp[i] > 0]
    # PS2X_SCHED_STATS=3 (pacing sleep only) reports dispatch=0: no per-dispatch figures then
    sched = f'{statistics.mean((around[i] * 10.0 - sleeps[i]) * 1e6 / disp[i] for i in idx):.0f} ns/dispatch' if idx else 'n/a (=3)'
    print(f'{name}: fps={fps:.1f}  sleep={sl:.0f} ms/s  busy/frame={(1000 - sl) / fps:.2f} ms  '
          f'dispatch/s={statistics.mean(disp[w]):.0f}  sched={sched}')
