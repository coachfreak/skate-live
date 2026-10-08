import struct,sys
sys.path.insert(0,'.')
from big import read, U32
def textures(rx2):
    base=len(rx2)-U32(rx2,0x54)
    out=[]
    a=0x100
    while a+48<=len(rx2):
        if U32(rx2,a+20)==0x00010031 and U32(rx2,a+44)==0x000200E8:
            raster=base+U32(rx2,a); size=U32(rx2,a+8); hdr=U32(rx2,a+24)
            f=struct.unpack_from('>6I',rx2,hdr+0x1C)
            w=(f[2]&0x1FFF)+1; h=((f[2]>>13)&0x1FFF)+1
            out.append(dict(a=a,raster=raster,size=size,hdr=hdr,fetch=f,w=w,h=h,fmt=f[1]&0x3F,endian=(f[1]>>6)&3,tiled=(f[0]>>31)&1,pitch=((f[0]>>22)&0x1FF)*32))
        a+=4
    return out
if __name__=='__main__':
    rx2=read('/Applications/game/data/big/fedynamic.big','data/fe/source/screens/bootflow/welcomescreen.rx2')
    open('welcomescreen.rx2','wb').write(rx2)
    for i,t in enumerate(textures(rx2)):
        print(i, f"{t['w']}x{t['h']} fmt={t['fmt']:#x} endian={t['endian']} tiled={t['tiled']} pitch={t['pitch']} size={t['size']} hdr={t['hdr']:#x} fetch={[hex(x) for x in t['fetch']]}")
