#include "ui_common.h"
#include "sd_storage.h"   // 项目数据: SD 优先 / LittleFS 回退

String readURLForSid(int sid) {
  RadioStation* s = stn(sid); if (!s) return "";
  // 2026-11-01 MEM: 用户台 URL 不再常驻 RAM (原 extraUrl[24][260]=6.2KB)。
  // 约定: !in_bin 时 url_offset = /radio_meta.bin 里 URL 长度字段的绝对偏移; 0 = 无 URL。
  if (!s->in_bin && s->url_offset == 0) return "";
  File f = sdstore::openRead(s->in_bin ? "/stations.bin" : "/radio_meta.bin"); if (!f) return "";
  f.seek(s->url_offset);
  if (s->in_bin) { uint8_t nl = f.read(); f.seek(f.position() + nl); }   // 跳过 name 长度 + name
  uint16_t ul = f.read() | (f.read() << 8);
  uint8_t ub[257]; f.read(ub, min(ul, (uint16_t)256)); ub[min(ul, (uint16_t)256)] = 0;
  f.close(); return String((char*)ub);
}
bool loadRadioDB() {
  File f = sdstore::openRead("/stations.bin");
  if (!f) { Serial.println(F("stations.bin not found!")); return false; }
  uint8_t magic[9]; f.read(magic, 8); magic[8] = 0;
  if (strcmp((char*)magic, "ESP32RDB") != 0 || f.read() != 2) { Serial.println(F("bad format")); f.close(); return false; }
  uint16_t numRegions = f.read() | (f.read() << 8);
  uint32_t numStations = f.read() | (f.read() << 8) | (f.read() << 16) | (f.read() << 24);
  uint32_t regionIdxOffset = f.read() | (f.read() << 8) | (f.read() << 16) | (f.read() << 24);
  uint32_t stationRecordsOffset = f.read() | (f.read() << 8) | (f.read() << 16) | (f.read() << 24);
  f.read();
  f.seek(regionIdxOffset);
  regionEntries = new RegionEntry[numRegions];
  for (uint16_t i = 0; i < numRegions; i++) {
    f.read((uint8_t*)regionEntries[i].name, 16); regionEntries[i].name[16] = 0;
    regionEntries[i].first_offset = f.read() | (f.read() << 8) | (f.read() << 16) | (f.read() << 24);
    regionEntries[i].count = f.read() | (f.read() << 8);
    f.seek(f.position() + 6);
  }
  regionCount = numRegions;
  Serial.printf("stations.bin: %d regions, %lu stations, size=%u B, 1st region '%s' off=%u cnt=%u\n",
                numRegions, numStations, (unsigned)f.size(),
                regionEntries[0].name, (unsigned)regionEntries[0].first_offset,
                (unsigned)regionEntries[0].count);
  f.close(); return true;
}
// ---- 定位省份(完整名) -> regionCat 键: geo::pinned() 非空则用之, 否则退回 RPINNED(兜底"广东")。
// 注意: 键必须与 stations.bin 的完整省名一致(黑龙江/内蒙古), 显示 2 字缩写由 labelOf 负责。----
const char* radioPinnedKey() {
  const char* p = geo::pinned();
  return (p && p[0]) ? p : RPINNED;
}
// 2026-10-24 问题①: 设计稿"我的"组含"助眠"分类 (6 台演示: 雨声/海浪/林间鸟鸣/篝火/咖啡馆/白噪音)。
// 无真实音源 URL (readURLForSid 对 !in_bin 且 url_offset==0 的台直接返回空 -> 点击不播放, 安全)。
// 待用户提供音源后, 把 URL 写进 /radio_meta.bin 对应记录并让 url_offset 指向它即可。
static const char* SLEEP_NAMES[6] = { "雨声", "海浪", "林间鸟鸣", "篝火", "咖啡馆", "白噪音" };
void addSleepStations() {
  for (int i = 0; i < 6 && radioStnTotal < RMAX_STN; i++) {
    RadioStation& s = radioStn[radioStnTotal];
    truncCopy(s.name, sizeof(s.name), (const uint8_t*)SLEEP_NAMES[i], (int)strlen(SLEEP_NAMES[i]));
    truncCopy(s.freq, sizeof(s.freq), (const uint8_t*)"--", 2);
    truncCopy(s.region, sizeof(s.region), (const uint8_t*)"助眠", (int)strlen("助眠"));
    truncCopy(s.cat, sizeof(s.cat), (const uint8_t*)"助眠", (int)strlen("助眠"));
    s.r = 0x78; s.g = 0xA0; s.b = 0xC8;
    s.is_user = true; s.in_bin = false;
    s.url_offset = 0;   // 无 URL (readURLForSid 直接返回空)
    radioStnTotal++;
  }
}

// 重建收音机分类。2026-10-22 对齐设计稿左列三分组:
//   组0 频道: 全国·中央·国际·海外
//   组1 省市: 置顶省 -> 省市(RORDER) -> 其它
//   组2 我的: 收藏 -> 个人 -> 伴音(电视伴音) -> 历史
// 联网定位到省份后可再调一次重建, 让置顶省(完整名)正确出现在列表中 (与 preview 一致)。
void buildRadioCats() {
  // 释放旧 sids (避免重复构建时 PSRAM 泄漏)
  for (int i = 0; i < radioCatCount; i++) { if (radioCats[i].sids) heap_caps_free(radioCats[i].sids); }
  radioCatCount = 0;
  auto addCat = [&](const char* label, const char* key, bool meta, const uint16_t* seq, int n, int grp) {
    if (radioCatCount >= RMAX_CATS) return;
    RadioCat& c = radioCats[radioCatCount];
    truncCopy(c.label, sizeof(c.label), (const uint8_t*)label, (int)strlen(label));
    truncCopy(c.key, sizeof(c.key), (const uint8_t*)key, (int)strlen(key));
    c.meta = meta; c.count = 0; c.group = grp; c.sids = nullptr;
    if (n > 0 && seq) {
      // 2026-10-30 去重: stations.bin/backup json 存在重复记录(如 CNR-2经济之声 sid266/277 同 name+url,
      // 安徽交通广播 sid841/845), radio_meta all[] 同一 sid 出现两次 -> 列表重复行 + 播放时双高亮.
      // 每分类同一 sid 只保留一次; 不动数据文件/sid 编号, 收藏历史不受影响.
      uint16_t* tmp = (uint16_t*)ps_malloc((size_t)n * 2);
      if (tmp) {
        int m = 0;
        for (int i = 0; i < n; i++) {
          bool dup = false;
          for (int j = 0; j < m; j++) if (tmp[j] == seq[i]) { dup = true; break; }
          if (!dup) tmp[m++] = seq[i];
        }
        c.count = m;
        if (m > 0) { c.sids = (uint16_t*)ps_malloc((size_t)m * 2); if (c.sids) memcpy(c.sids, tmp, (size_t)m * 2); }
        heap_caps_free(tmp);
      } else {
        c.count = n;
        c.sids = (uint16_t*)ps_malloc((size_t)n * 2);
        if (c.sids) memcpy(c.sids, seq, (size_t)n * 2);
      }
    }
    radioCatCount++;
  };
  // ---- 组0 频道: 全国 + 中央/国际/海外 ----
  addCat(RLBL[0], RLBL[0], true, allSeq, allCount, 0);   // 全国
  // 2 字缩写: 电视伴音->伴音 / 黑龙江->龙江 / 内蒙古->内蒙 (镜像 mobile labelMap)
  auto labelOf = [](const char* key) -> const char* {
    if (strcmp(key, "电视伴音") == 0) return "伴音";
    if (strcmp(key, "黑龙江") == 0) return "龙江";
    if (strcmp(key, "内蒙古") == 0) return "内蒙";
    return key;
  };
  auto regionCat = [&](const char* key, int grp) {
    static uint16_t rc[RMAX_ALL]; int n = 0;
    for (int i = 0; i < allCount; i++) { RadioStation* s = stn(allSeq[i]); if (s && strcmp(s->region, key) == 0) rc[n++] = allSeq[i]; }   // 仅 channels(1652), 与 preview region_cats 一致
    addCat(labelOf(key), key, false, rc, n, grp);
  };
  regionCat("中央", 0); regionCat("国际", 0); regionCat("海外", 0);
  // ---- 组1 省市: 置顶省 + 其余省 + 其它 ----
  const char* pinned = radioPinnedKey();
  strlcpy(radioPinnedUsed, pinned, sizeof(radioPinnedUsed));   // 记录本次用的置顶省名, loop 比对用
  if (strcmp(pinned, "中央") != 0 && strcmp(pinned, "国际") != 0 && strcmp(pinned, "海外") != 0)
    regionCat(pinned, 1);
  for (int i = 0; i < RORDER_N; i++) if (strcmp(RORDER[i], pinned) != 0) regionCat(RORDER[i], 1);
  regionCat("其它", 1);
// ---- 组2 我的: 收藏 -> 个人 -> 伴音 -> 助眠 -> 历史 (设计稿"我的"组序, 2026-10-24 补"助眠") ----
  addCat(RLBL[1], RLBL[1], true, favSids, favCount, 2);   // 收藏
  static uint16_t userSeq[16]; int userN = 0;
  for (int e = 0; e < extraCount; e++) if (stn(radioBinTotal + e) && stn(radioBinTotal + e)->is_user) userSeq[userN++] = radioBinTotal + e;
  addCat(RLBL[3], RLBL[3], true, userSeq, userN, 2);      // 个人
  regionCat("电视伴音", 2);                               // 伴音
  // 助眠 (运行时追加台: sid = radioBinTotal+extraCount .. radioStnTotal-1, 设计稿 6 台演示无真实 URL -> 点击不播放)
  { static uint16_t sleepSeq[8]; int sleepN = 0;
    for (int e = radioBinTotal + extraCount; e < radioStnTotal && sleepN < 8; e++) sleepSeq[sleepN++] = e;
    addCat("助眠", "助眠", true, sleepSeq, sleepN, 2);
  }
  addCat(RLBL[2], RLBL[2], true, histSids, histCount, 2); // 历史
}
// 2026-11-01 FIX(栈越界崩溃): 原来的 `uint8_t rn[41]; sf.read(rn, nl); rn[nl] = 0;` 直接用
// 文件里的名字字节数 nl 写入固定数组 —— nl 可以 >40 (实测 "Phate Radio 飞翔电台 - anime,
// games and pop" 为 47 字节), 越界写踩坏同一栈帧里 File sf 的 shared_ptr, 下一条 sf.read()
// 在脏指针上解引用 -> LoadProhibited (EXCVADDR 0x4c)。改为按缓冲容量截断读取, 余下字节用 seek 跳过
// (UI 里 RadioStation::name 也只有 32 字节, 本来就要截断)。
static void readVarStr(File& f, uint8_t* dst, int cap, int len) {
  if (len < 0) { dst[0] = 0; return; }
  int take = (len < cap - 1) ? len : cap - 1;
  if (take > 0 && f.read(dst, (size_t)take) != (size_t)take) { dst[0] = 0; return; }
  dst[take] = 0;
  if (len > take) f.seek(f.position() + (uint32_t)(len - take));
}
bool loadRadioMeta() {
  File f = sdstore::openRead("/radio_meta.bin");
  if (!f) { Serial.println(F("radio_meta.bin not found")); return false; }
  uint8_t magic[9]; f.read(magic, 8); magic[8] = 0;
  if (strcmp((char*)magic, "ESP32RDT") != 0 || f.read() != 1) { Serial.println(F("radio_meta bad")); f.close(); return false; }
  uint16_t binTotal   = f.read() | (f.read() << 8);
  extraCount = f.read() | (f.read() << 8);   // 写入全局 extraCount, buildRadioCats() 用
  f.read(); f.read();                 // userCount (skip)
  uint16_t favCnt     = f.read() | (f.read() << 8);
  uint16_t histCnt    = f.read() | (f.read() << 8);
  uint16_t allCnt     = f.read() | (f.read() << 8);
  f.read();                            // reserved (skip)
  radioBinTotal = binTotal;
  // PSRAM heap on this board is mapped near 0x3D9xxxxx (NOT 0x3C000000).
  // heap_caps_malloc(MALLOC_CAP_SPIRAM) returns a valid PSRAM pointer (same path as fonts).
  radioStn = (RadioStation*)heap_caps_malloc((size_t)RMAX_STN * sizeof(RadioStation), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!radioStn) {
    Serial.printf("radioStn PSRAM alloc failed (need %uKB, free %uKB)\n",
                  (unsigned)((size_t)RMAX_STN * sizeof(RadioStation)) / 1024,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    f.close(); return false;
  }
  uint16_t* all  = (uint16_t*)ps_malloc((size_t)allCnt * 2);
  uint16_t* fav  = (uint16_t*)ps_malloc((size_t)favCnt * 2);
  uint16_t* hist = (uint16_t*)ps_malloc((size_t)histCnt * 2);
  if (!all || !fav || !hist) { f.close(); return false; }
  for (int i = 0; i < allCnt; i++) all[i] = f.read() | (f.read() << 8);
  for (int i = 0; i < favCnt; i++) fav[i] = f.read() | (f.read() << 8);
  for (int i = 0; i < histCnt; i++) hist[i] = f.read() | (f.read() << 8);
  radioStnTotal = 0;
  File sf = sdstore::openRead("/stations.bin");   // 一次打开, 40 个 region 复用同一句柄
  for (uint16_t ri = 0; sf && ri < regionCount && radioStnTotal < RMAX_STN; ri++) {
    sf.seek(regionEntries[ri].first_offset);
    for (uint16_t c = 0; c < regionEntries[ri].count && radioStnTotal < RMAX_STN; c++) {
      uint32_t recStart = sf.position();
      int nl = sf.read();
      if (nl <= 0) { Serial.printf("[RADIO] rec bad r%u c%u pos=%u nl=%d\n", ri, c, (unsigned)recStart, nl); break; }
      uint8_t rn[40]; readVarStr(sf, rn, (int)sizeof(rn), nl);
      int lo = sf.read(), hi = sf.read();
      if (lo < 0 || hi < 0) break;
      uint16_t ul = (uint16_t)(lo | (hi << 8));
      if (!sf.seek(sf.position() + ul)) break;          // 跳过 URL 正文, 不常驻 RAM
      int ridx = sf.read();
      int cl = sf.read(); if (ridx < 0 || cl < 0) break;
      if (ridx >= regionCount) break;                   // 2026-11-01: 原代码直接用 regionEntries[ridx] 无边界检查
      uint8_t rc[13]; readVarStr(sf, rc, (int)sizeof(rc), cl);
      int fl = sf.read(); if (fl < 0) break;
      uint8_t rf[13]; readVarStr(sf, rf, (int)sizeof(rf), fl);
      int rr = sf.read(), gg = sf.read(), bb = sf.read(); if (bb < 0) break;
      RadioStation& s = radioStn[radioStnTotal];
      truncCopy(s.name, sizeof(s.name), rn, nl);
      truncCopy(s.freq, sizeof(s.freq), rf, fl);
      memcpy(s.region, regionEntries[ridx].name, 17);
      truncCopy(s.cat, sizeof(s.cat), rc, cl);
      s.r = (uint8_t)rr; s.g = (uint8_t)gg; s.b = (uint8_t)bb;
      s.is_user = false; s.in_bin = true; s.url_offset = recStart; radioStnTotal++;
    }
  }
  if (sf) sf.close();
  for (int e = 0; e < extraCount && radioStnTotal < RMAX_STN; e++) {
    int nl = f.read();
    if (nl <= 0) break;
    uint8_t rn[40]; readVarStr(f, rn, (int)sizeof(rn), nl);
    uint32_t urlOff = (uint32_t)f.position();        // URL 长度字段绝对偏移 (起播时按需读)
    int lo = f.read(), hi = f.read();
    if (lo < 0 || hi < 0) break;
    uint16_t ul = (uint16_t)(lo | (hi << 8));
    if (!f.seek(f.position() + ul)) break;            // 跳过 URL 正文, 不常驻 RAM
    int rl = f.read(); if (rl < 0) break;
    uint8_t rr[18]; readVarStr(f, rr, (int)sizeof(rr), rl);
    int cl = f.read(); if (cl < 0) break;
    uint8_t rc[13]; readVarStr(f, rc, (int)sizeof(rc), cl);
    int fl = f.read(); if (fl < 0) break;
    uint8_t rf[13]; readVarStr(f, rf, (int)sizeof(rf), fl);
    int r = f.read(), g = f.read(), bb = f.read(), isUser = f.read();
    if (isUser < 0) break;
    RadioStation& s = radioStn[radioStnTotal];
    truncCopy(s.name, sizeof(s.name), rn, nl);
    truncCopy(s.freq, sizeof(s.freq), rf, fl);
    truncCopy(s.region, sizeof(s.region), rr, rl);
    truncCopy(s.cat, sizeof(s.cat), rc, cl);
    s.r = (uint8_t)r; s.g = (uint8_t)g; s.b = (uint8_t)bb; s.is_user = (isUser != 0);
    s.in_bin = false; s.url_offset = urlOff; radioStnTotal++;
  }
  memcpy(allSeq, all, (size_t)allCnt * 2); allCount = allCnt;
  memcpy(favSids, fav, (size_t)favCnt * 2); favCount = favCnt;
  memcpy(histSids, hist, (size_t)histCnt * 2); histCount = histCnt;
  radioNvsLoad();          // 2026-10-20: NVS 里用户运行时点亮的收藏/历史优先于离线 meta
  addSleepStations();      // 2026-10-24 问题①: 追加"助眠"6 台 (设计稿"我的"组), 必须在 buildRadioCats 前
  buildRadioCats();
  f.close();
  Serial.printf("radio_meta: %d stn, %d cats, %d all, %d fav, %d hist\n", radioStnTotal, radioCatCount, allCount, favCount, histCount);
  radioInited = true; return true;
}
// ---- 收藏/历史 运行时更新 + NVS 持久化 (设计稿: 星标实时同步, 历史自动记 30 个) ----
#define RNVS_NS      "rmeta"
#define RNVS_HISTMAX 30
bool radioMuted = false;
static void radioNvsSaveFH() {
  nvs_handle_t h;
  if (nvs_open(RNVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_blob(h, "fav", favSids, (size_t)favCount * 2);
  nvs_set_blob(h, "hist", histSids, (size_t)histCount * 2);
  nvs_commit(h); nvs_close(h);
}
void radioNvsLoad() {
  nvs_handle_t h;
  if (nvs_open(RNVS_NS, NVS_READONLY, &h) != ESP_OK) return;
  size_t sz = 0;
  if (nvs_get_blob(h, "fav", NULL, &sz) == ESP_OK && sz > 0 && sz <= sizeof(favSids) && sz % 2 == 0)
    if (nvs_get_blob(h, "fav", favSids, &sz) == ESP_OK) favCount = (int)(sz / 2);
  sz = 0;
  if (nvs_get_blob(h, "hist", NULL, &sz) == ESP_OK && sz > 0 && sz <= sizeof(histSids) && sz % 2 == 0)
    if (nvs_get_blob(h, "hist", histSids, &sz) == ESP_OK) histCount = (int)(sz / 2);
  nvs_close(h);
}
void radioToggleFav(int sid) {
  if (sid < 0 || sid >= radioStnTotal) return;
  int idx = -1;
  for (int i = 0; i < favCount; i++) if (favSids[i] == sid) { idx = i; break; }
  if (idx >= 0) {                       // 取消收藏
    for (int i = idx; i < favCount - 1; i++) favSids[i] = favSids[i + 1];
    favCount--;
  } else {                              // 收藏 (满则挤掉最旧)
    if (favCount >= RMAX_FAV) favCount = RMAX_FAV - 1;
    for (int i = favCount; i > 0; i--) favSids[i] = favSids[i - 1];
    favSids[0] = (uint16_t)sid; favCount++;
  }
  radioNvsSaveFH();
  buildRadioCats();                     // 收藏分类实时同步
  Serial.printf("[RADIO] fav toggle sid=%d -> %d favs\n", sid, favCount);
}
void radioPushHist(int sid) {
  if (sid < 0 || sid >= radioStnTotal) return;
  int idx = -1;
  for (int i = 0; i < histCount; i++) if (histSids[i] == sid) { idx = i; break; }
  if (idx >= 0) { for (int i = idx; i < histCount - 1; i++) histSids[i] = histSids[i + 1]; histCount--; }
  if (histCount >= RNVS_HISTMAX) histCount = RNVS_HISTMAX - 1;   // 设计稿: 保留最近 30 个
  for (int i = histCount; i > 0; i--) histSids[i] = histSids[i - 1];
  histSids[0] = (uint16_t)sid; histCount++;
  radioNvsSaveFH();
  buildRadioCats();                     // 历史分类实时同步
}
void radioPause() {   // 暂停: 停流但保留 currentPlayingSid, 播放条 ▶ 可续播同一台
  Serial.printf("[RADIO] pause cur=%d pending=%d\n", currentPlayingSid, pendingPlaySid);
  radioPlaying = false;
  audio.stopSong();
  STATUS_LED_OFF();
}
void radioToggleMute() {
  radioMuted = !radioMuted;
  audio.setVolume(radioMuted ? 0 : (uint8_t)radioVolume);
  Serial.printf("[RADIO] mute -> %d (vol=%d)\n", radioMuted, radioVolume);
}
void radioVolumeDelta(int d) {   // 2026-10-28: 全屏音量±按钮 (0..RADIO_VOL_MAX=64, 每步±1≈1.6%, 调音量自动解除静音)
  radioMuted = false;
  int nv = radioVolume + d;
  if (nv < 0) nv = 0;
  if (nv > RADIO_VOL_MAX) nv = RADIO_VOL_MAX;
  radioVolume = nv;
  audio.setVolume((uint8_t)radioVolume);
  Serial.printf("[RADIO] vol -> %d\n", radioVolume);
}
void playStationAt(int sid) {
  RadioStation* s = stn(sid); if (!s) return;
  if (currentPlayingSid == sid && radioPlaying) return;
  String url = readURLForSid(sid);
  if (url.length() == 0) return;
  // 延迟起播: 双击窗口(RADIO_DBL_MS)关闭后, 由 modeRadio() 真正连流(见下方刷入)。
  // audio.connecttohost() 阻塞(连流要几秒), 若点下就立刻连会吞掉双击窗口,
  // 使"双击正在播放的电台"的第二下被当成新单击, 打不开播放界面(全屏)。
  musicPlaying = false;               // 点电台 -> 停掉节日音乐 (收音机优先)
  Serial.printf("[RADIO] play req sid=%d cur=%d playing=%d pending=%d now=%lu\n",
                sid, currentPlayingSid, radioPlaying, pendingPlaySid, (unsigned long)millis());
  // 2026-10-28 修复: 同一台已在等待起播时, 不重置 pendingPlayAt ——
  // 否则用户单击后 600ms 内(双击窗口外)连点同台, 每次都会重置计时器 -> 永不 flush -> 不播放
  // (表现为"按钮一直是播放图标" + "环球资讯不播放")
  if (pendingPlaySid == sid) return;
  pendingPlaySid = sid; pendingPlayAt = millis();
}
void stopRadio() { radioPlaying = false; audio.stopSong(); currentPlayingSid = -1; STATUS_LED_OFF(); }  // BUILDV5-NETTASK: 先停泵再停流





// ============================================================
// 节日/节气 音乐 (网络�? 与收音机同一�?�?, 不占 flash)
// 注意: 与收音机共用同一�?audio 解码�? 二�?�互�? 收音机�?�终优先�?
// ============================================================
// 音乐 URL 配置来自 LittleFS 文件 /festival.json (�?`pio run -t uploadfs`
// 上传, 改配�?无需重新编译固件, 改完上传后重�?即可生�??:
//   { "festival_music": { "春节": "http://...", "元旦": "http://..." } }
//   URL 为空 = 该事件不�?音乐。文件缺�?损坏 = 不播 (显示不受影响)�?
