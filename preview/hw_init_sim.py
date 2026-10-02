#!/usr/bin/env python3
"""hw_init_sim.py - 模拟硬件 init 链 (镜像 main.cpp setup() 的关键步骤)。

启动时按固件 setup() 顺序逐项加载-校验, 报告每项的 OK/FAIL 状态。
preview 调用方根据状态决定是否走 AA 路径 / 降级画面, 不再用 simhei.ttf 兜底。

**设计目的 (方向 A)**: preview "看着好" 必须蕴含 "硬件 init 链也走通了",
否则 preview 漂亮但烧到硬件空白/花屏, 调试不可靠。

镜像的硬件 init 链 (main.cpp setup() @L1577 附近):
  1. LittleFS.begin()                         -> _sim_fs()
  2. loadFestivalConfig()                     -> _sim_festival()
  3. font16.reInit("/font_simhei_16.font")    -> _sim_font16()  (含 XAAX magic + 大端 count + size 校验)
  4. font24.reInit("/font_simhei_24.font")    -> _sim_font24()  (旧 1-bit 路径)
  5. loadRadioDB() (data/stations.bin)        -> _sim_db()     (含 ESP32RDB magic 校验)
  6. loadFavorites()                          -> _sim_fav()

**故障注入** (环境变量 HW_SIMULATE_FAIL, 复现已知 bug 验证降级画面):
  HW_SIMULATE_FAIL=font       -> font16 加载失败 (镜像文件缺失/PSRAM alloc 失败)
  HW_SIMULATE_FAIL=psram      -> loadAA 内部 heap_caps_malloc 失败 (中文空白, 同 09-22 字节序破口表现)
  HW_SIMULATE_FAIL=byteorder  -> 故意按小端读 count -> count=63503 -> size 校验失败
                                 (复现 2026-09-22 字节序 bug, 当时 preview 正常/硬件中文空白 = lockstep 破口)
  HW_SIMULATE_FAIL=db         -> stations.bin 加载失败 (收音机页 DB: MISSING)
  HW_SIMULATE_FAIL=fs         -> LittleFS 挂载失败 (所有 data/ 读不到)
  HW_SIMULATE_FAIL=festival   -> festival.json 加载失败
  不设 -> 全部正常 (与正常硬件一致)

字库文件格式 (data/font_simhei_16.font, ~1MB):
  magic "XAAX"(4) | ver(1)=1 | pad(1)=0 | n(2, **大端**) = 4088 |
  每字 [unicode(2, **大端**) | advance(1) | 16x16 alpha(256)]

硬件 loadAA() 关键校验 (xfont.cpp L651-690):
  - magic == "XAAX"
  - count = (blob[6] << 8) | blob[7]   // 大端, 与 gen_aa_font.py 一致
  - size >= 8 + count * 259
  - _aaUni[i] = (g[0] << 8) | g[1]    // 大端
  - _aaAdv[i] = g[2]
"""
import os
import json
import struct

DATA_DIR = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), os.pardir, "data"))

FAIL = os.environ.get("HW_SIMULATE_FAIL", "").strip().lower()


class InitState:
    """镜像 main.cpp setup() 各步骤的状态。属性命名与 boot log 一致。"""

    def __init__(self):
        # 6 个组件 (按 setup 顺序)
        self.fs_ok = False          # LittleFS.begin()
        self.festival_ok = False    # loadFestivalConfig()
        self.font16_ok = False      # XFont::reInit("/font_simhei_16.font")
        self.font24_ok = False      # XFont::reInit("/font_simhei_24.font")
        self.db_ok = False          # loadRadioDB() (stations.bin)
        self.fav_ok = False         # loadFavorites()
        # 详情 (镜像 [XFont]/[Fest]/[DB] boot log 行)
        self.fs_msg = ""
        self.font16_msg = ""
        self.font16_count = 0       # 字库字数 (4088 正常)
        self.font16_is_aa = False   # AA 路径 vs 1-bit 路径
        self.font24_msg = ""
        self.festival_msg = ""
        self.db_msg = ""
        self.fav_msg = ""

    def __str__(self):
        # 镜像 boot log 一行打印, 便于和真机串口对照
        return ("[HW-SIM] fs:%s | festival:%s | font16:%s(%s, %d字%s) | "
                "font24:%s | db:%s | fav:%s") % (
            "OK" if self.fs_ok else "FAIL",
            "OK" if self.festival_ok else "FAIL",
            "OK" if self.font16_ok else "FAIL",
            self.font16_msg, self.font16_count,
            " AA" if self.font16_is_aa else " 1bit",
            "OK" if self.font24_ok else "FAIL",
            "OK" if self.db_ok else "FAIL",
            "OK" if self.fav_ok else "FAIL")


# ---- 单项校验 (镜像硬件对应函数) ----

def _sim_fs():
    """镜像 LittleFS.begin(false)."""
    if FAIL == "fs":
        return False, "LittleFS 挂载失败 (HW_SIMULATE_FAIL=fs)"
    if not os.path.isdir(DATA_DIR):
        return False, "data/ 目录不存在: %s" % DATA_DIR
    return True, "LittleFS 已挂载"


def _sim_font16():
    """镜像 XFont::begin() -> magic "XAAX" -> loadAA() 全套校验."""
    if FAIL == "fs":
        return False, "字库文件不存在 (fs FAIL)", 0, False
    p = os.path.join(DATA_DIR, "font_simhei_16.font")
    if FAIL in ("font", "psram"):
        return False, ("loadAA: %s (HW_SIMULATE_FAIL=%s)"
                       % ("PSRAM alloc 失败" if FAIL == "psram" else "加载失败",
                          FAIL)), 0, False
    if not os.path.exists(p):
        return False, "字库文件不存在: %s" % p, 0, False
    try:
        blob = open(p, "rb").read()
    except Exception as e:
        return False, "无法打开字库文件: %s" % e, 0, False
    if len(blob) < 8:
        return False, "size %d < 8" % len(blob), 0, False
    # magic 校验 (与硬件 xfont.cpp begin() L86 一致)
    if blob[0:4] != b"XAAX":
        # 旧 1-bit 路径 (无 magic), 走旧 begin() 路径, 不算 AA
        # font16 历史一直是 AA 格式, 这里若走到 1-bit 视为 FAIL (font24 才是 1-bit)
        return False, "非 AA 格式 (无 XAAX magic)", 0, False
    # 大端 count (与硬件 loadAA L665 / gen_aa_font.py 一致)
    if FAIL == "byteorder":
        # 复现 2026-09-22 字节序 bug: 故意按小端读
        # 文件实际 n=4088=0x0FF8 (BE: blob[6]=0x0F, blob[7]=0xF8)
        # 小端读 -> 0xF80F = 63503 -> size 校验失败
        count = blob[6] | (blob[7] << 8)
    else:
        count = (blob[6] << 8) | blob[7]
    # size 校验 (与硬件 loadAA L666 一致)
    _sz = 64 if blob[4] == 3 else (24 if blob[4] == 4 else (32 if blob[4] == 2 else 16))   # ver3=64 ver4=24 ver2=32 ver1=16
    _per = 3 + _sz * _sz
    if len(blob) < 8 + count * _per:
        return False, ("size %d < 8+%d*%d 不符 (校验失败)" % (len(blob), count, _per)), 0, False
    return True, "AA 字库已加载", count, True


def _sim_font24():
    """镜像 XFont::begin() (旧 1-bit 路径, 无 XAAX magic)."""
    if FAIL == "fs":
        return False, "字库文件不存在 (fs FAIL)"
    p = os.path.join(DATA_DIR, "font_simhei_24.font")
    if not os.path.exists(p):
        return False, "字库文件不存在: %s" % p
    try:
        blob = open(p, "rb").read()
    except Exception as e:
        return False, "无法打开字库文件: %s" % e
    if len(blob) < 10:
        return False, "size %d < 10" % len(blob)
    return True, "1-bit 字库已加载"


def _sim_festival():
    """镜像 loadFestivalConfig() (data/festival.json)."""
    if FAIL == "fs":
        return False, "festival.json 不存在 (fs FAIL)"
    if FAIL == "festival":
        return False, "festival.json 解析失败 (HW_SIMULATE_FAIL=festival)"
    p = os.path.join(DATA_DIR, "festival.json")
    if not os.path.exists(p):
        return False, "festival.json 不存在"
    try:
        with open(p, encoding="utf-8") as f:
            json.load(f)
        return True, "festival.json 已加载"
    except Exception as e:
        return False, "festival.json 解析失败: %s" % e


def _sim_db():
    """镜像 loadRadioDB() (data/stations.bin, magic 'ESP32RDB')."""
    if FAIL == "fs":
        return False, "stations.bin 不存在 (fs FAIL)"
    if FAIL == "db":
        return False, "stations.bin 加载失败 (HW_SIMULATE_FAIL=db)"
    p = os.path.join(DATA_DIR, "stations.bin")
    if not os.path.exists(p):
        return False, "stations.bin 不存在"
    try:
        blob = open(p, "rb").read()
    except Exception as e:
        return False, "无法打开 stations.bin: %s" % e
    if len(blob) < 16 or blob[0:8] != b"ESP32RDB":
        return False, "stations.bin magic 不符"
    return True, "stations.bin 已加载"


def _sim_fav():
    """镜像 loadFavorites() (data/favorites.json, 不存在不算 FAIL)."""
    if FAIL == "fs":
        return False, "favorites 不存在 (fs FAIL)"
    p = os.path.join(DATA_DIR, "favorites.json")
    if not os.path.exists(p):
        return True, "favorites.json 不存在 (首次启动, 默认空)"
    try:
        with open(p, encoding="utf-8") as f:
            json.load(f)
        return True, "favorites.json 已加载"
    except Exception:
        return True, "favorites.json 解析失败 (用默认空)"


def run():
    """镜像 main.cpp setup() 顺序, 返回 InitState.

    每次调用都重新校验 (与硬件每次启动 boot 一致); 模块级缓存 = HW.
    """
    st = InitState()
    # 1. LittleFS (后续步骤全靠 fs)
    st.fs_ok, st.fs_msg = _sim_fs()
    if not st.fs_ok:
        print("[HW-SIM] %s -- 后续步骤全部 FAIL" % st.fs_msg)
        return st
    # 2. festival
    st.festival_ok, st.festival_msg = _sim_festival()
    # 3. font16 (关键: 决定中文是否显示)
    st.font16_ok, st.font16_msg, st.font16_count, st.font16_is_aa = _sim_font16()
    # 4. font24
    st.font24_ok, st.font24_msg = _sim_font24()
    # 5. db
    st.db_ok, st.db_msg = _sim_db()
    # 6. fav
    st.fav_ok, st.fav_msg = _sim_fav()
    print(st)
    return st


# 模块加载即执行 (镜像硬件 boot 时 setup() 必跑), 缓存到 HW 供 preview 各处查
HW = run()
