import sys,glob,re
from PIL import Image, ImageDraw
d,t0,t1=sys.argv[1],int(sys.argv[2]),int(sys.argv[3])
crop=tuple(map(int,sys.argv[4].split(','))) if len(sys.argv)>4 else None
fs=sorted(glob.glob(d+'/gs_*.png'),key=lambda f:int(re.search(r'_t(\d+)s',f).group(1)))
sel=[f for f in fs if t0<=int(re.search(r'_t(\d+)s',f).group(1))<=t1]
W,H=340,300
m=Image.new('RGB',(W*5,H*((len(sel)+4)//5)))
for i,f in enumerate(sel):
    im=Image.open(f).convert('RGB')
    if crop: im=im.crop(crop)
    im=im.resize((W,H-20)); x,y=(i%5)*W,(i//5)*H
    m.paste(im,(x,y+20)); ImageDraw.Draw(m).text((x+5,y+3),f.split('gs_')[1],fill=(255,255,0))
m.save(d+'/montage.png'); print(len(sel))
