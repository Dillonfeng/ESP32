# -*- coding: utf-8 -*-
# Analyze the stream frames the firmware rejects: build per-segment firmware ES,
# walk all frames, mark which frames are rejected (from FRAMEHEX dumps) and
# compare their headers/lengths/positions against normal neighbors.
import os, re, sys, glob

capdir=r"D:\ESP32-S3\segs_cap"; logfile=r"D:\ESP32-S3\cap_dump.log"

lines=open(logfile,encoding="utf-8",errors="replace").read().splitlines()
dumps=[]; cur=None
for l in lines:
    m=re.search(r"FRAMEHEX (\d+)\+(\d+):([0-9A-F]*)",l)
    if not m: continue
    n=int(m.group(1)); base=int(m.group(2)); h=m.group(3)
    if base==0: cur=bytearray(n); dumps.append(cur)
    if cur is not None:
        b=bytes.fromhex(h); cur[base:base+len(b)]=b
dumps=set(bytes(d) for d in dumps)

# reuse parse logic (inline copy of match_frame.parse_all)
def parse_all(d):
    PDL=0; fill=0; pid=0
    es=bytearray()
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

segfiles=sorted(glob.glob(os.path.join(capdir,"*.ts")))
for sf in segfiles:
    es=parse_all(open(sf,"rb").read())
    off=0; frames=[]
    while off+7<=len(es):
        if es[off]!=0xFF or (es[off+1]&0xF0)!=0xF0: off+=1; continue
        fl=((es[off+3]&3)<<11)|(es[off+4]<<3)|(es[off+5]>>5)
        if not (7<=fl<=8192): off+=1; continue
        frames.append((off,fl)); off+=fl
    flagged=[i for i,(o,fl) in enumerate(frames) if bytes(es[o:o+fl]) in dumps]
    if not flagged: continue
    print("=== segment", os.path.basename(sf), "frames:", len(frames), "flagged:", flagged)
    for i in flagged:
        for j in (i-1,i,i+1):
            if not (0<=j<len(frames)): continue
            o,fl=frames[j]
            h=es[o:o+7]
            tag=">> REJECTED <<" if j==i else ""
            cc=((es[o+2]&1)<<2)|((es[o+3]>>6)&3)
            nrb=es[o+6]&3
            print("  frame#%-4d off=%-6d fl=%-4d cc=%d nrb=%d hdr=%s p0=%02X p1=%02X %s" %
                  (j,o,fl,cc,nrb,h.hex().upper(), es[o+7], es[o+8], tag))
