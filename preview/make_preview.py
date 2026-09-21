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
import datetime
from PIL import Image, ImageDraw, ImageFont

DIGITS_H = r"D:\ESP32-S3\include\digits.h"
DIALNUM_H = r"D:\ESP32-S3\include\dialnum.h"
OUT_DIR = r"D:\ESP32-S3\preview"
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nongli_data

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
CARD_SEAM  = c565(0x2108)
CARD_SHADE = c565(0x1084)
CARD_EDGE  = c565(0x39C6)
ROW1_BG    = c565(0x2146)   # 主日期行底色 (深灰蓝, 突出)
ROW2_BG    = c565(0x18E3)   # 详情行底色 (近黑)
DATE_STRIP = c565(0x18E3)   # (兼容保留) 详情行底色
DATE_SEP   = c565(0x0821)   # 分隔竖线
NAV_BG     = c565(0x2104)
NAV_SEL    = c565(0x4A69)
CYAN       = c565(0x07FE)   # 青色 (星期 / 湿度)
GOLD       = c565(0xFE4C)   # 金色 (干支)

CARD_FACE_W = c565(0xFFFF)  # 白底 (0xFFFF = 纯白)

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

F16, F18, F20 = load_font(16), load_font(18), load_font(20)

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

def _shade_band(old_half, bandH, strength):
    """把旧半牌竖向压缩到 bandH 高, 并按竖向渐变变暗(靠近铰链端亮、翻过去那端暗),
    模拟翻片在空间中转动 (cosθ 透视压缩 + 背面变暗)。"""
    H = max(1, bandH)
    sq = old_half.resize((CARD_W, H), Image.LANCZOS)
    out = Image.new("RGB", sq.size, (0, 0, 0))
    sp, op = sq.load(), out.load()
    for yy in range(H):
        t = yy / max(1, H - 1)                    # 0 靠近铰链 .. 1 翻过去的远端
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
        band = _shade_band(HALF_BOT[old_d], bandH, p)
        img.paste(band, (x, seamY))
        d.line([x + 1, seamY + bandH, x + w - 2, seamY + bandH], fill=(120, 120, 135))

    # 中央铰链线
    d.line([x + 2, seamY, x + w - 3, seamY], fill=CARD_SEAM)
    d.line([x + 2, seamY + 1, x + w - 3, seamY + 1], fill=face)


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

def draw_nav(d):
    d.rectangle([0, NAV_Y, 479, 319], fill=NAV_BG)
    for i, lab in enumerate(["时钟", "日历", "收音机", "天气"]):
        x = i * NAV_BTN_W
        if i == 0:
            d.rounded_rectangle([x + 4, NAV_Y + 3, x + NAV_BTN_W - 5, NAV_Y + NAV_H - 4],
                                radius=6, fill=NAV_SEL)
        d.text((x + NAV_BTN_W // 2, 303), lab, font=F16, fill=(255, 255, 255), anchor="mm")

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
    lyz = ly - 1 if (lm >= 10 and m <= 2) else ly       # 春节前的日期生肖取前一年
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
    cx = (DATE_LEFT + DATE_RIGHT) // 2

    # ---- 行1: 公历+星期 | 农历+干支+节日 ----
    # 与 main.cpp 一致: 整行统一用 F16 (font16), 保证公历/星期/农历/干支/节日垂直对齐。
    d.rounded_rectangle([DATE_LEFT, ROW1_TOP, DATE_RIGHT, ROW1_TOP + ROW1_H - 1],
                        radius=8, fill=ROW1_BG)
    cy1 = ROW1_TOP + ROW1_H // 2
    x = DATE_LEFT + 12
    d.text((x, cy1), info["date"], font=F16, fill=WHITE, anchor="lm")
    x += F16.getlength(info["date"]) + 8
    d.text((x, cy1), info["week"], font=F16, fill=CYAN, anchor="lm")
    d.line([cx, ROW1_TOP + 9, cx, ROW1_TOP + ROW1_H - 9], fill=DATE_SEP)
    x = cx + 12
    d.text((x, cy1), info["lunar"], font=F16, fill=WHITE, anchor="lm")
    x += F16.getlength(info["lunar"]) + 6
    d.text((x, cy1), info["gz"], font=F16, fill=GOLD, anchor="lm")
    if info["fest"]:
        x += F16.getlength(info["gz"]) + 6
        fest_s = "今日" + info["fest"]
        d.text((x, cy1), fest_s, font=F16, fill=event_color(info["fest"]), anchor="lm")

    # ---- 行2: 节气 | 月相+天气 ----
    d.rounded_rectangle([DATE_LEFT, ROW2_TOP, DATE_RIGHT, ROW2_TOP + ROW2_H - 1],
                        radius=8, fill=ROW2_BG)
    cy2 = ROW2_TOP + ROW2_H // 2
    xj = DATE_LEFT + 12
    if info["cur_d"] == 0:
        # 节气当天: "今日XX" + 节日色, 后接下一节气 (灰蓝)
        cur_s = "今日" + info["cur"]
        d.text((xj, cy2), cur_s, font=F16, fill=event_color(info["cur"]), anchor="lm")
        xj += int(F16.getlength(cur_s)) + 10
        d.text((xj, cy2), "%s %d天后" % (info["nxt"], info["nxt_d"]), font=F16, fill=SOFT, anchor="lm")
    else:
        d.text((xj, cy2),
               "%s 已过%d天   %s %d天后" % (info["cur"], info["cur_d"], info["nxt"], info["nxt_d"]),
               font=F16, fill=SOFT, anchor="lm")
    # 右侧: 湿度 | 温度 | 月相名 | 月相图标 (自右向左)
    x = DATE_RIGHT - 12
    d.text((x, cy2), "%d%%" % MOCK_HUMIDITY, font=F16, fill=humid_color(MOCK_HUMIDITY), anchor="rm")
    x -= F16.getlength("%d%%" % MOCK_HUMIDITY) + 8
    d.text((x, cy2), "%d°C" % int(MOCK_TEMP), font=F16, fill=temp_color(int(MOCK_TEMP)), anchor="rm")
    x -= F16.getlength("%d°C" % int(MOCK_TEMP)) + 8
    d.text((x, cy2), info["moon"], font=F16, fill=MOON_TXT, anchor="rm")
    x -= F16.getlength(info["moon"]) + 6
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
    d.line([cx, cy, cx + math.cos(sa) * 60, cy + math.sin(sa) * 60], fill=AC_S, width=1)
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
    digits = "%02d%02d%02d" % (now.hour, now.minute, now.second)
    after = scene(digits, gears=True, white=True, gear_scroll=0)
    after.save(OUT_DIR + "\\preview_after.png")
    # 指针样式静态图 (同一时刻, 秒针带小数)
    analog = draw_analog_scene(now.hour, now.minute, now.second, now.microsecond // 1000)
    analog.save(OUT_DIR + "\\preview_analog.png")

    # 翻页动图: 秒位 7 -> 8, 完整两相翻页; 齿轮每翻完一相走一格(一次翻页=两格)
    frames = []
    P1, P2 = 150, 130
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
    print("done: preview_after.png preview_analog.png preview_anim.gif")

if __name__ == "__main__":
    main()

