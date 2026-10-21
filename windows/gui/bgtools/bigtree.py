from PIL import Image, ImageDraw, ImageFilter
import numpy as np, math, random
SS=2
LIMBS=[]
SIDE=0.2
def norm(v):
    l=math.sqrt(sum(x*x for x in v)) or 1; return tuple(x/l for x in v)
L1=norm((-0.55,-0.5,0.7)); L2=norm((0.9,-0.2,0.35))
def shade(u,nx,ny,rnd,base,lit,rim):
    nz=math.sqrt(max(0,1-u*u)); n=(nx*u,ny*u,nz)
    a=max(0,sum(n[i]*L1[i] for i in range(3))); r=max(0,sum(n[i]*L2[i] for i in range(3)))**2
    a=0.04+0.96*(a**1.5); a=a*(1+rnd.uniform(-0.22,0.22))
    col=[base[k]+(lit[k]-base[k])*min(1,a)+rim[k]*r*0.55 for k in range(3)]
    return tuple(int(max(0,min(255,c))) for c in col)
def render(W,H,seed,base,lit,rim,leaf_pal,trunk_x,trunk_w,scale=1.0,leaves=True,limit_depth=8,dens=1.0,vein=(90,200,255),by=None,tlen=0.42,forced=((-0.85,0.55),(0.05,0.62),(0.8,0.5)),fl=(0.85,1.1),leaf_min=4,leaf_n=14,nroots=7,start=None,kids=None,clen=(0.4,0.62),nvein=(2,4),leaf_spread=(11,9),leaf_r=(3,7)):
    by=H-10 if by is None else by
    rnd=random.Random(seed); Ws,Hs=W*SS,H*SS
    img=Image.new("RGBA",(Ws,Hs),(0,0,0,0)); d=ImageDraw.Draw(img)
    leafpts=[]; gl=Image.new('RGBA',(Ws,Hs),(0,0,0,0)); gd=ImageDraw.Draw(gl)
    def ring(p,ang,w,N=None):
        N=N or max(3,min(9,3+int(w/16)))
        nx,ny=-math.sin(ang),math.cos(ang); out=[]
        for k in range(N):
            u=-1+2*k/(N-1); j=rnd.uniform(-0.10,0.10) if 0<k<N-1 else 0
            out.append(((p[0]+nx*w/2*(u+j))*SS,(p[1]+ny*w/2*(u+j))*SS,u))
        return out,(nx,ny)
    def limb(p,ang,length,w0,w1,depth,bias=0.05):
        pts=[p]; angs=[ang]; a=ang; x,y=p
        step=max(w0*0.22,5)
        n=max(3,int(length/step)); ws=[]
        for i in range(n+1): ws.append(w0+(w1-w0)*(i/n)**0.9)
        for i in range(n):
            a+=rnd.gauss(0,0.10 if depth>1 else 0.16)
            a+=(-math.pi/2-a)*bias if True else 0          # тянется вверх
            x+=math.cos(a)*step; y+=math.sin(a)*step; pts.append((x,y)); angs.append(a)
        LIMBS.append((list(pts),list(angs),list(ws),depth))
        Nl=max(3,min(9,3+int(w0/16)))
        rings=[ring(pts[i],angs[i]+0,ws[i]*(1+rnd.uniform(-0.05,0.05)),Nl) for i in range(n+1)]
        for i in range(n):
            (r0,(nx,ny)),(r1,_)=rings[i],rings[i+1]
            for k in range(len(r0)-1):
                u=(r0[k][2]+r0[k+1][2])/2; col=shade(u,nx,ny,rnd,base,lit,rim)
                d.polygon([r0[k][:2],r0[k+1][:2],r1[k+1][:2],r1[k][:2]],fill=col+(255,))
            if ws[i]>14 and rnd.random()<0.5:       # трещины коры
                k=rnd.randrange(len(r0)-1); a0=r0[k][:2]; b0=r1[k][:2]
                d.line([a0,b0],fill=(10,16,26,150),width=SS)
        if w0>24:
            for _v in range(nvein[0] if w0<40 else nvein[1]):
                u0=rnd.uniform(-0.65,0.65); on=False
                for i in range(n):
                    if rnd.random()<0.12: on=not on
                    if on or i==0:
                        (r0v,_),(r1v,_)=rings[i],rings[i+1]; wa=ws[i]/2; wb=ws[i+1]/2
                        ax=(pts[i][0]-math.sin(angs[i])*wa*u0)*SS; ay=(pts[i][1]+math.cos(angs[i])*wa*u0)*SS
                        bx=(pts[i+1][0]-math.sin(angs[i+1])*wb*u0)*SS; by=(pts[i+1][1]+math.cos(angs[i+1])*wb*u0)*SS
                        gd.line([(ax,ay),(bx,by)],fill=vein+(190,),width=max(1,int(SS*(1+w0/90))))
        # потомки
        if depth==0:
            for da,k in forced:
                cw=w1*k*1.05; limb(pts[-3],angs[-3]+da,H*rnd.uniform(*fl),cw*1.1,cw*0.55,1,0.05)
            for i in range(int(n*0.5),n):
                if rnd.random()<SIDE:
                    side=rnd.choice((-1,1)); cw=ws[i]*0.34; limb(pts[i],angs[i]+side*rnd.uniform(0.7,1.1),H*0.6,cw,cw*0.45,2,0.05)
        elif depth==1 and kids and bias>=0 and start is None:
            for _k in range(rnd.randint(*kids)):
                i=rnd.randint(int(n*0.3),int(n*0.8)); side=rnd.choice((-1,1)); cw=ws[i]*rnd.uniform(0.45,0.6)
                limb(pts[i],angs[i]+side*rnd.uniform(0.45,0.8),length*rnd.uniform(0.8,1.1),cw*1.1,cw*0.6,2,0.04)
        elif depth<limit_depth and w1>1.2 and bias>=0:
            _lim=1
            for i in range(2,n+1):
                if rnd.random()<min(0.9,dens*step/(70+depth*8)):
                    side=rnd.choice((-1,1)); da=side*rnd.uniform(0.4,0.95); cw=ws[i]*rnd.uniform(0.55,0.75)
                    if cw<9: continue
                    limb(pts[i],angs[i]+da,length*rnd.uniform(*clen),cw,max(1.0,cw*0.35),depth+1,0.06+0.015*depth)
        if (w1<=2.4 or depth>=limit_depth) and leaves and bias>=0 and depth>=leaf_min: leafpts.append(pts[-1])
    for k in range(0 if start else nroots):
        sd=-1 if k%2==0 else 1; a0=-math.pi/2+sd*rnd.uniform(1.0,1.45)
        limb((trunk_x+sd*trunk_w*rnd.uniform(0.0,0.3),by-8),a0,trunk_w*rnd.uniform(1.3,2.2),trunk_w*rnd.uniform(0.22,0.34),trunk_w*0.05,3,-0.03)
    if start: limb((start[0],start[1]),start[2],start[3],start[4],start[5],1,0.0)
    else: limb((trunk_x,by),-math.pi/2+0.04,H*tlen*scale,trunk_w*1.3,trunk_w*0.8,0,0.03)
    if leaves:
        for (x,y) in leafpts:
            for _ in range(int(leaf_n*dens)+6):
                cx=x+rnd.gauss(0,leaf_spread[0]); cy=y+rnd.gauss(0,leaf_spread[1]); r=rnd.uniform(*leaf_r)
                a=rnd.uniform(0,6.28); n_=rnd.choice((3,4,6))
                pts=[((cx+r*math.cos(a+k*6.283/n_))*SS,(cy+r*math.sin(a+k*6.283/n_)*1.15)*SS) for k in range(n_)]
                c=rnd.choice(leaf_pal); d.polygon(pts,fill=c+(rnd.randint(190,250),))
                if rnd.random()<0.06: d.line(pts+[pts[0]],fill=(190,235,255,150),width=SS)
    bl=gl.filter(ImageFilter.GaussianBlur(5)); img.alpha_composite(bl); img.alpha_composite(gl)
    return img.resize((W,H),Image.LANCZOS)
if __name__=="__main__":
    W,H=1120,700
    far=render(W,H,11,(6,12,22),(45,80,125),(50,100,160),[(12,36,66),(20,56,100)],760,70,0.9,True,7,0.8).filter(ImageFilter.GaussianBlur(1.6))
    near=render(W,H,4,(8,16,28),(105,160,215),(80,170,255),[(12,40,76),(18,60,110),(26,86,146),(40,120,190),(18,60,110),(110,200,255)],420,230,1.35,True,9,1.0)
    bg=Image.new("RGBA",(W,H),(3,7,12,255)); bg.alpha_composite(far); bg.alpha_composite(near)
    bg.convert("RGB").save("bigtree.png")
