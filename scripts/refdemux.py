# -*- coding: utf-8 -*-
# Decisive experiment: a clean ISO-13818 reference demuxer (independent of the
# firmware code). If the reference AAC ES has perfectly back-to-back ADTS frames
# with one stable sample rate, the stream is GOOD and the firmware demux is at
# fault. If the reference ES also switches sample rate / has gaps, the stream
# itself changes configuration and the firmware must follow it.
import urllib.request, sys
from collections import Counter

def g(url):
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    return urllib.request.urlopen(req, timeout=15).read()

SR = {0:96000,1:88200,2:64000,3:48000,4:44100,5:32000,6:24000,
      7:22050,8:16000,9:12000,10:11025,11:8000,12:7350}

def ref_demux(d, label):
    print("=" * 70)
    print(label, "bytes", len(d))
    # pass 1: find audio PID(s) by PES start code scan
    pid_is_audio = set()
    n = {}
    for i in range(0, len(d) - 187, 188):
        p = d[i:i+188]
        if p[0] != 0x47: continue
        pid = ((p[1] & 0x1F) << 8) | p[2]
        afc = (p[3] & 0x30) >> 4
        pls = (5 + p[4]) if afc & 2 else 4
        n[pid] = n.get(pid, 0) + 1
        for k in range(pls, 185):
            if k+3 < 188 and p[k] == 0 and p[k+1] == 0 and p[k+2] == 1:
                if 0xC0 <= p[k+3] <= 0xDF:
                    pid_is_audio.add(pid)
    print("audio PIDs:", [hex(x) for x in pid_is_audio], "all pids:",
          {hex(k): v for k, v in sorted(n.items(), key=lambda x: -x[1])[:6]})

    # pass 2: reference PES extraction
    es = bytearray()
    state = {pid: [0, 0] for pid in pid_is_audio}  # [remaining payload bytes, fill]
    afc_counter = Counter()
    pes_starts = 0
    boundary_issues = []
    for i in range(0, len(d) - 187, 188):
        p = d[i:i+188]
        if p[0] != 0x47: continue
        pid = ((p[1] & 0x1F) << 8) | p[2]
        if pid not in pid_is_audio: continue
        pusi = (p[1] & 0x40) >> 6
        afc = (p[3] & 0x30) >> 4
        afc_counter[afc] += 1
        rem, fill = state[pid]
        payload_start = (5 + p[4]) if afc & 2 else 4

        if pusi:
            # locate PES header anywhere in this packet's payload
            ps = -1
            for k in range(payload_start, 185):
                if k+8 < 188 and p[k] == 0 and p[k+1] == 0 and p[k+2] == 1 \
                   and 0xC0 <= p[k+3] <= 0xDF:
                    ps = k
                    break
            if ps < 0:
                boundary_issues.append(("PUSI-no-PES", i))
                state[pid] = [0, 0]
                continue
            pes_starts += 1
            ppl = (p[ps+4] << 8) | p[ps+5]
            hdl = p[ps+8]
            data0 = ps + 9 + hdl
            # bytes available in this packet from data0
            if data0 < 188:
                avail = 188 - data0
                if ppl:
                    total_payload = ppl - 3 - hdl
                    take = min(avail, total_payload)
                else:
                    take = avail  # unknown length, till next PUSI
                es += p[data0:data0+take]
                if ppl:
                    left = total_payload - take
                    state[pid] = [left, max(0, data0 + take - 188)]
                else:
                    state[pid] = [0x7FFFFFFF, 0]
            else:
                # header ends at/after packet edge
                state[pid] = [max(0, (ppl - 3 - hdl) if ppl else 0x7FFFFFFF),
                              data0 - 188]
        else:
            # continuation
            s0 = payload_start + fill
            if s0 >= 188:
                state[pid] = [rem, 0]
                continue
            avail = 188 - s0
            take = avail if rem >= avail else rem
            es += p[s0:s0+take]
            state[pid] = [rem - take, 0]

    print("AFC dist (audio pkts):", dict(afc_counter), "PES starts:", pes_starts,
          "boundary issues:", boundary_issues[:5])

    # ADTS scan over reference ES
    frames = []
    i = 0
    L = len(es)
    while i + 7 <= L:
        if es[i] == 0xFF and (es[i+1] & 0xF0) == 0xF0:
            layer = (es[i+1] >> 1) & 3
            sri = (es[i+2] >> 2) & 0xF
            ch = ((es[i+2] & 1) << 2) | ((es[i+3] >> 6) & 3)
            flen = ((es[i+3] & 0x3) << 11) | (es[i+4] << 3) | ((es[i+5] >> 5) & 7)
            if layer == 0 and sri < 13 and 0 < ch < 8 and 7 <= flen <= 8192:
                frames.append((i, sri, ch, flen))
                i += flen
                continue
        i += 1

    print("reference ES=%d bytes, ADTS frames=%d (frame bytes=%d, %.1f%%)" %
          (L, len(frames), sum(f[3] for f in frames),
           100 * sum(f[3] for f in frames) / max(L, 1)))
    if frames:
        sris = Counter(f[1] for f in frames)
        chs = Counter(f[2] for f in frames)
        print("  sample rates:", {SR.get(k): v for k, v in sris.items()})
        print("  channels:", dict(chs))
        fl = Counter(f[3] for f in frames)
        print("  frame len dist:", dict(sorted(fl.items())))
        # exact back-to-back continuity
        gaps = []
        for k in range(1, len(frames)):
            exp = frames[k-1][0] + frames[k-1][3]
            if frames[k][0] != exp:
                gaps.append((k, frames[k][0] - exp))
        print("  non-contiguous frame boundaries: %d / %d" % (len(gaps), len(frames)-1))
        if gaps:
            print("   first gaps (frameIdx, byteGap):", gaps[:10])
        # sample-rate change points
        changes = [k for k in range(1, len(frames)) if frames[k][1] != frames[k-1][1]]
        print("  sample-rate CHANGE points: %d" % len(changes))
        for k in changes[:8]:
            a, b = frames[k-1], frames[k]
            print("    @frame %d esOff %d: %d -> %d (flen %d->%d)" %
                  (k, b[0], SR.get(a[1]), SR.get(b[1]), a[3], b[3]))
    return frames

if __name__ == "__main__":
    t = sys.argv[1] if len(sys.argv) > 1 else "jjzs"
    urls = {
        "905": ("http://sk.cri.cn/905.m3u8", "http://sk.cri.cn/"),
        "jjzs": ("http://ngcdn002.cnr.cn/live/jjzs/index.m3u8",
                 "http://ngcdn002.cnr.cn/live/jjzs/"),
        "dszs": ("http://ngcdn002.cnr.cn/live/dszs/index.m3u8",
                 "http://ngcdn002.cnr.cn/live/dszs/"),
    }
    m3u8, root = urls[t]
    txt = g(m3u8).decode()
    seg = [l.strip() for l in txt.splitlines() if l.strip() and not l.startswith("#")][0]
    if not seg.startswith("http"):
        seg = root + seg
    d = g(seg)
    ref_demux(d, t)
