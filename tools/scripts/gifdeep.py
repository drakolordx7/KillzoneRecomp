# GIF trace analysis with GS register state: vertex kicks per (prim, FBP, ZTST, fog) and XY/Z ranges.
# usage: gifdeep.py trace.bin [first_vsync] [n_vsyncs]
import struct, sys, collections
f = open(sys.argv[1], 'rb').read()
v0 = int(sys.argv[2]) if len(sys.argv) > 2 else 0
nv = int(sys.argv[3]) if len(sys.argv) > 3 else 1 << 30
pos = 0; vs = 0
prim = 0; frame = [0, 0]; zbuf = [0, 0]; test = [0, 0]; xyoff = [0, 0]
stats = collections.Counter(); rng = {}
ADDR_NAMES = {0: 'PRIM', 1: 'RGBAQ', 2: 'ST', 3: 'UV', 4: 'XYZF2', 5: 'XYZ2', 6: 'TEX0_1', 0xc: 'XYZF3', 0xd: 'XYZ3',
              0x18: 'XYOFFSET_1', 0x47: 'TEST_1', 0x4c: 'FRAME_1', 0x4e: 'ZBUF_1'}


def vert(x, y, z, kick):
    ctx = (prim >> 9) & 1
    key = (prim & 7, (frame[ctx] & 0x1ff) * 32, (test[ctx] >> 17) & 3 if test[ctx] >> 16 & 1 else -1,
           (prim >> 5) & 1, (prim >> 4) & 1)  # type, fbp*32 (word addr/64? keep raw*32), ztst, fog, tme
    stats[key] += 1
    r = rng.setdefault(key, [1e9, -1e9, 1e9, -1e9, 1 << 32, 0])
    xo, yo = (xyoff[ctx] & 0xffff) / 16, ((xyoff[ctx] >> 32) & 0xffff) / 16
    X, Y = x / 16 - xo, y / 16 - yo
    r[0] = min(r[0], X); r[1] = max(r[1], X); r[2] = min(r[2], Y); r[3] = max(r[3], Y); r[4] = min(r[4], z); r[5] = max(r[5], z)


def reg(addr, val):
    global prim
    if addr == 0: prim = val & 0x7ff
    elif addr in (0x4c, 0x4d): frame[addr - 0x4c] = val
    elif addr in (0x4e, 0x4f): zbuf[addr - 0x4e] = val
    elif addr in (0x47, 0x48): test[addr - 0x47] = val
    elif addr in (0x18, 0x19): xyoff[addr - 0x18] = val


while pos + 12 <= len(f):
    typ, arg, size = struct.unpack_from('<III', f, pos); pos += 12
    data = f[pos:pos + size]; pos += size
    if typ == 2:
        vs += 1
        continue
    if vs < v0 or vs >= v0 + nv:
        continue
    q = 0; n = len(data) // 16
    while q < n:
        lo, hi = struct.unpack_from('<QQ', data, q * 16); q += 1
        nloop = lo & 0x7fff; pre = (lo >> 46) & 1; flg = (lo >> 58) & 3; nreg = (lo >> 60) & 0xf or 16
        regs = [(hi >> (4 * i)) & 0xf for i in range(nreg)]
        if pre: prim = (lo >> 47) & 0x7ff
        if flg == 0:
            for l in range(nloop):
                for r in regs:
                    if q >= n: break
                    a, b = struct.unpack_from('<QQ', data, q * 16); q += 1
                    if r == 0xe: reg(b & 0xff, a)
                    elif r in (4, 0xc):
                        vert(a & 0xffff, (a >> 32) & 0xffff, (b >> 4) & 0xffffff, r == 4)
                    elif r in (5, 0xd):
                        vert(a & 0xffff, (a >> 32) & 0xffff, b & 0xffffffff, r == 5)
                    elif r == 0: prim = a & 0x7ff
        elif flg == 1:
            cnt = nloop * nreg
            for i in range(cnt):
                if q * 16 + i * 8 + 8 > len(data): break
                w = struct.unpack_from('<Q', data, q * 16 + i * 8)[0]
                r = regs[i % nreg]
                if r in (4, 0xc): vert(w & 0xffff, (w >> 16) & 0xffff, (w >> 32) & 0xffffff, True)
                elif r in (5, 0xd): vert(w & 0xffff, (w >> 16) & 0xffff, w >> 32, True)
                elif r == 0: prim = w & 0x7ff
            q += (cnt + 1) // 2
        else:
            q += nloop
print('vsyncs', vs)
for k, v in sorted(stats.items(), key=lambda kv: -kv[1]):
    r = rng[k]
    print('prim%d fbp=%04x ztst=%2d fog=%d tme=%d  verts=%7d  x[%7.1f..%7.1f] y[%7.1f..%7.1f] z[%08x..%08x]' % (
        k[0], k[1], k[2], k[3], k[4], v, r[0], r[1], r[2], r[3], r[4], r[5]))
