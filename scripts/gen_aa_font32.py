#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 32px 原生 AA(8-bit 灰度抗锯齿) 中文字库 font_simhei_32.font, 供 XFont 大字(黄历弹层)使用。

为什么单独一个小字集 (而不是复用 16px 全 GB2312 字集):
  * 32x32 单元格 1024 字节/字, 全字集(~4000 字)会占 ~7MB PSRAM, 设备只有 8MB PSRAM,
    再加 WiFi/电台/音频缓冲会 OOM。
  * 32px 字库**只**被黄历弹层的大字用 (almBigText): 年份数字 / 月份数字 / 日数数字 /
    农历X月X (左) / 干支X日 (右) / 星期+周几字。所以只需这些字符 -> ~250KB, 安全。
  * 缺字时 XFont::drawBigChar 返回(不画), 与 font16 缺字行为一致; 黄历大字只渲染已知词池, 全覆盖。

与 gen_aa_font.py (16px, ver=1) 的区别: SIZE=32 (原生烘焙, 不再把 16px 上采样) + VERSION=2
  (XFont 按 ver 决定单元格: ver1=16x16, ver2=32x32)。同源字体 msyhbd.ttc, 左中锚点(0,SIZE//2),
advance=getlength -> 32px 与 16px 逐字对齐 (advance 2x), 视觉同一字体。

运行: python scripts/gen_aa_font32.py  ->  data/font_simhei_32.font
"""
import os
import struct
from PIL import Image, ImageDraw, ImageFont

FONT_PATH = r"C:\Windows\Fonts\msyh.ttc"   # 常规字重 (非粗体; 只有黄历中间大数字用粗体 Bahnschrift)
SIZE = 64                        # 黄历大字原生分辨率 (64px -> 所有目标尺寸 13~34 都是干净下采样)
OUT = os.path.join(os.path.dirname(__file__), "..", "data", "font_simhei_64.font")
MAGIC = b"XAAX"
VERSION = 3                      # XFont 读 ver 决定单元格: ver3 = 64x64


def build_charset():
    chars = set()
    for i in range(0x20, 0x7F):            # 可打印 ASCII (数字/标点/字母)
        chars.add(chr(i))
    for i in range(0xA0, 0x100):           # Latin-1 补充 (°/· 等, 保险)
        chars.add(chr(i))
    # 黄历大字用到的全部汉字 (词池并集, 覆盖 almBigText 能渲染的所有字):
    cjk = (
        "甲乙丙丁戊己庚辛壬癸"                       # 天干
        "子丑寅卯辰巳午未申酉戌亥"                   # 地支
        "鼠牛虎兔龙蛇马羊猴鸡狗猪"                   # 生肖
        "年月日星闰"                                  # 年/月/日/星期/闰
        "日一二三四五六"                               # 星期几
        "正一二三四五六七八九十冬腊"                   # 农历月名
        "初一十廿"                                    # 农历日名 (初/一/十/廿)
        "正杏桃槐榴荷兰桂菊阳"                        # 花名月
        "嫁娶移徙入宅装修动土栽种祭祀安床会友"          # 宜/忌项
        "开张开业安葬破土词讼开光"                    # 忌项
        "小寒大寒立春雨水惊蛰春分清明谷雨"            # 节气
        "立夏小满芒种夏至小暑大暑立秋处暑白露秋分"     # 节气
        "寒露霜降立冬小雪大雪冬至"                    # 节气
        "春节元旦元宵劳动端午中秋国庆重阳圣诞"        # 节日
        "宜忌吉"
        "农历干支期今后"                              # 小字(almScaledText): 农历/干支/星期/今日X/X后
        "除夕妇女节儿童建军植树教师青年母亲父护士"    # 小字: 常见节日名(节日框可能来自政府 JSON)
        "万黑棍色情愚党平夜感恩复活抬头条"           # 万圣节/黑色星期五/光棍节 + 其余节日名缺字(全64px不降级)
    )
    chars.update(cjk)
    return sorted(chars)


def render_glyph(font, ch):
    """渲染单个字到 SIZE x SIZE 灰度画布(左中锚点), 返回 (alpha bytes, advance)。"""
    canvas = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(canvas).text((0, SIZE // 2), ch, font=font, fill=255, anchor="lm")
    alpha = bytes(canvas.getdata())
    adv = int(round(font.getlength(ch)))
    adv = 1 if adv < 1 else (255 if adv > 255 else adv)
    return alpha, adv


# 缺字占位框 (tofu) 检测: 与 gen_aa_font.py 同款 (PUA \uF8FF 参考框)
TOFU_REF = "\uF8FF"


def main():
    if not os.path.exists(FONT_PATH):
        sys_exit = "font not found: " + FONT_PATH
        raise SystemExit(sys_exit)
    font = ImageFont.truetype(FONT_PATH, SIZE)
    chars = build_charset()
    print("charset size:", len(chars))
    ref = render_glyph(font, TOFU_REF)[0]

    out = bytearray()
    skipped_tofu = 0
    for ch in chars:
        alpha, adv = render_glyph(font, ch)
        if alpha == ref:                  # tofu 缺字框跳过
            skipped_tofu += 1
            continue
        out += struct.pack(">H", ord(ch))  # unicode (BE)
        out.append(adv)                    # advance (64px: CJK=64 数字=40 ...)
        out += alpha                       # 1024 字节 (32x32)
    print("skipped %d tofu glyphs" % skipped_tofu)
    n_glyphs = len(chars) - skipped_tofu
    header = MAGIC + bytes([VERSION, 0]) + struct.pack(">H", n_glyphs)
    out = header + out

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(bytes(out))
    print("wrote %s  (%d bytes, %d glyphs, %dpx)" % (os.path.abspath(OUT), len(out), n_glyphs, SIZE))


if __name__ == "__main__":
    main()
