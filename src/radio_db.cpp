#include "ui_common.h"

String readURLForSid(int sid) {
  RadioStation* s = stn(sid); if (!s) return "";
  if (s->in_bin) {
    File f = LittleFS.open("/stations.bin", "r"); if (!f) return "";
    f.seek(s->url_offset);
    uint8_t nl = f.read(); f.seek(f.position() + nl);
    uint16_t ul = f.read() | (f.read() << 8);
    uint8_t ub[257]; f.read(ub, min(ul, (uint16_t)256)); ub[min(ul, (uint16_t)256)] = 0;
    f.close(); return String((char*)ub);
  }
  int ei = (int)s->url_offset; return (ei >= 0 && ei < extraUrlCount) ? String(extraUrl[ei]) : String();
}
bool loadRadioDB() {
  File f = LittleFS.open("/stations.bin", "r");
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
  Serial.printf("stations.bin: %d regions, %lu stations\n", numRegions, numStations);
  f.close(); return true;
}
// ---- 定位省份(完整名) -> regionCat 键: geo::pinned() 非空则用之, 否则退回 RPINNED(兜底"广东")。
// 注意: 键必须与 stations.bin 的完整省名一致(黑龙江/内蒙古), 显示 2 字缩写由 labelOf 负责。----
const char* radioPinnedKey() {
  const char* p = geo::pinned();
  return (p && p[0]) ? p : RPINNED;
}
// 重建收音机分类 (全部/收藏/历史/个人 + 地区)。地区顺序镜像 mobile getCategoryList():
// RHEAD(4) -> 置顶省(若不在 RHEAD/RORDER 则插到 RHEAD 后) -> RORDER(跳过已置顶) -> RTAIL(2)。
// 联网定位到省份后可再调一次重建, 让置顶省(完整名)正确出现在列表中 (与 preview 一致)。
void buildRadioCats() {
  // 释放旧 sids (避免重复构建时 PSRAM 泄漏)
  for (int i = 0; i < radioCatCount; i++) { if (radioCats[i].sids) heap_caps_free(radioCats[i].sids); }
  radioCatCount = 0;
  auto addCat = [&](const char* label, const char* key, bool meta, const uint16_t* seq, int n) {
    if (radioCatCount >= RMAX_CATS) return;
    RadioCat& c = radioCats[radioCatCount];
    truncCopy(c.label, sizeof(c.label), (const uint8_t*)label, (int)strlen(label));
    truncCopy(c.key, sizeof(c.key), (const uint8_t*)key, (int)strlen(key));
    c.meta = meta; c.count = n;
    c.sids = (n > 0) ? (uint16_t*)ps_malloc((size_t)n * 2) : nullptr;
    if (c.sids && seq) memcpy(c.sids, seq, (size_t)n * 2);
    radioCatCount++;
  };
  addCat(RLBL[0], RLBL[0], true, allSeq, allCount);
  addCat(RLBL[1], RLBL[1], true, favSids, favCount);
  addCat(RLBL[2], RLBL[2], true, histSids, histCount);
  static uint16_t userSeq[16]; int userN = 0;
  for (int e = 0; e < extraCount; e++) if (stn(radioBinTotal + e) && stn(radioBinTotal + e)->is_user) userSeq[userN++] = radioBinTotal + e;
  addCat(RLBL[3], RLBL[3], true, userSeq, userN);
  // 2 字缩写: 电视伴音->伴音 / 黑龙江->龙江 / 内蒙古->内蒙 (镜像 mobile labelMap)
  auto labelOf = [](const char* key) -> const char* {
    if (strcmp(key, "电视伴音") == 0) return "伴音";
    if (strcmp(key, "黑龙江") == 0) return "龙江";
    if (strcmp(key, "内蒙古") == 0) return "内蒙";
    return key;
  };
  auto regionCat = [&](const char* key) {
    static uint16_t rc[RMAX_ALL]; int n = 0;
    for (int i = 0; i < allCount; i++) { RadioStation* s = stn(allSeq[i]); if (s && strcmp(s->region, key) == 0) rc[n++] = allSeq[i]; }   // 仅 channels(1652), 与 preview region_cats 一致
    addCat(labelOf(key), key, false, rc, n);
  };
  const char* pinned = radioPinnedKey();
  strlcpy(radioPinnedUsed, pinned, sizeof(radioPinnedUsed));   // 记录本次用的置顶省名, loop 比对用
  for (int i = 0; i < 4; i++) regionCat(RHEAD[i]);
  if (strcmp(pinned, RHEAD[0]) != 0 && strcmp(pinned, RHEAD[1]) != 0 && strcmp(pinned, RHEAD[2]) != 0 && strcmp(pinned, RHEAD[3]) != 0)
    regionCat(pinned);
  for (int i = 0; i < RORDER_N; i++) if (strcmp(RORDER[i], pinned) != 0) regionCat(RORDER[i]);
  for (int i = 0; i < 2; i++) regionCat(RTAIL[i]);
}
bool loadRadioMeta() {
  File f = LittleFS.open("/radio_meta.bin", "r");
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
  for (uint16_t ri = 0; ri < regionCount && radioStnTotal < RMAX_STN; ri++) {
    File sf = LittleFS.open("/stations.bin", "r"); if (!sf) break;
    sf.seek(regionEntries[ri].first_offset);
    for (uint16_t c = 0; c < regionEntries[ri].count && radioStnTotal < RMAX_STN; c++) {
      uint32_t recStart = sf.position();
      uint8_t nl = sf.read(); uint8_t rn[41]; sf.read(rn, nl); rn[nl] = 0;
      uint16_t ul = sf.read() | (sf.read() << 8); sf.seek(sf.position() + ul);
      uint8_t ridx = sf.read();
      uint8_t cl = sf.read(); uint8_t rc[13]; sf.read(rc, cl); rc[cl] = 0;
      uint8_t fl = sf.read(); uint8_t rf[21]; sf.read(rf, fl); rf[fl] = 0;
      RadioStation& s = radioStn[radioStnTotal];
      truncCopy(s.name, sizeof(s.name), rn, nl);
      truncCopy(s.freq, sizeof(s.freq), rf, fl);
      memcpy(s.region, regionEntries[ridx].name, 17);
      truncCopy(s.cat, sizeof(s.cat), rc, cl);
      s.r = sf.read(); s.g = sf.read(); s.b = sf.read();
      s.is_user = false; s.in_bin = true; s.url_offset = recStart; radioStnTotal++;
    }
    sf.close();
  }
  for (int e = 0; e < extraCount && radioStnTotal < RMAX_STN; e++) {
    uint8_t nl = f.read(); uint8_t rn[41]; f.read(rn, nl); rn[nl] = 0;
    uint16_t ul = f.read() | (f.read() << 8); uint8_t ur[257]; f.read(ur, min(ul, (uint16_t)256)); ur[min(ul, (uint16_t)256)] = 0;
    uint8_t rl = f.read(); uint8_t rr[18]; f.read(rr, rl); rr[rl] = 0;
    uint8_t cl = f.read(); uint8_t rc[13]; f.read(rc, cl); rc[cl] = 0;
    uint8_t fl = f.read(); uint8_t rf[17]; f.read(rf, fl); rf[fl] = 0;
    uint8_t r = f.read(), g = f.read(), bb = f.read(); uint8_t isUser = f.read();
    if (extraUrlCount < RMAX_EXTRA) { memcpy(extraUrl[extraUrlCount], (char*)ur, min(ul, (uint16_t)259)); extraUrlCount++; }
    RadioStation& s = radioStn[radioStnTotal];
    truncCopy(s.name, sizeof(s.name), rn, nl);
    truncCopy(s.freq, sizeof(s.freq), rf, fl);
    truncCopy(s.region, sizeof(s.region), rr, rl);
    truncCopy(s.cat, sizeof(s.cat), rc, cl);
    s.r = r; s.g = g; s.b = bb; s.is_user = (isUser != 0);
    s.in_bin = false; s.url_offset = extraUrlCount - 1; radioStnTotal++;
  }
  memcpy(allSeq, all, (size_t)allCnt * 2); allCount = allCnt;
  memcpy(favSids, fav, (size_t)favCnt * 2); favCount = favCnt;
  memcpy(histSids, hist, (size_t)histCnt * 2); histCount = histCnt;
  buildRadioCats();
  f.close();
  Serial.printf("radio_meta: %d stn, %d cats, %d all, %d fav, %d hist\n", radioStnTotal, radioCatCount, allCount, favCount, histCount);
  radioInited = true; return true;
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
  pendingPlaySid = sid; pendingPlayAt = millis();
}
void stopRadio() { radioPlaying = false; audio.stopSong(); currentPlayingSid = -1; digitalWrite(STATUS_LED_PIN, LOW); }  // BUILDV5-NETTASK: 先停泵再停流





// ============================================================
// 节日/节气 音乐 (网络�? 与收音机同一�?�?, 不占 flash)
// 注意: 与收音机共用同一�?audio 解码�? 二�?�互�? 收音机�?�终优先�?
// ============================================================
// 音乐 URL 配置来自 LittleFS 文件 /festival.json (�?`pio run -t uploadfs`
// 上传, 改配�?无需重新编译固件, 改完上传后重�?即可生�??:
//   { "festival_music": { "春节": "http://...", "元旦": "http://..." } }
//   URL 为空 = 该事件不�?音乐。文件缺�?损坏 = 不播 (显示不受影响)�?
