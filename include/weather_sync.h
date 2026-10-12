// ============================================================
// weather_sync.h — 实时天气 (open-meteo) 在线拉取 + LittleFS 缓存
// 与 geo_pin / holiday_sync 同款模式: 设备无 TLS 能力 -> 一律纯 HTTP
// (WiFiClient + HTTPClient). 失败 -> 保留上次缓存 (绝不把页面弄空/弄乱)。
//
// open-meteo 无需 API key, 纯 HTTP, 一次请求返回 当前 + 7 天预报
// (温度/体感/最高最低/湿度/风/气压/UV/日出日落/天气代码)。
// 位置由 geo::pinned() 定位省份 -> 映射到省会城市经纬度 (无 GPS, 离线兜底广州)。
//
// 数据全部存固定 char/数值缓冲 (无 String 拼接 -> 无 realloc -> 无跨核
// use-after-free, 同 holiday_sync namePool 教训)。
// ============================================================
#pragma once
#include <Arduino.h>

namespace weather {

// 单份天气数据 (全部按值/固定缓冲, 指针永不悬垂)
struct WData {
  bool   ok = false;         // 是否有有效数据 (缓存或刚拉取)
  // 当前
  float  temp = 0, feels = 0, hi = 0, lo = 0;
  float  pressure = 0, uv = 0;
  int    humidity = 0, windSpeed = 0, windScale = 0;
  int    code = 0;           // open-meteo WMO 天气代码 (当前)
  int    icon = 0;           // 映射后的图标 0..7 (wicons)
  char   windDir[16];        // 如 "东南风"
  char   cond[24];           // 如 "多云"
  char   city[24];           // 显示城市名 (省会)
  char   date[16];           // "10-04 周日"
  char   update[8];          // "15:30"
  char   sunrise[8];         // "06:20"
  char   sunset[8];          // "18:08"
  // 7 天预报
  char   dayLabel[7][8];     // 今天/明天/周一..周五
  int    dayIcon[7];         // 0..7
  int    dayHi[7], dayLo[7];
};

void init();        // setup: 联网前先读缓存 (幂等)
void tick();        // loop: 联网后首次拉取 + 每 30min 刷新 (不阻塞, 失败保留缓存)
const WData& get(); // 当前数据 (无数据时 ok=false, 调用方退回 mock)
unsigned seq();     // 每次成功刷新 +1 (用于 UI 判断"数据是否变化" -> 重绘天气页)
} // namespace weather
