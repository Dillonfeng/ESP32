#!/usr/bin/env python3
"""生成 AA(8-bit 灰度抗锯齿) 中文字库 font_simhei_16.font, 供 XFont 使用。

关键设计(为了让"硬件 == 预览"):
  * 字体文件与 preview 完全同源: 用 C:\\Windows\\Fonts\\msyhbd.ttc (雅黑 Bold) 在 16px 渲染,
    每个字 16x16 8-bit 灰度 alpha 位图 + 逐字 advance 宽度(与 preview 的 F16.getlength 一致,
    即 CJK=16, 数字=10, 'A'=12, '-'=7, '.'=5, '%'=15 ...)。
  * 逐字 alpha 与 advance 都按"左中锚点 (0, SIZE//2)"渲染, 与 make_preview 里
    d.text((x, cy2), s, anchor='lm') 的落点/步进逐像素一致。
  * 文件为二进制:  magic 'XAAX' | ver(1) | (1) | n(2, BE) | 每字 [adv(1) + 16x16(256)]。
    XFont 按 magic 自动识别(旧 base-64 1-bit 文件没有 magic, 走旧路径)。
  * 字集 = GB2312 一级常用字(覆盖 99% 汉字) ∪ 源码/数据里出现的全部汉字 ∪ 可打印 ASCII。

运行: python scripts/gen_aa_font.py  ->  data/font_simhei_16.font (原地覆盖, 文件名不变)
"""
import os
import sys
import glob
import struct
from PIL import Image, ImageDraw, ImageFont

FONT_PATH = r"C:\Windows\Fonts\msyhbd.ttc"
SIZE = 16                       # 与 XFont 16px 一致
OUT = os.path.join(os.path.dirname(__file__), "..", "data", "font_simhei_16.font")
MAGIC = b"XAAX"
VERSION = 1


def build_charset():
    chars = set()
    for i in range(0x20, 0x7F):          # 可打印 ASCII (数字/标点/字母)
        chars.add(chr(i))
    for i in range(0xA0, 0x100):         # Latin-1 补充 (°=0xB0, ·=0xB7 等, 温度/电台用)
        chars.add(chr(i))
    for ch in "★☆→←↑↓•…—–":           # 常用符号 (收藏/箭头等)
        chars.add(ch)
    # GB2312 一级常用字 (行 B0..D7, 列 A1..FE), 覆盖绝大多数汉字
    for hi in range(0xB0, 0xD8):
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
    return sorted(chars)


def render_glyph(font, ch):
    """渲染单个字到 SIZE x SIZE 灰度画布(左中锚点), 返回 (alpha bytes, advance)。"""
    canvas = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(canvas).text((0, SIZE // 2), ch, font=font, fill=255, anchor="lm")
    alpha = bytes(canvas.getdata())
    adv = int(round(font.getlength(ch)))
    adv = 1 if adv < 1 else (255 if adv > 255 else adv)
    return alpha, adv


def main():
    if not os.path.exists(FONT_PATH):
        sys.exit("font not found: " + FONT_PATH)
    font = ImageFont.truetype(FONT_PATH, SIZE)
    chars = build_charset()
    print("charset size:", len(chars))

    out = bytearray()
    for ch in chars:
        alpha, adv = render_glyph(font, ch)
        out += struct.pack(">H", ord(ch))  # unicode (BE)
        out.append(adv)                    # advance
        out += alpha                       # 256 字节
    header = MAGIC + bytes([VERSION, 0]) + struct.pack(">H", len(chars))
    out = header + out

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(bytes(out))
    print("wrote %s  (%d bytes, %d glyphs)" % (os.path.abspath(OUT), len(out), len(chars)))


if __name__ == "__main__":
    main()
