# -*- coding: utf-8 -*-
# Validate the overlap detection: it must fire exactly on the frames the
# firmware rejected (from FRAMEHEX dumps), and never on normal frames.
import os, re, glob, sys
sys.path.insert(0, r"D:\ESP32-S3\scripts")
from check_after import parse_all

logfile=r"D:\ESP32-S3\cap_dump.log"; capdir=r"D:\ESP32-S3\segs_cap"
lines=open(logfile,encoding="utf-8",errors="replace").read().splitlines()
dump_list=[]; cur=None
for l in lines:
    m=re.search(r"FRAMEHEX (\d+)\+(\d+):([0-9A-F]*)",l)
    if not m: continue
    n=int(m.group(1)); base=int(m.group(2)); h=m.group(3)
    if base==0: cur=bytearray(n); dump_list.append(cur)
    b=bytes.fromhex(h); cur[base:base+len(b)]=b
dumps={bytes(d) for d in dump_list}

# InBuff contains continuous ES across segments: concatenate in order.
allES=bytearray()
for sf in sorted(glob.glob(os.path.join(capdir,"*.ts"))):
    allES += parse_all(open(sf,"rb").read())
allES=bytes(allES)

def probe(block, L):
    """mirror the firmware detection loop; return pos or None"""
    for d in (1,2):
        pos=L-d
        if L<16 or pos+8>len(block): continue
        if block[pos]!=0xFF or (block[pos+1]&0xF0)!=0xF0: continue
        if (block[pos+1]&6)!=0: continue
        if ((block[pos+2]>>6)&3)==3: continue
        if (block[pos+2]&0x3C)==0x3C: continue
        ecc=((block[pos+2]&1)<<2)|((block[pos+3]>>6)&3)
        efl=((block[pos+3]&3)<<11)|(block[pos+4]<<3)|(block[pos+5]>>5)
        if ecc<1 or efl<7: continue
        if pos+efl+1>=len(block): continue
        if block[pos+efl]!=0xFF or (block[pos+efl+1]&0xF0)!=0xF0: continue
        if not (efl>d and block[pos+d:pos+efl]==block[L:L+efl-d]): continue
        return pos
    return None

total_frames=0; detected=set(); rejected_seen=set(); fp=[]
off=0
while off+7<=len(allES):
    if allES[off]!=0xFF or (allES[off+1]&0xF0)!=0xF0: off+=1; continue
    L=((allES[off+3]&3)<<11)|(allES[off+4]<<3)|(allES[off+5]>>5)
    if not (7<=L<=8192): off+=1; continue
    total_frames+=1
    fr=allES[off:off+L]
    isRej = fr in dumps
    if isRej: rejected_seen.add(fr)
    block=allES[off:off+8192]
    posf=probe(block, L)
    if posf is not None:
        detected.add(fr)
        if not isRej: fp.append((off,L,posf))
        off += posf
    elif isRej:
        j=allES.find(b"\xFF\xF1", off+L-2, off+L+8)
        off = j if j>=0 else off+L
    else:
        off += L

print("total frames walked:", total_frames)
print("rejected frames found in walk:", len(rejected_seen), "of", len(dumps))
print("detection fired on:", len(detected))
print("correct hits:", len(detected & rejected_seen))
print("FALSE positives:", len(fp), fp)
print("MISSED:", len(rejected_seen - detected))
