# Drive a reference PCSX2 run (tools/pcsx2) into Killzone gameplay without a keyboard or pad.
#
# Input: tools/scripts/pcsx2_pad_hook.pnach reroutes the game's only scePadRead call (0x416D98) through a cave at
# 0xFF000 that clears the buttons in the word at 0xFFF00 from the pad reply (active-high mask, cross also sets its
# pressure byte). This script writes that word over PINE. The pnach is copied into tools/pcsx2/patches for the run and
# removed afterwards; PCSX2.ini is pointed at the folder memory card memcards/KZ.ps2 (the port's profile save,
# game/mc0/BASCUS-974024BCCDDB8, plus a formatted _pcsx2_superblock) and restored afterwards.
#
# Commands are read from work/pcsx2_drive/cmd.txt (one per line, consumed): a button name (start, cross, circle,
# triangle, square, up, down, left, right, l1, r1, l2, r2, select), "hold <mask hex> <seconds>", "wait <seconds>",
# "save <slot>" (savestate to sstates/, via PINE), "shot <name>", "quit". A window screenshot is taken every 5 s
# (work/pcsx2_drive/shots), and the log is work/pcsx2_drive/drive.log.
# Path used for the 2026-09-29 gameplay reference (savestates 09/10): start (menu), cross x5 (Game, Campaign, profile,
# level, Easy), cross (Templar), start + cross (skip the intro movie), save 9.
import os, shutil, struct, subprocess, sys, time

sys.path.insert(0, r'D:\KillzoneRecomp\tools\scripts')
from pine import Pine

ROOT = r'D:\KillzoneRecomp'
W = os.path.join(ROOT, 'work', 'pcsx2_drive')
PCSX2 = os.path.join(ROOT, 'tools', 'pcsx2')
ISO = r'C:\Users\drakolord\Downloads\Killzone (USA)\Killzone (USA).iso'
PNACH_SRC = os.path.join(ROOT, 'tools', 'scripts', 'pcsx2_pad_hook.pnach')
PNACH_DST = os.path.join(PCSX2, 'patches', 'SCUS-97402_CAAEC49C.pnach')
INI = os.path.join(PCSX2, 'inis', 'PCSX2.ini')
BTN = {'select': 0x0001, 'start': 0x0008, 'up': 0x0010, 'right': 0x0020, 'down': 0x0040, 'left': 0x0080,
       'l2': 0x0100, 'r2': 0x0200, 'l1': 0x0400, 'r1': 0x0800, 'triangle': 0x1000, 'circle': 0x2000,
       'cross': 0x4000, 'square': 0x8000}


def make_memcard():
    root = os.path.join(PCSX2, 'memcards', 'KZ.ps2')
    os.makedirs(root, exist_ok=True)
    sb = bytearray(b'\xff' * 0x2000)  # PS2 memory card superblock of an 8 MB card
    hdr = bytearray(0x152)
    hdr[0:28] = b'Sony PS2 Memory Card Format '
    hdr[0x1c:0x28] = b'1.2.0.0'.ljust(12, b'\0')
    struct.pack_into('<HHHH', hdr, 0x28, 512, 2, 16, 0xFF00)
    struct.pack_into('<IIIIII', hdr, 0x30, 8192, 41, 8135, 0, 1023, 1022)
    struct.pack_into('<32I', hdr, 0x50, *([8] + [0] * 31))
    struct.pack_into('<32I', hdr, 0xd0, *([0xFFFFFFFF] * 32))
    hdr[0x150], hdr[0x151] = 2, 0x52
    sb[:len(hdr)] = hdr
    open(os.path.join(root, '_pcsx2_superblock'), 'wb').write(sb)
    save = os.path.join(root, 'BASCUS-974024BCCDDB8')
    if not os.path.exists(save):
        shutil.copytree(os.path.join(ROOT, 'game', 'mc0', 'BASCUS-974024BCCDDB8'), save)


def main():
    os.makedirs(os.path.join(W, 'shots'), exist_ok=True)
    log = open(os.path.join(W, 'drive.log'), 'a', buffering=1)
    make_memcard()
    ini_backup = open(INI).read()
    if '[MemoryCards]' not in ini_backup:
        open(INI, 'w').write(ini_backup + '\n[MemoryCards]\nSlot1_Enable = true\nSlot1_Filename = KZ.ps2\nSlot2_Enable = false\n')
    shutil.copy(PNACH_SRC, PNACH_DST)

    def shot(name):
        subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                        os.path.join(ROOT, 'tools', 'scripts', 'capture_window.ps1'), '-ProcessName', 'pcsx2-qt',
                        '-Out', os.path.join(W, 'shots', name + '.png')], capture_output=True)

    p = subprocess.Popen([os.path.join(PCSX2, 'pcsx2-qt.exe'), '-batch', '-fastboot', '--', ISO])
    t0 = time.time()
    try:
        pine = None
        for _ in range(60):
            try:
                pine = Pine(); break
            except OSError:
                time.sleep(1)
        log.write('pine connected %.1f\n' % (time.time() - t0))
        last_shot = 0
        cmdfile = os.path.join(W, 'cmd.txt')
        while p.poll() is None:
            now = time.time() - t0
            if now - last_shot >= 5:
                last_shot = now
                try:
                    vs, hook = pine.read32(0x55A6E0), pine.read32(0x416D98)
                except Exception:
                    vs = hook = -1
                shot('t%04d' % int(now))
                log.write('t=%.0f vsync=%d hook=%08x\n' % (now, vs, hook & 0xffffffff))
            if os.path.exists(cmdfile):
                lines = open(cmdfile).read().split('\n')
                os.remove(cmdfile)
                for ln in lines:
                    a = ln.split()
                    if not a:
                        continue
                    log.write('t=%.0f cmd %s\n' % (time.time() - t0, ln))
                    if a[0] in BTN:
                        pine.write32(0xFFF00, BTN[a[0]]); time.sleep(0.3); pine.write32(0xFFF00, 0); time.sleep(0.5)
                    elif a[0] == 'hold':
                        pine.write32(0xFFF00, int(a[1], 16)); time.sleep(float(a[2])); pine.write32(0xFFF00, 0)
                    elif a[0] == 'wait':
                        time.sleep(float(a[1]))
                    elif a[0] == 'save':
                        pine._call(struct.pack('<BB', 9, int(a[1])))
                    elif a[0] == 'shot':
                        shot(a[1])
                    elif a[0] == 'quit':
                        raise KeyboardInterrupt
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass
    finally:
        log.write('exit\n')
        p.kill()
        p.wait()
        open(INI, 'w').write(ini_backup)
        if os.path.exists(PNACH_DST):
            os.remove(PNACH_DST)


if __name__ == '__main__':
    main()
