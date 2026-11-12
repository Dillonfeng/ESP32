# -*- coding: utf-8 -*-
"""
电台数据 PC 端同步打包器（2026-11-08）

输入（只读）：
  D:\\Trae Work\\RetroRadioDesktop\\mobile\\www\\channels.js     移动端电台表（radio 2412 条）
  D:\\Trae Work\\RetroRadioDesktop\\mobile\\www\\logos\\           台标原图（PNG/JPG）
  D:\\ESP32-S3\\data\\stations.bin                                旧电台表（用于 sid 重映射）
  D:\\ESP32-S3\\data\\radio_meta.bin                              旧 meta（用于保留 18 条用户台 + fav/hist）

输出（默认 D:\\ESP32-S3\\build\\radio_sync_20261108\\）：
  stations.bin      电台表，**保持 v2 格式**（固件无需改解析代码）
  radio_meta.bin    meta，v1 格式（binTotal 更新 + all[] 重生 + extras 原样保留 + fav/hist 重映射）
  logos.bin         台标包（40x40 列表档 + 64x64 全屏档，RGB565 裸数据，放 SD 根目录）
  sid_remap.bin     旧 sid -> 新 sid（0xFFFF = 该台已不存在），固件一次性迁移 NVS 收藏/历史用
  alturls.bin       729 台的多源备份流（放 SD，固件按需读，不常驻 RAM）
  report.md         本次生成的全部数字
  preview_logos_check.png  台标两档渲染 + 列表行版面模拟（烧机前肉眼核验）

用法：
  python scripts/build_radio_sync.py [输出目录]
"""

import json, os, re, struct, sys, hashlib, collections

MOBILE   = r"D:\Trae Work\RetroRadioDesktop\mobile\www\channels.js"
LOGOS_DIR= r"D:\Trae Work\RetroRadioDesktop\mobile\www\logos"
OLD_STN  = r"D:\ESP32-S3\data\stations.bin"
OLD_META = r"D:\ESP32-S3\data\radio_meta.bin"
OUT_DIR  = sys.argv[1] if len(sys.argv) > 1 else r"D:\ESP32-S3\build\radio_sync_20261108"

THUMB = 40
FULL  = 64

# ---------------- 基础工具 ----------------
def rd(p):
    with open(p, "rb") as f:
        return f.read()

def wr(p, data):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(data)

def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

def hex_rgb(h, dflt=(200, 200, 200)):
    h = (h or "").lstrip("#")
    if len(h) != 6:
        return dflt
    try:
        return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16))
    except ValueError:
        return dflt

def cut_bytes(s, max_len):
    """按 UTF-8 边界截断到 <= max_len 字节"""
    b = s.encode("utf-8")
    if len(b) <= max_len:
        return b
    b = b[:max_len]
    while b and (b[-1] & 0xC0) == 0x80:
        b = b[:-1]
    if b and (b[-1] & 0x80):          # 落在多字节首字节上，去掉
        b = b[:-1]
    return b

# ---------------- 读入 ----------------
s = open(MOBILE, encoding="utf-8").read()
data = json.loads(s[s.index("{"): s.rindex("}") + 1])
radio = data["radio"]
print("[读入] 移动端电台 %d 条" % len(radio))

# 旧 stations.bin（按固件同样的方式解析）
ob = rd(OLD_STN)
assert ob[:8] == b"ESP32RDB" and ob[8] == 2
o_nreg  = struct.unpack_from("<H", ob, 9)[0]
o_nstn  = struct.unpack_from("<I", ob, 11)[0]
o_rec   = struct.unpack_from("<I", ob, 19)[0]
o_regs  = []
for i in range(o_nreg):
    q = 24 + i * 28
    o_regs.append(ob[q:q + 16].split(b"\x00")[0].decode("utf-8", "replace"))
pos = o_rec
old_bin = []
for _ in range(o_nstn):
    nl = ob[pos]; pos += 1
    nm = ob[pos:pos + nl].decode("utf-8", "replace"); pos += nl
    ul = struct.unpack_from("<H", ob, pos)[0]; pos += 2
    url = ob[pos:pos + ul].decode("utf-8", "replace"); pos += ul
    pos += 1                              # region_idx
    ridx = ob[pos - 1]
    cl = ob[pos]; pos += 1
    cat = ob[pos:pos + cl].decode("utf-8", "replace"); pos += cl
    fl = ob[pos]; pos += 1
    freq = ob[pos:pos + fl].decode("utf-8", "replace"); pos += fl
    rr, gg, bbb = ob[pos], ob[pos + 1], ob[pos + 2]
    pos += 3                              # rgb
    old_bin.append(dict(name=nm, url=url, cat=cat, freq=freq,
                        region=o_regs[ridx] if ridx < len(o_regs) else "",
                        rgb=(rr, gg, bbb)))
assert pos == len(ob), "旧 stations.bin 解析未到文件尾 (%d/%d)" % (pos, len(ob))
print("[读入] 旧 stations.bin %d 台 / %d 地区" % (o_nstn, o_nreg))

# 旧 radio_meta（含 18 条用户台 + fav/hist）
om = rd(OLD_META)
assert om[:8] == b"ESP32RDT" and om[8] == 1
o_binTotal, o_extra, o_user = struct.unpack_from("<HHH", om, 9)
o_favCnt, o_histCnt, o_allCnt = struct.unpack_from("<HHH", om, 15)
p = 22
o_all  = [struct.unpack_from("<H", om, p + i * 2)[0] for i in range(o_allCnt)];  p += o_allCnt * 2
o_fav  = [struct.unpack_from("<H", om, p + i * 2)[0] for i in range(o_favCnt)];  p += o_favCnt * 2
o_hist = [struct.unpack_from("<H", om, p + i * 2)[0] for i in range(o_histCnt)]; p += o_histCnt * 2
extras = []
for _ in range(o_extra):
    nl = om[p]; p += 1
    nm = om[p:p + nl].decode("utf-8", "replace"); p += nl
    ul = struct.unpack_from("<H", om, p)[0]; p += 2
    url = om[p:p + ul].decode("utf-8", "replace"); p += ul
    rl = om[p]; p += 1
    rg = om[p:p + rl].decode("utf-8", "replace"); p += rl
    cl = om[p]; p += 1
    ct = om[p:p + cl].decode("utf-8", "replace"); p += cl
    fl = om[p]; p += 1
    fq = om[p:p + fl].decode("utf-8", "replace"); p += fl
    r, g, bb, isu = om[p:p + 4]; p += 4
    extras.append(dict(name=nm, url=url, region=rg, cat=ct, freq=fq, rgb=(r, g, bb), user=isu))
print("[读入] 旧 meta: binTotal=%d extras=%d fav=%d hist=%d all=%d" % (o_binTotal, o_extra, o_favCnt, o_histCnt, o_allCnt))

# ---------------- 地区 / 排序 ----------------
from pypinyin import lazy_pinyin

def pykey(name):
    return "".join(lazy_pinyin(name)).lower()

# 地区顺序：沿用旧 build 的做法 = 首次出现顺序（旧数据的第一个是"电视伴音"，与现网一致）
regions, region_map = [], {}
for it in radio:
    d = (it.get("description") or "其它").strip()
    if d not in region_map:
        region_map[d] = len(regions)
        regions.append(d)
print("[地区] %d 个：%s" % (len(regions), "、".join(regions)))

for rn in regions:
    if len(rn.encode("utf-8")) > 15:
        raise SystemExit("地区名超过 15 字节（region 索引只有 16 字节）：%r" % rn)

# 每个地区内按拼音排序（对应移动端 localeCompare zh-CN），保证设备与 App 观感一致
groups = collections.defaultdict(list)
for it in radio:
    groups[(it.get("description") or "其它").strip()].append(it)
for k in groups:
    groups[k].sort(key=lambda it: pykey(it.get("name", "")))

ordered = []
for rn in regions:
    ordered.extend(groups[rn])
assert len(ordered) == len(radio)

# ---------------- 生成 stations.bin (v2) ----------------
records, sid_logo_file = [], []
for it in ordered:
    rn = (it.get("description") or "其它").strip()
    name_b = cut_bytes(it.get("name", ""), 60)
    url_b  = cut_bytes(it.get("url", ""), 512)
    cat_b  = cut_bytes(it.get("category", ""), 19)
    freq_b = cut_bytes(it.get("frequency", ""), 12)
    r, g, b = hex_rgb(it.get("color"))
    blob = (bytes([len(name_b)]) + name_b
            + struct.pack("<H", len(url_b)) + url_b
            + bytes([region_map[rn]])
            + bytes([len(cat_b)]) + cat_b
            + bytes([len(freq_b)]) + freq_b
            + bytes([r, g, b]))
    records.append(blob)
    sid_logo_file.append(it.get("logo") or None)

NREG = len(regions)
rec_off = 24 + 28 * NREG
first_off, counts = [None] * NREG, [0] * NREG
cur = rec_off
sids = []
for k, blob in enumerate(records):
    ri = region_map[(ordered[k].get("description") or "其它").strip()]
    if first_off[ri] is None:
        first_off[ri] = cur
    counts[ri] += 1
    sids.append(cur)
    cur += len(blob)

buf = bytearray()
buf += b"ESP32RDB" + bytes([2])
buf += struct.pack("<H", NREG)
buf += struct.pack("<I", len(records))
buf += struct.pack("<I", 24)
buf += struct.pack("<I", rec_off)
buf += b"\x00"
for i, rn in enumerate(regions):
    buf += cut_bytes(rn, 15).ljust(16, b"\x00")
    buf += struct.pack("<I", first_off[i] if first_off[i] is not None else rec_off)
    buf += struct.pack("<H", counts[i])
    buf += b"\x00" * 6
for blob in records:
    buf += blob

stn_bytes = bytes(buf)
assert stn_bytes[:8] == b"ESP32RDB" and stn_bytes[8] == 2
wr(os.path.join(OUT_DIR, "stations.bin"), stn_bytes)
print("[写出] stations.bin %d 台 / %d 地区 / %d B (%.1f KB)" % (len(records), NREG, len(stn_bytes), len(stn_bytes) / 1024))

# ---------------- all[]（全国列表顺序 = 拼音序） ----------------
all_seq = sorted(range(len(ordered)), key=lambda i: pykey(ordered[i].get("name", "")))

# ---------------- sid 重映射（旧 -> 新） ----------------
new_by_url  = collections.defaultdict(list)
new_by_name = collections.defaultdict(list)
for i, it in enumerate(ordered):
    new_by_url[it.get("url", "").strip()].append(i)
    new_by_name[it.get("name", "").strip()].append(i)

def norm(u):
    u = (u or "").strip()
    u = re.sub(r"^http://", "", u, flags=re.I)
    return u.rstrip("/")

new_by_nurl = collections.defaultdict(list)
for i, it in enumerate(ordered):
    new_by_nurl[norm(it.get("url", ""))].append(i)

remap, matched_by = [], {"url": 0, "name": 0, "nurl": 0, "lost": 0}
for old in old_bin:
    cand = None
    lst = new_by_url.get(old["url"].strip())
    if lst and len(lst) == 1:
        cand = lst[0]; matched_by["url"] += 1
    elif lst:
        for i in lst:
            if ordered[i].get("name", "").strip() == old["name"].strip():
                cand = i; matched_by["url"] += 1
                break
    if cand is None:
        lst = new_by_name.get(old["name"].strip())
        if lst and len(lst) == 1:
            cand = lst[0]; matched_by["name"] += 1
        elif lst:
            n2 = [i for i in lst if norm(ordered[i].get("url", "")) == norm(old["url"])]
            if len(n2) == 1:
                cand = n2[0]; matched_by["name"] += 1
    if cand is None:
        lst = new_by_nurl.get(norm(old["url"]))
        if lst and len(lst) == 1:
            cand = lst[0]; matched_by["nurl"] += 1
    if cand is None:
        matched_by["lost"] += 1
    remap.append(0xFFFF if cand is None else cand)

NEW_BIN_TOTAL = len(records)
for e in range(o_extra):                       # 用户台整段平移到新 binTotal 之后，一一对应
    remap.append(NEW_BIN_TOTAL + e)

# 收藏/历史里指向"新表已不存在的台"时，把它作为孤儿台追加到 extras，避免用户数据丢失
extra_records = [dict(name=ex["name"], url=ex["url"], region=ex["region"], cat=ex["cat"],
                      freq=ex["freq"], rgb=ex["rgb"], user=ex["user"]) for ex in extras]
orphan_keep = set()
for x in list(o_fav) + list(o_hist):
    if x < o_nstn and remap[x] == 0xFFFF and x not in orphan_keep:
        orphan_keep.add(x)
        old = old_bin[x]
        remap[x] = NEW_BIN_TOTAL + len(extra_records)
        extra_records.append(dict(name=old["name"], url=old["url"], region=old["region"],
                                  cat=old["cat"], freq=old["freq"], rgb=old["rgb"], user=0))
if orphan_keep:
    print("[孤儿台] 为保住收藏/历史，追加 %d 台到 extras（sid %d..%d）"
          % (len(orphan_keep), NEW_BIN_TOTAL + o_extra, NEW_BIN_TOTAL + len(extra_records) - 1))

OLD_TOTAL = len(remap)
def remap_sid(x):
    return remap[x] if x < OLD_TOTAL else 0xFFFF

fav_new  = [remap_sid(x) for x in o_fav  if remap_sid(x) != 0xFFFF]
hist_new = [remap_sid(x) for x in o_hist if remap_sid(x) != 0xFFFF]
print("[重映射] 旧 %d 台：URL 命中 %d / 台名唯一 %d / 归一 URL %d / 丢弃 %d"
      % (o_nstn, matched_by["url"], matched_by["name"], matched_by["nurl"], matched_by["lost"]))
print("[重映射] fav %d->%d，hist %d->%d" % (len(o_fav), len(fav_new), len(o_hist), len(hist_new)))

# ---------------- radio_meta.bin (v1) ----------------
mbuf = bytearray()
mbuf += b"ESP32RDT" + bytes([1])
mbuf += struct.pack("<H", NEW_BIN_TOTAL)
mbuf += struct.pack("<H", len(extra_records))  # extraCount（用户台 + 为保收藏而留的孤儿台）
mbuf += struct.pack("<HHH", o_user, len(fav_new), len(hist_new))
mbuf += struct.pack("<H", len(all_seq))
mbuf += b"\x00"
for i in all_seq:  mbuf += struct.pack("<H", i)
for x in fav_new:  mbuf += struct.pack("<H", x)
for x in hist_new: mbuf += struct.pack("<H", x)
for ex in extra_records:
    nb = cut_bytes(ex["name"], 60)
    ub = cut_bytes(ex["url"], 512)
    rb = cut_bytes(ex["region"], 17)
    cb = cut_bytes(ex["cat"], 12)
    fb = cut_bytes(ex["freq"], 12)
    mbuf += bytes([len(nb)]) + nb
    mbuf += struct.pack("<H", len(ub)) + ub
    mbuf += bytes([len(rb)]) + rb
    mbuf += bytes([len(cb)]) + cb
    mbuf += bytes([len(fb)]) + fb
    mbuf += bytes([ex["rgb"][0], ex["rgb"][1], ex["rgb"][2], ex["user"]])
meta_bytes = bytes(mbuf)
wr(os.path.join(OUT_DIR, "radio_meta.bin"), meta_bytes)
print("[写出] radio_meta.bin %d B (binTotal=%d extras=%d fav=%d hist=%d all=%d)"
      % (len(meta_bytes), NEW_BIN_TOTAL, len(extra_records), len(fav_new), len(hist_new), len(all_seq)))

# ---------------- sid_remap.bin ----------------
rb2 = b"ESP32RSM" + bytes([1]) + struct.pack("<H", OLD_TOTAL)
for x in remap:
    rb2 += struct.pack("<H", x)
wr(os.path.join(OUT_DIR, "sid_remap.bin"), rb2)
print("[写出] sid_remap.bin %d B（旧 %d 台）" % (len(rb2), OLD_TOTAL))

# ---------------- alturls.bin ----------------
au_idx, au_blob = [], bytearray()
for it in ordered:
    alts = [u for u in (it.get("urls") or []) if u and u != it.get("url")]
    if not alts:
        au_idx.append(0); continue
    au_idx.append(len(au_blob))
    alts = alts[:8]
    au_blob += bytes([len(alts)])
    for u in alts:
        ub = cut_bytes(u, 512)
        au_blob += struct.pack("<H", len(ub)) + ub
    while len(au_blob) % 2:            # 2 字节对齐，方便固件读
        au_blob += b"\x00"
hdr = (b"ESP32RAU" + b"\x01\x00"
       + struct.pack("<H", len(ordered))
       + struct.pack("<I", 20)
       + struct.pack("<I", 20 + 4 * len(ordered)))
au = bytearray(hdr)
for off in au_idx:
    au += struct.pack("<I", off)
au += au_blob
wr(os.path.join(OUT_DIR, "alturls.bin"), bytes(au))
print("[写出] alturls.bin %d B（%d 台有多源）" % (len(au), sum(1 for x in au_idx if x)))

# ---------------- logos.bin ----------------
from PIL import Image

def load_logo(fname):
    p = os.path.join(LOGOS_DIR, fname)
    im = Image.open(p)
    im = im.convert("RGBA")
    bg = Image.new("RGB", im.size, (255, 255, 255))
    bg.paste(im, mask=im.split()[3])
    return bg

def to565(im):
    from array import array
    a = array("H")
    for px in im.convert("RGB").getdata():
        a.append(((px[0] & 0xF8) << 8) | ((px[1] & 0xFC) << 3) | (px[2] >> 3))
    return a.tobytes()          # x86 上为 little-endian uint16

logo_ids, logo_files, logo_blobs = {}, [], []
sid_logo = []
thumbs_for_preview = []
bad_logos = []

def try_load(fn):
    try:
        im = load_logo(fn)
        if im.size[0] < 8 or im.size[1] < 8:
            raise ValueError("尺寸过小 %s" % (im.size,))
        return im
    except Exception as e:
        bad_logos.append((fn, str(e)[:60]))
        return None

for fname in sid_logo_file:
    if not fname:
        sid_logo.append(0xFFFF); continue
    fn = os.path.basename(fname)
    if not os.path.exists(os.path.join(LOGOS_DIR, fn)):
        sid_logo.append(0xFFFF); continue
    if fn not in logo_ids:
        im = try_load(fn)
        if im is None:
            sid_logo.append(0xFFFF); continue
        w, h = im.size
        side = max(w, h)
        sq = Image.new("RGB", (side, side), (255, 255, 255))
        sq.paste(im, ((side - w) // 2, (side - h) // 2))
        th = sq.resize((THUMB, THUMB), Image.LANCZOS)
        fu = sq.resize((FULL, FULL), Image.LANCZOS)
        blob = to565(th) + to565(fu)
        logo_ids[fn] = len(logo_files)
        logo_files.append(fn)
        logo_blobs.append(blob)
        thumbs_for_preview.append((fn, th, fu))
    sid_logo.append(logo_ids[fn])

NUM_STN, NUM_LOGO = len(ordered), len(logo_files)
logo_hdr = (b"ESP32LGB" + bytes([1]) + bytes([THUMB, FULL, 0])
            + struct.pack("<H", NUM_STN) + struct.pack("<H", NUM_LOGO)
            + struct.pack("<I", 32) + struct.pack("<I", 32 + NUM_STN * 2)
            + struct.pack("<I", 32 + NUM_STN * 2 + NUM_LOGO * 4) + b"\x00" * 4)
lb = bytearray(logo_hdr)
for x in sid_logo:
    lb += struct.pack("<H", x)
off = 0
for blob in logo_blobs:
    lb += struct.pack("<I", off)
    off += len(blob)
for blob in logo_blobs:
    lb += blob
logo_bytes = bytes(lb)
wr(os.path.join(OUT_DIR, "logos.bin"), logo_bytes)
print("[写出] logos.bin %d B (%.2f MB) / %d 台有台标 / %d 个去重图块"
      % (len(logo_bytes), len(logo_bytes) / 1048576, sum(1 for x in sid_logo if x != 0xFFFF), NUM_LOGO))

# ---------------- preview（台标两档 + 列表行版面模拟） ----------------
try:
    from PIL import ImageDraw, ImageFont
    FONT = r"C:\Windows\Fonts\msyhbd.ttc"
    f16 = ImageFont.truetype(FONT, 16)
    W, H = 480, 320
    canvas = Image.new("RGB", (W, H), (247, 243, 230))
    dr = ImageDraw.Draw(canvas)

    # 上半：6 行列表模拟（40x40 台标 + 台名 + 分类/频率行）
    rows = [i for i in range(len(ordered)) if sid_logo[i] != 0xFFFF][:6]
    for n, sid in enumerate(rows):
        y = 4 + n * 48
        im = thumbs_for_preview[sid_logo[sid]][1]
        canvas.paste(im, (6, y + 4))
        it = ordered[sid]
        dr.text((54, y + 4), (it.get("name") or "")[:14], fill=(30, 30, 30), font=f16)
        dr.text((54, y + 24), "%s  %s" % ((it.get("category") or ""), (it.get("frequency") or "")),
                fill=(120, 116, 108), font=f16)
        dr.text((300, y + 4), "☆", fill=(160, 160, 160), font=f16)
        dr.line((0, y + 47, 330, y + 47), fill=(214, 208, 194))

    # 右侧：12 个台标 64x64（全屏档）
    for n in range(min(12, len(thumbs_for_preview))):
        im = thumbs_for_preview[n][2]
        canvas.paste(im, (340 + (n % 3) * 46, 4 + (n // 3) * 46))
    dr.text((340, 196), "64x64 全屏档", fill=(90, 90, 90), font=f16)
    dr.text((4, 296), "左：列表 40x40 档  右：全屏 64x64 档", fill=(90, 90, 90), font=f16)
    pv = os.path.join(OUT_DIR, "preview_logos_check.png")
    canvas.save(pv)
    print("[写出] preview_logos_check.png")
except Exception as e:
    print("[跳过] preview：%s" % e)

# ---------------- report ----------------
url_max = max(len((it.get("url") or "").encode()) for it in ordered)
name_max = max(len((it.get("name") or "").encode()) for it in ordered)
name_trunc = sum(1 for it in ordered if len((it.get("name") or "").encode()) > 60)
cat_set = sorted({(it.get("category") or "") for it in ordered})
reg_counts = sorted(((rn, counts[i]) for i, rn in enumerate(regions)), key=lambda x: -x[1])

rep = []
rep.append("# 电台数据同步 —— PC 端生成报告（%s）\n" % "2026-11-08")
rep.append("## 产物\n")
rep.append("| 文件 | 大小 | 去向 | 说明 |")
rep.append("|---|---|---|---|")
rep.append("| `stations.bin` | %d B（%.1f KB） | LittleFS `/stations.bin`（`uploadfs`） | v2 格式，固件解析代码不用改 |" % (len(stn_bytes), len(stn_bytes) / 1024))
rep.append("| `radio_meta.bin` | %d B | LittleFS `/radio_meta.bin` | binTotal=%d，all[] 重生，18 条用户台原样保留 |" % (len(meta_bytes), NEW_BIN_TOTAL))
rep.append("| `logos.bin` | %d B（%.2f MB） | **SD 根目录** | 40×40 列表档 + 64×64 全屏档，RGB565 裸数据 |" % (len(logo_bytes), len(logo_bytes) / 1048576))
rep.append("| `alturls.bin` | %d B | **SD 根目录** | %d 台的多源备份流 |" % (len(au), sum(1 for x in au_idx if x)))
rep.append("| `sid_remap.bin` | %d B | LittleFS 或 SD | 旧 sid→新 sid，迁移收藏/历史用 |" % len(rb2))
rep.append("")
rep.append("## 电台数量\n")
rep.append("- 写入：**%d 台 / %d 地区**" % (len(records), NREG))
rep.append("- 旧库重映射命中：URL %d、台名唯一 %d、归一 URL %d、丢弃 %d" % (matched_by["url"], matched_by["name"], matched_by["nurl"], matched_by["lost"]))
rep.append("- 用户台（extras）%d 条平移到 sid %d..%d" % (o_extra, NEW_BIN_TOTAL, NEW_BIN_TOTAL + o_extra - 1))
rep.append("- fav %d→%d，hist %d→%d" % (len(o_fav), len(fav_new), len(o_hist), len(hist_new)))
rep.append("- extras 共 %d 条（原用户台 %d + 为保住收藏/历史补的孤儿台 %d）" % (len(extra_records), o_extra, len(extra_records) - o_extra))
rep.append("- 台标：%d 台有图 / %d 个去重图块，坏图跳过 %d 个%s" % (
    sum(1 for x in sid_logo if x != 0xFFFF), NUM_LOGO, len(set(b[0] for b in bad_logos)),
    ("（" + "、".join(sorted(set(b[0] for b in bad_logos))[:3]) + "…）") if bad_logos else ""))
rep.append("")
rep.append("## 字段上界（对照固件缓冲）\n")
rep.append("- 台名：源数据最长 %d B；超过 60 B 记录上限被截断 **%d 台**；超过 32 B（`RadioStation::name`）在设备上会被截断显示 **%d 台**（与移动端显示不同，但不会越界）"
           % (name_max, sum(1 for it in ordered if len((it.get("name") or "").encode()) > 60),
              sum(1 for it in ordered if len((it.get("name") or "").encode()) > 32)))
rep.append("- URL 最长 %d B（记录上限 512）" % url_max)
rep.append("- 无台标台数：**%d 台**（用首字色块占位）" % (len(ordered) - sum(1 for x in sid_logo if x != 0xFFFF)))
rep.append("- 分类取值：%s" % "、".join(cat_set))
rep.append("")
rep.append("## 各地区台数\n")
for rn, c in reg_counts:
    rep.append("- %s：%d" % (rn, c))
rep.append("")
rep.append("## 固件侧还差什么（本次只做 PC 端，以下都没动代码）\n")
rep.append("1. **`RMAX_STN` / `RMAX_ALL` 1700 → 2500**（`src/ui_common.h` 238/262 与 `src/main.cpp` 155/161 双处）。")
rep.append("   ⚠️ 不改就烧这套数据会**越界**：`memcpy(allSeq, all, allCnt*2)` 要写 2412×2 = 4824 B，而 `allSeq` 只有 3400 B。")
rep.append("2. `radio_meta.bin` 的 27 条 extras + 6 条助眠 = 总台数 2445，`RMAX_STN=2500` 刚好留 55 个余量。")
rep.append("3. 新增读 `logos.bin`（SD）：按 sid 查 u16 logo_id → 读 40×40 块推列表行 / 64×64 块推全屏；PSRAM→内部中转→`pushImage`（守 R3）。")
rep.append("4. 新增读 `alturls.bin`（SD）：起播失败时按备用流重试，治断流。")
rep.append("5. 一次性迁移 NVS 收藏/历史：读 `sid_remap.bin`，用 NVS 版本键保证只跑一次。")
rep.append("6. 列表行版面：40×40 台标占 x 6..46，台名起点 24 → 52（可视宽度 280 → 约 250 px）。全屏 64×64 需你定位置。")
rep.append("")
rep.append("## 落地步骤（等固件改完再执行）\n")
rep.append("```powershell")
rep.append("# 1) 数据进 LittleFS（stations.bin / radio_meta.bin / sid_remap.bin）")
rep.append("Copy-Item 'build\\radio_sync_20261108\\stations.bin'    'data\\stations.bin'    -Force")
rep.append("Copy-Item 'build\\radio_sync_20261108\\radio_meta.bin'  'data\\radio_meta.bin'  -Force")
rep.append("Copy-Item 'build\\radio_sync_20261108\\sid_remap.bin'   'data\\sid_remap.bin'   -Force")
rep.append("& '.platformio\\penv\\Scripts\\pio.exe' run -e s3v5 -t uploadfs")
rep.append("# 2) 台标与备用流进 SD 根目录（13 MB，LittleFS 装不下）")
rep.append("Copy-Item 'build\\radio_sync_20261108\\logos.bin'   'E:\\logos.bin'   -Force   # E: = SD 卡盘符")
rep.append("Copy-Item 'build\\radio_sync_20261108\\alturls.bin' 'E:\\alturls.bin' -Force")
rep.append("```")
rep.append("")
rep.append("注意：`sdstore::openRead` **SD 优先**，SD 根目录若还留着旧的 `stations.bin`/`radio_meta.bin`，必须一起覆盖或删掉，否则读到的还是旧表。")
wr(os.path.join(OUT_DIR, "report.md"), ("\n".join(rep) + "\n").encode("utf-8"))
print("[写出] report.md")
print("\n完成 → %s" % OUT_DIR)
