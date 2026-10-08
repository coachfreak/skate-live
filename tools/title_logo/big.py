import struct, sys
def U16(b,o): return struct.unpack_from('>H',b,o)[0]
def U32(b,o): return struct.unpack_from('>I',b,o)[0]
def refpack(src, expected=0):
    n=len(src); p = (6 if src[0]&0x80 else 5) if n>=2 and src[1]==0xFB and src[0] in (0x10,0x90) else 0
    out=bytearray()
    def lit(c):
        nonlocal p
        out.extend(src[p:p+c]); p+=c
    def cp(d,c):
        for _ in range(c): out.append(out[-d])
    while p<n:
        c=src[p]; p+=1
        if c<0x80:
            b1=src[p]; p+=1; lit(c&3); cp(((c&0x60)<<3)+b1+1, ((c>>2)&7)+3)
        elif c<0xC0:
            b1,b2=src[p],src[p+1]; p+=2; lit(b1>>6); cp(((b1&0x3F)<<8)+b2+1,(c&0x3F)+4)
        elif c<0xE0:
            b1,b2,b3=src[p],src[p+1],src[p+2]; p+=3; lit(c&3); cp(((c&0x10)<<12)+(b1<<8)+b2+1, ((c&0x0C)<<6)+b3+5)
        elif c<0xFC: lit(((c&0x1F)<<2)+4)
        else: lit(c&3); break
    return bytes(out)
def entries(path):
    f=open(path,'rb'); head=f.read(48)
    assert U16(head,0)==0x4542 and U16(head,2)==3
    f.seek(0); meta=f.read(U32(head,12)+U32(head,16))
    count=U32(meta,4); shift=meta[10]; name_rec=meta[20]; dir_rec=meta[21]
    es=20 if U16(meta,8)&1 else 16
    comp=48+((es*count+15)&~15)
    names=U32(head,12); dirs=names+((name_rec*count+15)&~15)
    directories=[]
    at=dirs
    while at+dir_rec<=len(meta):
        directories.append(meta[at:at+dir_rec].split(b'\0')[0].decode('latin1')); at+=dir_rec
    out=[]
    for i in range(count):
        rec=names+i*name_rec; d=U16(meta,rec)
        full=meta[rec+2:rec+name_rec].split(b'\0')[0].decode('latin1')
        if d<len(directories) and directories[d] not in ('','.'): full=directories[d]+'/'+full
        full=full.replace('\\','/')
        e=48+i*es; stored=U32(meta,e+4); unp=U32(meta,e+8) or stored; stored=stored or unp
        out.append((full,U32(meta,e)<<shift,stored,unp,meta[comp+i]))
    return out
def read(path,name):
    for full,off,stored,unp,m in entries(path):
        if full==name:
            f=open(path,'rb'); f.seek(off); d=f.read(stored)
            return refpack(d,unp) if m==1 else d
    raise KeyError(name)
if __name__=='__main__':
    for p in sys.argv[1:]:
        for e in entries(p): print(p.split('/')[-1], e[0], e[3])
