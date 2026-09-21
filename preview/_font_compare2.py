#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
font_compare2.py — 完整时钟场景下 1-bit vs AA 日期条对比

上: 当前硬件效果 (1-bit 16px, 无AA, ASCII半宽8px)
下: 升级后效果 (8-bit AA, 雅黑, 自然字宽)

输出: D:\ESP32-S3\preview\font_compare2.png
"""
import os, sys, datetime
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("PREVIEW_TIME", "15:32:08")

from PIL import Image, ImageDraw, ImageFont
import make_preview as mp
import nongli_data

# --- 字体: 1-bit 用 SimHei, AA 用 YaHei (与 font_compare.py 一致) ---
F16_1BIT = ImageFont.truetype(r"C:\Windows\Fonts\simhei.ttf", 16)
F16_AA   = ImageFont.truetype(r"C:\Windows\Fonts\msyh.ttc", 16)

def is_ascii(ch):
    return ord(ch) < 128

def render_1bit_text(img, x, y, text, color, font_size=16):
    """模拟 XFont::drawGlyph 1-bit 效果: 16x16 cell, 阈值, ASCII 半宽 8px."""
    f = F16_1BIT
    for ch in text:
        box = Image.new("L", (16, 16), 0)
        ImageDraw.Draw(box).text((0, 0), ch, font=f, fill=255, anchor="la")
        # 阈值 -> 1-bit
        box = box.point(lambda p: 255 if p >= 128 else 0)
        # 创建色块
        if is_ascii(ch):
            w_draw = 8  # 半宽
        else:
            w_draw = 16
        tile = Image.new("RGB", (w_draw, 16), color)
        mask = box.crop((0, 0, w_draw, 16))
        img.paste(tile, (int(x), y), mask)
        x += w_draw + 1  # 字间距 1px
    return x

def render_aa_text(img, x, y, text, color, font_size=16):
    """模拟升级后 8-bit AA 效果: 自然字宽, PIL 默认 AA."""
    f = F16_AA
    d.text((x, y), text, font=f, fill=color, anchor="lm")
    return x + f.getlength(text) + 1

def draw_date_1bit(img, d, info):
    """与 make_preview.draw_date 相同布局, 但用 1-bit 渲染 (模拟当前硬件)."""
    cx = (mp.DATE_LEFT + mp.DATE_RIGHT) // 2

    # 行1
    d.rounded_rectangle([mp.DATE_LEFT, mp.ROW1_TOP, mp.DATE_RIGHT, mp.ROW1_TOP + mp.ROW1_H - 1],
                        radius=8, fill=mp.ROW1_BG)
    cy1 = mp.ROW1_TOP + mp.ROW1_H // 2
    x = mp.DATE_LEFT + 12
    x = render_1bit_text(img, x, cy1 - 8, info["date"], mp.WHITE)
    x += 8
    x = render_1bit_text(img, x, cy1 - 8, info["week"], mp.CYAN)
    d.line([cx, mp.ROW1_TOP + 9, cx, mp.ROW1_TOP + mp.ROW1_H - 9], fill=mp.DATE_SEP)
    x = cx + 12
    x = render_1bit_text(img, x, cy1 - 8, info["lunar"], mp.WHITE)
    x += 6
    x = render_1bit_text(img, x, cy1 - 8, info["gz"], mp.GOLD)
    if info["fest"]:
        x += 6
        x = render_1bit_text(img, x, cy1 - 8, "今日" + info["fest"], mp.event_color(info["fest"]))

    # 行2
    d.rounded_rectangle([mp.DATE_LEFT, mp.ROW2_TOP, mp.DATE_RIGHT, mp.ROW2_TOP + mp.ROW2_H - 1],
                        radius=8, fill=mp.ROW2_BG)
    cy2 = mp.ROW2_TOP + mp.ROW2_H // 2
    xj = mp.DATE_LEFT + 12
    if info["cur_d"] == 0:
        xj = render_1bit_text(img, xj, cy2 - 8, "今日" + info["cur"], mp.event_color(info["cur"]))
        xj += 10
        xj = render_1bit_text(img, xj, cy2 - 8, "%s %d天后" % (info["nxt"], info["nxt_d"]), mp.SOFT)
    else:
        xj = render_1bit_text(img, xj, cy2 - 8,
                               "%s 已过%d天   %s %d天后" % (info["cur"], info["cur_d"], info["nxt"], info["nxt_d"]),
                               mp.SOFT)
    # 右侧
    h_str = "%d%%" % mp.MOCK_HUMIDITY
    t_str = "%d°C" % int(mp.MOCK_TEMP)
    moon_str = info["moon"]
    # 右对齐: 先算总宽度
    total_w = _w1bit(h_str) + 8 + _w1bit(t_str) + 8 + _w1bit(moon_str) + 6 + 18
    x = mp.DATE_RIGHT - 12 - total_w
    x = render_1bit_text(img, x, cy2 - 8, moon_str, mp.MOON_TXT)
    x += 6
    mp.draw_moon(d, int(x) + 9, cy2, 9, info["mp"])
    x += 18
    x = render_1bit_text(img, x, cy2 - 8, t_str, mp.temp_color(int(mp.MOCK_TEMP)))
    x += 8
    x = render_1bit_text(img, x, cy2 - 8, h_str, mp.humid_color(mp.MOCK_HUMIDITY))

def _w1bit(text):
    """1-bit 模式文本宽度."""
    w = 0
    for ch in text:
        w += (8 if is_ascii(ch) else 16) + 1
    return w - 1  # 末尾无间隔

def draw_date_aa(d, info):
    """与 make_preview.draw_date 完全一致 (AA YaHei). 复用 mp 的逻辑."""
    # 直接调用 make_preview.draw_date (它内部用 F16 即 msyhbd.ttc AA)
    mp.draw_date(d)

def build():
    now = datetime.datetime.now()
    digits = "%02d%02d%02d" % (now.hour, now.minute, now.second)
    info = mp.date_strings()

    # 上: 1-bit (当前硬件)
    img1 = Image.new("RGB", (480, 320), (0, 0, 0))
    d1 = ImageDraw.Draw(img1)
    # 时钟卡片
    for i, ch in enumerate(digits):
        mp.draw_flap_card(img1, d1, i, ord(ch) - 48, ord(ch) - 48, 0, 0.0)
    mp.draw_roller(d1, mp.GAP1_CX, mp.CARD_TOP, mp.CARD_TOP + mp.CARD_H - 1, 0)
    mp.draw_roller(d1, mp.GAP2_CX, mp.CARD_TOP, mp.CARD_TOP + mp.CARD_H - 1, 0)
    draw_date_1bit(img1, d1, info)
    mp.draw_nav(d1)
    if mp.DEBUG_UI:
        mp.draw_mode_hint(d1, 0)

    # 下: AA (升级后)
    img2 = Image.new("RGB", (480, 320), (0, 0, 0))
    d2 = ImageDraw.Draw(img2)
    for i, ch in enumerate(digits):
        mp.draw_flap_card(img2, d2, i, ord(ch) - 48, ord(ch) - 48, 0, 0.0)
    mp.draw_roller(d2, mp.GAP1_CX, mp.CARD_TOP, mp.CARD_TOP + mp.CARD_H - 1, 0)
    mp.draw_roller(d2, mp.GAP2_CX, mp.CARD_TOP, mp.CARD_TOP + mp.CARD_H - 1, 0)
    mp.draw_date(d2)
    mp.draw_nav(d2)
    if mp.DEBUG_UI:
        mp.draw_mode_hint(d2, 0)

    # 拼接: 上1-bit + 下AA, 中间标注
    LABEL_H = 28
    W = 480
    H = 320 + LABEL_H + 320 + 4
    out = Image.new("RGB", (W, H), (18, 18, 22))
    out.paste(img1, (0, 0))
    lbl = ImageDraw.Draw(out)
    lbl.text((10, 324), "▲ 当前硬件: 1-bit SimHei 16px (无AA, ASCII半宽8px)",
             font=ImageFont.truetype(r"C:\Windows\Fonts\arialbd.ttf", 16), fill=(255, 100, 100))
    out.paste(img2, (0, 320 + LABEL_H))

    path = r"D:\ESP32-S3\preview\font_compare2.png"
    out.save(path)
    print("saved", path, out.size)

if __name__ == "__main__":
    build()
