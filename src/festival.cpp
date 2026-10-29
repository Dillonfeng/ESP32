#include "ui_common.h"
#include "sd_storage.h"   // 项目数据: SD 优先 / LittleFS 回退

JsonDocument festivalDoc;

void loadFestivalConfig() {
  festivalDoc.clear();
  File f = sdstore::openRead("/festival.json");
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
      if (!radioPlaying) STATUS_LED_OFF();
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
  STATUS_LED_ON();
}

// ============================================================
// 收藏
// ============================================================

// SD 卡诊断/挂载已由 sd_storage.cpp (sdstore::init) 常驻接管;
// 旧的 sdProbe() 一次性探测+卸载实现已移除 (挂载后立即 SD.end() 会让
// 后续项目文件无法落在卡上)。

// ============================================================
// setup / loop
// ============================================================
