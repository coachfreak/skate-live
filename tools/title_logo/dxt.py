import struct, zlib
def swap16(b):
    a=bytearray(b); a[0::2],a[1::2]=b[1::2],b[0::2]; return bytes(a)
def c565(v): return (((v>>11)&31)*255//31, ((v>>5)&63)*255//63, (v&31)*255//31)
def decode_dxt5(data, w, h, pitch):
    data=swap16(data)
    bw=max(1,(w+3)//4); bh=max(1,(h+3)//4); rowblocks=max(pitch//4,bw)
    px=bytearray(w*h*4)
    for by in range(bh):
        for bx in range(bw):
            o=(by*rowblocks+bx)*16; blk=data[o:o+16]
            a0,a1=blk[0],blk[1]; abits=int.from_bytes(blk[2:8],'little')
            al=[a0,a1]+([(6-i)*a0//6+i*a1//6 for i in range(1,5)] if False else [])
            if a0>a1: al=[a0,a1]+[((7-i)*a0+i*a1)//7 for i in range(1,7)]
            else: al=[a0,a1]+[((5-i)*a0+i*a1)//5 for i in range(1,5)]+[0,255]
            c0,c1=struct.unpack_from('<HH',blk,8); cb=struct.unpack_from('<I',blk,12)[0]
            p0,p1=c565(c0),c565(c1)
            cols=[p0,p1,tuple((2*a+b)//3 for a,b in zip(p0,p1)),tuple((a+2*b)//3 for a,b in zip(p0,p1))]
            for i in range(16):
                x=bx*4+i%4; y=by*4+i//4
                if x>=w or y>=h: continue
                c=cols[(cb>>(2*i))&3]; a=al[(abits>>(3*i))&7]
                q=(y*w+x)*4; px[q:q+4]=bytes((c[0],c[1],c[2],a))
    return px
def png(path,w,h,rgba):
    raw=b''.join(b'\0'+bytes(rgba[y*w*4:(y+1)*w*4]) for y in range(h))
    def chunk(t,d): return struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
    open(path,'wb').write(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(raw,6))+chunk(b'IEND',b''))
