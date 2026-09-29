# Usage: chain2buf.py <eeMemory.bin> <Scratchpad.bin> <start tadr hex> <out.bin>. Walks a VIF1 DMA chain in a RAM image and
# writes the byte stream VIF1 receives (TTE VIFcodes + data), the format of PS2X_VIF1_DUMP files, for vifparse.py.
# Killzone keeps the current frame chain start in the word at 0x559040.
import struct,sys
m=open(sys.argv[1],'rb').read(); spr=open(sys.argv[2],'rb').read()
start=int(sys.argv[3],16); out=sys.argv[4]
def rd(a,n):
    if a & 0x80000000: return spr[a&0x3ff0:(a&0x3ff0)+n]
    a&=0x1ffffff; return m[a:a+n]
t=start; asr=[]; buf=bytearray(); n=0
while n<20000:
    lo,=struct.unpack('<Q',rd(t,8)); qwc=lo&0xffff; id_=(lo>>28)&7; addr=lo>>32; n+=1
    buf+=rd(t+8,8)
    if id_ in (0,3,4): data=addr; nt=t+16
    elif id_==1: data=t+16; nt=t+16+qwc*16
    elif id_==2: data=t+16; nt=addr
    elif id_==5: data=t+16; asr.append(t+16+qwc*16); nt=addr
    elif id_==6: data=t+16; nt=asr.pop() if asr else None
    else: data=t+16; nt=None
    buf+=rd(data,qwc*16)
    if id_ in (0,7) or nt is None: break
    t=nt
open(out,'wb').write(buf); print(n,'tags',len(buf),'bytes')
