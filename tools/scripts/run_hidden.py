"""Run the game exactly as a player does (real window, real audio device, no PS2X_HEADLESS) but on a private,
invisible Windows desktop, so it can neither take the focus nor show a window. The audio volume is forced to 0.
Usage: python run_hidden.py <exe dir> <seconds> [KEY=VALUE ...]
The game's own log is <exe dir>/killzone.log (written because stdout is not redirected)."""
import os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pcsx2_hidden import HiddenProcess

exe_dir = sys.argv[1]
seconds = float(sys.argv[2])
for kv in sys.argv[3:]:
    k, v = kv.split('=', 1)
    os.environ[k] = v
os.environ.pop('PS2X_HEADLESS', None)
os.environ['KZ_AUDIO_VOLUME'] = '0'  # never audible
iso = r'C:\Users\drakolord\Downloads\Killzone (USA)\Killzone (USA).iso'
p = HiddenProcess(os.path.join(exe_dir, 'killzone.exe'), ['--no-launcher', '--iso', iso], exe_dir, desktop='kz_hidden_run')
time.sleep(seconds)
p.kill()
print('done')
