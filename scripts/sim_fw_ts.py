# -*- coding: utf-8 -*-
# Precisely simulate the FIRMWARE's ts_parsePacket() state machine on a real
# downloaded segment. If the firmware-style ES has misaligned ADTS frames while
# the reference demux (refdemux.py) is clean, the bug is in ts_parsePacket().
import urllib.request, sys

def g(url):
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    return urllib.request.urlopen(req, timeout=15).read()

t = sys.argv[1] if len(sys.argv) > 1 else "jjzs"
urls = {
 "jjzs": ("http://ngcdn002.cnr.cn/live/jjzs/index.m3u8",
          "http://ngcdn002.cnr.cn/live/jjzs/"),
 "905": ("http://sk.cri.cn/905.m3u8", "http://sk.cri.cn/"),
}
m3u8, root = urls[t]
txt = g(m3u8).decode()
seg = [l.strip() for l in txt.splitlines() if l.strip() and not l.startswith("#")][0]
if not seg.startswith("http"):
    seg = root + seg
d = g(seg)
print("seg:", seg, "len", len(d))

# firmware statics
PES_DataLength = 0
fillData = 0
pidOfAAC = 0

def ts_parse_packet(packet, packetStart, packetLength):
    global PES_DataLength, fillData, pidOfAAC
    if packet is None:  # reset
        PES_DataLength = 0; fillData = 0; pidOfAAC = 0
        return True
    if packet[0] != 0x47:
        packetStart[0] = 0; packetLength[0] = 0; return True
    PID = ((packet[1] & 0x1F) << 8) | packet[2]
    PUSI = (packet[1] & 0x40) >> 6
    AFC = (packet[3] & 0x30) >> 4
    AFL = packet[4] if AFC & 2 else -1
    PLS = (5 + AFL) if AFC & 2 else 4

    # PAT branch (PID 0, PUSI) - simplified, just skip
    if PID == 0 and PUSI:
        packetStart[0] = 0; packetLength[0] = 0; return True

    # auto-discovery
    if PUSI and PID > 0 and pidOfAAC == 0:
        pos = 4 if AFL < 0 else 5 + AFL
        isAudio = False
        if pos + 4 <= 188 and packet[pos]==0 and packet[pos+1]==0 and packet[pos+2]==1:
            if 0xC0 <= packet[pos+3] <= 0xDF: isAudio = True
        if isAudio:
            pidOfAAC = PID
            # inline PES parse (firmware auto-discovery branch)
            posOfPacketStart = pos
            fb = packet[posOfPacketStart]; sb = packet[posOfPacketStart+1]; tb = packet[posOfPacketStart+2]
            if fb==0 and sb==0 and tb==1:
                sid = packet[posOfPacketStart+3]
                if 0xC0 <= sid <= 0xDF:
                    PES_HeaderDataLength = packet[posOfPacketStart+8]
                    PES_PacketLength = (packet[posOfPacketStart+4]<<8)|packet[posOfPacketStart+5]
                    PES_DataLength = 0x7FFFFFFF if PES_PacketLength==0 else PES_PacketLength
                    startOfData = PES_HeaderDataLength + 9
                    if posOfPacketStart + startOfData >= 188:
                        packetStart[0]=0; packetLength[0]=0
                        PES_DataLength -= (PES_HeaderDataLength+3)
                        return True
                    packetStart[0] = posOfPacketStart + startOfData
                    packetLength[0] = 188 - posOfPacketStart - startOfData
                    PES_DataLength -= packetLength[0]
                    PES_DataLength -= (PES_HeaderDataLength+3)
                    return True
            # raw ADTS
            packetStart[0] = posOfPacketStart
            packetLength[0] = 188 - posOfPacketStart
            return True
        packetStart[0]=0; packetLength[0]=0; return True

    if PID == pidOfAAC:
        posOfPacketStart = 4 if AFL < 0 else 5 + AFL
        if posOfPacketStart + 3 > 188:
            packetStart[0]=0; packetLength[0]=0; return True
        if PUSI and PES_DataLength > 0:
            PES_DataLength = 0; fillData = 0
        if PES_DataLength > 0:
            if posOfPacketStart + fillData >= 188:
                packetStart[0]=0; packetLength[0]=0; fillData=0; return True
            packetStart[0] = posOfPacketStart + fillData
            packetLength[0] = 188 - posOfPacketStart - fillData
            fillData = 0
            PES_DataLength -= packetLength[0]
            return True
        else:
            pesStart = -1
            if PUSI:
                for i in range(posOfPacketStart, 185):
                    if i+3<188 and packet[i]==0 and packet[i+1]==0 and packet[i+2]==1:
                        sid = packet[i+3]
                        if 0xC0 <= sid <= 0xDF:
                            pesStart = i; break
            if pesStart < 0:
                packetStart[0]=0; packetLength[0]=0; return True
            posOfPacketStart = pesStart
            if posOfPacketStart + 9 > 188:
                packetStart[0]=0; packetLength[0]=0; return True
            fb=packet[posOfPacketStart]; sb=packet[posOfPacketStart+1]; tb=packet[posOfPacketStart+2]
            if fb==0 and sb==0 and tb==1:
                PES_HeaderDataLength = packet[posOfPacketStart+8]
                PES_PacketLength = (packet[posOfPacketStart+4]<<8)|packet[posOfPacketStart+5]
                PES_DataLength = 0x7FFFFFFF if PES_PacketLength==0 else PES_PacketLength
                startOfData = PES_HeaderDataLength + 9
                if posOfPacketStart + startOfData >= 188:
                    packetStart[0]=0; packetLength[0]=0
                    PES_DataLength -= (PES_HeaderDataLength+3)
                    fillData = (posOfPacketStart + startOfData) - 188
                    return True
                packetStart[0] = posOfPacketStart + startOfData
                packetLength[0] = 188 - posOfPacketStart - startOfData
                PES_DataLength -= packetLength[0]
                PES_DataLength -= (PES_HeaderDataLength+3)
                return True
            packetStart[0]=0; packetLength[0]=0; return True
    packetStart[0]=0; packetLength[0]=0; return True

# reset at segment start (firmware does this per segment)
ts_parse_packet(None, [0], [0])

es = bytearray()
for i in range(0, len(d)-187, 188):
    p = d[i:i+188]
    ps = [0]; pl = [0]
    ts_parse_packet(p, ps, pl)
    if pl[0] > 0:
        es += p[ps[0]:ps[0]+pl[0]]

print("firmware ES len:", len(es))
# ADTS walk
off=0; nf=0; mism=0; gaps=[]
while off+7 <= len(es) and nf < 60:
    if not (es[off]==0xFF and (es[off+1]&0xF0)==0xF0):
        nxt=off+1
        while nxt+1<len(es) and not (es[nxt]==0xFF and (es[nxt+1]&0xF0)==0xF0):
            nxt+=1
        print("off %d: NO sync, next at %d (gap %d)" % (off, nxt, nxt-off))
        off=nxt; mism+=1
        if mism>15: break
        continue
    sri=(es[off+2]>>2)&0xF
    ch=((es[off+2]&1)<<2)|((es[off+3]>>6)&3)
    flen=((es[off+3]&0x3)<<11)|(es[off+4]<<3)|((es[off+5]>>5)&7)
    layer=(es[off+1]>>1)&3
    valid = layer==0 and sri<13 and 0<ch<8 and 7<=flen<=8192
    if not valid:
        print("BAD off=%d sri=%d ch=%d flen=%d" % (off, sri, ch, flen))
        off+=1; continue
    print("OK  off=%-7d sr=%d ch=%d flen=%d  nextSync=%s" %
          (off, {3:48000,4:44100,1:88200,7:22050}.get(sri,sri), ch, flen,
           (off+flen+1<len(es) and es[off+flen]==0xFF and (es[off+flen+1]&0xF0)==0xF0)))
    off+=flen; nf+=1
