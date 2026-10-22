#include "ui_common.h"

// ============================================================================
// 收音机 UI -- 2026-10-20 按 doubao app_17g7jt0w5v1 设计稿重构 (浅色主题)
//   状态栏 24px: 网络收音机 | WiFi | 音量 | 时间
//   左列 150px:  全国·中央·国际·海外(固定4) + 省市 + 其它/伴音/收藏/历史/个人
//   右列 330px:  分类小标题 + 电台行 48px (直播点/星标/正在播放态)
//   播放条 48px: 迷你均衡器 + 台名/状态 + 音量(点按静音) + 播放/暂停圆钮
//   全屏:        台名/频率/地区/类别 + L·R 各 30 段 7 彩 VU 表(峰值余晖)
// ============================================================================

int radioFailCnt = 0;        // 起播连接失败计数 (3 次彻底停)
uint32_t radioWasRunAt = 0;  // watchdog timer base

void updateSpectrum();    // 全屏 VU 表刷新 (后文定义)
void refreshMiniEq();     // 播放条均衡器动画 (后文定义)

// ===== 设计稿浅色配色 =====
#define RGBC(r,g,b) ((((r)&0xF8)<<8)|(((g)&0xFC)<<3)|((b)>>3))
static const uint16_t C_BG     = RGBC(0xF5,0xF7,0xFA);   // 页面底
static const uint16_t C_CARD   = RGBC(0xFF,0xFF,0xFF);   // 卡片/列表
static const uint16_t C_SIDE   = RGBC(0xEC,0xF0,0xF4);   // 左列底
static const uint16_t C_TXT    = RGBC(0x1F,0x29,0x37);   // 主文字
static const uint16_t C_DIM    = RGBC(0x6B,0x72,0x80);   // 次文字
static const uint16_t C_LINE   = RGBC(0xE5,0xE7,0xEB);   // 分隔线
static const uint16_t C_ACC    = RGBC(0x0E,0x97,0xC4);   // 主强调
static const uint16_t C_ACCBG  = RGBC(0xE1,0xF3,0xFA);   // 选中浅底
static const uint16_t C_FAV    = RGBC(0xE3,0x9A,0x1B);   // 收藏星
static const uint16_t C_LIVE   = RGBC(0xE5,0x48,0x4D);   // 直播点
static const uint16_t C_THUMB  = RGBC(0xC4,0xCC,0xD4);   // 滚动条
static const uint16_t C_WIFI   = RGBC(0x2E,0x8B,0x57);   // WiFi 绿

// ===== VU 采样 (ESP32-audioI2S v4 弱符号覆盖) =====
// audio task 在 I2S 输出前回调; int32 交错立体声, 16bit PCM 在高半字
// (库内 sampleToVU 用 mag>>23 归一 0..255 可证)。UI 消费后清零。
uint16_t g_vuL = 0, g_vuR = 0;
void audio_process_i2s(int32_t* outBuff, int16_t validSamples, bool* continueI2S) {
  (void)continueI2S;
  if (!radioPlaying) return;
  int32_t pl = 0, pr = 0;
  for (int i = 0; i + 1 < validSamples; i += 2) {
    int32_t l = outBuff[i], r = outBuff[i + 1];
    if (l < 0) l = -l;
    if (r < 0) r = -r;
    if (l > pl) pl = l;
    if (r > pr) pr = r;
  }
  pl >>= 16; pr >>= 16;
  if (pl > 32767) pl = 32767;
  if (pr > 32767) pr = 32767;
  if (pl > (int32_t)g_vuL) g_vuL = (uint16_t)pl;
  if (pr > (int32_t)g_vuR) g_vuR = (uint16_t)pr;
}

void pumpRadio() {
  if (audio.isRunning()) radioWasRunAt = millis();
  else if (millis() - radioWasRunAt > 8000) {
    stopRadio();
    Serial.println("[RADIO] stream dead >8s -> stopped");
    return;
  }
  // BUILD16 watchdog: "connected but silent" (isRunning true 但 30s 无解码输出 = 毒流)
  if (millis() - radioWasRunAt > 30000 && audio.getAudioCurrentTime() == 0) {
    stopRadio();
    Serial.println("[RADIO] no audio output >30s after connect -> stopped");
    return;
  }
}

void modeRadio() {
  if (!radioInited) return;

  // 延迟起播刷入: 双击窗口(RADIO_DBL_MS)已关闭且未进入全屏 -> 真正连流
  if (pendingPlaySid >= 0 && !showingFullscreen && millis() - pendingPlayAt >= RADIO_PLAY_FLUSH_MS) {
    int sid = pendingPlaySid; pendingPlaySid = -1;
    RadioStation* s = stn(sid);
    if (s) {
      String url = readURLForSid(sid);
      if (url.length() > 0) {
        radioPlaying = false;   // 先停 netTask 泵（避免切台与 loop 并发）
        audio.stopSong();
        Serial.printf("[RADIO] heap free=%u max=%u url=%s\n",
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(), url.c_str());
        Serial.printf("Playing: %s\n", s->name);
        bool ok = audio.connecttohost(url.c_str());
        if (ok) {
          radioPlaying = true; currentPlayingSid = sid;
          radioFailCnt = 0;
          radioWasRunAt = millis();
          radioPushHist(sid);              // 设计稿: 历史自动记录 (含收藏分类重建)
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
        }
        listScrollDirty = true;
      }
    }
  }

  if (showingFullscreen) {
    if (!modeDrawn[MODE_RADIO]) { drawStationFullscreen(); modeDrawn[MODE_RADIO] = true; }
    updateSpectrum();
    if (radioPlaying) pumpRadio();
    return;
  }

  if (!modeDrawn[MODE_RADIO] || modeChanged) {
    tft.fillRect(0, 0, 480, NAV_BAR_Y, C_BG);
    tft.startWrite();
    drawRadioHeader(); drawRadioRegionSidebar(); drawRadioList(); drawRadioPlayBar();
    tft.endWrite();
    modeDrawn[MODE_RADIO] = true; listScrollDirty = false; sideScrollDirty = false;
  }

  // 状态栏时间: 分钟变化局部重绘 (24px 条, 开销极小)
  static int s_lastMin = -1;
  time_t tn = time(nullptr); struct tm tmv; localtime_r(&tn, &tmv);
  if (tmv.tm_min != s_lastMin) { s_lastMin = tmv.tm_min; drawRadioHeader(); }

  // 2026-09-27: 侧栏/列表局部刷新, startWrite/endWrite 包整段 (硬件反模式 R1)
  if (sideScrollDirty) { tft.startWrite(); drawRadioRegionSidebar(); tft.endWrite(); sideScrollDirty = false; }
  if (listScrollDirty) { tft.startWrite(); drawRadioList(); drawRadioPlayBar(); tft.endWrite(); listScrollDirty = false; }

  // 播放条迷你均衡器动画 (~8fps, 只刷 40x30 小区)
  static unsigned long s_eqLast = 0;
  if (radioPlaying && millis() - s_eqLast >= 120) {
    s_eqLast = millis();
    tft.startWrite(); refreshMiniEq(); tft.endWrite();
  }

  if (radioPlaying) pumpRadio();
}

// ============================================================
// 数据 helpers (与 make_preview.load_radio 逐站逐序一致)
// ============================================================
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

static int utf8_next(const uint8_t* p, uint16_t* code) {
  if (*p >= 0xF0) { *code = (((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F)); return 4; }
  if (*p >= 0xE0) { *code = (((*p & 0x0F) << 6) | (p[1] & 0x3F)); return 3; }
  if (*p >= 0xC0) { *code = (((*p & 0x1F) << 6) | (p[1] & 0x3F)); return 2; }
  *code = *p; return 1;
}
static const char* firstChar(const RadioStation* s) {
  static char oc[4];
  if (!s || !s->name[0]) return "?";
  strlcpy(oc, (const char*)s->name, 4);   // 首个 UTF-8 字符 + NUL
  return oc;
}
static const char* stationCatLabel(const RadioStation* s) {
  if (!s) return "";
  if (s->cat[0] && strcmp(s->cat, "其它") != 0) return s->cat;   // 真实分类
  return (s->freq[0]) ? s->freq : "";                              // 无分类 -> 频率
}
// 截断到 maxw + "…"
static void fitText(String& s, int maxw) {
  if (font16.textWidth(s.c_str()) <= maxw) return;
  while (s.length() > 1 && font16.textWidth((s + "…").c_str()) > maxw) s = s.substring(0, s.length() - 1);
  s += "…";
}

// ============================================================
// 触摸处理
// ============================================================
void handleRadioTap(uint16_t tx, uint16_t ty) {
  // ---- 全屏: 收藏chip / 返回chip / 任意处返回列表 ----
  if (showingFullscreen) {
    if (currentPlayingSid >= 0 && tx >= 396 && tx <= 476 && ty >= 4 && ty <= 38) {
      radioToggleFav(currentPlayingSid);
      modeDrawn[MODE_RADIO] = false; return;          // 重绘全屏刷新收藏chip
    }
    showingFullscreen = false;
    modeDrawn[MODE_RADIO] = false;
    return;
  }
  if (ty < RADIO_HEADER_H) return;                    // 状态栏
  // ---- 左列分类 (整列滚动: 固定4行 + 地区行, regionScrollTop = 像素偏移) ----
  if (tx < RADIO_SIDEBAR_W) {
    int S = regionScrollTop;
    int top = RADIO_HEADER_H - S;
    int boundary = RADIO_REGION_TOP - S;
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
  // ---- 播放条: 播放/暂停圆钮 | 音量(静音) | 台名/均衡器(进全屏) ----
  if (ty >= RADIO_LIST_BOT && ty < NAV_BAR_Y) {
    int x = RADIO_LIST_X;
    if (tx >= x + RADIO_LIST_W - 44) {                // 圆钮
      if (radioPlaying) radioPause();
      else if (currentPlayingSid >= 0) playStationAt(currentPlayingSid);
      else if (curCount() > 0) playStationAt(curSidAt(0));
      modeDrawn[MODE_RADIO] = false;
    } else if (tx >= x + RADIO_LIST_W - 108) {        // 音量区 -> 静音切换
      radioToggleMute();
      tft.startWrite(); drawRadioPlayBar(); tft.endWrite();
    } else if (currentPlayingSid >= 0) {              // 台名/均衡器 -> 全屏
      showingFullscreen = true; modeDrawn[MODE_RADIO] = false;
    }
    return;
  }
  // ---- 列表区 ----
  if (ty >= RADIO_LIST_TOP && ty < RADIO_LIST_BOT && tx >= RADIO_LIST_X) {
    int rowIdx = listScrollTop + (ty - RADIO_LIST_TOP) / RADIO_ROW_H;
    if (rowIdx < 0 || rowIdx >= curCount()) return;
    // 星标命中区 (行右缘 36px): 收藏/取消, 收藏分类下整列刷新(行会消失)
    if (tx >= RADIO_LIST_X + RADIO_LIST_W - 36) {
      bool inFavCat = (currentCat < radioCatCount) && strcmp(radioCats[currentCat].key, RLBL[1]) == 0;
      radioToggleFav(curSidAt(rowIdx));
      if (inFavCat) { listScrollTop = 0; modeDrawn[MODE_RADIO] = false; }
      else { tft.startWrite(); drawRadioList(); drawRadioPlayBar(); tft.endWrite(); }
      lastTapTime = 0;
      return;
    }
    // 单击播放(延迟起播保双击窗口), 双击全屏
    unsigned long now = millis();
    if (now - lastTapTime < RADIO_DBL_MS && lastTapRow == rowIdx) {
      showingFullscreen = true; pendingPlaySid = -1; modeDrawn[MODE_RADIO] = false;   // 双击 -> 全屏, 取消延迟起播
      Serial.printf("[DBG-T] DOUBLETAP row=%d dt=%lu -> fullscreen\n", rowIdx, now - lastTapTime);
    } else {
      playStationAt(curSidAt(rowIdx));
      tft.startWrite(); drawRadioList(); drawRadioPlayBar(); tft.endWrite();
    }
    lastTapTime = now; lastTapRow = rowIdx;
  }
}

// ============================================================
// 绘制: 状态栏
// ============================================================
void drawRadioHeader() {
  tft.fillRect(0, 0, 480, RADIO_HEADER_H, C_CARD);
  font16.drawString(6, 4, "网络收音机", C_TXT, C_CARD);
  tft.fillRect(6 + font16.textWidth("网络收音机") + 5, 6, 3, 12, C_ACC);   // 设计稿光标点缀
  char buf[12];
  time_t tn = time(nullptr); struct tm tmv; localtime_r(&tn, &tmv);
  snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
  int x = 474 - font16.textWidth(buf);
  font16.drawString(x, 4, buf, C_TXT, C_CARD);
  char vb[10];
  if (radioMuted) strcpy(vb, "静音"); else snprintf(vb, sizeof(vb), "%d%%", radioVolume * 100 / 21);
  x -= (font16.textWidth(vb) + 10);
  font16.drawString(x, 4, vb, radioMuted ? C_LIVE : C_DIM, C_CARD);
  const char* wt = wifiConnected ? "WiFi" : "离线";
  x -= (font16.textWidth(wt) + 10);
  font16.drawString(x, 4, wt, wifiConnected ? C_WIFI : C_LIVE, C_CARD);
  tft.drawFastHLine(0, RADIO_HEADER_H, 480, C_LINE);
}

// ============================================================
// 绘制: 左列分类 (整列滚动, 像素级; 裁剪到视口 24..287)
// ============================================================
void drawRadioRegionSidebar() {
  tft.fillRect(0, RADIO_HEADER_H, RADIO_SIDEBAR_W, NAV_BAR_Y - RADIO_HEADER_H, C_SIDE);
  int S = regionScrollTop;
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
    uint16_t bg = sel ? C_ACCBG : C_SIDE;
    uint16_t fg = sel ? C_ACC : C_DIM;
    tft.fillRect(0, y0, RADIO_SIDEBAR_W - 2, fy1 - y0 + 1, bg);
    if (sel) tft.fillRect(0, y0, 3, fy1 - y0 + 1, C_ACC);
    int tyy = ry + (rh - 16) / 2;
    if (tyy >= y0 && tyy + 15 <= fy1)
      font16.drawString(10, tyy, catLabel(i), fg, bg);
  }
  // 侧栏滚动条 (右缘 3px)
  int totalH = RADIO_SPECIAL_CNT * RADIO_SPECIAL_H + max(0, radioCatCount - RADIO_SPECIAL_CNT) * RADIO_ROW_H_SIDE;
  int viewH = NAV_BAR_Y - RADIO_HEADER_H;
  if (totalH > viewH) {
    int thumbH = max(14, viewH * viewH / totalH);
    int smax = totalH - viewH;
    int thumbY = RADIO_HEADER_H + (viewH - thumbH) * regionScrollTop / smax;
    tft.fillRect(RADIO_SIDEBAR_W - 5, thumbY, 3, thumbH, C_THUMB);
  }
  tft.drawFastVLine(RADIO_SIDEBAR_W - 1, RADIO_HEADER_H, NAV_BAR_Y - RADIO_HEADER_H, C_LINE);
}

// ============================================================
// 绘制: 右列小标题 + 电台列表
// ============================================================
static void drawRadioSubHeader() {
  int x = RADIO_LIST_X, w = RADIO_LIST_W;
  tft.fillRect(x, RADIO_HEADER_H, w, RADIO_SUBHDR_H, C_CARD);
  font16.drawStringFast(x + 10, RADIO_HEADER_H + 4, catLabel(currentCat), C_TXT, C_CARD);
  char c[16]; snprintf(c, sizeof(c), "%d 台", curCount());
  int cw = font16.textWidth(c);
  font16.drawStringFast(x + w - 10 - cw, RADIO_HEADER_H + 4, c, C_DIM, C_CARD);
  tft.drawFastHLine(x, RADIO_LIST_TOP - 1, w, C_LINE);
}

void drawStationRow(int rowY, RadioStation* st, bool playing, bool favorited) {
  if (!st) return;
  int x = RADIO_LIST_X, w = RADIO_LIST_W;
  uint16_t bg = playing ? C_ACCBG : C_CARD;
  tft.fillRect(x, rowY, w, RADIO_ROW_H, bg);
  // 直播点 (播放中, 设计稿 #E5484D)
  int tx = x + 12;
  if (playing) { tft.fillCircle(x + 13, rowY + 14, 3, C_LIVE); tx = x + 24; }
  // 台名 (上) + 分类·地区 (下, 12px 缩放小字)
  font16.drawStringFast(tx, rowY + 6, st->name, C_TXT, bg);
  char info[52];
  const char* catl = stationCatLabel(st);
  if (playing) snprintf(info, sizeof(info), "正在播放 · %s · %s", catl, regionNameOf(st));
  else         snprintf(info, sizeof(info), "%s · %s", catl, regionNameOf(st));
  String inf = info; fitText(inf, w - 36 - tx);
  almScaledText(tx, rowY + 28, inf, 12, playing ? C_ACC : C_DIM, bg, false);
  // 星标 (右缘, 收藏 #E39A1B)
  font16.drawStringFast(x + w - 28, rowY + 5, favorited ? "★" : "☆", favorited ? C_FAV : C_DIM, bg);
  tft.drawFastHLine(x, rowY + RADIO_ROW_H - 1, w, C_LINE);
}

void drawRadioList() {
  drawRadioSubHeader();
  int x = RADIO_LIST_X, y = RADIO_LIST_TOP, w = RADIO_LIST_W, h = RADIO_LIST_H;
  tft.fillRect(x, y, w, h, C_CARD);
  int count = curCount();
  if (count == 0) {
    font16.drawStringFast(x + w / 2 - 56, y + h / 2 - 8, "该分类暂无电台", C_DIM, C_CARD);
    return;
  }
  int startRow = listScrollTop;
  int endRow = startRow + RADIO_STATION_ROWS;
  if (endRow > count) endRow = count;
  for (int i = startRow; i < endRow; i++) {
    drawStationRow(y + (i - startRow) * RADIO_ROW_H, curStnAt(i),
                   curSidAt(i) == currentPlayingSid && radioPlaying, isFavSid(curSidAt(i)));
  }
  drawRadioScrollbar();
}

void drawRadioScrollbar() {
  int count = curCount();
  if (count <= RADIO_STATION_ROWS) return;
  int listH = RADIO_LIST_H;
  int thumbH = max(24, listH * RADIO_STATION_ROWS / count);
  int scrollRange = count - RADIO_STATION_ROWS;
  int thumbY = RADIO_LIST_TOP + (listH - thumbH) * listScrollTop / scrollRange;
  tft.fillRect(RADIO_LIST_X + RADIO_LIST_W - 4, thumbY, 3, thumbH, C_THUMB);
}

// ============================================================
// 绘制: 播放条 (均衡器 + 台名/状态 + 音量 + 播放/暂停)
// ============================================================
static void drawMiniEqBars(int bx, int baseY, bool playing) {
  for (int i = 0; i < 4; i++) tft.fillRect(bx + i * 9, baseY - 3, 6, 3, playing ? C_ACC : C_DIM);
}
void refreshMiniEq() {   // modeRadio ~120ms 调用, 只刷均衡器小区
  int bx = RADIO_LIST_X + 8, baseY = RADIO_LIST_BOT + RADIO_PLAYBAR_H - 8;
  tft.fillRect(bx - 2, baseY - 28, 42, 30, C_CARD);
  static float s_eq = 0; static int s_ph = 0; s_ph++;
  float tgt = (g_vuL + g_vuR) / 56000.0f; if (tgt > 1) tgt = 1;
  g_vuL = 0; g_vuR = 0;
  s_eq = (tgt > s_eq) ? tgt : s_eq * 0.82f;
  for (int i = 0; i < 4; i++) {
    int h = 3 + (int)(s_eq * (10 + ((i * 5 + s_ph * 3) % 12)));
    if (h > 26) h = 26;
    tft.fillRect(bx + i * 9, baseY - h, 6, h, C_ACC);
  }
}
static void drawVolumeCtl(int x, int y, int h) {
  int cy = y + h / 2;
  tft.fillRect(x, y + 1, 60, h - 2, C_CARD);
  uint16_t col = radioMuted ? C_DIM : C_ACC;
  tft.fillRect(x, cy - 4, 4, 8, col);
  tft.fillTriangle(x + 4, cy, x + 10, cy - 7, x + 10, cy + 7, col);
  if (!radioMuted) {   // 两道声波 (折线近似弧)
    tft.drawLine(x + 13, cy - 5, x + 15, cy - 2, col); tft.drawLine(x + 15, cy - 2, x + 15, cy + 2, col); tft.drawLine(x + 15, cy + 2, x + 13, cy + 5, col);
    tft.drawLine(x + 18, cy - 8, x + 21, cy - 2, col); tft.drawLine(x + 21, cy - 2, x + 21, cy + 2, col); tft.drawLine(x + 21, cy + 2, x + 18, cy + 8, col);
  } else {             // 静音斜杠
    tft.drawLine(x + 12, cy - 7, x + 22, cy + 7, C_LIVE);
  }
  char vb[10];
  if (radioMuted) strcpy(vb, "静音"); else snprintf(vb, sizeof(vb), "%d%%", radioVolume * 100 / 21);
  almScaledText(x + 26, cy - 6, vb, 12, radioMuted ? C_LIVE : C_DIM, C_CARD, false);
}
void drawRadioPlayBar() {
  int x = RADIO_LIST_X, y = RADIO_LIST_BOT, w = RADIO_LIST_W, h = RADIO_PLAYBAR_H;
  tft.fillRect(x, y, w, h, C_CARD);
  tft.drawFastHLine(x, y, w, C_LINE);
  drawMiniEqBars(x + 8, y + h - 8, radioPlaying);
  RadioStation* st = (currentPlayingSid >= 0) ? stn(currentPlayingSid) : nullptr;
  int nx = x + 54;
  int volX = x + w - 108;
  if (st) {
    String nm(st->name); fitText(nm, volX - nx - 8);
    font16.drawStringFast(nx, y + 4, nm.c_str(), C_TXT, C_CARD);
    almScaledText(nx, y + 27, radioPlaying ? "正在播放" : "已暂停", 12, radioPlaying ? C_ACC : C_DIM, C_CARD, false);
  } else {
    font16.drawStringFast(nx, y + 7, "点击电台开始播放", C_DIM, C_CARD);
    almScaledText(nx, y + 29, "左列选分类 · 右列点电台", 12, C_DIM, C_CARD, false);
  }
  drawVolumeCtl(volX, y, h);
  int cx = x + w - 24, cy = y + h / 2;
  tft.fillCircle(cx, cy, 17, C_ACC);
  if (radioPlaying) {
    tft.fillRect(cx - 5, cy - 6, 4, 12, TFT_WHITE);
    tft.fillRect(cx + 2, cy - 6, 4, 12, TFT_WHITE);
  } else {
    tft.fillTriangle(cx - 4, cy - 7, cx + 7, cy, cx - 4, cy + 7, TFT_WHITE);
  }
}

// ============================================================
// 全屏播放视图 + L·R 30 段 7 彩 VU 表 (30ms 级刷新, 峰值余晖)
// ============================================================
#define FS_SEG_N      30
#define FS_SEG_W      5
#define FS_SEG_PITCH  7
#define FS_BASE_Y     258
#define FS_MAX_H      66
static const uint16_t FS_SEGCOLOR[7] = {   // 青·绿·黄绿·黄·橙·红·紫
  RGBC(0x00,0xBD,0xD4), RGBC(0x4C,0xAF,0x50), RGBC(0x8B,0xC3,0x4A), RGBC(0xFF,0xC1,0x07),
  RGBC(0xFF,0x98,0x00), RGBC(0xF4,0x43,0x36), RGBC(0x9C,0x27,0xB0) };
static int  s_litL = 0, s_litR = 0, s_peakL = 0, s_peakR = 0;
static float s_smL = 0, s_smR = 0;
static unsigned long s_peakTL = 0, s_peakTR = 0;
static int fsGroupX(int side) { return side == 0 ? 22 : 252; }
static void fsSegRect(int side, int i, uint16_t col) {   // 画/擦第 i 段 (i>=1)
  int x0 = fsGroupX(side);
  int h = i * FS_MAX_H / FS_SEG_N;
  tft.fillRect(x0 + (i - 1) * FS_SEG_PITCH, FS_BASE_Y - h, FS_SEG_W, h, col);
}
static void fsResetSpectrum() { s_litL = s_litR = s_peakL = s_peakR = 0; s_smL = s_smR = 0; s_peakTL = s_peakTR = 0; }

void updateSpectrum() {   // 仅全屏时由 modeRadio 调用 (~25fps 节流)
  static unsigned long s_tLast = 0;
  if (millis() - s_tLast < 40) return;
  s_tLast = millis();
  float tl = g_vuL / 28000.0f; g_vuL = 0; if (tl > 1) tl = 1; tl = sqrtf(tl);
  float tr = g_vuR / 28000.0f; g_vuR = 0; if (tr > 1) tr = 1; tr = sqrtf(tr);
  s_smL = (tl > s_smL) ? tl : s_smL - 0.05f; if (s_smL < 0) s_smL = 0;   // 攻击瞬时/释放缓
  s_smR = (tr > s_smR) ? tr : s_smR - 0.05f; if (s_smR < 0) s_smR = 0;
  int litL = (int)(s_smL * FS_SEG_N + 0.5f); if (litL > FS_SEG_N) litL = FS_SEG_N;
  int litR = (int)(s_smR * FS_SEG_N + 0.5f); if (litR > FS_SEG_N) litR = FS_SEG_N;
  tft.startWrite();
  for (int side = 0; side < 2; side++) {
    int lit = side ? litR : litL;
    int prev = side ? s_litR : s_litL;
    int lo = (lit < prev) ? lit : prev;
    int hi = (lit > prev) ? lit : prev;
    for (int i = lo + 1; i <= hi; i++)   // 段号 1..N
      fsSegRect(side, i, (i <= lit) ? FS_SEGCOLOR[(i - 1) * 7 / FS_SEG_N] : C_BG);
    // 峰值余晖点
    int& peak = side ? s_peakR : s_peakL;
    unsigned long& pt = side ? s_peakTR : s_peakTL;
    if (peak > 0) {
      int x0 = fsGroupX(side);
      int h = peak * FS_MAX_H / FS_SEG_N;
      tft.fillRect(x0 + (peak - 1) * FS_SEG_PITCH, FS_BASE_Y - h - 4, FS_SEG_W, 2, C_BG);
    }
    if (lit >= peak) { peak = lit; pt = millis(); }
    else if (millis() - pt > 600) { peak = (peak > lit + 1) ? peak - 1 : lit; }   // 保持 600ms 后逐帧降
    if (peak > 0) {
      int x0 = fsGroupX(side);
      int h = peak * FS_MAX_H / FS_SEG_N;
      tft.fillRect(x0 + (peak - 1) * FS_SEG_PITCH, FS_BASE_Y - h - 4, FS_SEG_W, 2, FS_SEGCOLOR[(peak - 1) * 7 / FS_SEG_N]);
    }
  }
  s_litL = litL; s_litR = litR;
  tft.endWrite();
}

static void drawFsFavChip() {
  bool fav = (currentPlayingSid >= 0) && isFavSid(currentPlayingSid);
  tft.fillRoundRect(400, 8, 72, 26, 13, C_CARD);
  almScaledText(436, 15, fav ? "★ 已收藏" : "☆ 收藏", 12, fav ? C_FAV : C_DIM, C_CARD, true);
}

void drawStationFullscreen() {
  tft.fillRect(0, 0, 480, NAV_BAR_Y, C_BG);
  RadioStation* st = (currentPlayingSid >= 0) ? stn(currentPlayingSid) : nullptr;
  if (!st) { showingFullscreen = false; return; }
  // 返回 chip (左上, 设计稿: 点左上角返回)
  tft.fillRoundRect(8, 8, 64, 26, 13, C_CARD);
  tft.drawLine(20, 21, 25, 16, C_TXT); tft.drawLine(20, 21, 25, 26, C_TXT);
  font16.drawStringFast(30, 13, "返回", C_TXT, C_CARD);
  drawFsFavChip();
  // 大圆角图标 (分类色 + 名首字)
  uint16_t col = catColor565(stationCatLabel(st));
  tft.fillRoundRect(204, 44, 72, 72, 14, col);
  const char* ch = firstChar(st);
  font16.drawStringFast(240 - font16.textWidth(ch) / 2, 72, ch, TFT_WHITE, col);
  // 台名 (22px, 超宽截断) + 信息行 (频率·地区·类别, 14px)
  String nm(st->name); fitText(nm, 440);
  almBigText(240, 124, nm, 22, C_TXT, C_BG, true);
  char info[72];
  snprintf(info, sizeof(info), "%s · %s · %s", st->freq[0] ? st->freq : "FM", regionNameOf(st), stationCatLabel(st));
  almScaledText(240, 152, info, 14, C_DIM, C_BG, true);
  // 播放状态 (直播点 + 正在播放/已暂停)
  const char* stx = radioPlaying ? "正在播放" : "已暂停";
  int sw = almScaledText(240, 172, stx, 12, radioPlaying ? C_ACC : C_DIM, C_BG, true);
  tft.fillCircle(240 - sw / 2 - 8, 178, 3, radioPlaying ? C_LIVE : C_DIM);
  // 频谱底座 + L/R 标签
  tft.drawFastHLine(18, FS_BASE_Y + 1, 446, C_LINE);
  almScaledText(6, 246, "L", 14, C_DIM, C_BG, false);
  almScaledText(236, 246, "R", 14, C_DIM, C_BG, false);
  fsResetSpectrum();
  almScaledText(240, 270, "点屏幕任意处返回列表", 12, C_DIM, C_BG, true);
}
