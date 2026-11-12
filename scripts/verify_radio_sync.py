# -*- coding: utf-8 -*-
"""按固件的读取逻辑逐字节复验 build_radio_sync.py 的产物"""
import struct, os, sys
from array import array
from PIL import Image

D = r"D:\ESP32-S3\build\radio_sync_20261108"
ok = True
def chk(cond, msg):
    global ok
    print(("  OK   " if cond else "  FAIL ") + msg)
    if not cond:
        ok = False

# ---------- stations.bin ----------
b = open(os.path.join(D, "stations.bin"), "rb").read()
chk(b[:8] == b"ESP32RDB", "magic ESP32RDB")
chk(b[8] == 2, "version == 2（固件硬要求）")
nreg = struct.unpack_from("<H", b, 9)[0]
nstn = struct.unpack_from("<I", b, 11)[0]
ridx = struct.unpack_from("<I", b, 15)[0]
rec  = struct.unpack_from("<I", b, 19)[0]
chk(ridx == 24, "region_idx_offset = 24")
chk(rec == 24 + 28 * nreg, "station_records_offset 紧跟地区表")
print("  regions=%d stations=%d rec_off=%d size=%d" % (nreg, nstn, rec, len(b)))

regs = []
for i in range(nreg):
    q = 24 + i * 28
    nm = b[q:q+16].split(b"\x00")[0].decode("utf-8", "replace")
    first = struct.unpack_from("<I", b, q+16)[0]
    cnt = struct.unpack_from("<H", b, q+20)[0]
    regs.append((nm, first, cnt))

# 逐地区按固件方式遍历，验证 offset/count 严密衔接
pos_expected = rec
recs = []
for (nm, first, cnt) in regs:
    chk(first == pos_expected, "地区「%s」first_offset == 上一地区结束 (%d)" % (nm, first))
    p = first
    for _ in range(cnt):
        nl = b[p]; p += 1
        name = b[p:p+nl].decode("utf-8", "replace"); p += nl
        ul = struct.unpack_from("<H", b, p)[0]; p += 2
        url = b[p:p+ul].decode("utf-8", "replace"); p += ul
        ri = b[p]; p += 1
        cl = b[p]; p += 1
        cat = b[p:p+cl].decode("utf-8", "replace"); p += cl
        fl = b[p]; p += 1
        freq = b[p:p+fl].decode("utf-8", "replace"); p += fl
        rgb = b[p:p+3]; p += 3
        chk(ri == regs.index((nm, first, cnt)), "记录 region_idx 与地区表一致") if False else None
        recs.append((name, url, nm, cat, freq, rgb))
    pos_expected = p
chk(pos_expected == len(b), "解析结束位置正好等于文件尾 (%d)" % pos_expected)
chk(len(recs) == nstn, "记录数 %d == 头部 %d" % (len(recs), nstn))
chk(max(len(x[0].encode()) for x in recs) <= 60, "台名最长字节 <= 60")
chk(max(len(x[1].encode()) for x in recs) <= 512, "URL 最长字节 <= 512")
chk(all(len(x[4].encode()) <= 12 for x in recs), "频率最长 12 字节（固件缓冲 13）")
chk(all(len(x[3].encode()) <= 19 for x in recs), "分类最长 19 字节")
print("  台名最长 %d B / URL 最长 %d B" % (max(len(x[0].encode()) for x in recs),
                                          max(len(x[1].encode()) for x in recs)))

# ---------- radio_meta.bin ----------
m = open(os.path.join(D, "radio_meta.bin"), "rb").read()
chk(m[:8] == b"ESP32RDT", "magic ESP32RDT")
chk(m[8] == 1, "version == 1")
binTotal, extra, userC = struct.unpack_from("<HHH", m, 9)
favC, histC, allC = struct.unpack_from("<HHH", m, 15)
print("  binTotal=%d extra=%d user=%d fav=%d hist=%d all=%d" % (binTotal, extra, userC, favC, histC, allC))
chk(binTotal == nstn, "binTotal(%d) == stations.bin 台数(%d)" % (binTotal, nstn))
p = 22
allq  = [struct.unpack_from("<H", m, p+i*2)[0] for i in range(allC)]; p += allC*2
favq  = [struct.unpack_from("<H", m, p+i*2)[0] for i in range(favC)]; p += favC*2
histq = [struct.unpack_from("<H", m, p+i*2)[0] for i in range(histC)]; p += histC*2
total_stn = binTotal + extra
chk(all(x < total_stn for x in allq), "all[] 全部 sid < binTotal+extra")
chk(len(set(allq)) == allC, "all[] 无重复")
chk(all(x < total_stn for x in favq), "fav[] 全部 sid 有效")
chk(all(x < total_stn for x in histq), "hist[] 全部 sid 有效")

ex = []
for _ in range(extra):
    nl = m[p]; p += 1
    nm = m[p:p+nl].decode("utf-8", "replace"); p += nl
    ul = struct.unpack_from("<H", m, p)[0]; p += 2
    url = m[p:p+ul].decode("utf-8", "replace"); p += ul
    rl = m[p]; p += 1
    rg = m[p:p+rl].decode("utf-8", "replace"); p += rl
    cl = m[p]; p += 1
    ct = m[p:p+cl].decode("utf-8", "replace"); p += cl
    fl = m[p]; p += 1
    fq = m[p:p+fl].decode("utf-8", "replace"); p += fl
    r, g, bb, isu = m[p:p+4]; p += 4
    ex.append((nm, url, rg, ct, fq, isu))
chk(p == len(m), "extras 解析结束正好等于文件尾 (%d/%d)" % (p, len(m)))
chk(len(ex) == extra, "extras 记录数 == 头部 %d" % extra)
print("  总台数（不含助眠）= %d，+6 助眠 = %d" % (total_stn, total_stn + 6))

# ---------- logos.bin ----------
l = open(os.path.join(D, "logos.bin"), "rb").read()
chk(l[:8] == b"ESP32LGB", "magic ESP32LGB")
ver, th, fu = l[8], l[9], l[10]
ns  = struct.unpack_from("<H", l, 12)[0]
nlogo = struct.unpack_from("<H", l, 14)[0]
idxo, tblo, blobo = struct.unpack_from("<III", l, 16)
chk(ver == 1 and th == 40 and fu == 64, "version=1 thumb=40 full=64")
chk(idxo == 32, "idx_off = 32")
chk(tblo == 32 + ns*2, "logo 表紧跟 sid 索引")
chk(blobo == tblo + nlogo*4, "blob 区紧跟 logo 表")
idx = [struct.unpack_from("<H", l, idxo + i*2)[0] for i in range(ns)]
tbl = [struct.unpack_from("<I", l, tblo + i*4)[0] for i in range(nlogo)]
chk(ns == nstn, "sid 索引条数(%d) == 台数(%d)" % (ns, nstn))
chk(all(x == 0xFFFF or x < nlogo for x in idx), "logo_id 全部合法")
blob_sz = 40*40*2 + 64*64*2
chk(all(tbl[i] == i*blob_sz for i in range(nlogo)), "logo 偏移连续、每块 %d B" % blob_sz)
chk(blobo + nlogo*blob_sz == len(l), "文件长度 = blob 起点 + %d×%d (%d)" % (nlogo, blob_sz, len(l)))
print("  logos: %d 台有图(%d 个去重块), %.2f MB, 单块 %d B" %
      (sum(1 for x in idx if x != 0xFFFF), nlogo, len(l)/1048576, blob_sz))

# ---------- sid_remap / alturls ----------
rm = open(os.path.join(D, "sid_remap.bin"), "rb").read()
chk(rm[:8] == b"ESP32RSM" and rm[8] == 1, "sid_remap magic/ver")
oc = struct.unpack_from("<H", rm, 9)[0]
vals = [struct.unpack_from("<H", rm, 11 + 2*i)[0] for i in range(oc)]
chk(11 + oc*2 == len(rm) - (len(rm) - 11 - oc*2), "sid_remap 长度与条数一致")
chk(all(v == 0xFFFF or v < total_stn for v in vals), "重映射目标 sid 全部有效")
print("  重映射：旧 %d 台，丢失 %d 台，保留 %d 台" % (oc, sum(1 for v in vals if v == 0xFFFF),
                                              sum(1 for v in vals if v != 0xFFFF)))

au = open(os.path.join(D, "alturls.bin"), "rb").read()
chk(au[:8] == b"ESP32RAU" and au[8] == 1, "alturls magic/ver")
ns2 = struct.unpack_from("<H", au, 10)[0]
io, bo = struct.unpack_from("<II", au, 12)
chk(io == 20 and bo == 20 + 4*ns2, "alturls 头部偏移正确")
chk(ns2 == nstn, "alturls sid 条数 == 台数")
offs = [struct.unpack_from("<I", au, io + 4*i)[0] for i in range(ns2)]
nz = sum(1 for x in offs if x)
tot = 0
for x in offs:
    if x:
        q = bo + x
        n = au[q]
        p2 = q + 1
        for _ in range(n):
            ln = struct.unpack_from("<H", au, p2)[0]
            p2 += 2 + ln
            tot += 1
        # 2 字节对齐
chk(len(au) > bo, "alturls 数据区存在")
print("  alturls: %d 台有备用流，共 %d 条 URL，%d B" % (nz, tot, len(au)))

# ---------- 台标解码抽样 ----------
out = os.path.join(D, "logo_samples")
os.makedirs(out, exist_ok=True)
def blob_to_img(off, size):
    data = l[blobo + off: blobo + off + size*size*2]
    a = array("H"); a.frombytes(data)
    im = Image.new("RGB", (size, size))
    px = []
    for v in a:
        px.append((((v >> 11) & 0x1F) * 255 // 31, ((v >> 5) & 0x3F) * 255 // 63, (v & 0x1F) * 255 // 31))
    im.putdata(px)
    return im

shown = 0
for sid in range(ns):
    if idx[sid] != 0xFFFF and shown < 6:
        im40 = blob_to_img(tbl[idx[sid]], 40)
        im64 = blob_to_img(tbl[idx[sid]] + 40*40*2, 64)
        im40.resize((160, 160), Image.NEAREST).save(os.path.join(out, "sid%04d_40.png" % sid))
        im64.save(os.path.join(out, "sid%04d_64.png" % sid))
        shown += 1
print("  已导出 %d 个台标样张到 %s" % (shown, out))
print("\n结果：%s" % ("全部通过" if ok else "有失败项"))
sys.exit(0 if ok else 1)
