#!/usr/bin/env python3
"""live_preview.py - 实时模拟窗口 (无需烧录):
按当前真实时间运行, 齿轮实时旋转, 秒位按新节奏慢速翻页。
运行: python live_preview.py   (需 tkinter, Windows 自带 Python 已包含)

可选环境变量 (对应固件 USE_NTP=0 的"本地系统时间"):
  PREVIEW_TIME=HH:MM:SS    -> 时钟从该时刻起, 按真实时间走秒 (可看翻页动画)
  PREVIEW_DATE=YYYY-MM-DD  -> 日期行(农历/干支/节日/节气/月相)用该日期
  两者都不设 -> 全部跟随电脑当前时间。
"""
import os
import time
import tkinter as tk
from PIL import Image, ImageDraw, ImageFont, ImageTk

import make_preview as mp  # 复用 scene()/draw_flap_card()/draw_roller() (有 __main__ 守卫, import 不产生副作用)

# ---- 日期条 1-bit 模拟 (模拟当前硬件 XFont::drawGlyph) ----
F16_1BIT = ImageFont.truetype(r"C:\Windows\Fonts\simhei.ttf", 16)

def _is_ascii(ch):
    return ord(ch) < 128

def _render_1bit(img, x, y, text, color):
    """模拟 XFont 1-bit 效果: 16px cell, 阈值, ASCII 半宽 8px."""
    for ch in text:
        box = Image.new("L", (16, 16), 0)
        ImageDraw.Draw(box).text((0, 0), ch, font=F16_1BIT, fill=255, anchor="la")
        box = box.point(lambda p: 255 if p >= 128 else 0)
        w = 8 if _is_ascii(ch) else 16
        tile = Image.new("RGB", (w, 16), color)
        img.paste(tile, (int(x), y), box.crop((0, 0, w, 16)))
        x += w + 1
    return x

def _w1bit(text):
    w = sum((8 if _is_ascii(c) else 16) + 1 for c in text)
    return w - 1

def _draw_date_1bit(img, d, info):
    """1-bit 日期条 (模拟当前硬件效果)."""
    cx = (mp.DATE_LEFT + mp.DATE_RIGHT) // 2
    # 行1
    d.rounded_rectangle([mp.DATE_LEFT, mp.ROW1_TOP, mp.DATE_RIGHT, mp.ROW1_TOP + mp.ROW1_H - 1],
                        radius=8, fill=mp.ROW1_BG)
    cy1 = mp.ROW1_TOP + mp.ROW1_H // 2
    x = mp.DATE_LEFT + 12
    x = _render_1bit(img, x, cy1 - 8, info["date"], mp.WHITE)
    x += 8
    x = _render_1bit(img, x, cy1 - 8, info["week"], mp.CYAN)
    d.line([cx, mp.ROW1_TOP + 9, cx, mp.ROW1_TOP + mp.ROW1_H - 9], fill=mp.DATE_SEP)
    x = cx + 12
    x = _render_1bit(img, x, cy1 - 8, info["lunar"], mp.WHITE)
    x += 6
    x = _render_1bit(img, x, cy1 - 8, info["gz"], mp.GOLD)
    if info["fest"]:
        x += 6
        x = _render_1bit(img, x, cy1 - 8, "今日" + info["fest"], mp.event_color(info["fest"]))
    # 行2
    d.rounded_rectangle([mp.DATE_LEFT, mp.ROW2_TOP, mp.DATE_RIGHT, mp.ROW2_TOP + mp.ROW2_H - 1],
                        radius=8, fill=mp.ROW2_BG)
    cy2 = mp.ROW2_TOP + mp.ROW2_H // 2
    xj = mp.DATE_LEFT + 12
    if info["cur_d"] == 0:
        xj = _render_1bit(img, xj, cy2 - 8, "今日" + info["cur"], mp.event_color(info["cur"]))
        xj += 10
        xj = _render_1bit(img, xj, cy2 - 8, "%s %d天后" % (info["nxt"], info["nxt_d"]), mp.SOFT)
    else:
        xj = _render_1bit(img, xj, cy2 - 8,
                           "%s 已过%d天   %s %d天后" % (info["cur"], info["cur_d"], info["nxt"], info["nxt_d"]),
                           mp.SOFT)
    # 右侧: 月相 + 温度 + 湿度 (右对齐)
    h_str = "%d%%" % mp.MOCK_HUMIDITY
    t_str = "%d°C" % int(mp.MOCK_TEMP)
    moon_str = info["moon"]
    total_w = _w1bit(h_str) + 8 + _w1bit(t_str) + 8 + _w1bit(moon_str) + 6 + 18
    x = mp.DATE_RIGHT - 12 - total_w
    x = _render_1bit(img, x, cy2 - 8, moon_str, mp.MOON_TXT)
    x += 6
    mp.draw_moon(d, int(x) + 9, cy2, 9, info["mp"])
    x += 18
    x = _render_1bit(img, x, cy2 - 8, t_str, mp.temp_color(int(mp.MOCK_TEMP)))
    x += 8
    x = _render_1bit(img, x, cy2 - 8, h_str, mp.humid_color(mp.MOCK_HUMIDITY))

FLIP_TOP_MS = 150      # 新节奏: 上半翻片下压
FLIP_BOT_MS = 130      # 下半翻片上翻
TICK_MS = 40           # 刷新间隔
PITCH = mp.ROLLER_PITCH  # 齿轮每翻完一相走一格

# 样式: 0=机械翻页(默认) 1=指针表盘 (与 main.cpp clockStyle 一致)
STYLE_FLAP, STYLE_ANALOG = 0, 1

class Sim:
    """两相 split-flap 状态机: 上半翻片下压露出新上半 -> 下半翻片上翻补全新下半。
    同时按每位翻页进度累计齿轮滚动量, 使齿轮与相邻数字翻牌同步(一相=一格, 缓动一致)。"""
    def __init__(self):
        self.displayed = ["0"] * 6   # 当前显示的 6 位
        self.flipping = [None] * 6   # 每位 (old, new, start_ms)
        self.clicks = [0] * 6        # 每位已完成的"相"数 => 累计滚动 = clicks*PITCH
        self.primed = False

    def tick(self, target, now):
        if not self.primed:
            # 首次进入: 直接显示, 不翻页 (对应固件 modeDrawn 分支)
            self.primed = True
            for i in range(6):
                self.displayed[i] = target[i]
                self.flipping[i] = None
            return target, [(0, 0.0, 0)] * 6, 0

        digits = []
        flips = []
        scroll = 0.0
        for i in range(6):
            f = self.flipping[i]
            if f is None and target[i] != self.displayed[i]:
                f = (self.displayed[i], target[i], now)
                self.flipping[i] = f
            if f is None:
                digits.append(self.displayed[i])
                flips.append((0, 0.0, 0))
                scroll += self.clicks[i] * PITCH
                continue
            old, new, start = f
            oldi = int(old)               # draw_flap_card 用整数索引 HALF_TOP/BOT
            el = now - start
            if el < FLIP_TOP_MS:
                p = el / FLIP_TOP_MS
                digits.append(new)
                flips.append((1, p, oldi))
                scroll += self.clicks[i] * PITCH + mp._ease(p) * PITCH
            elif el < FLIP_TOP_MS + FLIP_BOT_MS:
                if self.clicks[i] < 1:
                    self.clicks[i] = 1
                p = (el - FLIP_TOP_MS) / FLIP_BOT_MS
                digits.append(new)
                flips.append((2, p, oldi))
                scroll += self.clicks[i] * PITCH + mp._ease(p) * PITCH
            else:
                if self.clicks[i] < 2:
                    self.clicks[i] = 2
                self.displayed[i] = new
                self.flipping[i] = None
                digits.append(new)
                flips.append((0, 0.0, 0))
                scroll += self.clicks[i] * PITCH
        return "".join(digits), flips, scroll


def main():
    root = tk.Tk()
    root.resizable(False, False)
    cv = tk.Canvas(root, width=480, height=320, bg="black", highlightthickness=0)
    cv.pack()
    item = cv.create_image(0, 0, anchor="nw")
    sim = Sim()

    # 时钟样式 (与真机点击切换一致): 默认翻页, 按 "空格" 或 "s" 切换 翻页/指针
    style = [STYLE_FLAP]
    label = tk.Label(root, text=" 时钟/日历/收音机/天气   |   [空格] 切换 翻页 ⇄ 指针   |   当前: 机械翻页 ",
                     bg="#121212", fg="#00FFFF", font=("Microsoft YaHei", 9), anchor="w")
    label.pack(side="bottom", fill="x")

    # 日期条字体模式: False=AA(当前硬件, preview 默认) / True=1-bit(旧硬件, 对比用)
    font1bit = [False]

    def set_title():
        name = "机械翻页" if style[0] == STYLE_FLAP else "指针表盘"
        fmode = "1-bit(旧硬件,对比)" if font1bit[0] else "AA(当前硬件)"
        root.title("ESP32 实时模拟 | %s | 日期条: %s | [空格]样式 [f]字体" % (name, fmode))
        label.config(text=" 样式: %s   |   日期条: %s   |   [空格] 翻页⇄指针   |   [f] AA⇄1-bit(旧) " % (name, fmode))

    def toggle_style(e=None):
        style[0] = STYLE_FLAP if style[0] == STYLE_ANALOG else STYLE_ANALOG
        set_title()
        print("style -> %s" % ("flap" if style[0] == STYLE_FLAP else "analog"))
        return "break"

    root.bind("<space>", toggle_style)
    root.bind("s", toggle_style)
    root.bind("S", toggle_style)
    set_title()

    # 可选: 用环境变量覆盖时间 (对应固件 USE_NTP=0 的"本地系统时间")
    #   PREVIEW_TIME=HH:MM:SS  -> 时钟从该时刻起, 按真实时间走秒 (可看翻页动画)
    #   PREVIEW_DATE=YYYY-MM-DD-> 日期行(农历/干支/节日/节气/月相)用该日期
    # 两者都不设 -> 全部跟随电脑当前时间。
    base_sec = None
    pv = os.environ.get("PREVIEW_TIME", "").strip()
    if pv:
        parts = pv.split(":")
        if len(parts) == 3:
            base_sec = (int(parts[0]) * 3600 + int(parts[1]) * 60 + int(parts[2])) % 86400
            print("PREVIEW_TIME=%s -> 时钟从 %s 起按真实时间走秒 (Ctrl+C 退出)" % (pv, pv))
        else:
            print("PREVIEW_TIME 需 HH:MM:SS 格式, 已忽略, 用系统时间")

    t0 = time.time()
    img_ref_holder = [None]
    info_cache = [None]   # 日期字符串 (每秒算一次)

    def scene_1bit(digits, flips, gear_scroll):
        """与 mp.scene 相同, 但日期条用 1-bit 渲染 (模拟旧硬件, 对比用)."""
        img = Image.new("RGB", (480, 320), (0, 0, 0))
        d = ImageDraw.Draw(img)
        if info_cache[0] is None:
            info_cache[0] = mp.date_strings()
        _draw_date_1bit(img, d, info_cache[0])
        for i, ch in enumerate(digits):
            new_d = ord(ch) - 48
            phase, progress, old_d = flips[i]
            if phase == 0:
                mp.draw_flap_card(img, d, i, new_d, new_d, 0, 0.0)
            else:
                mp.draw_flap_card(img, d, i, old_d, new_d, phase, progress)
        mp.draw_roller(d, mp.GAP1_CX, mp.CARD_TOP, mp.CARD_TOP + mp.CARD_H - 1, gear_scroll)
        mp.draw_roller(d, mp.GAP2_CX, mp.CARD_TOP, mp.CARD_TOP + mp.CARD_H - 1, gear_scroll)
        mp.draw_mode_hint(d, 0)
        mp.draw_nav(d)
        return img

    def set_title():
        name = "机械翻页" if style[0] == STYLE_FLAP else "指针表盘"
        fmode = "1-bit(旧硬件,对比)" if font1bit[0] else "AA(当前硬件)"
        root.title("ESP32 实时模拟 | %s | 日期条: %s | [空格]样式 [f]字体" % (name, fmode))
        label.config(text=" 样式: %s   |   日期条: %s   |   [空格] 翻页⇄指针   |   [f] AA⇄1-bit(旧) " % (name, fmode))

    def toggle_font(e=None):
        font1bit[0] = not font1bit[0]
        info_cache[0] = mp.date_strings()   # 刷新 (跨日保护)
        set_title()
        print("font -> %s" % ("1bit" if font1bit[0] else "AA"))
        return "break"

    root.bind("f", toggle_font)
    root.bind("F", toggle_font)
    set_title()

    def step():
        now_ms = (time.time() - t0) * 1000
        ms_real = int(time.time() * 1000) % 1000   # 真实毫秒, 秒针平滑扫秒

        if style[0] == STYLE_ANALOG:
            # 指针表盘: 用毫秒平滑扫秒 (对应固件 modeClock 每帧重绘 + millis 秒针)
            if base_sec is not None:
                tot = (base_sec + int(time.time() - t0)) % 86400
                h, mi, se = tot // 3600, (tot % 3600) // 60, tot % 60
            else:
                n = time.localtime()
                h, mi, se = n.tm_hour, n.tm_min, n.tm_sec
            if font1bit[0]:
                # 指针样式 + 1-bit 日期条
                img = mp.draw_analog_scene(h, mi, se, ms_real)
                d = ImageDraw.Draw(img)
                # 覆盖日期条: 清场后画 1-bit
                d.rectangle([mp.DATE_LEFT, mp.ROW1_TOP - 2, mp.DATE_RIGHT, mp.ROW2_TOP + mp.ROW2_H + 2], fill=(0, 0, 0))
                if info_cache[0] is None:
                    info_cache[0] = mp.date_strings()
                _draw_date_1bit(img, d, info_cache[0])
            else:
                img = mp.draw_analog_scene(h, mi, se, ms_real)
        else:
            if base_sec is not None:
                tot = (base_sec + int(time.time() - t0)) % 86400
                target = "%02d%02d%02d" % (tot // 3600, (tot % 3600) // 60, tot % 60)
            else:
                n = time.localtime()
                target = "%02d%02d%02d" % (n.tm_hour, n.tm_min, n.tm_sec)
            digits, flips, scroll = sim.tick(target, now_ms)
            if font1bit[0]:
                img = scene_1bit(digits, flips, scroll)
            else:
                img = mp.scene(digits, flips, gears=True, white=True, gear_scroll=scroll)

        img_ref_holder[0] = ImageTk.PhotoImage(img)
        cv.itemconfig(item, image=img_ref_holder[0])
        root.after(TICK_MS, step)

    step()
    root.mainloop()

if __name__ == "__main__":
    main()
