// ============================================================
// geo_pin.h — 定位坐标/省份/城市: WiFi-BSSID 定位 + IP 反查 + 缓存
// 对齐 D:\WinCalender\老黄历.hta 的三级定位:
//   ① WiFi/BSSID -> Apple gs-loc WLoc (移植 hl_geoloc.ps1) => 区县级坐标
//   ② myip.ipip.net (国内城市级 IP 库) => 省市名
//   ③ ip-api.com (常误判深圳=广州, 仅最终降级) => 省市名
// 设备无 GPS / Windows 定位服务, 故①用 WiFi 扫描等效复刻;
// WLoc 只给坐标, 名称(省/市)由②③提供。
// 失败 -> 保留上次缓存 (绝不把列表弄空/弄乱)。
// pinned() 省份完整名 (radio 侧栏 regionCat 键用), city() 城市名 (天气显示用),
// hasCoord()/lat()/lon() 区县级坐标 (天气接口用, 精度远高于省会映射)。
// ============================================================
#pragma once
#include <Arduino.h>

namespace geo {
  void init();      // setup: LittleFS 挂载后调用 (幂等读缓存)
  void tick();      // loop: 联网后首次定位 + 每 24h 刷新 (不阻塞, 失败保留缓存)
  const char* pinned();  // 当前定位省份完整名 (2-3 字, 如 "黑龙江"), ""=未定位
  const char* city();    // 当前定位城市名 (如 "深圳"), ""=未知
  bool  hasCoord();      // true=有 WLoc 区县级坐标
  float lat();           // 纬度 (无坐标时为 0)
  float lon();           // 经度 (无坐标时为 0)
}
