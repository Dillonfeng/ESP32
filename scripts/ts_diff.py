# -*- coding: utf-8 -*-
# Download live HLS segments from a WORKING station (环球资讯/CRI 905) and two
# FAILING stations (经济之声, 经典音乐广播) and structurally compare their TS:
#   - PUSI / pointer_field / AFC usage
#   - PES header: stream_id, PES_packet_length (esp. ==0), header_data_length
#   - packets per PES, PES header crossing packet boundary
#   - extracted AAC ES: ADTS frame sync, frame length / rate / channels continuity
# Mirrors the firmware ts_parsePacket() logic so any case it mishandles is visible.
import urllib.request, sys, io, struct

def http_get(url, timeout=10, maxbytes=None):
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        data = r.read() if maxbytes is None else r.read(maxbytes)
    return data

def get_segments(m3u8_url, n=2):
    base = m3u8_url
    # strip query for base join
    root = m3u8_url.rsplit("/", 1)[0] + "/"
    txt = http_get(m3u8_url).decode("utf-8", "replace")
    media_seq = None
    segs = []
    for line in txt.splitlines():
        line = line.strip()
        if line.startswith("#EXT-X-MEDIA-SEQUENCE:"):
            media_seq = int(line.split(":", 1)[1])
        elif line and not line.startswith("#"):
            if line.startswith("http"):
                segs.append(line)
            elif line.startswith("/"):
                # find scheme://host
                from urllib.parse import urlparse
                p = urlparse(m3u8_url)
                segs.append("%s://%s%s" % (p.scheme, p.netloc, line))
            else:
                segs.append(root + line)
    return txt, media_seq, segs[:n]

def parse_ts(data, label):
    print("=" * 70)
    print(label, " segment bytes:", len(data))
    pkts = [data[i:i+188] for i in range(0, len(data) - 187, 188)]
    n_bad_sync = sum(1 for p in pkts if p[0] != 0x47)
    n_pusi = n_afc01 = n_afc11 = n_afc10 = 0
    n_audio_pid_pusi = 0
    pes_zero_len = 0
    pes_nonzero = 0
    pointer_vals = {}
    pkts_per_pes = []
    pes_since_last = 0
    pes_headers_cross = 0
    audio_pid = None
    # ES accumulation by firmware-style logic
    es = bytearray()
    pids_seen = {}
    # firmware-style state
    f_pes_len = 0
    f_fill = 0
    f_pid = 0
    f_first = True
    firmware_glitch_bytes = 0  # bytes that firmware would mislabel as raw ADTS
    raw_fallback = 0

    for pi, p in enumerate(pkts):
        if p[0] != 0x47:
            continue
        pid = ((p[1] & 0x1F) << 8) | p[2]
        pusi = (p[1] & 0x40) >> 6
        afc = (p[3] & 0x30) >> 4
        afl = p[4] if (afc & 0x2) == 0x2 else -1
        pls = 4 if (afc & 0x2) != 0x2 else 5 + max(afl, 0)
        pids_seen[pid] = pids_seen.get(pid, 0) + 1
        if pusi: n_pusi += 1
        if afc == 1: n_afc01 += 1
        if afc == 3: n_afc11 += 1
        if afc == 2: n_afc10 += 1

        # ---- firmware-style auto-discovery (first PUSI only), HARDCODED pos ----
        if pusi and pid > 0 and f_pid == 0:
            pos = 4 if afl < 0 else 5 + afl
            is_audio = False
            if pos + 4 <= 188 and p[pos] == 0 and p[pos+1] == 0 and p[pos+2] == 1:
                sid = p[pos+3]
                if 0xC0 <= sid <= 0xDF:
                    is_audio = True
            if not is_audio:
                # firmware ADTS scan (omitted detail, scan small window)
                for i2 in range(pos, min(pos + 8, 183)):
                    if p[i2] == 0xFF and (p[i2+1] & 0xE0) == 0xE0:
                        is_audio = True
                        break
            if is_audio:
                f_pid = pid
                audio_pid = pid
                # firmware HARDCODED PES parse (NO pointer_field scan):
                if p[pos] == 0 and p[pos+1] == 0 and p[pos+2] == 1:
                    sid = p[pos+3]
                    if 0xC0 <= sid <= 0xDF:
                        hdl = p[pos+8]
                        ppl = (p[pos+4] << 8) | p[pos+5]
                        f_pes_len = 0x7FFFFFFF if ppl == 0 else ppl
                        sod = hdl + 9
                        if pos + sod >= 188:
                            f_pes_len -= (hdl + 3)
                        else:
                            plen = 188 - pos - sod
                            es += p[pos+sod:pos+sod+plen]
                            f_pes_len -= plen
                            f_pes_len -= (hdl + 3)
                    # NOTE firmware bug case: if start code NOT at hardcoded pos:
                else:
                    # raw ADTS fallback: outputs pointer_field+PES header as data
                    es += p[pos:188]
                    raw_fallback += 188 - pos

        # ---- firmware main AAC branch ----
        elif pid == f_pid and f_pid != 0:
            pos = 4 if afl < 0 else 5 + afl
            # correct PES analysis for statistics:
            if pusi:
                pes_since_last += 1
                pkts_per_pes.append(pes_since_last)
                pes_since_last = 0
                # pointer_field lives at payload start when PUSI for PES PID
                pf = p[pls]
                pointer_vals[pf] = pointer_vals.get(pf, 0) + 1
                # find PES start code (correct logic)
                ps = -1
                for i2 in range(pls, 184):
                    if i2+3 < 188 and p[i2] == 0 and p[i2+1] == 0 and p[i2+2] == 1 and 0xC0 <= p[i2+3] <= 0xDF:
                        ps = i2
                        break
                if ps >= 0:
                    if ps + 9 > 188:
                        pes_headers_cross += 1
                    else:
                        ppl = (p[ps+4] << 8) | p[ps+5]
                        if ppl == 0: pes_zero_len += 1
                        else: pes_nonzero += 1
                # firmware state: close old PES
                f_pes_len = 0
                f_fill = 0
            else:
                pes_since_last += 1

            # firmware PES continuation / parse
            if f_pes_len > 0:
                if pos + f_fill >= 188:
                    f_fill = 0
                else:
                    start = pos + f_fill
                    plen = 188 - pos - f_fill
                    f_fill = 0
                    es += p[start:start+plen]
                    f_pes_len -= plen
            else:
                ps = -1
                if pusi:
                    for i2 in range(pos, 184):
                        if i2+3 < 188 and p[i2] == 0 and p[i2+1] == 0 and p[i2+2] == 1 and 0xC0 <= p[i2+3] <= 0xDF:
                            ps = i2
                            break
                if ps < 0:
                    pass  # dropped
                elif ps + 9 > 188:
                    pes_headers_cross += 1
                else:
                    hdl = p[ps+8]
                    ppl = (p[ps+4] << 8) | p[ps+5]
                    f_pes_len = 0x7FFFFFFF if ppl == 0 else ppl
                    sod = hdl + 9
                    if ps + sod >= 188:
                        f_pes_len -= (hdl + 3)
                        f_fill = (ps + sod) - 188
                    else:
                        plen = 188 - ps - sod
                        es += p[ps+sod:ps+sod+plen]
                        f_pes_len -= plen
                        f_pes_len -= (hdl + 3)
        else:
            pes_since_last += 1

    print("packets=%d  bad_sync=%d  PUSI=%d  AFC: 01-only=%d 11=%d 10-only=%d" %
          (len(pkts), n_bad_sync, n_pusi, n_afc01, n_afc11, n_afc10))
    print("PIDs seen (top 6):", sorted(pids_seen.items(), key=lambda x: -x[1])[:6])
    print("audio_pid=%s  PES pkt_len: zero=%d nonzero=%d  headers_cross_pkt=%d" %
          (hex(audio_pid) if audio_pid else None, pes_zero_len, pes_nonzero, pes_headers_cross))
    print("pointer_field values at PUSI:", dict(sorted(pointer_vals.items())))
    if pkts_per_pes:
        from collections import Counter
        c = Counter(pkts_per_pes)
        print("TS packets per PES: min=%d max=%d dist=%s" %
              (min(pkts_per_pes), max(pkts_per_pes), dict(sorted(c.items()))))
    print("firmware raw-ADTS-fallback bytes (pointer+PES header leaked):", raw_fallback)

    # ---- ADTS scan on firmware-extracted ES ----
    adts = []
    i = 0
    while i + 7 <= len(es):
        if es[i] == 0xFF and (es[i+1] & 0xF0) == 0xF0:
            idv = (es[i+1] >> 3) & 1
            layer = (es[i+1] >> 1) & 3
            sr_idx = (es[i+2] >> 2) & 0xF
            ch = ((es[i+2] & 1) << 2) | ((es[i+3] >> 6) & 3)
            flen = ((es[i+2] & 0x3) << 11) | (es[i+3] << 3) | ((es[i+4] >> 5) & 0x7)
            frames = (es[i+6] & 3) + 1
            if layer == 0 and sr_idx < 13 and 0 < ch < 8 and flen >= 7:
                adts.append((i, sr_idx, ch, flen))
                i += flen
                continue
        i += 1
    print("ES bytes=%d  valid ADTS frames=%d" % (len(es), len(adts)))
    if adts:
        sr = set(a[1] for a in adts)
        chs = set(a[2] for a in adts)
        flens = [a[3] for a in adts]
        print("  sr_idx=%s channels=%s frameLen min/max=%d/%d" %
              (sr, chs, min(flens), max(flens)))
        # gaps between frames = mis-extracted bytes
        gaps = []
        for k in range(1, len(adts)):
            expect = adts[k-1][0] + adts[k-1][3]
            if adts[k][0] != expect:
                gaps.append(adts[k][0] - expect)
        print("  frame-boundary gaps (misplaced bytes): count=%d total=%d sample=%s" %
              (len(gaps), sum(gaps), gaps[:12]))
    return es, adts

if __name__ == "__main__":
    targets = [
        ("环球资讯 [WORKS]", "http://sk.cri.cn/905.m3u8"),
        ("经济之声 [FAILS]", "http://ngcdn002.cnr.cn/live/jjzs/index.m3u8"),
        ("经典音乐广播 [FAILS]", "http://ngcdn002.cnr.cn/live/dszs/index.m3u8"),
    ]
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    for label, url in targets:
        if which != "all" and which not in label:
            continue
        try:
            txt, mseq, segs = get_segments(url, 2)
            print("# %s  media_seq=%s  segments found=%d" % (label, mseq, len(segs)))
            print("# m3u8 first 400 chars:\n", txt[:400].replace("\n", "\n# "))
            if segs:
                data = http_get(segs[0])
                parse_ts(data, label)
        except Exception as e:
            import traceback
            print("ERROR for %s: %r" % (label, e))
            traceback.print_exc()
