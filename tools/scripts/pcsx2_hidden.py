"""Run the reference PCSX2 (tools/pcsx2_par, a copy of tools/pcsx2 with its own PINE slot 28111) on a private,
invisible Windows desktop, so it can neither take the focus nor show a window on the user's desktop.
Used by parity_pcsx2.py. The process is a normal child; kill() ends it and closes the desktop."""
import ctypes, ctypes.wintypes as wt, os, subprocess, time

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
u32 = ctypes.WinDLL('user32', use_last_error=True)


class STARTUPINFOW(ctypes.Structure):
    _fields_ = [('cb', wt.DWORD), ('lpReserved', wt.LPWSTR), ('lpDesktop', wt.LPWSTR), ('lpTitle', wt.LPWSTR),
                ('dwX', wt.DWORD), ('dwY', wt.DWORD), ('dwXSize', wt.DWORD), ('dwYSize', wt.DWORD),
                ('dwXCountChars', wt.DWORD), ('dwYCountChars', wt.DWORD), ('dwFillAttribute', wt.DWORD),
                ('dwFlags', wt.DWORD), ('wShowWindow', wt.WORD), ('cbReserved2', wt.WORD),
                ('lpReserved2', ctypes.c_void_p), ('hStdInput', wt.HANDLE), ('hStdOutput', wt.HANDLE),
                ('hStdError', wt.HANDLE)]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [('hProcess', wt.HANDLE), ('hThread', wt.HANDLE), ('dwProcessId', wt.DWORD), ('dwThreadId', wt.DWORD)]


u32.CreateDesktopW.restype = wt.HANDLE
u32.CreateDesktopW.argtypes = [wt.LPCWSTR, wt.LPCWSTR, ctypes.c_void_p, wt.DWORD, wt.DWORD, ctypes.c_void_p]
u32.CloseDesktop.argtypes = [wt.HANDLE]
k32.CreateProcessW.argtypes = [wt.LPCWSTR, wt.LPWSTR, ctypes.c_void_p, ctypes.c_void_p, wt.BOOL, wt.DWORD,
                               ctypes.c_void_p, wt.LPCWSTR, ctypes.POINTER(STARTUPINFOW),
                               ctypes.POINTER(PROCESS_INFORMATION)]
k32.TerminateProcess.argtypes = [wt.HANDLE, wt.UINT]
k32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
k32.CloseHandle.argtypes = [wt.HANDLE]


class HiddenProcess:
    def __init__(self, exe, args, cwd, desktop='kz_parity_desktop'):
        self.desk = u32.CreateDesktopW(desktop, None, None, 0, 0x10000000, None)
        if not self.desk:
            raise OSError('CreateDesktop failed %d' % ctypes.get_last_error())
        si = STARTUPINFOW()
        si.cb = ctypes.sizeof(si)
        si.lpDesktop = desktop
        pi = PROCESS_INFORMATION()
        cmd = subprocess.list2cmdline([exe] + list(args))
        if not k32.CreateProcessW(exe, cmd, None, None, False, 0, None, cwd, ctypes.byref(si), ctypes.byref(pi)):
            raise OSError('CreateProcess failed %d' % ctypes.get_last_error())
        self.hp, self.pid = pi.hProcess, pi.dwProcessId
        k32.CloseHandle(pi.hThread)

    def alive(self):
        return k32.WaitForSingleObject(self.hp, 0) == 0x102

    def kill(self):
        if self.hp:
            k32.TerminateProcess(self.hp, 1)
            k32.WaitForSingleObject(self.hp, 5000)
            k32.CloseHandle(self.hp)
            self.hp = None
        if self.desk:
            u32.CloseDesktop(self.desk)
            self.desk = None
