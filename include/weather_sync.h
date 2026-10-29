// ============================================================
// weather_sync.h — 实时天气 (和风 QWeather) 在线拉取 + LittleFS 缓存
// 失败 -> 保留上次缓存 (绝不把页面弄空/弄乱)。
//
// 数据源对齐 D:\WinCalender 老黄历.hta: 专属 Host HTTPS (WiFiClientSecure
// setInsecure), 6 接口: weather/now + weather/7d + airquality current|daily
// + weatheralert (气象预警/台风) + tropical/storm-list (活跃台风兜底)。
// key/host 见 config.h WEATHER_API_KEY / WEATHER_API_HOST。
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
  char   city[24];           // 显示城市名 (GeoAPI adm2, 如 "深圳")
  char   district[16];       // 区县名 (GeoAPI name, 如 "龙岗"; 空=无)
  char   date[16];           // "10-04 周日"
  char   update[8];          // "15:30"
  char   sunrise[8];         // "06:20"
  char   sunset[8];          // "18:08"
  float  visKm = 0;          // 能见度 km (current.visibility)
  int    aqi = 0;            // 空气质量指数 (air-quality API us_aqi; 0=无数据)
  int    pm25 = 0, pm10 = 0; // μg/m³
  // 7 天预报
  char   dayLabel[7][8];     // 今天/明天/周一..周五
  char   dayDate[7][6];      // "10/22"
  char   dayCond[7][12];     // 晴/多云/小雨..
  int    dayIcon[7];         // 0..7
  int    dayHi[7], dayLo[7];
  int    dayHum[7];          // 日均湿度 % (hourly 平均)
  int    dayAqi[7];          // 日 AQI: 今天=实时实测; 未来=和风daily, 缺的用 open-meteo PM 折算
  // 气象预警 (和风 weatheralert; alertCount=0 无预警)
  int    alertCount = 0;
  char   alertTitle[40];     // "暴雨橙色预警"
  char   alertColor[8];      // "蓝"/"黄"/"橙"/"红"/"白"
  char   alertDesc[200];     // 预警正文 (弹窗用, 截断)
  char   alertTime[16];      // "10-24 18:00"
  // 台风 (weatheralert eventType=1001 或 storm-list 活跃台风)
  bool   typhoon = false;
  char   typhoonName[24];    // "台风海鸥"
  char   typhoonDesc[200];   // 弹窗正文
};

void init();        // setup: 联网前先读缓存 (幂等)
void tick();        // loop: 联网后首次拉取 + 每 30min 刷新 (不阻塞, 失败保留缓存)
void forceRefresh();// UI 刷新按钮: 下一次 tick 立即拉取
const WData& get(); // 当前数据 (无数据时 ok=false, 调用方退回 mock)
unsigned seq();     // 每次成功刷新 +1 (用于 UI 判断"数据是否变化" -> 重绘天气页)
} // namespace weather
