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
import sys
import time
import json
import datetime
import tkinter as tk
from PIL import Image, ImageDraw, ImageFont, ImageTk

import make_preview as mp  # 复用 scene()/draw_flap_card()/draw_roller() (有 __main__ 守卫, main() 不跑)
# 方向 A: make_preview 顶部已 import hw_init_sim 并跑 init (打印 boot log).
# 这里直接拿 HW 引用, 在 set_title 显示 font16/fs/db 状态, 让用户一眼看到当前
# 模拟的硬件 init 链是否走通. 故障注入: 环境变量 HW_SIMULATE_FAIL=font/psram/byteorder/...
import hw_init_sim
HW = hw_init_sim.HW

# 节假日: 联网时同步 当年+次年 (镜像固件 boot 读缓存 + loop 后台刷新).
# 关键: 用 fetch_async 的**后台守护线程**, 启动**不阻塞** —— 之前同步 fetch 两次,
# timor.tech 慢/不通时各等 10s (TIMEOUT), 窗口要 ~20s 才开。现在先渲染本地缓存,
# 后台拉新数据落盘后清 mp._hol_cache, 让下一帧重读最新缓存 (与固件 loop 里 tick()
# 后台刷新、绝不在 setup 里等网络一致)。
def _on_hol_done():
    try:
        mp._hol_cache.clear()   # 后台刷新落盘后, 让 make_preview 下一帧重读文件
    except Exception:
        pass
try:
    import sync_holidays as _sh
    _now = datetime.datetime.now()
    _sh.fetch_async([_now.year, _now.year + 1], done=_on_hol_done)
except Exception:
    pass

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
    cx = 200   # 行1 分隔线左移 (与 main.cpp/make_preview 一致), 给 "今日XX" 腾右缘空间
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

FLIP_TOP_MS = 400      # 上半翻片下压 (再放慢版, 与硬件 FLIP_TOP_MS 同步)
FLIP_BOT_MS = 340      # 下半翻片上翻 (与硬件 FLIP_BOT_MS 同步)
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


def _run_hw_lint():
    """启动前跑硬件反模式 lint (preview 看不出 SPI 推送过程问题, 靠静态扫描拦截).

    main.cpp 修硬件后每次启动 live_preview 会先跑 check_hw_patterns.py:
      - stdout 有内容 (WARN 或 ERROR) → 原样打印
      - exit != 0 (有 ERROR) → 5s 延时 + 提示, 让用户在启动 preview 前先修
    """
    import subprocess
    here = os.path.dirname(os.path.abspath(__file__))
    root_dir = os.path.dirname(here)
    lint_py = os.path.join(root_dir, 'scripts', 'check_hw_patterns.py')
    if not os.path.exists(lint_py):
        return
    try:
        r = subprocess.run([sys.executable, lint_py], cwd=root_dir,
                            capture_output=True, text=True, timeout=10)
    except Exception as e:
        print('[live_preview] check_hw_patterns 跳过: %s' % e)
        return
    # 有 stdout (OK 一行 / WARN 列表 / ERROR 列表) 就打印, 让用户看到 lint 跑了什么状态
    if r.stdout.strip():
        print(r.stdout)
    # exit != 0 (有 ERROR) → 5s 延时让用户先修; WARN-only 不延时
    if r.returncode != 0:
        print('>>> [live_preview] main.cpp 有 ERROR 级硬件反模式 (preview 看不出, 烧录会暴露).')
        print('>>> 建议先修 lint 报的 ERROR 再启动 preview. 5s 后仍启动 (WARN 不延时)...')
        time.sleep(5)


def main():
    _run_hw_lint()
    root = tk.Tk()
    root.resizable(False, False)
    cv = tk.Canvas(root, width=480, height=320, bg="black", highlightthickness=0)
    cv.pack()
    item = cv.create_image(0, 0, anchor="nw")
    sim = Sim()

    # 时钟样式 (与真机点击切换一致): 默认翻页, 按 "空格" 或 "s" 切换 翻页/指针
    style = [STYLE_FLAP]
    # 页面: 0=时钟 1=日历 2=收音机 (对应固件 MODE_CLOCK / MODE_CALENDAR / MODE_RADIO), 按 1/2/3 或 [ ] 切换
    PAGE_CLOCK, PAGE_CALENDAR, PAGE_RADIO = 0, 1, 2
    page = [PAGE_CLOCK]
    # 日历当前显示月份 (默认今天所在月), ← → 翻月
    _nm = datetime.datetime.now()
    cal_ym = [_nm.year, _nm.month]
    # 日历选中日 + 年月选择弹层状态 (镜像 main.cpp calSelY/M/D / calPickerOpen / calPickY/M)
    cal_sel = [(_nm.year, _nm.month, _nm.day)]
    cal_picker = [False]
    cal_pick_ym = [(_nm.year, _nm.month)]
    # 黄历弹层状态 (镜像 main.cpp calAlmOpen + 400ms 双击判定 calLastTap*/Ms)
    cal_alm_open = [False]
    cal_last_tap = [None]        # (day, year, month)
    cal_last_tap_ms = [0.0]
    # 收音机状态 (镜像参考项目: 分类 全部/收藏/历史/个人 + 地区; curCat / listScrollTop /
    # regionScrollTop / currentPlayingIdx / radioPlaying / favorites / showingFullscreen / lastTap*)
    _cats0 = mp.radio_cats()
    _fav_ids0 = set()
    try:
        raw = json.load(open(mp.BACKUP_JSON, encoding="utf-8"))
        _fav_ids0 = set(json.loads(raw.get("radio_favorites", "[]")))
    except Exception:
        pass
    radio_cat       = [0]                  # 当前分类下标 (curCat)
    radio_region_scroll = [0]             # 分类侧边栏滚动 (regionScrollTop)
    radio_list_scroll   = [0]             # 电台列表滚动 (listScrollTop)
    radio_playing_idx   = [-1]            # 当前播放电台下标 (currentPlayingIdx)
    radio_playing       = [False]         # 是否播放中 (radioPlaying)
    radio_favorites     = [set(_fav_ids0)]  # 收藏 (sid 集合, 镜像 favorites[])
    radio_fullscreen    = [False]         # 全屏 (showingFullscreen)
    radio_last_tap_row  = [-1]            # 双击判定 (lastTapRow)
    radio_last_tap_ms   = [0.0]
    radio_last_fs_ms    = [0.0]           # 全屏底部双击判定 (lastFsTap, 2026-10-22)
    radio_vol           = [100]           # 音量% (镜像 radioVolume, 全屏±用)
    label = tk.Label(root, text=" 时钟/日历/收音机/天气   |   [1/2/3] 时钟/日历/收音机 [空格]翻页⇄指针 [←→]翻月   |   收音机: 点电台=播放 拖拽=滚动 双击=全屏 ",
                     bg="#121212", fg="#00FFFF", font=("Microsoft YaHei", 9), anchor="w")
    label.pack(side="bottom", fill="x")

    # 日期条字体模式: False=AA(当前硬件, preview 默认) / True=1-bit(旧硬件, 对比用)
    font1bit = [False]

    def set_title():
        pagen = {PAGE_CLOCK: "时钟", PAGE_CALENDAR: "日历", PAGE_RADIO: "收音机"}[page[0]]
        if page[0] == PAGE_CLOCK:
            name = "机械翻页" if style[0] == STYLE_FLAP else "指针表盘"
        elif page[0] == PAGE_CALENDAR:
            name = "%d年%d月" % (cal_ym[0], cal_ym[1])
        else:
            name = "无数据" if not _cats0 else mp.radio_cat_label(radio_cat[0])
        fmode = "1-bit(旧硬件,对比)" if font1bit[0] else "AA(当前硬件)"
        # 方向 A: 标题显示硬件 init 状态 (font16/fs/db), 让用户一眼看到当前模拟的硬件 init 链.
        # font16 FAIL 时日期条中文变灰色矩形 (镜像硬件中文空白); 全 OK 时和真机一致显示 AA 中文.
        initstat = "font16:%s fs:%s db:%s" % (
            "OK" if HW.font16_ok else "FAIL",
            "OK" if HW.fs_ok else "FAIL",
            "OK" if HW.db_ok else "FAIL")
        root.title("ESP32 实时模拟 | %s(%s) | 日期条: %s | init: %s | [1/2]页面 [空格]样式 [←→]翻月" % (pagen, name, fmode, initstat))
        label.config(text=" 页面: %s(%s)   |   日期条: %s   |   init: %s   |   [1/2/3] 时钟/日历/收音机   |   [空格] 翻页⇄指针   |   [←→] 翻月   |   [f] AA⇄1-bit(旧) "
                     % (pagen, name, fmode, initstat))

    def toggle_style(e=None):
        style[0] = STYLE_FLAP if style[0] == STYLE_ANALOG else STYLE_ANALOG
        set_title()
        print("style -> %s" % ("flap" if style[0] == STYLE_FLAP else "analog"))
        return "break"

    def set_page(p, e=None):
        page[0] = p
        set_title()
        print("page -> %s" % ("clock" if p == PAGE_CLOCK else "calendar"))
        return "break"

    def step_month(delta, e=None):
        y, m = cal_ym[0], cal_ym[1] + delta
        while m < 1:
            m += 12; y -= 1
        while m > 12:
            m -= 12; y += 1
        cal_ym[0], cal_ym[1] = y, m
        set_title()
        return "break"

    def flash_hint(msg):
        """临时提示 (收音机/天气/配网 等模拟器暂不支持的区域)。"""
        label.config(text=" " + msg)
        root.after(1500, set_title)

    # ---- 日历点击 (逐条镜像 main.cpp handleCalendarTap / handlePickerTap) ----
    def _clk_on_click(x, y):
        import calendar as _cm
        if cal_alm_open[0]:
            # 黄历弹层打开: 点弹层内任意处关闭 (与 main.cpp handleCalendarTap 一致, 无关闭按钮)
            if mp.ALM_PX <= x < mp.ALM_PX + mp.ALM_PW and mp.ALM_PT <= y < mp.ALM_PT + mp.ALM_PH:
                cal_alm_open[0] = False
                set_title()
            return
        if cal_picker[0]:
            # 弹层: 年份 < > 步进 / 选月份 / 确定 / 取消 / 点面板外关闭
            if (mp.PICK_PT <= y <= mp.PICK_PB and mp.PICK_PANEL_X <= x < mp.PICK_PANEL_X + mp.PICK_PANEL_W):
                py, pm = cal_pick_ym[0]
                if y < mp.PICK_PT + 40:
                    return                                       # 标题带, 点了不处理
                if mp.PICK_PT + 40 <= y < mp.PICK_MTOP:        # 年份步进行
                    if x < 240:
                        py = max(2000, py - 1)
                    elif x > 240:
                        py = min(2099, py + 1)
                    cal_pick_ym[0] = (py, pm)
                    set_title()
                    return
                if mp.PICK_MTOP <= y < mp.PICK_MTOP + 2 * mp.PICK_MROW_H \
                        and mp.PICK_MGRID_X <= x < mp.PICK_MGRID_X + 6 * mp.PICK_MCOL_W:  # 月份网格
                    mc = (x - mp.PICK_MGRID_X) // mp.PICK_MCOL_W
                    mr = (y - mp.PICK_MTOP) // mp.PICK_MROW_H
                    m = mr * 6 + mc + 1
                    if 1 <= m <= 12:
                        cal_pick_ym[0] = (py, m)
                        set_title()
                    return
                if y > mp.PICK_PB - mp.PICK_MROW_H:            # 按钮行
                    if x < 240:                                  # 取消
                        cal_picker[0] = False
                        set_title()
                    else:                                         # 确定
                        y2, m2 = cal_pick_ym[0]
                        cal_ym[0], cal_ym[1] = y2, m2
                        cal_picker[0] = False
                        today = datetime.date.today()
                        if (y2, m2) == (today.year, today.month):
                            cal_sel[0] = (y2, m2, today.day)
                        else:
                            cal_sel[0] = (y2, m2, 1)
                        set_title()
                    return
                return                                         # 面板内缝隙 -> 忽略, 不关闭
            # 点面板外 -> 关闭
            cal_picker[0] = False
            set_title()
            return
        # 关闭态
        if y < mp.CAL_GRID_TOP:
            # 顶部: 中间 1/3 = 打开选择器; 左右 1/3 = 上一月/下一月
            if x < mp.CAL_X1 / 3:
                step_month(-1)
            elif x > mp.CAL_X1 * 2 / 3:
                step_month(1)
            else:
                cal_pick_ym[0] = (cal_ym[0], cal_ym[1])
                cal_picker[0] = True
                set_title()
            return
        # 日期网格: 命中某天 -> 选中 (显示其农历)
        if mp.CAL_GRID_TOP <= y < mp.CAL_GRID_TOP + 6 * mp.CELL_H \
                and mp.CAL_X0 <= x < mp.CAL_X1:
            cy, cm = cal_ym[0], cal_ym[1]
            first = datetime.date(cy, cm, 1)
            start_w = (first.weekday() + 1) % 7
            dim = _cm.monthrange(cy, cm)[1]
            c = (x - mp.CAL_X0) // mp.CELL_W
            r = (y - mp.CAL_GRID_TOP) // mp.CELL_H
            day = (r * 7 + c) - start_w + 1
            if 1 <= day <= dim:
                cal_sel[0] = (cy, cm, day)           # 单击 = 选中 (底部信息条显示其农历)
                now_ms2 = time.time() * 1000
                if (cal_last_tap[0] is not None and cal_last_tap[0] == (day, cy, cm)
                        and now_ms2 - cal_last_tap_ms[0] < 600):
                    cal_alm_open[0] = True            # 600ms 内双击同一天 -> 打开该日黄历 (与 main.cpp 一致)
                cal_last_tap[0] = (day, cy, cm)
                cal_last_tap_ms[0] = now_ms2
                set_title()
            return
        # 底部信息条 "今日" 按钮: 一键回到今天 (跳今天月 + 选中今天), 与 main.cpp 一致
        strip_top = mp.CAL_GRID_TOP + 6 * mp.CELL_H + 8
        if strip_top <= y < strip_top + 34:
            today = datetime.date.today()
            bt_w = mp.F16.getlength("今日") + 16
            bt_x = mp.CAL_X1 - 8 - bt_w
            if bt_x - 2 <= x < bt_x + bt_w + 2:
                cal_ym[0] = today.year
                cal_ym[1] = today.month
                cal_sel[0] = (today.year, today.month, today.day)
                set_title()

    # ---- 收音机点击 (逐条镜像参考项目: 分类侧栏 + 列表/播放栏) ----
    def _radio_stations(ci):
        return _cats0[ci]["stations"] if 0 <= ci < len(_cats0) else []

    def _radio_on_click(x, y):
        # 全屏模式 (2026-10-22 镜像固件 handleRadioTap): 返回chip | 收藏星 | 三控制钮 | 底部双击
        if radio_fullscreen[0]:
            if 8 <= x <= 72 and 6 <= y <= 32:                       # 返回 chip
                radio_fullscreen[0] = False
            elif 410 <= x <= 472 and 6 <= y <= 32:                  # 收藏星
                flash_hint("收藏切换 (模拟器不持久化)")
            elif FS_BTN_Y - 30 <= y <= FS_BTN_Y + 30:
                if FS_BTN_DN_X - 28 <= x <= FS_BTN_DN_X + 28:
                    radio_vol[0] = max(0, radio_vol[0] - 1)         # 音量− (镜像固件 ±1 档)
                elif FS_BTN_UP_X - 28 <= x <= FS_BTN_UP_X + 28:
                    radio_vol[0] = min(100, radio_vol[0] + 1)       # 音量+ (镜像固件 ±1 档)
                elif FS_BTN_MAIN_X - 30 <= x <= FS_BTN_MAIN_X + 30:
                    radio_playing[0] = not radio_playing[0]         # 播放/暂停
            elif mp.NAV_Y <= y < mp.NAV_Y + 30:                     # 底部提示条: 双击返回
                now_ms = time.time() * 1000
                if now_ms - radio_last_fs_ms[0] < 300:
                    radio_fullscreen[0] = False
                radio_last_fs_ms[0] = now_ms
            set_title()
            return
        # 侧边栏分类 (x < 150): 三分组(频道/省市/我的) 组标题行+分类行 整列滚动
        # (镜像 main.cpp sideRowTop/sideTotalH, 2026-10-22)
        if x < mp.R_SIDEBAR_W:
            S = radio_region_scroll[0]
            top = mp.R_HEADER_H + mp.R_HEAD_H2 - S
            idx = -1
            for i in range(len(_cats0)):
                ry = top + mp.side_row_top(i)
                rh = mp.R_SPECIAL_H if i < mp.R_SPECIAL_CNT else mp.R_ROW_H_SIDE
                if ry <= y < ry + rh:
                    idx = i
                    break
            if 0 <= idx < len(_cats0) and idx != radio_cat[0]:
                radio_cat[0] = idx
                radio_list_scroll[0] = 0
                set_title()
            return
        # 播放栏 (y >= R_LIST_BOT, 即 208..288) (2026-10-22 镜像固件 handleRadioTap):
        #   右缘圆钮(播放/暂停) | 音量区(静音切换) | 台名区(进全屏)
        if mp.R_LIST_BOT <= y < mp.NAV_Y:
            if mp.R_LIST_X <= x < mp.R_LIST_X + mp.R_LIST_W:
                relx = x - mp.R_LIST_X
                sts = _radio_stations(radio_cat[0])
                cur = radio_playing_idx[0]
                if relx >= mp.R_LIST_W - 44:                     # 圆钮: 播放/暂停
                    if radio_playing[0]:
                        radio_playing[0] = False
                    elif cur >= 0:
                        radio_playing[0] = True
                elif relx >= mp.R_LIST_W - 108:                  # 音量区: 静音切换 (模拟)
                    flash_hint("音量/静音 (模拟器不持久化)")
                elif cur >= 0:                                   # 台名/均衡器 -> 全屏
                    radio_fullscreen[0] = True
                set_title()
            return
        # 滚动条点击跳转 (x in [R_LIST_X+R_LIST_W, +R_SB_W]) (2026-10-22 像素)
        if mp.R_LIST_TOP <= y < mp.R_LIST_BOT and \
           mp.R_LIST_X + mp.R_LIST_W <= x < mp.R_LIST_X + mp.R_LIST_W + mp.R_SB_W:
            sts = _radio_stations(radio_cat[0])
            cnt = len(sts)
            if cnt > 0:
                smax = max(0, cnt * mp.R_ROW_H - mp.R_LIST_H)
                pos = (y - mp.R_LIST_TOP) * smax // mp.R_LIST_H
                radio_list_scroll[0] = min(max(pos, 0), smax)
                set_title()
            return
        # 列表区: 单击=播放, 双击(350ms 同一天)=全屏 (listScrollTop 像素偏移, 2026-10-22)
        if mp.R_LIST_TOP <= y < mp.R_LIST_BOT and x >= mp.R_LIST_X:
            row_idx = (radio_list_scroll[0] + (y - mp.R_LIST_TOP)) // mp.R_ROW_H
            sts = _radio_stations(radio_cat[0])
            if 0 <= row_idx < len(sts):
                now_ms = time.time() * 1000
                if now_ms - radio_last_tap_ms[0] < 500 and radio_last_tap_row[0] == row_idx:
                    radio_fullscreen[0] = True
                    set_title()
                else:
                    radio_playing_idx[0] = row_idx
                    radio_playing[0] = True
                    set_title()
                radio_last_tap_row[0] = row_idx
                radio_last_tap_ms[0] = now_ms
            return

    # ---- 收音机列表/侧栏 拖拽滚动状态 (镜像 main.cpp handleTouch 的 isDown 滚动: dy/3) ----
    # "zone" = "list"(右侧电台列表) / "side"(左侧地区分类, 可上下拖动) / None
    rdrag = {"on": False, "zone": None, "sy": 0, "start": 0, "moved": False}

    # ---- 鼠标点击 = 真机触摸 (逐条镜像 main.cpp handleTouch 的判定) ----
    def on_click(e):
        x, y = e.x, e.y
        if y >= mp.NAV_Y:                                   # 底部导航栏 (y>=288)
            idx = x // mp.NAV_BTN_W                          # 4 个 120px 按钮
            if idx == 0:
                set_page(PAGE_CLOCK)
            elif idx == 1:
                set_page(PAGE_CALENDAR)
            elif idx == 2:
                set_page(PAGE_RADIO)
            else:
                flash_hint(" 天气 页面模拟器暂不支持 (真机可点) ")
        elif page[0] == PAGE_CLOCK:
            if y < 30 and x < 100:                          # 左上 100x30 配网热区
                flash_hint(" 配网热区 (真机会弹 WiFiManager, 模拟器不支持) ")
            else:
                toggle_style()                               # 点时钟页 = 翻页/指针 切换
        elif page[0] == PAGE_CALENDAR:
            _clk_on_click(x, y)
        elif page[0] == PAGE_RADIO:
            _radio_on_click(x, y)

    # 按下: 收音机列表区 -> 只启动拖拽候选, 延迟到 release 判定 tap/drag;
    #       其它所有区域 -> 立即 on_click (保持"按下即响应"语义, 时钟/日历/双击不受影响)。
    # 注意: <Button-1> 与 <ButtonPress-1> 是同一事件 (tag "button-1"),
    #       只能绑一次, 后绑会覆盖前绑 —— 所以统一在这里处理"按下"。
    def _on_press(e):
        if page[0] == PAGE_RADIO:
            # 右侧电台列表区 -> 拖拽列表
            if mp.R_LIST_TOP <= e.y < mp.R_LIST_BOT and e.x >= mp.R_LIST_X:
                rdrag.update(on=True, zone="list", sy=e.y, start=radio_list_scroll[0], moved=False)
                return
            # 左侧地区分类区 (可上下拖动) -> 拖拽侧栏 (铺满到导航栏 NAV_Y)
            if e.x < mp.R_SIDEBAR_W and mp.R_HEADER_H + mp.R_HEAD_H2 <= e.y < mp.NAV_Y:
                rdrag.update(on=True, zone="side", sy=e.y, start=radio_region_scroll[0], moved=False)
                return
        rdrag["on"] = False
        rdrag["zone"] = None
        on_click(e)

    def _rdrag_move(e):
        if not rdrag["on"]:
            return
        dy = e.y - rdrag["sy"]
        if abs(dy) > 4:
            rdrag["moved"] = True
        if not rdrag["moved"]:
            return
        # 自然滚动 (与硬件 main.cpp 一致): 上划(鼠标向上, dy<0) = 看下面更多项,
        # 即 scrollTop 增加 -> 用 -dy。两侧均为像素级 1:1 跟手 (2026-10-22 问题③对齐)。
        if rdrag["zone"] == "list":
            sts = _radio_stations(radio_cat[0])
            cnt = len(sts)
            new_scroll = rdrag["start"] - dy
            smax = max(0, cnt * mp.R_ROW_H - mp.R_LIST_H)
            radio_list_scroll[0] = min(max(new_scroll, 0), smax)
        elif rdrag["zone"] == "side":
            # 整列像素级 1:1 跟手, 与 main.cpp 一致 (2026-10-22: 含组标题行 sideTotalH)
            total_h = mp.side_total_h()
            smax = max(0, total_h - (mp.NAV_Y - (mp.R_HEADER_H + mp.R_HEAD_H2)))
            new_scroll = rdrag["start"] - dy
            radio_region_scroll[0] = min(max(new_scroll, 0), smax)

    def _rdrag_end(e):
        # 释放: 若确实开始过拖拽且未移动 -> 按 tap 处理; 移动过 -> 只清状态。
        if rdrag["on"]:
            rdrag["on"] = False
            if rdrag["zone"] == "side":
                # 吸附: 视口顶所在行的行顶对齐视口顶 (镜像 main.cpp sideRowTop, 避免停在半行)
                total_h = mp.side_total_h()
                smax = max(0, total_h - (mp.NAV_Y - (mp.R_HEADER_H + mp.R_HEAD_H2)))
                snap = 0
                for k in range(len(_cats0)):
                    if mp.side_row_top(k) <= radio_region_scroll[0]:
                        snap = mp.side_row_top(k)
                radio_region_scroll[0] = min(snap, smax)
            elif rdrag["zone"] == "list":
                # 2026-10-22 问题③: 右列释放吸附到行顶 (与左侧一致)
                sts = _radio_stations(radio_cat[0])
                cnt = len(sts)
                smax = max(0, cnt * mp.R_ROW_H - mp.R_LIST_H)
                snap = (radio_list_scroll[0] + mp.R_ROW_H // 2) // mp.R_ROW_H * mp.R_ROW_H
                radio_list_scroll[0] = min(snap, smax)
            if not rdrag["moved"]:
                on_click(e)

    cv.bind("<Button-1>", _on_press)
    cv.bind("<B1-Motion>", _rdrag_move)
    cv.bind("<ButtonRelease-1>", _rdrag_end)

    # 键盘备用
    root.bind("<space>", toggle_style)
    root.bind("s", toggle_style)
    root.bind("S", toggle_style)
    root.bind("1", lambda e: set_page(PAGE_CLOCK))
    root.bind("2", lambda e: set_page(PAGE_CALENDAR))
    root.bind("3", lambda e: set_page(PAGE_RADIO))
    root.bind("[", lambda e: set_page(PAGE_CLOCK))     # [ = 上一页面
    root.bind("]", lambda e: set_page(PAGE_CALENDAR))  # ] = 下一页面
    root.bind("<Left>", lambda e: step_month(-1))
    root.bind("<Right>", lambda e: step_month(1))
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

        if page[0] == PAGE_RADIO:
            # 收音机页面 (对应固件 modeRadio)
            img = mp.draw_radio_scene(
                cur_cat=radio_cat[0],
                list_scroll_top=radio_list_scroll[0],
                region_scroll_top=radio_region_scroll[0],
                playing_idx=radio_playing_idx[0],
                playing=radio_playing[0],
                favorites=radio_favorites[0],
                fullscreen=radio_fullscreen[0])
        elif page[0] == PAGE_CALENDAR:
            # 万年历页面 (对应固件 modeCalendar); 传选中日 + 弹层状态
            img = mp.draw_calendar_scene(cal_ym[0], cal_ym[1], selected=cal_sel[0],
                                         picker=cal_picker[0], pick_ym=cal_pick_ym[0],
                                         almanac=cal_alm_open[0])
        elif style[0] == STYLE_ANALOG:
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
