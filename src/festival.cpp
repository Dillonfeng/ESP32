#include "ui_common.h"

JsonDocument festivalDoc;

void loadFestivalConfig() {
  festivalDoc.clear();
  File f = LittleFS.open("/festival.json", "r");
  if (!f) { Serial.println("[Fest] festival.json not found (no festival music)"); return; }
  if (f.size() > 8192) { Serial.printf("[Fest] festival.json too large: %u B\n", f.size()); f.close(); return; }
  String data = f.readString();
  f.close();
  DeserializationError err = deserializeJson(festivalDoc, data);
  if (err) { Serial.printf("[Fest] festival.json parse error: %s\n", err.c_str()); festivalDoc.clear(); return; }
  Serial.printf("[Fest] festival.json loaded (%u B)\n", (unsigned)data.length());
}

static String festivalUrl(const char* name) {
  JsonVariant v = festivalDoc["festival_music"][name];
  if (!v || !v.is<String>()) return String();
  return String(v.as<String>());
}

// 当天事件�? 优先节日, 其�?�节�?(仅节气当�?, 无则�?
static String todayEventName(int year, int mon, int mday) {
  int ld[31]; nl_month_days(year, mon, ld);
  int v = ld[mday - 1];
  int lm = v / 100, ldn = v % 100;
  bool isLeap = (lm > 100); if (isLeap) lm -= 100;
  int ly = nl_zodiac_year(year, mon, mday);   // 干支/生肖�?(按春节分�? �?drawDateRow �?�?
  char fest[16] = "";
  nl_festival(year, mon, mday, ly, lm, ldn, isLeap ? 1 : 0, fest, sizeof(fest));
  if (fest[0]) return String(fest);
  int jqCur = -1, jqCurD = 0;
  if (nl_jq_info(year, mon, mday, &jqCur, &jqCurD, nullptr, nullptr) && jqCurD == 0)
    return String(nl_jq_text[jqCur]);
  return String();
}

// 每天�?跑一�? 有音乐配�?且已联网且收音机�?在播 -> �?�? 无事件则停掉昨天的流
void checkFestivalMusic(int year, int mon, int mday) {
  static int lastY = -1, lastM = -1, lastD = -1;
  if (year == lastY && mon == lastM && mday == lastD) return;
  lastY = year; lastM = mon; lastD = mday;

  String ev = todayEventName(year, mon, mday);
  if (ev.length() == 0) {
    if (musicPlaying) {  // 今天没有事件, 停掉
      musicPlaying = false;   // BUILDV5-NETTASK: 先停 netTask 泵（避免与 loop 并发）
      audio.stopSong();
      if (!radioPlaying) digitalWrite(STATUS_LED_PIN, LOW);
    }
    return;
  }
  String url = festivalUrl(ev.c_str());
  if (url.length() == 0) return;      // �?配置该事件的音�??
  if (!wifiConnected) return;         // 离线不播 (显示不受影响)
  if (radioPlaying) return;           // 收音机优�?
  // BUILD14 失败重试: 开播失败(HTTPS TLS OOM 等) 15s 后再试, 最多 3 次, 跨日重置。
  // 旧版失败一次就永远不播 (元旦是 HTTPS, 常因内部 RAM 紧张 esp-aes 分配失败)。
  static unsigned long fmsAt = 0; static int fmsTries = 0; static int fmsKey = -100;
  int key = year * 10000 + mon * 100 + mday;
  if (key != fmsKey) { fmsKey = key; fmsTries = 0; fmsAt = 0; }
  if (fmsTries >= 3 || (fmsAt && millis() - fmsAt < 15000)) return;
  musicPlaying = false;   // BUILDV5-NETTASK: 先停 netTask 泵（避免与 loop 并发）
  audio.stopSong();
  Serial.printf("Festival music: %s -> %s (try %d)\n", ev.c_str(), url.c_str(), fmsTries + 1);
  bool fok = audio.connecttohost(url.c_str());
  if (!fok) {
    fmsTries++;
    Serial.printf("[Fest] play FAILED (try %d/3)\n", fmsTries);
    return;
  }
  fmsTries = 0; fmsAt = millis();
  musicPlaying = true;
  digitalWrite(STATUS_LED_PIN, HIGH);
}

// ============================================================
// 收藏
// ============================================================


// ============================================================
// SD card DIAG probe (on-board slot, CS=GPIO6, shares the display's
// FSPI SPI bus: MISO=13 / MOSI=11 / SCLK=12). Runs ONCE at boot,
// before initTFT() so the SPI bus is re-init'd cleanly by the TFT
// afterwards. Dumps the card's file tree to the serial log, then
// unmounts. If the card/CS is wrong it prints a clear FAIL line.
// ============================================================
static int  sdDepth = 0;
static int  sdCount = 0;
static void sdList(const char* path) {
  if (sdCount >= 200) { Serial.printf("[SD] ... (cap 200)\n"); return; }
  File dir = SD.open(path);
  if (!dir) { Serial.printf("[SD] open %s FAIL\n", path); return; }
  if (!dir.isDirectory()) { dir.close(); return; }
  File e;
  while ((e = dir.openNextFile())) {
    if (sdCount >= 200) { e.close(); break; }
    sdCount++;
    String name = e.name();
    String full = String(path) + "/" + name;
    if (e.isDirectory()) {
      Serial.printf("[SD] DIR   %s/\n", full.c_str());
      e.close();
      if (sdDepth < 2) { sdDepth++; sdList(full.c_str()); sdDepth--; }
    } else {
      Serial.printf("[SD] FILE  %-40s %u B\n", full.c_str(), (unsigned)e.size());
      e.close();
    }
  }
  dir.close();
}

void sdProbe() {
  Serial.println("==================== SD PROBE ====================");
  // The display uses the FSPI host (=VSPI=host1 on S3) on pins SCLK=12 / MISO=13 /
  // MOSI=11. The global `SPI` object is on that host too but with *default* pins, so
  // we build our own SPIClass bound to FSPI and explicitly set the display's pins.
  // begin(sclk,miso,mosi) also pre-inits the bus; SD.begin() re-calls spi.begin()
  // which is a no-op once _initted, so the pins we set here are what the SD uses.
  SPIClass sdSpi(FSPI);
  sdSpi.begin(12 /*SCLK*/, 13 /*MISO*/, 11 /*MOSI*/);
  bool ok = SD.begin(6 /*CS*/, sdSpi, 20000000 /*20MHz, safe*/, "/sdcard", 16, false);
  if (!ok) {
    Serial.println("[SD] FAIL: SD.begin() returned false (no card? wrong CS? CS=6)");
    Serial.println("  -> try another CS pin, or confirm the card is inserted & formatted FAT");
    Serial.println("===============================================");
    return;
  }
  Serial.printf("[SD] OK  cardSize=%llu B  total=%llu B  used=%llu B\n",
                (unsigned long long)SD.cardSize(),
                (unsigned long long)SD.totalBytes(),
                (unsigned long long)SD.usedBytes());
  sdList("/sdcard");
  SD.end();   // release the bus so initTFT() can re-init it cleanly
  Serial.println("===============================================");
}

// ============================================================
// setup / loop
// ============================================================
