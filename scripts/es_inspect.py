# -*- coding: utf-8 -*-
# Minimal self-conspecting inspection of ONE freshly downloaded segment:
#  - audio packet AFC/PUSI stats + first PUSI packets byte[3] (settle afc=1 vs 3)
#  - reference PES -> ES, then strict sequential ADTS walk from offset 0
#  - whenever offset+frame_len does not land on a sync, show the actual bytes
import urllib.request, sys
from collections import Counter

def g(url):
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    return urllib.request.urlopen(req, timeout=15).read()

SR = {3:48000,4:44100,1:88200,7:22050,0:96000,8:16000}

t = sys.argv[1] if len(sys.argv) > 1 else "jjzs"
urls = {
 "jjzs": ("http://ngcdn002.cnr.cn/live/jjzs/index.m3u8",
          "http://ngcdn002.cnr.cn/live/jjzs/"),
 "dszs": ("http://ngcdn002.cnr.cn/live/dszs/index.m3u8",
          "http://ngcdn002.cnr.cn/live/dszs/"),
 "905": ("http://sk.cri.cn/905.m3u8", "http://sk.cri.cn/"),
}
m3u8, root = urls[t]
txt = g(m3u8).decode()
seg = [l.strip() for l in txt.splitlines() if l.strip() and not l.startswith("#")][0]
if not seg.startswith("http"):
    seg = root + seg
d = g(seg)
print("segment:", seg, len(d))

# packet stats over ALL packets and audio packets
allp = Counter(); audiop = Counter(); b3_first = []
AUDIO = None
# find audio pid
for i in range(0, len(d)-187, 188):
    p = d[i:i+188]
    if p[0] != 0x47: continue
    pid = ((p[1]&0x1F)<<8)|p[2]
    afc = (p[3]&0x30)>>4
    pusi = (p[1]&0x40)>>6
    allp[(pusi,afc)] += 1
    for k in range((5+p[4]) if afc&2 else 4, 185):
        if k+3<188 and p[k]==0 and p[k+1]==0 and p[k+2]==1 and 0xC0<=p[k+3]<=0xDF:
            AUDIO = pid
            break
    if AUDIO is not None: break

pusi_afc_audio = Counter()
for i in range(0, len(d)-187, 188):
    p = d[i:i+188]
    if p[0] != 0x47: continue
    pid = ((p[1]&0x1F)<<8)|p[2]
    afc = (p[3]&0x30)>>4
    pusi = (p[1]&0x40)>>6
    if pid == AUDIO:
        audiop[(pusi,afc)] += 1
print("AUDIO pid:", hex(AUDIO))
print("ALL pkt (pusi,afc):", dict(allp))
print("AUDIO pkt (pusi,afc):", dict(audiop))

# reference PES extraction
es = bytearray()
rem = 0; fill = 0
for i in range(0, len(d)-187, 188):
    p = d[i:i+188]
    if p[0] != 0x47: continue
    pid = ((p[1]&0x1F)<<8)|p[2]
    if pid != AUDIO: continue
    afc = (p[3]&0x30)>>4
    pusi = (p[1]&0x40)>>6
    ps0 = (5+p[4]) if afc&2 else 4
    if pusi:
        ps = -1
        for k in range(ps0, 185):
            if k+8<188 and p[k]==0 and p[k+1]==0 and p[k+2]==1 and 0xC0<=p[k+3]<=0xDF:
                ps = k; break
        if ps < 0:
            rem = 0; fill = 0; continue
        ppl = (p[ps+4]<<8)|p[ps+5]; hdl = p[ps+8]; d0 = ps+9+hdl
        if d0 < 188:
            total = ppl - 3 - hdl if ppl else 0x7FFFFFFF
            take = min(188-d0, total)
            es += p[d0:d0+take]
            rem = total-take; fill = 0
        else:
            rem = (ppl-3-hdl) if ppl else 0x7FFFFFFF; fill = d0-188
    else:
        s = ps0+fill
        if s >= 188: fill = 0; continue
        take = min(188-s, rem)
        es += p[s:s+take]
        rem -= take; fill = 0

print("ES len:", len(es))
def hx(b): return " ".join("%02X"%x for x in b)
print("ES[0:32]:", hx(es[0:32]))

# strict sequential ADTS walk
off = 0; nf = 0; mism = 0
while off+7 <= len(es) and nf < 40:
    if not (es[off]==0xFF and (es[off+1]&0xF0)==0xF0):
        # search next sync
        nxt = off+1
        while nxt+1<len(es) and not (es[nxt]==0xFF and (es[nxt+1]&0xF0)==0xF0):
            nxt += 1
        print("off %d: NO sync, next sync at %d (skip %d), bytes:" % (off,nxt,nxt-off),
              hx(es[off:min(off+12,len(es))]))
        off = nxt; mism += 1
        if mism > 12: break
        continue
    sri = (es[off+2]>>2)&0xF
    ch = ((es[off+2]&1)<<2)|((es[off+3]>>6)&3)
    flen = ((es[off+3]&0x3)<<11)|(es[off+4]<<3)|((es[off+5]>>5)&7)
    layer = (es[off+1]>>1)&3
    valid = layer==0 and sri<13 and 0<ch<8 and 7<=flen<=8192
    mark = "OK " if valid else "BAD"
    print("%s off=%-7d sr=%-5s ch=%d flen=%d  next@%d sync=%s" %
          (mark, off, SR.get(sri,sri), ch, flen, off+flen,
           (off+flen+1<len(es) and es[off+flen]==0xFF and (es[off+flen+1]&0xF0)==0xF0)))
    if not valid:
        print("    bytes:", hx(es[off:off+10]))
        off += 1
    else:
        off += flen
    nf += 1
