#!/usr/bin/env python3
"""验证: 硬件 AA 字库 (烘焙 16x16 灰度 + (bg*(255-m)+fg*m)/255 合成)
与 preview 的 d.text (msyhbd 16px 矢量 AA) 渲染是否逐像素一致。

模拟硬件 XFont::compositeGlyph / drawStringFastAA 的逐字渲染:
  - 每个字是 16x16 灰度位图 (左中锚点在 (0,8)), pushImage 到 (x, y)
  - advance = gen_aa_font 里存的字宽 (msyhbd getlength)
  - 合成公式: out = (bg*(255-m) + fg*m) / 255 (与 blitDialNum 同款)
"""
import os
import struct
import bisect
from PIL import Image, ImageDraw, ImageChops

import make_preview as mp

FONT_FILE = r"D:\ESP32-S3\data\font_simhei_16.font"
OUT = os.path.dirname(os.path.abspath(__file__))


def load_font_glyphs(path):
    d = open(path, "rb").read()
    assert d[0:4] == b"XAAX", "bad magic"
    n = struct.unpack(">H", d[6:8])[0]
    unis, advs = [], []
    pix = d[8:]
    for i in range(n):
        g = pix[i * 259:i * 259 + 259]
        unis.append(struct.unpack(">H", g[0:2])[0])
        advs.append(g[2])
    return unis, advs, pix


def hw_compose_glyph(target, x, y, m, fg, bg):
    """m: 16x16 灰度. 合成到 target (RGB), 区域 (x,y)-(x+16,y+16)。
    out = (bg*(255-m)+fg*m)/255 — 与硬件 XFont::compositeGlyph 完全一致。"""
    x, y = int(x), int(y)
    for j in range(16):
        for k in range(16):
            a = int(m[j * 16 + k])
            px, py = x + k, y + j
            if px < 0 or py < 0 or px >= target.width or py >= target.height:
                continue
            r, g, b = target.getpixel((px, py))
            a = int(a)
            target.putpixel((px, py), (
                int((bg[0] * (255 - a) + fg[0] * a) // 255),
                int((bg[1] * (255 - a) + fg[1] * a) // 255),
                int((bg[2] * (255 - a) + fg[2] * a) // 255)))


def hw_draw_string(img, x, y, s, unis, advs, pix, fg, bg):
    """模拟硬件: 逐字 16x16 合成, advance=字宽。y 是 pushImage 顶部 (左中锚点在 y+8)。
    Python3 str 已是 unicode, 每个 s[i] 直接是一个码点 (无需 UTF-8 解码)。"""
    cur = x
    for ch in s:
        cp = ord(ch)
        idx = bisect.bisect_left(unis, cp)
        if idx < len(unis) and unis[idx] == cp:
            m = pix[idx * 259 + 3: idx * 259 + 3 + 256]
            hw_compose_glyph(img, cur, y, m, fg, bg)
            cur += advs[idx]
        else:
            cur += 16 if cp > 127 else 8
    return cur


def render_date_hw(unis, advs, pix):
    info = mp.date_strings()
    img = Image.new("RGB", (480, 320), mp.c565(0x0000))
    d = ImageDraw.Draw(img)
    cx = (mp.DATE_LEFT + mp.DATE_RIGHT) // 2
    d.rounded_rectangle([mp.DATE_LEFT, mp.ROW1_TOP, mp.DATE_RIGHT, mp.ROW1_TOP + mp.ROW1_H - 1],
                        radius=8, fill=mp.ROW1_BG)
    cy1 = mp.ROW1_TOP + mp.ROW1_H // 2
    y1 = cy1 - 8
    x = mp.DATE_LEFT + 12
    hw_draw_string(img, x, y1, info["date"], unis, advs, pix, mp.WHITE, mp.ROW1_BG)
    x += mp.F16.getlength(info["date"]) + 8
    hw_draw_string(img, x, y1, info["week"], unis, advs, pix, mp.CYAN, mp.ROW1_BG)
    d.line([cx, mp.ROW1_TOP + 9, cx, mp.ROW1_TOP + mp.ROW1_H - 9], fill=mp.DATE_SEP)
    x = cx + 12
    hw_draw_string(img, x, y1, info["lunar"], unis, advs, pix, mp.WHITE, mp.ROW1_BG)
    x += mp.F16.getlength(info["lunar"]) + 6
    hw_draw_string(img, x, y1, info["gz"], unis, advs, pix, mp.GOLD, mp.ROW1_BG)
    if info["fest"]:
        x += mp.F16.getlength(info["gz"]) + 6
        hw_draw_string(img, x, y1, "今日" + info["fest"], unis, advs, pix,
                       mp.event_color(info["fest"]), mp.ROW1_BG)
    d.rounded_rectangle([mp.DATE_LEFT, mp.ROW2_TOP, mp.DATE_RIGHT, mp.ROW2_TOP + mp.ROW2_H - 1],
                        radius=8, fill=mp.ROW2_BG)
    cy2 = mp.ROW2_TOP + mp.ROW2_H // 2
    y2 = cy2 - 8
    xj = mp.DATE_LEFT + 12
    if info["cur_d"] == 0:
        cur_s = "今日" + info["cur"]
        hw_draw_string(img, xj, y2, cur_s, unis, advs, pix, mp.event_color(info["cur"]), mp.ROW2_BG)
        xj += int(mp.F16.getlength(cur_s)) + 10
        hw_draw_string(img, xj, y2, "%s %d天后" % (info["nxt"], info["nxt_d"]), unis, advs, pix, mp.SOFT, mp.ROW2_BG)
    else:
        hw_draw_string(img, xj, y2,
                       "%s 已过%d天   %s %d天后" % (info["cur"], info["cur_d"], info["nxt"], info["nxt_d"]),
                       unis, advs, pix, mp.SOFT, mp.ROW2_BG)
    x = mp.DATE_RIGHT - 12
    hms = "%d%%" % mp.MOCK_HUMIDITY
    hw_draw_string(img, x - mp.F16.getlength(hms), y2, hms, unis, advs, pix,
                   mp.humid_color(mp.MOCK_HUMIDITY), mp.ROW2_BG)
    x -= int(mp.F16.getlength(hms)) + 8
    tms = "%d°C" % int(mp.MOCK_TEMP)
    hw_draw_string(img, x - mp.F16.getlength(tms), y2, tms, unis, advs, pix,
                   mp.temp_color(int(mp.MOCK_TEMP)), mp.ROW2_BG)
    x -= int(mp.F16.getlength(tms)) + 8
    hw_draw_string(img, x - mp.F16.getlength(info["moon"]), y2, info["moon"], unis, advs, pix, mp.MOON_TXT, mp.ROW2_BG)
    x -= int(mp.F16.getlength(info["moon"])) + 6
    mp.draw_moon(d, int(x) - 8, cy2, 9, info["mp"])
    return img


def render_date_preview():
    img = Image.new("RGB", (480, 320), mp.c565(0x0000))
    d = ImageDraw.Draw(img)
    mp.draw_date(d)
    return img


def main():
    unis, advs, pix = load_font_glyphs(FONT_FILE)
    hw = render_date_hw(unis, advs, pix)
    pv = render_date_preview()
    hw.save(os.path.join(OUT, "verify_hw.png"))
    pv.save(os.path.join(OUT, "verify_preview.png"))
    diff = ImageChops.difference(hw, pv)
    y0, y1 = mp.ROW1_TOP - 2, mp.ROW2_TOP + mp.ROW2_H + 2
    diff.crop((0, y0, 480, y1)).resize((480, (y1 - y0) * 3), Image.NEAREST).save(os.path.join(OUT, "verify_diff.png"))
    ca = hw.crop((0, mp.ROW1_TOP, 480, mp.ROW2_TOP + mp.ROW2_H))
    cb = pv.crop((0, mp.ROW1_TOP, 480, mp.ROW2_TOP + mp.ROW2_H))
    pa, pb = list(ca.getdata()), list(cb.getdata())
    s = 0
    mx = 0
    for (r1, g1, b1), (r2, g2, b2) in zip(pa, pb):
        dd = abs(r1 - r2) + abs(g1 - g2) + abs(b1 - b2)
        s += dd
        if dd > mx:
            mx = dd
    n = len(pa)
    print("日期条区域 平均偏差/通道: %.2f  最大单点偏差(3通道和): %d  (0=完全一致)" % (s / n / 3.0, mx))
    print("已输出 verify_hw.png / verify_preview.png / verify_diff.png")


if __name__ == "__main__":
    main()
