// ============================================================
// geo_pin.cpp — 定位省份(收音机侧栏置顶): IP 反查 + LittleFS 缓存
// 实现说明见 include/geo_pin.h
// ============================================================
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "geo_pin.h"

namespace geo {

namespace {

#define REFRESH_MS    (24UL * 60UL * 60UL * 1000UL)   // 24h
#define HTTP_TIMEOUT_MS 8000

const char* kFile = "/geo_loc.json";

bool        fsReady     = false;   // LittleFS 已挂载
String      provName;             // 定位到的完整省名 (""=未定位)
unsigned long lastFetch = 0;       // 上次联网刷新时间 (0=尚未)
bool        pendingInit = true;    // 联网后需做首次定位

// ip-api 英文 regionName -> 中文省名 (与 mobile app.js EN_MAP 一致)
static bool enToCn(const char* e, char* out, size_t n) {
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

// 联网: GET ip-api.com (纯 HTTP), 解析 regionName -> 中文省名, 写缓存
bool fetchAndCache() {
  static WiFiClient cli;
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(cli, "http://ip-api.com/json/?fields=status,regionName")) {
    return false;
  }
  int code = http.GET();
  if (code != 200) {
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();
  if (body.length() == 0) return false;

  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  if (doc["status"] != "success") return false;

  const char* regionName = doc["regionName"];
  if (!regionName || !regionName[0]) return false;

  char cn[24];
  if (!enToCn(regionName, cn, sizeof(cn))) return false;   // 英文未命中 -> 保留旧缓存

  provName = cn;
  // 写缓存 (失败不影响本次定位)
  if (fsReady) {
    File f = LittleFS.open(kFile, "w");
    if (f) {
      JsonDocument o;
      o["province"] = cn;
      o["ts"] = millis();
      serializeJson(o, f);
      f.close();
      Serial.printf("[Geo] pinned province=%s (regionName=%s)\n", cn, regionName);
    }
  }
  return true;
}

} // anon

void init() {
  fsReady = LittleFS.begin();   // 幂等: setup 里已挂载则立即返回 true (与 holiday_sync 同款)
  if (fsReady) {
    File f = LittleFS.open(kFile, "r");
    if (f) {
      JsonDocument doc;
      if (!deserializeJson(doc, f)) {
        const char* p = doc["province"];
        if (p && p[0]) provName = p;
      }
      f.close();
      if (provName.length())
        Serial.printf("[Geo] cached province=%s\n", provName.c_str());
    }
  }
}

void tick() {
  if (!WiFi.isConnected()) return;          // 离线不做网络请求
  if (pendingInit) {
    pendingInit = false;
    lastFetch = millis();
    fetchAndCache();                        // 联网后首次定位 (失败保留缓存)
  } else if (millis() - lastFetch > REFRESH_MS) {
    lastFetch = millis();
    fetchAndCache();                        // 24h 定期刷新
  }
}

const char* pinned() {
  return provName.c_str();
}

} // namespace geo