"""Log survival test: start the game on a private invisible desktop (as a player does), run N seconds, then end it the way
a player does ('close': WM_CLOSE to its window = Alt+F4 / the X button; 'kill': TerminateProcess) and print the log size,
the number of lines and the tail. Run with stdout redirected to NUL (>NUL) so the game uses its log file.
Usage: python closetest.py <exe dir> <seconds> close|kill <result file>   (stdout must be NUL so the game logs to killzone.log; results go to the file)"""
import ctypes, ctypes.wintypes as wt, os, sys, time
_lines = []
def print(*a): _lines.append(' '.join(str(x) for x in a))
import atexit
atexit.register(lambda: open(sys.argv[4], 'w').write(chr(10).join(_lines) + chr(10)))
sys.path.insert(0, r'D:\KillzoneRecomp\tools\scripts')
from pcsx2_hidden import HiddenProcess, u32, k32
exe_dir, secs, mode = os.path.normpath(sys.argv[1]), float(sys.argv[2]), sys.argv[3]
os.environ.pop('PS2X_HEADLESS', None); os.environ['KZ_AUDIO_VOLUME'] = '0'
log = os.path.join(exe_dir, 'killzone.log')
if os.path.exists(log): os.remove(log)
iso = r'C:\Users\drakolord\Downloads\Killzone (USA)\Killzone (USA).iso'
p = HiddenProcess(os.path.join(exe_dir, 'killzone.exe'), ['--no-launcher', '--iso', iso], exe_dir, desktop='kz_close_test')
time.sleep(secs)
if mode == 'close':
    u32.OpenDesktopW.restype = wt.HANDLE
    hd = u32.OpenDesktopW('kz_close_test', 0, False, 0x1FF)
    found = []
    CB = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, l):
        pid = wt.DWORD(); u32.GetWindowThreadProcessId(h, ctypes.byref(pid))
        if pid.value == p.pid and u32.IsWindowVisible(h): found.append(h)
        return True
    u32.EnumDesktopWindows(hd, CB(cb), 0)
    print('windows', len(found))
    for h in found: u32.PostMessageW(h, 0x0010, 0, 0)  # WM_CLOSE
    t0 = time.time()
    while p.alive() and time.time() - t0 < 20: time.sleep(0.2)
    print('exited after close:', not p.alive())
p.kill()
b = open(log, 'rb').read() if os.path.exists(log) else b''
print('log bytes', len(b), 'lines', b.count(b'\n'))
print(b[-300:].decode('utf-8', 'replace'))
