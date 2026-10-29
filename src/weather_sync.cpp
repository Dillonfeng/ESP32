// ============================================================
// weather_sync.cpp — 实时天气 (和风 QWeather) 在线拉取 + LittleFS 缓存
// 实现说明见 include/weather_sync.h
// 功能对齐 D:\WinCalender 老黄历.hta 的天气部分:
//   /v7/weather/now (实况) + /v7/weather/7d (7天预报) +
//   /airquality/v1/current|daily (AQI/PM) + /weatheralert/v1/current (气象预警) +
//   /v7/tropical/storm-list (活跃台风兜底)
// 专属 Host 仅支持 HTTPS -> WiFiClientSecure setInsecure()。
// 失败保留缓存。
// ============================================================
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>
#include "config.h"         // WEATHER_API_KEY / WEATHER_API_HOST
#include "weather_sync.h"
#include "geo_pin.h"
#include "sd_storage.h"   // 天气缓存: SD 优先 / LittleFS 回退
#include "ui_common.h"    // BUILDV5-TZFIX: epochToLocalNoLock
#include "puff.h"         // gzip 解压: 和风专属 Host 强制 gzip (忽略 Accept-Encoding)

namespace weather {

namespace {

#define REFRESH_MS      (30UL * 60UL * 1000UL)   // 30min
#define FAIL_RETRY_MS   (30UL * 1000UL)          // 失败后 30s 重试
#define HTTP_TIMEOUT_MS 10000                    // TLS 握手较慢, 给足 10s
const char* kFile = "/weather_cache.json";

bool          fsReady     = false;
unsigned long lastFetch   = 0;
unsigned long retryMs     = REFRESH_MS;   // 成功后的刷新间隔; 失败时缩短为 FAIL_RETRY_MS
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

// 和风 icon 代码 -> 设备图标 0..7 (0晴 1多云 2阴 3雾/霾 4雨 5毛毛雨 6雪 7雷阵雨)
// 状况中文名直接用接口返回的 text/textDay, 无需映射。
static int qwIcon(int code) {
  if (code == 100 || code == 150) return 0;                 // 晴 (日/夜)
  if ((code >= 101 && code <= 103) || (code >= 151 && code <= 153)) return 1;  // 多云/少云/晴间多云
  if (code == 104 || code == 154) return 2;                 // 阴
  if (code >= 500 && code <= 515) return 3;                 // 雾/霾/浮尘/沙尘
  if (code >= 300 && code <= 399) {                         // 雨
    if (code == 302 || code == 303 || code == 304) return 7;  // 雷阵雨
    if (code == 305 || code == 309) return 5;                 // 小雨/毛毛雨
    return 4;
  }
  if (code >= 400 && code <= 499) return 6;                 // 雪
  if (code == 900) return 0;                                // 热
  if (code == 901) return 2;                                // 冷
  return 2;
}

// 和风 severityColor -> 中文 (预警标题用: 暴雨+橙+预警)
static const char* colorCN(const char* sc, const char* headline) {
  if (sc && sc[0]) {
    if (!strcmp(sc, "Blue"))   return "蓝";
    if (!strcmp(sc, "Yellow")) return "黄";
    if (!strcmp(sc, "Orange")) return "橙";
    if (!strcmp(sc, "Red"))    return "红";
    if (!strcmp(sc, "White"))  return "白";
  }
  // 兜底: 从 headline 里找颜色字
  if (headline) {
    if (strstr(headline, "红")) return "红";
    if (strstr(headline, "橙")) return "橙";
    if (strstr(headline, "黄")) return "黄";
    if (strstr(headline, "蓝")) return "蓝";
    if (strstr(headline, "白")) return "白";
  }
  return "";
}

// 安全拷字符串: 字段缺失时 as<const char*>() 返回 nullptr, %s 传 NULL 在 ESP32 是 UB(会崩)。
static void safeStr(char* dst, size_t n, const char* s) { snprintf(dst, n, "%s", s ? s : ""); }

static bool parseYmd(const char* s, int* y, int* m, int* d) {
  if (!s || !s[0]) return false;
  return sscanf(s, "%d-%d-%d", y, m, d) == 3;
}
static bool parseIsoLocal(const char* s, int* y, int* mo, int* d, int* hh, int* mi) {
  if (!s || !s[0]) return false;
  return sscanf(s, "%d-%d-%dT%d:%d", y, mo, d, hh, mi) == 5;
}
// 0=周日 .. 6=周六 (Sakamoto)
static int weekdayFromYmd(int y, int m, int d) {
  static const int T[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + T[m - 1] + d) % 7;
}

// 和风 GET (专属 Host, HTTPS): path 不带 key (如 "/v7/weather/now?location=114.0,22.5")
// 专属 Host 强制 gzip 响应 -> puff 解压后再解析 JSON。
// 内存约束 (regression 修复): 这里不放任何 static 大缓冲。
//   clock_ui.cpp 的时钟池需 58.5KB 连续内部 RAM (boot 时 allocClockBufsResident);
//   原先 static gzBuf[10240] 占 .bss + static WiFiClientSecure 常驻 TLS 上下文
//   -> boot 最大连续块被压到池阈值以下 -> 指针/翻页时钟双双空白。
//   现改为: TLS client 每请求局部构造(析构即释放), 解压缓冲由 fetchAndCache 临时分配。
#define GZ_CAP  8192     // 7d 明文约 3.6KB, 留余量
static bool gunzip(const uint8_t* in, size_t inLen, uint8_t* out, size_t outCap, size_t* outLen) {
  if (inLen < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8) return false;
  uint8_t flg = in[3];
  size_t pos = 10;
  if (flg & 0x04) { if (pos + 2 > inLen) return false; pos += 2 + (in[pos] | (in[pos + 1] << 8)); } // FEXTRA
  if (flg & 0x08) { while (pos < inLen && in[pos]) pos++; pos++; }   // FNAME
  if (flg & 0x10) { while (pos < inLen && in[pos]) pos++; pos++; }   // FCOMMENT
  if (flg & 0x02) pos += 2;                                          // FHCRC
  if (pos + 8 > inLen) return false;
  unsigned long dstLen = outCap, srcLen = inLen - pos - 8;
  if (puff(out, &dstLen, in + pos, &srcLen) != 0) return false;
  *outLen = dstLen;
  return true;
}
static bool qwGetJson(const char* path, JsonDocument& doc, uint8_t* gzOut, size_t gzCap);

// GET + (可选 gzip) 解压 -> 明文写入 out (<= cap-1 字节), *plainLen 输出
static bool qwGetPlain(const char* path, uint8_t* out, size_t cap, size_t* plainLen) {
  WiFiClientSecure cli;     // 局部: 请求结束即析构, 不常驻 TLS 上下文
  cli.setInsecure();        // 免证书校验 (ESP32 不内置 CA; key 在 query 里已是 HTTPS 保护)
  char url[240];
  snprintf(url, sizeof(url), "https://%s%s%skey=%s",
           WEATHER_API_HOST, path, strchr(path, '?') ? "&" : "?", WEATHER_API_KEY);
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(cli, url)) { Serial.printf("[Wx] begin FAIL %s\n", path); return false; }
  int code = http.GET();
  if (code != 200) { Serial.printf("[Wx] HTTP %d %s\n", code, path); http.end(); return false; }
  String body = http.getString();
  http.end();
  if (body.length() >= 2 && (uint8_t)body[0] == 0x1f && (uint8_t)body[1] == 0x8b) {
    size_t n = 0;
    if (!out || !gunzip((const uint8_t*)body.c_str(), body.length(), out, cap - 1, &n)) {
      Serial.printf("[Wx] gunzip FAIL %s (%u B)\n", path, (unsigned)body.length());
      return false;
    }
    out[n] = 0;
    *plainLen = n;
    return true;
  }
  size_t n = body.length();
  if (n >= cap) n = cap - 1;
  memcpy(out, body.c_str(), n);
  out[n] = 0;
  *plainLen = n;
  return true;
}

static bool qwGetJson(const char* path, JsonDocument& doc, uint8_t* gzOut, size_t gzCap) {
  size_t n = 0;
  if (!qwGetPlain(path, gzOut, gzCap, &n)) return false;
  if (deserializeJson(doc, (const char*)gzOut)) {
    Serial.printf("[Wx] json FAIL %s (%u B)\n", path, (unsigned)n);
    return false;
  }
  return true;
}

// 去掉 UTF-8 字符串尾部后缀 (如 "深圳市"->"深圳", "南山区"->"南山")
static void trimSuffix(String& s, const char* suf) {
  size_t sl = strlen(suf);
  if (s.length() > sl && s.endsWith(suf)) s.remove(s.length() - sl);
}

// 上次 GeoAPI 反查用的坐标 + 结果 (落盘, 坐标未变则跳过请求)
static float s_qLat = 0, s_qLon = 0;
static char  s_qCity[16] = "", s_qDist[16] = "";

// 和风 GeoAPI 按坐标反查行政区 (对齐 HTA geoQwLookup): 返回 市 + 区县
static bool qwRevGeo(float lat, float lon, uint8_t* gz, size_t gzCap,
                     char* city, size_t cn, char* dist, size_t dn) {
  char path[72];
  snprintf(path, sizeof(path), "/geo/v2/city/lookup?location=%.4f,%.4f&lang=zh", lon, lat);
  JsonDocument doc;
  if (!qwGetJson(path, doc, gz, gzCap)) return false;
  JsonArray loc = doc["location"];
  if (!loc.size()) return false;
  JsonObject o = loc[0];
  String c = o["adm2"] | "";
  String d = o["name"] | "";
  trimSuffix(c, "市");
  trimSuffix(d, "区"); trimSuffix(d, "县"); trimSuffix(d, "市"); trimSuffix(d, "旗");
  if (d == c) d = "";
  // 自治州/地区/盟全称太长 (如"大理白族自治州大理"): 有县级名时显示县级名 (同 HTA)
  if ((c.endsWith("自治州") || c.endsWith("地区") || c.endsWith("盟")) && d.length()) { c = d; d = ""; }
  if (!c.length()) { c = o["adm1"] | ""; trimSuffix(c, "省"); trimSuffix(c, "市"); trimSuffix(c, "自治区"); }
  if (!c.length()) return false;
  snprintf(city, cn, "%s", c.c_str());
  snprintf(dist, dn, "%s", d.c_str());
  return true;
}

// UTC 日期时间 -> epoch (newlib 无 timegm; civil-from-days 算法)
static time_t utcEpoch(int y, int mo, int d, int hh, int mi) {
  y -= mo <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = (long)era * 146097 + (long)doe - 719468;
  return (time_t)days * 86400 + hh * 3600 + mi * 60;
}

// UTC ISO 时间 -> 本地 "MM/DD" (和风 daily 用 UTC 16:00Z 表示本地次日 0 点)
static bool isoUtcToLocalMd(const char* s, char* out, size_t n) {
  int y, mo, d, hh, mi;
  if (!s || sscanf(s, "%d-%d-%dT%d:%d", &y, &mo, &d, &hh, &mi) != 5) return false;
  time_t ep = utcEpoch(y, mo, d, hh, mi);
  if (ep <= 0) return false;
  struct tm lt; epochToLocalNoLock(ep, lt);
  snprintf(out, n, "%d/%02d", lt.tm_mon + 1, lt.tm_mday);
  return true;
}

// 中国标准 AQI (HJ 633-2012): PM2.5/PM10 浓度 -> IAQI (分段线性)
static int cnIaqi(float c, const float* bp) {
  static const int IA[8] = {0, 50, 100, 150, 200, 300, 400, 500};
  if (c <= bp[0]) return 0;
  for (int i = 1; i < 8; i++) {
    if (c <= bp[i]) {
      float v = (IA[i] - IA[i - 1]) * (c - bp[i - 1]) / (bp[i] - bp[i - 1]) + IA[i - 1];
      return (int)(v + 0.5f);
    }
  }
  return 500;
}
static int cnAqiFromPm(float pm25, float pm10) {
  static const float BP25[8] = {0, 35, 75, 115, 150, 250, 350, 500};
  static const float BP10[8] = {0, 50, 150, 250, 350, 420, 500, 600};
  int a = cnIaqi(pm25, BP25), b = cnIaqi(pm10, BP10);
  return a > b ? a : b;
}

// 和风逐日 AQI 只有约 3 天 -> 缺的日子用 open-meteo 逐小时 PM 求日均后按国标折算补齐
// (air-quality API 的 daily 变量名不可用, 只能用 hourly=pm2_5,pm10 自己算日均)
static bool openMeteoAqiFill(WData& d, float lat, float lon) {
  WiFiClient cli;
  HTTPClient http;
  char url[240];
  snprintf(url, sizeof(url),
           "http://air-quality-api.open-meteo.com/v1/air-quality?latitude=%.4f&longitude=%.4f"
           "&hourly=pm2_5,pm10&forecast_days=7&timezone=auto", lat, lon);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.useHTTP10(true);
  if (!http.begin(cli, url)) return false;
  http.addHeader("Accept-Encoding", "identity");
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  String body = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  JsonArray t = doc["hourly"]["time"], p25 = doc["hourly"]["pm2_5"], p10 = doc["hourly"]["pm10"];

  float s25[7] = {}, s10[7] = {}; int cnt[7] = {};
  for (size_t i = 0; i < t.size(); i++) {
    const char* ts = t[i] | "";                 // "YYYY-MM-DDTHH:MM"
    if (strlen(ts) < 10) continue;
    char md[7]; snprintf(md, sizeof(md), "%d/%d",
                         (ts[5]-'0') * 10 + (ts[6]-'0'), (ts[8]-'0') * 10 + (ts[9]-'0'));
    for (int k = 0; k < 7; k++) {
      if (strcmp(d.dayDate[k], md)) continue;
      if (!p25[i].isNull()) s25[k] += p25[i].as<float>();
      if (!p10[i].isNull()) s10[k] += p10[i].as<float>();
      cnt[k]++;
      break;
    }
  }
  bool any = false;
  for (int k = 0; k < 7; k++) {
    if (d.dayAqi[k] > 0 || cnt[k] == 0) continue;
    d.dayAqi[k] = cnAqiFromPm(s25[k] / cnt[k], s10[k] / cnt[k]);
    any = true;
  }
  return any;
}

// 台风强度/移动方向中文 (同 HTA TYPHOON_STATUS / TYPHOON_DIR)
static const char* tyStatus(const char* t) {
  if (!t) return "";
  if (!strcmp(t, "TD")) return "热带低压";
  if (!strcmp(t, "TS")) return "热带风暴";
  if (!strcmp(t, "STS")) return "强热带风暴";
  if (!strcmp(t, "TY")) return "台风";
  if (!strcmp(t, "STY")) return "强台风";
  if (!strcmp(t, "SuperTY")) return "超强台风";
  if (!strcmp(t, "ET")) return "温带气旋";
  return t;
}
static const char* tyDir(const char* d) {
  static const struct { const char* k; const char* v; } M[] = {
    {"N","北"},{"NNE","北东北"},{"NE","东北"},{"ENE","东东北"},{"E","东"},{"ESE","东东南"},
    {"SE","东南"},{"SSE","南东南"},{"S","南"},{"SSW","南西南"},{"SW","西南"},{"WSW","西西南"},
    {"W","西"},{"WNW","西西北"},{"NW","西北"},{"NNW","北西北"}
  };
  if (!d) return "";
  for (auto& e : M) if (!strcmp(d, e.k)) return e.v;
  return d;
}
// 从明文 JSON 中截取 "now" 子对象 (只解析这一段, 不建整棵树)
static bool extractNow(const char* s, const char** obj, size_t* len) {
  const char* k = strstr(s, "\"now\"");
  if (!k) return false;
  const char* p = strchr(k, '{');
  if (!p) return false;
  int depth = 0; bool inStr = false;
  for (const char* q = p; *q; q++) {
    if (inStr) { if (*q == '\\') q++; else if (*q == '"') inStr = false; continue; }
    if (*q == '"') inStr = true;
    else if (*q == '{') depth++;
    else if (*q == '}' && --depth == 0) { *obj = p; *len = (size_t)(q - p + 1); return true; }
  }
  return false;
}

// 活跃台风实况 /v7/tropical/storm-track (对齐 HTA: 强度/风速/气压/位置/移动)
// storm-track 含整条路径 (实测明文 47KB) 远超 GZ_CAP, 单独用 64KB PSRAM 缓冲解压,
// 只提取 "now" 子对象解析, 用完即释放。
static bool qwStormTrack(const char* id, char* out, size_t n) {
  struct Big { uint8_t* p; ~Big() { if (p) free(p); } } big{ (uint8_t*)ps_malloc(65536) };
  if (!big.p) return false;
  char path[80];
  snprintf(path, sizeof(path), "/v7/tropical/storm-track?stormid=%s", id);
  size_t plainLen = 0;
  if (!qwGetPlain(path, big.p, 65536, &plainLen)) return false;
  const char* obj = nullptr; size_t objLen = 0;
  if (!extractNow((const char*)big.p, &obj, &objLen)) {
    Serial.printf("[Wx] storm-track no now (%u B)\n", (unsigned)plainLen);
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, obj, objLen)) { Serial.println("[Wx] storm-track json FAIL"); return false; }
  JsonObject nw = doc.as<JsonObject>();
  if (nw.isNull()) return false;
  const char* la = nw["lat"] | "";
  const char* lo = nw["lon"] | "";
  if (!la[0] || !lo[0]) return false;
  snprintf(out, n, "%s · 最大风速 %d m/s · 中心气压 %d hPa · 向%s移动 %d km/h · 中心 %s°N %s°E",
           tyStatus(nw["type"] | ""), atoi(nw["windSpeed"] | "0"), atoi(nw["pressure"] | "0"),
           tyDir(nw["moveDir"] | ""), atoi(nw["moveSpeed"] | "0"), la, lo);
  return true;
}

// 联网: 依 WinCalender 顺序拉取 实况/7天/空气/预警/台风, 填 g + 写缓存。
// now 或 7d 失败 -> 整体失败 (保留缓存); 空气/预警/台风失败只缺对应块。
bool fetchAndCache() {
  // 位置 (对齐老黄历.hta 三级定位):
  //   ① geo::hasCoord() = WiFi-BSSID(WLoc) 区县级坐标 -> 直接给和风, 精度远高于省会
  //   ② 无坐标时按定位省份取省会坐标兜底 (最后才用)
  //   城市名: geo::city() (ipip 中文 / ip-api 英文), 空则用省会名
  const char* prov  = geo::pinned();
  const char* gcity = geo::city();
  const CityInfo* ci = nullptr;
  for (auto& c : CITIES) if (strcmp(c.prov, prov) == 0) { ci = &c; break; }
  if (!ci) ci = &CITIES[18];   // 广州 (index 18, 最终兜底)

  float lat = geo::hasCoord() ? geo::lat() : ci->lat;
  float lon = geo::hasCoord() ? geo::lon() : ci->lon;
  char locParam[24];
  snprintf(locParam, sizeof(locParam), "%.4f,%.4f", lon, lat);

  // 区县级地名 (左上角显示): 和风 GeoAPI 按坐标反查; 坐标未变则复用上次结果
  char cityBuf[16] = "", distBuf[16] = "";
  if (s_qCity[0] && fabsf(lat - s_qLat) < 0.003f && fabsf(lon - s_qLon) < 0.003f) {
    snprintf(cityBuf, sizeof(cityBuf), "%s", s_qCity);
    snprintf(distBuf, sizeof(distBuf), "%s", s_qDist);
  }

  // 解压缓冲: 仅在本次拉取期间存在, 优先 PSRAM (不碰内部连续块); 用完 free。
  // RAII: 任何提前 return 都会释放。
  struct GzGuard {
    uint8_t* p;
    ~GzGuard() { if (p) free(p); }
  } gz{ (uint8_t*)ps_malloc(GZ_CAP) };
  if (!gz.p) gz.p = (uint8_t*)malloc(GZ_CAP);
  if (!gz.p) { Serial.println("[Wx] no mem for gz buffer"); return false; }

  // 坐标变化时才请求 (每次定位/24h 刷新最多 1 次)
  if (!cityBuf[0] && geo::hasCoord()) {
    if (qwRevGeo(lat, lon, gz.p, GZ_CAP, cityBuf, sizeof(cityBuf), distBuf, sizeof(distBuf))) {
      s_qLat = lat; s_qLon = lon;
      snprintf(s_qCity, sizeof(s_qCity), "%s", cityBuf);
      snprintf(s_qDist, sizeof(s_qDist), "%s", distBuf);
      Serial.printf("[Wx] revgeo %.4f,%.4f -> %s %s\n", lat, lon, cityBuf, distBuf);
    }
  }

  WData d{};
  static const char* WK[7] = {"周日","周一","周二","周三","周四","周五","周六"};
  char path[200];

  // ---- 1) 实况 /v7/weather/now ----
  snprintf(path, sizeof(path), "/v7/weather/now?location=%s", locParam);
  {
    JsonDocument doc;
    if (!qwGetJson(path, doc, gz.p, GZ_CAP)) { Serial.println("[Wx] now FAIL"); return false; }
    JsonObject now = doc["now"];
    if (now.isNull()) { Serial.println("[Wx] no now object"); return false; }
    d.temp      = now["temp"].as<float>();
    d.feels     = now["feelsLike"].as<float>();
    d.humidity  = now["humidity"].as<int>();
    safeStr(d.windDir, sizeof(d.windDir), now["windDir"].as<const char*>());
    d.windScale = now["windScale"].as<int>();
    d.windSpeed = now["windSpeed"].as<int>();   // km/h
    d.pressure  = now["pressure"].as<float>();  // hPa
    d.visKm     = now["vis"].as<float>();       // km
    d.icon      = qwIcon(now["icon"].as<int>());
    safeStr(d.cond, sizeof(d.cond), now["text"].as<const char*>());

    // 当前日期/更新时间: obsTime 已是本地时间, 不依赖 NTP (开机 NTP 未同步也能显示)
    int ty = 0, tmo = 0, td = 0, thh = 0, tmi = 0;
    if (parseIsoLocal(now["obsTime"] | "", &ty, &tmo, &td, &thh, &tmi)) {
      int wd = weekdayFromYmd(ty, tmo, td);
      snprintf(d.date,   sizeof(d.date),   "%02d-%02d %s", tmo, td, WK[wd]);
      snprintf(d.update, sizeof(d.update), "%02d:%02d",    thh, tmi);
    } else {
      snprintf(d.date,   sizeof(d.date),   "--");
      snprintf(d.update, sizeof(d.update), "--:--");
    }
  }

  // ---- 2) 7 天预报 /v7/weather/7d ----
  snprintf(path, sizeof(path), "/v7/weather/7d?location=%s", locParam);
  {
    JsonDocument doc;
    if (!qwGetJson(path, doc, gz.p, GZ_CAP)) { Serial.println("[Wx] 7d FAIL"); return false; }
    JsonArray da = doc["daily"];
    if (da.size() < 1) { Serial.println("[Wx] no daily"); return false; }
    // 日出/日落/紫外线: 取今天
    JsonObject d0 = da[0];
    safeStr(d.sunrise, 8, d0["sunrise"].as<const char*>());
    safeStr(d.sunset,  8, d0["sunset"].as<const char*>());
    d.uv = d0["uvIndex"].as<float>();
    for (int i = 0; i < 7 && i < (int)da.size(); i++) {
      JsonObject e = da[i];
      int dy = 0, dm = 0, dd2 = 0;
      bool hasDate = parseYmd(e["fxDate"] | "", &dy, &dm, &dd2);
      if (hasDate) snprintf(d.dayDate[i], 6, "%d/%02d", dm, dd2);
      else         snprintf(d.dayDate[i], 6, "--");
      if (i == 0)      snprintf(d.dayLabel[i], 8, "今天");
      else if (i == 1) snprintf(d.dayLabel[i], 8, "明天");
      else if (hasDate) snprintf(d.dayLabel[i], 8, "%s", WK[weekdayFromYmd(dy, dm, dd2)]);
      else              snprintf(d.dayLabel[i], 8, "周?");
      safeStr(d.dayCond[i], sizeof(d.dayCond[i]), e["textDay"].as<const char*>());
      d.dayIcon[i] = qwIcon(e["iconDay"].as<int>());
      d.dayHi[i]   = e["tempMax"].as<int>();
      d.dayLo[i]   = e["tempMin"].as<int>();
      d.dayHum[i]  = e["humidity"].as<int>();
    }
  }

  // ---- 3) 空气实况 /airquality/v1/current/{lat}/{lng} (失败不影响主数据) ----
  snprintf(path, sizeof(path), "/airquality/v1/current/%.4f/%.4f", lat, lon);
  {
    JsonDocument doc;
    if (qwGetJson(path, doc, gz.p, GZ_CAP)) {
      JsonObject idx0 = doc["indexes"][0];   // cn-mee 中国标准 AQI
      if (!idx0.isNull()) d.aqi = idx0["aqi"].as<int>();
      for (JsonObject p : doc["pollutants"].as<JsonArray>()) {
        const char* pc = p["code"] | "";
        int v = (int)(p["concentration"]["value"].as<float>() + 0.5f);
        if (!strcmp(pc, "pm2p5")) d.pm25 = v;
        else if (!strcmp(pc, "pm10")) d.pm10 = v;
      }
    }
  }

  // ---- 4) 逐日 AQI: 和风 /airquality/v1/daily (免费版约 3 天) + open-meteo 折算补齐 7 天 ----
  // 对齐 HTA: 今天用实时实测 AQI; 未来按日期匹配和风 daily (cn-mee);
  // 和风没有的日子用 open-meteo 逐日 PM2.5/PM10 折 CN AQI, 保证 7 天都有药丸。
  snprintf(path, sizeof(path), "/airquality/v1/daily/%.4f/%.4f", lat, lon);
  {
    JsonDocument doc;
    if (qwGetJson(path, doc, gz.p, GZ_CAP)) {
      for (JsonObject day : doc["days"].as<JsonArray>()) {
        char md[7] = "";
        if (!isoUtcToLocalMd(day["forecastStartTime"] | "", md, sizeof(md))) continue;
        JsonArray idxs = day["indexes"];
        int aqi = 0;
        for (JsonObject ix : idxs) {
          const char* ic = ix["code"] | "";
          if (!strcmp(ic, "cn-mee") || !strcmp(ic, "qaqi") || !strcmp(ic, "caqi")) {
            aqi = ix["aqi"].as<int>(); break;
          }
        }
        if (aqi <= 0 && idxs.size()) aqi = idxs[0]["aqi"].as<int>();
        for (int i = 0; i < 7; i++)
          if (!strcmp(d.dayDate[i], md)) { d.dayAqi[i] = aqi; break; }
      }
    }
  }
  if (openMeteoAqiFill(d, lat, lon))
    Serial.println("[Wx] aqi: open-meteo 补齐剩余日");
  if (d.aqi > 0) d.dayAqi[0] = d.aqi;   // 今天优先实时实测 (同 HTA)

  // ---- 5) 气象预警 /weatheralert/v1/current/{lat}/{lng} ----
  // 台风(eventType 1001)单独进 typhoon 字段; 其余取第一条非台风预警入药丸。
  snprintf(path, sizeof(path), "/weatheralert/v1/current/%.4f/%.4f", lat, lon);
  {
    JsonDocument doc;
    if (qwGetJson(path, doc, gz.p, GZ_CAP)) {
      JsonArray alerts = doc["alerts"];
      d.alertCount = alerts.size();
      for (JsonObject a : alerts) {
        const char* etCode = a["eventType"]["code"] | "";
        const char* etName = a["eventType"]["name"] | "预警";
        const char* hl     = a["headline"] | "";
        bool isTy = !strcmp(etCode, "1001") || strstr(etName, "台风");
        if (isTy) {
          d.typhoon = true;
          // 台风名: 对齐 WinCalender —— headline 截取 "台风/热带风暴/..." 起的片段
          static const char* TY_KW[] = {"超强台风","强台风","强热带风暴","热带风暴","热带低压","台风"};
          const char* p = nullptr;
          for (auto& kw : TY_KW) { const char* q = strstr(hl, kw); if (q && (!p || q < p)) p = q; }
          if (p) {
            snprintf(d.typhoonName, sizeof(d.typhoonName), "%s", p);
            char* w = strstr(d.typhoonName, "预警");
            if (w) *w = 0;   // 去掉 "..预警信号" 尾巴
          } else {
            snprintf(d.typhoonName, sizeof(d.typhoonName), "%s", etName);
          }
          safeStr(d.typhoonDesc, sizeof(d.typhoonDesc), a["description"] | hl);
        } else if (d.alertTitle[0] == 0) {
          const char* cn = colorCN(a["severityColor"] | "", hl);
          snprintf(d.alertTitle, sizeof(d.alertTitle), "%s%s预警", etName, cn);
          safeStr(d.alertColor, sizeof(d.alertColor), cn);
          safeStr(d.alertDesc,  sizeof(d.alertDesc),  a["description"] | hl);
          int ay, am, ad, ah, ami;
          if (parseIsoLocal(a["issuedTime"] | "", &ay, &am, &ad, &ah, &ami))
            snprintf(d.alertTime, sizeof(d.alertTime), "%02d-%02d %02d:%02d", am, ad, ah, ami);
        }
      }
    }
  }

  // ---- 6) 活跃台风兜底 /v7/tropical/storm-list (仅当预警里没有台风) ----
  // 注意: 和风字段是 isActive (字符串 "1"), 之前误用 "active".as<int>() -> 永远匹配不到。
  if (!d.typhoon) {
    time_t t = time(nullptr);
    struct tm lt; epochToLocalNoLock(t, lt);
    int year = (lt.tm_year + 1900 >= 2024) ? lt.tm_year + 1900 : 2026;
    snprintf(path, sizeof(path), "/v7/tropical/storm-list?basin=NP&year=%d", year);
    JsonDocument doc;
    if (qwGetJson(path, doc, gz.p, GZ_CAP)) {
      int active = 0;
      for (JsonObject s : doc["storm"].as<JsonArray>()) {
        if (strcmp(s["isActive"] | "", "1") != 0) continue;
        active++;
        if (d.typhoon) continue;              // 只展示第一个 (徽标只放一个)
        d.typhoon = true;
        const char* nm = s["name"] | "";
        const char* id = s["id"] | "";
        snprintf(d.typhoonName, sizeof(d.typhoonName), "台风%s", nm);
        // 实况详情 (强度/风速/气压/位置/移动), 失败退回简单描述
        if (!id[0] || !qwStormTrack(id, d.typhoonDesc, sizeof(d.typhoonDesc)))
          snprintf(d.typhoonDesc, sizeof(d.typhoonDesc), "%s 正在西北太平洋活动", nm);
      }
      if (active) Serial.printf("[Wx] storm-list active=%d desc=%s\n", active, d.typhoonDesc);
    }
  }

  // 城市名: GeoAPI 反查 (市) 优先; 区县单独存 (顶栏显示到区县)
  safeStr(d.city, sizeof(d.city), cityBuf[0] ? cityBuf : (gcity[0] ? gcity : ci->city));
  safeStr(d.district, sizeof(d.district), distBuf);
  d.ok = true;
  g = d;

  // 写缓存 (失败不影响本次显示)
  if (fsReady) {
    File f = sdstore::openWrite(kFile);
    if (f) {
      JsonDocument o;
      o["city"] = d.city; o["date"] = d.date; o["update"] = d.update;
      o["cond"] = d.cond; o["windDir"] = d.windDir;
      o["temp"] = d.temp; o["feels"] = d.feels;
      o["humidity"] = d.humidity; o["windSpeed"] = d.windSpeed; o["windScale"] = d.windScale;
      o["pressure"] = d.pressure; o["uv"] = d.uv; o["icon"] = d.icon;
      o["sunrise"] = d.sunrise; o["sunset"] = d.sunset;
      o["visKm"] = d.visKm; o["aqi"] = d.aqi; o["pm25"] = d.pm25; o["pm10"] = d.pm10;
      o["alertCount"] = d.alertCount; o["alertTitle"] = d.alertTitle;
      o["alertColor"] = d.alertColor; o["alertDesc"] = d.alertDesc; o["alertTime"] = d.alertTime;
      o["typhoon"] = d.typhoon; o["typhoonName"] = d.typhoonName; o["typhoonDesc"] = d.typhoonDesc;
      o["district"] = d.district;
      // GeoAPI 反查结果 + 所用坐标 (开机/坐标未变时免请求)
      o["qCity"] = s_qCity; o["qDist"] = s_qDist;
      o["qLat"] = s_qLat; o["qLon"] = s_qLon;
      JsonArray da = o["day"].to<JsonArray>();
      for (int i = 0; i < 7; i++) {
        JsonObject e = da.add<JsonObject>();
        e["label"] = d.dayLabel[i]; e["icon"] = d.dayIcon[i];
        e["hi"] = d.dayHi[i]; e["lo"] = d.dayLo[i];
        e["date"] = d.dayDate[i]; e["cond"] = d.dayCond[i];
        e["hum"] = d.dayHum[i]; e["aqi"] = d.dayAqi[i];
      }
      serializeJson(o, f);
      f.close();
      Serial.printf("[Wx] fetched %s: %dC %s %s %d级 aqi=%d alerts=%d ty=%d\n", d.city, (int)d.temp,
                    d.cond, d.windDir, d.windScale, d.aqi, d.alertCount, (int)d.typhoon);
    }
  }
  g_seq++;   // 数据刷新 -> 通知 UI 重绘
  return true;
}

// 读缓存 -> g (启动即有数据, 无需等网络)
void loadCache() {
  File f = sdstore::openRead(kFile);
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
  d.windScale = doc["windScale"].as<int>(); d.pressure = doc["pressure"].as<float>();
  d.uv = doc["uv"].as<float>(); d.icon = doc["icon"].as<int>();
  safeStr(d.sunrise, 8, doc["sunrise"].as<const char*>());
  safeStr(d.sunset,  8, doc["sunset"].as<const char*>());
  d.visKm = doc["visKm"].as<float>();
  d.aqi = doc["aqi"].as<int>(); d.pm25 = doc["pm25"].as<int>(); d.pm10 = doc["pm10"].as<int>();
  d.alertCount = doc["alertCount"].as<int>();
  safeStr(d.alertTitle, sizeof(d.alertTitle), doc["alertTitle"].as<const char*>());
  safeStr(d.alertColor, sizeof(d.alertColor), doc["alertColor"].as<const char*>());
  safeStr(d.alertDesc,  sizeof(d.alertDesc),  doc["alertDesc"].as<const char*>());
  safeStr(d.alertTime,  sizeof(d.alertTime),  doc["alertTime"].as<const char*>());
  d.typhoon = doc["typhoon"].as<bool>();
  safeStr(d.typhoonName, sizeof(d.typhoonName), doc["typhoonName"].as<const char*>());
  safeStr(d.typhoonDesc, sizeof(d.typhoonDesc), doc["typhoonDesc"].as<const char*>());
  safeStr(d.district,    sizeof(d.district),    doc["district"].as<const char*>());
  // 恢复 GeoAPI 反查缓存 (坐标未变则开机不再请求)
  safeStr(s_qCity, sizeof(s_qCity), doc["qCity"].as<const char*>());
  safeStr(s_qDist, sizeof(s_qDist), doc["qDist"].as<const char*>());
  s_qLat = doc["qLat"].as<float>(); s_qLon = doc["qLon"].as<float>();
  JsonArray da = doc["day"];
  for (int i = 0; i < 7 && i < (int)da.size(); i++) {
    JsonObject e = da[i].as<JsonObject>();
    safeStr(d.dayLabel[i], 8, e["label"].as<const char*>());
    safeStr(d.dayDate[i],  6, e["date"].as<const char*>());
    safeStr(d.dayCond[i], 12, e["cond"].as<const char*>());
    d.dayIcon[i] = e["icon"].as<int>(); d.dayHi[i] = e["hi"].as<int>(); d.dayLo[i] = e["lo"].as<int>();
    d.dayHum[i] = e["hum"].as<int>(); d.dayAqi[i] = e["aqi"].as<int>();
  }
  d.ok = true;
  g = d;
  g_seq++;   // 缓存加载 -> 通知 UI 重绘 (开机即有数据)
  Serial.printf("[Wx] cached %s: %dC %s\n", d.city, (int)d.temp, d.cond);
}

} // anon

void init() {
  bool lfs = LittleFS.begin();   // 幂等: setup 里已挂载则立即返回 true
  fsReady = lfs || sdstore::mounted();   // SD 挂载成功也可作为缓存载体
  if (fsReady) loadCache();
}

void tick() {
  if (!WiFi.isConnected()) return;          // 离线不做网络请求
  uint32_t now = millis();
  if (!pendingInit && (uint32_t)(now - lastFetch) < retryMs) return;
  pendingInit = false;
  lastFetch = now;
  bool ok = fetchAndCache();                // 失败保留缓存
  retryMs = ok ? REFRESH_MS : FAIL_RETRY_MS;
  if (!ok) Serial.printf("[Wx] fetch failed, retry in %lus\n", (unsigned long)(FAIL_RETRY_MS / 1000UL));
}

void forceRefresh() { pendingInit = true; }   // UI 刷新按钮: 下一次 tick 立即拉取

const WData& get() { return g; }

unsigned seq() { return g_seq; }

} // namespace weather
