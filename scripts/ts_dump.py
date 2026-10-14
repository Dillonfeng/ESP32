# -*- coding: utf-8 -*-
import urllib.request, sys

def g(url):
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    return urllib.request.urlopen(req, timeout=12).read()

name = sys.argv[1] if len(sys.argv) > 1 else "jjzs"
hosts = {
    "jjzs": ("http://ngcdn002.cnr.cn/live/jjzs/index.m3u8",
             "http://ngcdn002.cnr.cn/live/jjzs/"),
    "dszs": ("http://ngcdn002.cnr.cn/live/dszs/index.m3u8",
             "http://ngcdn002.cnr.cn/live/dszs/"),
}
m3u8, root = hosts[name]
txt = g(m3u8).decode()
seg = [l.strip() for l in txt.splitlines() if l.strip() and not l.startswith("#")][0]
if not seg.startswith("http"):
    seg = root + seg
d = g(seg)
print("seg:", seg, "len", len(d))

cnt = 0
for i in range(0, len(d) - 187, 188):
    p = d[i:i+188]
    if p[0] != 0x47:
        continue
    pid = ((p[1] & 0x1F) << 8) | p[2]
    pusi = (p[1] & 0x40) >> 6
    afc = (p[3] & 0x30) >> 4
    cc = p[3] & 0xF
    afl = p[4] if afc & 2 else -1
    if cnt < 3 or (pusi and pid > 0):
        print("--- pktOff=%d pid=%d(0x%03X) pusi=%d afc=%d cc=%d afl=%s" %
              (i, pid, pid, pusi, afc, cc, afl))
        print("  hdr[0..21]:", " ".join("%02X" % b for b in p[:22]))
        pls = (5 + afl) if afc & 2 else 4
        print("  payloadStart=%d [%d..%d]:" % (pls, pls, min(pls + 24, 187)),
              " ".join("%02X" % b for b in p[pls:min(pls + 24, 188)]))
        # decode PES if found
        for k in range(pls, 184):
            if k + 8 < 188 and p[k] == 0 and p[k+1] == 0 and p[k+2] == 1:
                sid = p[k+3]
                if 0xC0 <= sid <= 0xDF:
                    ppl = (p[k+4] << 8) | p[k+5]
                    hdl = p[k+8]
                    sod = k + hdl + 9
                    print("  PES@%d sid=%02X PES_len=%d hdrDataLen=%d dataStart=%d (inPkt=%s)" %
                          (k, sid, ppl, hdl, sod, sod < 188))
                    break
        cnt += 1
    if cnt >= 8:
        break
