import numpy as np
from PIL import Image
rng=np.random.default_rng(11); W,H=2048,1024
def fbm(beta):
    n=rng.standard_normal((H,W)); F=np.fft.fft2(n)
    ky=np.fft.fftfreq(H)[:,None]; kx=np.fft.fftfreq(W)[None,:]; k=np.sqrt(kx**2+ky**2); k[0,0]=1
    h=np.real(np.fft.ifft2(F/k**beta)); return (h-h.mean())/h.std()
h=fbm(1.55)*0.5
lat=(np.arange(H)[:,None]+0.5)/H*np.pi-np.pi/2; yy=np.arange(H)[:,None]; xx=np.arange(W)[None,:]
for i in range(700):
    r=float(np.clip(rng.pareto(1.6)*6+3,3,110)); cx=rng.uniform(0,W); cy=rng.uniform(H*0.08,H*0.92)
    sx=1/max(0.2,np.cos((cy/H-0.5)*np.pi))
    y0,y1=int(max(0,cy-2.2*r)),int(min(H,cy+2.2*r)); xr=int(2.2*r*sx)+2
    xs=np.arange(int(cx)-xr,int(cx)+xr); dx=(xs-cx)/sx; dy=(np.arange(y0,y1)[:,None]-cy)
    d=np.sqrt(dx[None,:]**2+dy**2)/r
    prof=np.where(d<1,(d**2-1)*0.9,0)+0.35*np.exp(-((d-1)/0.18)**2)     # bowl plus raised rim
    h[y0:y1][:,xs%W]+=prof*(r/40)**0.8
# dark plains ("seas")
seas=fbm(2.2); seas=np.clip((seas-0.6)*1.4,0,1)
gy,gx=np.gradient(h); L=np.array([-0.6,-0.55,0.58]); L/=np.linalg.norm(L)
nz=np.ones_like(h)*0.32; nrm=np.sqrt(gx**2+gy**2+nz**2)
shade=np.clip((-gx*L[0]-gy*L[1]+nz*L[2])/nrm,0,1)
alb=0.74-0.28*seas+0.05*fbm(0.9)
v=np.clip((0.25+0.85*shade)*alb,0,1)
img=np.stack([v*0.93,v*0.95,v*1.0],-1)*255
Image.fromarray(img.astype(np.uint8)).save('/home/claude/kit/LunarMusic-VR-Kit/assets/textures/moon-surface-equirect-2048x1024.png')
# matching height map (for displacement / normal maps)
hm=(h-h.min())/(h.max()-h.min()); Image.fromarray((hm*255).astype(np.uint8)).save('/home/claude/kit/LunarMusic-VR-Kit/assets/textures/moon-height-equirect-2048x1024.png')
