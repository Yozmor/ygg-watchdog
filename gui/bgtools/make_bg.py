"""Фон «древо Иггдрасиль» для окон ygg-watchdog и панели Yggdrasil.
Запуск:  python make_bg.py            -> bg_gray.png, bg_green.png, bg_ice.png (1560x960)
         python make_bg.py 2340 1440  -> другой размер (во столько же раз крупнее геометрия)
Нужны: Pillow, numpy, scipy и рядом лежащий bigtree.py
Параметры дерева: seed (форма), TRUNK_X (положение ствола, доля ширины), DIM (яркость)."""
import sys, numpy as np
from PIL import Image
import bigtree
W=int(sys.argv[1]) if len(sys.argv)>1 else 1560
H=int(sys.argv[2]) if len(sys.argv)>2 else 960
K=H/640.0                    # эталон — окно 1040x640
SEED=33                      # форма дерева (одобренная)
TRUNK_X=0.48                 # положение ствола по ширине
DIM_ICE,DIM_GREEN=0.30,0.36  # яркость дальней кроны (меньше = тусклее)
bigtree.SIDE=0.0
def make(green,fg_on=True):
    if green: P=[(30,100,170)]; cl=((6,22,16),(60,170,120),(80,255,170)); bgc=(2,9,7); vein=(90,255,170)
    else: P=[(30,100,170),(45,140,215),(70,175,240),(120,215,255),(20,70,130),(180,240,255)]; cl=((10,26,48),(90,160,225),(90,190,255)); bgc=(3,7,12); vein=(120,220,255)
    tx=int(W*TRUNK_X)
    far=bigtree.render(W,H,SEED,cl[0],cl[1],cl[2],P,tx,int(210*K),1.0,not green,1,0.30,vein=vein,by=int(H+(240 if green else 260)*K),tlen=0.62,
        forced=((-1.2,0.7),(-0.6,0.72),(0.0,0.75),(0.6,0.72),(1.2,0.7)),fl=(1.2,1.5),clen=(1.0,1.3),nvein=(0,1),kids=(1,3),leaf_min=3,leaf_n=26,nroots=0,
        leaf_spread=(11*K,9*K),leaf_r=(3*K,7*K))
    a=np.array(far.split()[3]).astype(float)/255
    from PIL import Image as I
    far.putalpha(I.fromarray((a*(DIM_GREEN if green else DIM_ICE)*255).astype("uint8")))
    base=I.new("RGBA",(W,H),bgc+(255,)); base.alpha_composite(far)
    if not green and fg_on:
        fg=bigtree.render(W,H,8,(8,16,28),(110,165,225),(80,170,255),[(10,30,56)],0,0,1.0,True,1,0.7,vein=vein,start=(-120*K,H-50*K,-0.015,1500*K,150*K,110*K),leaf_min=2,leaf_n=10,nroots=0,
            leaf_spread=(11*K,9*K),leaf_r=(3*K,7*K))
        base.alpha_composite(fg)
    return base.convert("RGB")
ice=make(False); green=make(True); ice_far=make(False,False)
ice.save("bg_ice.png"); green.save("bg_green.png")
g=np.array(ice_far.convert("L")).astype(float); g=np.clip(g/np.percentile(g,99.7)*190,0,255)   # серый: только дальнее древо, без переднего плана
Image.fromarray(g.astype("uint8")).save("bg_gray.png")
print("готово", W,H)
