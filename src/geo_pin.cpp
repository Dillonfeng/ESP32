// ============================================================
// geo_pin.cpp — 定位坐标/省份/城市: WiFi-BSSID 定位 + IP 反查 + 缓存
// 实现说明见 include/geo_pin.h
// 优先级对齐 D:\WinCalender\老黄历.hta 的三级定位:
//   ① WiFi/BSSID 定位 (移植 hl_geoloc.ps1 的 Apple gs-loc WLoc 请求/解析)
//      -> 区县级坐标 (实测本机 22.6304,114.1160 = 龙岗布吉, 精度 ~36m)
//   ② myip.ipip.net 城市级 IP 库 -> 省市名 (国内较准)
//   ③ ip-api.com -> 省市名 (常把深圳误判成广州, 仅最终降级)
// 设备无 GPS/Windows 定位服务, 故①用 WiFi 扫描等效复刻 (同一 gs-loc 服务);
// 名称(省/市)仍由②③提供, 因为 WLoc 只返回坐标。
// ============================================================
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "geo_pin.h"
#include "sd_storage.h"   // 定位缓存: SD 优先 / LittleFS 回退

namespace geo {

namespace {

#define REFRESH_MS    (24UL * 60UL * 60UL * 1000UL)   // 24h
#define HTTP_TIMEOUT_MS 8000
#define WLOC_TIMEOUT_MS 10000

const char* kFile = "/geo_loc.json";

bool        fsReady     = false;   // LittleFS 已挂载
String      provName;              // 定位到的完整省名 (""=未定位)
String      cityName;              // 定位到的城市名 (ipip 中文; ip-api 英文) (""=未知)
bool        coordOk     = false;   // 是否拿到区县级坐标 (WLoc)
float       coordLat    = 0.0f;
float       coordLon    = 0.0f;
int         coordAcc    = 0;       // 精度 (米, WLoc 返回)
unsigned long lastFetch = 0;       // 上次联网刷新时间 (0=尚未)
bool        pendingInit = true;    // 联网后需做首次定位

// ip-api 英文 regionName -> 中文省名 (与 mobile app.js EN_MAP 一致)
static const struct { const char* en; const char* cn; } MAP[] = {
  {"Beijing","北京"},{"Shanghai","上海"},{"Tianjin","天津"},{"Chongqing","重庆"},
  {"Hebei","河北"},{"Shanxi","山西"},{"Liaoning","辽宁"},{"Jilin","吉林"},{"Heilongjiang","黑龙江"},
  {"Jiangsu","江苏"},{"Zhejiang","浙江"},{"Anhui","安徽"},{"Fujian","福建"},{"Jiangxi","江西"},{"Shandong","山东"},
  {"Henan","河南"},{"Hubei","湖北"},{"Hunan","湖南"},{"Guangdong","广东"},{"Guangxi","广西"},{"Hainan","海南"},
  {"Sichuan","四川"},{"Guizhou","贵州"},{"Yunnan","云南"},{"Tibet","西藏"},
  {"Shaanxi","陕西"},{"Gansu","甘肃"},{"Qinghai","青海"},{"Ningxia","宁夏"},{"Xinjiang","新疆"},
  {"Inner Mongolia","内蒙古"},{"Nei Mongol","内蒙古"},
  {"Hong Kong","香港"},{"Macau","澳门"},{"Macao","澳门"},{"Taiwan","台湾"}
};

static bool enToCn(const char* e, char* out, size_t n) {
  if (!e) return false;
  // 大小写不敏感逐条匹配 (含 "Inner Mongolia"/"Nei Mongol" 空格变体)
  for (size_t i = 0; i < sizeof(MAP)/sizeof(MAP[0]); i++) {
    if (strcasecmp(e, MAP[i].en) == 0) {
      size_t len = strlen(MAP[i].cn);
      if (len < n) { memcpy(out, MAP[i].cn, len + 1); return true; }
    }
  }
  return false;
}

// ipip 直接给中文省名 -> 校验是本项目认识的大陆/港澳台省份
static bool cnProvinceOk(const char* cn) {
  if (!cn) return false;
  for (size_t i = 0; i < sizeof(MAP)/sizeof(MAP[0]); i++) {
    if (strcmp(cn, MAP[i].cn) == 0) return true;
  }
  return false;
}

// 去掉城市名尾部"市" (3 字节), 如 深圳市 -> 深圳
static void stripCitySuffix(String& s) {
  if (s.length() > 3 && s.endsWith("市")) s.remove(s.length() - 3);
}

// ---------------- protobuf 小工具 (对应 ps1 的 RdVar) ----------------
static bool rdVar(const uint8_t* b, size_t n, size_t& p, uint64_t& v) {
  v = 0; int sh = 0;
  while (p < n) {
    uint8_t x = b[p++];
    v |= (uint64_t)(x & 0x7F) << sh;
    if (!(x & 0x80)) return true;
    sh += 7;
    if (sh > 63) return true;
  }
  return false;
}

// ---------------- ① WLoc: 扫周边 BSSID -> Apple gs-loc 定位 ----------------
// 请求体格式逐字节移植自 D:\WinCalender\hl_geoloc.ps1 的 TryWloc。
static bool fetchWloc() {
  // 连接中的 AP 优先, 再补周边 AP, 最多 6 个 (与 ps1 一致, 请求体 <= 250B)
  String cands[6]; int nc = 0;
  String b0;
  if (WiFi.isConnected()) {
    b0 = WiFi.BSSIDstr();      // 当前 AP 的 BSSID
    if (b0.length() == 17) { b0.toLowerCase(); cands[nc++] = b0; }
  }
  int found = WiFi.scanNetworks(false /*async*/, true /*hidden*/);
  for (int i = 0; i < found && nc < 6; i++) {
    String m = WiFi.BSSIDstr(i);
    if (m.length() != 17) continue;
    m.toLowerCase();
    bool dup = false;
    for (int k = 0; k < nc; k++) if (cands[k] == m) { dup = true; break; }
    if (!dup) cands[nc++] = m;
  }
  WiFi.scanDelete();
  if (nc == 0) return false;

  // 内部载荷: 每个 AP -> 0x12,len,0x0A,0x11,17 字节 mac; 结尾 0x18,0x00,0x20,0x01
  uint8_t pl[200]; size_t pn = 0;
  for (int i = 0; i < nc; i++) {
    const char* m = cands[i].c_str();
    if (strlen(m) != 17) continue;
    pl[pn++] = 0x12; pl[pn++] = 2 + 17; pl[pn++] = 0x0A; pl[pn++] = 17;
    memcpy(pl + pn, m, 17); pn += 17;
  }
  pl[pn++] = 0x18; pl[pn++] = 0x00; pl[pn++] = 0x20; pl[pn++] = 0x01;
  if (pn > 250) return false;

  // 外层包头 (语言/客户端标识/版本) + 载荷长度 + 载荷
  static const uint8_t kHdr[] = {
    0x00, 0x01, 0x00, 0x05, 'e','n','_','U','S',
    0x00, 0x13, 'c','o','m','.','a','p','p','l','e','.','l','o','c','a','t','i','o','n','d',
    0x00, 0x0A, '8','.','1','.','1','2','B','4','1','1',
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
  };
  uint8_t body[300]; size_t bn = 0;
  memcpy(body, kHdr, sizeof(kHdr)); bn = sizeof(kHdr);
  body[bn++] = (uint8_t)pn;
  memcpy(body + bn, pl, pn); bn += pn;

  WiFiClientSecure cli;
  cli.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(WLOC_TIMEOUT_MS);
  http.setTimeout(WLOC_TIMEOUT_MS);
  if (!http.begin(cli, "https://gs-loc.apple.com/clls/wloc")) return false;
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.addHeader("User-Agent", "locationd/1753.17 CFNetwork/889.9 Darwin/17.2.0");
  int code = http.POST(body, bn);
  if (code != 200) { Serial.printf("[Geo] wloc HTTP %d\n", code); http.end(); return false; }
  String resp = http.getString();
  http.end();
  size_t n = resp.length();
  if (n < 14) return false;

  // 解析: 顶层字段2 = 每个 AP 的记录; 记录内字段1=mac, 字段2={1:lat,2:lon,3:acc} (1e-8 度)
  const uint8_t* b = (const uint8_t*)resp.c_str();
  double fLat = 0, fLon = 0, bLat = 0, bLon = 0;
  bool hasFirst = false, hasBest = false; int bAcc = 0;
  size_t p = 10;
  while (p < n) {
    uint8_t tag = b[p++]; int fld = tag >> 3, wir = tag & 7;
    if (wir == 2) {
      uint64_t l = 0;
      if (!rdVar(b, n, p, l)) break;
      size_t end = p + (size_t)l;
      if (end > n) break;
      if (fld == 2) {
        size_t q = p;
        String db; int64_t la = 0, lo = 0; bool hla = false, hlo = false; int ac = 0;
        while (q < end) {
          uint8_t t2 = b[q++]; int f2 = t2 >> 3, w2 = t2 & 7;
          if (w2 == 2) {
            uint64_t l2 = 0;
            if (!rdVar(b, n, q, l2)) break;
            size_t e2 = q + (size_t)l2;
            if (e2 > n) break;
            if (f2 == 1) {                       // BSSID 字符串
              db = "";
              for (size_t i = q; i < e2; i++) db += (char)b[i];
              db.toLowerCase();
            } else if (f2 == 2) {                // 坐标子消息
              size_t rr = q;
              while (rr < e2) {
                uint8_t t3 = b[rr++]; int f3 = t3 >> 3, w3 = t3 & 7;
                if (w3 == 0) {
                  uint64_t vv = 0;
                  if (!rdVar(b, n, rr, vv)) break;
                  if (f3 == 1)      { la = (int64_t)vv; hla = true; }
                  else if (f3 == 2) { lo = (int64_t)vv; hlo = true; }
                  else if (f3 == 3) { ac = (int)vv; }
                } else if (w3 == 5) rr += 4; else rr += 8;
              }
            }
            q = e2;
          } else if (w2 == 0) { uint64_t vv; if (!rdVar(b, n, q, vv)) break; }
          else if (w2 == 5) q += 4; else q += 8;
        }
        if (hla && hlo) {
          double a = (double)la / 1e8, o = (double)lo / 1e8;
          if (!(a == -180.0 && o == -180.0)) {
            if (!hasFirst) { fLat = a; fLon = o; hasFirst = true; }
            if (b0.length() && db == b0) { bLat = a; bLon = o; bAcc = ac; hasBest = true; }
          }
        }
      }
      p = end;
    } else if (wir == 0) { uint64_t vv; if (!rdVar(b, n, p, vv)) break; }
    else if (wir == 5) p += 4; else p += 8;
  }

  if (hasBest)      { coordLat = (float)bLat; coordLon = (float)bLon; coordAcc = bAcc; }
  else if (hasFirst){ coordLat = (float)fLat; coordLon = (float)fLon; coordAcc = 0; }
  else return false;
  coordOk = true;
  Serial.printf("[Geo] wloc %.5f,%.5f acc=%dm (%d APs)\n", coordLat, coordLon, coordAcc, nc);
  return true;
}

// ---------------- ② ipip.net 城市级 ----------------
static bool fetchIpip() {
  static WiFiClient cli;
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(cli, "http://myip.ipip.net/json")) return false;
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  String body = http.getString();
  http.end();
  if (body.length() == 0) return false;

  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  const char* ret = doc["ret"] | "";
  if (strcmp(ret, "ok") != 0) return false;
  JsonArray loc = doc["data"]["location"];
  if (loc.size() < 2) return false;
  const char* cn = loc[1] | "";   // 省
  const char* ct = loc[2] | "";   // 市 (可能为空)
  if (!cnProvinceOk(cn)) return false;

  provName = cn;
  cityName = ct;
  stripCitySuffix(cityName);
  Serial.printf("[Geo] ipip province=%s city=%s\n", provName.c_str(), cityName.c_str());
  return true;
}

// ---------------- ③ ip-api.com 兜底 ----------------
static bool fetchIpApi() {
  static WiFiClient cli;
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(cli, "http://ip-api.com/json/?fields=status,regionName,city")) return false;
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  String body = http.getString();
  http.end();
  if (body.length() == 0) return false;

  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  if (strcmp(doc["status"] | "", "success") != 0) return false;

  const char* regionName = doc["regionName"] | "";
  if (!regionName[0]) return false;

  char cn[24];
  if (!enToCn(regionName, cn, sizeof(cn))) return false;   // 英文未命中 -> 保留旧缓存

  provName = cn;
  const char* ct = doc["city"] | "";                       // 英文城市名
  if (ct[0]) cityName = ct;
  Serial.printf("[Geo] ip-api province=%s city=%s\n", provName.c_str(), cityName.c_str());
  return true;
}

static void saveCache() {
  if (!fsReady) return;
  File f = sdstore::openWrite(kFile);
  if (!f) return;
  JsonDocument o;
  o["province"] = provName;
  o["city"]     = cityName;
  o["lat"]      = coordLat;
  o["lon"]      = coordLon;
  o["acc"]      = coordAcc;
  o["hasCoord"] = coordOk;
  serializeJson(o, f);
  f.close();
}

// 联网: ①区县坐标 -> ②省市名 (ipip) -> ③ip-api; 全部失败则保留缓存
void fetchAll() {
  bool gotCoord = fetchWloc();          // 主路径: 区县坐标
  bool gotName  = fetchIpip() || fetchIpApi();   // 名称: 省市 (radio 侧栏也要用)
  if (gotCoord || gotName) saveCache();
  if (!gotCoord) Serial.println("[Geo] wloc failed -> 使用 IP 省市名兜底坐标");
}

} // anon

void init() {
  bool lfs = LittleFS.begin();   // 幂等: setup 里已挂载则立即返回 true (与 holiday_sync 同款)
  fsReady = lfs || sdstore::mounted();
  if (fsReady) {
    File f = sdstore::openRead(kFile);
    if (f) {
      JsonDocument doc;
      if (!deserializeJson(doc, f)) {
        const char* p = doc["province"] | "";
        const char* c = doc["city"] | "";
        if (p[0]) provName = p;
        if (c[0]) cityName = c;
        coordOk  = doc["hasCoord"] | false;
        coordLat = doc["lat"] | 0.0f;
        coordLon = doc["lon"] | 0.0f;
        coordAcc = doc["acc"] | 0;
      }
      f.close();
      if (provName.length())
        Serial.printf("[Geo] cached province=%s city=%s coord=%d %.5f,%.5f\n", provName.c_str(),
                      cityName.c_str(), (int)coordOk, coordLat, coordLon);
    }
  }
}

void tick() {
  if (!WiFi.isConnected()) return;          // 离线不做网络请求
  if (pendingInit) {
    pendingInit = false;
    lastFetch = millis();
    fetchAll();                             // 联网后首次定位 (失败保留缓存)
  } else if (millis() - lastFetch > REFRESH_MS) {
    lastFetch = millis();
    fetchAll();                             // 24h 定期刷新
  }
}

const char* pinned() {
  return provName.c_str();
}

const char* city() {
  return cityName.c_str();
}

bool  hasCoord()  { return coordOk; }
float lat()       { return coordLat; }
float lon()       { return coordLon; }

} // namespace geo
