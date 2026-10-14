# -*- coding: utf-8 -*-
# Forensic, ground-truth-only analysis of captured TS segments.
# Goal: determine objectively whether apparent "early sync" is a true frame
# overlap (stream property) or payload coincidence (firmware must trust L).
import sys

def demux(ts):
    data=ts; pmtpid=None; aacpid=None
    for i in range(0,len(data)-188+1,188):
        if data[i]!=0x47: continue
        pid=((data[i+1]&0x1F)<<8)|data[i+2]; pusi=data[i+1]&0x40
        if pid==0 and pusi:
            p=data[i+4:]; ptr=p[0]; t=bytes(p[1+ptr:]); pmtpid=((t[10]&0x1F)<<8)|t[11]
        elif pmtpid and pid==pmtpid and pusi:
            p=data[i+4:]; ptr=p[0]; t=bytes(p[1+ptr:])
            pi_len=((t[10]&0x0F)<<8)|t[11]; k=12+pi_len; sec_len=((t[1]&0x0F)<<8)|t[2]; end=3+sec_len-4
            while k<end:
                et=t[k]; ep=((t[k+1]&0x1F)<<8)|t[k+2]
                if et in (0x0F,0x11): aacpid=ep
                ei_len=((t[k+3]&0x0F)<<8)|t[k+4]; k+=5+ei_len
            break
    es=bytearray(); pesbuf=bytearray()
    def flush():
        nonlocal pesbuf
        if not pesbuf: return
        if len(pesbuf)>=3 and pesbuf[0]==0 and pesbuf[1]==0 and pesbuf[2]==1:
            pesbuf=pesbuf[9+pesbuf[8]:]
        es.extend(pesbuf); pesbuf=bytearray()
    for i in range(0,len(data)-188+1,188):
        if data[i]!=0x47: continue
        pid=((data[i+1]&0x1F)<<8)|data[i+2]; pusi=data[i+1]&0x40
        if pid==aacpid:
            afc=(data[i+3]>>4)&3; off=4
            if afc in (2,3): off=4+1+data[i+4]
            if pusi: flush()
            pesbuf.extend(data[i+off:i+188])
    flush()
    return bytes(es)

def hdr(b):
    if len(b)<7: return None
    if b[0]!=0xFF or (b[1]&0xF0)!=0xF0: return None
    if (b[1]&6)!=0: return None
    prof=(b[2]>>6)&3; sri=(b[2]>>2)&0xF; cc=((b[2]&1)<<2)|((b[3]>>6)&3)
    L=((b[3]&3)<<11)|(b[4]<<3)|(b[5]>>5)
    return dict(prof=prof,sri=sri,cc=cc,L=L)

def isvalid(h):
    return h and h['prof']!=3 and h['sri']!=0xF and 1<=h['cc']<=6 and 7<=h['L']<=8192

def scan(path, want):
    ts=open(path,'rb').read(); es=demux(ts)
    print("=== %s  ts=%d es=%d ==="%(path.split('/')[-1].split('\\')[-1],len(ts),len(es)))
    off=0; total=0; anomalies=0
    while off+7<=len(es):
        h=hdr(es[off:])
        if not isvalid(h): off+=1; continue
        total+=1; L=h['L']
        # find earliest valid-looking sync strictly inside (0,L)
        early=None
        for p in range(1,L):
            if off+p+7>len(es): break
            hh=hdr(es[off+p:])
            if isvalid(hh): early=(p,hh); break
        if early:
            anomalies+=1
            if anomalies<=want:
                p,hh=early
                atL = hdr(es[off+L:]) if off+L+7<=len(es) else None
                print(" frame@%d L=%d | early@%d efl=%d | bytes[L-3..L+4]=%s"%(
                    off,L,p,hh['L']," ".join("%02x"%x for x in es[off+L-3:off+L+5])))
                print("   header@declared L valid? %s ; header@early valid chain-next? %s"%(
                    isvalid(atL), isvalid(hdr(es[off+p+hh['L']:]))))
                # does declared-L walk and early walk agree 3 frames later?
                def walk(start, n=4):
                    o=start
                    for _ in range(n):
                        x=hdr(es[o:])
                        if not isvalid(x): return None
                        o+=x['L']
                    return o
                print("   walk4 from declared-L -> %s ; walk4 from early -> %s"%(
                    walk(off+L), walk(off+p)))
        off+=L
    print(" total frames=%d anomalies=%d (%.2f%%)"%(total,anomalies,100.0*anomalies/max(1,total)))

if __name__=="__main__":
    want=6
    for p in sys.argv[1:]:
        scan(p,want)
