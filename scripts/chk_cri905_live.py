# -*- coding: utf-8 -*-
# One-off: download current CRI905 (环球资讯 sk.cri.cn) live segments and check
# whether the stream itself contains overlap frames (valid ADTS sync before the
# declared frame end), independent of device code.
import urllib.request, sys, os, time
from urllib.parse import urljoin

M3U8 = "http://sk.cri.cn/905.m3u8"
def g(url):
    req=urllib.request.Request(url, headers={"User-Agent":"Mozilla/5.0"})
    return urllib.request.urlopen(req,timeout=15).read()

def demux(ts):
    data=ts; pmtpid=None; aacpid=None
    i=0
    while i+188<=len(data):
        if data[i]!=0x47: i+=1; continue
        pid=((data[i+1]&0x1F)<<8)|data[i+2]; pusi=data[i+1]&0x40
        p=data[i+4:]
        if pid==0 and pusi:
            ptr=p[0]; t=bytes(p[1+ptr:])
            pmtpid=((t[10]&0x1F)<<8)|t[11]
        elif pmtpid and pid==pmtpid and pusi:
            ptr=p[0]; t=bytes(p[1+ptr:])
            pi_len=((t[10]&0x0F)<<8)|t[11]; k=12+pi_len
            sec_len=((t[1]&0x0F)<<8)|t[2]
            end=3+sec_len-4
            while k<end:
                et=t[k]
                ep=((t[k+1]&0x1F)<<8)|t[k+2]
                if et in (0x0F,0x11): aacpid=ep
                ei_len=((t[k+3]&0x0F)<<8)|t[k+4]
                k+=5+ei_len
            break
        i+=188
    if aacpid is None: return b""
    es=bytearray(); pesbuf=bytearray()
    def flush():
        nonlocal pesbuf
        if not pesbuf: return
        if len(pesbuf)>=3 and pesbuf[0]==0 and pesbuf[1]==0 and pesbuf[2]==1:
            pesbuf=pesbuf[9+pesbuf[8]:]
        es.extend(pesbuf); pesbuf=bytearray()
    i=0
    while i+188<=len(data):
        if data[i]!=0x47: i+=1; continue
        pid=((data[i+1]&0x1F)<<8)|data[i+2]; pusi=data[i+1]&0x40
        if pid==aacpid:
            afc=(data[i+3]>>4)&3; off=4
            if afc in (2,3): off=4+1+data[i+4]
            if pusi: flush()
            pesbuf.extend(data[i+off:i+188])
        i+=188
    flush()
    return bytes(es)

def valid_head(b):
    if len(b)<6: return None
    if b[0]!=0xFF or (b[1]&0xF0)!=0xF0: return None
    if (b[1]&6)!=0: return None
    if ((b[2]>>6)&3)==3 or (b[2]&0x3C)==0x3C: return None
    cc=((b[2]&1)<<2)|((b[3]>>6)&3)
    if cc<1 or cc>6: return None
    L=((b[3]&3)<<11)|(b[4]<<3)|(b[5]>>5)
    if L<7 or L>8192: return None
    return L

def analyze(es, label):
    off=0; total=0; overlap=0
    while off+6<=len(es):
        L=valid_head(es[off:])
        if L is None: off+=1; continue
        total+=1
        # search for a valid sync strictly before declared end
        found=None
        for p in range(1,L):
            if off+p+6>len(es): break
            h=valid_head(es[off+p:])
            if h is not None: found=(p,h); break
        if found:
            overlap+=1
            if overlap<=3: print("  [%s] frame@%d L=%d earlysync@%d efl=%d"%(label,off,L,found[0],found[1]))
        off+=L
    print("[%s] frames=%d overlap=%d"%(label,total,overlap))

txt=g(M3U8).decode()
names=[l.strip() for l in txt.splitlines() if l.strip().endswith(".ts") or ".ts?" in l.strip()]
print("m3u8 entries:",len(names))
names=names[-4:]
for n in names:
    url=urljoin(M3U8,n)
    ts=g(url)
    es=demux(ts)
    analyze(es,n.split("?")[0].split("/")[-1])
