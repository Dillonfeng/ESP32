#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
font_compare.py — 中文小字效果对比 (供离线决策路线A/B)

结论先行: 路线A 与 路线B 在屏幕上显示的**像素完全相同**(都是把同一个 TTF
做 8-bit 抗锯齿)。区别只在固件实现方式(库 vs 自己扩 XFont), 肉眼看不出差别。
所以真正要选的是两件事:
  1) 1-bit(现在硬件, 锯齿)  vs  抗锯齿(AA, 平滑)
  2) 抗锯齿时, 用哪个字体的字形 (黑体 / 雅黑 / 雅黑细体)

本脚本生成 font_compare.png:
  - 上区: 1:1 原尺寸 (480x320 屏上真实大小, 16px 小字)
  - 下区: 4x 放大 (像素级, 锯齿 vs 平滑一眼可见)
"""
from PIL import Image, ImageDraw, ImageFont

def c565(h):
    return (((h >> 11) * 255 // 31,
             ((h >> 5) & 0x3F) * 255 // 63,
             (h & 0x1F) * 255 // 31))

ROW_BG = c565(0x2146)   # 日期条底色 (与 main.cpp / make_preview.py 一致)
TXT_W  = (255, 255, 255)
SZ = 16                 # 小字号

TEXT = "2026-09-20 星期日 八月初九 庚申 大寒 14天后"

# 每个选项: (标签, TTF路径, 模式)
#   mode="1bit" -> 模拟现在硬件 XFont::drawGlyph (1-bit, ASCII 半宽 8px)
#   mode="aa"   -> 路线A/B 的最终效果 (8-bit 抗锯齿)
OPTIONS = [
    ("现在硬件 1-bit 黑体 (锯齿, ASCII半宽)", r"C:\Windows\Fonts\simhei.ttf", "1bit"),
    ("路线A/B 抗锯齿 · 黑体 SimHei",          r"C:\Windows\Fonts\simhei.ttf", "aa"),
    ("路线A/B 抗锯齿 · 雅黑 YaHei",           r"C:\Windows\Fonts\msyh.ttc",   "aa"),
    ("路线A/B 抗锯齿 · 雅黑细体 YaHei-Light", r"C:\Windows\Fonts\msyhl.ttc",  "aa"),
]

def is_ascii(ch):
    return ord(ch) < 128

def render_row(path, mode, zoom):
    """按 16px 渲染 TEXT, 返回 RGB 行条 (ROW_BG 背景). zoom 仅用于放大展示。"""
    f = ImageFont.truetype(path, SZ)
    # 先算每个字的步进宽度
    advs = []
    for ch in TEXT:
        if mode == "1bit":
            advs.append(8 if is_ascii(ch) else 16)   # 硬件: 中文16/ASCII半宽8
        else:
            advs.append(max(1, int(round(f.getlength(ch)))))
    total_w = sum(advs)

    base = Image.new("L", (total_w, SZ), 0)   # 覆盖率/alpha
    x = 0
    for ch, adv in zip(TEXT, advs):
        box = Image.new("L", (SZ, SZ), 0)
        bd = ImageDraw.Draw(box)
        bd.text((0, 0), ch, font=f, fill=255, anchor="la")   # 左上对齐, 模拟 16x16 字库
        if mode == "1bit":
            box = box.point(lambda p: 255 if p >= 128 else 0)  # 阈值 -> 纯 1-bit
            if is_ascii(ch):
                box = box.crop((0, 0, 8, SZ))                # ASCII 只画左半 (8px)
        base.paste(box, (x, 0))
        x += adv

    row = Image.new("RGB", base.size, ROW_BG)
    row.paste(Image.new("RGB", base.size, TXT_W), mask=base)  # 白字合成到底色

    W, H = total_w * zoom, SZ * zoom
    resample = Image.NEAREST if mode == "1bit" else Image.LANCZOS
    return row.resize((W, H), resample)

def build():
    lab_f   = ImageFont.truetype(r"C:\Windows\Fonts\arial.ttf", 15)
    title_f = ImageFont.truetype(r"C:\Windows\Fonts\arialbd.ttf", 20)
    sec_f   = ImageFont.truetype(r"C:\Windows\Fonts\arialbd.ttf", 18)

    Z1 = 1
    Z4 = 4
    pad = 10
    # 计算宽度
    row_w = {o[0]: render_row(o[1], o[2], z).width for o, z in
             ((o, Z1) for o in OPTIONS)}
    row_w4 = {o[0]: render_row(o[1], o[2], Z4).width for o in OPTIONS}
    maxw = max(max(row_w.values()), max(row_w4.values()))
    M = 16
    W = M * 2 + maxw

    def block_h(zoom):
        # 标签 + 行条
        h_lab = 18
        h_row = SZ * zoom + 6
        return h_lab + h_row

    # 估算总高
    H = M
    H += 26                     # 主标题
    H += 6
    for o in OPTIONS:
        H += block_h(Z1)
        H += 4
    H += 24                     # 分节标题 "4x 放大"
    H += 6
    for o in OPTIONS:
        H += block_h(Z4)
        H += 4
    H += M

    img = Image.new("RGB", (W, H), (24, 26, 32))
    d = ImageDraw.Draw(img)
    y = M
    d.text((M, y), "中文小字 效果对比  (480×320 屏 · 16px 小字 · 白色字/日期条底色)",
           font=title_f, fill=(230, 232, 240))
    y += 26 + 6

    d.text((M, y), "① 原尺寸 1:1 (屏幕上真实大小)", font=sec_f, fill=(120, 200, 255))
    y += 24 + 6

    for label, path, mode in OPTIONS:
        d.text((M, y), label, font=lab_f, fill=(200, 202, 210))
        y += 18
        r = render_row(path, mode, Z1)
        img.paste(r, (M, y))
        y += r.height + 4
    y += 6

    d.text((M, y), "② 放大 4× (像素级: 锯齿 vs 平滑, 一眼可见)", font=sec_f, fill=(120, 200, 255))
    y += 24 + 6

    for label, path, mode in OPTIONS:
        d.text((M, y), label, font=lab_f, fill=(200, 202, 210))
        y += 18
        r = render_row(path, mode, Z4)
        img.paste(r, (M, y))
        y += r.height + 4

    out = r"D:\ESP32-S3\preview\font_compare.png"
    img.save(out)
    print("saved", out, img.size)

if __name__ == "__main__":
    build()
