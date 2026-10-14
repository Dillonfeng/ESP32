# -*- coding: utf-8 -*-
# Simulate the FIRMWARE behavior across HLS segment boundaries:
#  - ts_parsePacket() state machine, reset per segment (as firmware does)
#  - ES bytes concatenated in stream order (as written into InBuff)
#  - decoder: m_f_playing forced false at boundary, findNextSync with the
#    ADTS validation + frame-length cross-check added in BUILD46
# Reports exactly which bytes get skipped at each boundary and whether the
# skipped region contains valid ADTS frames (= audible "lost words").
import urllib.request, sys

def g(url):
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    return urllib.request.urlopen(req, timeout=20).read()

root = "http://ngcdn002.cnr.cn/live/jjzs/"
txt = g(root + "index.m3u8").decode()
segfiles = [l.strip() for l in txt.splitlines() if l.strip() and not l.startswith("#")]
segfiles = segfiles[:4]
segs = [g(root + s) for s in segfiles]
print("segments:", segfiles, "sizes:", [len(s) for s in segs])

# ---- firmware ts_parsePacket state (reset per segment) ----
PES_DataLength = 0; fillData = 0; pidOfAAC = 0

def ts_reset():
    global PES_DataLength, fillData, pidOfAAC
    PES_DataLength = 0; fillData = 0; pidOfAAC = 0

def ts_parse_packet(packet, ps, pl):
    global PES_DataLength, fillData, pidOfAAC
    if packet[0] != 0x47:
        ps[0]=0; pl[0]=0; return
    PID = ((packet[1]&0x1F)<<8)|packet[2]
    PUSI = (packet[1]&0x40)>>6
    AFC = (packet[3]&0x30)>>4
    AFL = packet[4] if AFC&2 else -1

    if PID==0 and PUSI:
        ps[0]=0; pl[0]=0; return
    if PUSI and PID>0 and pidOfAAC==0:
        pos = 4 if AFL<0 else 5+AFL
        if pos+4<=188 and packet[pos]==0 and packet[pos+1]==0 and packet[pos+2]==1 and 0xC0<=packet[pos+3]<=0xDF:
            pidOfAAC=PID
            PES_HeaderDataLength=packet[pos+8]
            PPL=(packet[pos+4]<<8)|packet[pos+5]
            PES_DataLength=0x7FFFFFFF if PPL==0 else PPL
            sod=PES_HeaderDataLength+9
            if pos+sod>=188:
                ps[0]=0; pl[0]=0; PES_DataLength-=(PES_HeaderDataLength+3); return
            ps[0]=pos+sod; pl[0]=188-pos-sod
            PES_DataLength-=pl[0]; PES_DataLength-=(PES_HeaderDataLength+3); return
        ps[0]=0; pl[0]=0; return
    if PID==pidOfAAC:
        pos = 4 if AFL<0 else 5+AFL
        if pos+3>188: ps[0]=0; pl[0]=0; return
        if PUSI and PES_DataLength>0:
            PES_DataLength=0; fillData=0
        if PES_DataLength>0:
            if pos+fillData>=188: ps[0]=0; pl[0]=0; fillData=0; return
            ps[0]=pos+fillData; pl[0]=188-pos-fillData; fillData=0
            PES_DataLength-=pl[0]; return
        else:
            pesStart=-1
            if PUSI:
                for i in range(pos,185):
                    if i+3<188 and packet[i]==0 and packet[i+1]==0 and packet[i+2]==1 and 0xC0<=packet[i+3]<=0xDF:
                        pesStart=i; break
            if pesStart<0: ps[0]=0; pl[0]=0; return
            pos=pesStart
            if pos+9>188: ps[0]=0; pl[0]=0; return
            PES_HeaderDataLength=packet[pos+8]
            PPL=(packet[pos+4]<<8)|packet[pos+5]
            PES_DataLength=0x7FFFFFFF if PPL==0 else PPL
            sod=PES_HeaderDataLength+9
            if pos+sod>=188:
                ps[0]=0; pl[0]=0; PES_DataLength-=(PES_HeaderDataLength+3)
                fillData=(pos+sod)-188; return
            ps[0]=pos+sod; pl[0]=188-pos-sod
            PES_DataLength-=pl[0]; PES_DataLength-=(PES_HeaderDataLength+3); return
    ps[0]=0; pl[0]=0

# ---- build ES with per-segment boundary markers ----
es = bytearray()
bounds = []   # absolute ES offsets where a new segment begins
for d in segs:
    ts_reset()
    for i in range(0, len(d)-187, 188):
        p = d[i:i+188]
        ps=[0]; pl=[0]
        ts_parse_packet(p, ps, pl)
        if pl[0]>0: es += p[ps[0]:ps[0]+pl[0]]
    bounds.append(len(es))
print("ES total:", len(es), "boundaries at:", bounds)

# ---- ADTS frame helpers ----
def hdr(off):
    """return dict or None if not a candidate sync"""
    if off+7>len(es) or es[off]!=0xFF or (es[off+1]&0xF0)!=0xF0: return None
    b1,b2,b3,b4,b5 = es[off+1:off+6]
    return dict(layer=(b1>>1)&3, sri=(b2>>2)&0xF, prof=(b2>>6)&3,
               cc=((b2&1)<<2)|((b3>>6)&3),
               fl=((b3&3)<<11)|(b4<<3)|(b5>>5))

def valid_candidate(off):
    h = hdr(off)
    if h is None: return False
    if h['layer']!=0 or h['sri']==0xF or h['prof']==3: return False
    if h['cc']==0 or h['cc']>6: return False
    if not (7<=h['fl']<=8191): return False
    # cross-check: next frame sync at off+fl
    if off+h['fl']+1 < len(es):
        if es[off+h['fl']]!=0xFF or (es[off+h['fl']+1]&0xF0)!=0xF0: return False
    return True

# ---- emulate decoder across boundaries ----
# firmware: at each segment boundary m_f_playing=false -> findNextSync
off = 0
playing = False
framecount = 0
for bi, b in enumerate(bounds):
    # find start point: beginning of first segment, or boundary offset
    start = 0 if bi==0 else bounds[bi-1]  # search region begins where prior seg's ES ended...
    # Actually decoder state at boundary: readPtr somewhere BEFORE b; emulate:
    # reset playing, run findNextSync from current readPtr (= off).
    if bi>0:
        playing = False
        # findNextSync scan from off
        p = off
        found = -1
        while p+7 <= len(es) and p < bounds[min(bi,len(bounds)-1)] + 2000:
            if valid_candidate(p):
                found = p; break
            p += 1
        if found < 0:
            print("boundary after seg %d: NO valid candidate from %d" % (bi-1, off)); break
        skipped = found - off
        # inspect skipped region for strict-valid frames (without crosscheck reliance)
        lost = 0; q = off
        while q < found:
            h = hdr(q)
            if h and h['layer']==0 and h['sri']<13 and 0<h['cc']<8 and 7<=h['fl']<=8192:
                # looks like a real frame header
                lost += 1
            q += 1
        print("boundary %d (esoff=%d): readPtr=%d -> resync=%d  skipped=%3d bytes, frame-like headers in skip=%d"
              % (bi, b, off, found, skipped, lost))
        off = found
        playing = True
    # decode all frames strictly inside this segment
    while off < b:
        h = hdr(off)
        if h is None or not (7<=h['fl']<=8191):
            off += 1; continue
        if off+h['fl'] > b and bi < len(bounds):
            break  # frame belongs to continuous stream; firmware keeps it pending across boundary check
        off += h['fl']; framecount += 1

print("frames decoded in emulation:", framecount)
