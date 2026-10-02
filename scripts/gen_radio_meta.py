# -*- coding: utf-8 -*-
"""
gen_radio_meta.py  --  从 海燕收音机 备份 JSON 生成 data/radio_meta.bin
让硬件收音机侧栏 (全部/收藏/历史/个人 + 40 地区) 与 make_preview.load_radio()
构建的 44 分类模型 逐站、逐序 一致。
数据布局 (与用户确认的 ~5KB 紧凑方案):
  * 每地区分类的列表直接来自 data/stations.bin (已按 region 分组, 与备份同序)
  * 本文件补 stations.bin 里没有的 3 样: 全部(ALL) 顺序 / 额外电台(user+未匹配) / 收藏,历史
  * 每个 station 用一个全局 sid (0..total-1): 0..bin_total-1 = stations.bin 第 i 条,
    bin_total..total-1 = extra 区第 (i-bin_total) 条
文件格式 (little-endian):
  HEADER(20B): magic "ESP32RDT"(8) | version u8=1 | binTotal u16 | extraCount u16
              | userCount u16 | favCount u16 | histCount u16 | allCount u16 | reserved u8
  SID ORDER (allCount*u16): 全部 的 sid 序列
  FAV (favCount*u16)
  HIST (histCount*u16)
  EXTRA (extraCount * record):
    nameLen u8; name(<=40) | urlLen u16; url(<=300) | regionLen u8; region(<=16)
    | catLen u8; cat(<=20) | freqLen u8; freq(<=16) | r g b (3B) | isUser u8
"""
import json, os, struct, glob

BASE = r'd:\ESP32-S3'
OUT = os.path.join(BASE, 'data', 'radio_meta.bin')

def u16(d, o): return d[o] | (d[o+1] << 8)
def u32(d, o): return d[o] | (d[o+1] << 8) | (d[o+2] << 16) | (d[o+3] << 24)

def hex_to_rgb(h):
    h = (h or '').lstrip('#')
    if len(h) != 6: return (200, 200, 200)
    try: return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16))
    except Exception: return (200, 200, 200)

def main():
    b = open(os.path.join(BASE, 'data', 'stations.bin'), 'rb').read()
    nr = u16(b, 9); ns = u32(b, 11); ro = u32(b, 15)
    region_names = []
    region_first = []
    region_count = []
    for i in range(nr):
        off = ro + i * 28
        region_names.append(b[off:off+16].rstrip(b'\x00').decode('utf-8', 'replace'))
        region_first.append(u32(b, off+16))
        region_count.append(u16(b, off+20))
    bin_recs = []
    for i in range(nr):
        pos = region_first[i]; cnt = region_count[i]
        for _ in range(cnt):
            st_off = pos
            nl = b[pos]; pos += 1
            name = b[pos:pos+nl].decode('utf-8', 'replace'); pos += nl
            ul = u16(b, pos); pos += 2
            url = b[pos:pos+ul].decode('latin-1'); pos += ul
            ridx = b[pos]; pos += 1
            cl = b[pos]; pos += 1
            cat = b[pos:pos+cl].decode('utf-8', 'replace'); pos += cl
            fl = b[pos]; pos += 1
            freq = b[pos:pos+fl].decode('utf-8', 'replace'); pos += fl
            rgb = (b[pos], b[pos+1], b[pos+2]); pos += 3
            bin_recs.append((name, url, region_names[ridx], cat, freq, rgb, st_off))
    bin_total = len(bin_recs)
    nid = {}
    for i, r in enumerate(bin_recs):
        nid[(r[0], r[1])] = i

    d = json.load(open(glob.glob(os.path.join(BASE, '*backup*.json'))[0], encoding='utf-8'))
    def inner(k):
        v = d.get(k, '[]')
        return json.loads(v) if isinstance(v, str) else v
    ch = inner('radio_channels').get('radio', [])
    user_st = inner('radio_user_stations')
    fav = inner('radio_favorites')
    hist = inner('radio_history')
    by_id = {}
    for c in ch: by_id[c.get('id')] = c
    for u in user_st: by_id[u.get('id')] = u
    user_ids = set(u.get('id') for u in user_st)

    extra = []
    extra_by_name = {}
    def sid_of(item, is_user):
        name = item.get('name', ''); url = item.get('url', '')
        key = (name, url)
        if key in nid:
            return nid[key]
        if key not in extra_by_name:
            extra_by_name[key] = bin_total + len(extra)
            extra.append((name, url, item.get('description', '其它'),
                          item.get('category', '其它'), item.get('frequency', ''),
                          hex_to_rgb(item.get('color', '#cccccc')), is_user))
        return extra_by_name[key]

    all_seq = []
    for c in ch:
        all_seq.append(sid_of(c, False))
    for u in user_st:
        all_seq.append(sid_of(u, True))

    def sid_of_id(key, is_user):
        st = by_id.get(key)
        if st is None: return None
        return sid_of(st, is_user)

    fav_ids = []
    for key in fav:
        s = sid_of_id(key, key in user_ids)
        if s is not None: fav_ids.append(s)
    hist_ids = []
    for key in hist:
        s = sid_of_id(key, key in user_ids)
        if s is not None: hist_ids.append(s)

    all_count = len(all_seq)
    user_count = len(user_st)
    blob = b''
    blob += b'ESP32RDT'
    blob += struct.pack('B', 1)
    blob += struct.pack('H', bin_total)
    blob += struct.pack('H', len(extra))
    blob += struct.pack('H', user_count)
    blob += struct.pack('H', len(fav_ids))
    blob += struct.pack('H', len(hist_ids))
    blob += struct.pack('H', all_count)
    blob += struct.pack('B', 0)
    for s in all_seq:
        blob += struct.pack('H', s)
    for s in fav_ids:
        blob += struct.pack('H', s)
    for s in hist_ids:
        blob += struct.pack('H', s)
    for (name, url, region, cat, freq, rgb, is_user) in extra:
        nb = name.encode('utf-8')[:40]; blob += struct.pack('B', len(nb)) + nb
        ub = url.encode('ascii', 'replace')[:300]; blob += struct.pack('H', len(ub)) + ub
        rb = region.encode('utf-8')[:16]; blob += struct.pack('B', len(rb)) + rb
        cb = cat.encode('utf-8')[:20]; blob += struct.pack('B', len(cb)) + cb
        fb = freq.encode('utf-8')[:16]; blob += struct.pack('B', len(fb)) + fb
        blob += struct.pack('BBB', rgb[0], rgb[1], rgb[2])
        blob += struct.pack('B', 1 if is_user else 0)

    open(OUT, 'wb').write(blob)
    print('WROTE %s  (%d bytes)' % (OUT, len(blob)))
    print('  bin_total=%d  extra=%d  user=%d  fav=%d  hist=%d  all=%d'
          % (bin_total, len(extra), user_count, len(fav_ids), len(hist_ids), all_count))
    mx = max(all_seq + fav_ids + hist_ids) if (all_seq or fav_ids or hist_ids) else -1
    print('  max sid=%d  (valid if < %d)' % (mx, bin_total + len(extra)))
    bad = [s for s in fav_ids + hist_ids if s >= bin_total + len(extra)]
    print('  unresolvable fav/hist sids: %d' % len(bad))

if __name__ == '__main__':
    main()
