# -*- coding: utf-8 -*-
"""
verify_radio_lockstep.py
Confirm the planned firmware model (stations.bin + radio_meta.bin) matches
make_preview.load_radio() exactly (44 cats, same station order + display).
This is the "verify in preview first" gate before editing main.cpp.
"""
import os, sys, io, glob
BASE = r'd:\ESP32-S3'
sys.path.insert(0, os.path.join(BASE, 'preview'))
out = io.StringIO()
def p(*a): print(*a, file=out)

def u16(d, o): return d[o] | (d[o+1] << 8)
def u32(d, o): return d[o] | (d[o+1] << 8) | (d[o+2] << 16) | (d[o+3] << 24)

b = open(os.path.join(BASE, 'data', 'stations.bin'), 'rb').read()
nr = u16(b, 9); ro = u32(b, 15)
region_first = [u32(b, ro+i*28+16) for i in range(nr)]
region_count = [u16(b, ro+i*28+20) for i in range(nr)]
bin_recs = []
for i in range(nr):
    pos = region_first[i]; cnt = region_count[i]
    for _ in range(cnt):
        nl = b[pos]; pos += 1
        name = b[pos:pos+nl].decode('utf-8', 'replace'); pos += nl
        ul = u16(b, pos); pos += 2; pos += ul
        ridx = b[pos]; pos += 1
        cl = b[pos]; pos += 1
        cat = b[pos:pos+cl].decode('utf-8', 'replace'); pos += cl
        fl = b[pos]; pos += 1
        freq = b[pos:pos+fl].decode('utf-8', 'replace'); pos += fl
        rgb = (b[pos], b[pos+1], b[pos+2]); pos += 3
        bin_recs.append((name, freq, i, cat, rgb))
bin_total = len(bin_recs)
rnames = [b[ro+i*28:ro+i*28+16].rstrip(b'\x00').decode('utf-8','replace') for i in range(nr)]

m = open(os.path.join(BASE, 'data', 'radio_meta.bin'), 'rb').read()
assert m[0:8] == b'ESP32RDT'
binTotal = u16(m, 9); extraCount = u16(m, 11); userCount = u16(m, 13)
favCount = u16(m, 15); histCount = u16(m, 17); allCount = u16(m, 19)
off = 22   # header = 8(magic)+1(ver)+2*6(counts)+1(reserved) = 22
all_seq = [u16(m, off+i*2) for i in range(allCount)]; off += allCount*2
fav_ids = [u16(m, off+i*2) for i in range(favCount)]; off += favCount*2
hist_ids = [u16(m, off+i*2) for i in range(histCount)]; off += histCount*2
extra = []
for _ in range(extraCount):
    nl = m[off]; off += 1; name = m[off:off+nl].decode('utf-8','replace'); off += nl
    ul = u16(m, off); off += 2; off += ul
    rl = m[off]; off += 1; region = m[off:off+rl].decode('utf-8','replace'); off += rl
    cl = m[off]; off += 1; cat = m[off:off+cl].decode('utf-8','replace'); off += cl
    fl = m[off]; off += 1; freq = m[off:off+fl].decode('utf-8','replace'); off += fl
    rgb = (m[off], m[off+1], m[off+2]); off += 3
    is_user = m[off]; off += 1
    extra.append((name, freq, region, cat, rgb, is_user))

def resolve(sid):
    if sid < bin_total:
        r = bin_recs[sid]
        return (r[0], r[1], rnames[r[2]], r[3])
    else:
        e = extra[sid - bin_total]
        return (e[0], e[1], e[2], e[3])

RADIO_REGION_HEAD = ['全国','中央','电视伴音','国际']
RADIO_PINNED = '广东'
RADIO_REGION_ORDER = ['山东','浙江','河北','江苏','河南','吉林','四川','安徽','山西','辽宁',
  '湖北','云南','新疆','黑龙江','湖南','江西','广西','内蒙古','贵州','福建','北京','陕西',
  '青海','上海','香港','重庆','宁夏','甘肃','台湾','海南','西藏','天津','澳门']
RADIO_REGION_TAIL = ['海外','其它']
LBL = {'电视伴音': '伴音'}
def label(k): return LBL.get(k, k)

def hw_trunc(name):
    bb = (name or '').encode('utf-8')
    if len(bb) <= 31: return name
    return bb[:28].decode('utf-8', 'ignore') + '...'

# apply hardware-load truncation to bin names (mimic loadRegionStations char[32])
bin_recs = [(hw_trunc(r[0]), r[1], r[2], r[3], r[4]) for r in bin_recs]
# extra names also truncated (firmware extra load uses same char[32] field)
extra = [(hw_trunc(e[0]), e[1], e[2], e[3], e[4], e[5]) for e in extra]

def resolve(sid):
    if sid < bin_total:
        r = bin_recs[sid]
        return (r[0], r[1], rnames[r[2]], r[3])
    else:
        e = extra[sid - bin_total]
        return (e[0], e[1], e[2], e[3])

# region list = all_seq[0:bin_total] (the backup channels, in backup order) filtered by region.
# This matches make_preview.region_cats exactly (backup order) and naturally:
#   - includes the 2 extra 中央 (they're in 全部)  -> 中央=32
#   - excludes the 2 bin-only 其它 (not in 全部)   -> 其它=0
def canonregion(key):
    out = []
    for s in all_seq[:bin_total]:
        st = resolve(s)
        if st[2] == key:
            out.append(st)
    return out

fw = []
def add(label_, key, seq): fw.append((label_, key, seq))
add('全部', '全部', [resolve(s) for s in all_seq])
add('收藏', '收藏', [resolve(s) for s in fav_ids])
add('历史', '历史', [resolve(s) for s in hist_ids])
add('个人', '个人', [resolve(bin_total+i) for i in range(extraCount) if extra[i][5]])
for key in RADIO_REGION_HEAD:
    add(label(key), key, canonregion(key))
if RADIO_PINNED not in RADIO_REGION_HEAD:
    add(label(RADIO_PINNED), RADIO_PINNED, canonregion(RADIO_PINNED))
for key in RADIO_REGION_ORDER:
    if key == RADIO_PINNED: continue
    add(label(key), key, canonregion(key))
for key in RADIO_REGION_TAIL:
    add(label(key), key, canonregion(key))

import make_preview as mp
pm = mp.load_radio()
pcats = pm['cats']
p('FW cats=%d  preview cats=%d' % (len(fw), len(pcats)))
mismatch_cats = 0
for i in range(min(len(fw), len(pcats))):
    fl, fk, fseq = fw[i]
    pl, pk, pseq = pcats[i]['label'], pcats[i]['key'], pcats[i]['stations']
    pnames = [s.name for s in pseq]
    fnames = [s[0] for s in fseq]
    ok = (fl == pl) and (fk == pk) and (fnames == pnames)
    if not ok:
        mismatch_cats += 1
        p('  CAT %d MISMATCH: fw %r/%r n=%d | pv %r/%r n=%d' %
          (i, fl, fk, len(fnames), pl, pk, len(pnames)))
        if fl == pl and fk == pk:
            for k in range(min(len(fnames), len(pnames))):
                if fnames[k] != pnames[k]:
                    p('      first station diff at %d: fw=%r pv=%r' % (k, fnames[k], pnames[k])); break
            if len(fnames) != len(pnames):
                p('      length diff: fw=%d pv=%d' % (len(fnames), len(pnames)))
    else:
        p('  CAT %d OK: %s (%d)' % (i, pl, len(fnames)))
p('TOTAL cat mismatches: %d / %d' % (mismatch_cats, min(len(fw), len(pcats))))
p('\n=== extra entries (name | region) ===')
for i, e in enumerate(extra):
    p('  extra[%d] is_user=%d region=%r name=%r' % (i, e[5], e[2], e[0][:40]))
p('=== bin 其它 stations ===')
for i in range(bin_total):
    if rnames[bin_recs[i][2]] == '其它':
        p('  bin 其它[%d] sid=%d name=%r' % (i, i, bin_recs[i][0][:40]))
open(os.path.join(BASE, '_lockstep.txt'), 'w', encoding='utf-8').write(out.getvalue())
print('WROTE _lockstep.txt  mismatches=%d' % mismatch_cats)
