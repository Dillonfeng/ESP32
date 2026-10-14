#include "ui_common.h"

int radioFailCnt = 0;   // 起播连接失败计数 (3 次彻底停, 见 modeRadio 延迟起播处)
uint32_t radioWasRunAt = 0;   // BUILD15: watchdog timer base; reset on each successful connect so a fresh
                              // buffering stream is not killed by a stale/zero value (fixes silent no-response).
void pumpRadio() {
  if (audio.isRunning()) radioWasRunAt = millis();
  else if (millis() - radioWasRunAt > 8000) {
    stopRadio();
    Serial.println("[RADIO] stream dead >8s -> stopped");
    return;
  }
  // BUILD16: 2nd watchdog - "connected but silent".
  // The library's isRunning() stays true even when it is stuck in its internal
  // reconnect loop (e.g. a master-playlist URL that it misparses -> 404 ->
  // reconnect every ~5s, forever, screen frozen). getTotalPlayingTime() only
  // starts counting once the FIRST audio frame is actually decoded, so
  // connected>30s with zero decoded audio = poison stream -> force stop.
  if (millis() - radioWasRunAt > 30000 && audio.getAudioCurrentTime() == 0) {
    stopRadio();
    Serial.println("[RADIO] no audio output >30s after connect -> stopped");
    return;
  }
  // BUILD26: periodic memory telemetry while playing (生产模式已删除，UART 竞争根除)
  // BUILDV5-HLSFIX: MEM/AUDIO 遥测打印已删除
  // BUILDV5-NETTASK: 下载泵已独立到 netTask（main loop 不再调 audio.loop）。
  // 根因（cap_td1 实证）：audio.loop() 每次调用阻塞 ~60-150ms（分片切换 httpPrint/
  // 网络等待）→ main loop 内高频泵反而把 UI 帧率拖到 1fps → 下载 12KB/s << 播放消耗
  // 110KB/s。netTask 单任务高频泵：阻塞只影响 netTask，UI 帧率恢复，下载速率达标。
}

void modeRadio() {
  if (!radioInited) return;

  // 延迟起播刷入: 双击窗口(RADIO_DBL_MS)已关闭且未进入全屏 -> 真正连流。
  // (若期间双击进了全屏, modeDrawn 被清, 此处不刷 -> 相当于取消起播)
  if (pendingPlaySid >= 0 && !showingFullscreen && millis() - pendingPlayAt >= RADIO_PLAY_FLUSH_MS) {
    int sid = pendingPlaySid; pendingPlaySid = -1;
    Serial.printf("[DBG-T] FLUSH deferred play sid=%d (elapsed %lu ms)\n", sid, millis() - pendingPlayAt);
    RadioStation* s = stn(sid);
    if (s) {
      String url = readURLForSid(sid);
      if (url.length() > 0) {
        radioPlaying = false;   // BUILDV5-NETTASK: 先停 netTask 泵（避免切台与 loop 并发）
        audio.stopSong();       // 停止旧流（无论 radioPlaying 是否已 true）
        // BUILD14 诊断: 起播前内部 RAM 余量 (HTTPS 电台 TLS 握手 esp-aes 需连续
        // ~8-12KB; free/最低堆过小 -> 连接必失败 = "总是停" 根因, boot log 已见
        // "esp-aes: Failed to allocate memory")
        Serial.printf("[RADIO] heap free=%u max=%u url=%s\n",
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(), url.c_str());
        // BUILD17: the decisive metric for HTTPS radio = largest contiguous internal-heap block
        // (mbedTLS handshake needs one big block, ~48KB; fragmentation, not total, is the enemy).
        // Before BUILD17: ~41KB (cap in bootcap40.log). After: should be ~200KB with clock bufs freed.
        Serial.printf("[MEM] pre-connect: internalFree=%uKB maxBlock=%uKB dramFree=%uKB\n",
                      (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                      (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                      (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
        Serial.printf("Playing: %s\n", s->name);
        bool ok = audio.connecttohost(url.c_str());
        // BUILD14 看门狗: connecttohost 失败 (TLS OOM / 断网 / 源 404) 不再假播。
        // 连败 3 次 = 彻底停 (清 radioPlaying/LED/当前台), 防止"点一下永远转圈"。
        if (ok) {
          radioPlaying = true; currentPlayingSid = sid;
          radioFailCnt = 0;
          radioWasRunAt = millis();          // BUILD15: arm the >8s dead-timer from connect time (fresh-stream grace)
          listScrollDirty = true;            // BUILD15: redraw list+playbar so display matches the station now playing (fix desync)
          digitalWrite(STATUS_LED_PIN, HIGH);
        } else {
          radioFailCnt++;
          Serial.printf("[RADIO] connect FAILED (try %d/3)\n", radioFailCnt);
          if (radioFailCnt >= 3) {
            radioFailCnt = 0;
            radioPlaying = false; currentPlayingSid = -1;
            if (!musicPlaying) digitalWrite(STATUS_LED_PIN, LOW);
            Serial.println("[RADIO] 3x connect failed -> stopped");
          }
          listScrollDirty = true;            // BUILD15: refresh playbar after a failed connect (show stopped / prior state)
        }
      }
    }
  }

  if (showingFullscreen) {
    if (!modeDrawn[MODE_RADIO]) {
      drawRadioHeader(); drawRadioRegionSidebar(); drawRadioList(); drawRadioPlayBar(); drawStationFullscreen();
      modeDrawn[MODE_RADIO] = true;
    }
    if (radioPlaying) pumpRadio();
    return;
  }

  if (!modeDrawn[MODE_RADIO] || modeChanged) {
    tft.fillRect(0, 0, 480, NAV_BAR_Y, COLOR_BG);
    drawRadioHeader(); drawRadioRegionSidebar(); drawRadioList(); drawRadioPlayBar();
    modeDrawn[MODE_RADIO] = true; listScrollDirty = false; sideScrollDirty = false;
  }

  // 2026-09-27: 侧栏局部刷新 (只刷 70x260 左列), startWrite/endWrite 包整段 (硬件反模式 R1)
  if (sideScrollDirty) { tft.startWrite(); drawRadioRegionSidebar(); tft.endWrite(); sideScrollDirty = false; }

  if (listScrollDirty) {
    // BUILD50-LISTSCROLL: 与侧栏一致, startWrite/endWrite 包整段(硬件反模式 R1),
    // 消除拖拽中列表重绘的闪烁/撕裂。
    tft.startWrite(); drawRadioList(); drawRadioPlayBar(); tft.endWrite();
    listScrollDirty = false;
  }

  if (radioPlaying) pumpRadio();
}

// ============================================================
// 收音机数�?加�??
// ============================================================
// ===== 收音机数据加载 + helpers (与 make_preview.load_radio 逐站逐序一致) =====
void truncCopy(char* dst, int n, const uint8_t* s, int len) {
  if (len <= n - 1) { memcpy(dst, s, len); dst[len] = 0; return; }
  memcpy(dst, s, n - 4); dst[n - 4] = '.'; dst[n - 3] = '.'; dst[n - 2] = '.'; dst[n - 1] = 0;
}
uint16_t catColor565(const char* cat) {
  for (int i = 0; i < (int)RADIO_CATN; i++) if (strcmp(RADIO_CATNAMES[i], cat) == 0) return RADIO_CATCOLS[i];
  return RADIO_CATCOLS[1];   // 未匹配 -> 综合色
}
RadioStation* stn(int sid) { return (sid >= 0 && sid < radioStnTotal) ? &radioStn[sid] : nullptr; }
RadioCat*     catv(int i)  { return (i >= 0 && i < radioCatCount) ? &radioCats[i] : nullptr; }
int  curCount() { RadioCat* c = catv(currentCat); return c ? c->count : 0; }
uint16_t curSidAt(int i) { RadioCat* c = catv(currentCat); return (c && i >= 0 && i < c->count) ? c->sids[i] : 0xFFFF; }
RadioStation* curStnAt(int i) { RadioCat* c = catv(currentCat); return (c && i >= 0 && i < c->count) ? stn(c->sids[i]) : nullptr; }
const char* catLabel(int i) { RadioCat* c = catv(i); return c ? c->label : ""; }
const char* regionNameOf(const RadioStation* s) { return s ? s->region : ""; }
const char* iconChar(const RadioStation* s) { return (s && s->name[0]) ? s->name : "?"; }
bool isFavSid(int sid) { for (int i = 0; i < favCount; i++) if (favSids[i] == sid) return true; return false; }
void handleRadioTap(uint16_t tx, uint16_t ty) {
  // 全屏: 点任意处关闭
  if (showingFullscreen) { showingFullscreen = false; modeDrawn[MODE_RADIO] = false; return; }
  // 侧边栏 (固定 4 行 + 可滚动地区行)
  if (tx < RADIO_SIDEBAR_W) {
    // 2026-09-27: 侧栏整列(固定4行+地区行)一起滚动, regionScrollTop = 像素偏移
    int S = regionScrollTop;
    int top = RADIO_HEADER_H - S;
    int boundary = RADIO_REGION_TOP - S;   // 地区行顶端 (随滚动移动)
    int idx = -1;
    if (ty < boundary) {
      if (ty >= top) idx = (ty - top) / RADIO_SPECIAL_H;
    } else if (ty >= boundary) {
      idx = RADIO_SPECIAL_CNT + (ty - boundary) / RADIO_ROW_H_SIDE;
    }
    if (idx >= 0 && idx < radioCatCount && idx != currentCat) {
      currentCat = idx; listScrollTop = 0; modeDrawn[MODE_RADIO] = false;
    }
    return;
  }
  // 播放栏 (三键: 上一 cx0-45 / 播放 cx0 / 下一 cx0+45, 各宽 ~44)
  if (ty >= RADIO_LIST_BOT && ty < NAV_BAR_Y) {
    int barX = RADIO_LIST_X, cx0 = barX + RADIO_LIST_W / 2;
    if (tx >= barX && tx < barX + RADIO_LIST_W) {
      int relX = tx - barX;
      int pos = -1;
      if (currentPlayingSid >= 0) for (int i = 0; i < curCount(); i++) if (curSidAt(i) == currentPlayingSid) { pos = i; break; }
      if (relX >= cx0 - 68 && relX <= cx0 - 22 && pos > 0) playStationAt(curSidAt(pos - 1));      // 上一
      else if (relX >= cx0 - 15 && relX <= cx0 + 15) { if (radioPlaying) stopRadio(); else if (pos >= 0) playStationAt(curSidAt(pos)); modeDrawn[MODE_RADIO] = false; }  // 播放/暂停
      else if (relX >= cx0 + 22 && relX <= cx0 + 68 && pos >= 0 && pos < curCount() - 1) playStationAt(curSidAt(pos + 1));  // 下一
    }
    return;
  }
  // 滚动条点击跳转
  if (ty >= RADIO_LIST_TOP && ty < RADIO_LIST_BOT && tx >= RADIO_LIST_X + RADIO_LIST_W && tx < RADIO_LIST_X + RADIO_LIST_W + RADIO_SCROLLBAR_W) {
    int count = curCount();
    int scrollRange = max(1, count - RADIO_STATION_ROWS);
    int listH = RADIO_LIST_BOT - RADIO_LIST_TOP;
    int scrollPos = (ty - RADIO_LIST_TOP) * scrollRange / listH;
    scrollPos = constrain(scrollPos, 0, max(0, count - RADIO_STATION_ROWS));
    if (scrollPos != listScrollTop) { listScrollTop = scrollPos; listScrollDirty = true; }
    return;
  }
  // 列表: 单击播放, 双击全屏
  if (ty >= RADIO_LIST_TOP && ty < RADIO_LIST_BOT && tx >= RADIO_LIST_X) {
    int listY = ty - RADIO_LIST_TOP;
    int rowIdx = listScrollTop + listY / RADIO_ROW_H;
    if (rowIdx >= 0 && rowIdx < curCount()) {
      unsigned long now = millis();
      if (now - lastTapTime < RADIO_DBL_MS && lastTapRow == rowIdx) {
        showingFullscreen = true; pendingPlaySid = -1; modeDrawn[MODE_RADIO] = false;   // 双击 -> 播放界面, 取消延迟起播
        Serial.printf("[DBG-T] DOUBLETAP row=%d dt=%lu -> fullscreen (pendingPlay cancelled)\n", rowIdx, now - lastTapTime);
      } else { playStationAt(curSidAt(rowIdx)); listScrollDirty = true;
        Serial.printf("[DBG-T] singletap row=%d dt=%lu lastRow=%d -> pendingPlay sid=%d\n", rowIdx, now - lastTapTime, lastTapRow, pendingPlaySid); }
      lastTapTime = now; lastTapRow = rowIdx;
    }
    return;
  }
}

void drawRadioHeader() {
  tft.fillRect(0, 0, 480, RADIO_HEADER_H, 0x1042);
  const char* label = catLabel(currentCat);
  int cnt = curCount();
  char title[64];
  snprintf(title, sizeof(title), "收音机· %s (%d)", label ? label : "无", cnt);
  font16.drawString(6, 6, title, TFT_WHITE, 0x1042);
  if (wifiConnected) {
    String ip = WiFi.localIP().toString();
    font16.drawString(470 - font16.textWidth(ip.c_str()), 6, ip.c_str(), COLOR_DIM, 0x1042);
  } else {
    font16.drawString(470 - 48, 6, "无网络", TFT_RED, 0x1042);
  }
  tft.drawFastVLine(RADIO_SIDEBAR_W - 1, RADIO_HEADER_H, NAV_BAR_Y - RADIO_HEADER_H, 0x3186);
}

void drawRadioRegionSidebar() {
  // 2026-09-27: 侧栏整列(固定4行+地区行)一起滚动, 像素级; 行裁剪到视口 (28..288)。
  tft.fillRect(0, RADIO_HEADER_H, RADIO_SIDEBAR_W, NAV_BAR_Y - RADIO_HEADER_H, 0x0821);
  int S = regionScrollTop;                 // 像素偏移 (0..smax)
  int top = RADIO_HEADER_H - S;
  for (int i = 0; i < radioCatCount; i++) {
    int rh = (i < RADIO_SPECIAL_CNT) ? RADIO_SPECIAL_H : RADIO_ROW_H_SIDE;
    int base = (i < RADIO_SPECIAL_CNT) ? i * RADIO_SPECIAL_H
                                       : RADIO_SPECIAL_CNT * RADIO_SPECIAL_H + (i - RADIO_SPECIAL_CNT) * RADIO_ROW_H_SIDE;
    int ry = top + base;
    int y0 = (ry > RADIO_HEADER_H) ? ry : RADIO_HEADER_H;
    int fy1 = (ry + rh - 1 < NAV_BAR_Y - 1) ? ry + rh - 1 : NAV_BAR_Y - 1;
    if (fy1 < y0) continue;
    bool sel = (i == currentCat);
    uint16_t bg = sel ? 0x210C : 0x0821;
    uint16_t fg = sel ? TFT_RED : COLOR_DIM;
    tft.fillRect(0, y0, RADIO_SIDEBAR_W - 2, fy1 - y0 + 1, bg);
    if (sel) tft.drawFastVLine(0, y0, fy1 - y0 + 1, TFT_RED);
    int tyy = ry + (rh - 16) / 2;
    if (tyy >= y0 && tyy + 16 - 1 <= fy1)
      font16.drawString(4, tyy, catLabel(i), fg, bg);
  }
  tft.drawFastVLine(RADIO_SIDEBAR_W - 1, RADIO_HEADER_H, NAV_BAR_Y - RADIO_HEADER_H, 0x3186);
  // 侧栏滚动条 (右侧细条, 整列, 仅当总高 > 视口)
  int totalH = RADIO_SPECIAL_CNT * RADIO_SPECIAL_H + max(0, radioCatCount - RADIO_SPECIAL_CNT) * RADIO_ROW_H_SIDE;
  int viewH = NAV_BAR_Y - RADIO_HEADER_H;
  if (totalH > viewH) {
    int thumbH = max(12, viewH * viewH / totalH);
    int smax = totalH - viewH;
    int thumbY = RADIO_HEADER_H + (viewH - thumbH) * regionScrollTop / smax;
    tft.fillRect(RADIO_SIDEBAR_W - 3, thumbY, 2, thumbH, TFT_WHITE);
  }
  tft.drawFastVLine(RADIO_SIDEBAR_W - 1, RADIO_HEADER_H, NAV_BAR_Y - RADIO_HEADER_H, 0x3186);
}
static int utf8_next(const uint8_t* p, uint16_t* code) {
  if (*p >= 0xF0) { *code = (((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F)); return 4; }
  if (*p >= 0xE0) { *code = (((*p & 0x0F) << 6) | (p[1] & 0x3F)); return 3; }
  if (*p >= 0xC0) { *code = (((*p & 0x1F) << 6) | (p[1] & 0x3F)); return 2; }
  *code = *p; return 1;
}
static const char* firstChar(const RadioStation* s) {
  static char oc[4];
  if (!s || !s->name[0]) return "?";
  const uint8_t* p = (const uint8_t*)s->name;
  uint16_t code; int n = utf8_next(p, &code);
  if (n > 3) n = 3;
  strlcpy(oc, (const char*)p, 4);   // copy the first UTF-8 char + NUL
  return oc;
}
static const char* stationCatLabel(const RadioStation* s) {
  if (!s) return "";
  if (s->cat[0] && strcmp(s->cat, "其它") != 0) return s->cat;   // 真实分类
  return (s->freq[0]) ? s->freq : "";                              // 无分类 -> 频率 (与 preview _station_cat 一致)
}
// 圆角方形图标 (按分类取色) + 名首字 + 下方小字(分类名, 10px) -- 镜像 preview _draw_station_icon
void drawStationIcon(int cx, int cy, RadioStation* st, int size) {
  if (!st) return;
  int half = size / 2;
  const char* catl = stationCatLabel(st);
  uint16_t col = catColor565(catl);
  tft.fillRoundRect(cx - half, cy - half, size, size, 6, col);
  const char* ch = firstChar(st);
  int cw = font16.textWidth(ch);
  font16.drawStringFast(cx - cw / 2, (cy - 1) - 8, ch, TFT_WHITE, col);
  // 下方小字 (最多 4 字, 10px, 居中) -- 硬件用 drawScaledChar(16->10 面积平均, 带 AA)
  char lbl[12]; int nl = 0;
  { uint16_t cc; const uint8_t* pc = (const uint8_t*)catl; while (*pc && nl < 4) { int n = utf8_next(pc, &cc); memcpy(lbl + nl, pc, n); nl += n; pc += n; } lbl[nl] = 0; }
  int nch = 0; { uint16_t cc; const uint8_t* q = (const uint8_t*)lbl; while (*q) { nch++; q += utf8_next(q, &cc); } }
  int wpx = nch * 10, lx = cx - wpx / 2, ly = (cy + half + 3) - 5;
  const uint8_t* rc = (const uint8_t*)lbl;
  for (int i = 0; i < nch; i++) {
    uint16_t code; int n = utf8_next(rc, &code);
    font16.drawScaledChar(lx + i * 10, ly, code, 10, COLOR_DIM, 0x0821);
    rc += n;
  }
}
void drawStationRow(int rowY, RadioStation* st, bool playing, bool favorited) {
  if (!st) return;
  int listX = RADIO_LIST_X;
  uint16_t bg = playing ? 0x1843 : COLOR_BG;
  tft.fillRect(listX, rowY, RADIO_LIST_W, RADIO_ROW_H, bg);
  // 圆角方形图标 (居中)
  drawStationIcon(listX + 24, rowY + RADIO_ROW_H / 2, st, 20);
  // 电台名 (上) + 频率·地区 (下) -- font16 去锯齿
  int nameX = listX + 46;
  font16.drawStringFast(nameX, rowY + 4, st->name, TFT_WHITE, bg);
  char info[40];
  snprintf(info, sizeof(info), "%s · %s", st->freq, regionNameOf(st));
  font16.drawStringFast(nameX, rowY + 28, info, COLOR_DIM, bg);
  // 右侧: 播放中=绿三角 / 否则爱心
  if (playing) {
    tft.fillTriangle(listX + RADIO_LIST_W - 14, rowY + 14,
                     listX + RADIO_LIST_W - 6, rowY + 10,
                     listX + RADIO_LIST_W - 6, rowY + 18, TFT_GREEN);
  } else {
    uint16_t hc = favorited ? TFT_RED : COLOR_DIM;
    int ax = listX + RADIO_LIST_W - 14;
    int ay = rowY + RADIO_ROW_H / 2 - 2;
    tft.fillCircle(ax - 4, ay - 2, 4, hc);
    tft.fillCircle(ax + 4, ay - 2, 4, hc);
    tft.fillTriangle(ax - 8, ay, ax + 8, ay, ax, ay + 8, hc);
  }
  tft.drawFastHLine(listX, rowY + RADIO_ROW_H - 1, RADIO_LIST_W, 0x2104);
}
void drawRadioList() {
  int listX = RADIO_LIST_X, listY = RADIO_LIST_TOP;
  int listH = RADIO_LIST_BOT - RADIO_LIST_TOP;
  tft.fillRect(listX, listY, RADIO_LIST_W, listH, COLOR_BG);
  int count = curCount();
  if (count == 0) {
    font16.drawStringFast(listX + 50, listY + 50, "暂无电台", TFT_RED, COLOR_BG);
    return;
  }
  int startRow = listScrollTop;
  int endRow = startRow + RADIO_STATION_ROWS;
  if (endRow > count) endRow = count;
  for (int i = startRow; i < endRow; i++) {
    int rowY = listY + (i - startRow) * RADIO_ROW_H;
    bool playing = (curSidAt(i) == currentPlayingSid && radioPlaying);
    bool favorited = isFavSid(curSidAt(i));
    drawStationRow(rowY, curStnAt(i), playing, favorited);
  }
  drawRadioScrollbar();
}
// 播放栏控件行 (≡ / 上一 / 播放暂停 / 下一 / 定时关闭 / 时间) -- 镜像 preview _draw_playbar_controls
static void drawPlaybarControls(int barX, int cy, bool playing) {
  int cx0 = barX + RADIO_LIST_W / 2;
  // 播放列表 (≡)
  tft.drawFastHLine(barX + 12, cy - 4, 12, TFT_WHITE);
  tft.drawFastHLine(barX + 12, cy,     12, TFT_WHITE);
  tft.drawFastHLine(barX + 12, cy + 4, 12, TFT_WHITE);
  // 上一曲 ◀| (左指三角 + 左竖线)
  int px = cx0 - 56;
  tft.fillTriangle(px + 18, cy - 6, px + 18, cy + 6, px + 8, cy, TFT_WHITE);
  tft.fillRect(px + 5, cy - 6, 3, 12, TFT_WHITE);
  // 播放/暂停 (大圆按钮, 恒绿, 白色图标)
  uint16_t ppBg = 0x07E0;
  tft.fillCircle(cx0, cy, 15, ppBg);
  if (playing) {
    tft.fillRect(cx0 - 6, cy - 6, 3, 12, TFT_WHITE);
    tft.fillRect(cx0 + 2, cy - 6, 3, 12, TFT_WHITE);
  } else {
    tft.fillTriangle(cx0 - 4, cy - 7, cx0 + 6, cy, cx0 - 4, cy + 7, TFT_WHITE);
  }
  // 下一曲 ▶|
  int nx = cx0 + 38;
  tft.fillTriangle(nx, cy - 6, nx, cy + 6, nx + 10, cy, TFT_WHITE);
  tft.fillRect(nx + 10, cy - 6, 3, 12, TFT_WHITE);
  // 定时关闭 (16px) + 时间 (右侧)
  font16.drawStringFast(cx0 + 55, cy - 8, "定时关闭", COLOR_DIM, 0x0821);
  font16.drawStringFast(barX + RADIO_LIST_W - 6 - font16.textWidth("00:00 / 00:00"), cy - 8, "00:00 / 00:00", COLOR_DIM, 0x0821);
}
void drawRadioPlayBar() {
  int barX = RADIO_LIST_X, barY = RADIO_LIST_BOT;
  int barW = RADIO_LIST_W, barH = RADIO_PLAYBAR_H;
  tft.fillRect(barX, barY, barW, barH, 0x0821);
  tft.drawFastHLine(barX, barY, barW, 0x3186);
  RadioStation* st = (currentPlayingSid >= 0) ? stn(currentPlayingSid) : nullptr;
  if (!st) {
    // 未播放: 仅控件行 + 居中提示
    drawPlaybarControls(barX, barY + barH / 2 - 14, false);
    font16.drawStringFast(barX + 44, barY + 8, "点击电台开始播放", COLOR_DIM, 0x0821);
    return;
  }
  // 行1: 图标(18) + 电台名(左, 截断) + "线路1 分类"(右)
  drawStationIcon(barX + 18, barY + 18, st, 18);
  const char* catl = stationCatLabel(st);
  char right_lbl[32];
  snprintf(right_lbl, sizeof(right_lbl), "线路1 %s", catl);
  int nameX = barX + 38;
  int rightW = font16.textWidth(right_lbl);
  int nameMaxw = (barX + barW - 10) - rightW - 8 - nameX;
  // 截断名到 nameMaxw + "…" (镜像 preview _fit)
  String nm = String(st->name);
  if (font16.textWidth(nm.c_str()) > nameMaxw) {
    while (nm.length() > 1 && font16.textWidth((nm + "…").c_str()) > nameMaxw) nm = nm.substring(0, nm.length() - 1);
    nm += "…";
  }
  font16.drawStringFast(nameX, barY + 10, nm, TFT_WHITE, 0x0821);
  font16.drawStringFast(barX + barW - 10 - rightW, barY + 10, right_lbl, COLOR_DIM, 0x0821);
  // 行2: 进度条 (占位, 40%)
  tft.fillRect(barX + 4, barY + 34, barW - 9, 2, 0x1082);
  tft.fillRect(barX + 4, barY + 34, (barW - 9) * 0.4, 2, TFT_GREEN);
  // 行3: 控件
  drawPlaybarControls(barX, barY + 52, radioPlaying);
}
void drawStationFullscreen() {
  int cx = 240, cy = 140;
  tft.fillRect(0, 0, 480, NAV_BAR_Y, 0x8000);
  RadioStation* st = (currentPlayingSid >= 0) ? stn(currentPlayingSid) : nullptr;
  if (!st) return;
  tft.fillRoundRect(60, 40, 360, 200, 8, 0x1042);
  // 大圆角方形图标 (按分类取色)
  uint16_t col = catColor565(stationCatLabel(st));
  tft.fillRoundRect(cx - 40, 40, 80, 80, 12, col);
  const char* ch = firstChar(st);
  font16.drawStringFast(cx - font16.textWidth(ch) / 2, 72, ch, TFT_WHITE, col);
  // 电台名 (16px 居中, 去锯齿)
  font16.drawStringFast(cx - font16.textWidth(st->name) / 2, 122, st->name, TFT_WHITE, 0x1042);
  // 详细信息
  char info[64];
  snprintf(info, sizeof(info), "%s · %s", st->freq, regionNameOf(st));
  font16.drawStringFast(cx - font16.textWidth(info) / 2, 157, info, COLOR_DIM, 0x1042);
  // 收藏
  bool fav = isFavSid(currentPlayingSid);
  font16.drawStringFast(cx - font16.textWidth(fav ? "★ 已收藏" : "☆ 收藏") / 2, 187, fav ? "★ 已收藏" : "☆ 收藏", fav ? TFT_YELLOW : COLOR_DIM, 0x1042);
  font16.drawStringFast(cx - 48, 217, "点屏幕任意处关闭", COLOR_DIM, 0x1042);
}
void drawRadioScrollbar() {
  int listH = RADIO_LIST_BOT - RADIO_LIST_TOP;
  int count = curCount();
  tft.fillRect(RADIO_LIST_X + RADIO_LIST_W, RADIO_LIST_TOP, RADIO_SCROLLBAR_W, listH, 0x1082);
  if (count > 0) {
    int scrollRange = max(1, count - RADIO_STATION_ROWS);
    int thumbH = max(20, listH * RADIO_STATION_ROWS / count);
    int thumbY = RADIO_LIST_TOP;
    if (scrollRange > 0) thumbY = RADIO_LIST_TOP + (listH - thumbH) * listScrollTop / scrollRange;
    tft.fillRect(RADIO_LIST_X + RADIO_LIST_W + 1, thumbY, RADIO_SCROLLBAR_W - 2, thumbH, TFT_WHITE);
  }
}
