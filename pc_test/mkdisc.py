import struct, sys
SEC=2048
def both16(v): return struct.pack('<H',v)+struct.pack('>H',v)
def both32(v): return struct.pack('<I',v)+struct.pack('>I',v)
def dt(): return bytes([95,1,1,0,0,0,0])
def dirrec(lba,size,flags,name):
    n=len(name); L=33+n+(1 if n%2==0 else 0)
    r=bytes([L,0])+both32(lba)+both32(size)+dt()+bytes([flags,0,0])+both16(1)+bytes([n])+name
    return r+b'\0'*(L-len(r))
# EXE
code=[0x3C081F80,0x3C090300,0xAD091814,0x3C090200,0x352900FF,0xAD091810,0xAD001810,
      0x3C0901E0,0x35290140,0xAD091810,0x1000FFFF,0]
import os
BIG=int(os.environ.get('BIG','0'))
body=b''.join(struct.pack('<I',c) for c in code); body+=b'\0'*(2048-len(body))
body+=bytes((i*7)&255 for i in range(BIG))
hdr=bytearray(2048); hdr[0:8]=b'PS-X EXE'
struct.pack_into('<IIII',hdr,0x10,0x80010000,0,0x80010000,len(body))
struct.pack_into('<II',hdr,0x30,0x801FFFF0,0)
exe=bytes(hdr)+body
NM=os.environ.get('NM','TEST.EXE').encode()
cnf=b'BOOT = cdrom:\\'+NM+b';1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFFF0\r\n'
files=[(b'SYSTEM.CNF;1',cnf),(NM+b';1',exe)]
ROOT=20; lba=21; ents=[]
for n,d in files:
    ents.append((n,lba,d)); lba+=(len(d)+SEC-1)//SEC
total=lba
img=bytearray(total*SEC)
if '--lic' in sys.argv:
    lic=os.environ.get('LIC','          Licensed  by          Sony Computer Entertainment Amer  ica ').encode()
    img[4*SEC:4*SEC+len(lic)]=lic
root=dirrec(ROOT,SEC,2,b'\0')+dirrec(ROOT,SEC,2,b'\1')
for n,l,d in ents:
    root+=dirrec(l,len(d),0,n); img[l*SEC:l*SEC+len(d)]=d
img[ROOT*SEC:ROOT*SEC+len(root)]=root
pt=lambda e: bytes([1,0])+(struct.pack('<I' if e=='<' else '>I',ROOT))+(struct.pack(e+'H',1))+b'\0\0'
img[18*SEC:18*SEC+10]=pt('<'); img[19*SEC:19*SEC+10]=pt('>')
p=bytearray(SEC); p[0]=1; p[1:6]=b'CD001'; p[6]=1
p[8:40]=b'PLAYSTATION'.ljust(32); p[40:72]=b'TEST'.ljust(32)
p[80:88]=both32(total); p[120:124]=both16(1); p[124:128]=both16(1); p[128:132]=both16(SEC)
p[132:140]=both32(10); struct.pack_into('<I',p,140,18); struct.pack_into('>I',p,148,19)
p[156:190]=dirrec(ROOT,SEC,2,b'\0'); p[881]=1
img[16*SEC:17*SEC]=p
t=bytearray(SEC); t[0]=255; t[1:6]=b'CD001'; t[6]=1; img[17*SEC:18*SEC]=t
open(sys.argv[1],'wb').write(img)
