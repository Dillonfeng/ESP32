# -*- coding: utf-8 -*-
# Locate the exact frame the firmware rejected (FF F1 4C 80 44 BF FC 21) in
# freshly downloaded segments, compare its payload-start byte with neighboring
# frames, and dump the frame for a reference decoder.
import urllib.request

def g(url):
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    return urllib.request.urlopen(req, timeout=20).read()

root = "http://ngcdn002.cnr.cn/live/jjzs/"
txt = g(root + "index.m3u8").decode()
segfiles = [l.strip() for l in txt.splitlines() if l.strip() and not l.startswith("#")]

# build firmware-style ES (exact copy of sim_fw_ts logic, condensed)
PES_DataLength=0; fillData=0; pidOfAAC=0
def reset():
    global PES_DataLength,fillData,pidOfAAC
    PES_DataLength=0; fillData=0; pidOfAAC=0
def parse(p, ps, pl):
    global PES_DataLength,fillData,pidOfAAC
    if p[0]!=0x47: ps[0]=0; pl[0]=0; return
    PID=((p[1]&0x1F)<<8)|p[2]; PUSI=(p[1]&0x40)>>6; AFC=(p[3]&0x30)>>4
    AFL=p[4] if AFC&2 else -1
    if PID==0 and PUSI: ps[0]=0; pl[0]=0; return
    if PUSI and PID>0 and pidOfAAC==0:
        pos=4 if AFL<0 else 5+AFL
        if pos+4<=188 and p[pos]==0 and p[pos+1]==0 and p[pos+2]==1 and 0xC0<=p[pos+3]<=0xDF:
            pidOfAAC=PID; PHL=p[pos+8]; PPL=(p[pos+4]<<8)|p[pos+5]
            PES_DataLength=0x7FFFFFFF if PPL==0 else PPL; sod=PHL+9
            if pos+sod>=188: ps[0]=0; pl[0]=0; PES_DataLength-=(PHL+3); return
            ps[0]=pos+sod; pl[0]=188-pos-sod; PES_DataLength-=pl[0]; PES_DataLength-=(PHL+3); return
        ps[0]=0; pl[0]=0; return
    if PID==pidOfAAC:
        pos=4 if AFL<0 else 5+AFL
        if pos+3>188: ps[0]=0; pl[0]=0; return
        if PUSI and PES_DataLength>0: PES_DataLength=0; fillData=0
        if PES_DataLength>0:
            if pos+fillData>=188: ps[0]=0; pl[0]=0; fillData=0; return
            ps[0]=pos+fillData; pl[0]=188-pos-fillData; fillData=0; PES_DataLength-=pl[0]; return
        psx=-1
        if PUSI:
            for i in range(pos,185):
                if i+3<188 and p[i]==0 and p[i+1]==0 and p[i+2]==1 and 0xC0<=p[i+3]<=0xDF:
                    psx=i; break
        if psx<0: ps[0]=0; pl[0]=0; return
        pos=psx
        if pos+9>188: ps[0]=0; pl[0]=0; return
        PHL=p[pos+8]; PPL=(p[pos+4]<<8)|p[pos+5]
        PES_DataLength=0x7FFFFFFF if PPL==0 else PPL; sod=PHL+9
        if pos+sod>=188:
            ps[0]=0; pl[0]=0; PES_DataLength-=(PHL+3); fillData=(pos+sod)-188; return
        ps[0]=pos+sod; pl[0]=188-pos-sod; PES_DataLength-=pl[0]; PES_DataLength-=(PHL+3); return
    ps[0]=0; pl[0]=0

es=bytearray()
for sf in segfiles:
    d=g(root+sf); reset()
    for i in range(0,len(d)-187,188):
        p=d[i:i+188]; ps=[0]; pl=[0]; parse(p,ps,pl)
        if pl[0]>0: es+=p[ps[0]:ps[0]+pl[0]]
print("ES", len(es))

# walk every frame; collect payload-start first-byte statistics and find the
# rejected frame header pattern
off=0; n=0; firstbytes={}; suspicious=[]
while off+7<=len(es):
    if es[off]!=0xFF or (es[off+1]&0xF0)!=0xF0:
        off+=1; continue
    fl=((es[off+3]&3)<<11)|(es[off+4]<<3)|(es[off+5]>>5)
    cc=((es[off+2]&1)<<2)|((es[off+3]>>6)&3)
    if not (7<=fl<=8192): off+=1; continue
    pb=es[off+7]
    firstbytes[pb]=firstbytes.get(pb,0)+1
    # element ID = top 3 bits of payload byte
    eid=pb>>5
    if cc==2 and eid!=2:   # cc=2 must start CPE (010)
        suspicious.append((off,fl,pb,eid))
    off+=fl; n+=1

print("frames:", n)
print("payload first-byte histogram (top 12):")
for b,c in sorted(firstbytes.items(), key=lambda x:-x[1])[:12]:
    print("  0x%02X (element ID %d): %d frames" % (b, b>>5, c))
print("cc=2 frames NOT starting with CPE:", len(suspicious))
for s in suspicious[:10]: print("  off=%d fl=%d pbyte=0x%02X eid=%d" % s)
