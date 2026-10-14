# -*- coding: utf-8 -*-
import glob, os, sys
sys.path.insert(0, r"D:\ESP32-S3\scripts")
# inline parse to avoid side effects
def parse_all(d):
    PDL=0; fill=0; pid=0; es=bytearray()
    def P(p,ps,pl):
        nonlocal PDL,fill,pid
        if p[0]!=0x47: ps[0]=0;pl[0]=0;return
        PID=((p[1]&0x1F)<<8)|p[2]; PUSI=(p[1]&0x40)>>6; AFC=(p[3]&0x30)>>4
        AFL=p[4] if AFC&2 else -1
        if PID==0 and PUSI: ps[0]=0;pl[0]=0;return
        if PUSI and PID>0 and pid==0:
            pos=4 if AFL<0 else 5+AFL
            if pos+4<=188 and p[pos]==0 and p[pos+1]==0 and p[pos+2]==1 and 0xC0<=p[pos+3]<=0xDF:
                pid=PID; PHL=p[pos+8]; PPL=(p[pos+4]<<8)|p[pos+5]
                PDL=0x7FFFFFFF if PPL==0 else PPL; sod=PHL+9
                if pos+sod>=188: ps[0]=0;pl[0]=0; PDL-=(PHL+3);return
                ps[0]=pos+sod;pl[0]=188-pos-sod; PDL-=pl[0]; PDL-=(PHL+3);return
            ps[0]=0;pl[0]=0;return
        if PID==pid:
            pos=4 if AFL<0 else 5+AFL
            if pos+3>188: ps[0]=0;pl[0]=0;return
            if PUSI and PDL>0: PDL=0;fill=0
            if PDL>0:
                if pos+fill>=188: ps[0]=0;pl[0]=0;fill=0;return
                ps[0]=pos+fill;pl[0]=188-pos-fill;fill=0; PDL-=pl[0];return
            psx=-1
            if PUSI:
                for i in range(pos,185):
                    if i+3<188 and p[i]==0 and p[i+1]==0 and p[i+2]==1 and 0xC0<=p[i+3]<=0xDF:
                        psx=i;break
            if psx<0: ps[0]=0;pl[0]=0;return
            pos=psx
            if pos+9>188: ps[0]=0;pl[0]=0;return
            PHL=p[pos+8]; PPL=(p[pos+4]<<8)|p[pos+5]
            PDL=0x7FFFFFFF if PPL==0 else PPL; sod=PHL+9
            if pos+sod>=188:
                ps[0]=0;pl[0]=0; PDL-=(PHL+3); fill=(pos+sod)-188;return
            ps[0]=pos+sod;pl[0]=188-pos-sod; PDL-=pl[0]; PDL-=(PHL+3);return
        ps[0]=0;pl[0]=0
    for i in range(0,len(d)-187,188):
        p=d[i:i+188];ps=[0];pl=[0];P(p,ps,pl)
        if pl[0]>0: es+=p[ps[0]:ps[0]+pl[0]]
    return bytes(es)

es = parse_all(open(r"D:\ESP32-S3\segs_cap\16116943.ts","rb").read())
# walk frames from the frame at 133670 (fl 457) and verify continuity
off=133670; n=0; bad=0
while off+7<=len(es) and n<40:
    if es[off]!=0xFF or (es[off+1]&0xF0)!=0xF0:
        print("NO SYNC at",off); bad+=1; off+=1
        if bad>5: break
        continue
    fl=((es[off+3]&3)<<11)|(es[off+4]<<3)|(es[off+5]>>5)
    cc=((es[off+2]&1)<<2)|((es[off+3]>>6)&3)
    print("off=%-7d fl=%-4d cc=%d hdr=%s" % (off,fl,cc,es[off:off+7].hex().upper()))
    off+=fl; n+=1
