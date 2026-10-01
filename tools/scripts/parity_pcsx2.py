"""Speed-parity measurement of the ORIGINAL game in PCSX2 (docs/findings.md "Speed parity with the original").

Runs tools/pcsx2_par (PINE slot 28111) on a private invisible desktop (pcsx2_hidden.py), loads a gameplay savestate,
injects pad input through the v2 pad cave (pcsx2_pad_hook2.pnach: 18 bytes at 0xFFF10 replace the pad reply when
0xFFF00 != 0), and samples game memory over PINE as fast as it goes. The output CSV has the same columns as the
port's KZ_PARITY_LOG (src/kz_parity.cpp): wall_s,vsync,now,elapsed,sec_per_tick,player,ctl,x,y,z,heading,<words>.

  python parity_pcsx2.py --state 10 --script "1:r1:0.3;3:ly=-1:4" --seconds 9 --words "p:0x148,c:0x454" --out walk.csv

--script uses the port's KZ_INPUT_SCRIPT syntax (t:button[:dur], t:ly=-1:dur, stick values -1..1; t = seconds after the
sampling starts). tools/pcsx2_par is a copy of tools/pcsx2 (own PINE slot, the pnach in its patches folder).
"""
import argparse, os, re, struct, sys, time

sys.path.insert(0, r'D:\KillzoneRecomp\tools\scripts')
from pine import Pine
from pcsx2_hidden import HiddenProcess

ROOT = r'D:\KillzoneRecomp'
PCSX2 = os.path.join(ROOT, 'tools', 'pcsx2_par')
ISO = r'C:\Users\drakolord\Downloads\Killzone (USA)\Killzone (USA).iso'
PORT = 28111
SPU2_MUTED = ('\n\n[SPU2/Output]\nBackend = Null\nOutputMuted = true\nStandardVolume = 0\n'
              'FastForwardVolume = 0\nOutputVolume = 0\n')
PLAYER_VT = 0x525710

# digital bit (active-low in the reply) and pressure byte index (0 = reply byte 8) per button
BUTTONS = {'select': (0, None), 'l3': (1, None), 'r3': (2, None), 'start': (3, None),
           'up': (4, 2), 'right': (5, 0), 'down': (6, 3), 'left': (7, 1),
           'l2': (8, 10), 'r2': (9, 11), 'l1': (10, 8), 'r1': (11, 9),
           'triangle': (12, 4), 'circle': (13, 5), 'cross': (14, 6), 'square': (15, 7)}


def parse_script(text):
    ev = []
    for item in [i for i in text.split(';') if i.strip()]:
        t, what = item.split(':', 1)
        dur = 0.15
        if ':' in what:
            what, d = what.split(':', 1)
            dur = float(d)
        if '=' in what:
            k, v = what.split('=')
            ev.append((float(t), dur, k.lower(), float(v)))
        else:
            ev.append((float(t), dur, what.lower(), None))
    return ev


def pad_block(buttons, lx, ly, rx, ry):
    """18 bytes for pad reply bytes 2..19 (the buttons are active-low)."""
    mask = 0
    pres = [0] * 12
    for b in buttons:
        bit, p = BUTTONS[b]
        mask |= 1 << bit
        if p is not None:
            pres[p] = 0xFF
    return struct.pack('<H', (~mask) & 0xFFFF) + bytes([rx, ry, lx, ly]) + bytes(pres)


def stick(v):
    return max(0, min(255, int(round(127.5 + v * 127.5))))


def fl(u):
    return struct.unpack('<f', struct.pack('<I', u))[0]


def _read_block_list(self, addrs):
    out = b''
    for i in range(0, len(addrs), 500):
        part = addrs[i:i + 500]
        payload = b''.join(struct.pack('<BI', 2, a) for a in part)
        self.s.sendall(struct.pack('<I', len(payload) + 4) + payload)
        size = struct.unpack('<I', self._recv(4))[0]
        body = self._recv(size - 4)
        if body[0] != 0:
            raise RuntimeError('PINE batch error')
        out += body[1:]
    return out


Pine.read_block_list = _read_block_list


class Ref:
    def __init__(self, renderer=None):
        self.ini = os.path.join(PCSX2, 'inis', 'PCSX2.ini')
        self.proc = None
        self.pine = None
        self.renderer = renderer

    def ensure_ini(self):
        """Silence PCSX2 (never any audible output) and apply the renderer choice; verified after the write."""
        txt = open(self.ini).read()
        txt = re.sub(r'(?ms)^\[SPU2/Output\][^\[]*', '', txt)
        txt = txt.rstrip() + SPU2_MUTED
        if self.renderer is not None:
            txt = re.sub(r'(?m)^Renderer = .*$', 'Renderer = %d' % self.renderer, txt)
        open(self.ini, 'w').write(txt)
        chk = open(self.ini).read()
        assert 'Backend = Null' in chk and 'OutputMuted = true' in chk, 'PCSX2 audio is not muted'

    def start(self):
        self.ensure_ini()
        self.proc = HiddenProcess(os.path.join(PCSX2, 'pcsx2-qt.exe'), ['-batch', '-fastboot', '--', ISO], PCSX2)
        t0 = time.time()
        while time.time() - t0 < 90:
            try:
                self.pine = Pine(port=PORT)
                self.pine.s.settimeout(60)
                return
            except OSError:
                time.sleep(1)
        raise RuntimeError('PINE did not come up')

    def stop(self):
        if self.proc:
            self.proc.kill()

    def r32(self, a):
        return self.pine.read32(a)

    def batch(self, addrs):
        return struct.unpack('<%dI' % len(addrs), self.pine.read_block_list(addrs))

    def load_state(self, slot):
        self.pine._call(struct.pack('<BB', 0x0A, slot))

    def save_state(self, slot):
        self.pine._call(struct.pack('<BB', 9, slot))

    def set_pad(self, buttons=(), lx=0.0, ly=0.0, rx=0.0, ry=0.0, enable=True):
        blk = pad_block(buttons, stick(lx), stick(ly), stick(rx), stick(ry)) + b'\0\0'
        for i in range(5):
            self.pine.write32(0xFFF10 + 4 * i, struct.unpack_from('<I', blk, 4 * i)[0])
        self.pine.write32(0xFFF00, 1 if enable else 0)


def parse_words(spec):
    words = []
    for item in [i for i in (spec or '').split(',') if i]:
        words.append((item, item[0], [int(x, 0) for x in item[2:].split('>')]))
    return words


def run(ref, args):
    player = int(args.player, 0)
    if ref.r32(player) != PLAYER_VT:
        raise RuntimeError('player vtable not at 0x%x: %08x' % (player, ref.r32(player)))
    ctl = ref.r32(player + 0x31C)
    obj = ref.r32(0x559178)
    words = parse_words(args.words)
    events = parse_script(args.script)
    rows = []
    fixed = [0x55A6E0, obj + 0x50, obj + 0x64, obj + 0x68, ctl + 0x50, ctl + 0x54, ctl + 0x58, ctl + 0x78]
    bases = {'p': player, 'c': ctl, 'g': obj, 'w': int(args.weapon, 0), 'a': 0}
    t0 = time.time()
    last = None
    pad_state = None
    while True:
        t = time.time() - t0
        if t >= args.seconds:
            break
        btn, ax = set(), {'lx': 0.0, 'ly': 0.0, 'rx': 0.0, 'ry': 0.0}
        for (st, dur, name, val) in events:
            if st <= t < st + dur:
                if name in ax:
                    ax[name] = val
                elif name in BUTTONS:
                    btn.add(name)
        key = (tuple(sorted(btn)), tuple(ax.values()))
        if key != pad_state:
            pad_state = key
            ref.set_pad(btn, **ax)
            rows.append('#ev,%d,%s' % (ref.r32(0x55A6E0), '+'.join(sorted(btn) + ['%s=%g' % kv for kv in ax.items() if kv[1]]) or 'idle'))
        finals = []
        for (txt, base, chain) in words:
            a = bases[base] + chain[0]
            for off in chain[1:]:
                a = ref.r32(a)
                a = a + off if 0x100000 <= a < 0x2000000 else 0
                if not a:
                    break
            finals.append(a)
        addrs = fixed + [a for a in finals if a]
        v = ref.batch(addrs)
        wv, k = [], 8
        for a in finals:
            if a:
                wv.append(v[k]); k += 1
            else:
                wv.append(0)
        if v != last:
            last = v
            # vsync, now (+0x68), elapsed (+0x64), seconds per tick (+0x50)
            rows.append('%.5f,%u,%u,%u,%.7f,%x,%x,%.4f,%.4f,%.4f,%.5f%s' % (
                time.time() - t0, v[0], v[3], v[2], fl(v[1]), player, ctl, fl(v[4]), fl(v[5]), fl(v[6]), fl(v[7]),
                ''.join(',%x' % w for w in wv)))
    ref.set_pad(enable=False)
    return rows, [w[0] for w in words]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--state', type=int, default=10)
    ap.add_argument('--script', default='')
    ap.add_argument('--seconds', type=float, default=10)
    ap.add_argument('--words', default='p:0x148,c:0x454')
    ap.add_argument('--out', required=True)
    ap.add_argument('--player', default='0x12db310')
    ap.add_argument('--weapon', default='0x1126480', help='the rifle object in the savestate (port: found by a RAM sweep)')
    ap.add_argument('--settle', type=float, default=3.0)
    ap.add_argument('--renderer', type=int, default=None)
    ap.add_argument('--save', type=int, default=None, help='after the run save a savestate to this slot')
    ap.add_argument('--keep', action='store_true', help='keep PCSX2 running afterwards')
    ap.add_argument('--repeat', type=int, default=1, help='load the state and run the script this many times (out.N.csv)')
    args = ap.parse_args()
    ref = Ref(args.renderer)
    ref.start()
    try:
        time.sleep(8)  # boot to the logo; the savestate load replaces everything
        for rep in range(args.repeat):
            ref.load_state(args.state)
            time.sleep(args.settle)
            rows, cols = run(ref, args)
            out = args.out if args.repeat == 1 else args.out.replace('.csv', '.%d.csv' % rep)
            with open(out, 'w') as f:
                f.write('wall_s,vsync,now,elapsed,sec_per_tick,player,ctl,x,y,z,heading' + ''.join(',' + c for c in cols) + '\n')
                f.write('\n'.join(rows) + '\n')
            print('wrote', out, len(rows), 'rows')
        if args.save is not None:
            ref.save_state(args.save)
            time.sleep(2)
    finally:
        if not args.keep:
            ref.stop()


if __name__ == '__main__':
    main()
