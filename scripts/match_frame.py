# -*- coding: utf-8 -*-
# Match frames dumped by the firmware (FRAMEHEX lines) against the segments
# saved by collect_segs.py. Builds firmware-style ES per segment (same logic as
# sim_fw_ts) and searches for the exact byte string.
#   found  -> stream/encoder content (report element-walk divergence point)
#   absent -> on-device runtime corruption
import os, re, sys, glob

capdir = sys.argv[1] if len(sys.argv)>1 else r"D:\ESP32-S3\segs_cap"
logfile = sys.argv[2] if len(sys.argv)>2 else r"D:\ESP32-S3\cap_dump.log"

# ---- parse dumped frames from serial log ----
lines = open(logfile, encoding="utf-8", errors="replace").read().splitlines()
dumps=[]   # list of (nbytes, bytes)
cur=None
for l in lines:
    m = re.search(r"FRAMEHEX (\d+)\+(\d+):([0-9A-F]*)", l)
    if not m: continue
    n=int(m.group(1)); base=int(m.group(2)); hexs=m.group(3)
    if base==0:
        cur=bytearray(n)
        dumps.append(cur)
    if cur is not None:
        b=bytes.fromhex(hexs)
        cur[base:base+len(b)]=b
dumps=[bytes(d) for d in dumps if len(d)>=7]
print("dumped frames:", len(dumps))

# ---- firmware-style TS parse (condensed, same logic) ----
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

# build ES per saved segment, plus a global concatenation
segfiles=sorted(glob.glob(os.path.join(capdir,"*.ts")))
print("saved segments:", len(segfiles))
allES=bytearray(); segES={}
for sf in segfiles:
    d=open(sf,"rb").read()
    es=parse_all(d)
    segES[os.path.basename(sf)]=es
    allES+=es
allES=bytes(allES)

for k,fr in enumerate(dumps):
    at = allES.find(fr)
    if at>=0:
        # find which segment
        acc=0; owner="?"
        for sf in segfiles:
            l=len(segES[os.path.basename(sf)])
            if acc+l>at: owner=os.path.basename(sf); break
            acc+=l
        print("frame %d: FOUND at ES off %d in %s -> stream/encoder content" % (k, at, owner))
    else:
        # try first 32 / 64 byte prefixes to measure how far bytes agree
        pre=None
        for L in (64,32,16,8):
            p=allES.find(fr[:L])
            if p>=0: pre=(L,p); break
        print("frame %d: NOT FOUND (closest prefix match: %s) -> runtime corruption" % (k, pre))
