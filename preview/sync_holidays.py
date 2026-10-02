#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""sync_holidays.py — 拉取节假日(放假/补班)并缓存到本地, 镜像固件 holiday_sync.cpp。

数据源: http://timor.tech/api/holiday/year/<year>  (纯 HTTP)
  返回: {"code":0,"holiday":{"MM-DD":{"holiday":true|false,"name":"春节",...},...}}
  holiday:true  = 放假(休);  holiday:false = 补班(班)

缓存文件: preview/holidays_<year>.json  (保存原始 JSON, 与固件 LittleFS 同内容)
  - make_preview.py 直接读该文件做高亮 (与固件 boot 时读缓存一致, 离线可用)
  - 拉取失败时保留上一次的好缓存 (不清空)

用法:
  python sync_holidays.py              # 同步 当年 + 次年 (与固件一致)
  python sync_holidays.py 2026 2027    # 同步指定年份
  python sync_holidays.py --offline    # 只打印现有缓存摘要, 不联网
"""
import json
import os
import sys
import datetime
import threading
import urllib.request

OUT_DIR = r"D:\ESP32-S3\preview"
URL = "http://timor.tech/api/holiday/year/%d"
TIMEOUT = 10


def cache_path(year):
    return os.path.join(OUT_DIR, "holidays_%d.json" % year)


def fetch(year):
    """联网拉取一年; 成功则写缓存并返回摘要 (放假/补班 天数), 失败返回 None (保留旧缓存)。"""
    try:
        req = urllib.request.Request(URL % year, headers={"User-Agent": "ESP32-preview/1.0"})
        with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
            data = r.read().decode("utf-8")
    except Exception as e:
        print("  [Hol] fetch %d failed: %s (保留旧缓存)" % (year, e))
        return None
    try:
        doc = json.loads(data)
    except Exception as e:
        print("  [Hol] parse %d error: %s (保留旧缓存)" % (year, e))
        return None
    with open(cache_path(year), "w", encoding="utf-8") as f:
        f.write(data)
    hol = doc.get("holiday", {})
    off = sum(1 for v in hol.values() if v.get("holiday", False))
    work = sum(1 for v in hol.values() if not v.get("holiday", False))
    print("  [Hol] synced %d: %d 放假 / %d 补班  -> %s" % (year, off, work, os.path.basename(cache_path(year))))
    return (off, work)


def fetch_async(years, done=None):
    """在**守护线程**里顺序 fetch, 调用方**立即返回** (不阻塞启动)。

    镜像固件 holiday_sync: boot 时读缓存、刷新放到 loop 后台 (绝不阻塞 setup)。
    之前 preview 启动时同步 fetch(当年)+fetch(次年), timor.tech 慢/不通时
    各阻塞 10s (TIMEOUT) -> 预览窗口要等 ~20s 才开。改成后台线程后窗口秒开,
    先渲染本地缓存, 后台拉新数据落盘 (done 回调可用来清 make_preview 的内存缓存)。
    daemon 线程 -> 主程序退出时不残留。
    """
    def _worker():
        for y in years:
            fetch(y)
        if done is not None:
            try:
                done()
            except Exception:
                pass
    t = threading.Thread(target=_worker, name="hol-sync", daemon=True)
    t.start()
    return t


def summary(year):
    p = cache_path(year)
    if not os.path.exists(p):
        print("  [Hol] %d: 无缓存 (%s)" % (year, p))
        return
    with open(p, encoding="utf-8") as f:
        doc = json.load(f)
    hol = doc.get("holiday", {})
    off = sum(1 for v in hol.values() if v.get("holiday", False))
    work = sum(1 for v in hol.values() if not v.get("holiday", False))
    print("  [Hol] %d: %d 放假 / %d 补班  (缓存)" % (year, off, work))


def main():
    args = [a for a in sys.argv[1:]]
    if args and args[0] == "--offline":
        now = datetime.datetime.now()
        for y in (now.year, now.year + 1):
            summary(y)
        return

    years = [int(a) for a in args if a.lstrip("-").isdigit()]
    if not years:
        now = datetime.datetime.now()
        years = [now.year, now.year + 1]   # 与固件一致: 当年 + 次年

    print("同步节假日: %s" % ", ".join(map(str, years)))
    for y in years:
        fetch(y)
    print("done. make_preview.py 将读取 preview/holidays_<year>.json 做高亮。")


if __name__ == "__main__":
    main()
