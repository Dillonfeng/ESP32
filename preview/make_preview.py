#!/usr/bin/env python3
"""make_preview.py - 用真实 digits.h 遮罩 + main.cpp 布局/配色,
1:1 模拟 ST7796 480x320 (rot1) split-flap 时钟效果。
关键视觉: 翻片两相 + cosθ透视压缩 + 铰链渐变 + 缓动; 间隙为贯穿全卡高的
竖直滚动齿轮, 滚动量随相邻数字翻页进度联动(一次翻页=走一格)。

日期/节气/天气:
  - 农历取自 nongli_data.py (与 nongli.cpp 同一套 17 位表 + 积日算法,
    已全量校验 0 误差), 不再硬编码。
  - 当前节气 + 下一节气(距今天数) 由天文节气表 JQ_DOY 计算。
  - 温度/湿度为演示用 mock 值(占位), 布局与最终 widget 一致。
"""
import math
import re
import sys
import os
import json
import datetime
import bisect
import urllib.request
import time
from PIL import Image, ImageDraw, ImageFont

DIGITS_H = r"D:\ESP32-S3\include\digits.h"
DIALNUM_H = r"D:\ESP32-S3\include\dialnum.h"
ALMNUM_H = r"D:\ESP32-S3\include\almnum.h"
OUT_DIR = r"D:\ESP32-S3\preview"
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nongli_data
# 方向 A: 模拟硬件 init 链 (镜像 main.cpp setup() 的加载-校验-失败路径)。
# 模块加载即触发 HW = run() 打印 boot log (镜像硬件每次启动 boot)。
# HW.font16_ok 决定 hw_text() 走 AA 字库 (与硬件 compositeGlyph 同款) 还是降级灰色矩形
# (镜像硬件字库加载失败时中文空白). 故障注入: 环境变量 HW_SIMULATE_FAIL=font/psram/
# byteorder/db/fs/festival 复现已知 bug 验证降级画面. 详见 hw_init_sim.py 文档.
import hw_init_sim
HW = hw_init_sim.HW

CARD_W, CARD_H, CARD_TOP, CARD_RADIUS = 56, 120, 48, 10
DIGIT_W, DIGIT_H = 46, 88
CARD_X = [31, 93, 181, 243, 331, 393]
NAV_Y, NAV_H, NAV_BTN_W = 288, 32, 120
DATE_LEFT, DATE_RIGHT = 31, 449
ROW1_TOP, ROW1_H = 186, 46   # 主日期行 (公历/星期 | 农历/干支)
ROW2_TOP, ROW2_H = 240, 36   # 详情行 (节气 | 月相+天气)
GAP1_CX = (CARD_X[1] + CARD_W + CARD_X[2]) // 2   # 165
GAP2_CX = (CARD_X[3] + CARD_W + CARD_X[4]) // 2   # 315

def c565(h):
    return (((h >> 11) * 255 // 31,
             ((h >> 5) & 0x3F) * 255 // 63,
             (h & 0x1F) * 255 // 31))

CARD_FACE  = c565(0x6318)
CARD_SEAM  = c565(0x3A48) # 铰链: 中央 1px 细缝 (冷灰)
CARD_SHADE = c565(0x1084)
CARD_EDGE  = c565(0x39C6)
# 铰链配色 (与 main.cpp 一致): 1px 细缝 + 两端 3x3 金属转轴帽 (受光高光->中灰->暗沟)
HINGE_HI   = c565(0x9514) # 转轴帽受光高光
HINGE_MID  = c565(0x4ACB) # 转轴帽中部
HINGE_LO   = c565(0x1945) # 转轴帽底沿
ROW1_BG    = c565(0x2146)   # 主日期行底色 (深灰蓝, 突出)
ROW2_BG    = c565(0x18E3)   # 详情行底色 (近黑)
DATE_STRIP = c565(0x18E3)   # (兼容保留) 详情行底色
DATE_SEP   = c565(0x0821)   # 分隔竖线
NAV_BG     = c565(0x2104)
NAV_SEL    = c565(0x4A69)
CYAN       = c565(0x07FE)   # 青色 (星期 / 湿度)
BLACK      = c565(0x0000)   # 黑 (与 main.cpp TFT_BLACK 一致)
GOLD       = c565(0xFE4C)   # 金色 (干支)

CARD_FACE_W = c565(0xFFFF)  # 白底 (0xFFFF = 纯白)

# 节假日高亮 (与 main.cpp CAL_OFF/CAL_WORK 完全一致)
HOL_OFF  = c565(0xF800)   # 放假(休) - 红 (与周末同色)
HOL_WORK = c565(0x6CF1)   # 补班(班) - 浅蓝 (深色底用)
# 日历浅色主题 (仅日历页; 其余页面仍深色) — 与 main.cpp CAL_* 同步
CAL_LIGHT_BG = c565(0xFFFF)
CAL_GRID     = c565(0xC65A)   # table 网格线 (浅灰蓝)
CAL_HDR      = c565(0x5ACB)   # 星期表头 / 箭头 (深灰)
CAL_TITLE    = c565(0x12D9)   # 月份标题 / "今日" (蓝)
CAL_NORM     = c565(0x31A6)   # 普通日期数字 (深灰)
CAL_STRIP_L  = c565(0xEF9F)   # 底部信息条底色 (浅蓝灰)
CAL_GOLD_L   = c565(0xCC21)   # 干支 (深金, 白底可读)
HOL_WORK_L   = c565(0x01F4)   # 补班 (深蓝, 浅色日历底可读)
# 天气页配色 (与 main.cpp W_* 完全一致)
W_BG      = c565(0x0000)   # 黑 (COLOR_BG)
W_PANEL   = c565(0x2104)   # 卡片底 深灰蓝
W_EDGE    = c565(0x4208)   # 卡片描边
W_TEXT    = c565(0xFFFF)   # 主文字 白
W_DIM     = c565(0xBDF7)   # 次级文字 浅灰
W_ACCENT  = c565(0x07FF)   # 强调 青
W_SUN     = c565(0xFFE0)   # 太阳 黄
W_CLOUD   = c565(0xFFFF)   # 云 白
W_RAIN    = c565(0x4DDF)   # 雨 浅蓝
W_SNOW    = c565(0x7DFF)   # 雪 淡蓝
W_WARN    = c565(0xFD20)   # 闪电 橙黄
# 年月选择弹层几何 (与 main.cpp PICK_* 一致)
PICK_PANEL_X = 20
PICK_PANEL_W = 440
PICK_PT      = 40
PICK_PB      = 250
PICK_MCOL_W  = 60
PICK_MROW_H  = 40
PICK_MGRID_X = 60          # (480 - 6*60)/2
PICK_MTOP    = 128
# 节假日缓存: 原始 JSON (由 sync_holidays.py 拉取, 与固件 LittleFS 同内容)
HOL_CACHE = os.path.join(OUT_DIR, "holidays_%d.json")
_hol_cache = {}           # year -> {"MM-DD": {"holiday":bool,"name":str}}

def load_holidays(year):
    """读取 preview/holidays_<year>.json (离线用缓存; 缺文件返回空). 与固件 boot 读缓存一致。"""
    if year in _hol_cache:
        return _hol_cache[year]
    d = {}
    p = HOL_CACHE % year
    if os.path.exists(p):
        try:
            with open(p, encoding="utf-8") as f:
                d = json.load(f).get("holiday", {})
        except Exception:
            d = {}
    _hol_cache[year] = d
    return d

def hol_day_type(year, month, day):
    """返回 0=普通, 1=放假, 2=补班 (与固件 holiday::dayType 一致)。"""
    key = "%02d-%02d" % (month, day)
    e = load_holidays(year).get(key)
    if not e:
        return 0
    return 1 if e.get("holiday", False) else 2

# 信息 widget 配色
WHITE      = (255, 255, 255)
SOFT       = c565(0x94F6)   # 节气 - 灰蓝
MOON_TXT   = c565(0xDE72)   # 月相 - 淡金
MOON_LIT   = c565(0xFF97)   # 月亮亮部 - 淡黄
MOON_DARK  = c565(0x2146)   # 月亮暗部
MOON_EDGE  = c565(0x424B)   # 月亮轮廓
TEMP_C     = c565(0xF800)   # 温度 - 橙红
HUMID_C    = c565(0x07FE)   # 湿度 - 青

# 节日/节气 当天 ("今日XX") 用色 (16bit, 与 main.cpp eventColor 完全一致)
EVENT_COLORS = {
    # 24 节气
    "春分": 0xFE4C, "秋分": 0xFE4C,
    "清明": 0x07E0, "谷雨": 0x07E0, "立春": 0x07E0, "雨水": 0x07E0,
    "惊蛰": 0x07E0, "立夏": 0x07E0, "白露": 0x07E0,
    "冬至": 0x001F, "小寒": 0x001F, "大寒": 0x001F, "小雪": 0x001F,
    "大雪": 0x001F, "立冬": 0x001F, "霜降": 0x001F, "寒露": 0x001F,
    "夏至": 0x07FF,
    "小暑": 0xFFE0, "大暑": 0xFFE0, "立秋": 0xFFE0, "处暑": 0xFFE0,
    "小满": 0xFFE0, "芒种": 0xFFE0,
    # 节日
    "春节": 0xF800, "元旦": 0xF800, "国庆节": 0xF800, "建党节": 0xF800,
    "建军节": 0xF800, "劳动节": 0xF800, "情人节": 0xF800, "圣诞节": 0xF800,
    "元宵节": 0xFE4C, "中秋节": 0xFE4C, "重阳节": 0xFE4C, "腊八节": 0xFE4C,
    "小年": 0xFE4C, "龙抬头": 0xFE4C, "端午节": 0xFE4C, "青年节": 0xFE4C,
}
_DEFAULT_EVENT_COLOR = SOFT
def event_color(name):
    return c565(EVENT_COLORS.get(name, _DEFAULT_EVENT_COLOR))

# ---------- 收音机页配色 (与 main.cpp 0x1042/0x0821/0x3186 等 565 值完全一致) ----------
RADIO_HEADER_BG  = c565(0x1042)   # 顶部标题栏底
RADIO_SIDE_BG    = c565(0x0821)   # 左侧分类栏底
RADIO_SIDE_SEL   = c565(0x210C)   # 选中分类底
RADIO_PLAY_BG    = c565(0x0821)   # 底部播放栏底
RADIO_LINE       = c565(0x3186)   # 分隔线 (竖/横)
RADIO_ROW_DIV    = c565(0x2104)   # 电台行分隔线
RADIO_PLAYING_BG = c565(0x1843)   # 正在播放行底
RADIO_SB_TRACK   = c565(0x1082)   # 滚动条轨道
RADIO_LIVE_BG    = c565(0x07E0)   # LIVE 标签底 (绿)
RADIO_PAUSE_BG   = c565(0xFBE0)   # PAUSE 标签底 (红)
RADIO_FAV_OFF    = c565(0x5A5A)   # 未收藏 (灰)
RADIO_BTN_PLAY   = c565(0x07E0)   # 播放按钮底 (绿)
RED              = c565(0xF800)   # (TFT_RED)
GREEN            = c565(0x07E0)   # (TFT_GREEN)
YELLOW           = c565(0xFFE0)   # (TFT_YELLOW)
DIM              = c565(0x7BEF)   # (COLOR_DIM 暗灰)

# ---------- 收音机几何 (与 main.cpp RADIO_* 一致) ----------
R_SIDEBAR_W = 70
R_HEADER_H  = 28
R_PLAYBAR_H = 80
R_LIST_X    = 70
R_LIST_W    = 390
R_SB_W      = 8
R_ROW_H     = 45                            # 行高 (180//45=4 行, 比 55px 3 行更密)
R_ROWS      = 4
R_LIST_TOP  = 28                       # = RADIO_HEADER_H
R_LIST_BOT  = NAV_Y - R_PLAYBAR_H      # = 208
R_LIST_H    = R_LIST_BOT - R_LIST_TOP  # = 180

# 温度颜色: 随数值变化 (与 main.cpp tempColor 完全一致)
# 冰点以下用浅蓝 (0x6CF1) 而非纯蓝 (0x001F), 避免在深色背景上不可读
def temp_color(c):
    if c <= 0:  return c565(0x6CF1)   # 冰点以下 - 浅蓝 (高对比度)
    if c <= 8:  return c565(0x07FF)   # 冷 - 青
    if c <= 15: return c565(0x07E0)   # 凉 - 绿
    if c <= 24: return c565(0xFE4C)   # 舒适 - 金
    if c <= 29: return c565(0xFFE0)   # 热 - 黄
    return c565(0xF800)               # 炎热 - 红 (>30°)

# 湿度颜色: 随数值变化 (与 main.cpp humidColor 完全一致)
def humid_color(h):
    if h <= 30: return c565(0xFFE0)   # 过干 - 黄
    if h <= 45: return c565(0xFE4C)   # 偏干 - 金
    if h <= 65: return c565(0x07E0)   # 舒适 - 绿
    if h <= 80: return c565(0x07FF)   # 偏湿 - 青
    return c565(0x6CF1)               # 过湿 - 浅蓝 (高对比度)


def load_font(px):
    for p in [r"C:\Windows\Fonts\msyhbd.ttc", r"C:\Windows\Fonts\simhei.ttf",
              r"C:\Windows\Fonts\msyh.ttc"]:
        try:
            return ImageFont.truetype(p, px)
        except OSError:
            pass
    raise RuntimeError("no CJK font found")

F10, F12, F16, F18, F20 = load_font(10), load_font(12), load_font(16), load_font(18), load_font(20)

# ---------- AA 8-bit 灰度字库 (font_simhei_16.font, 与硬件 font16 同源) ----------
# 角标 "休"/"班" 在硬件上用 16x16 AA 字形缩放到 12x12 (XFont::drawScaledChar)。
# 预览用同一份 AA blob + 同样的"面积平均 + 逐通道 (bg*(255-m)+fg*m)/255 合成", 逐像素一致。
_AA_FONT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        os.pardir, "data", "font_simhei_16.font")
_AA_CACHE = {}   # path -> (unis, advs, pix)
_AA_CACHE32 = {} # path -> (unis, advs, pix, size)
def _aa_load(path):
    if path in _AA_CACHE:
        return _AA_CACHE[path]
    import struct
    try:
        d = open(path, "rb").read()
        assert d[0:4] == b"XAAX", "not an AA font: %s" % path
        size = 64 if d[4] == 3 else (24 if d[4] == 4 else (32 if d[4] == 2 else 16))   # ver3=64 ver4=24 ver2=32 ver1=16
        per = 3 + size * size
    except Exception:
        _AA_CACHE[path] = None
        return None
    n = struct.unpack(">H", d[6:8])[0]
    unis, advs, pix = [], [], d[8:]
    for i in range(n):
        g = pix[i * per:i * per + per]
        unis.append(struct.unpack(">H", g[0:2])[0])
        advs.append(g[2])
    _AA_CACHE[path] = (unis, advs, pix, size)
    return _AA_CACHE[path]

def _aa_load32(path):
    """加载 ver2 (32x32) / ver3 (64x64) 单元格 AA 字库 -> (unis, advs, pix, size). 镜像 XFont::loadAA."""
    if path in _AA_CACHE32:
        return _AA_CACHE32[path]
    import struct
    try:
        d = open(path, "rb").read()
        assert d[0:4] == b"XAAX", "not an AA font: %s" % path
        size = 64 if d[4] == 3 else (32 if d[4] == 2 else 16)   # ver1=16 ver2=32 ver3=64
        per = 3 + size * size
    except Exception:
        _AA_CACHE32[path] = None
        return None
    n = struct.unpack(">H", d[6:8])[0]
    unis, advs, pix = [], [], d[8:]
    for i in range(n):
        g = pix[i * per:i * per + per]
        unis.append(struct.unpack(">H", g[0:2])[0])
        advs.append(g[2])
    _AA_CACHE32[path] = (unis, advs, pix, size)
    return _AA_CACHE32[path]

# 64px 原生 AA 字库 (黄历大字; ver3)。与硬件 font32 同源 -> big_glyph 逐像素一致。
_AA_FONT32 = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          os.pardir, "data", "font_simhei_64.font")

def aa_compose_scaled_glyph(img, x, y, ch, dst, fg, bg):
    """在 img 的 (x,y)-(x+dst,y+dst) 画一个缩放 AA 中文角标 (镜像 XFont::drawScaledChar)。
    ch=unicode 码点; dst=目标边长(如 12); fg/bg=RGB 元组(前景/背景)。字库缺失则跳过。"""
    if dst < 1: dst = 1
    if dst > 24: dst = 24
    loaded = _aa_load(_AA_FONT)
    if not loaded:
        return
    unis, advs, pix, S = loaded
    per = 3 + S * S
    i = bisect.bisect_left(unis, ch)
    if i >= len(unis) or unis[i] != ch:
        return
    mcell = pix[i * per + 3: i * per + 3 + S * S]
    for oy in range(dst):
        sy0, sy1 = (oy * S) // dst, ((oy + 1) * S) // dst
        if sy1 <= sy0: sy1 = sy0 + 1
        for ox in range(dst):
            sx0, sx1 = (ox * S) // dst, ((ox + 1) * S) // dst
            if sx1 <= sx0: sx1 = sx0 + 1
            s = 0
            for sy in range(sy0, sy1):
                row = mcell[sy * S:(sy + 1) * S]
                s += sum(row[sx0:sx1])
            m = s // ((sy1 - sy0) * (sx1 - sx0))
            px, py = x + ox, y + oy
            if 0 <= px < img.width and 0 <= py < img.height:
                img.putpixel((px, py), (
                    (bg[0] * (255 - m) + fg[0] * m) // 255,
                    (bg[1] * (255 - m) + fg[1] * m) // 255,
                    (bg[2] * (255 - m) + fg[2] * m) // 255))

def big_glyph(img, x, y, ch, dst, fg, bg):
    """镜像 XFont::drawBigChar: 源 AA 灰度 (font32 32x32, 缺字回退 font16 16x16) 重采样到 dst x dst
    (面积平均 + 逐通道 565 合成) -> 边缘平滑, 与黄历中间大数字一样顺滑。
    x,y = 目标左上角; dst<=40。字库缺失则跳过。
    与硬件 almBigText 一致: font32.hasGlyph -> 32px 原生; 否则 font16 (16px)。"""
    if dst < 1: dst = 1
    if dst > 40: dst = 40
    mcell = None; S = 16
    f32 = _aa_load32(_AA_FONT32)
    if f32 is not None:
        u32, _a32, p32, S = f32
        per = 3 + S * S
        i32 = bisect.bisect_left(u32, ch)
        if i32 < len(u32) and u32[i32] == ch:
            mcell = p32[i32 * per + 3 : i32 * per + 3 + S * S]
    if mcell is None:
        loaded = _aa_load(_AA_FONT)
        if not loaded:
            return
        u16, _a16, p16, S = loaded
        per16 = 3 + S * S
        i16 = bisect.bisect_left(u16, ch)
        if i16 >= len(u16) or u16[i16] != ch:
            return
        mcell = p16[i16 * per16 + 3 : i16 * per16 + 3 + S * S]
    for oy in range(dst):
        sy0 = (oy * S) // dst
        sy1 = ((oy + 1) * S) // dst
        if sy1 <= sy0: sy1 = sy0 + 1
        if sy0 < 0: sy0 = 0
        if sy1 > S: sy1 = S
        for ox in range(dst):
            sx0 = (ox * S) // dst
            sx1 = ((ox + 1) * S) // dst
            if sx1 <= sx0: sx1 = sx0 + 1
            if sx0 < 0: sx0 = 0
            if sx1 > S: sx1 = S
            s = 0
            for yy in range(sy0, sy1):
                row = mcell[yy * S:(yy + 1) * S]
                s += sum(row[sx0:sx1])
            m = s // ((sy1 - sy0) * (sx1 - sx0))
            px, py = x + ox, y + oy
            if 0 <= px < img.width and 0 <= py < img.height:
                img.putpixel((px, py), (
                    (bg[0] * (255 - m) + fg[0] * m) // 255,
                    (bg[1] * (255 - m) + fg[1] * m) // 255,
                    (bg[2] * (255 - m) + fg[2] * m) // 255))


def big_text(d, x, y, text, dst, fg, bg, anchor="l"):
    """镜像 main.cpp almBigText: 逐字 big_glyph(dst)。CJK advance=dst, ASCII(数字/字母)=dst*7//10。
    x,y = 左上角; anchor 'l'=左对齐, 'c'=以 x 为中心。返回总宽(像素)。"""
    img = d._image if hasattr(d, "_image") else None
    adv = lambda ch: dst * 7 // 10 if ord(ch) < 128 else dst
    total = sum(adv(ch) for ch in text)
    if anchor == "c":
        x -= total // 2
    if img is None:
        d.text((x, y), text, font=F16, fill=fg)
        return total
    sx = x
    for ch in text:
        code = ord(ch)
        if code >= 0x10000:
            continue
        big_glyph(img, sx, y, code, dst, fg, bg)
        sx += adv(ch)
    return total


# ---------- 方向 A: hw_text() 镜像 XFont::drawStringFastAA ----------
# font16 OK -> 逐字 AA 合成 (与硬件 compositeGlyph 同款 out=(bg*(255-m)+fg*m)/255)
# font16 FAIL -> 画灰色占位矩形 (镜像硬件字库加载失败时中文空白)
# 这样 preview "看着好" 必蕴含 "硬件字库 init 也走通了", 不再靠 simhei.ttf 兜底画漂亮中文.

def _aa_advance(ch_code):
    """镜像 XFont::textWidthAA 单字 advance (字库缺失回退 16)."""
    loaded = _aa_load(_AA_FONT)
    if not loaded:
        return 8 if ch_code < 128 else 16
    unis, advs, _, _ = loaded
    i = bisect.bisect_left(unis, ch_code)
    if i < len(unis) and unis[i] == ch_code:
        return advs[i]
    return 8 if ch_code < 128 else 16

def aa_text_width(text):
    """镜像 XFont::textWidthAA: 逐字 advance 求和.

    Python 3 字符串已是 unicode 字符序列 (每字符 1 个码点), 不需要 UTF-8 字节解码.
    硬件 xfont.cpp 的 drawStringFastAA 用 UTF-8 字节流解码是因为 C++ String 是 UTF-8 字节;
    Python 这边直接 for ch in text 即可. 字库仅支持 BMP 平面 (code < 0x10000).
    """
    w = 0
    for ch in text:
        code = ord(ch)
        if code < 0x10000:
            w += _aa_advance(code)
    return w

def hw_text(d, x, y, text, fg, bg, anchor="lm"):
    """镜像 XFont::drawStringFastAA(str, fg, bg) 的中文绘制.

    - HW.font16_ok=True: 逐字 16x16 AA 合成 (调 aa_composite_scaled_glyph dst=16),
      cx += advance (CJK=16, ASCII=10, 'A'=12, '-'=7, '.'=5...), 与硬件逐字 advance 一致.
    - HW.font16_ok=False: 画和字宽相当的深灰色矩形, 镜像硬件字库加载失败时"中文空白".
      (硬件失败时中文位置不画, 留底色; 这里画一个比底色略亮的灰色块便于肉眼定位.)

    anchor: "lm"=left-middle (硬件左上锚点 (x, y-8)); "rm"=right-middle; "mm"=middle-middle.
    y 为竖直中心 (与 d.text 的 anchor="*m" 语义一致, 字形 16px 顶到 y-8 底到 y+7).

    Python 3 字符串已是 unicode 字符序列, 直接 for ch in text 迭代每字符 (1 码点),
    不需要 UTF-8 字节解码. 字库仅支持 BMP 平面 (code < 0x10000).
    """
    if not HW.font16_ok:
        # 镜像硬件"中文空白": 画一个和字宽相当的深灰色矩形 (比 bg 略亮便于定位)
        w = aa_text_width(text) if False else max(8, len(text) * 16)
        if anchor == "lm":
            rx = x
        elif anchor == "rm":
            rx = x - w
        else:  # mm
            rx = x - w // 2
        d.rectangle([rx, y - 8, rx + w, y + 8], fill=(48, 48, 52))
        return
    # AA 路径: 计算总宽 -> 按 anchor 算起始 cx -> 逐字合成
    total = aa_text_width(text)
    if anchor == "lm":
        cx = x
    elif anchor == "rm":
        cx = x - total
    else:  # mm
        cx = x - total // 2
    # 复用已加载 img (d 的 image), 逐字 dst=16 合成 (与硬件 compositeGlyph 同款)
    img = d._image if hasattr(d, "_image") else None
    if img is None:
        # ImageDraw.Draw 没有公开 _image, 但实际有; 万一拿不到就退化为 d.text
        d.text((x, y), text, font=F16, fill=fg, anchor=anchor)
        return
    for ch in text:
        code = ord(ch)
        if code >= 0x10000:
            continue   # 字库仅支持 BMP, 跳过 emoji 等
        aa_compose_scaled_glyph(img, cx, y - 8, code, 16, fg, bg)
        cx += _aa_advance(code)

def hw_small(d, x, ytop, text, size, fg, bg, anchor="l"):
    """镜像硬件 almScaledText 逐字 (BUILD21: 走 64px 字库下采样, 同 big_glyph 路径)。
    (x, ytop) = 左上角; advance: CJK=size, ASCII=size*7//10; anchor 'l'左 / 'c'中心。返回总宽。
    与硬件 almScaledText 一致: 每字 big_glyph (font32 64px 源, 缺字回退 font16) -> size x size。
    根因: 16px 源降到 13px 会把 2px 细笔画(如"三"底横)塌成 1px -> "底横截一半"; 64px 源保持 2px。"""
    if not HW.font16_ok:
        return 0
    adv = lambda code: size * 7 // 10 if code < 128 else size
    total = sum(adv(ord(ch)) for ch in text)
    if anchor == "c":
        x -= total // 2
    img = d._image if hasattr(d, "_image") else None
    if img is None:
        d.text((x, ytop + size // 2), text, font=load_font(size), fill=fg, anchor="lm")
        return total
    cx = x
    for ch in text:
        code = ord(ch)
        if code >= 0x10000:
            continue
        big_glyph(img, cx, ytop, code, size, fg, bg)
        cx += adv(code)
    return total


def hw_glyphs(d, x, y, text, fg, bg, size, anchor="mm"):
    """镜像硬件 font16/font24 逐字绘制, 但**字库没有的字(韩/藏/维/哈/emoji)留白** (与硬件一致).

    硬件契约 (xfont.cpp): font16(findAA miss) / font24(getCodeFromFile 空串) 对缺失字都
    "只前进 advance、不画像素" → 屏幕上该位置留底色 (空白)。raw PIL d.text 会画 □ 豆腐块,
    与硬件不符 (lockstep 破口)。本函数用同一份 AA blob 逐字合成, 字库里没有的码点直接跳过
    (不画 = 留底), 与硬件逐字 advance 一致; 真实 CJK 走 16x16 灰度 AA 再缩放到 size。

    size: 目标字形边长 (16=font16, 20/24=font24 近似)。anchor: mm/rm/lm (y 为竖直中心)。
    """
    if not HW.font16_ok:
        # 镜像硬件"字库加载失败=中文空白": 不画 (与 hw_text 的降级块不同, 这里直接留底色)
        return
    loaded = _aa_load(_AA_FONT)
    if not loaded:
        return
    unis, advs, _, _ = loaded
    # 逐字 advance (用 16px advance; 与 hw_text/aa_text_width 同款), 按 size 缩放
    def _adv16(code):
        i = bisect.bisect_left(unis, code)
        return advs[i] if (i < len(unis) and unis[i] == code) else (8 if code < 128 else 16)
    total = sum(_adv16(ord(ch)) for ch in text)
    total = int(total * size / 16)
    if anchor == "lm":
        cx = x
    elif anchor == "rm":
        cx = x - total
    else:  # mm
        cx = x - total // 2
    img = d._image if hasattr(d, "_image") else None
    if img is None:
        d.text((x, y), text, font=(F20 if size > 16 else F16), fill=fg, anchor=anchor)
        return
    for ch in text:
        code = ord(ch)
        if code >= 0x10000:
            continue   # 字库仅支持 BMP (与 hw_text 一致)
        i = bisect.bisect_left(unis, code)
        if i >= len(unis) or unis[i] != code:
            # 字库没有 -> 只前进, 不画 (镜像硬件空白)
            cx += int(_adv16(code) * size / 16)
            continue
        aa_compose_scaled_glyph(img, cx, y - size // 2, code, size, fg, bg)
        cx += int(_adv16(code) * size / 16)

# ---------- 农历 / 节气 计算 (与 nongli.cpp 完全一致) ----------
LUNAR_MONTHS = ["正", "二", "三", "四", "五", "六", "七", "八", "九", "十", "冬", "腊"]
LUNAR_DAYS = ["初一","初二","初三","初四","初五","初六","初七","初八","初九","初十",
              "十一","十二","十三","十四","十五","十六","十七","十八","十九","二十",
              "廿一","廿二","廿三","廿四","廿五","廿六","廿七","廿八","廿九","三十"]
ZODIAC = ["猴","鸡","狗","猪","鼠","牛","虎","兔","龙","蛇","马","羊"]  # 与 nl_sx_text 一致 (index = year%12)
GAN = ["庚","辛","壬","癸","甲","乙","丙","丁","戊","己"]             # 与 nl_tg_text 一致 (index = year%10)
ZHI = ["申","酉","戌","亥","子","丑","寅","卯","辰","巳","午","未"]   # 与 nl_dz_text 一致 (index = year%12)
WEEK_FULL = ["星期日","星期一","星期二","星期三","星期四","星期五","星期六"]
WEEK_CN = ["日","一","二","三","四","五","六"]   # (兼容保留)
MOON_NAMES = ["新月","娥眉月","上弦月","盈凸月","满月","亏凸月","下弦月","残月"]


def _g_leap(y):
    return (y % 4 == 0 and y % 100 != 0) or (y % 400 == 0)


def _days_in_month(y, m):
    dm = [31,28,31,30,31,30,31,31,30,31,30,31]
    return 29 if (m == 2 and _g_leap(y)) else dm[m-1]


def _dn(y, m, d):
    days = 0
    for yy in range(1900, y): days += 366 if _g_leap(yy) else 365
    for mm in range(1, m): days += _days_in_month(y, mm)
    return days + d - 1


def _ly_days(y):
    v = nongli_data.LUNAR_INFO[y-1900]; t = 0
    for m in range(1, 13): t += 30 if (v & (1 << (15-(m-1)))) else 29
    if v & 0x0F: t += 30 if (v & (1 << 16)) else 29
    return t


def _lm_days(y, m):
    return 30 if (nongli_data.LUNAR_INFO[y-1900] & (1 << (15-(m-1)))) else 29


def _ll_days(y):
    return 30 if (nongli_data.LUNAR_INFO[y-1900] & (1 << 16)) else 29


def lunar_solar_to_lunar(year, month, day):
    """返回 (lunar_year, lunar_month, lunar_day, is_leap). 与 nongli.cpp 一致。"""
    days = _dn(year, month, day) - 30
    ly = 1900
    while ly < 2100:
        L = _ly_days(ly)
        if days < L: break
        days -= L; ly += 1
    leap = nongli_data.LUNAR_INFO[ly-1900] & 0x0F
    m = 1; isleap = False
    while True:
        dim2 = _ll_days(ly) if isleap else _lm_days(ly, m)
        if days < dim2: break
        days -= dim2
        if (not isleap) and m == leap: isleap = True; continue
        isleap = False; m += 1
    return ly, m, days + 1, isleap


def zodiac_year(year, mon):
    """地支生肖年份: 农历10月及之后(春节后)取当年, 否则取前一年, 与 main.cpp 的 ly 规则一致。"""
    return year


def solar_terms(y, m, d):
    """返回 (当前节气名, 距今天数, 下一节气名, 距下一节气天数)。"""
    today = datetime.date(y, m, d)

    def doy_date(yy, doy):
        return datetime.date(yy, 1, 1) + datetime.timedelta(days=doy-1)

    best = None
    for yy in (y-1, y):
        for i in range(24):
            dt = doy_date(yy, nongli_data.JQ_DOY[yy-1900][i])
            if dt <= today and (best is None or dt > best[1]):
                best = (i, dt, nongli_data.JQ_NAMES[i])
    nxt = None
    for yy in (y, y+1):
        for i in range(24):
            dt = doy_date(yy, nongli_data.JQ_DOY[yy-1900][i])
            if dt > today and (nxt is None or dt < nxt[1]):
                nxt = (i, dt, nongli_data.JQ_NAMES[i])
    return best[2], (today-best[1]).days, nxt[2], (nxt[1]-today).days


def ganzi_year(ly):
    """干支生肖年: 如 2026 -> 丙午马年 (与 nl_tg/nl_dz/nl_sx_text 一致)。"""
    return "%s%s%s年" % (GAN[ly % 10], ZHI[ly % 12], ZODIAC[ly % 12])


def moon_phase(y, m, d):
    """月相 0..1 (0=新月, 0.5=满月)。基于儒略日与朔望月周期, 与 nongli.cpp 一致。"""
    yy, mm = y, m
    if mm <= 2:
        yy -= 1
        mm += 12
    A = yy // 100
    B = 2 - A + A // 4
    jd = math.floor(365.25 * (yy + 4716)) + math.floor(30.6001 * (mm + 1)) + d + B - 1524.5 + 12.0 / 24.0
    age = (jd - 2451550.1) % 29.530588853
    return age / 29.530588853


def moon_name(phase):
    return MOON_NAMES[int(phase * 8 + 0.5) % 8]


def load_masks():
    src = open(DIGITS_H, encoding="utf-8").read()
    masks = []
    for i in range(10):
        m = re.search(r"DigitMask%d\[\] PROGMEM = \{(.*?)\};" % i, src, re.S)
        vals = [int(v) for v in re.findall(r"\d+", m.group(1))]
        assert len(vals) >= DIGIT_W * DIGIT_H, "mask %d too short" % i
        masks.append(vals[: DIGIT_W * DIGIT_H])
    return masks


# 指针表盘数字位图 (与翻页时钟同款 Bahnschrift 字体): 解析 include/dialnum.h。
# 顺序 [0]="12",[1..9]="1".."9",[10]="10",[11]="11"; 灰度遮罩 0=笔画 255=透明。
def load_dialnum():
    src = open(DIALNUM_H, encoding="utf-8").read()
    ws = [int(v) for v in re.findall(r"\d+", re.search(r"DialNumW\[\d+\] PROGMEM = \{(.*?)\};", src, re.S).group(1))]
    hs = [int(v) for v in re.findall(r"\d+", re.search(r"DialNumH\[\d+\] PROGMEM = \{(.*?)\};", src, re.S).group(1))]
    nums = []
    for i in range(12):
        m = re.search(r"DialNum%d\[\d+\] PROGMEM = \{(.*?)\};" % i, src, re.S)
        vals = [int(v) for v in re.findall(r"\d+", m.group(1))]
        assert len(vals) >= ws[i] * hs[i], "dialnum %d too short" % i
        nums.append((vals[: ws[i] * hs[i]], ws[i], hs[i]))
    return nums


DIALNUMS = load_dialnum()


# 黄历大数字位图 (62x112, Bahnschrift w700): 解析 include/almnum.h。
# 8-bit 灰度遮罩 0=笔画 255=底 (与 almBigNum 合成同款)。
def load_almnum():
    src = open(ALMNUM_H, encoding="utf-8").read()
    dw = int(re.search(r"ALM_DW (\d+)", src).group(1))
    dh = int(re.search(r"ALM_DH (\d+)", src).group(1))
    masks = []
    for i in range(10):
        m = re.search(r"AlmMask%d\[\d+\] PROGMEM = \{(.*?)\};" % i, src, re.S)
        vals = [int(v) for v in re.findall(r"\d+", m.group(1))]
        assert len(vals) >= dw * dh, "almnum %d too short" % i
        masks.append(vals[: dw * dh])
    return dw, dh, masks


ALM_DW, ALM_DH, ALM_MASKS = load_almnum()
_DIALNUM_IMG = {}
def get_dialnum(idx, face_rgb):
    """合成数字位图到指定背景色 (同 composite): out = bg*mask/255。"""
    if idx not in _DIALNUM_IMG or _DIALNUM_IMG[idx][0] != face_rgb:
        vals, w, h = DIALNUMS[idx]
        R, G, B = face_rgb
        m = Image.frombytes("L", (w, h), bytes(vals))
        out = Image.new("RGB", (w, h))
        pm, po = m.load(), out.load()
        for y in range(h):
            for x in range(w):
                v = pm[x, y]
                po[x, y] = (R * v // 255, G * v // 255, B * v // 255)
        _DIALNUM_IMG[idx] = (face_rgb, out)
    return _DIALNUM_IMG[idx][1]

def composite(mask, bg_rgb):
    R, G, B = bg_rgb
    m = Image.frombytes("L", (DIGIT_W, DIGIT_H), bytes(mask))
    out = Image.new("RGB", (DIGIT_W, DIGIT_H))
    pm, po = m.load(), out.load()
    for y in range(DIGIT_H):
        for x in range(DIGIT_W):
            v = pm[x, y]
            po[x, y] = (R * v // 255, G * v // 255, B * v // 255)
    return out


MASKS = load_masks()
_DIG_CACHE = {}
def get_digit(d, bg_rgb):
    key = (d, bg_rgb)
    if key not in _DIG_CACHE:
        _DIG_CACHE[key] = composite(MASKS[d], bg_rgb)
    return _DIG_CACHE[key]

# ---- 半牌预计算: 白底 + 数字, 裁成上/下半 (56x60) ----
# 圆角外的区域为黑色, 与屏幕背景一致, 贴上去不会把圆角填成直角。
def _build_half(d):
    card = Image.new("RGB", (CARD_W, CARD_H), (0, 0, 0))
    dc = ImageDraw.Draw(card)
    dc.rounded_rectangle([0, 0, CARD_W - 1, CARD_H - 1], radius=CARD_RADIUS,
                         fill=(255, 255, 255))
    card.paste(get_digit(d, (255, 255, 255)), ((CARD_W - DIGIT_W) // 2,
                                              (CARD_H - DIGIT_H) // 2))
    half = CARD_H // 2
    return card.crop((0, 0, CARD_W, half)), card.crop((0, half, CARD_W, CARD_H))

HALF_TOP, HALF_BOT = [], []
for _d in range(10):
    t, b = _build_half(_d)
    HALF_TOP.append(t)
    HALF_BOT.append(b)


def _ease(p):
    """smoothstep 缓动: 慢-快-慢, 让翻片有惯性/落位感(而不是匀速)。"""
    p = max(0.0, min(1.0, p))
    return p * p * (3 - 2 * p)

def _draw_hinge(d, x, seamY, w):
    """铰链: 中央 1px 细缝 + 两端 3x3 金属转轴帽 (受光高光->中灰->暗沟 竖渐变)。
    与硬件 drawHingeCard/hingeBufCard/bandCard 逐像素一致。x=牌左缘, seamY=缝 y。"""
    d.line([x + 4, seamY, x + w - 5, seamY], fill=CARD_SEAM)   # 中央 1px 细缝 (两端转轴帽之间)
    kc = [HINGE_HI, HINGE_HI, HINGE_MID, HINGE_MID, HINGE_LO, HINGE_LO]   # 顶高光->中->底暗 (两端 4x6 帽)
    for r in range(6):
        y = seamY - 3 + r
        d.line([x, y, x + 3, y], fill=kc[r])                  # 左转轴帽 (4px 宽)
        d.line([x + w - 4, y, x + w - 1, y], fill=kc[r])      # 右转轴帽 (4px 宽)


def _shade_band(old_half, bandH, strength, flip=False):
    """把旧半牌竖向压缩到 bandH 高, 并按竖向渐变变暗(靠近铰链端亮、翻过去那端暗),
    模拟翻片在空间中转动 (cosθ 透视压缩 + 背面变暗)。"""
    H = max(1, bandH)
    sq = old_half.resize((CARD_W, H), Image.LANCZOS)
    out = Image.new("RGB", sq.size, (0, 0, 0))
    sp, op = sq.load(), out.load()
    for yy in range(H):
        t = (H - 1 - yy) / max(1, H - 1) if flip else yy / max(1, H - 1)                    # 0 靠近铰链 .. 1 翻过去的远端
        a = 0.12 + 0.70 * strength * t           # 越远越暗 (翻片转到背面)
        for xx in range(CARD_W):
            r, g, b = sp[xx, yy]
            op[xx, yy] = (int(r * (1 - a)), int(g * (1 - a)), int(b * (1 - a)))
    return out


def draw_flap_card(img, d, idx, old_d, new_d, phase, progress):
    """phase 0=稳态, 1=上半翻片下压(露出新上半, 旧上半压向铰链),
    2=下半翻片上翻(补全新下半, 旧下半压向铰链)。progress 0..1。"""
    x, y = CARD_X[idx], CARD_TOP
    w, h = CARD_W, CARD_H
    half = h // 2
    seamY = y + half
    face = CARD_FACE_W
    # 下缘投影 + 牌面
    d.rounded_rectangle([x, y + 3, x + w - 1, y + h + 2], radius=CARD_RADIUS, fill=CARD_EDGE)
    d.rounded_rectangle([x, y, x + w - 1, y + h - 1], radius=CARD_RADIUS, fill=face)

    p = _ease(progress)

    if phase == 0:
        # 稳态: 整牌显示新数字
        img.paste(get_digit(new_d, face), (x + (w - DIGIT_W) // 2, y + (h - DIGIT_H) // 2))
    elif phase == 1:
        # 上半翻片下落: 露出的新上半; 下半仍旧数字; 旧上半压缩压向铰链
        img.paste(HALF_TOP[new_d], (x, y))
        img.paste(HALF_BOT[old_d], (x, seamY))
        bandH = max(1, int(round((1 - p) * half)))
        band = _shade_band(HALF_TOP[old_d], bandH, p)
        img.paste(band, (x, seamY - bandH))
        d.line([x + 1, seamY - bandH, x + w - 2, seamY - bandH], fill=(120, 120, 135))
    else:
        # 下半翻片上翻: 已锁定的新上半; 露出的新下半; 旧下半压缩压向铰链
        img.paste(HALF_TOP[new_d], (x, y))
        img.paste(HALF_BOT[new_d], (x, seamY))
        bandH = max(1, int(round((1 - p) * half)))
        band = _shade_band(HALF_BOT[old_d], bandH, p, flip=True)
        img.paste(band, (x, y + h - bandH))
        d.line([x + 1, y + h - bandH, x + w - 2, y + h - bandH], fill=(120, 120, 135))

    # 金属铰链 (5px 渐变条 + 两端转轴钉, 必须在数字之后画) —— 与硬件 drawHingeCard/hingeBufCard 一致
    _draw_hinge(d, x, seamY, w)


# ---- 竖直滚动齿轮: 一整条贯穿卡片的金属圆柱, 齿纹随翻页进度上下滚动 ----
ROLLER_W = 16          # 圆柱直径 (侧面视角, 宽 16px)
ROLLER_PITCH = 14      # 齿距 (翻一次 = 走一格 = 滚 PITCH px)


def _roller_col_profile(x, W):
    """圆柱截面横向明暗: 两侧(轮廓)暗, 中间亮, 并带一条窄中央高光带,
    模拟圆柱受正面光照射的立体感。返回 0..1 的亮度因子。"""
    t = x / max(1, W - 1)
    g = 0.20 + 0.80 * math.sin(t * math.pi)          # 0..1, 中心亮
    spec = max(0.0, 1.0 - abs(t - 0.5) / 0.16) * 0.30  # 中央窄高光
    return g + spec


def _tooth_factor(ly):
    """单个齿的周期明暗 (梯形波 = 基波 - 二次谐波, 平顶平谷, 更像机械齿而非正弦)。
    ly 为齿纹在齿距内的局部位置(0..PITCH)。返回 0..1 的因子。"""
    th = (ly / ROLLER_PITCH) * 2 * math.pi
    return 0.62 + 0.38 * math.cos(th) - 0.08 * math.cos(2 * th)


def draw_roller(d, cx, top, bottom, scroll):
    """在间隙 (cx, top..bottom) 画竖直滚动的 3D 圆柱齿轮:
    - 无轴心圆; 齿是缠绕在圆柱表面的一圈圈环纹, 随 scroll 向下滚动(方向不变)。
    - 横向左→亮→右的圆柱明暗 + 中央高光, 让圆柱有立体感。
    scroll 为累计滚动距离(px): 越大齿轮向下滚越多, 齿纹随之移动。"""
    W = ROLLER_W
    x0, x1 = cx - W // 2, cx + W // 2 - 1
    y0, y1 = top, bottom
    H = y1 - y0 + 1
    # 预计算每列的圆柱明暗 (含中央高光)
    colB = [_roller_col_profile(x, W) for x in range(W)]
    phase = scroll % ROLLER_PITCH
    for yy in range(H):
        ly = (yy + phase) % ROLLER_PITCH           # 局部齿位 (随 scroll 下移, 方向不变)
        tf = _tooth_factor(ly)
        for x in range(W):
            v = min(1.0, colB[x] * tf)
            c = int(24 + 210 * v)                  # 24..234 灰度
            cb = min(255, c + 8)                   # 轻微偏蓝, 金属感
            d.point((x0 + x, y0 + yy), fill=(c, c, cb))
    # 左右轮廓线 (圆柱边缘最暗, 收边)
    d.line([x0, y0, x0, y1], fill=(40, 40, 52))
    d.line([x1, y0, x1, y1], fill=(40, 40, 52))
    # 上下端盖 (圆柱两端的鼓轮端缘), 稍暗收边, 增加机械感
    for x in range(W):
        c = int(40 + 150 * math.sin((x / (W - 1)) * math.pi))
        d.point((x0 + x, y0), fill=(c - 24, c - 24, c - 16))
        d.point((x0 + x, y1), fill=(c - 24, c - 24, c - 16))

def draw_nav(d, sel=0):
    d.rectangle([0, NAV_Y, 479, 319], fill=NAV_BG)
    for i, lab in enumerate(["时钟", "日历", "收音机", "天气"]):
        x = i * NAV_BTN_W
        if i == sel:
            d.rounded_rectangle([x + 4, NAV_Y + 3, x + NAV_BTN_W - 5, NAV_Y + NAV_H - 4],
                                radius=6, fill=NAV_SEL)
        d.text((x + NAV_BTN_W // 2, 303), lab, font=F16,
               fill=c565(0xFFE0) if i == sel else (255, 255, 255), anchor="mm")  # 选中=黄字, 未选=白字 (与 main.cpp 一致)

# ============================================================
# 收音机: 解析 data/stations.bin (与 main.cpp loadRadioDB/loadRegionStations 一致)
# 关键: 多字节字段为**小端** (f.read()|(f.read()<<8) = LE), 与 convert_channels.py 的
# struct.pack('<H'/'<I') 一致。region 表每 28 字节 (name16 + first32 + count16 + res6)。
# ============================================================
STATIONS_BIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, "data", "stations.bin")
BACKUP_JSON  = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir,
                            "海燕收音机_备份_20260912_195414.backup.json")

# ---- 收音机分类结构 (镜像参考项目 HEADER_CATEGORIES / TAIL_CATEGORIES / 定位省份置顶) ----
#   顶部功能分类: 全部 / 收藏 / 历史 / 个人
#   地区分类 HEAD: 全国 / 中央 / 电视伴音(侧栏 2 字"伴音") / 国际
#   定位省份 (RADIO_PINNED) 插到 HEAD 之后第一位, 其余省份按 RADIO_REGION_ORDER
#   尾部: 海外 / 其它
RADIO_META         = ["全部", "收藏", "历史", "个人"]
RADIO_REGION_HEAD  = ["全国", "中央", "电视伴音", "国际"]
RADIO_REGION_TAIL  = ["海外", "其它"]
RADIO_REGION_ORDER = ["山东", "浙江", "河北", "江苏", "河南", "吉林", "四川", "安徽", "山西", "辽宁",
                      "湖北", "云南", "新疆", "黑龙江", "湖南", "江西", "广西", "内蒙古", "贵州", "福建",
                      "北京", "陕西", "青海", "上海", "香港", "重庆", "宁夏", "甘肃", "台湾", "海南",
                      "西藏", "天津", "澳门"]
RADIO_PINNED       = "广东"                        # 定位省份 (backup last_location._key = 广东)
RADIO_LABEL_MAP    = {"电视伴音": "伴音", "黑龙江": "龙江", "内蒙古": "内蒙"}           # 侧栏 4 字 -> 2 字 (与参考项目一致)
# 侧栏几何 (侧栏可上下拖动): 顶部 4 个特殊行(全部/收藏/历史/个人)固定,
# 地区分类列表在下方滚动 (与参考项目一致).
R_SPECIAL_CNT   = 4                             # 特殊行数
R_SPECIAL_H     = 22                            # 顶部特殊行(全部/收藏/历史/个人)每行高
R_ROW_H_SIDE    = 24                            # 地区行每行高: 侧栏铺满到导航栏 (NAV_Y-R_REGION_TOP)=172, 7 行留 4px
R_REGION_TOP    = R_HEADER_H + R_SPECIAL_CNT * R_SPECIAL_H   # 地区滚动区顶部 = 28 + 4*22 = 116
# 分类配色 (与参考一致; 电台图标底色按"分类"取色, 24-bit hex -> 565)
RADIO_CAT_COLORS = {
    "交通": 0x3498DB, "综合": 0x34495E, "经济": 0xF39C12, "新闻": 0xE74C3C,
    "文艺": 0xE67E22, "音乐": 0x9B59B6, "体育": 0x2ECC71, "农村": 0x27AE60,
    "影视": 0xD35400, "教育": 0x2980B9, "生活": 0x1ABC9C,
}
def _hex565(hexc):
    """24-bit 0xRRGGBB -> 565 色元组 (与参考分类配色一致)."""
    r = (hexc >> 16) & 0xFF; g = (hexc >> 8) & 0xFF; b = hexc & 0xFF
    return c565(((r >> 3) << 5) | ((g >> 2) << 1) | (b >> 3))
_radio_cache = None

def _radio_label(key):
    return RADIO_LABEL_MAP.get(key, key)

def _hex_to_rgb(h):
    h = (h or "").lstrip("#")
    if len(h) != 6:
        return (200, 200, 200)
    try:
        return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16))
    except Exception:
        return (200, 200, 200)

class _RadioStation(object):
    __slots__ = ("name", "freq", "region_idx", "region_key", "category",
                 "r", "g", "b", "bin_offset", "sid", "is_user")

def _hw_trunc_name(name):
    """镜像硬件 StationView.name(char[32]) 的截断规则: nameLen>31 时取前 28 字节+\"...\"
    (loadRegionStations / extra 加载同款). 硬件 32 字节字段放不下长名 -> preview 也必须截断,
    否则长名电台(如 朝鲜어방송) preview 全名 / 硬件截断 = lockstep 破口."""
    b = (name or "").encode("utf-8")
    if len(b) <= 31:
        return name
    t = b[:28]
    # 退到最后一个合法 UTF-8 边界 (丢弃被截断的半个多字节字符, 与硬件"显示不出残字"一致)
    try:
        return t.decode("utf-8", "ignore") + "..."
    except Exception:
        return t.decode("utf-8", "replace") + "..."

def _make_station(name, freq, region_key, color, sid, is_user, category="其它"):
    st = _RadioStation()
    r, g, b = _hex_to_rgb(color)
    st.name, st.freq, st.region_idx = _hw_trunc_name(name), freq, -1
    st.region_key, st.category = region_key, (category or "其它")
    st.r, st.g, st.b = r, g, b
    st.bin_offset, st.sid, st.is_user = 0, sid, is_user
    return st

def _read_u16le(b, o): return b[o] | (b[o+1] << 8)
def _read_u32le(b, o): return b[o] | (b[o+1] << 8) | (b[o+2] << 16) | (b[o+3] << 24)

def _parse_bin_channels():
    """stations.bin -> [ _RadioStation ] (仅地区, 无 ID/功能分类; backup 缺失时的兜底)."""
    d = open(STATIONS_BIN, "rb").read()
    if d[0:8] != b"ESP32RDB" or d[8] != 2:
        raise ValueError("bad format")
    num_regions = _read_u16le(d, 9)
    region_idx_offset = _read_u32le(d, 15)
    rnames = []
    for i in range(num_regions):
        off = region_idx_offset + i * 28
        rnames.append(d[off:off+16].rstrip(b"\x00").decode("utf-8", "replace"))
        first = _read_u32le(d, off + 16)
        count = _read_u16le(d, off + 20)
        pos = first
        for _ in range(count):
            nlen = d[pos]; pos += 1
            nm = d[pos:pos+nlen].decode("utf-8", "replace"); pos += nlen
            ulen = _read_u16le(d, pos); pos += 2
            pos += ulen
            ridx = d[pos]; pos += 1
            clen = d[pos]; pos += 1
            pos += clen
            flen = d[pos]; pos += 1
            frq = d[pos:pos+flen].decode("utf-8", "replace"); pos += flen
            r, g, b = d[pos], d[pos+1], d[pos+2]; pos += 3
            st = _make_station(nm, frq, rnames[ridx], None, "", False)
            st.r, st.g, st.b = r, g, b
            yield st

# ============================================================
# 定位省份(收音机侧栏置顶) -- 镜像硬件 geo_pin.cpp + mobile app.js autoDetectProvinceOnLaunch
#   本板无 GPS, 只走 IP 兜底 (ip-api.com, 纯 HTTP 无 TLS). 结果缓存到 preview/geo_loc.json
#   (镜像硬件 LittleFS /geo_loc.json), 24h 刷新, 失败保留上次值.
#   环境变量 PREVIEW_PROVINCE 可覆盖(调试/离线); PREVIEW_OFFLINE=1 时不联网(只用缓存/兜底).
#   返回"完整省名"(如 黑龙江/内蒙古), 空则退回 RADIO_PINNED(广东).
# ============================================================
GEO_CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "geo_loc.json")
GEO_REFRESH = 24 * 3600
_GEO_EN_MAP = {
    "Beijing": "北京", "Shanghai": "上海", "Tianjin": "天津", "Chongqing": "重庆",
    "Hebei": "河北", "Shanxi": "山西", "Liaoning": "辽宁", "Jilin": "吉林", "Heilongjiang": "黑龙江",
    "Jiangsu": "江苏", "Zhejiang": "浙江", "Anhui": "安徽", "Fujian": "福建", "Jiangxi": "江西", "Shandong": "山东",
    "Henan": "河南", "Hubei": "湖北", "Hunan": "湖南", "Guangdong": "广东", "Guangxi": "广西", "Hainan": "海南",
    "Sichuan": "四川", "Guizhou": "贵州", "Yunnan": "云南", "Tibet": "西藏",
    "Shaanxi": "陕西", "Gansu": "甘肃", "Qinghai": "青海", "Ningxia": "宁夏", "Xinjiang": "新疆",
    "Inner Mongolia": "内蒙古", "Nei Mongol": "内蒙古",
    "Hong Kong": "香港", "Macau": "澳门", "Macao": "澳门", "Taiwan": "台湾",
}
def _locate_province():
    """镜像硬件 geo::pinned(): IP 反查省份(完整名) + 缓存. 失败/离线返回 None(调用方退回兜底)."""
    ov = os.environ.get("PREVIEW_PROVINCE")
    if ov:
        return ov
    cached, ts = None, 0.0
    try:
        with open(GEO_CACHE, encoding="utf-8") as fh:
            d = json.load(fh)
        cached = (d.get("province") or None)
        ts = float(d.get("ts", 0))
    except Exception:
        pass
    offline = os.environ.get("PREVIEW_OFFLINE") == "1"
    if (not cached or (time.time() - ts > GEO_REFRESH)) and not offline:
        try:
            req = urllib.request.Request(
                "http://ip-api.com/json/?fields=status,regionName",
                headers={"User-Agent": "haiyan-radio-preview"})
            with urllib.request.urlopen(req, timeout=8) as r:
                doc = json.loads(r.read().decode("utf-8"))
            if doc.get("status") == "success":
                cn = _GEO_EN_MAP.get(doc.get("regionName", ""))
                if cn:
                    cached = cn
                    try:
                        with open(GEO_CACHE, "w", encoding="utf-8") as fh:
                            json.dump({"province": cn, "regionName": doc.get("regionName", ""),
                                       "ts": time.time()}, fh, ensure_ascii=False)
                    except Exception:
                        pass
        except Exception:
            pass   # 失败保留上次缓存 (或 None -> 调用方退回 广东)
    return cached

def load_radio():
    """构建收音机分类模型 (镜像参考项目 + 海燕收音机 backup 数据)。
    返回 dict:
      'cats'    : [ {'label','key','meta','stations':[_RadioStation..]} ]  (侧边栏顺序)
      'regions' : [region_key, ...]  (station.region_idx 指向这里, 用于 "· 山东" 显示)
    优先读 backup JSON (含 ID/收藏/历史/个人/定位); 缺失则退回 stations.bin (仅 全部+地区分类)。
    """
    global _radio_cache
    if _radio_cache is not None:
        return _radio_cache

    regions = []
    def ridx_of(key):
        if key not in regions:
            regions.append(key)
        return regions.index(key)

    channels, user_st, fav_ids, hist_ids = [], [], [], []
    used_backup = False
    try:
        raw = json.load(open(BACKUP_JSON, encoding="utf-8"))
        def inner(k):
            v = raw.get(k, "[]")
            return json.loads(v) if isinstance(v, str) else v
        for c in inner("radio_channels").get("radio", []):
            channels.append(_make_station(c.get("name", ""), c.get("frequency", ""),
                                          c.get("description", "其它"), c.get("color"),
                                          c.get("id", ""), False, c.get("category", "其它")))
        user_st = [_make_station(u.get("name", ""), u.get("frequency", ""),
                                 u.get("description", "其它"), u.get("color"),
                                 u.get("id", ""), True, u.get("category", "其它"))
                   for u in inner("radio_user_stations")]
        fav_ids = list(inner("radio_favorites"))
        hist_ids = list(inner("radio_history"))
        used_backup = True
    except Exception as e:
        print("backup JSON 读取失败, 退回 stations.bin: %s" % e)
        try:
            channels = list(_parse_bin_channels())
        except Exception as e2:
            print("stations.bin 读取也失败: %s" % e2)

    for st in channels + user_st:
        st.region_idx = ridx_of(st.region_key)

    by_id = {}
    for st in channels + user_st:
        by_id[st.sid] = st
    pool = channels + user_st

    def region_cats(key):
        return [s for s in channels if s.region_key == key]

    cats = []
    def add_cat(label, key, meta, stations):
        cats.append({"label": label, "key": key, "meta": meta, "stations": stations})

    add_cat("全部", "全部", True, pool)
    if used_backup:
        add_cat("收藏", "收藏", True, [by_id[i] for i in fav_ids if i in by_id])
        add_cat("历史", "历史", True, [by_id[i] for i in hist_ids if i in by_id])
        add_cat("个人", "个人", True, list(user_st))
    for key in RADIO_REGION_HEAD:
        add_cat(_radio_label(key), key, False, region_cats(key))
    pinned = _locate_province() or RADIO_PINNED     # 定位省份(完整名), 失败退回 RADIO_PINNED(广东)
    if pinned not in RADIO_REGION_HEAD:
        add_cat(_radio_label(pinned), pinned, False, region_cats(pinned))
    for key in RADIO_REGION_ORDER:
        if key == pinned:
            continue
        add_cat(_radio_label(key), key, False, region_cats(key))
    for key in RADIO_REGION_TAIL:
        add_cat(_radio_label(key), key, False, region_cats(key))

    _radio_cache = {"cats": cats, "regions": regions}
    return _radio_cache

def radio_cats():
    return load_radio()["cats"]

def radio_region_name(st):
    """电台行的 "· 山东" 地区文字 (station.region_idx -> regions 下标)."""
    regions = load_radio()["regions"]
    return regions[st.region_idx] if 0 <= st.region_idx < len(regions) else ""

def radio_cat_stations(ci):
    cats = radio_cats()
    return cats[ci]["stations"] if 0 <= ci < len(cats) else []

def radio_cat_label(ci):
    cats = radio_cats()
    return cats[ci]["label"] if 0 <= ci < len(cats) else ""

def _station_icon(st):
    """图标内文字 (与参考一致): 电台名首字.
    例: 青岛交通广播->"青"  菏泽交通广播->"荷"  广东新闻广播->"广"."""
    if len(st.name) > 0:
        return st.name[0]
    return "?"

def _station_cat(st):
    """电台的分类名 (图标底色 + 图标下方小字标签). 无分类则取频率."""
    c = st.category if st.category and st.category != "其它" else ""
    return c or st.freq

def _fit(text, maxw):
    """把文字截到不超过 maxw (像素), 超出加 "…". 用 aa_text_width 估宽."""
    try:
        if aa_text_width(text) <= maxw:
            return text
    except Exception:
        return text
    while len(text) > 1 and aa_text_width(text + "…") > maxw:
        text = text[:-1]
    return text + "…"

def draw_radio_header(d, cur_cat):
    d.rectangle([0, 0, 479, R_HEADER_H - 1], fill=RADIO_HEADER_BG)
    cats = radio_cats()
    if cats:
        label = radio_cat_label(cur_cat)
        cnt = len(cats[cur_cat]["stations"]) if cur_cat < len(cats) else 0
        title = "收音机· %s (%d)" % (label, cnt)
    else:
        title = "收音机· (无数据)"
    hw_text(d, 6, R_HEADER_H // 2, title, WHITE, RADIO_HEADER_BG, anchor="lm")
    # 右侧: 无网络 (镜像固件 wifiConnected 分支; 预览无 WiFi -> 显示"无网络")
    hw_text(d, 470, R_HEADER_H // 2, "无网络", RED, RADIO_HEADER_BG, anchor="rm")
    d.line([R_SIDEBAR_W - 1, R_HEADER_H, R_SIDEBAR_W - 1, R_LIST_BOT - 1], fill=RADIO_LINE)

def draw_radio_sidebar(d, cur_cat, region_scroll_top=0):
    """左侧分类栏: 固定4行(全部/收藏/历史/个人) + 地区行 整列一起滚动 (镜像 main.cpp 2026-09-27)。
    region_scroll_top 为像素偏移。选中分类 = 红色字 + 红色左条。"""
    cats = radio_cats()
    n_spec = R_SPECIAL_CNT
    n = len(cats)
    S = region_scroll_top
    top = R_HEADER_H - S
    d.rectangle([0, R_HEADER_H, R_SIDEBAR_W - 1, NAV_Y - 1], fill=RADIO_SIDE_BG)
    for i, c in enumerate(cats):
        rh = R_SPECIAL_H if i < n_spec else R_ROW_H_SIDE
        base = i * R_SPECIAL_H if i < n_spec else n_spec * R_SPECIAL_H + (i - n_spec) * R_ROW_H_SIDE
        ry = top + base
        y0 = max(ry, R_HEADER_H)
        fy1 = min(ry + rh - 1, NAV_Y - 1)
        if fy1 < y0:
            continue
        sel = (i == cur_cat)
        bg = RADIO_SIDE_SEL if sel else RADIO_SIDE_BG
        fg = RED if sel else DIM
        d.rectangle([0, y0, R_SIDEBAR_W - 2, fy1], fill=bg)
        if sel:
            d.line([0, y0, 0, fy1], fill=RED)
        ty = ry + (rh - 16) // 2
        if ty >= y0 and ty + 15 <= fy1:
            hw_text(d, 4, ty + 8, c["label"], fg, bg, anchor="lm")
    # 侧栏滚动条 (右侧细条, 整列, 仅当总高 > 视口)
    d.line([R_SIDEBAR_W - 1, R_HEADER_H, R_SIDEBAR_W - 1, NAV_Y - 1], fill=RADIO_LINE)
    total_h = n_spec * R_SPECIAL_H + max(0, n - n_spec) * R_ROW_H_SIDE
    view_h = NAV_Y - R_HEADER_H
    if total_h > view_h:
        thumb_h = max(12, view_h * view_h // total_h)
        smax = total_h - view_h
        thumb_y = R_HEADER_H + (view_h - thumb_h) * S // smax
        d.rectangle([R_SIDEBAR_W - 3, thumb_y, R_SIDEBAR_W - 1, thumb_y + thumb_h - 1], fill=WHITE)
    d.line([R_SIDEBAR_W - 1, R_HEADER_H, R_SIDEBAR_W - 1, NAV_Y - 1], fill=RADIO_LINE)

def _draw_station_icon(d, cx, cy, st, size=22):
    """参考式图标: 圆角方形(按分类取色) + 电台名首字 + 下方小字(分类名)."""
    half = size // 2
    cat = _station_cat(st)
    col = _hex565(RADIO_CAT_COLORS.get(cat, 0x34495E))
    d.rounded_rectangle([cx - half, cy - half, cx + half, cy + half], radius=6, fill=col)
    hw_glyphs(d, cx, cy - 1, _station_icon(st), WHITE, col, 16, anchor="mm")
    # 下方小字标签 (分类名, 截到 4 字)
    lbl = cat[:4]
    d.text((cx, cy + half + 3), lbl, font=F10, fill=DIM, anchor="mm")

def draw_radio_station_row(d, st, row_y, playing=False, favorited=False):
    bg = RADIO_PLAYING_BG if playing else BLACK
    d.rectangle([R_LIST_X, row_y, R_LIST_X + R_LIST_W - 1, row_y + R_ROW_H - 1], fill=bg)
    # 圆角方形图标 (按分类取色 + 名首字 + 分类小字), 居中
    icon_cx, icon_cy = R_LIST_X + 24, row_y + R_ROW_H // 2
    _draw_station_icon(d, icon_cx, icon_cy, st, 20)
    # 电台名 + 频率·地区 (46px 行高: 名称上 12 / 信息下 30)
    name_x = R_LIST_X + 46
    hw_text(d, name_x, row_y + 12, st.name, WHITE, bg, anchor="lm")
    info = "%s · %s" % (st.freq, radio_region_name(st))
    hw_text(d, name_x, row_y + 30, info, DIM, bg, anchor="lm")
    # 右侧: 播放中=绿三角 / 否则爱心
    if playing:
        d.polygon([(R_LIST_X + R_LIST_W - 14, row_y + 14),
                   (R_LIST_X + R_LIST_W - 6, row_y + 10),
                   (R_LIST_X + R_LIST_W - 6, row_y + 18)], fill=GREEN)
    else:
        hc = RED if favorited else DIM
        ax = R_LIST_X + R_LIST_W - 14
        ay = row_y + R_ROW_H // 2 - 2
        d.ellipse([ax - 8, ay - 6, ax, ay + 2], fill=hc)
        d.ellipse([ax, ay - 6, ax + 8, ay + 2], fill=hc)
        d.polygon([(ax - 8, ay), (ax + 8, ay), (ax, ay + 8)], fill=hc)
    d.line([R_LIST_X, row_y + R_ROW_H - 1, R_LIST_X + R_LIST_W - 1, row_y + R_ROW_H - 1], fill=RADIO_ROW_DIV)

def draw_radio_scrollbar(d, count, list_scroll_top):
    track_x = R_LIST_X + R_LIST_W
    d.rectangle([track_x, R_LIST_TOP, track_x + R_SB_W - 1, R_LIST_BOT - 1], fill=RADIO_SB_TRACK)
    if count > 0:
        thumb_h = max(20, R_LIST_H * R_ROWS // count)
        scroll_range = max(1, count - R_ROWS)
        thumb_y = R_LIST_TOP if scroll_range == 0 else \
            R_LIST_TOP + (R_LIST_H - thumb_h) * list_scroll_top // scroll_range
        d.rectangle([track_x + 1, thumb_y, track_x + R_SB_W - 2, thumb_y + thumb_h - 1], fill=WHITE)

def draw_radio_list(d, cur_cat, list_scroll_top=0, playing_idx=-1, favorites=()):
    d.rectangle([R_LIST_X, R_LIST_TOP, R_LIST_X + R_LIST_W - 1, R_LIST_BOT - 1], fill=BLACK)
    stations = radio_cat_stations(cur_cat)
    count = len(stations)
    if count == 0:
        d.text((R_LIST_X + 50, R_LIST_TOP + 50), "暂无电台", font=F16, fill=RED, anchor="lm")
        return
    start = list_scroll_top
    end = min(start + R_ROWS, count)
    fav_set = set(favorites)
    for i in range(start, end):
        row_y = R_LIST_TOP + (i - start) * R_ROW_H
        playing = (i == playing_idx)
        favorited = stations[i].sid in fav_set
        draw_radio_station_row(d, stations[i], row_y, playing, favorited)
    draw_radio_scrollbar(d, count, list_scroll_top)

def draw_radio_playbar(d, cur_cat, playing_idx=-1, playing=False, favorited=False):
    bar_x, bar_y = R_LIST_X, R_LIST_BOT
    bar_w, bar_h = R_LIST_W, R_PLAYBAR_H
    d.rectangle([bar_x, bar_y, bar_x + bar_w - 1, bar_y + bar_h - 1], fill=RADIO_PLAY_BG)
    d.line([bar_x, bar_y, bar_x + bar_w - 1, bar_y], fill=RADIO_LINE)
    stations = radio_cat_stations(cur_cat)
    if playing_idx < 0 or playing_idx >= len(stations):
        # 未播放: 仅控件行 (列表图标/名留白, 居中提示)
        _draw_playbar_controls(d, bar_x, bar_y + bar_h // 2 - 14, playing=False,
                               favorited=favorited, progress=0.0)
        hw_text(d, bar_x + 44, bar_y + 16, "点击电台开始播放", DIM, RADIO_PLAY_BG, anchor="lm")
        return
    st = stations[playing_idx]
    # 行1: 图标 + 电台名 (左) + "线路1 分类" (右)
    cat = _station_cat(st)
    _draw_station_icon(d, bar_x + 18, bar_y + 18, st, 18)
    right_lbl = "线路1 %s" % cat
    name_x = bar_x + 38
    name_maxw = (bar_x + bar_w - 10) - aa_text_width(right_lbl) - 8 - name_x
    hw_text(d, name_x, bar_y + 18, _fit(st.name, name_maxw), WHITE, RADIO_PLAY_BG, anchor="lm")
    hw_text(d, bar_x + bar_w - 10, bar_y + 18, right_lbl, DIM, RADIO_PLAY_BG, anchor="rm")
    # 行2: 进度条 (占位, 模拟播放中进度)
    d.rectangle([bar_x + 4, bar_y + 34, bar_x + bar_w - 5, bar_y + 34 + 2], fill=RADIO_SB_TRACK)
    d.rectangle([bar_x + 4, bar_y + 34, bar_x + 4 + int((bar_w - 9) * 0.4), bar_y + 34 + 2], fill=GREEN)
    # 行3: 控件 (≡/上一/播放暂停/下一) + 定时关闭 + 时间
    _draw_playbar_controls(d, bar_x, bar_y + 52, playing=playing,
                           favorited=favorited, progress=0.4)

def _draw_playbar_controls(d, bar_x, cy, playing=False, favorited=False, progress=0.0):
    """底部播放栏控件行: ≡  [上一] [播放/暂停] [下一]  定时关闭  时间.
    上一/播放/下一 三键以播放栏水平中心 (bar_x+R_LIST_W/2) 为轴居中,
    定时关闭 + 时间靠右 (不占用居中的三键)."""
    cx0 = bar_x + R_LIST_W // 2                 # 播放栏水平中心 (=bar_x+195)
    # 播放列表 (≡) 左端
    d.line([bar_x + 12, cy - 4, bar_x + 24, cy - 4], fill=WHITE)
    d.line([bar_x + 12, cy, bar_x + 24, cy], fill=WHITE)
    d.line([bar_x + 12, cy + 4, bar_x + 24, cy + 4], fill=WHITE)
    # 上一曲 ◀| (左指三角 + 左竖线, 中心 = cx0-45)
    px = cx0 - 56
    d.polygon([(px + 18, cy - 6), (px + 18, cy + 6), (px + 8, cy)], fill=WHITE)
    d.rectangle([px + 5, cy - 6, px + 8, cy + 6], fill=WHITE)
    # 播放/暂停 (大圆按钮, 居中)
    pp_cx, pp_cy = cx0, cy
    d.ellipse([pp_cx - 15, pp_cy - 15, pp_cx + 15, pp_cy + 15], fill=RADIO_BTN_PLAY)
    if not playing:
        d.polygon([(pp_cx - 4, pp_cy - 7), (pp_cx + 6, pp_cy), (pp_cx - 4, pp_cy + 7)], fill=WHITE)
    else:
        d.rectangle([pp_cx - 6, pp_cy - 6, pp_cx - 3, pp_cy + 6], fill=WHITE)
        d.rectangle([pp_cx + 2, pp_cy - 6, pp_cx + 5, pp_cy + 6], fill=WHITE)
    # 下一曲 ▶| (中心 = cx0+45)
    nx = cx0 + 38
    d.polygon([(nx, cy - 6), (nx, cy + 6), (nx + 10, cy)], fill=WHITE)
    d.rectangle([nx + 10, cy - 6, nx + 13, cy + 6], fill=WHITE)
    # 定时关闭 (下一曲右侧) + 时间 (最右, 右对齐)
    hw_text(d, cx0 + 55, cy, "定时关闭", DIM, RADIO_PLAY_BG, anchor="lm")
    d.text((bar_x + 384, cy), "00:00 / 00:00", font=F10, fill=DIM, anchor="rm")

def draw_radio_fullscreen(d, cur_cat, playing_idx=-1, favorited=False):
    d.rectangle([0, 0, 479, NAV_Y - 1], fill=(0, 0, 0))   # 半透明黑幕 (PIL 无 alpha, 用纯黑近似)
    stations = radio_cat_stations(cur_cat)
    if playing_idx < 0 or playing_idx >= len(stations):
        return
    st = stations[playing_idx]
    cx, cy = 240, 140
    d.rounded_rectangle([60, 40, 60 + 359, 40 + 199], radius=8, fill=c565(0x1042))
    col = ((st.r >> 3) << 5) | ((st.g >> 2) << 1) | (st.b >> 3)
    if col == 0:
        col = 0xCCCC
    d.ellipse([cx - 40, 80 - 40, cx + 40, 80 + 40], fill=c565(col))
    hw_glyphs(d, cx, 80, _station_icon(st), WHITE, c565(col), 20, anchor="mm")
    hw_glyphs(d, cx, 130, st.name, WHITE, c565(0x1042), 20, anchor="mm")
    info = "%s · %s" % (st.freq, radio_region_name(st))
    hw_text(d, cx, 165, info, DIM, c565(0x1042), anchor="mm")
    fav_text = "★ 已收藏" if favorited else "☆ 收藏"
    hw_text(d, cx, 195, fav_text, YELLOW if favorited else DIM, c565(0x1042), anchor="mm")
    hw_text(d, cx, 225, "点屏幕任意处关闭", DIM, c565(0x1042), anchor="mm")

def draw_radio_scene(cur_cat=0, list_scroll_top=0, region_scroll_top=0,
                    playing_idx=-1, playing=False, favorites=(), fullscreen=False):
    """收音机页面 (镜像参考项目: 分类侧栏 全部/收藏/历史/个人 + 地区分类, 电台列表, 播放栏, 全屏)."""
    img = Image.new("RGB", (480, 320), (0, 0, 0))
    d = ImageDraw.Draw(img)
    draw_radio_header(d, cur_cat)
    draw_radio_sidebar(d, cur_cat, region_scroll_top)
    draw_radio_list(d, cur_cat, list_scroll_top, playing_idx, favorites)
    stations = radio_cat_stations(cur_cat)
    cur_fav = (playing_idx >= 0 and playing_idx < len(stations)
               and stations[playing_idx].sid in set(favorites))
    draw_radio_playbar(d, cur_cat, playing_idx, playing, cur_fav)
    if fullscreen:
        draw_radio_fullscreen(d, cur_cat, playing_idx, cur_fav)
    draw_nav(d, sel=2)   # 导航栏选中"收音机"
    return img

# 预览日期: 默认取"今天"(与设备 NTP 一致, 方便对照),
# 可用环境变量 PREVIEW_DATE=YYYY-MM-DD 覆盖 (测试节日/农历用)。
_env_date = os.environ.get("PREVIEW_DATE", "").strip()
if _env_date:
    _y, _m, _d = map(int, _env_date.split("-"))
    PREVIEW_DATE = datetime.date(_y, _m, _d)
else:
    PREVIEW_DATE = datetime.date.today()
# 预览用温度/湿度 (可用环境变量覆盖, 测试不同颜色: PREVIEW_TEMP / PREVIEW_HUMIDITY)
MOCK_TEMP = float(os.environ.get("PREVIEW_TEMP", "25.0"))
MOCK_HUMIDITY = int(os.environ.get("PREVIEW_HUMIDITY", "60"))


def date_strings():
    """由 PREVIEW_DATE 计算日期条两行的文字 (与 nongli.cpp / main.cpp 一致)。"""
    y, m, d = PREVIEW_DATE.year, PREVIEW_DATE.month, PREVIEW_DATE.day
    wday = (PREVIEW_DATE.weekday() + 1) % 7            # 0=Sun..6=Sat
    date = "%d-%02d-%02d" % (y, m, d)
    week = WEEK_FULL[wday]
    ly, lm, ldn, isleap = lunar_solar_to_lunar(y, m, d)
    lyz = ly                                            # 干支/生肖年 = 真实农历年 (按春节分界, 见 nongli nl_zodiac_year)
    lunar = LUNAR_MONTHS[lm - 1] + "月" + LUNAR_DAYS[ldn - 1] + ("(闰)" if isleap else "")
    gz = ganzi_year(lyz)
    fest = nongli_data.festival_of(y, m, d, ly, lm, ldn, isleap)
    cur_name, cur_d, nxt_name, nxt_d = solar_terms(y, m, d)
    mp = moon_phase(y, m, d)
    return dict(date=date, week=week, lunar=lunar, gz=gz, fest=fest,
                cur=cur_name, cur_d=cur_d, nxt=nxt_name, nxt_d=nxt_d,
                mp=mp, moon=moon_name(mp))


def draw_moon(d, cx, cy, R, phase):
    """在 (cx,cy) 画月相图标: 暗底圆盘 + 亮部 (上弦亮右 / 下弦亮左)。"""
    cosP = math.cos(2 * math.pi * phase)
    d.ellipse([cx - R, cy - R, cx + R, cy + R], fill=MOON_DARK)
    for yy in range(-R, R + 1):
        hw = math.sqrt(max(0.0, R * R - yy * yy))
        if phase <= 0.5:                       # 上弦: 亮部在右
            xl, xr = max(cosP * hw, -hw), hw
        else:                                  # 下弦: 亮部在左
            xl, xr = -hw, min(-cosP * hw, hw)
        if xr >= xl:
            x0 = int(round(cx + xl))
            x1 = int(round(cx + xr))
            if x1 >= x0:
                d.line([x0, cy + yy, x1, cy + yy], fill=MOON_LIT)
    d.ellipse([cx - R, cy - R, cx + R, cy + R], outline=MOON_EDGE)


def draw_date(d):
    info = date_strings()
    # 行1 分隔线 x: 不用居中 (240), 而是左移到左半块(日期+星期, 恒止于 ~193)右侧空白处 (200),
    # 给右半块(农历/干支/今日XX)腾出右缘空间, 避免 "今日中秋节" 等长节日名溢出背景右缘 (与 main.cpp 一致)
    cx = 200

    # ---- 行1: 公历+星期 | 农历+干支+节日 ----
    # 与 main.cpp 一致: 整行统一用 font16, 保证公历/星期/农历/干支/节日垂直对齐。
    # 方向 A: 用 hw_text (镜像 XFont::drawStringFastAA), font16 FAIL 时画灰色矩形
    # 镜像硬件"中文空白"; 不再用 F16 (msyhbd.ttf) 兜底画漂亮中文, 保证 preview 与硬件同款降级。
    # AA 合成时必须传所画行底色 (与 main.cpp drawDateRow 调 font16.drawStringFast(..., ROW1_BG/ROW2_BG) 一致)。
    d.rounded_rectangle([DATE_LEFT, ROW1_TOP, DATE_RIGHT, ROW1_TOP + ROW1_H - 1],
                        radius=8, fill=ROW1_BG)
    cy1 = ROW1_TOP + ROW1_H // 2
    x = DATE_LEFT + 12
    hw_text(d, x, cy1, info["date"], WHITE, ROW1_BG, "lm")
    x += aa_text_width(info["date"]) + 8
    hw_text(d, x, cy1, info["week"], CYAN, ROW1_BG, "lm")
    d.line([cx, ROW1_TOP + 9, cx, ROW1_TOP + ROW1_H - 9], fill=DATE_SEP)
    x = cx + 12
    hw_text(d, x, cy1, info["lunar"], WHITE, ROW1_BG, "lm")
    x += aa_text_width(info["lunar"]) + 6
    hw_text(d, x, cy1, info["gz"], GOLD, ROW1_BG, "lm")
    if info["fest"]:
        x += aa_text_width(info["gz"]) + 6
        fest_s = "今日" + info["fest"]
        hw_text(d, x, cy1, fest_s, event_color(info["fest"]), ROW1_BG, "lm")

    # ---- 行2: 节气 | 月相+天气 ----
    d.rounded_rectangle([DATE_LEFT, ROW2_TOP, DATE_RIGHT, ROW2_TOP + ROW2_H - 1],
                        radius=8, fill=ROW2_BG)
    cy2 = ROW2_TOP + ROW2_H // 2
    xj = DATE_LEFT + 12
    if info["cur_d"] == 0:
        # 节气当天: "今日XX" + 节日色, 后接下一节气 (灰蓝)
        cur_s = "今日" + info["cur"]
        hw_text(d, xj, cy2, cur_s, event_color(info["cur"]), ROW2_BG, "lm")
        xj += int(aa_text_width(cur_s)) + 10
        hw_text(d, xj, cy2, "%s %d天后" % (info["nxt"], info["nxt_d"]), SOFT, ROW2_BG, "lm")
    else:
        hw_text(d, xj, cy2,
               "%s 已过%d天   %s %d天后" % (info["cur"], info["cur_d"], info["nxt"], info["nxt_d"]),
               SOFT, ROW2_BG, "lm")
    # 右侧: 湿度 | 温度 | 月相名 | 月相图标 (自右向左)
    x = DATE_RIGHT - 12
    hw_text(d, x, cy2, "%d%%" % MOCK_HUMIDITY, humid_color(MOCK_HUMIDITY), ROW2_BG, "rm")
    x -= aa_text_width("%d%%" % MOCK_HUMIDITY) + 8
    hw_text(d, x, cy2, "%d°C" % int(MOCK_TEMP), temp_color(int(MOCK_TEMP)), ROW2_BG, "rm")
    x -= aa_text_width("%d°C" % int(MOCK_TEMP)) + 8
    hw_text(d, x, cy2, info["moon"], MOON_TXT, ROW2_BG, "rm")
    x -= aa_text_width(info["moon"]) + 6
    draw_moon(d, int(x) - 8, cy2, 9, info["mp"])

# ---- 模拟指针表盘 (白色表盘 + 黑指针/刻度 + 位图数字 + 红秒针, 与 main.cpp drawAnalogClock 完全一致) ----
AC_FACE  = (255, 255, 255)   # 白色表盘 (无外圈描边)
AC_TICK1 = (0, 0, 0)      # 时标 (黑)
AC_TICK2 = (0, 0, 0)      # 分标 (黑)
AC_H     = (0, 0, 0)      # 时针 (黑)
AC_M     = (0, 0, 0)      # 分针 (黑)
AC_S     = c565(0xF800)   # 秒针 (红)
AC_HUB   = c565(0xF800)   # 中心枢纽 (红)
AC_NUM   = (0, 0, 0)      # 数字 (黑)

def _draw_hand(d, cx, cy, ang, ln, w, tail, col):
    """棒棒(baton)指针: 中段最宽、向两端渐细的细长多边形 + 圆头帽, 边缘平滑无锯齿。
    与 main.cpp drawAnalogClock 的 hand() 完全一致 (图二风格)。"""
    a = 2 * math.pi * ang - math.pi / 2
    dx, dy = math.cos(a), math.sin(a)
    ux, uy = -math.sin(a), math.cos(a)
    hb = w / 2.0                       # 中段半宽
    ht = max(1.0, w * 0.30)            # 两端半宽
    capD = ln - ht                     # 圆帽起点(距中心)
    A = (cx + dx * (-tail) + ux * ht, cy + dy * (-tail) + uy * ht)  # 尾部上
    C = (cx + ux * hb, cy + uy * hb)                            # 中段左(最宽)
    E = (cx + dx * capD + ux * ht, cy + dy * capD + uy * ht)    # 尖端上
    F = (cx + dx * capD - ux * ht, cy + dy * capD - uy * ht)    # 尖端下
    D = (cx - ux * hb, cy - uy * hb)                            # 中段右
    B = (cx + dx * (-tail) - ux * ht, cy + dy * (-tail) - uy * ht)  # 尾部下
    d.polygon([A, C, E, F, D, B], fill=col)
    d.ellipse([cx + dx * capD - ht, cy + dy * capD - ht,
               cx + dx * capD + ht, cy + dy * capD + ht], fill=col)


def draw_analog_clock(img, d, h, m, s, ms=0):
    cx, cy, R = 240, 108, 73   # 与 main.cpp 一致: 指针表盘放大 (顶 ~32 / 底 ~184)
    t2h = (h + m / 60.0 + s / 3600.0) / 12.0
    t2m = (m + s / 60.0 + ms / 60000.0) / 60.0
    t2s = (s + ms / 1000.0) / 60.0
    # 表盘 (白色, 无外圈描边)
    d.ellipse([cx - R, cy - R, cx + R, cy + R], fill=AC_FACE)
    # 刻度
    for i in range(60):
        a = 2 * math.pi * i / 60.0 - math.pi / 2
        major = (i % 5 == 0)
        ro, ri = R - 2, R - (10 if major else 5)
        d.line([cx + math.cos(a) * ri, cy + math.sin(a) * ri,
                cx + math.cos(a) * ro, cy + math.sin(a) * ro],
               fill=AC_TICK1 if major else AC_TICK2, width=2 if major else 1)
    # 数字 12..11 (位图遮罩, 与翻页时钟同款字体; 中心落在半径 R-20 处, 与 main.cpp 一致)
    for k in range(1, 13):
        an = 2 * math.pi * k / 12.0 - math.pi / 2
        idx = 0 if k == 12 else k   # 12->0, 10->10, 11->11, 1..9 -> 1..9 (DialNum[10]="10",[11]="11")
        nx = cx + math.cos(an) * (R - 20)
        ny = cy + math.sin(an) * (R - 20)
        num_img = get_dialnum(idx, AC_FACE)
        img.paste(num_img, (int(nx - num_img.width / 2), int(ny - num_img.height / 2)))
    _draw_hand(d, cx, cy, t2h, 40, 4, 11, AC_H)
    _draw_hand(d, cx, cy, t2m, 55, 3, 14, AC_M)
    sa = 2 * math.pi * t2s - math.pi / 2
    d.line([cx, cy, cx + math.cos(sa) * 60, cy + math.sin(sa) * 60], fill=AC_S, width=2)
    d.ellipse([cx - math.cos(sa) * 15 - 3, cy - math.sin(sa) * 15 - 3,
               cx - math.cos(sa) * 15 + 3, cy - math.sin(sa) * 15 + 3], fill=AC_S)
    d.ellipse([cx - 5, cy - 5, cx + 5, cy + 5], fill=AC_HUB)
    d.ellipse([cx - 2, cy - 2, cx + 2, cy + 2], fill=AC_FACE)


# 调试提示开关: 调试期显示"点屏幕切换"; 正式产品设 0 隐藏 (与 main.cpp DEBUG_UI 一致)。
DEBUG_UI = 0

def draw_mode_hint(d, style):
    """顶右侧样式提示 (与 main.cpp drawModeHint 一致): 黑底 + 灰字。"""
    if not DEBUG_UI:
        return
    s = "%s | 点屏幕切换" % ("翻页" if style == 0 else "指针")
    w = int(F16.getlength(s))
    d.rectangle([478 - w - 8, 6, 478 - 8, 27], fill=(0, 0, 0))
    d.text((470 - w, 12), s, font=F16, fill=c565(0x7BEF))


def draw_analog_scene(h, m, s, ms=0):
    img = Image.new("RGB", (480, 320), (0, 0, 0))
    d = ImageDraw.Draw(img)
    draw_analog_clock(img, d, h, m, s, ms)
    draw_mode_hint(d, 1)
    # 指针样式不显示 "NTP OK" (与 main.cpp 一致), 表盘放大到顶部
    draw_date(d)
    draw_nav(d)
    return img


# ---------- 万年历页面 (480x320, 与固件 modeCalendar 保持一致) ----------
CAL_X0, CELL_W, CELL_H = 16, 64, 30
CAL_X1 = CAL_X0 + 7 * CELL_W            # 464
CAL_GRID_TOP = 54
CAL_RED      = c565(0xF800)   # 周末 - 红
WEEK_HDR     = ["日", "一", "二", "三", "四", "五", "六"]   # 0=周日 (与 WEEK_FULL 一致)


# ---------- 黄历 (新设计: 米纸 + 红/褐) 弹层: 双击某天打开 (镜像 main.cpp drawAlmPopup) ----------
ALM_PAPER   = c565(0xEF9C)   # 纸面 (暖白米纸)
ALM_RED     = c565(0xB9C5)   # 主红 (大数字 / 月份 / 忌 / 节日框)
ALM_DARK    = c565(0x3985)   # 深褐 (标题 / 干支 / 星期)
ALM_LINE    = c565(0xCCB0)   # 细线 (外框 / 分栏 / 星期框 / 图例)
ALM_JI      = c565(0x6D0C)   # 吉 (绿)
ALM_RING    = c565(0xE5F5)   # 大数字装饰环 (淡粉)
ALM_WKND    = c565(0x8A52)   # 周末图例点 (褐红, 区别于 平日深褐 / 节假日主红)
ALM_PAPER_565  = 0xEF9C   # (大数字合成底色 565 原值, = ALM_PAPER, 与面板无缝)
ALM_RED_565    = 0xB9C5   # (大数字笔画 565 原值, 与 main.cpp ALM_RED 一致)
ALM_WKND_565   = 0x8A52   # (周末 大数字笔画 565 原值, = ALM_WKND)
ALM_JI_565     = 0x6D0C   # (平日 大数字笔画 565 原值, = ALM_JI, 与图例一致)
ALM_DARK_565   = 0x3985   # (深褐 565 原值, = ALM_DARK)
ALM_PMON = ["正月", "杏月", "桃月", "槐月", "榴月", "荷月",
            "兰月", "桂月", "菊月", "阳月", "冬月", "腊月"]   # 花名月 (与 main.cpp 一致)
ZODIAC_DAY = ["鼠", "牛", "虎", "兔", "龙", "蛇", "马", "羊", "猴", "鸡", "狗", "猪"]  # index=dayStd%12, 0=子


def nl_jq_after(y, m, dd):
    """当前节气名 (黄历顶行 "X月 农历..  ·  秋分后" 用; 与 solar_terms 一致)."""
    curN, _, _, _ = solar_terms(y, m, dd)
    return curN


def _alm_fest_name(y, m, dd, lyz, lm, ldn, isleap):
    # Mirror main.cpp almHolidayFest: whole contiguous holiday run shares one festival name.
    import datetime
    # 除夕: (a) 当天节假日名 == "除夕" (holidays JSON, 政府数据) 或 (b) 次日是农历正月初一
    #   (镜像 nongli.cpp nl_is_chuxi, 不依赖 JSON —— 2027 JSON 为空时除夕仍显示)。
    #   必须在 festival_of 之前: 农历表对 2026 差一天, festival_of 会先误返"春节"。
    e0 = load_holidays(y).get("%02d-%02d" % (m, dd))
    nxt = datetime.date(y, m, dd) + datetime.timedelta(days=1)
    nly, nlm, nldn, nleap = lunar_solar_to_lunar(nxt.year, nxt.month, nxt.day)
    is_chuxi = (e0 and e0.get("name") == "除夕") or (nlm == 1 and nldn == 1 and not nleap)
    if is_chuxi:
        return "除夕"
    nf = nongli_data.festival_of(y, m, dd, lyz, lm, ldn, isleap)
    if nf:
        return nf
    if hol_day_type(y, m, dd) != 1:
        return ""
    cur = datetime.date(y, m, dd)
    a = cur
    while hol_day_type(a.year, a.month, a.day) == 1 and (cur - a).days < 40:
        a = a - datetime.timedelta(days=1)
    a = a + datetime.timedelta(days=1)
    b = cur
    while hol_day_type(b.year, b.month, b.day) == 1 and (b - cur).days < 40:
        b = b + datetime.timedelta(days=1)
    b = b - datetime.timedelta(days=1)
    day = a
    while day <= b:
        rly, rlm, rldn, rileap = lunar_solar_to_lunar(day.year, day.month, day.day)
        nf2 = nongli_data.festival_of(day.year, day.month, day.day, rly, rlm, rldn, rileap)
        if nf2:
            return nf2
        day = day + datetime.timedelta(days=1)
    e = load_holidays(y).get("%02d-%02d" % (m, dd))
    if e and e.get("name"):
        return e["name"]
    return ""
ALM_TG = ["甲","乙","丙","丁","戊","己","庚","辛","壬","癸"]   # 干支 0=甲 (与 main.cpp TG2 一致)
ALM_DZ = ["子","丑","寅","卯","辰","巳","午","未","申","酉","戌","亥"]   # 地支 0=子
ALM_GZ2 = ALM_DZ
ALM_HS = [False, True, False, False, False, True, True, False, False, False, True, False]   # 丑午未吉
ALM_HB = [True, False, False, True, False, False, False, True, False, True, False, True]    # 子卯酉凶
ALM_YI_ITEMS = ["嫁娶","出行","移徙","入宅","装修","动土","栽种","祭祀","安床","会友","嫁娶","出行"]   # 宜 (与 main.cpp 一致)
ALM_JI_ITEMS = ["开张","开业","嫁娶","动土","安葬","出行","安床","入宅","词讼","破土","开光","会友"]   # 忌 (与 main.cpp 一致)

ALM_PX, ALM_PW, ALM_PT, ALM_PH = 24, 432, 16, 272   # 弹层几何 (与 main.cpp 一致)


def alm_jdn(y, m, d):
    """儒略日 (与 main.cpp almJDN 同式)."""
    a = (14 - m) // 12
    yy = y + 4800 - a
    mm = m + 12 * a - 3
    return d + (153 * mm + 2) // 5 + 365 * yy + yy // 4 - yy // 100 + yy // 400 - 32045


def alm_big_number(day, fg565, bg565):
    """两位 62x112 遮罩并排 = 124x112, bg 底 + fg 笔画. 与 main.cpp almBigNum 逐像素一致
    (565 原值逐通道混合 -> c565 转 RGB; day<10 时十位留 bg 底)."""
    fr5, fg6, fb5 = fg565 >> 11, (fg565 >> 5) & 0x3F, fg565 & 0x1F
    pr5, pg6, pb5 = bg565 >> 11, (bg565 >> 5) & 0x3F, bg565 & 0x1F
    out = Image.new("RGB", (ALM_DW * 2, ALM_DH))
    po = out.load()
    for dig, dx0 in ((day // 10, 0), (day % 10, ALM_DW)):
        mask = ALM_MASKS[dig]
        blank = False   # 前导零 (day<10 时十位显 "0", 与参考图 "03" 一致)
        for dy in range(ALM_DH):
            for dx in range(ALM_DW):
                a = 255 if blank else mask[dy * ALM_DW + dx]    # 0=笔画(fg) 255=底(bg)
                r5 = (pr5 * a + fr5 * (255 - a)) // 255
                g6 = ((pg6 * a + fg6 * (255 - a)) // 255) & 0x3F
                b5 = (pb5 * a + fb5 * (255 - a)) // 255 & 0x1F
                po[dx0 + dx, dy] = c565((r5 << 11) | (g6 << 5) | b5)
    return out


def alm_pil(y, m, dd):
    """黄历数据 (与 main.cpp drawAlmPopup 的计算逐项一致).
    返回 (lyz, lm, ldn, isleap, dayStd, mgStd, mzStd, moonPhase).
    干支日=(JDN+49)%60; 月支按上节(24节气奇数, 奇数化&~1)起寅; 月干=五虎遁(年干)."""
    ly, lm, ldn, isleap = lunar_solar_to_lunar(y, m, dd)
    lyz = ly
    dayStd = (alm_jdn(y, m, dd) + 49) % 60
    curN, _, _, _ = solar_terms(y, m, dd)
    curI = nongli_data.JQ_NAMES.index(curN)         # 当前节气编号 0..23 (小寒=0)
    lastJie = curI & ~1
    mzStd = (lastJie // 2 + 1) % 12
    ygStd = (lyz % 10 + 6) % 10
    yinGan = (2 + (ygStd % 5) * 2) % 10
    mgStd = (yinGan + ((mzStd - 2) % 12 + 12) % 12) % 10
    mpv = moon_phase(y, m, dd)
    return lyz, lm, ldn, isleap, dayStd, mgStd, mzStd, mpv


def draw_almanac_popup(img, d, y, m, dd):
    """黄历 (米纸+红/褐 新设计) 弹层 (逐像素镜像 main.cpp drawAlmPopup).
    顶行(年份/干支 + 红色月份 + 农历/节气 + 节日框) | 三列中部(农历/大数字+环/干支) |
    底栏(星期框 + 宜[吉]X项 + 忌[凶]X项 + 图例). 点任意处关闭 (镜像 main.cpp)."""
    lyz, lm, ldn, isleap, dayStd, mgStd, mzStd, mpv = alm_pil(y, m, dd)
    PX, PW, PT, PH = ALM_PX, ALM_PW, ALM_PT, ALM_PH   # 24,432,16,272
    cx = 240
    ygz = "%s%s%s" % (GAN[lyz % 10], ZHI[lyz % 12], ZODIAC[lyz % 12])      # 甲辰年
    dgz = ALM_TG[dayStd % 10] + ALM_DZ[dayStd % 12]                          # 干支日 壬寅
    pmon = ALM_PMON[(lm - 1) % 12] if not isleap else "闰" + ALM_PMON[(lm - 1) % 12]
    lcol_main = "%s月%s" % (LUNAR_MONTHS[lm - 1], LUNAR_DAYS[ldn - 1])          # 八月初四
    wday = WEEK_FULL[(datetime.date(y, m, dd).weekday() + 1) % 7][-1]          # 末字(如"六"), 与 weekHdr[tm_wday] 一致
    # 节气: 当日 -> "今日X"; 之后 -> "X后" (镜像参考图). 花名月·节气 顶行右侧小字.
    curN, curDays, nxtN, nxtDays = solar_terms(y, m, dd)
    pjText = ("今日" + curN) if curDays == 0 else ("%s·%s后" % (pmon, curN))
    yi3, seen = [], set()
    for k in range(12):
        it = ALM_YI_ITEMS[(dayStd + k) % 12]
        if it not in seen:
            seen.add(it); yi3.append(it)
        if len(yi3) == 3:
            break
    ji3, seen = [], set()
    for k in range(12):
        it = ALM_JI_ITEMS[(dayStd + k) % 12]
        if it not in seen:
            seen.add(it); ji3.append(it)
        if len(ji3) == 3:
            break
    fest = _alm_fest_name(y, m, dd, lyz, lm, ldn, isleap)
    # 三列 grid: 左 24..150 / 中 150..330 / 右 330..456; 顶行分隔线 y68, 底栏分隔线 y230
    LC, RC = 90, 390
    TOPDIV, BOTDIV, MIDL, MIDR = 68, 230, 150, 330
    # --- 背景 + 四角括号 ---
    d.rectangle([PX, PT, PX + PW - 1, PT + PH - 1], fill=ALM_PAPER)
    L = 16
    for (ax, ay, sxx, syy) in ((PX, PT, 1, 1), (PX + PW - 1, PT, -1, 1),
                               (PX, PT + PH - 1, 1, -1), (PX + PW - 1, PT + PH - 1, -1, -1)):
        d.line([ax, ay, ax + sxx * L, ay], fill=ALM_LINE)
        d.line([ax, ay, ax, ay + syy * L], fill=ALM_LINE)
    # --- 网格线 (镜像参考图 table 结构: 顶行线 + 三列竖线 + 底栏线) ---
    d.line([PX, TOPDIV, PX + PW - 1, TOPDIV], fill=ALM_LINE)
    d.line([MIDL, TOPDIV, MIDL, BOTDIV], fill=ALM_LINE)
    d.line([MIDR, TOPDIV, MIDR, BOTDIV], fill=ALM_LINE)
    # --- 顶行 (y30..58): 年份 干支X年 | 月份 月 | 农历X月X | 花名月·节气后 | 右上 节日框 ---
    big_text(d, PX + 14, 30, "%d" % y, 24, ALM_DARK, ALM_PAPER)          # 2026
    ygzX = PX + 14 + 4 * (24 * 7 // 10) + 8
    hw_small(d, ygzX, 36, ygz, 13, ALM_DARK, ALM_PAPER, "l")             # 丙午马 (13px)
    monX = 162
    big_text(d, monX, 30, "%d" % m, 24, ALM_RED, ALM_PAPER)              # 10 (大红)
    hw_small(d, monX + 2 * (24 * 7 // 10) + 4, 36, "月", 13, ALM_RED, ALM_PAPER, "l")  # 月
    lunX = 218
    lunStr = "农历%s月%s" % (LUNAR_MONTHS[lm - 1], LUNAR_DAYS[ldn - 1])
    hw_small(d, lunX, 36, lunStr, 13, ALM_DARK, ALM_PAPER, "l")          # 农历X月X
    lunW = aa_text_width(lunStr) * 13 // 16
    pjX = lunX + lunW + 6
    pjW = aa_text_width(pjText) * 13 // 16
    fw = (aa_text_width(fest) + 14) if fest else 0
    boxLeft = (PX + PW - 12 - fw) if fest else (PX + PW - 14)
    if pjX + pjW > boxLeft - 4:
        pjX = max(lunX + lunW + 2, boxLeft - 4 - pjW)                    # 不越过节日框
    # BUILD21: 花名月·节气 节日当天也显示(与右上节日框并排, 靠左由上方 clamp 保证不重叠)。
    hw_small(d, pjX, 36, pjText, 13, ALM_DARK, ALM_PAPER, "l")           # 花名月·节气后 / 今日X
    if fest:
        d.rectangle([PX + PW - 12 - fw, 28, PX + PW - 12, 54], outline=ALM_RED, width=1)
        hw_small(d, PX + PW - 12 - fw + 7, 34, fest, 15, ALM_RED, ALM_PAPER, "l")
    # --- 三列中部 (左列 农历 红 / 右列 干支 深褐; 中央环 + 大数字 红; 环中心 y=148) ---
    _ht0 = hol_day_type(y, m, dd)
    _wd0 = datetime.date(y, m, dd).weekday()   # 0=Mon .. 6=Sun (周六/日=周末)
    if _ht0 in (1, 2):
        num565 = ALM_RED_565       # 节假日(放假/补班) -> 红
    elif fest:
        num565 = ALM_RED_565       # BUILD23: 任何节日(含西方 万圣节/黑五/母亲节...) -> 红 (镜像 main.cpp fest[0])
    elif _wd0 in (5, 6):
        num565 = ALM_WKND_565      # 周末 -> 紫
    else:
        num565 = ALM_JI_565        # 平日 -> 绿
    numcol = c565(num565)
    big_text(d, LC, 122, lcol_main, 24, numcol, ALM_PAPER, "c")   # 农历 (大字 numCol)
    hw_small(d, LC, 166, "农历·%s" % pmon, 14, ALM_DARK, ALM_PAPER, "c")  # 农历·桂月
    big_text(d, RC, 122, dgz + "日", 24, ALM_DARK, ALM_PAPER, "c")   # 壬寅日 (深褐)
    hw_small(d, RC, 166, "干支·%s" % ZODIAC_DAY[dayStd % 12], 14, ALM_DARK, ALM_PAPER, "c")  # 干支·虎
    d.ellipse([cx - 72, 76, cx + 72, 220], outline=ALM_RING, width=2)
    d.ellipse([cx - 57, 91, cx + 57, 205], outline=ALM_RING, width=1)
    img.paste(alm_big_number(dd, num565, ALM_PAPER_565), (cx - ALM_DW, 148 - ALM_DH // 2))
    # --- 底栏 (y230..288) ---
    d.line([PX + 10, BOTDIV, PX + PW - 11, BOTDIV], fill=ALM_LINE)
    # 星期 (左): "星期" 字 (18px) + 右侧大 周几字 (镜像参考图, 不重叠)
    hw_small(d, PX + 12, 240, "星期", 18, ALM_DARK, ALM_PAPER, "l")
    big_text(d, PX + 54, 234, wday, 30, ALM_DARK, ALM_PAPER)
    # 宜 (吉绿) + 忌 (凶红) 两行
    tx = 150
    tag = lambda x, yy, bg: d.rectangle([x, yy, x + 16, yy + 16], fill=bg)
    tag(tx, 236, ALM_JI)
    hw_text(d, tx, 236, "宜", ALM_PAPER, ALM_JI, "lm")
    xi = tx + 24
    for i in range(3):
        hw_text(d, xi, 236, yi3[i], ALM_DARK, ALM_PAPER, "lm")
        xi += aa_text_width(yi3[i]) + 6
    tag(tx, 258, ALM_RED)
    hw_text(d, tx, 258, "忌", ALM_PAPER, ALM_RED, "lm")
    xi = tx + 24
    for i in range(3):
        hw_text(d, xi, 258, ji3[i], ALM_DARK, ALM_PAPER, "lm")
        xi += aa_text_width(ji3[i]) + 6
    # 图例 (右): 平日/周末/节假日
    lx = PX + PW - 14 - 130
    for r, (nm, colr) in enumerate((("平日", ALM_JI), ("周末", ALM_WKND), ("节假日", ALM_RED))):
        yy = 234 + r * 18
        d.ellipse([lx, yy + 5, lx + 7, yy + 12], fill=colr)
        hw_text(d, lx + 12, yy + 8, nm, colr, ALM_PAPER, "lm")
    # 月相 (图例右侧空白区): 图标 + 名称
    draw_moon(d, 415, 250, 9, mpv)
    hw_text(d, 415, 272, moon_name(mpv), c565(0xDE72), ALM_PAPER, "mm")



def draw_calendar_scene(year, month, today=None, selected=None, picker=False, pick_ym=None, almanac=False):
    """万年历页面: 月份网格(周日列起) + 顶部月份标题/上下月 + 底部农历信息条。
    today: 高亮的"今天" (默认 PREVIEW_DATE)。
    selected: 选中的日期 (year,month,day); 底部信息条显示其农历, 非今天时画蓝框。
    picker: 是否显示"选择年月"弹层; pick_ym=(year,month) 为弹层当前值。
    almanac: 是否显示"黄历"弹层 (双击某天打开, 点右下角"关闭"返回; 镜像 main.cpp calAlmOpen)。
    导航栏高亮"日历"。"""
    import calendar as _calmod
    img = Image.new("RGB", (480, 320), (0, 0, 0))
    d = ImageDraw.Draw(img)
    if today is None:
        today = PREVIEW_DATE
    # ---- 浅色背景 (仅日历页) ----
    d.rectangle([0, 0, 479, NAV_Y - 1], fill=CAL_LIGHT_BG)
    if picker:
        _draw_cal_picker(d, pick_ym or (year, month))
        draw_nav(d, sel=1)
        return img
    if almanac:
        sd = selected if selected is not None else (today.year, today.month, today.day)
        draw_almanac_popup(img, d, sd[0], sd[1], sd[2])   # 黄历弹层 (全屏覆盖, 与 main.cpp calAlmOpen 分支一致)
        draw_nav(d, sel=1)
        return img
    # ---- 顶部: 上月 | 月份标题(点击选年月) | 下月 ----
    d.text((24, 24), "<", font=F16, fill=CAL_HDR, anchor="mm")
    d.text((240, 24), "%d年%d月" % (year, month), font=F16, fill=CAL_TITLE, anchor="mm")
    d.text((456, 24), ">", font=F16, fill=CAL_HDR, anchor="mm")
    # ---- 星期表头 ----
    for i, w in enumerate(WEEK_HDR):
        d.text((CAL_X0 + i * CELL_W + CELL_W // 2, 42), w, font=F16, fill=CAL_HDR, anchor="mm")
    # ---- table 网格线 (7 列 × 6 行) ----
    for i in range(8):
        gx = CAL_X0 + i * CELL_W
        d.line([gx, CAL_GRID_TOP, gx, CAL_GRID_TOP + 6 * CELL_H], fill=CAL_GRID)
    for r in range(7):
        gy = CAL_GRID_TOP + r * CELL_H
        d.line([CAL_X0, gy, CAL_X1, gy], fill=CAL_GRID)
    # ---- 日期网格 ----
    first = datetime.date(year, month, 1)
    start_w = (first.weekday() + 1) % 7           # 0=周日
    dim = _calmod.monthrange(year, month)[1]
    for day in range(1, dim + 1):
        r, c = divmod(start_w + day - 1, 7)
        if r >= 6:
            break
        cx = CAL_X0 + c * CELL_W + CELL_W // 2
        cy = CAL_GRID_TOP + r * CELL_H + CELL_H // 2
        is_today = (today.year, today.month, today.day) == (year, month, day)
        if is_today:                              # 今天: 青色圆角高亮
            d.rounded_rectangle([cx - 13, cy - 12, cx + 13, cy + 12], radius=6, fill=CYAN)
            d.text((cx, cy), str(day), font=F16, fill=(0, 0, 0), anchor="mm")
            _ht = hol_day_type(year, month, day)   # 2026-09-28: today-that-is-a-holiday still shows badge
            if _ht in (1, 2):
                aa_compose_scaled_glyph(img, cx + 19, cy - 14, 0x4F11 if _ht == 1 else 0x73ED,
                                        12, HOL_OFF if _ht == 1 else HOL_WORK_L, CYAN)
        else:
            ht = hol_day_type(year, month, day)   # 0=普通 1=放假 2=补班
            if ht == 1:
                fc = HOL_OFF
            elif ht == 2:
                fc = HOL_WORK_L
            else:
                fc = CAL_RED if (c == 0 or c == 6) else CAL_NORM
            d.text((cx, cy), str(day), font=F16, fill=fc, anchor="mm")
            if ht in (1, 2):                      # 右上角小 "休"/"班" 角标 (缩放 12px, 替代下划线)
                aa_compose_scaled_glyph(img, cx + 19, cy - 14,
                                        0x4F11 if ht == 1 else 0x73ED, 12, fc, CAL_LIGHT_BG)
        # 选中日 (非今天): 蓝色边框
        if selected is not None and not is_today \
                and selected == (year, month, day):
            d.rectangle([cx - 14, cy - 13, cx + 15, cy + 14], outline=CAL_TITLE, width=1)
    # ---- 底部信息条: 干支年 + 选中日的农历 (点击某天即显示; 默认=今天) ----
    sel = selected if selected is not None else (today.year, today.month, today.day)
    sly, slm, slm_d, sisleap = lunar_solar_to_lunar(sel[0], sel[1], sel[2])
    slyz = sly                                          # 干支/生肖年 = 真实农历年 (按春节分界, 见 nongli nl_zodiac_year)
    strip_top = CAL_GRID_TOP + 6 * CELL_H + 8
    d.rounded_rectangle([CAL_X0, strip_top, CAL_X1, strip_top + 34], radius=8, fill=CAL_STRIP_L)
    cys = strip_top + 17
    x = CAL_X0 + 12
    gz = ganzi_year(slyz)
    d.text((x, cys), gz, font=F16, fill=CAL_GOLD_L, anchor="lm")
    x += F16.getlength(gz) + 12
    # 选中日 = 今天 -> "今日农历X月X"; 否则 "M月D日 农历X月X"
    if (sel[0], sel[1], sel[2]) == (today.year, today.month, today.day):
        lm_s = "今日农历" + LUNAR_MONTHS[slm - 1] + "月" + LUNAR_DAYS[slm_d - 1] + ("(闰)" if sisleap else "")
    else:
        lm_s = "%d月%d日 农历" % (sel[1], sel[2]) + LUNAR_MONTHS[slm - 1] + "月" + LUNAR_DAYS[slm_d - 1] + ("(闰)" if sisleap else "")
    d.text((x, cys), lm_s, font=F16, fill=CAL_NORM, anchor="lm")
    # "今日" 按钮 (信息条右端): 一键回到今天 (当前月 + 选中今天)。与 main.cpp 同款几何。
    bt_txt = "今日"
    bt_w = F16.getlength(bt_txt) + 16
    bt_x = CAL_X1 - 8 - bt_w
    bt_y = strip_top + 5
    at_today = (year == today.year and month == today.month)
    d.rounded_rectangle([bt_x, bt_y, bt_x + bt_w, bt_y + 24], radius=6,
                        fill=CYAN if at_today else CAL_STRIP_L)
    d.text((bt_x + 8, cys), bt_txt, font=F16,
           fill=BLACK if at_today else CAL_TITLE, anchor="lm")
    draw_nav(d, sel=1)
    return img


def _draw_cal_picker(d, pick_ym):
    """年月选择弹层 (镜像 main.cpp drawCalPicker): < 年份 > + 6x2 月份网格 + 取消/确定。"""
    py, pm = pick_ym
    d.rectangle([PICK_PANEL_X, PICK_PT, PICK_PANEL_X + PICK_PANEL_W - 1, PICK_PB - 1],
                fill=CAL_LIGHT_BG, outline=CAL_GRID, width=1)
    d.text((240, PICK_PT + 12 + 8), "选择年月", font=F16, fill=CAL_HDR, anchor="mm")
    # 年份步进: < 2026 >
    d.text((40, 88 + 8), "<", font=F16, fill=CAL_HDR, anchor="mm")
    ys = str(py)
    d.text((240, 88 + 8), ys, font=F16, fill=CAL_TITLE, anchor="mm")
    d.text((420, 88 + 8), ">", font=F16, fill=CAL_HDR, anchor="mm")
    # 月份网格 6 列 x 2 行
    for m in range(1, 13):
        col = (m - 1) % 6
        row = (m - 1) // 6
        bx = PICK_MGRID_X + col * PICK_MCOL_W
        by = PICK_MTOP + row * PICK_MROW_H
        mlab = "%d月" % m
        if m == pm:
            d.rounded_rectangle([bx + 3, by + 3, bx + PICK_MCOL_W - 3, by + PICK_MROW_H - 3],
                                radius=8, fill=CYAN)
            d.text((bx + PICK_MCOL_W // 2, by + PICK_MROW_H // 2), mlab, font=F16, fill=CAL_TITLE, anchor="mm")
        else:
            d.rounded_rectangle([bx + 3, by + 3, bx + PICK_MCOL_W - 3, by + PICK_MROW_H - 3],
                                radius=8, fill=CAL_STRIP_L)
            d.text((bx + PICK_MCOL_W // 2, by + PICK_MROW_H // 2), mlab, font=F16, fill=CAL_NORM, anchor="mm")
    # 按钮: 取消 | 确定  (文字垂直居中于 确定 按钮中心 y=PICK_PB-21, 与硬件 top y=PICK_PB-29 对齐)
    d.text((150, PICK_PB - 21), "取消", font=F16, fill=CAL_HDR, anchor="mm")
    d.rounded_rectangle([280, PICK_PB - 34, 280 + 120, PICK_PB - 34 + 26], radius=8, fill=CAL_TITLE)
    d.text((340, PICK_PB - 21), "确定", font=F16, fill=CAL_LIGHT_BG, anchor="mm")



# ============================================================================
# 天气页 (逐像素镜像 main.cpp drawWeatherScreen)
# ============================================================================
import wicons
WEATHER_MOCK = {
    "city": "广州", "date": "10-04 周日", "cond": "多云", "icon": 1,
    "temp": 25, "feels": 27, "hi": 28, "lo": 22,
    "humidity": 62, "windSpeed": 18, "windDir": "东南风", "windScale": 3,
    "pressure": 1013, "uv": 5,
    "sunrise": "06:12", "sunset": "18:34", "update": "15:30",
    "day": ["今天", "明天", "周一", "周二", "周三", "周四", "周五"],
    "dayIcon": [0, 1, 2, 4, 6, 4, 1],
    "dayHi": [28, 27, 25, 23, 22, 24, 26],
    "dayLo": [22, 21, 20, 19, 18, 20, 22],
}

def w_icon(d, type_, cx, cy, s, bg):
    """镜像 main.cpp wIcon: 24x24 位图 (wicons.W_ICONS) 最近邻缩放到 s,
    idx0=透明 -> bg, 其余按 W_ICON_PAL565 上色 (中心 cx,cy)。"""
    type_ = max(0, min(7, type_))
    g = wicons.W_ICONS[type_]
    pal = {0: bg}
    for k, v in wicons.W_ICON_PAL565.items():
        if k != 0:
            pal[k] = c565(v)
    ox, oy = cx - s // 2, cy - s // 2
    for dy in range(s):
        sy = (dy * 24) // s
        for dx in range(s):
            sx = (dx * 24) // s
            c = pal[g[sy][sx]]
            if c:
                d.point((ox + dx, oy + dy), fill=c)

def _wmo(code):
    """镜像 weather_sync.cpp codeToIconName: open-meteo WMO 码 -> (图标 0..7, 中文)。"""
    m = {0: (0, "晴"), 1: (1, "晴间多云"), 2: (1, "多云"), 3: (2, "阴"),
         45: (3, "雾"), 48: (3, "雾"),
         51: (5, "毛毛雨"), 53: (5, "毛毛雨"), 55: (5, "毛毛雨"),
         56: (5, "毛毛雨"), 57: (5, "毛毛雨"),
         61: (4, "雨"), 63: (4, "雨"), 65: (4, "雨"), 66: (4, "雨"), 67: (4, "雨"),
         71: (6, "雪"), 73: (6, "雪"), 75: (6, "雪"), 77: (6, "雪"),
         80: (4, "阵雨"), 81: (4, "阵雨"), 82: (4, "阵雨"),
         85: (6, "阵雪"), 86: (6, "阵雪"),
         95: (7, "雷阵雨"), 96: (7, "雷雨伴冰雹"), 99: (7, "雷雨伴冰雹")}
    return m.get(code, (2, "多云"))

def _wind_dir(deg):
    return ["北风", "东北风", "东风", "东南风", "南风", "西南风", "西风", "西北风"][int((deg + 22.5) / 45) % 8]

def _wind_scale(kmh):
    th = [1, 6, 12, 20, 29, 39, 50, 62, 75, 89, 103, 118]
    for i in range(12):
        if kmh < th[i]:
            return i
    return 12

def fetch_weather_real(city="广州", lat=23.13, lon=113.26):
    """镜像 weather_sync.cpp fetchAndCache: GET open-meteo (纯 HTTP), 返回 WEATHER_MOCK 同款 dict。
    失败返回 None (调用方退回 mock)。"""
    url = ("http://api.open-meteo.com/v1/forecast?latitude=%.2f&longitude=%.2f"
           "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,"
           "wind_speed_10m,wind_direction_10m,pressure_msl,uv_index"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset"
           "&forecast_days=7&timezone=Asia%%2FShanghai") % (lat, lon)
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "preview/1.0"})
        doc = json.load(urllib.request.urlopen(req, timeout=8))
    except Exception as e:
        print("[Wx] fetch failed: %r" % e)
        return None
    cur = doc["current"]; daily = doc["daily"]
    icon, cond = _wmo(int(cur["weather_code"]))
    now = datetime.datetime.now()
    wk = ["周日", "周一", "周二", "周三", "周四", "周五", "周六"]
    day, dayIcon, dayHi, dayLo = [], [], [], []
    for i in range(7):
        if i == 0: day.append("今天")
        elif i == 1: day.append("明天")
        else: day.append(wk[(now.weekday() + 1 + i) % 7])
        ic, _ = _wmo(int(daily["weather_code"][i]))
        dayIcon.append(ic)
        dayHi.append(int(daily["temperature_2m_max"][i]))
        dayLo.append(int(daily["temperature_2m_min"][i]))
    return {
        "city": city,
        "date": "%02d-%02d %s" % (now.month, now.day, wk[(now.weekday() + 1) % 7]),   # 镜像 C++ WK[tm_wday] (0=周日)
        "cond": cond, "icon": icon,
        "temp": round(cur["temperature_2m"]), "feels": round(cur["apparent_temperature"]),
        "hi": int(daily["temperature_2m_max"][0]), "lo": int(daily["temperature_2m_min"][0]),
        "humidity": int(cur["relative_humidity_2m"]),
        "windSpeed": int(cur["wind_speed_10m"]),
        "windDir": _wind_dir(float(cur["wind_direction_10m"])),
        "windScale": _wind_scale(float(cur["wind_speed_10m"])),
        "pressure": int(cur["pressure_msl"]), "uv": round(float(cur["uv_index"])),
        "sunrise": daily["sunrise"][0][11:],
        "sunset": daily["sunset"][0][11:],
        "update": "%02d:%02d" % (now.hour, now.minute),
        "day": day, "dayIcon": dayIcon, "dayHi": dayHi, "dayLo": dayLo,
    }

def draw_weather_scene(w=None):
    """镜像 main.cpp drawWeatherScreen: 头部 + 主区(大温度/大图标/状况) + 4 卡片 + 7 天预报。"""
    w = w or WEATHER_MOCK
    img = Image.new("RGB", (480, 320), W_BG)
    d = ImageDraw.Draw(img)
    # 清场 0..NAV_Y-1
    d.rectangle([0, 0, 479, NAV_Y - 1], fill=W_BG)

    # ---- 头部: 城市 (左, 22px, 顶 y30) + 日期 + 更新时间 (右, 16px 顶 y36) ----
    big_text(d, 10, 30, w["city"], 22, W_TEXT, W_BG)
    hdr = w["date"] + "  " + w["update"]
    hw = aa_text_width(hdr)
    hw_text(d, 470 - hw, 36 + 8, hdr, W_DIM, W_BG, "lm")   # 硬件 top=36 -> 中心=44

    # ---- 主区左: 大温度数字 (124x112, 顶 y52) + 度号 (x152, y60, 22px) ----
    big = alm_big_number(w["temp"], 0xFFFF, 0x0000)   # fg=白 笔画, bg=黑 底
    img.paste(big, (20, 52))
    d = ImageDraw.Draw(img)
    big_glyph(img, 152, 60, 0x00B0, 22, W_TEXT, W_BG)   # ° 紧跟数字右缘
    d = ImageDraw.Draw(img)

    # ---- 主区右: 大图标 (72px, 中心 402,108) ----
    w_icon(d, w["icon"], 402, 108, 72, W_BG)

    # ---- 主区中: 状况/体感/最高最低/湿度/风力 (16px, 左锚点 x165; 硬件 top=y) ----
    hw_text(d, 165, 62 + 8,  w["cond"],                        W_TEXT,   W_BG, "lm")
    hw_text(d, 165, 86 + 8,  "体感 %d°" % w["feels"],           W_DIM,    W_BG, "lm")
    hw_text(d, 165, 110 + 8, "最高 %d°  最低 %d°" % (w["hi"], w["lo"]), W_TEXT, W_BG, "lm")
    hw_text(d, 165, 134 + 8, "湿度 %d%%" % w["humidity"],        W_ACCENT, W_BG, "lm")
    hw_text(d, 165, 158 + 8, "%s %d级" % (w["windDir"], w["windScale"]), W_TEXT, W_BG, "lm")

    # ---- 卡片区: 4 张 (x=10,114,218,322, 宽 96, 高 50, y=178) ----
    def card(x, label, value, vcol, unit):
        d.rounded_rectangle([x, 178, x + 96 - 1, 178 + 50 - 1], radius=8, fill=W_PANEL, outline=W_EDGE, width=1)
        hw_small(d, x + 8, 178 + 7, label, 12, W_DIM, W_PANEL, "l")          # 顶行 label (左)
        if unit:
            uw = aa_text_width(unit)
            hw_text(d, x + 96 - 8 - uw, 178 + 7 + 8, unit, W_DIM, W_PANEL, "lm")  # 顶行 unit (右)
        big_text(d, x + 96 // 2, 178 + 26, value, 24, vcol, W_PANEL, "c")    # value (居中 24px)
    card(10,  "气压",   "%d" % w["pressure"], W_TEXT, "hPa")
    card(114, "紫外线", "%.0f" % w["uv"],    W_SUN,  "UV")
    card(218, "日出",   w["sunrise"],        W_TEXT, "")
    card(322, "日落",   w["sunset"],         W_TEXT, "")

    # ---- 7 天预报条 (y232 分隔线; 三行垂直居中; 每列中心 cx=40+i*68) ----
    d.line([8, 232, 472, 232], fill=W_EDGE)
    for i in range(7):
        cx = 40 + i * 68
        dc = W_ACCENT if i == 0 else W_TEXT
        hw_small(d, cx, 234, w["day"][i], 14, dc, W_BG, "c")          # 行1 星期 (顶 y234)
        w_icon(d, w["dayIcon"][i], cx, 262, 20, W_BG)                 # 行2 图标 (中心 y262)
        hb = "%d°" % w["dayHi"][i]
        lb = "%d°" % w["dayLo"][i]
        hw2 = aa_text_width(hb); lw2 = aa_text_width(lb)
        x0 = cx - (hw2 + 4 + lw2) // 2
        hw_text(d, x0,           274 + 8, hb, W_TEXT, W_BG, "lm")     # 行3 高/低温 (顶 y274)
        hw_text(d, x0 + hw2 + 4, 274 + 8, lb, W_DIM,  W_BG, "lm")

    # 导航栏 (选中 天气=idx 3)
    draw_nav(d, sel=3)
    return img


def scene(digits, flips=None, gears=True, white=True, gear_scroll=0.0):
    """flips: 每位的 (phase, progress, old_d); phase 0=稳态。None 时全稳态。
    gear_scroll: 齿轮累计滚动距离(px), 由调用方按翻页进度累加, 使齿轮与翻牌同步。"""
    img = Image.new("RGB", (480, 320), (0, 0, 0))
    d = ImageDraw.Draw(img)
    draw_date(d)
    if flips is None:
        flips = [(0, 0.0, 0)] * 6
    for i, ch in enumerate(digits):
        new_d = ord(ch) - 48
        phase, progress, old_d = flips[i]
        if phase == 0:
            draw_flap_card(img, d, i, new_d, new_d, 0, 0.0)
        else:
            draw_flap_card(img, d, i, old_d, new_d, phase, progress)
    if gears:
        # 竖直齿轮贯穿整张卡片高度 (CARD_TOP..CARD_TOP+CARD_H)
        draw_roller(d, GAP1_CX, CARD_TOP, CARD_TOP + CARD_H - 1, gear_scroll)
        draw_roller(d, GAP2_CX, CARD_TOP, CARD_TOP + CARD_H - 1, gear_scroll)
    draw_mode_hint(d, 0)   # 顶右侧样式提示 (与实机一致)
    draw_nav(d)
    return img

def labeled(img, text):
    out = Image.new("RGB", (480, 348), (18, 18, 22))
    out.paste(img, (0, 28))
    ImageDraw.Draw(out).text((10, 14), text, font=F20, fill=(255, 255, 255), anchor="lm")
    return out

def main():
    # 稳态静态图 (白底 + 竖直齿轮)
    # 翻页样式静态图: 默认用当前时间 (与设备 NTP 一致, 方便对照)
    now = datetime.datetime.now()
    # 节假日: 联网时同步 当年+次年 (镜像固件 boot 同步); 离线/失败则用本地缓存, 绝不阻塞预览
    try:
        import sync_holidays
        sync_holidays.fetch(now.year)
        sync_holidays.fetch(now.year + 1)
    except Exception as e:
        print("holiday auto-sync skipped (offline? using cache): %s" % e)
    digits = "%02d%02d%02d" % (now.hour, now.minute, now.second)
    after = scene(digits, gears=True, white=True, gear_scroll=0)
    after.save(OUT_DIR + "\\preview_after.png")
    # 指针样式静态图 (同一时刻, 秒针带小数)
    analog = draw_analog_scene(now.hour, now.minute, now.second, now.microsecond // 1000)
    analog.save(OUT_DIR + "\\preview_analog.png")
    # 万年历页面静态图
    cal = draw_calendar_scene(now.year, now.month)
    cal.save(OUT_DIR + "\\preview_calendar.png")
    # 天气页静态图 (镜影 main.cpp drawWeatherScreen; mock 数据)
    # 天气页: 优先拉取真实天气 (open-meteo, 纯 HTTP, 与固件同源); 失败退回 mock。
    _wx = fetch_weather_real()
    if _wx:
        print("[Wx] preview using live open-meteo data: %s %dC %s" % (_wx["city"], _wx["temp"], _wx["cond"]))
    else:
        print("[Wx] preview using mock weather")
    draw_weather_scene(_wx or WEATHER_MOCK).save(OUT_DIR + "\\preview_weather.png")
    # 黄历弹层示例图 (镜像 main.cpp drawAlmPopup): 今天 / 本周六 / 25号
    import calendar as _cald
    def _safe_dt(dt):
        return dt if dt.day <= _cald.monthrange(dt.year, dt.month)[1] else None
    d1 = _safe_dt(datetime.date(now.year, now.month, now.day))
    d2 = _safe_dt(datetime.date(now.year, now.month, 1)
                  + datetime.timedelta(days=(5 - datetime.date(now.year, now.month, 1).weekday()) % 7))
    d3 = _safe_dt(datetime.date(now.year, now.month, 25))
    for tag, dt in (("alm_workday", d1), ("alm_weekend", d2), ("alm_month25", d3)):
        if dt is None:
            continue
        draw_calendar_scene(dt.year, dt.month,
                            selected=(dt.year, dt.month, dt.day), almanac=True).save(
            OUT_DIR + "\\preview_%s.png" % tag)
        print("saved preview_%s.png (%d-%d-%d)" % (tag, dt.year, dt.month, dt.day))

    # --- 收音机页面静态示例图 (镜像参考项目: 分类侧栏 全部/收藏/历史/个人 + 地区分类) ---
    _cats = radio_cats()
    def _cat_idx_by_key(key):
        for i, c in enumerate(_cats):
            if c["key"] == key:
                return i
        return -1
    def _radio_stations(ci):
        return _cats[ci]["stations"] if 0 <= ci < len(_cats) else []
    if _cats:
        # 1) 默认: "全部" 分类 (侧栏顶部, 无播放)
        draw_radio_scene(cur_cat=0).save(OUT_DIR + "\\preview_radio_idle.png")
        # 2) 定位省份 "广东" 播放中 + 收藏 + 列表滚动 (侧栏 收藏 高亮为红心)
        ri = _cat_idx_by_key(_locate_province() or RADIO_PINNED)
        if ri < 0:
            ri = min(1, len(_cats) - 1)
        sts = _radio_stations(ri)
        if sts:
            favs = [sts[0].sid, sts[1].sid]
            draw_radio_scene(cur_cat=ri, list_scroll_top=1, playing_idx=1,
                             playing=True, favorites=favs).save(OUT_DIR + "\\preview_radio_playing.png")
            # 3) 全屏
            draw_radio_scene(cur_cat=ri, playing_idx=1, playing=True,
                             favorites=[sts[1].sid], fullscreen=True).save(OUT_DIR + "\\preview_radio_fullscreen.png")
        # 4) "收藏" 功能分类 (展示 45 条收藏电台)
        fi = _cat_idx_by_key("收藏")
        if fi >= 0 and _radio_stations(fi):
            draw_radio_scene(cur_cat=fi).save(OUT_DIR + "\\preview_radio_favs.png")
        print("radio preview saved (cats=%d)" % len(_cats))
    else:
        print("radio: no data -> skip radio preview")

    # 翻页动图: 秒位 7 -> 8, 完整两相翻页; 齿轮每翻完一相走一格(一次翻页=两格)
    frames = []
    P1, P2 = 400, 340   # 再放慢版翻页时序 (与硬件 FLIP_TOP_MS/FLIP_BOT_MS 一致)
    idle = (0, 0.0, 0)
    for f in range(22):
        el = f * 40.0
        scroll = 0.0
        if el < P1:
            p = el / P1
            flips = [idle] * 5 + [(1, p, 7)]
            scroll = _ease(p) * ROLLER_PITCH          # 第一相走完一格
        elif el < P1 + P2:
            p = (el - P1) / P2
            flips = [idle] * 5 + [(2, p, 7)]
            scroll = ROLLER_PITCH + _ease(p) * ROLLER_PITCH   # 第二相再走一格
        else:
            flips = [idle] * 6
            scroll = 2 * ROLLER_PITCH
        digits = "151408"
        frames.append(scene(digits, flips, gears=True, white=True, gear_scroll=scroll))
    frames[0].save(OUT_DIR + "\\preview_anim.gif", save_all=True,
                   append_images=frames[1:], duration=60, loop=0)
    print("done: preview_after.png preview_analog.png preview_calendar.png preview_anim.gif")

if __name__ == "__main__":
    main()

