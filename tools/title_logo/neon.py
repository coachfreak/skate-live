import sys, math
sys.path.insert(0,'.')
from dxt import png
W,H=int(sys.argv[3]) if len(sys.argv)>3 else 384, int(sys.argv[4]) if len(sys.argv)>4 else 80
def load_mask(path,bpr):
    raw=open(path,'rb').read()
    return [[raw[y*bpr+x*4+3]/255 for x in range(W)] for y in range(H)]
def edt(inside):
    # chamfer distance to the nearest outside pixel (for inside pixels), 2-pass
    INF=1e9; d=[[0 if not inside[y][x] else INF for x in range(W)] for y in range(H)]
    a,b=1.0,1.4142
    for y in range(H):
        for x in range(W):
            if d[y][x]:
                v=d[y][x]
                if x>0: v=min(v,d[y][x-1]+a)
                if y>0:
                    v=min(v,d[y-1][x]+a)
                    if x>0: v=min(v,d[y-1][x-1]+b)
                    if x<W-1: v=min(v,d[y-1][x+1]+b)
                d[y][x]=v
    for y in range(H-1,-1,-1):
        for x in range(W-1,-1,-1):
            if d[y][x]:
                v=d[y][x]
                if x<W-1: v=min(v,d[y][x+1]+a)
                if y<H-1:
                    v=min(v,d[y+1][x]+a)
                    if x<W-1: v=min(v,d[y+1][x+1]+b)
                    if x>0: v=min(v,d[y+1][x-1]+b)
                d[y][x]=v
    return d
def blur(img,r):
    out=[row[:] for row in img]
    for _ in range(3):
        tmp=[[0]*W for _ in range(H)]
        for y in range(H):
            s=0; row=out[y]
            acc=[0]*(W+1)
            for x in range(W): acc[x+1]=acc[x]+row[x]
            for x in range(W):
                x0=max(0,x-r); x1=min(W,x+r+1); tmp[y][x]=(acc[x1]-acc[x0])/(x1-x0)
        for x in range(W):
            acc=[0]*(H+1)
            for y in range(H): acc[y+1]=acc[y]+tmp[y][x]
            for y in range(H):
                y0=max(0,y-r); y1=min(H,y+r+1); out[y][x]=(acc[y1]-acc[y0])/(y1-y0)
    return out
def premul(c,a):
    # like the stock letters: color fades to black with alpha (the movie blends them additively)
    k=min(1.0,a/255*1.6)
    return bytes((int(c[0]*k),int(c[1]*k),int(c[2]*k),a))
def style(mask):
    inside=[[mask[y][x]>0.5 for x in range(W)] for y in range(H)]
    d=edt(inside)
    glow=blur(mask,2)
    px=bytearray(W*H*4)
    edge=(150,196,226); tube=(204,232,247); line=(168,206,232); core=(232,246,252)
    for y in range(H):
        for x in range(W):
            m=mask[y][x]; q=(y*W+x)*4
            if m>0.02:
                dd=d[y][x] if inside[y][x] else 0.5
                if dd<1.6: c=edge
                elif 3.0<=dd<4.4: c=line          # the tube's inner line
                elif dd>=6: c=core
                else: c=tube
                a=max(int(255*min(1,m)),int(200*glow[y][x]))
                px[q:q+4]=premul(c,a)
            else:
                g=glow[y][x]
                if g>0.01: px[q:q+4]=premul((140,190,225),int(min(1,g*1.4)*70))
    return px
if __name__=='__main__':
    mask=load_mask(sys.argv[1],int(sys.argv[2]))
    px=style(mask)
    png('neon.png',W,H,px)
    open('neon.rgba','wb').write(px)
