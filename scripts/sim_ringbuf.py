# -*- coding: utf-8 -*-
# Simulate the AudioBuffer ring buffer exactly as firmware does, including
# getReadPtr() linearization and bytesWasRead()/bytesWritten() wrap-around.
# Feed a clean ES stream (from sim_fw_ts.py) through the buffer at a rate
# that makes it hit 100% full, then check if the consumer reads back the
# same bytes. If not, the ring buffer implementation corrupts data when full.

class AudioBuffer:
    def __init__(self, buffSize, maxBlockSize=1600):
        # m_resBuffSizePSRAM = 4096*4 = 16384 reserved for linearization
        self.reserved = 16384
        self.m_buffSizePSRAM = buffSize + self.reserved
        self.m_buffer = bytearray(self.m_buffSizePSRAM)
        self.m_buffSize = buffSize
        self.m_endPtr = self.m_buffSize - 1  # index of last valid byte
        self.m_readPtr = 0
        self.m_writePtr = 0
        self.m_f_start = True
        self.m_maxBlockSize = maxBlockSize

    def bufferFilled(self):
        if self.m_f_start: return 0
        if self.m_writePtr >= self.m_readPtr:
            return self.m_writePtr - self.m_readPtr
        return (self.m_endPtr - self.m_readPtr) + (self.m_writePtr - 0) + 1

    def freeSpace(self):
        return self.m_buffSize - self.bufferFilled()

    def writeSpace(self):
        if self.m_f_start: return self.m_buffSize
        if self.m_writePtr >= self.m_readPtr:
            return (self.m_endPtr - self.m_writePtr) + 1
        return self.m_readPtr - self.m_writePtr

    def getWritePtr(self):
        return self.m_writePtr

    def bytesWritten(self, bw):
        self.m_writePtr += bw
        if self.m_writePtr > self.m_endPtr:
            tmp = self.m_writePtr - self.m_endPtr - 1
            self.m_writePtr = tmp
        self.m_f_start = False

    def getReadPtr(self):
        # returns index into m_buffer (firmware returns pointer)
        length = self.m_endPtr - self.m_readPtr
        if length < self.m_maxBlockSize:
            # memcpy(m_endPtr+1, m_buffer, maxBlockSize - (length - 1))
            copyLen = self.m_maxBlockSize - (length - 1)
            # copy m_buffer[0:copyLen] to m_buffer[m_endPtr+1 : m_endPtr+1+copyLen]
            src = self.m_buffer[0:copyLen]
            dst_start = self.m_endPtr + 1
            self.m_buffer[dst_start:dst_start+copyLen] = src
        return self.m_readPtr

    def bytesWasRead(self, br):
        self.m_readPtr += br
        if self.m_readPtr > self.m_endPtr:
            tmp = self.m_readPtr - self.m_endPtr - 1
            self.m_readPtr = tmp

# Now simulate producer/consumer with a clean ES stream
import urllib.request, sys

def g(url):
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    return urllib.request.urlopen(req, timeout=15).read()

m3u8 = "http://ngcdn002.cnr.cn/live/jjzs/index.m3u8"
txt = g(m3u8).decode()
seg = [l.strip() for l in txt.splitlines() if l.strip() and not l.startswith("#")][0]
seg = "http://ngcdn002.cnr.cn/live/jjzs/" + seg
d = g(seg)
print("seg:", seg, "len", len(d))

# Get clean ES using the same sim as before
PES_DataLength = 0; fillData = 0; pidOfAAC = 0
def ts_parse(packet, ps, pl):
    global PES_DataLength, fillData, pidOfAAC
    if packet is None:
        PES_DataLength=0; fillData=0; pidOfAAC=0; return
    if packet[0]!=0x47: ps[0]=0; pl[0]=0; return
    PID=((packet[1]&0x1F)<<8)|packet[2]; PUSI=(packet[1]&0x40)>>6
    AFC=(packet[3]&0x30)>>4; AFL=packet[4] if AFC&2 else -1
    PLS=(5+AFL) if AFC&2 else 4
    if PID==0 and PUSI: ps[0]=0; pl[0]=0; return
    if PUSI and PID>0 and pidOfAAC==0:
        pos=4 if AFL<0 else 5+AFL
        isA=pos+4<=188 and packet[pos]==0 and packet[pos+1]==0 and packet[pos+2]==1 and 0xC0<=packet[pos+3]<=0xDF
        if isA:
            pidOfAAC=PID
            if packet[pos]==0 and packet[pos+1]==0 and packet[pos+2]==1:
                PES_HDL=packet[pos+8]
                PES_PL=(packet[pos+4]<<8)|packet[pos+5]
                PES_DataLength=0x7FFFFFFF if PES_PL==0 else PES_PL
                startOfData=PES_HDL+9
                if pos+startOfData>=188:
                    ps[0]=0; pl[0]=0; PES_DataLength-=(PES_HDL+3); return
                ps[0]=pos+startOfData; pl[0]=188-pos-startOfData
                PES_DataLength-=pl[0]; PES_DataLength-=(PES_HDL+3); return
            ps[0]=pos; pl[0]=188-pos; return
        ps[0]=0; pl[0]=0; return
    if PID==pidOfAAC:
        pos=4 if AFL<0 else 5+AFL
        if pos+3>188: ps[0]=0; pl[0]=0; return
        if PUSI and PES_DataLength>0: PES_DataLength=0; fillData=0
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
            if packet[pos]==0 and packet[pos+1]==0 and packet[pos+2]==1:
                PES_HDL=packet[pos+8]
                PES_PL=(packet[pos+4]<<8)|packet[pos+5]
                PES_DataLength=0x7FFFFFFF if PES_PL==0 else PES_PL
                startOfData=PES_HDL+9
                if pos+startOfData>=188:
                    ps[0]=0; pl[0]=0; PES_DataLength-=(PES_HDL+3)
                    fillData=(pos+startOfData)-188; return
                ps[0]=pos+startOfData; pl[0]=188-pos-startOfData
                PES_DataLength-=pl[0]; PES_DataLength-=(PES_HDL+3); return
            ps[0]=0; pl[0]=0; return
    ps[0]=0; pl[0]=0

ts_parse(None, [0], [0])
es = bytearray()
for i in range(0, len(d)-187, 188):
    p=d[i:i+188]; ps=[0]; pl=[0]
    ts_parse(p, ps, pl)
    if pl[0]>0: es += p[ps[0]:ps[0]+pl[0]]
print("clean ES len:", len(es))

# Now feed ES through the ring buffer at producer > consumer rate
# to force the buffer to hit 100% full, then verify data integrity.
BUFF = 245759
buf = AudioBuffer(BUFF, 1600)

# Producer: write in chunks of 184 bytes (TS payload size)
# Consumer: read in frames (parse ADTS to get frame length)
def adts_frame_len(data, off):
    if off+7 > len(data): return None
    if not (data[off]==0xFF and (data[off+1]&0xF0)==0xF0): return None
    flen = ((data[off+3]&0x3)<<11) | (data[off+4]<<3) | ((data[off+5]>>5)&7)
    if flen < 7 or flen > 8192: return None
    return flen

prod_off = 0  # offset into clean ES
cons_off = 0  # expected offset
errors = 0
total_written = 0
total_read = 0
step = 0

# We simulate: producer writes chunks, consumer reads frames.
# To force buffer-full, make producer write faster.
while prod_off < len(es) or cons_off < total_written:
    step += 1
    if step > 500000:
        print("STUCK: prod_off=%d cons_off=%d total_written=%d filled=%d" % (prod_off, cons_off, total_written, buf.bufferFilled()))
        break

    # Producer: write up to 5 chunks of 184 bytes (faster than consumer)
    for _ in range(5):
        if prod_off >= len(es): break
        if buf.freeSpace() < 184: break
        chunk = 184 if prod_off + 184 <= len(es) else len(es) - prod_off
        ws = buf.writeSpace()
        if ws >= chunk:
            buf.m_buffer[buf.getWritePtr():buf.getWritePtr()+chunk] = es[prod_off:prod_off+chunk]
            buf.bytesWritten(chunk)
        else:
            buf.m_buffer[buf.getWritePtr():buf.getWritePtr()+ws] = es[prod_off:prod_off+ws]
            buf.bytesWritten(ws)
            rem = chunk - ws
            buf.m_buffer[buf.getWritePtr():buf.getWritePtr()+rem] = es[prod_off+ws:prod_off+chunk]
            buf.bytesWritten(rem)
        prod_off += chunk
        total_written += chunk

    # Consumer: read one ADTS frame
    if buf.bufferFilled() < 1600: continue
    readPtr = buf.getReadPtr()
    # Find ADTS syncword at readPtr
    flen = adts_frame_len(buf.m_buffer, readPtr)
    if flen is None:
        # skip 1 byte
        buf.bytesWasRead(1)
        total_read += 1
        cons_off += 1
        errors += 1
        if errors <= 20:
            print("ERROR at cons_off=%d, readPtr=%d, byte=%02X (expected %02X)" % (
                cons_off, readPtr, buf.m_buffer[readPtr], es[cons_off-1] if cons_off>0 else -1))
        continue
    # Check frame content matches expected
    frame_end = readPtr + flen
    if frame_end <= buf.m_endPtr + 1:
        actual = bytes(buf.m_buffer[readPtr:frame_end])
    else:
        # linearized: readPtr..endPtr + (endPtr+1)..(readPtr+flen after wrap)
        part1 = bytes(buf.m_buffer[readPtr:buf.m_endPtr+1])
        part2_len = flen - len(part1)
        actual = part1 + bytes(buf.m_buffer[buf.m_endPtr+1:buf.m_endPtr+1+part2_len])
    expected = bytes(es[cons_off:cons_off+flen])
    if actual != expected:
        errors += 1
        if errors <= 10:
            print("MISMATCH at cons_off=%d flen=%d readPtr=%d filled=%d" % (cons_off, flen, readPtr, buf.bufferFilled()))
            # find first differing byte
            for i in range(min(len(actual), len(expected))):
                if actual[i] != expected[i]:
                    print("  first diff at offset %d: actual=%02X expected=%02X" % (i, actual[i], expected[i]))
                    break
    buf.bytesWasRead(flen)
    total_read += flen
    cons_off += flen

print("\nTotal written: %d, total read: %d, errors: %d" % (total_written, total_read, errors))
print("Final cons_off: %d, es len: %d" % (cons_off, len(es)))
