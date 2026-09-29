# Parse PS2X_VIF1_DUMP buffers: VIFcode histogram, MSCAL targets, UNPACK formats, per buffer.
# usage: vifparse.py <dumpdir> [first] [count] [-v]
import struct, sys, os, collections, glob

NAMES = {0: 'NOP', 1: 'STCYCL', 2: 'OFFSET', 3: 'BASE', 4: 'ITOP', 5: 'STMOD', 6: 'MSKPATH3', 7: 'MARK',
         0x10: 'FLUSHE', 0x11: 'FLUSH', 0x13: 'FLUSHA', 0x14: 'MSCAL', 0x15: 'MSCALF', 0x17: 'MSCNT',
         0x20: 'STMASK', 0x30: 'STROW', 0x31: 'STCOL', 0x4A: 'MPG', 0x50: 'DIRECT', 0x51: 'DIRECTHL'}
UPK = ['S32', 'S16', 'S8', 'S?', 'V2_32', 'V2_16', 'V2_8', 'V2?', 'V3_32', 'V3_16', 'V3_8', 'V3?', 'V4_32', 'V4_16',
       'V4_8', 'V4_5']
GSZ = [4, 2, 1, 0, 8, 4, 2, 0, 12, 6, 3, 0, 16, 8, 4, 2]


def parse(data, verbose=False, out=None):
    pos = 0
    cycle = 0x0101
    ops = collections.Counter()
    mscal = collections.Counter()
    unpack = collections.Counter()
    seq = []
    while pos + 4 <= len(data):
        cmd, = struct.unpack_from('<I', data, pos)
        pos += 4
        op = (cmd >> 24) & 0x7F
        imm = cmd & 0xFFFF
        num = (cmd >> 16) & 0xFF
        if op >= 0x60:
            vnvl = op & 0xF
            n = num or 256
            cl = cycle & 0xFF
            wl = (cycle >> 8) & 0xFF or 256
            if wl <= cl:
                srcn = n
            else:
                srcn = cl * (n // wl) + min(n % wl, cl)
            size = (srcn * GSZ[vnvl] + 3) & ~3
            key = '%s%s%s' % (UPK[vnvl], 'm' if op & 0x10 else '', 'u' if imm & 0x4000 else '')
            unpack[key] += 1
            ops['UNPACK'] += 1
            if verbose:
                seq.append('%06x UNPACK %s num=%d addr=%03x%s cyc=%04x' % (pos - 4, key, n, imm & 0x3FF,
                                                                           ' +TOPS' if imm & 0x8000 else '', cycle))
            pos += size
            continue
        name = NAMES.get(op, 'UNK%02x' % op)
        ops[name] += 1
        if verbose and name not in ('NOP',):
            seq.append('%06x %s imm=%04x num=%d' % (pos - 4, name, imm, num))
        if op == 1:
            cycle = imm
        elif op in (0x14, 0x15, 0x17):
            mscal[(name, imm * 8 if op != 0x17 else -1)] += 1
        elif op == 0x20:
            pos += 4
        elif op in (0x30, 0x31):
            pos += 16
        elif op == 0x4A:
            pos += (num or 256) * 8
        elif op in (0x50, 0x51):
            pos += (imm or 65536) * 16
        elif name.startswith('UNK'):
            if verbose:
                seq.append('   ^ unknown, stopping')
            break
    return ops, mscal, unpack, seq


if __name__ == '__main__':
    d = sys.argv[1]
    files = sorted(glob.glob(os.path.join(d, 'vif1_*.bin')))
    first = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    count = int(sys.argv[3]) if len(sys.argv) > 3 else len(files)
    verbose = '-v' in sys.argv
    tot_ops = collections.Counter(); tot_ms = collections.Counter(); tot_up = collections.Counter()
    for f in files[first:first + count]:
        data = open(f, 'rb').read()
        ops, ms, up, seq = parse(data, verbose)
        tot_ops += ops; tot_ms += ms; tot_up += up
        print('%s %7d bytes  mscal=%d unpack=%d direct=%d mpg=%d' % (os.path.basename(f), len(data),
              sum(ms.values()), ops['UNPACK'], ops['DIRECT'], ops['MPG']))
        if verbose:
            print('\n'.join(seq))
    print('ops', dict(tot_ops))
    print('mscal', sorted(((k[0], hex(k[1]) if k[1] >= 0 else '-'), v) for k, v in tot_ms.items()))
    print('unpack', dict(tot_up))
