#!/usr/bin/env python3
"""生成 AA(8-bit 灰度抗锯齿) 中文字库 font_simhei_16.font, 供 XFont 使用。

关键设计(为了让"硬件 == 预览"):
  * 字体文件与 preview 完全同源: 用 C:\\Windows\\Fonts\\msyhbd.ttc (雅黑 Bold) 在 16px 渲染,
    每个字 16x16 8-bit 灰度 alpha 位图 + 逐字 advance 宽度(与 preview 的 F16.getlength 一致,
    即 CJK=16, 数字=10, 'A'=12, '-'=7, '.'=5, '%'=15 ...)。
  * 逐字 alpha 与 advance 都按"左中锚点 (0, SIZE//2)"渲染, 与 make_preview 里
    d.text((x, cy2), s, anchor='lm') 的落点/步进逐像素一致。
  * 文件为二进制:  magic 'XAAX' | ver(1) | pad(1)=0 | n(2, BE) | 每字 [adv(1) + 16x16(256)]。
  * ver1 = 16px 原生灰度: 运行时 16->16 恒等合成 (无下移/无加粗), 与 preview F16 (msyhbd 16px) 逐像素一致; advance 按 16px 量 (布局不变)。
    XFont 按 magic 自动识别(旧 base-64 1-bit 文件没有 magic, 走旧路径)。
  * 字集 = GB2312 一级常用字(覆盖 99% 汉字) ∪ 源码/数据里出现的全部汉字 ∪ 可打印 ASCII。

运行: python scripts/gen_aa_font.py  ->  data/font_simhei_16.font (原地覆盖, 文件名不变)
"""
import os
import sys
import glob
import struct
from PIL import Image, ImageDraw, ImageFont

FONT_PATH = r"C:\Windows\Fonts\msyhbd.ttc"   # 雅黑 Bold (与 preview F16 同源; 常规体 16px 笔画~1px 发虚, 粗体~2px 更实)
SIZE = 16                       # 原生渲染单元格: 直接烤 16x16 灰度, 运行时 16->16 恒等合成 (无下移/无加粗), 与 preview F16 逐像素一致
ADV_SIZE = 16                   # advance 仍按 16px 量 (getlength@16px) -> 与旧字库逐字 advance 一致, 布局不变
OUT = os.path.join(os.path.dirname(__file__), "..", "data", "font_simhei_16.font")
MAGIC = b"XAAX"
VERSION = 1                     # XFont 读 ver 决定单元格: ver1 = 16x16 原生 (运行时 16->16 恒等, 无下移/无加粗)


def build_charset():
    chars = set()
    for i in range(0x20, 0x7F):          # 可打印 ASCII (数字/标点/字母)
        chars.add(chr(i))
    for i in range(0xA0, 0x100):         # Latin-1 补充 (°=0xB0, ·=0xB7 等, 温度/电台用)
        chars.add(chr(i))
    for ch in "★☆→←↑↓•…—–":           # 常用符号 (收藏/箭头等)
        chars.add(ch)
    # GB2312 全字集 (一级 B0..D7 + 二级 D8..F7), 覆盖绝大多数汉字。
    # 二级含生僻地名/人名用字 (例 "岷""黔""赣""邛"), 电台名常出现 —— 只收一级会在
    # 电台列表里留出空白方块 (用户反馈"岷江音乐"的"岷"变空格)。
    for hi in range(0xB0, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                chars.add(bytes([hi, lo]).decode("gb2312"))
            except Exception:
                pass
    # 源码 / 数据里实际用到的全部非 ASCII 可打印字符 (含汉字 + 符号, 保证不缺失)
    files = glob.glob(os.path.join("src", "*.cpp")) + glob.glob(os.path.join("include", "*.h"))
    files += [os.path.join("data", "festival.json"), os.path.join("data", "stations.json")]
    for f in files:
        if os.path.exists(f):
            try:
                t = open(f, encoding="utf-8", errors="ignore").read()
            except Exception:
                continue
            chars.update(ch for ch in t if 0x20 <= ord(ch) <= 0xFFFF and ch not in "\r\n\t")
    # 收音机备份 JSON (电台名/分类/地区/频率/URL 里的全部非 ASCII 字符).
    # 电台名常含生僻字 (例 "岷" "黔" "赣"), 不在 GB2312 一级内 —— 必须全收,
    # 否则预览/硬件上电台名会出现空白方块.
    try:
        import glob as _g
        bkp = _g.glob(os.path.join("..", "*收音机*.json")) + \
                _g.glob("*收音机*.json") + \
                _g.glob("海燕收音机_备份_*.json")
        for bf in bkp:
            if os.path.exists(bf):
                raw = open(bf, encoding="utf-8", errors="ignore").read()
                try:
                    dj = __import__("json").loads(raw)
                except Exception:
                    dj = None
                # 提取: radio_channels.radio[*].{name,category,description,frequency,lineLabels}
                #       + radio_user_stations[*].{name,category,description,frequency}
                #       + 顶层 radio_* 键的全部字符串值
                def _walk(o, sink):
                    if isinstance(o, str):
                        for ch in o:
                            if 0x20 <= ord(ch) <= 0xFFFF and ch not in "\r\n\t":
                                sink.add(ch)
                    elif isinstance(o, dict):
                        for v in o.values(): _walk(v, sink)
                    elif isinstance(o, (list, tuple)):
                        for v in o: _walk(v, sink)
                if dj is not None:
                    for k in ("radio_channels", "radio_user_stations"):
                        v = dj.get(k, "")
                        if isinstance(v, str):
                            try: _walk(__import__("json").loads(v), chars)
                            except Exception: pass
                        else:
                            _walk(v, chars)
                else:
                    # 兜底: 整文件非 ASCII 全收
                    for ch in raw:
                        if 0x20 <= ord(ch) <= 0xFFFF and ch not in "\r\n\t":
                            chars.add(ch)
    except Exception as e:
        print("  (radio backup charset scan failed:", e, ")")
    return sorted(chars)


def render_glyph(font, ch, font16):
    """渲染单个字到 SIZE x SIZE 灰度画布(左中锚点), 返回 (alpha bytes, advance)。
    alpha 用 font (SIZE=16px 原生), advance 用 font16 (16px, 与旧字库逐字一致)。"""
    canvas = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(canvas).text((0, SIZE // 2), ch, font=font, fill=255, anchor="lm")
    alpha = bytes(canvas.getdata())
    adv = int(round(font16.getlength(ch)))
    adv = 1 if adv < 1 else (255 if adv > 255 else adv)
    return alpha, adv


# 微软雅黑(msyhbd.ttc)不含韩/维/藏/蒙古等少数民族文字, Pillow 对缺失字符统一回退
# 成同一个"缺字占位框" (tofu □, 16x16 内 ~11x11 空心方块)。若照单全收会被烤进
# 字库 -> 硬件/预览电台名出现一排乱码方块。检测法 = 渲染一个保证不存在的 PUA
# 码点 (\uF8FF) 得到参考方框, 任何字符 alpha 与参考逐字节相等即判为 tofu 跳过。
# 该法精确: 门/口/0/O/方/C 等真实"方框"字形 alpha 不同, 不会被误杀。
TOFU_REF = "\uF8FF"


def is_tofu(alpha, ref):
    """alpha 是否与参考缺字框逐字节一致 (= 字体里没有该字, Pillow 画的 □)。"""
    return alpha == ref


def main():
    if not os.path.exists(FONT_PATH):
        sys.exit("font not found: " + FONT_PATH)
    font = ImageFont.truetype(FONT_PATH, SIZE)        # 16px 原生, 烤 16x16 alpha
    font16 = ImageFont.truetype(FONT_PATH, ADV_SIZE)  # 16px, 只用来量 advance (与旧字库一致)
    chars = build_charset()
    print("charset size:", len(chars))
    ref = render_glyph(font, TOFU_REF, font16)[0]   # 参考缺字框 alpha

    out = bytearray()
    skipped_tofu = 0
    for ch in chars:
        alpha, adv = render_glyph(font, ch, font16)
        if is_tofu(alpha, ref):
            skipped_tofu += 1
            continue   # 缺字方框不进字库 (硬件/预览对该字按"无字"处理)
        out += struct.pack(">H", ord(ch))  # unicode (BE)
        out.append(adv)                    # advance
        out += alpha                       # 256 字节
    print("skipped %d tofu glyphs" % skipped_tofu)
    n_glyphs = len(chars) - skipped_tofu   # 实际写入的字形数 (排除缺字方框)
    header = MAGIC + bytes([VERSION, 0]) + struct.pack(">H", n_glyphs)
    out = header + out

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(bytes(out))
    print("wrote %s  (%d bytes, %d glyphs)" % (os.path.abspath(OUT), len(out), n_glyphs))


if __name__ == "__main__":
    main()
