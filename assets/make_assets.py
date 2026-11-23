import json, math, random, re, os
from PIL import Image, ImageDraw, ImageFilter
K='/home/claude/kit/LunarMusic-VR-Kit'
L=K+'/assets/logo'
# ---------- logo PNGs (the exact app icon, drawn by mkicon.py) ----------
icon=Image.open('/tmp/icon1024.png').convert('RGBA')
for s in (1024,512,256,128,64,32):
    icon.resize((s,s),Image.LANCZOS).save(f'{L}/lunar-logo-round-{s}.png')
icon.save(f'{L}/lunar-logo-round.ico',sizes=[(256,256),(128,128),(64,64),(48,48),(32,32),(16,16)])
sq=Image.open('/home/claude/lunar-music-iosfixed/LunarMusic/Assets.xcassets/AppIcon.appiconset/icon.png').convert('RGB')
sq.save(f'{L}/lunar-logo-square-1024.png'); sq.resize((512,512),Image.LANCZOS).save(f'{L}/lunar-logo-square-512.png')
# crescent only, transparent, for placing on any background
S=1024
moon=Image.new('L',(S,S),0); m=ImageDraw.Draw(moon); m.ellipse((230,230,S-230,S-230),fill=255); m.ellipse((400,150,S-40,S-310),fill=0)
moon=moon.filter(ImageFilter.GaussianBlur(3))
glowm=moon.filter(ImageFilter.GaussianBlur(40)).point(lambda v:int(v*0.55))
cres=Image.new('RGBA',(S,S),(150,170,255,0)); cres.putalpha(glowm)
solid=Image.new('RGBA',(S,S),(238,242,255,0)); solid.putalpha(moon)
cres=Image.alpha_composite(cres,solid); cres.save(f'{L}/lunar-crescent-1024.png'); cres.resize((256,256),Image.LANCZOS).save(f'{L}/lunar-crescent-256.png')

# ---------- SVG versions ----------
open(f'{L}/lunar-logo-round.svg','w').write('''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024">
  <!-- Lunar Music app icon: navy disc, soft periwinkle glow, pale crescent. Matches lunar-logo-round-1024.png. -->
  <defs>
    <filter id="glow" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="70"/></filter>
    <mask id="crescent"><rect width="1024" height="1024" fill="black"/>
      <circle cx="512" cy="512" r="282" fill="white"/><circle cx="692" cy="432" r="292" fill="black"/></mask>
    <clipPath id="badge"><circle cx="512" cy="512" r="488"/></clipPath>
  </defs>
  <g clip-path="url(#badge)">
    <circle cx="512" cy="512" r="488" fill="#0a0f26"/>
    <circle cx="512" cy="512" r="302" fill="#96aaff" fill-opacity="0.59" filter="url(#glow)"/>
    <rect width="1024" height="1024" fill="#eef2ff" mask="url(#crescent)"/>
  </g>
</svg>
''')
open(f'{L}/lunar-header-mark.svg','w').write('''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 40 40">
  <!-- The small glossy moon beside the "LUNAR MUSIC" title (CSS .logo), Lunar theme colors.
       A lit sphere with a bite taken out by a background-colored circle. Glow: 0 0 22px rgba(184,198,255,.65). -->
  <defs>
    <radialGradient id="sphere" cx="35%" cy="30%" r="75%">
      <stop offset="0" stop-color="#ffffff"/><stop offset="0.45" stop-color="#e3e8ff"/><stop offset="1" stop-color="#8f9bcc"/>
    </radialGradient>
    <clipPath id="c"><circle cx="20" cy="20" r="20"/></clipPath>
  </defs>
  <g clip-path="url(#c)">
    <circle cx="20" cy="20" r="20" fill="url(#sphere)"/>
    <circle cx="33" cy="13" r="20" fill="#0a0f26" fill-opacity="0.92"/>
  </g>
</svg>
''')

# ---------- theme data ----------
def h2r(h): return tuple(int(h[i:i+2],16) for i in (1,3,5))
def r2h(c): return '#%02x%02x%02x'%tuple(max(0,min(255,round(v))) for v in c)
def mix(a,b,t): a=h2r(a) if isinstance(a,str) else a; b=h2r(b) if isinstance(b,str) else b; return r2h([a[i]*t+b[i]*(1-t) for i in range(3)])
src=open('/home/claude/lunar-exe/app.html',encoding='utf-8').read()
order=[]; themes={}
for key,name,ac,a2,bg,ink in re.findall(r"(\w+):\s*\{ name: '([^']+)',\s*accent: '(#\w+)', accent2: '(#\w+)', bg: '(#\w+)', ink: '(#\w+)' \}",src):
    order.append(key)
    themes[key]={'name':name,'accent':ac,'accent2':a2,'bg':bg,'ink':ink,
      'derived':{
        'inkSoft':mix(ink,bg,.68),'inkFaint':mix(ink,bg,.44),
        'moonLight':mix(ac,'#ffffff',.40),'moonDark':mix(ac,'#4c5478',.62),
        'bgTop':mix(bg,'#000000',.72),'bgBottom':mix(bg,'#000000',.42),
        'vizBars':mix('#ffffff',ac,.6),
        'clunk':{'face':mix(ac,'#2e3033',.12),'light':mix(ac,'#383a3d',.10),'hover':mix(ac,'#3c3f43',.18),'press':mix(ac,'#232528',.14),
                 'field':mix(ac,'#1b1c1e',.05),'selection':mix(a2,'#151515',.78),'lyricMark':mix(ac,'#4d4520',.30)}}}
def floats(h): return [round(v/255,4) for v in h2r(h)]
json.dump({'about':'Lunar Music themes. accent = main glow, accent2 = second glow, bg = background, ink = text. "derived" values are what the app computes from those four (see DESIGN.md for the formulas). "linear01" gives the same colors as 0-1 floats for game engines (sRGB, not linearized).',
           'default':'lunar','order':order,'themes':themes,
           'linear01':{k:{f:floats(v[f]) for f in ('accent','accent2','bg','ink')} for k,v in themes.items()}},
          open(K+'/themes/themes.json','w'),indent=2)
css=['/* Lunar Music themes as CSS custom properties. Put the class on <html> or <body>. */']
for k in order:
    t=themes[k]; d=t['derived']
    css.append(f'''.theme-{k} {{ /* {t['name']} */
  --accent:{t['accent']}; --accent-rgb:{",".join(map(str,h2r(t['accent'])))};
  --accent2:{t['accent2']}; --accent2-rgb:{",".join(map(str,h2r(t['accent2'])))};
  --bg:{t['bg']}; --ink:{t['ink']}; --ink-soft:{d['inkSoft']}; --ink-faint:{d['inkFaint']};
  --moon-light:{d['moonLight']}; --moon-dark:{d['moonDark']};
}}''')
open(K+'/themes/themes.css','w').write('\n'.join(css)+'\n')
# swatch sheet
W,H=1200,140*len(order)//2+40
sheet=Image.new('RGB',(1200,40+140*6),'#06080f'); d=ImageDraw.Draw(sheet)
for i,k in enumerate(order):
    t=themes[k]; x=20+(i%2)*590; y=20+(i//2)*140
    bg=Image.new('RGB',(570,120),t['bg']); g=Image.new('RGBA',(570,120),(0,0,0,0)); gd=ImageDraw.Draw(g)
    gd.ellipse((330,-80,700,200),fill=h2r(t['accent2'])+(150,)); g=g.filter(ImageFilter.GaussianBlur(40))
    bg=Image.alpha_composite(bg.convert('RGBA'),g)
    md=ImageDraw.Draw(bg); md.ellipse((470,30,530,90),fill=t['derived']['moonLight'])
    sheet.paste(bg.convert('RGB'),(x,y))
    for j,(lab,col) in enumerate([('accent',t['accent']),('accent2',t['accent2']),('bg',t['bg']),('ink',t['ink'])]):
        d.rectangle((x+14+j*96,y+60,x+14+j*96+80,y+100),fill=col,outline='#888888')
        d.text((x+14+j*96,y+102),f'{lab} {col}',fill='#cfd5ea')
    d.text((x+14,y+14),t['name'].upper(),fill=t['ink'])
sheet.save(K+'/themes/theme-swatches.png')

# ---------- VR textures ----------
T=K+'/assets/textures'
random.seed(7)
# moon surface, equirectangular 2:1, for a sphere
W,H=2048,1024
base=Image.new('L',(W,H),0); px=base.load()
noise=Image.effect_noise((W//8,H//8),40).resize((W,H),Image.BICUBIC).filter(ImageFilter.GaussianBlur(6))
fine=Image.effect_noise((W,H),18).filter(ImageFilter.GaussianBlur(1))
moonL=Image.blend(noise,fine,0.35).point(lambda v:int(150+(v-128)*0.55))
dm=ImageDraw.Draw(moonL)
for _ in range(260):
    r=int(abs(random.gauss(0,1))*28)+4; cx=random.randint(0,W); cy=int(H/2+random.gauss(0,1)*H/4)
    sx=1/max(0.25,math.cos((cy/H-0.5)*math.pi))     # wider near the poles, so they look round on the sphere
    box=(cx-r*sx,cy-r,cx+r*sx,cy+r)
    dm.ellipse(box,fill=max(60,150-int(r*1.1)))
    dm.arc((box[0]+1,box[1]+1,box[2]-1,box[3]-1),200,340,fill=210,width=max(1,r//6))
for _ in range(9):   # dark "seas"
    r=random.randint(90,220); cx=random.randint(0,W); cy=random.randint(H//4,3*H//4)
    sea=Image.new('L',(W,H),0); ImageDraw.Draw(sea).ellipse((cx-r*1.3,cy-r,cx+r*1.3,cy+r),fill=70)
    moonL=Image.composite(Image.new('L',(W,H),95),moonL,sea.filter(ImageFilter.GaussianBlur(40)))
moonL=moonL.filter(ImageFilter.GaussianBlur(0.8))
rgb=Image.merge('RGB',[moonL.point(lambda v:int(v*0.93)),moonL.point(lambda v:int(v*0.95)),moonL])
rgb.save(T+'/moon-surface-equirect-2048x1024.png')
# glowing moon disc as the app draws it (Lunar theme), on transparent background
S=1024; disc=Image.new('RGBA',(S,S),(0,0,0,0))
glow=Image.new('RGBA',(S,S),(0,0,0,0)); ImageDraw.Draw(glow).ellipse((150,150,S-150,S-150),fill=(184,198,255,170)); glow=glow.filter(ImageFilter.GaussianBlur(70))
disc=Image.alpha_composite(disc,glow)
body=Image.new('RGBA',(S,S),(0,0,0,0)); bp=body.load(); c=S/2; R=330
for y in range(S):
    for x in range(S):
        dx,dy=x-c,y-c
        if dx*dx+dy*dy<=R*R:
            gx,gy=(x-(c-0.28*R*2*0.5))/(R*2),(y-(c-0.4*R*2*0.5))/(R*2)   # light from upper left (36% 30%)
            t=min(1,math.hypot(x-(c-R+0.72*R),y-(c-R+0.6*R))/(R*1.55))
            col=[255,255,255] if t<0 else None
            a=(255,255,255); b=(227,232,255); d_=(143,155,204)
            if t<0.42: k=t/0.42; col=[a[i]+(b[i]-a[i])*k for i in range(3)]
            else: k=(t-0.42)/0.58; col=[b[i]+(d_[i]-b[i])*k for i in range(3)]
            bp[x,y]=tuple(int(v) for v in col)+(255,)
cr=ImageDraw.Draw(body)
for fx,fy,fr,al in [(0.30,0.64,0.08,115),(0.62,0.36,0.12,97),(0.54,0.76,0.06,82)]:
    ov=Image.new('RGBA',(S,S),(0,0,0,0)); ImageDraw.Draw(ov).ellipse(((c-R)+fx*2*R-fr*2*R,(c-R)+fy*2*R-fr*2*R,(c-R)+fx*2*R+fr*2*R,(c-R)+fy*2*R+fr*2*R),fill=(110,124,175,al))
    mask=Image.new('L',(S,S),0); ImageDraw.Draw(mask).ellipse((c-R,c-R,c+R,c+R),fill=255)
    ov.putalpha(Image.composite(ov.getchannel('A'),Image.new('L',(S,S),0),mask)); body=Image.alpha_composite(body,ov)
disc=Image.alpha_composite(disc,body); disc.save(T+'/moon-disc-glow-1024.png')
# starfield, equirectangular, transparent so a theme-tinted sky can show through
W,H=4096,2048; st=Image.new('RGBA',(W,H),(0,0,0,0)); sd=ImageDraw.Draw(st)
for i in range(2600):
    x,y=random.random()*W,random.random()*H; r=random.random()*1.4+0.4; a=int((random.random()*0.6+0.25)*255)
    col=(201,212,255,a) if i%7==0 else (255,255,255,a)
    sd.ellipse((x-r,y-r,x+r,y+r),fill=col)
st.save(T+'/starfield-equirect-4096x2048.png')
# sky gradient per theme (vertical), to put behind the stars
for k in order:
    t=themes[k]; g=Image.new('RGB',(16,512)); gp=g.load()
    top,mid,bot=h2r(t['derived']['bgTop']),h2r(t['bg']),h2r(t['derived']['bgBottom'])
    for y in range(512):
        f=y/511
        col=[top[i]+(mid[i]-top[i])*(f/0.45) for i in range(3)] if f<0.45 else [mid[i]+(bot[i]-mid[i])*((f-0.45)/0.55) for i in range(3)]
        for x in range(16): gp[x,y]=tuple(int(v) for v in col)
    g.save(f'{T}/sky-gradient-{k}.png')

# ---------- UI icons ----------
I=K+'/assets/icons'
paths={
 'play':'M8 4.5v15l12-7.5z','pause':'M6.5 4.5h4v15h-4zM13.5 4.5h4v15h-4z',
 'previous':'M6 5h2.4v14H6zM20 5v14L9.5 12z','next':'M15.6 5H18v14h-2.4zM4 5l10.5 7L4 19z',
 'shuffle':'M17 3l4 4-4 4V8h-2.2l-2.6 3.3-1.3-1.6L13.8 6H17V3zM3 6h4.2l8.6 11H17v-3l4 4-4 4v-3h-2.2L6.2 8H3V6zm0 11h3.2l2.5-3.2 1.3 1.6L7.2 19H3v-2z',
 'repeat':'M7 7h10v3l4-4-4-4v3H5v6h2V7zm10 10H7v-3l-4 4 4 4v-3h12v-6h-2v4z',
 'volume':'M4 9v6h4l5 4V5L8 9H4zm11.5 3a4 4 0 0 0-2-3.5v7a4 4 0 0 0 2-3.5zM13.5 3.3v2.1a7 7 0 0 1 0 13.2v2.1a9 9 0 0 0 0-17.4z',
 'heart':'M12 20.5 4.6 13.4a4.9 4.9 0 0 1 0-7 4.8 4.8 0 0 1 6.9 0l.5.5.5-.5a4.8 4.8 0 0 1 6.9 0 4.9 4.9 0 0 1 0 7z',
 'lyrics':'M4 5h16v2H4zM4 9.7h11v2H4zM4 14.4h16v2H4zM4 19h9v2H4z',
 'plus':'M11 4h2v7h7v2h-7v7h-2v-7H4v-2h7z','close':'M6.4 5 12 10.6 17.6 5 19 6.4 13.4 12 19 17.6 17.6 19 12 13.4 6.4 19 5 17.6 10.6 12 5 6.4z',
 'chevron-up':'M12 6.5 19.4 14 18 15.4l-6-6-6 6L4.6 14z','chevron-down':'M12 17.5 4.6 10 6 8.6l6 6 6-6L19.4 10z','back':'M14.6 4.6 16 6l-6 6 6 6-1.4 1.4L7.2 12z',
 'clock':'M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zm0 2a7 7 0 1 1 0 14 7 7 0 0 1 0-14zm-1 2.5v5l4.2 2.5 1-1.7-3.2-1.9V7.5z',
 'star':'m12 3 2.7 5.6 6.1.8-4.5 4.2 1.2 6L12 16.7 6.5 19.6l1.2-6L3.2 9.4l6.1-.8z',
 'refresh':'M12 5V2L7.5 6 12 10V7a5 5 0 1 1-4.9 6H5.07A7 7 0 1 0 12 5z',
 'settings':'M19.4 13a7.6 7.6 0 0 0 0-2l2-1.6-2-3.4-2.4 1a7.4 7.4 0 0 0-1.7-1L15 3.5h-4l-.4 2.5a7.4 7.4 0 0 0-1.7 1l-2.4-1-2 3.4 2 1.6a7.6 7.6 0 0 0 0 2l-2 1.6 2 3.4 2.4-1a7.4 7.4 0 0 0 1.7 1l.4 2.5h4l.4-2.5a7.4 7.4 0 0 0 1.7-1l2.4 1 2-3.4-2-1.6zM12 15.2a3.2 3.2 0 1 1 0-6.4 3.2 3.2 0 0 1 0 6.4z'}
for n,pth in paths.items():
    open(f'{I}/{n}.svg','w').write(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><path fill="#eef1fb" d="{pth}"/></svg>\n')
open(f'{I}/search.svg','w').write('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="#eef1fb" stroke-width="2.2" stroke-linecap="round"><circle cx="11" cy="11" r="7"/><path d="M20 20l-3.6-3.6"/></svg>\n')
print('ok', len(order), 'themes', len(paths)+1, 'icons')
