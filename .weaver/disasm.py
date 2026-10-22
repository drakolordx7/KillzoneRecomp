# usage: python .weaver/disasm.py 0x16e420 [ninstr=60]   -- MIPS disassembly of SCUS_974.02 (capstone, R5900 MMI/COP2 show as data)
import sys, struct, capstone
d = open(r'D:\KillzoneRecomp\game\SCUS_974.02', 'rb').read()
OFF, VA = 0x1000, 0x140000
a = int(sys.argv[1], 0); n = int(sys.argv[2]) if len(sys.argv) > 2 else 60
md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS64 | capstone.CS_MODE_LITTLE_ENDIAN)
for i in range(n):
    pc = a + 4 * i; w = d[OFF + pc - VA: OFF + pc - VA + 4]
    ins = list(md.disasm(w, pc))
    print('%08x  %s' % (pc, ('%s %s' % (ins[0].mnemonic, ins[0].op_str)) if ins else '.word 0x%08x' % struct.unpack('<I', w)[0]))
