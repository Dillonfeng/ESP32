// ============================================================
// geo_pin.h — 定位省份(收音机侧栏置顶): IP 反查 + LittleFS 缓存
// 镜像 mobile app.js 的 autoDetectProvinceOnLaunch:
//   本板无 GPS 模块, 无法走高德/OSM 逆地理, 只走 IP 兜底 (ip-api.com)。
//   设备无 TLS 能力 -> 一律纯 HTTP (WiFiClient + HTTPClient, 与 holiday_sync 同款)。
//   失败 -> 保留上次缓存 (绝不把列表弄空/弄乱)。
// pinned() 返回"定位省份"的【完整省名】(如 "黑龙江"/"内蒙古"), 空串=未定位。
//   (完整名用于 buildRadioCats 的 regionCat 键匹配; 显示侧 2 字缩写由 labelOf 负责, 与 mobile 一致)
// ============================================================
#pragma once
#include <Arduino.h>

namespace geo {
  void init();      // setup: LittleFS 挂载后调用 (幂等读缓存)
  void tick();      // loop: 联网后首次定位 + 每 24h 刷新 (不阻塞, 失败保留缓存)
  const char* pinned();  // 当前定位省份完整名 (2-3 字, 如 "黑龙江"), ""=未定位
}
