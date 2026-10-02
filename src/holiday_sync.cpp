// ============================================================
// holiday_sync.cpp — 节假日(放假/补班) 在线同步 + LittleFS 缓存
// 实现说明见 include/holiday_sync.h
// ============================================================
#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "holiday_sync.h"

namespace holiday {

namespace {

#define SYNC_INTERVAL_MS (24UL * 60UL * 60UL * 1000UL)   // 24h
#define CACHE_LIMIT      4                                // 最多缓存年份数
#define HTTP_TIMEOUT_MS  8000

// 每一年: 紧凑 (month,day)->type 表 (1-based 索引, 0=无标注) + 名称表
// BUILD19 (2026-10-01): 名称改为**固定 .bss 池按值存储** (namePool)。
//   旧实现用 `String nameStr` 拼接, name[] 存 String 内部缓冲指针; String 扩容走
//   realloc 会释放旧缓冲 -> name[] 变悬垂指针。音频任务(Core0)随即复用这些已释放
//   块做 HTTP 缓冲(含 "Connection: keep-alive"), 显示任务(Core1) dayName() 读到悬垂
//   指针 -> 黄历节日框显示乱码/HTTP 头碎片 (实机: "国庆节国庆"/"keep alive" 等)。
//   固定池永不释放/重分配 -> 指针恒稳, 彻底消除跨核 use-after-free。
struct YearData {
  int  year;
  bool valid;
  uint8_t type[13][32];        // [1..12][1..31]
  const char* name[13][32];    // 指向 namePool, 无则 nullptr
  char   namePool[1024];       // 名称池 (按值存储, 永不重分配 -> 无悬垂指针)
  int    nameLen;              // 已写入字节数
};

YearData years[4];
int nYears = 0;

unsigned long lastSync = 0;        // 上次联网同步时间 (0=尚未)
bool pendingInit = true;           // 联网后需做首次同步
bool syncing = false;              // 正在同步 (防重入)
int syncQueue[3]; int nQueue = 0;  // 待同步年份 (浏览补同步)

// 当前日期 (由 init() 设定)
int curYear = 2000, curMonth = 1, curDay = 1;
bool fsReady = false;             // LittleFS 是否已挂载

int countDays(const YearData& Y) {
  int c = 0;
  for (int m = 1; m <= 12; m++) for (int d = 1; d <= 31; d++)
    if (Y.type[m][d]) c++;
  return c;
}

YearData* findYear(int y) {
  for (int i = 0; i < nYears; i++) if (years[i].year == y) return &years[i];
  return nullptr;
}

// 把解析后的 JSON 填进 YearData (type + 名称池)
// BUILD19: 名称按值 memcpy 进固定 namePool (见 struct 注释), 不再用 String 拼接,
//   避免 realloc 释放旧缓冲使 name[] 悬垂 (跨核 use-after-free 根因)。
void fillYear(YearData& Y, JsonDocument& doc) {
  memset(Y.type, 0, sizeof(Y.type));
  for (int i = 0; i < 13; i++) for (int j = 0; j < 32; j++) Y.name[i][j] = nullptr;
  Y.namePool[0] = '\0';
  Y.nameLen = 0;
  JsonVariant hv = doc["holiday"];
  if (!hv.is<JsonVariant>()) return;
  for (JsonPair p : hv.as<JsonObject>()) {
    const char* key = p.key().c_str();          // "MM-DD"
    int mm = atoi(key);
    int dd = atoi(key + 3);
    if (mm < 1 || mm > 12 || dd < 1 || dd > 31) continue;
    JsonObject o = p.value().as<JsonObject>();
    Y.type[mm][dd] = o["holiday"].as<bool>() ? HOL_OFF : HOL_WORK;
    const char* nm = o["name"].as<const char*>();
    if (nm && nm[0]) {
      int n = (int)strlen(nm);
      if (Y.nameLen + n + 1 <= (int)sizeof(Y.namePool)) {   // 池够大才写 (溢出则跳过该条, 不越界)
        memcpy(&Y.namePool[Y.nameLen], nm, n + 1);          // 连 '\0' 一起存 -> name[] 稳定指向
        Y.name[mm][dd] = &Y.namePool[Y.nameLen];
        Y.nameLen += n + 1;
      }
    }
  }
}

void loadAllYears() {
  nYears = 0;
  for (int y = curYear - 1; y <= curYear + 1; y++) {
    char path[24];
    snprintf(path, sizeof(path), "/holidays_%d.json", y);
    File f = LittleFS.open(path, "r");
    if (!f) continue;
    if (f.size() > 16000) { f.close(); continue; }   // 异常大文件忽略
    String data = f.readString();
    f.close();

    YearData& Y = years[nYears];
    memset(&Y, 0, sizeof(Y));
    Y.year = y;
    JsonDocument doc;
    if (deserializeJson(doc, data)) continue;         // 解析失败 -> 跳过
    fillYear(Y, doc);
    Y.valid = true;
    nYears++;
    Serial.printf("[Hol] cached %d (%d days)\n", y, countDays(Y));
  }
}

bool fetchAndCache(int year) {
  // 本设备 (ESP32-S3) 空闲内部 RAM 只剩 ~52KB, 完全无法做 TLS (esp-sha/esp-aes 都 OOM);
  // 而 timor.tech 的纯 HTTP 会 301 跳转到 https, HTTPClient 不跟跨协议 301 ->
  // 在线同步在此设备上**永远失败**。改为只读 LittleFS 里预置的缓存
  // (data/holidays_<year>.json, 由 PC 端 preview/sync_holidays.py 拉好后随 firmware 烧进),
  // 不再发起任何网络请求 (省 RAM + 消除 boot 期 esp-sha OOM)。
  // 拉取失败一律 return false, loadAllYears()/内存表保留已有缓存不清空。
  (void)year;
  return false;
}

void doPendingSyncs() {
  if (syncing || !fsReady) return;
  if (nQueue == 0) return;
  syncing = true;
  for (int i = 0; i < nQueue; i++) fetchAndCache(syncQueue[i]);
  nQueue = 0;
  syncing = false;
  lastSync = millis();
}

} // anon

void init() {
  fsReady = LittleFS.begin();   // 幂等: setup 里已挂载则立即返回 true
  struct tm t;
  time_t now = time(nullptr);
  if (now > 1000000000) {
    localtime_r(&now, &t);
    curYear = t.tm_year + 1900; curMonth = t.tm_mon + 1; curDay = t.tm_mday;
  }
  if (fsReady && curYear >= 2024) loadAllYears();   // NTP 未就绪时推迟到 tick() (用正确年份)
  pendingInit = true;
  lastSync = 0;
}

void tick() {
  if (!WiFi.isConnected() || !fsReady) return;

  // NTP 未就绪时 curYear 仍是初值 2000: 等时间有效后再加载缓存并同步,
  // 否则首次同步会去拉 2000/2001 (timor 无数据, HTTP 301)
  if (curYear < 2024) {
    struct tm t;
    time_t now = time(nullptr);
    if (now <= 1000000000) return;
    localtime_r(&now, &t);
    curYear = t.tm_year + 1900; curMonth = t.tm_mon + 1; curDay = t.tm_mday;
    loadAllYears();
  }

  // 联网后首次: 同步 当年 + 次年
  if (pendingInit) {
    pendingInit = false;
    syncQueue[0] = curYear;
    syncQueue[1] = curYear + 1;
    nQueue = 2;
  } else if (lastSync && millis() - lastSync >= SYNC_INTERVAL_MS) {
    syncQueue[0] = curYear;
    syncQueue[1] = curYear + 1;
    nQueue = 2;
  }
  if (nQueue) doPendingSyncs();
}

void ensureYear(int y) {
  if (y < 2020 || y > 2099) return;                     // timor 只有近年数据, 越界年份不拉取
  if (findYear(y)) return;                              // 已有缓存
  for (int i = 0; i < nQueue; i++) if (syncQueue[i] == y) return;  // 已排队
  if (nQueue < 3) syncQueue[nQueue++] = y;
  if (WiFi.isConnected()) doPendingSyncs();             // 在线立即做
}

uint8_t dayType(int y, int m, int d) {
  if (m < 1 || m > 12 || d < 1 || d > 31) return HOL_NONE;
  YearData* Y = findYear(y);
  if (!Y || !Y->valid) return HOL_NONE;
  return Y->type[m][d];
}

const char* dayName(int y, int m, int d) {
  if (m < 1 || m > 12 || d < 1 || d > 31) return "";
  YearData* Y = findYear(y);
  if (!Y || !Y->valid) return "";
  const char* n = Y->name[m][d];
  return n ? n : "";
}

} // namespace holiday
