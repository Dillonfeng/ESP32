// ============================================================
// weather_sync.cpp — 实时天气 (open-meteo) 在线拉取 + LittleFS 缓存
// 实现说明见 include/weather_sync.h
// 模式镜像 geo_pin.cpp / holiday_sync.cpp: 纯 HTTP (无 TLS), 失败保留缓存。
// ============================================================
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>
#include "weather_sync.h"
#include "geo_pin.h"

namespace weather {

namespace {

#define REFRESH_MS      (30UL * 60UL * 1000UL)   // 30min
#define HTTP_TIMEOUT_MS 6000
const char* kFile = "/weather_cache.json";

bool          fsReady     = false;
unsigned long lastFetch   = 0;
bool          pendingInit = true;
unsigned      g_seq       = 0;   // 每次成功刷新 +1 (UI 据此判断是否重绘)
WData         g;

// 定位省份(完整名) -> 省会城市 (lat, lon, 显示名). 与 geo::pinned() 的中文省名一致。
struct CityInfo { const char* prov; const char* city; float lat, lon; };
static const CityInfo CITIES[] = {
  {"北京","北京",39.90,116.40}, {"上海","上海",31.23,121.47},
  {"天津","天津",39.08,117.20}, {"重庆","重庆",29.56,106.55},
  {"河北","石家庄",38.04,114.51},{"山西","太原",37.87,112.55},
  {"辽宁","沈阳",41.80,123.43}, {"吉林","长春",43.82,125.32},
  {"黑龙江","哈尔滨",45.80,126.53},{"江苏","南京",32.06,118.80},
  {"浙江","杭州",30.27,120.15}, {"安徽","合肥",31.82,117.22},
  {"福建","福州",26.07,119.30}, {"江西","南昌",28.68,115.85},
  {"山东","济南",36.65,117.00}, {"河南","郑州",34.75,113.62},
  {"湖北","武汉",30.59,114.30}, {"湖南","长沙",28.23,112.94},
  {"广东","广州",23.13,113.26}, {"广西","南宁",22.82,108.32},
  {"海南","海口",20.03,110.35}, {"四川","成都",30.57,104.07},
  {"贵州","贵阳",26.65,106.63}, {"云南","昆明",24.88,102.83},
  {"西藏","拉萨",29.65,91.14},  {"陕西","西安",34.34,108.94},
  {"甘肃","兰州",36.06,103.83}, {"青海","西宁",36.62,101.78},
  {"宁夏","银川",38.49,106.23}, {"新疆","乌鲁木齐",43.83,87.62},
  {"内蒙古","呼和浩特",40.84,111.75},{"香港","香港",22.32,114.17},
  {"澳门","澳门",22.20,113.54}, {"台湾","台北",25.03,121.57},
};

// open-meteo WMO 天气代码 -> 图标 0..7 + 中文状况名
static void codeToIconName(int code, int* icon, const char** name) {
  switch (code) {
    case 0:  *icon = 0; *name = "晴"; break;
    case 1:  *icon = 1; *name = "晴间多云"; break;
    case 2:  *icon = 1; *name = "多云"; break;
    case 3:  *icon = 2; *name = "阴"; break;
    case 45: case 48: *icon = 3; *name = "雾"; break;
    case 51: case 53: case 55: case 56: case 57: *icon = 5; *name = "毛毛雨"; break;
    case 61: case 63: case 65: case 66: case 67: *icon = 4; *name = "雨"; break;
    case 71: case 73: case 75: case 77: *icon = 6; *name = "雪"; break;
    case 80: case 81: case 82: *icon = 4; *name = "阵雨"; break;
    case 85: case 86: *icon = 6; *name = "阵雪"; break;
    case 95: *icon = 7; *name = "雷阵雨"; break;
    case 96: case 99: *icon = 7; *name = "雷雨伴冰雹"; break;
    default: *icon = 2; *name = "多云"; break;
  }
}

// 风向 (open-meteo wind_direction_10m, 度, 风"来自"方向) -> 中文
static void windDirName(float deg, char* out, size_t n) {
  static const char* DIRS[8] = {"北风","东北风","东风","东南风","南风","西南风","西风","西北风"};
  int idx = (int)((deg + 22.5f) / 45.0f) % 8;
  snprintf(out, n, "%s", DIRS[idx]);
}

// 风速 km/h -> 蒲福风级 (0..12)
static int windScaleFrom(float kmh) {
  static const float TH[] = {1,6,12,20,29,39,50,62,75,89,103,118};
  for (int i = 0; i < 12; i++) if (kmh < TH[i]) return i;
  return 12;
}

// 安全拷字符串: 字段缺失时 as<const char*>() 返回 nullptr, %s 传 NULL 在 ESP32 是 UB(会崩)。
static void safeStr(char* dst, size_t n, const char* s) { snprintf(dst, n, "%s", s ? s : ""); }

// 联网: GET open-meteo (纯 HTTP), 解析当前 + 7 天, 填 g + 写缓存。
bool fetchAndCache() {
  // 城市/经纬度: 定位省份 -> 省会; 未定位退回广州 (后续 30min 刷新会拿到真城市)
  const char* prov = geo::pinned();
  const CityInfo* ci = nullptr;
  for (auto& c : CITIES) if (strcmp(c.prov, prov) == 0) { ci = &c; break; }
  if (!ci) ci = &CITIES[18];   // 广州 (index 18)
  char city[24];
  snprintf(city, sizeof(city), "%s", ci->city);

  char url[220];
  snprintf(url, sizeof(url),
    "http://api.open-meteo.com/v1/forecast?latitude=%.2f&longitude=%.2f"
    "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,"
    "wind_speed_10m,wind_direction_10m,pressure_msl,uv_index"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min&forecast_days=7"
    "&timezone=Asia%%2FShanghai", ci->lat, ci->lon);

  static WiFiClient cli;
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(cli, url)) return false;
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  String body = http.getString();
  http.end();
  if (body.length() == 0) return false;

  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;

  WData d{};
  JsonObject cur = doc["current"];
  if (cur.isNull()) return false;
  d.temp     = cur["temperature_2m"].as<float>();
  d.feels    = cur["apparent_temperature"].as<float>();
  d.humidity = (int)cur["relative_humidity_2m"].as<int>();
  d.windSpeed= (int)cur["wind_speed_10m"].as<float>();
  d.windScale= windScaleFrom(cur["wind_speed_10m"].as<float>());
  windDirName(cur["wind_direction_10m"].as<float>(), d.windDir, sizeof(d.windDir));
  d.pressure = (int)cur["pressure_msl"].as<float>();
  d.uv       = cur["uv_index"].as<float>();
  int ic; const char* nm; codeToIconName((int)cur["weather_code"].as<int>(), &ic, &nm);
  d.icon = ic;
  safeStr(d.cond, sizeof(d.cond), nm);

  // 当前日期/更新时间 (NTP 本地时间)
  time_t now = time(nullptr);
  struct tm* lt = localtime(&now);
  if (!lt || lt->tm_year < 100) return false;
  static const char* WK[7] = {"周日","周一","周二","周三","周四","周五","周六"};
  snprintf(d.date,   sizeof(d.date),   "%02d-%02d %s", lt->tm_mon + 1, lt->tm_mday, WK[lt->tm_wday]);
  snprintf(d.update, sizeof(d.update), "%02d:%02d",    lt->tm_hour, lt->tm_min);

  // 7 天预报 (今天/明天/周一..周五)
  JsonObject daily = doc["daily"];
  JsonArray cArr  = daily["weather_code"];
  JsonArray hiArr = daily["temperature_2m_max"];
  JsonArray loArr = daily["temperature_2m_min"];
  JsonArray srArr = daily["sunrise"];
  JsonArray ssArr = daily["sunset"];
  if (srArr.size()) { const char* s = srArr[0].as<const char*>(); if (s) safeStr(d.sunrise, 8, s + 11); }
  if (ssArr.size()) { const char* s = ssArr[0].as<const char*>(); if (s) safeStr(d.sunset,  8, s + 11); }
  for (int i = 0; i < 7; i++) {
    if (i == 0)      snprintf(d.dayLabel[i], 8, "今天");
    else if (i == 1) snprintf(d.dayLabel[i], 8, "明天");
    else             snprintf(d.dayLabel[i], 8, "%s", WK[(lt->tm_wday + i) % 7]);
    if (i >= (int)cArr.size()) break;
    int di; const char* dn; codeToIconName((int)cArr[i].as<int>(), &di, &dn);
    d.dayIcon[i] = di;
    d.dayHi[i] = (int)hiArr[i].as<float>();
    d.dayLo[i] = (int)loArr[i].as<float>();
  }
  safeStr(d.city, sizeof(d.city), city);
  d.ok = true;
  g = d;

  // 写缓存 (失败不影响本次显示)
  if (fsReady) {
    File f = LittleFS.open(kFile, "w");
    if (f) {
      JsonDocument o;
      o["city"] = d.city; o["date"] = d.date; o["update"] = d.update;
      o["cond"] = d.cond; o["windDir"] = d.windDir;
      o["temp"] = d.temp; o["feels"] = d.feels;
      o["humidity"] = d.humidity; o["windSpeed"] = d.windSpeed; o["windScale"] = d.windScale;
      o["pressure"] = d.pressure; o["uv"] = d.uv; o["icon"] = d.icon;
      o["sunrise"] = d.sunrise; o["sunset"] = d.sunset;
      JsonArray da = o["day"].to<JsonArray>();
      for (int i = 0; i < 7; i++) {
        JsonObject e = da.add<JsonObject>();
        e["label"] = d.dayLabel[i]; e["icon"] = d.dayIcon[i];
        e["hi"] = d.dayHi[i]; e["lo"] = d.dayLo[i];
      }
      serializeJson(o, f);
      f.close();
      Serial.printf("[Wx] fetched %s: %dC %s %s %d级\n", d.city, (int)d.temp,
                    d.cond, d.windDir, d.windScale);
    }
  }
  g_seq++;   // 数据刷新 -> 通知 UI 重绘
  return true;
}

// 读缓存 -> g (启动即有数据, 无需等网络)
void loadCache() {
  File f = LittleFS.open(kFile, "r");
  if (!f) return;
  JsonDocument doc;
  if (deserializeJson(doc, f)) { f.close(); return; }
  f.close();
  WData d{};
  safeStr(d.city,    sizeof(d.city),    doc["city"].as<const char*>());
  safeStr(d.date,    sizeof(d.date),    doc["date"].as<const char*>());
  safeStr(d.update,  sizeof(d.update),  doc["update"].as<const char*>());
  safeStr(d.cond,    sizeof(d.cond),    doc["cond"].as<const char*>());
  safeStr(d.windDir, sizeof(d.windDir), doc["windDir"].as<const char*>());
  d.temp = doc["temp"].as<float>();      d.feels = doc["feels"].as<float>();
  d.humidity = doc["humidity"].as<int>(); d.windSpeed = doc["windSpeed"].as<int>();
  d.windScale = doc["windScale"].as<int>(); d.pressure = doc["pressure"].as<int>();
  d.uv = doc["uv"].as<float>(); d.icon = doc["icon"].as<int>();
  safeStr(d.sunrise, 8, doc["sunrise"].as<const char*>());
  safeStr(d.sunset,  8, doc["sunset"].as<const char*>());
  JsonArray da = doc["day"];
  for (int i = 0; i < 7 && i < (int)da.size(); i++) {
    JsonObject e = da[i].as<JsonObject>();
    safeStr(d.dayLabel[i], 8, e["label"].as<const char*>());
    d.dayIcon[i] = e["icon"].as<int>(); d.dayHi[i] = e["hi"].as<int>(); d.dayLo[i] = e["lo"].as<int>();
  }
  d.ok = true;
  g = d;
  g_seq++;   // 缓存加载 -> 通知 UI 重绘 (开机即有数据)
  Serial.printf("[Wx] cached %s: %dC %s\n", d.city, (int)d.temp, d.cond);
}

} // anon

void init() {
  fsReady = LittleFS.begin();   // 幂等: setup 里已挂载则立即返回 true
  if (fsReady) loadCache();
}

void tick() {
  if (!WiFi.isConnected()) return;          // 离线不做网络请求
  if (pendingInit) {
    pendingInit = false;
    lastFetch = millis();
    fetchAndCache();                        // 联网后首次拉取 (失败保留缓存)
  } else if (millis() - lastFetch > REFRESH_MS) {
    lastFetch = millis();
    fetchAndCache();                        // 30min 定期刷新
  }
}

const WData& get() { return g; }

unsigned seq() { return g_seq; }

} // namespace weather

