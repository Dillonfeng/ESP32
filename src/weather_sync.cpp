// ============================================================
// weather_sync.cpp — 实时天气 (和风 QWeather) 在线拉取 + LittleFS 缓存
// 实现说明见 include/weather_sync.h
// 功能对齐 D:\WinCalender 老黄历.hta 的天气部分 (同一 key/专属 Host):
//   /weather/v1/current + /weather/v1/daily (实况 / 7天预报, 和风新平台 V1) +
//   /v7/indices/1d?type=5 (紫外线) + 本地 NOAA 算法算日出日落 +
//   /airquality/v1/current|daily (AQI/PM) + /weatheralert/v1/current (气象预警)
// 台风: 中国气象局台风网 http://typhoon.nmc.cn (免费/无 key/明文 HTTP);
//   和风 /v7/tropical/* 属"台风和海洋"收费产品 (0.003 元/次), 已全部移除。
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

#define FAST_REFRESH_MS (10UL * 60UL * 1000UL)   // 10min: 实况 now + 预警 + Open-Meteo 实时 UV
#define FULL_REFRESH_MS (60UL * 60UL * 1000UL)   // 60min: 7天预报 + 空气 + 和风 UV 日值 + 台风
#define FAIL_RETRY_MS   (30UL * 1000UL)          // 失败后 30s 重试
#define HTTP_TIMEOUT_MS 10000                    // TLS 握手较慢, 给足 10s
const char* kFile = "/weather_cache.json";

bool          fsReady     = false;
unsigned long lastFetch   = 0;     // 最近一次尝试 (成功或失败), 用于失败重试节流
unsigned long lastFastTs  = 0;     // 最近一次成功快速刷新
unsigned long lastFullTs  = 0;     // 最近一次成功全量刷新
bool          lastFailed  = false; // 上次刷新是否失败 (失败时 30s 重试, 不等满 10min)
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

// 和风 icon 代码 -> 设备图标 0..15 (16 类, 对齐设计稿 V1.8)
// 0晴 1多云 2多云转晴 3阴 4小雨 5中雨 6大雨 7雷阵雨 8雨夹雪 9冻雨 10小雪 11大雪 12雾 13霾 14沙尘 15大风
// 状况中文名直接用接口返回的 text/textDay, 无需映射。
static int qwIcon(int code) {
  if (code == 100 || code == 150) return 0;                 // 晴 (日/夜)
  if (code == 101 || code == 151) return 1;                 // 多云
  if (code == 102 || code == 103 || code == 152 || code == 153) return 2;  // 少云/晴间多云 -> 多云转晴
  if (code == 104 || code == 154) return 3;                 // 阴
  if (code >= 500 && code <= 515) {                         // 雾/霾/浮尘/沙尘
    if (code == 502 || (code >= 509 && code <= 511)) return 13;  // 霾
    if (code == 503 || code == 504 || code == 505 || code == 506 || code == 515) return 14;  // 沙尘
    if (code == 514) return 9;                                 // 雨凇 -> 冻雨
    return 12;                                                 // 雾
  }
  if (code >= 300 && code <= 399) {                         // 雨
    if (code >= 302 && code <= 304) return 7;                 // 雷阵雨(伴冰雹)
    if (code == 305 || code == 309 || code == 314) return 4;  // 小雨/毛毛雨/小到中雨
    if (code == 306 || code == 315) return 5;                 // 中雨/中到大雨
    if (code == 313) return 9;                                // 冻雨
    return 6;                                                 // 大雨/暴雨..
  }
  if (code >= 400 && code <= 499) {                         // 雪
    if (code >= 404 && code <= 406) return 8;                 // 雨夹雪
    if (code == 402 || code == 403 || code == 409 || code == 410) return 11;  // 大雪/暴雪
    return 10;                                                // 小雪/阵雪
  }
  if (code == 900) return 0;                                // 热
  if (code == 901) return 3;                                // 冷
  return 1;
}

// ---- 和风新平台 V1 (/weather/v1/*) 适配 (对齐 WinCalender _normV1Current/_normV1Daily) ----
// V1 数值字段可能是 number, 也可能是字符串 (如 "104"), 统一转 int
static int numOrStr(JsonVariant v) {
  if (v.is<const char*>()) { const char* s = v.as<const char*>(); return s ? atoi(s) : 0; }
  return v.as<int>();
}
// V1 的天气现象 16 类别中文名 (icon 0..15) —— text 若是英文则按 code 本地化
static const char* condCN(int code) {
  static const char* T[16] = { "晴", "多云", "少云", "阴", "小雨", "中雨", "大雨", "雷阵雨",
                               "雨夹雪", "冻雨", "小雪", "大雪", "雾", "霾", "沙尘", "大风" };
  int k = qwIcon(code);
  return T[(k >= 0 && k <= 15) ? k : 1];
}
static void qwCondText(char* dst, size_t n, int code, const char* apiText) {
  bool cjk = false;
  if (apiText) for (const char* p = apiText; *p; ++p) if ((uint8_t)*p >= 0x80) { cjk = true; break; }
  if (cjk) { snprintf(dst, n, "%s", apiText); return; }
  snprintf(dst, n, "%s", condCN(code));
}
// 前向声明 (tyDir 定义在后, compassCN 复用其 16 方位映射)
static const char* tyDir(const char* d);

// V1 wind.direction.compass ("n"/"ne"/"nne"...) -> 16 方位中文名 (不含"风"字)
// 对齐 WinCalender _compassZh/dir16Zh: 统一"偏"式口径 (NNE=东北偏北), 不做"北东北"直译;
// "风"字由 UI 渲染时补 (详情卡空间紧, 16 方位名最多 4 字 + "风" + 风力需挤在 ~96px 列内)。
static const char* compassCN(const char* c) {
  if (!c || !c[0]) return "";
  // 风向不定 (vrb) -> "不定向" (同 HTA _compassZh)
  if ((c[0] == 'v' || c[0] == 'V') && (c[1] == 'r' || c[1] == 'R') &&
      (c[2] == 'b' || c[2] == 'B') && !c[3]) return "不定向";
  const char* r = tyDir(c);     // 16 方位 "偏" 式映射, 大小写不敏感
  return r ? r : "";
}
// 日出/日落 (NOAA 算法, 同 WinCalender sunTime, 输出东八区 HH:MM)
static void sunTime(int y, int m, int d, double lat, double lon, bool rise, char* out, size_t n) {
  const double D2R = 3.14159265358979 / 180.0, R2D = 180.0 / 3.14159265358979;
  double N1 = floor(275.0 * m / 9.0);
  double N2 = floor((m + 9) / 12.0);
  double N3 = 1.0 + floor((y - 4.0 * floor(y / 4.0) + 2.0) / 3.0);
  double N  = N1 - N2 * N3 + d - 30.0;
  double lngHour = lon / 15.0;
  double t = N + ((rise ? 6.0 : 18.0) - lngHour) / 24.0;
  double M = 0.9856 * t - 3.289;
  double L = M + 1.916 * sin(M * D2R) + 0.020 * sin(2.0 * M * D2R) + 282.634;
  L = fmod(fmod(L, 360.0) + 360.0, 360.0);
  double RA = atan(0.91764 * tan(L * D2R)) * R2D;
  RA = fmod(fmod(RA, 360.0) + 360.0, 360.0);
  RA += floor(L / 90.0) * 90.0 - floor(RA / 90.0) * 90.0;
  RA /= 15.0;
  double sinDec = 0.39782 * sin(L * D2R);
  double cosDec = cos(asin(sinDec));
  double cosH = (cos(90.833 * D2R) - sinDec * sin(lat * D2R)) / (cosDec * cos(lat * D2R));
  if (cosH > 1.0 || cosH < -1.0) { snprintf(out, n, "--:--"); return; }
  double H = rise ? 360.0 - acos(cosH) * R2D : acos(cosH) * R2D;
  H /= 15.0;
  double T = H + RA - 0.06571 * t - 6.622;
  double local = T - lngHour + 8.0;
  local = fmod(fmod(local, 24.0) + 24.0, 24.0);
  int hh = (int)local;
  int mm = (int)((local - hh) * 60.0);
  snprintf(out, n, "%02d:%02d", hh, mm);
}
// V1 daily 的 forecastStartTime (UTC ISO) -> 本地 年/月/日
static time_t utcEpoch(int y, int mo, int d, int hh, int mi);   // 定义在下方
static bool isoUtcToLocalYmd(const char* s, int* y, int* mo, int* d) {
  int yy, mm, dd, hh, mi;
  if (!s || sscanf(s, "%d-%d-%dT%d:%d", &yy, &mm, &dd, &hh, &mi) != 5) return false;
  time_t ep = utcEpoch(yy, mm, dd, hh, mi);
  if (ep <= 0) return false;
  struct tm lt; epochToLocalNoLock(ep, lt);
  *y = lt.tm_year + 1900; *mo = lt.tm_mon + 1; *d = lt.tm_mday;
  return true;
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
#define GZ_CAP  16384    // V1 /weather/v1/daily 明文约 12KB (含 metadata), 留余量
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

// 常驻 TLS 连接: 收音机播放时内部堆最大连续块仅 ~23KB (空闲 31KB), 原有实现每个请求
// 新建 WiFiClientSecure 并握手一次, 9 次/轮; 播放中曾实测主循环静默卡死在握手 (音频任务
// 仍存活, 无 panic/无 WDT)。改为 keep-alive 复用同一条连接 -> 每轮刷新只握手 1 次。
static WiFiClientSecure* s_cli = nullptr;

// GET + (可选 gzip) 解压 -> 明文写入 out (<= cap-1 字节), *plainLen 输出
static bool qwGetPlain(const char* path, uint8_t* out, size_t cap, size_t* plainLen) {
  if (!s_cli) {
    s_cli = new WiFiClientSecure();
    if (!s_cli) { Serial.println("[Wx] no mem for tls client"); return false; }
    s_cli->setInsecure();   // 免证书校验 (ESP32 不内置 CA; key 在 query 里已是 HTTPS 保护)
  }
  bool reused = s_cli->connected();
  if (!reused) {
    s_cli->setTimeout(HTTP_TIMEOUT_MS);
    if (!s_cli->connect(WEATHER_API_HOST, 443, HTTP_TIMEOUT_MS)) {
      Serial.printf("[Wx] tls connect FAIL %s\n", path);
      return false;
    }
  }
  char uri[220];
  snprintf(uri, sizeof(uri), "%s%skey=%s",
           path, strchr(path, '?') ? "&" : "?", WEATHER_API_KEY);
  HTTPClient http;
  http.setReuse(true);            // 连接归还复用, 不每请求重建 TLS 上下文
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(*s_cli, WEATHER_API_HOST, 443, uri)) { Serial.printf("[Wx] begin FAIL %s\n", path); return false; }
  Serial.printf("[Wx] GET %s reuse=%d heap=%u largest=%u psram=%u\n", path, (int)reused,
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(), (unsigned)ESP.getFreePsram());
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

// sojson 15 天预报补 AQI (对齐 WinCalender: 和风 daily 仅 3 天, sojson 给 15 天)。
// 城市码表同 HTA cityMap (中国天气网城市码), 按坐标取最近城市。
struct SoCity { const char* name; const char* code; float lat, lon; };
static const SoCity SOCITIES[] = {
  {"北京","101010100",39.9042f,116.4074f}, {"上海","101020100",31.2304f,121.4737f},
  {"深圳","101280601",22.5431f,114.0579f}, {"广州","101280101",23.1291f,113.2644f},
  {"杭州","101210101",30.2741f,120.1551f}, {"成都","101270101",30.5728f,104.0668f},
  {"武汉","101200101",30.5928f,114.3055f}, {"南京","101190101",32.0603f,118.7969f},
  {"重庆","101040100",29.5630f,106.5516f}, {"天津","101030100",39.3434f,117.3616f},
  {"西安","101110101",34.3416f,108.9398f}, {"苏州","101190401",31.2990f,120.5853f},
  {"长沙","101250101",28.2282f,112.9388f}, {"沈阳","101070101",41.8057f,123.4315f},
  {"哈尔滨","101050101",45.8038f,126.5350f},{"郑州","101180101",34.7466f,113.6253f},
  {"青岛","101120201",36.0671f,120.3826f}, {"大连","101070201",38.9140f,121.6143f},
  {"福州","101230101",26.0745f,119.2965f}, {"厦门","101230201",24.4798f,118.0894f},
  {"昆明","101290101",25.0389f,102.7183f}, {"贵阳","101260101",26.6470f,106.6302f},
  {"南宁","101300101",22.8290f,108.9532f}, {"海口","101310101",20.0440f,110.1990f},
  {"兰州","101160101",36.0611f,103.8343f}, {"太原","101100101",37.8706f,112.5489f},
  {"石家庄","101090101",38.0428f,114.5149f},{"呼和浩特","101080101",40.8426f,111.7511f},
  {"乌鲁木齐","101130101",43.8256f,87.6168f},{"拉萨","101140101",29.6500f,91.1000f},
  {"银川","101170101",38.4872f,106.2309f},{"西宁","101150101",36.6171f,101.7782f},
  {"南昌","101240101",28.6820f,115.8579f},{"济南","101120101",36.6512f,117.1201f},
  {"合肥","101220101",31.8206f,117.2272f},{"长春","101060101",43.8171f,125.3235f},
};
static bool sojsonAqiFill(WData& d, float lat, float lon) {
  const SoCity* best = nullptr; float bestD = 1e9f;
  for (auto& c : SOCITIES) {
    float dy = c.lat - lat, dx = c.lon - lon, dd = dx * dx + dy * dy;
    if (dd < bestD) { bestD = dd; best = &c; }
  }
  if (!best) return false;
  char url[80];
  snprintf(url, sizeof(url), "http://t.weather.sojson.com/api/weather/city/%s", best->code);
  WiFiClient cli;
  HTTPClient http;
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
  JsonArray fc = doc["data"]["forecast"];       // 15 天, 每项含 ymd + aqi
  bool any = false;
  for (JsonObject f : fc) {
    const char* ymd = f["ymd"] | "";            // "2026-11-05"
    if (strlen(ymd) < 10) continue;
    char md[7]; snprintf(md, sizeof(md), "%d/%02d",
                         (ymd[5]-'0') * 10 + (ymd[6]-'0'), (ymd[8]-'0') * 10 + (ymd[9]-'0'));
    int aqi = numOrStr(f["aqi"]);
    if (aqi <= 0) continue;
    for (int k = 0; k < 7; k++) {
      if (strcmp(d.dayDate[k], md)) continue;
      if (d.dayAqi[k] == 0) { d.dayAqi[k] = aqi; any = true; }
      break;
    }
  }
  return any;
}

// 实时紫外线 Open-Meteo current.uv_index (约 15 分钟更新一次, 对齐 WinCalender fetchUvNow)。
// 和风只有当日指数, wttr 是 3 小时步长; Open-Meteo 实时值最细, 作为 UV 主源。
// 顺便取响应顶层 elevation (open-meteo 模型地形海拔, 只与坐标有关) 赋给 elevM,
// 避免单独为海拔发请求 (HTA 不显示海拔, 此字段为 ESP32 UI 自有)。
// 走 HTTP (非 HTTPS), 失败返回 false (调用方保留兜底)。
static bool openMeteoUvNow(float lat, float lon, float& uvOut, int& elevOut) {
  WiFiClient cli;
  HTTPClient http;
  char url[200];
  snprintf(url, sizeof(url),
           "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=uv_index&timezone=Asia%%2FShanghai", lat, lon);
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
  float v = doc["current"]["uv_index"] | -1.0f;
  elevOut = (int)((doc["elevation"] | 0.0f) + 0.5f);
  if (v < 0.0f) return false;
  uvOut = v;
  return true;
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
  // 设计稿风格: "西北偏西" 16 方位 (对齐 HTA DIR16_ZH / dir16Zh, 大小写不敏感)
  static const struct { const char* k; const char* v; } M[] = {
    {"N","北"},{"NNE","东北偏北"},{"NE","东北"},{"ENE","东北偏东"},{"E","东"},{"ESE","东南偏东"},
    {"SE","东南"},{"SSE","东南偏南"},{"S","南"},{"SSW","西南偏南"},{"SW","西南"},{"WSW","西南偏西"},
    {"W","西"},{"WNW","西北偏西"},{"NW","西北"},{"NNW","西北偏北"}
  };
  if (!d) return "";
  char up[8] = "";
  size_t n = strlen(d); if (n > sizeof(up) - 1) n = sizeof(up) - 1;
  for (size_t i = 0; i < n; i++)
    up[i] = (d[i] >= 'a' && d[i] <= 'z') ? (char)(d[i] - 32) : d[i];
  for (auto& e : M) if (!strcmp(up, e.k)) return e.v;
  return d;
}

// ========== 台风实况: 中国气象局台风网 (typhoon.nmc.cn, 免费/无 key/明文 HTTP) ==========
// 和风 /v7/tropical/* 属"台风和海洋"收费产品 (0.003 元/次), 已全部移除。
//   列表 /weatherservice/typhoon/jsons/list_default -> typhoonList[[id,en,zh,...,status]]
//   详情 /weatherservice/typhoon/jsons/view_<id>    -> 路径点数组 (明文约 45KB, JSONP 包装)
// 单点: [pntId,"YYYYMMDDHHMM",ts,"TYPE",lon,lat,press,wind,"DIR",speed,[["30KTS",NE,SE,SW,NW,id]..],{预报},[..]]
// 取最后一个点(最新实况)。整条路径用 64KB PSRAM 缓冲后按文本扫描, 不建大 JSON 树
// (收音机播放时内部堆最大连续块仅 ~23KB)。
#define NMC_HOST "typhoon.nmc.cn"

static bool nmcGetPlain(const char* url, uint8_t* out, size_t cap, size_t* outLen) {
  WiFiClient cli;
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.useHTTP10(true);
  if (!http.begin(cli, url)) { Serial.printf("[Wx] nmc begin FAIL %s\n", url); return false; }
  http.addHeader("Accept-Encoding", "identity");
  int code = http.GET();
  if (code != 200) { Serial.printf("[Wx] nmc HTTP %d %s\n", code, url); http.end(); return false; }
  size_t n = 0;
  int total = http.getSize();
  if (total > 0) {
    size_t want = (size_t)total;
    if (want > cap - 1) want = cap - 1;
    n = http.getStreamPtr()->readBytes(out, want);
  } else {
    auto* st = http.getStreamPtr();
    while (n < cap - 1) {
      int r = st->read(out + n, cap - 1 - n);
      if (r <= 0) { if (!st->available()) break; continue; }
      n += r;
    }
  }
  http.end();
  out[n] = 0;
  *outLen = n;
  return n > 1;
}

// 活跃台风 (status=="start") -> 全部 id + 中文名 (最多 max 个), 返回个数
static int nmcListStorms(int* ids, char names[][24], int max) {
  struct Guard { uint8_t* p; ~Guard() { if (p) free(p); } } g{ (uint8_t*)ps_malloc(8192) };
  if (!g.p) return 0;
  size_t n = 0;
  if (!nmcGetPlain("http://" NMC_HOST "/weatherservice/typhoon/jsons/list_default",
                   g.p, 8192, &n)) return 0;
  const char* js = strchr((const char*)g.p, '{');   // 去掉 JSONP 包装
  if (!js) return 0;
  JsonDocument doc;
  if (deserializeJson(doc, js)) { Serial.println("[Wx] nmc list json FAIL"); return 0; }
  int cnt = 0;
  for (JsonVariant v : doc["typhoonList"].as<JsonArray>()) {
    if (cnt >= max) break;
    JsonArray e = v.as<JsonArray>();
    if (e.isNull()) continue;
    if (strcmp(e[7] | "", "start") != 0) continue;   // 仅活动中的
    int id = e[0].as<int>();
    if (id <= 0) continue;
    const char* zh = e[2] | "";
    if (!zh[0]) zh = e[1] | "";
    ids[cnt] = id;
    snprintf(names[cnt], 24, "%s", zh);
    cnt++;
  }
  if (cnt > 1) Serial.printf("[Wx] nmc list active=%d (%s)\n", cnt, names[0]);
  return cnt;
}

// 最新实测点 -> 单个台风实况 (s.wind>0 表示成功); name 由调用方填
static bool nmcStormTrack(int id, WData::TyStorm& s) {
  struct Guard { uint8_t* p; ~Guard() { if (p) free(p); } } g{ (uint8_t*)ps_malloc(65536) };
  if (!g.p) return false;
  char url[140];
  snprintf(url, sizeof(url), "http://" NMC_HOST "/weatherservice/typhoon/jsons/view_%d", id);
  size_t n = 0;
  if (!nmcGetPlain(url, g.p, 65536, &n)) return false;
  // 最新实测点: 路径点与预报点都以 [<id>,"20xx 开头, 预报点(风力预报)多在后,
  // 故扫描全部候选并取"最后一个能按路径点格式解析成功"的 (预报点结构不同, 必然解析失败)。
  char typ[16] = "", dir[8] = "";
  float lon = 0, lat = 0; int press = 0, wind = 0, spd = 0;
  const char* last = nullptr;
  for (const char* q = (const char*)g.p; *q; q++) {
    if (*q != '[') continue;
    const char* c = q + 1;
    if (*c < '0' || *c > '9') continue;
    while (*c >= '0' && *c <= '9') c++;
    if (!(c[0] == ',' && c[1] == '"' && c[2] == '2' && c[3] == '0')) continue;
    char t[16] = "", dr[8] = "";
    float lo = 0, la = 0; int pr = 0, wi = 0, sp = 0;
    if (sscanf(q, "[%*d,\"%*[^\"]\",%*[^,],\"%15[^\"]\",%f,%f,%d,%d,\"%7[^\"]\",%d",
               t, &lo, &la, &pr, &wi, dr, &sp) == 7) {
      last = q;
      memcpy(typ, t, sizeof(typ)); safeStr(dir, sizeof(dir), dr);
      lon = lo; lat = la; press = pr; wind = wi; spd = sp;
    }
  }
  if (!last) { Serial.printf("[Wx] nmc no point id=%d (%u B)\n", id, (unsigned)n); return false; }
  s.lat = lat; s.lon = lon;
  s.wind = wind; s.press = press; s.moveSpeed = spd;
  safeStr(s.type, sizeof(s.type), tyStatus(typ));
  safeStr(s.moveDir, sizeof(s.moveDir), (dir[0] && strcmp(dir, "0")) ? tyDir(dir) : "");
  // 12 级 (64 节) 风圈: 弱台风没有该级半径 -> 清零, UI 自动隐藏该行
  memset(s.r64, 0, sizeof(s.r64));
  const char* r64 = strstr(last, "\"64KTS\"");
  if (r64) sscanf(r64, "\"64KTS\",%d,%d,%d,%d",
                  &s.r64[0], &s.r64[1], &s.r64[2], &s.r64[3]);
  return s.wind > 0;
}

// 城市弹窗手动选城覆盖 (setManualCity 设置; 不持久化, 重启恢复自动定位)
static bool  s_manual = false;
static char  s_mCity[16] = "", s_mDist[16] = "";
static float s_mLat = 0, s_mLon = 0;
// 最近一次 fetch 实际使用的坐标 (手动/自动; UI 算台风相对位置用)
static float s_usedLat = 0, s_usedLon = 0;

// 联网: 依 WinCalender 顺序拉取 实况/7天/空气/预警/台风, 填 g + 写缓存。
// 分链路 (对齐 WinCalender fetchWeather fast/full):
//   fast (每 10min, full=false): 实况 now + 预警 + 日出日落 + Open-Meteo 实时 UV
//   full (每 60min, full=true):  在 fast 基础上 + 7天预报 + 空气 + 和风 UV 日值 + 台风
// fast 链路失败 -> 整体失败 (保留缓存); full 链路失败只缺对应块 (fast 数据仍有效)。
// 返回: { fastOk(实况是否成功), fullOk(全量块是否成功) }
struct FetchResult { bool fastOk; bool fullOk; };
static FetchResult fetchAndCache(bool full) {
  FetchResult r{false, false};
  // 位置 (对齐老黄历.hta 三级定位):
  //   ① geo::hasCoord() = WiFi-BSSID(WLoc) 区县级坐标 -> 直接给和风, 精度远高于省会
  //   ② 无坐标时按定位省份取省会坐标兜底 (最后才用)
  //   城市名: geo::city() (ipip 中文 / ip-api 英文), 空则用省会名
  //   手动选城 (设计稿城市弹窗) 优先级最高, 覆盖自动定位
  const char* prov  = geo::pinned();
  const char* gcity = geo::city();
  const CityInfo* ci = nullptr;
  for (auto& c : CITIES) if (strcmp(c.prov, prov) == 0) { ci = &c; break; }
  if (!ci) ci = &CITIES[18];   // 广州 (index 18, 最终兜底)

  float lat, lon;
  if (s_manual) { lat = s_mLat; lon = s_mLon; }
  else {
    lat = geo::hasCoord() ? geo::lat() : ci->lat;
    lon = geo::hasCoord() ? geo::lon() : ci->lon;
  }
  s_usedLat = lat; s_usedLon = lon;

  // 区县级地名 (左上角显示): 和风 GeoAPI 按坐标反查; 坐标未变则复用上次结果
  char cityBuf[16] = "", distBuf[16] = "";
  if (s_manual) {
    snprintf(cityBuf, sizeof(cityBuf), "%s", s_mCity);
    snprintf(distBuf, sizeof(distBuf), "%s", s_mDist);
  } else if (s_qCity[0] && fabsf(lat - s_qLat) < 0.003f && fabsf(lon - s_qLon) < 0.003f) {
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
  if (!gz.p) { Serial.println("[Wx] no mem for gz buffer"); return r; }

  // 坐标变化时才请求 (每次定位/24h 刷新最多 1 次); 手动选城时跳过 (城市名已由弹窗给定)
  if (!s_manual && !cityBuf[0] && geo::hasCoord()) {
    if (qwRevGeo(lat, lon, gz.p, GZ_CAP, cityBuf, sizeof(cityBuf), distBuf, sizeof(distBuf))) {
      s_qLat = lat; s_qLon = lon;
      snprintf(s_qCity, sizeof(s_qCity), "%s", cityBuf);
      snprintf(s_qDist, sizeof(s_qDist), "%s", distBuf);
      Serial.printf("[Wx] revgeo %.4f,%.4f -> %s %s\n", lat, lon, cityBuf, distBuf);
    }
  }

  // fast 模式只刷实况/预警/UV, 7天/空气/台风等字段沿用上次全量结果 (从 g 拷贝)。
  WData d = g;
  static const char* WK[7] = {"周日","周一","周二","周三","周四","周五","周六"};
  char path[200];

  // ---- 1) 实况 /weather/v1/current/{lat}/{lng} (和风新平台 V1, 同 WinCalender) ----
  // V1 与 V7 差异: 温度/体感是 {value} 对象, 湿度 0~1, 风速 m/s, 能见度 m, 字段在顶层无 now 包装。
  snprintf(path, sizeof(path), "/weather/v1/current/%.4f/%.4f", lat, lon);
  {
    JsonDocument doc;
    if (!qwGetJson(path, doc, gz.p, GZ_CAP)) { Serial.println("[Wx] v1 current FAIL"); return r; }
    if (doc["temperature"].isNull()) { Serial.println("[Wx] v1 current: no temperature"); return r; }
    d.temp      = doc["temperature"]["value"] | 0.0f;
    d.feels     = doc["feelsLike"]["value"] | 0.0f;
    d.humidity  = (int)((doc["humidity"] | 0.0f) * 100.0f + 0.5f);             // V1: 0~1
    int ccode   = numOrStr(doc["condition"]["code"]);
    qwCondText(d.cond, sizeof(d.cond), ccode, doc["condition"]["text"] | "");
    d.icon      = qwIcon(ccode);
    snprintf(d.windDir, sizeof(d.windDir), "%s", compassCN(doc["wind"]["direction"]["compass"] | ""));
    d.windScale = numOrStr(doc["wind"]["scale"]);
    d.windSpeed = (int)((doc["wind"]["speed"]["value"] | 0.0f) * 3.6f + 0.5f);   // m/s -> km/h
    d.pressure  = doc["pressure"]["value"] | 0.0f;                             // hPa
    d.visKm     = (doc["visibility"]["value"] | 0.0f) / 1000.0f;               // m -> km

    // V1 无 obsTime -> 用本地时钟; 日期稍后由 daily[0] 覆盖 (NTP 未同步时不至于显示 1970)
    time_t tt = time(nullptr);
    struct tm lt; epochToLocalNoLock(tt, lt);
    snprintf(d.date,   sizeof(d.date),   "%02d-%02d", lt.tm_mon + 1, lt.tm_mday);
    snprintf(d.update, sizeof(d.update), "%02d:%02d", lt.tm_hour, lt.tm_min);
    r.fastOk = true;   // 实况成功 -> fast 链路有效
  }

  // ---- 2) 7 天预报 /weather/v1/daily/{lat}/{lng}?days=7 (仅 full) ----
  // V1 days[] -> daytime/nighttime 各自 condition/wind/humidity(0~1)/温度{value};
  // 顶层还有 temperatureMax/Min, uvIndexMax, astro{sunrise,sunset}。
  if (full) {
  // 重置台风更新时间戳: d=g 会拷贝上轮 tyTime, 若不清零, 后续刷新因 !d.tyTime[0] 为假
  // 而永远不更新 (看起来"一直没同步")。清零后由 storm-track 成功或 817 行兜底重新打时间。
  d.tyTime[0] = 0;
  snprintf(path, sizeof(path), "/weather/v1/daily/%.4f/%.4f?days=7", lat, lon);
  {
    JsonDocument doc;
    if (!qwGetJson(path, doc, gz.p, GZ_CAP)) { Serial.println("[Wx] v1 daily FAIL (kept cache)"); }
    else {
    JsonArray da = doc["days"];
    if (da.size() < 1) { Serial.println("[Wx] no days"); }
    else {
    for (int i = 0; i < 7 && i < (int)da.size(); i++) {
      JsonObject e = da[i];
      JsonObject day = e["daytime"], night = e["nighttime"];
      int dy = 0, dm = 0, dd2 = 0;
      bool hasDate = isoUtcToLocalYmd(e["forecastStartTime"] | "", &dy, &dm, &dd2);
      if (hasDate) snprintf(d.dayDate[i], 6, "%d/%02d", dm, dd2);
      else         snprintf(d.dayDate[i], 6, "--");
      if (i == 0)       snprintf(d.dayLabel[i], 8, "今天");
      else if (i == 1)  snprintf(d.dayLabel[i], 8, "明天");
      else if (hasDate) snprintf(d.dayLabel[i], 8, "%s", WK[weekdayFromYmd(dy, dm, dd2)]);
      else              snprintf(d.dayLabel[i], 8, "周?");
      int dcode = numOrStr(day["condition"]["code"]);
      qwCondText(d.dayCond[i], sizeof(d.dayCond[i]), dcode, day["condition"]["text"] | "");
      d.dayIcon[i] = qwIcon(dcode);
      d.dayHi[i]   = (int)((e["temperatureMax"]["value"] | 0.0f) + 0.5f);
      d.dayLo[i]   = (int)((e["temperatureMin"]["value"] | 0.0f) + 0.5f);
      // 湿度: 白天+夜间均值 (V1 daytime/nighttime.humidity 为 0~1 小数)
      float hd = day["humidity"] | -1.0f, hn = night["humidity"] | -1.0f;
      if (hd >= 0.0f && hn >= 0.0f) d.dayHum[i] = (int)((hd + hn) * 50.0f + 0.5f);
      else if (hd >= 0.0f)          d.dayHum[i] = (int)(hd * 100.0f + 0.5f);
      else if (hn >= 0.0f)          d.dayHum[i] = (int)(hn * 100.0f + 0.5f);
      else                          d.dayHum[i] = 0;
      if (i == 0 && hasDate)
        snprintf(d.date, sizeof(d.date), "%02d-%02d %s", dm, dd2, WK[weekdayFromYmd(dy, dm, dd2)]);
    }
    r.fullOk = true;   // 7 天成功 -> full 链路主体有效
    // 日期已重建 -> 清空逐日 AQI, 让下方 和风/sojson 按新日期重新填充。
    // 否则 d=g 带入的旧值使 sojson 的 ==0 补缺失效: AQI 与日期错位且永不更新。
    for (int i = 0; i < 7; i++) d.dayAqi[i] = 0;
    // 紫外线: V1 daily 顶层 uvIndexMax 即日值 (省去单独 /v7/indices/1d 调用)
    // 和风日值作为兜底; Open-Meteo 实时 UV 在末尾覆盖 (fast 链路也跑)。
    float uvMax = da[0]["uvIndexMax"] | -1.0f;
    if (uvMax >= 0.0f) {
      d.uv = uvMax;
      d.uvRealtime = false;
      Serial.printf("[Wx] uv(qw daily uvIndexMax) %.1f\n", (double)d.uv);
    }
    } }
  }
  } // end if (full)

  // ---- 2c) 日出/日落: NOAA 本地计算 (同 WinCalender, V1 daily 无该字段; 常跑) ----
  {
    time_t tt = time(nullptr);
    struct tm lt; epochToLocalNoLock(tt, lt);
    int y = lt.tm_year + 1900, mo = lt.tm_mon + 1, dd = lt.tm_mday;
    sunTime(y, mo, dd, lat, lon, true,  d.sunrise, sizeof(d.sunrise));
    sunTime(y, mo, dd, lat, lon, false, d.sunset,  sizeof(d.sunset));
  }

  // ---- 3) 空气实况 /airquality/v1/current/{lat}/{lng} (仅 full; 失败不影响主数据) ----
  if (full) {
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

  // ---- 4) 逐日 AQI: 和风 /airquality/v1/daily (免费版约 3 天) + sojson 补缺 (对齐 HTA) ----
  // HTA 策略: 今天用实时实测 AQI; 未来按日期匹配和风 daily (cn-mee);
  // 和风没有的日子用 sojson 15 天预报补齐; 不再用 open-meteo 折算 (HTA 无此调用)。
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
  // sojson 15 天 AQI 补缺 (对齐 HTA: 和风 daily 不够的天数用 sojson forecast 回填)
  if (sojsonAqiFill(d, lat, lon))
    Serial.println("[Wx] aqi: sojson 15天补齐");
  if (d.aqi > 0) d.dayAqi[0] = d.aqi;   // 今天优先实时实测 (同 HTA)
  Serial.printf("[Wx] aqi7 %s..%s = %d/%d/%d/%d/%d/%d/%d\n",
                d.dayDate[0], d.dayDate[6], d.dayAqi[0], d.dayAqi[1], d.dayAqi[2],
                d.dayAqi[3], d.dayAqi[4], d.dayAqi[5], d.dayAqi[6]);

  // ---- 4b) 7 天湿度: 和风 V1 daily 无湿度字段 (对齐 HTA, 不补; UI 显示 "--") ----
  // 海拔 elevM: 优先由步骤 7 的 Open-Meteo UV 响应顶层 elevation 给出 (同坐标固定);
  // 此处仅在 UV 失败时退回缓存 (d=g 已拷贝)。
  if (d.elevM <= 0) d.elevM = g.elevM;
  Serial.printf("[Wx] elev=%dm\n", d.elevM);
  } // end if (full)

  // ---- 5) 气象预警 /weatheralert/v1/current/{lat}/{lng} (常跑; fast 链路) ----
  // 台风(eventType 1001)单独进 typhoon 字段; 其余取第一条非台风预警入药丸。
  // 非台风预警字段每次重拉必须清零 (否则旧预警残留); 台风字段保留 (full 链路 storm-list 补实况)。
  bool tyAlert = false;   // 本轮预警是否含台风 (预警解除后为 false -> 用于清除残留台风标记)
  d.alertCount = 0;
  d.alertTitle[0] = 0; d.alertColor[0] = 0; d.alertDesc[0] = 0;
  d.alertTime[0] = 0; d.alertSender[0] = 0; d.alertSeverity[0] = 0;
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
          tyAlert = true;
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
          safeStr(d.alertSender, sizeof(d.alertSender), a["sender"] | "");
          // severity 英文 -> 中文 (和风: Minor/Moderate/Severe/Extreme)
          const char* sv = a["severity"] | "";
          safeStr(d.alertSeverity, sizeof(d.alertSeverity),
                  !strcmp(sv, "Minor") ? "一般" : !strcmp(sv, "Moderate") ? "较重" :
                  !strcmp(sv, "Severe") ? "严重" : !strcmp(sv, "Extreme") ? "特别严重" : "");
          int ay, am, ad, ah, ami;
          if (parseIsoLocal(a["issuedTime"] | "", &ay, &am, &ad, &ah, &ami))
            snprintf(d.alertTime, sizeof(d.alertTime), "%02d-%02d %02d:%02d", am, ad, ah, ami);
        }
      }
    }
  }

  // 本轮已无台风预警: 若手上也没有台风实况 (上轮 full 未抓到或已停编), 清掉残留的台风标记,
  // 否则台风消散后顶栏台风按钮/弹窗仍显示旧信息 (d=g 每轮拷贝旧状态, 之前只置位从不清除)。
  // fast 链路沿用 g 的 tyCount; full 链路在步骤 6 开头还会再重推一次。
  if (!tyAlert && d.tyCount <= 0) {
    d.typhoon = false;
    d.typhoonName[0] = 0;
    d.typhoonDesc[0] = 0;
  }

  // ---- 6) 活跃台风: 中国气象局台风网 (免费源, 仅 full 链路) ----
  // 列表里 status=="start" 的台风全部拉取实况 (最多 TY_MAX 个), 弹窗一次列出、超出可滚动。
  // 预警里点名的台风排最前 -> 弹窗首屏/顶部按钮显示的是本地预警的那个。
  if (full) {
  {
    // full 链路重推台风状态: 上轮 storm-list 写入的台风标记/名称/描述全部作废,
    // 仅保留步骤 5 本轮预警刚点名的台风; 随后按 NMC 活跃列表重建 (无活跃台风则保持清除)。
    if (!tyAlert) {
      d.typhoon = false;
      d.typhoonName[0] = 0;
      d.typhoonDesc[0] = 0;
    }
    int  ids[WData::TY_MAX];
    char names[WData::TY_MAX][24];
    int  n = nmcListStorms(ids, names, WData::TY_MAX);
    d.tyCount = 0;
    if (n > 0) {
      int order[WData::TY_MAX];
      int first = 0;
      for (int i = 0; i < n; i++) order[i] = i;
      if (d.typhoon && d.typhoonName[0]) {
        for (int i = 0; i < n; i++) {
          if (strstr(d.typhoonName, names[i])) { first = i; break; }
        }
      }
      if (first != 0) { int t = order[0]; order[0] = order[first]; order[first] = t; }
      if (!d.typhoon) {
        d.typhoon = true;
        snprintf(d.typhoonName, sizeof(d.typhoonName), "台风%s", names[order[0]]);
      }
      for (int i = 0; i < n; i++) {
        WData::TyStorm& s = d.ty[d.tyCount];
        snprintf(s.name, sizeof(s.name), "%s", names[order[i]]);
        if (nmcStormTrack(ids[order[i]], s)) {   // 强度/风速/气压/位置/移向/风圈
          d.tyCount++;
          Serial.printf("[Wx] nmc storm %d/%d id=%d name=%s type=%s %d m/s %d hPa move=%s %dkm/h\n",
                        d.tyCount, n, ids[order[i]], s.name, s.type, s.wind, s.press,
                        s.moveDir, s.moveSpeed);
        }
      }
      if (d.tyCount <= 0)
        snprintf(d.typhoonDesc, sizeof(d.typhoonDesc), "%s 正在西北太平洋活动", names[0]);
    }
  }

  // 台风数据本次刷新成功 -> 更新时间 = 本地抓取时刻 (预警里的台风没有实况时也要有)
  if (d.typhoon && !d.tyTime[0]) {
    time_t tt = time(nullptr);
    struct tm lt; epochToLocalNoLock(tt, lt);
    snprintf(d.tyTime, sizeof(d.tyTime), "%02d:%02d", lt.tm_hour, lt.tm_min);
  }
  } // end if (full)

  // ---- 7) 实时紫外线 Open-Meteo current.uv_index (常跑; fast 链路, 约 15 分钟级) ----
  // 对齐 WinCalender fetchUvNow: 实时值优先覆盖和风日值; 夜间 UI 显示"弱"。
  // 同响应取顶层 elevation 作为海拔 (HTA 不显示海拔, 仅 ESP32 UI 用; 不额外发请求)。
  {
    float uvNow = -1.0f;
    int elevNow = 0;
    if (openMeteoUvNow(lat, lon, uvNow, elevNow)) {
      d.uv = (float)((int)(uvNow + 0.5f));   // Math.round
      d.uvRealtime = true;
      if (elevNow > 0) d.elevM = elevNow;
      Serial.printf("[Wx] uv(om now) %.1f -> %.0f elev=%dm\n",
                    (double)uvNow, (double)d.uv, d.elevM);
    } else {
      Serial.println("[Wx] uv(om now) FAIL (kept qw daily/cache)");
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
      o["pressure"] = d.pressure; o["uv"] = d.uv; o["uvRealtime"] = d.uvRealtime; o["icon"] = d.icon;
      o["sunrise"] = d.sunrise; o["sunset"] = d.sunset;
      o["visKm"] = d.visKm; o["elevM"] = d.elevM; o["aqi"] = d.aqi; o["pm25"] = d.pm25; o["pm10"] = d.pm10;
      o["alertCount"] = d.alertCount; o["alertTitle"] = d.alertTitle;
      o["alertColor"] = d.alertColor; o["alertDesc"] = d.alertDesc; o["alertTime"] = d.alertTime;
      o["typhoon"] = d.typhoon; o["typhoonName"] = d.typhoonName; o["typhoonDesc"] = d.typhoonDesc;
      o["district"] = d.district;
      o["alertSender"] = d.alertSender; o["alertSeverity"] = d.alertSeverity;
      // 台风实况结构字段: 每台风一项 (弹窗一次列出, 超出窗口可滚动)
      o["tyCount"] = d.tyCount; o["tyTime"] = d.tyTime;
      {
        JsonArray ta = o["ty"].to<JsonArray>();
        for (int i = 0; i < d.tyCount; i++) {
          JsonObject e = ta.add<JsonObject>();
          e["n"] = d.ty[i].name;   e["t"] = d.ty[i].type;
          e["la"] = d.ty[i].lat;   e["lo"] = d.ty[i].lon;
          e["w"] = d.ty[i].wind;   e["p"] = d.ty[i].press;
          e["d"] = d.ty[i].moveDir; e["s"] = d.ty[i].moveSpeed;
          JsonArray r = e["r"].to<JsonArray>();
          for (int k = 0; k < 4; k++) r.add(d.ty[i].r64[k]);
        }
      }
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
      Serial.printf("[Wx] fetched %s: %dC %s %s %d级 aqi=%d alerts=%d ty=%d/%d",
                    d.city, (int)d.temp, d.cond, d.windDir, d.windScale, d.aqi,
                    d.alertCount, (int)d.typhoon, d.tyCount);
      for (int i = 0; i < d.tyCount; i++)
        Serial.printf(" [%s %s %dm/s %dhPa %s %dkm/h]", d.ty[i].name, d.ty[i].type,
                      d.ty[i].wind, d.ty[i].press, d.ty[i].moveDir, d.ty[i].moveSpeed);
      Serial.printf(" @%s (full=%d)\n", d.tyTime, (int)full);
    }
  }
  g_seq++;   // 数据刷新 -> 通知 UI 重绘
  return r;
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
  d.uvRealtime = doc["uvRealtime"] | false;
  safeStr(d.sunrise, 8, doc["sunrise"].as<const char*>());
  safeStr(d.sunset,  8, doc["sunset"].as<const char*>());
  d.visKm = doc["visKm"].as<float>(); d.elevM = doc["elevM"].as<int>();
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
  safeStr(d.alertSender,   sizeof(d.alertSender),   doc["alertSender"].as<const char*>());
  safeStr(d.alertSeverity, sizeof(d.alertSeverity), doc["alertSeverity"].as<const char*>());
  d.tyCount = doc["tyCount"] | 0;
  if (d.tyCount < 0) d.tyCount = 0;
  if (d.tyCount > WData::TY_MAX) d.tyCount = WData::TY_MAX;
  {
    JsonArray ta = doc["ty"];
    for (int i = 0; i < d.tyCount; i++) {
      JsonObject e = ta[i].as<JsonObject>();
      WData::TyStorm& s = d.ty[i];
      safeStr(s.name, sizeof(s.name), e["n"].as<const char*>());
      safeStr(s.type, sizeof(s.type), e["t"].as<const char*>());
      s.lat = e["la"].as<float>(); s.lon = e["lo"].as<float>();
      s.wind = e["w"].as<int>();   s.press = e["p"].as<int>();
      safeStr(s.moveDir, sizeof(s.moveDir), e["d"].as<const char*>());
      s.moveSpeed = e["s"].as<int>();
      for (int k = 0; k < 4; k++) s.r64[k] = e["r"][k].as<int>();
    }
  }
  safeStr(d.tyTime, sizeof(d.tyTime), doc["tyTime"].as<const char*>());
  // 恢复 GeoAPI 反查缓存 (坐标未变则开机不再请求)
  safeStr(s_qCity, sizeof(s_qCity), doc["qCity"].as<const char*>());
  safeStr(s_qDist, sizeof(s_qDist), doc["qDist"].as<const char*>());
  s_qLat = doc["qLat"].as<float>(); s_qLon = doc["qLon"].as<float>();
  JsonArray da = doc["day"];
  for (int i = 0; i < 7 && i < (int)da.size(); i++) {
    JsonObject e = da[i].as<JsonObject>();
    safeStr(d.dayLabel[i], 8, e["label"].as<const char*>());
    safeStr(d.dayDate[i],  6, e["date"].as<const char*>());
    safeStr(d.dayCond[i], sizeof(d.dayCond[i]), e["cond"].as<const char*>());
    d.dayIcon[i] = e["icon"].as<int>(); d.dayHi[i] = e["hi"].as<int>(); d.dayLo[i] = e["lo"].as<int>();
    d.dayHum[i] = e["hum"].as<int>(); d.dayAqi[i] = e["aqi"].as<int>();
  }
  d.ok = true;
  g = d;
  g_seq++;   // 缓存加载 -> 通知 UI 重绘 (开机即有数据)
  Serial.printf("[Wx] cached %s: %dC %s\n", d.city, (int)d.temp, d.cond);
  Serial.printf("[Wx] cached ty=%d/%d @%s\n", (int)d.typhoon, d.tyCount, d.tyTime);
  for (int i = 0; i < d.tyCount; i++)
    Serial.printf("[Wx] cached ty[%d] %s %s %dm/s %dhPa %s %dkm/h\n", i, d.ty[i].name,
                  d.ty[i].type, d.ty[i].wind, d.ty[i].press, d.ty[i].moveDir, d.ty[i].moveSpeed);
  Serial.printf("[Wx] cached aqi7 = %d/%d/%d/%d/%d/%d/%d\n", d.dayAqi[0], d.dayAqi[1], d.dayAqi[2],
                d.dayAqi[3], d.dayAqi[4], d.dayAqi[5], d.dayAqi[6]);
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

  // 失败重试: 上次失败后 30s 内不再尝试 (避免短时间内反复撞墙)
  if (!pendingInit && lastFailed && (uint32_t)(now - lastFetch) < FAIL_RETRY_MS) return;

  // 分链路 (对齐 WinCalender fetchWeather fast/full):
  //   fast(10min): 实况+预警+实时UV   full(60min): +7天+空气+台风+UV日值
  //   force (pendingInit): 两条都跑
  bool force = pendingInit;
  bool full = force || (uint32_t)(now - lastFullTs) >= FULL_REFRESH_MS;
  bool fast = force || (uint32_t)(now - lastFastTs) >= FAST_REFRESH_MS;
  if (!full && !fast) return;

  lastFetch = now;
  if (fast) lastFastTs = now;
  if (full) lastFullTs = now;
  pendingInit = false;

  Serial.printf("[Wx] refresh mode=%s heap=%u largest=%u psram=%u\n", full ? "full" : "fast",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(), (unsigned)ESP.getFreePsram());
  FetchResult r = fetchAndCache(full);
  lastFailed = !r.fastOk;

  // full 请求但全量块失败 -> 回退 lastFullTs, 下次 tick 尽早重试全量 (不等满 60min)
  if (full && !r.fullOk) lastFullTs = now - FULL_REFRESH_MS + FAIL_RETRY_MS;

  if (!r.fastOk) Serial.printf("[Wx] fetch failed, retry in %lus\n", (unsigned long)(FAIL_RETRY_MS / 1000UL));
}

void forceRefresh() {
  // 冷却 60s: 播放中全量刷新要 30s 左右, 期间界面不响应; 用户连点会排队触发多轮
  // 全量 (9 个 HTTPS 请求), 徒增内存压力。冷却期内忽略。
  static uint32_t s_lastForce = 0;
  uint32_t now = millis();
  if (s_lastForce && (uint32_t)(now - s_lastForce) < 60000UL) return;
  s_lastForce = now;
  pendingInit = true;   // UI 刷新按钮: 下一次 tick 立即拉取 (fast+full)
}

// 城市弹窗手动选城 (设计稿): city=nullptr/空串 恢复自动定位
void setManualCity(const char* city, const char* district, float lat, float lon) {
  if (!city || !city[0]) {
    s_manual = false;
  } else {
    s_manual = true;
    snprintf(s_mCity, sizeof(s_mCity), "%s", city);
    snprintf(s_mDist, sizeof(s_mDist), "%s", district ? district : "");
    s_mLat = lat; s_mLon = lon;
  }
  pendingInit = true;   // 下一次 tick 立即用新位置拉取
}

const WData& get() { return g; }

float locLat() { return s_usedLat ? s_usedLat : s_qLat; }
float locLon() { return s_usedLon ? s_usedLon : s_qLon; }

unsigned seq() { return g_seq; }

// 当前是否在日出~日落之间 (对齐 WinCalender isDaytime; 紫外线昼夜区分显示用)
// 计算失败 (日出日落为 "--:--") 默认按白天处理。
bool isDaytime() {
  auto hm = [](const char* s) -> int {
    if (!s || !s[0]) return -1;
    int h = 0, m = 0;
    if (sscanf(s, "%d:%d", &h, &m) != 2) return -1;
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
  };
  int sr = hm(g.sunrise), ss = hm(g.sunset);
  if (sr < 0 || ss < 0) return true;
  time_t tt = time(nullptr);
  struct tm lt; epochToLocalNoLock(tt, lt);
  int cur = lt.tm_hour * 60 + lt.tm_min;
  return cur >= sr && cur <= ss;
}

} // namespace weather
