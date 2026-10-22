"""Level N (default 2) as a player runs the game: real window + real audio device (volume 0) on a private invisible desktop,
menu path from lv2fast.ps1, from a fresh copy of work/saves/level2. Usage:
  python lv2hidden.py <name> <seconds> [--level 2] [--exe D:\KillzoneRecomp\build\agentw] [--script "110:r1:1000"] [KEY=VALUE ...]
Log: <exe dir>\killzone.log. Never audible (KZ_AUDIO_VOLUME=0)."""
import os, shutil, sys, time
sys.path.insert(0, r'D:\KillzoneRecomp\tools\scripts')
from pcsx2_hidden import HiddenProcess

args = sys.argv[1:]
name, seconds = args[0], float(args[1])
level, exe_dir, extra = 2, r'D:\KillzoneRecomp\build\agentw', ''
env = {}
sample = []
i = 2
while i < len(args):
    a = args[i]
    if a == '--level': level = int(args[i + 1]); i += 2
    elif a == '--exe': exe_dir = args[i + 1]; i += 2
    elif a == '--script': extra = args[i + 1]; i += 2
    elif a == '--sample': sample = ['--sample', args[i + 1]]; i += 2  # stack sampler -> <exe dir>\work\stacks.txt
    else:
        k, v = a.split('=', 1); env[k] = v; i += 1
mc = r'D:\KillzoneRecomp\work\saves\%s_mc' % name
if os.path.exists(mc): shutil.rmtree(mc)
shutil.copytree(r'D:\KillzoneRecomp\work\saves\level2', mc)
ups = 11 - level
s = '30:start:0.3;38:cross:0.3;46:cross:0.3;54:cross:0.3'
for k in range(ups): s += ';%g:up:0.2' % (66 + k * 1.5)
t = 66 + ups * 1.5 + 3
s += ';%g:cross:0.3;%g:cross:0.3;%g:cross:0.3' % (t, t + 6, t + 12)
if extra: s += ';' + extra
os.environ.update({'KZ_IPU': 'off', 'KZ_MC_ROOT': mc, 'KZ_INPUT_SCRIPT': s, 'KZ_AUDIO_DEVICE': '1', 'KZ_AUDIO_VOLUME': '0'})
os.environ.update(env)
os.environ.pop('PS2X_HEADLESS', None)
iso = r'C:\Users\drakolord\Downloads\Killzone (USA)\Killzone (USA).iso'
p = HiddenProcess(os.path.join(exe_dir, 'killzone.exe'), ['--no-launcher', '--iso', iso] + sample, exe_dir, desktop='kz_hidden_run_' + name)
t0 = time.time()
while time.time() - t0 < seconds and p.alive():
    time.sleep(2)
print('alive at end:', p.alive())
p.kill()
