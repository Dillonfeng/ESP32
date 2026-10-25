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
static const uint16_t C_GROUP  = RGBC(0x9A,0xA5,0xB0);   // 左列分组标题 (2026-10-22, 弱化字色)
static const uint16_t C_WIFI   = RGBC(0x2E,0x8B,0x57);   // WiFi 绿

// ===== 全屏控制按钮 (2026-10-22 设计稿: − / 播放暂停 / +) =====
// 定义在此供 handleRadioTap 与 drawStationFullscreen 共用
#define FS_BTN_Y   200
#define FS_BTN_DN_X 168
#define FS_BTN_MAIN_X 226
#define FS_BTN_UP_X 284
#define FS_BTN_R   22
#define FS_BTN_MAIN_R 26
static unsigned long lastFsTap = 0;   // 全屏底部提示条双击返回判定

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
  time_t tn = time(nullptr); struct tm tmv; epochToLocalNoLock(tn, tmv);   // BUILDV5-TZFIX: 无锁
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

// ============ 左列分组行模型 (2026-10-22, 对齐设计稿 频道/省市/我的) ============
// 侧栏不再是"固定4行+地区行"的简单公式: 每组首个分类前插入一个组标题行(RADIO_GROUP_H)。
// sideRowTop(i) = 分类 i 的顶边 y 偏移 (相对 RADIO_HEADER_H, 未减 regionScrollTop)。
// 组标题行边: 若 i 是组首 (i==0 或 group 与前一个不同), 它的顶边前有 RADIO_GROUP_H 的标题行。
int sideRowTop(int i) {
  int y = 0;
  for (int k = 0; k < i; k++) {
    if (k == 0 || radioCats[k].group != radioCats[k - 1].group) y += RADIO_GROUP_H;   // 组首前的标题行
    y += (k < RADIO_SPECIAL_CNT) ? RADIO_SPECIAL_H : RADIO_ROW_H_SIDE;
  }
  if (i == 0 || radioCats[i].group != radioCats[i - 1].group) y += RADIO_GROUP_H;     // i 自身前的标题行
  return y;
}
// 侧栏总高 = 最后一个分类的底边 (2026-10-22: 修正旧版 sideRowTop(radioCatCount) 越界读残留槽)
int sideTotalH() {
  if (radioCatCount <= 0) return 0;
  int i = radioCatCount - 1;
  return sideRowTop(i) + ((i < RADIO_SPECIAL_CNT) ? RADIO_SPECIAL_H : RADIO_ROW_H_SIDE);
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
  strlcpy(oc, (const char*)s->name, 4);   // 首个 UTF-8 字符 + NUL
  return oc;
}
static const char* stationCatLabel(const RadioStation* s) {
  if (!s) return "";
  if (s->cat[0] && strcmp(s->cat, "其它") != 0) return s->cat;   // 真实分类
  return (s->freq[0]) ? s->freq : "";                              // 无分类 -> 频率
}
// 截断到 maxw + "…" (font 可指定, 默认 font16; 全屏大字用 font24 测宽)
static void fitText(String& s, int maxw, XFont& f = font16) {
  if (f.textWidth(s.c_str()) <= maxw) return;
  while (s.length() > 1 && f.textWidth((s + "…").c_str()) > maxw) s = s.substring(0, s.length() - 1);
  s += "…";
}

// ============================================================
// 触摸处理
// ============================================================
void handleRadioTap(uint16_t tx, uint16_t ty) {
  // ---- 全屏 (2026-10-22 对齐设计稿): 返回chip | 收藏星 | 控制按钮 | 双击底部提示条返回 ----
  if (showingFullscreen) {
    // 返回 chip (左上 8,6..72,32)
    if (tx >= 8 && tx <= 72 && ty >= 6 && ty <= 32) {
      showingFullscreen = false; modeDrawn[MODE_RADIO] = false; return;
    }
    // 收藏星 (右上 410,6..472,32)
    if (tx >= 410 && tx <= 472 && ty >= 6 && ty <= 32) {
      radioToggleFav(currentPlayingSid);
      modeDrawn[MODE_RADIO] = false; return;
    }
    // 控制按钮: 音量− | 播放/暂停 | 音量+ (中心 FS_BTN_Y=200)
    if (ty >= FS_BTN_Y - FS_BTN_MAIN_R - 4 && ty <= FS_BTN_Y + FS_BTN_MAIN_R + 4) {
      if (tx >= FS_BTN_DN_X - FS_BTN_R - 6 && tx <= FS_BTN_DN_X + FS_BTN_R + 6) {
        radioVolumeDelta(-1); modeDrawn[MODE_RADIO] = false; return;
      }
      if (tx >= FS_BTN_UP_X - FS_BTN_R - 6 && tx <= FS_BTN_UP_X + FS_BTN_R + 6) {
        radioVolumeDelta(1); modeDrawn[MODE_RADIO] = false; return;
      }
      if (tx >= FS_BTN_MAIN_X - FS_BTN_MAIN_R - 4 && tx <= FS_BTN_MAIN_X + FS_BTN_MAIN_R + 4) {
        if (radioPlaying) radioPause();
        else if (currentPlayingSid >= 0) playStationAt(currentPlayingSid);
        modeDrawn[MODE_RADIO] = false; return;
      }
    }
    // 底部提示条 (NAV_BAR_Y..NAV_BAR_Y+30): 双击返回 (设计稿 fs-hint)
    if (ty >= NAV_BAR_Y && ty < NAV_BAR_Y + 30) {
      unsigned long now = millis();
      if (now - lastFsTap < 300) { lastFsTap = 0; showingFullscreen = false; modeDrawn[MODE_RADIO] = false; }
      else lastFsTap = now;
      return;
    }
    return;   // 其余区域不响应 (设计稿: 只有控件可点)
  }
  if (ty < RADIO_HEADER_H) return;                    // 状态栏
  // ---- 左列分类 (整列滚动: 分组标题+分类行, regionScrollTop = 像素偏移) ----
  if (tx < RADIO_SIDEBAR_W) {
    int S = regionScrollTop;
    int top = RADIO_HEADER_H + SIDEBAR_HEAD_H - S;
    int idx = -1;
    for (int i = 0; i < radioCatCount; i++) {          // 组标题行不可点: 只比对分类行
      int ry = top + sideRowTop(i);
      int rh = (i < RADIO_SPECIAL_CNT) ? RADIO_SPECIAL_H : RADIO_ROW_H_SIDE;
      if (ty >= ry && ty < ry + rh) { idx = i; break; }
    }
    Serial.printf("[RADIO-TAP] tx=%d ty=%d S=%d idx=%d cur=%d\n", tx, ty, S, idx, currentCat);
    if (idx >= 0 && idx < radioCatCount && idx != currentCat) {
      currentCat = idx; listScrollTop = 0; modeDrawn[MODE_RADIO] = false;
      Serial.printf("[RADIO-TAP] switch to idx=%d\n", idx);
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
    // 2026-10-22 问题③: listScrollTop 像素偏移 -> 命中行号按像素反算
    int rowIdx = (listScrollTop + (ty - RADIO_LIST_TOP)) / RADIO_ROW_H;
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
  time_t tn = time(nullptr); struct tm tmv; epochToLocalNoLock(tn, tmv);   // BUILDV5-TZFIX: 无锁
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
// 侧栏分类图标 8x8 (设计稿 SVG 简化位图): 0全国 1中央 2国际 3海外 4省市 5收藏 6个人 7伴音 8助眠 9历史
static const uint8_t SIDE_ICON[10][8] = {
  {0x3C,0x42,0x99,0xA5,0xA5,0x99,0x42,0x3C},   // 0 全国: 地球(圆环+经纬)
  {0x08,0x1C,0x3E,0x7F,0x3E,0x14,0x36,0x63},   // 1 中央: 五角星(简化)
  {0x3C,0x42,0x99,0x81,0xFF,0x81,0x42,0x3C},   // 2 国际: 地球+横线
  {0x18,0x3C,0x7E,0x7E,0x3C,0x18,0x00,0x00},   // 3 海外: 水滴
  {0x18,0x3C,0x7E,0x7E,0x3C,0x18,0x18,0x18},   // 4 省市: 地图钉
  {0x08,0x1C,0x3E,0x7F,0x3E,0x14,0x36,0x63},   // 5 收藏: 星
  {0x0E,0x11,0x11,0x0E,0x00,0x1F,0x21,0x21},   // 6 个人: 人形
  {0x3C,0x42,0x42,0x42,0x42,0x42,0x3C,0x24},   // 7 伴音: 电视(含底座)
  {0x06,0x0F,0x09,0x09,0x09,0x09,0x00,0x00},   // 8 助眠: 音符
  {0x3C,0x42,0x42,0x5A,0x5A,0x42,0x42,0x3C}    // 9 历史: 时钟(带指针)
};
static void drawSideIcon(int x, int y, int icon, uint16_t fg, uint16_t bg) {
  (void)bg;
  if (icon < 0 || icon > 9) return;
  for (int r = 0; r < 8; r++) {
    uint8_t row = SIDE_ICON[icon][r];
    for (int c = 0; c < 8; c++)
      if (row & (0x80 >> c)) tft.drawPixel(x + c, y + r, fg);
  }
}
static int catIconIndex(const char* key, int grp) {
  if (strcmp(key, "全国") == 0) return 0;
  if (strcmp(key, "中央") == 0) return 1;
  if (strcmp(key, "国际") == 0) return 2;
  if (strcmp(key, "海外") == 0) return 3;
  if (grp == 1) return 4;                          // 省市 → 地图钉
  if (strcmp(key, "收藏") == 0) return 5;
  if (strcmp(key, "个人") == 0) return 6;
  if (strcmp(key, "电视伴音") == 0 || strcmp(key, "伴音") == 0) return 7;
  if (strcmp(key, "助眠") == 0) return 8;
  if (strcmp(key, "历史") == 0) return 9;
  return -1;
}

void drawRadioRegionSidebar() {
  tft.fillRect(0, RADIO_HEADER_H, RADIO_SIDEBAR_W, NAV_BAR_Y - RADIO_HEADER_H, C_SIDE);
  int S = regionScrollTop;
  int top = RADIO_HEADER_H + SIDEBAR_HEAD_H - S;
  // 固定头 "频道分类" (设计稿 lp-head 28px, 弱化字色) + 下分隔线
  font16.drawString(10, RADIO_HEADER_H + (SIDEBAR_HEAD_H - 16) / 2, "频道分类", C_DIM, C_SIDE);
  tft.drawFastHLine(0, RADIO_HEADER_H + SIDEBAR_HEAD_H - 1, RADIO_SIDEBAR_W - 1, C_LINE);
  // 组标题 (频道/省市/我的): 每组首个分类前一行, 弱化字色
  const char* GNAMES[RADIO_GROUP_CNT] = { "频道", "省市", "我的" };
  for (int i = 0; i < radioCatCount; i++) {
    bool grpHead = (i == 0) || (radioCats[i].group != radioCats[i - 1].group);
    int base = sideRowTop(i);
    if (grpHead) {
      int gy = top + base - RADIO_GROUP_H;
      int gy0 = (gy > RADIO_HEADER_H + SIDEBAR_HEAD_H) ? gy : RADIO_HEADER_H + SIDEBAR_HEAD_H;
      int gy1 = (gy + RADIO_GROUP_H - 1 < NAV_BAR_Y - 1) ? gy + RADIO_GROUP_H - 1 : NAV_BAR_Y - 1;
      if (gy1 >= gy0) {
        tft.fillRect(0, gy0, RADIO_SIDEBAR_W - 2, gy1 - gy0 + 1, C_SIDE);
        int g = radioCats[i].group;
        const char* gn = (g >= 0 && g < RADIO_GROUP_CNT) ? GNAMES[g] : "";
        font16.drawString(10, gy + (RADIO_GROUP_H - 16) / 2, gn, C_GROUP, C_SIDE);
      }
    }
    int rh = (i < RADIO_SPECIAL_CNT) ? RADIO_SPECIAL_H : RADIO_ROW_H_SIDE;
    int ry = top + base;
    int y0 = (ry > RADIO_HEADER_H + SIDEBAR_HEAD_H) ? ry : RADIO_HEADER_H + SIDEBAR_HEAD_H;
    int fy1 = (ry + rh - 1 < NAV_BAR_Y - 1) ? ry + rh - 1 : NAV_BAR_Y - 1;
    if (fy1 < y0) continue;
    bool sel = (i == currentCat);
    uint16_t bg = sel ? C_ACCBG : C_SIDE;
    uint16_t fg = sel ? C_ACC : C_DIM;
    tft.fillRect(0, y0, RADIO_SIDEBAR_W - 2, fy1 - y0 + 1, bg);
    if (sel) tft.fillRect(0, y0, 3, fy1 - y0 + 1, C_ACC);
    int tyy = ry + (rh - 16) / 2;
    if (tyy >= y0 && tyy + 15 <= fy1) {
      int ic = catIconIndex(radioCats[i].key, radioCats[i].group);
      if (ic >= 0) drawSideIcon(10, tyy + 4, ic, sel ? C_ACC : C_DIM, bg);
      font16.drawString(26, tyy, catLabel(i), fg, bg);
    }
    // 2026-10-24 问题①: 数量徽标 (设计稿 <span class="cnt">, 行右缘对齐)
    if (tyy >= y0 && tyy + 15 <= fy1) {
      char cc[10]; snprintf(cc, sizeof(cc), "%d", radioCats[i].count);
      int cw2 = font16.textWidth(cc);
      font16.drawString(RADIO_SIDEBAR_W - 12 - cw2, tyy, cc, sel ? C_ACC : C_DIM, bg);
    }
  }
  // 侧栏滚动条 (右缘 3px)
  int totalH = sideTotalH();
  int viewH = NAV_BAR_Y - (RADIO_HEADER_H + SIDEBAR_HEAD_H);
  if (totalH > viewH) {
    int thumbH = max(14, viewH * viewH / totalH);
    int smax = totalH - viewH;
    int thumbY = RADIO_HEADER_H + SIDEBAR_HEAD_H + (viewH - thumbH) * regionScrollTop / smax;
    tft.fillRect(RADIO_SIDEBAR_W - 5, thumbY, 3, thumbH, C_THUMB);
  }
  tft.drawFastVLine(RADIO_SIDEBAR_W - 1, RADIO_HEADER_H + SIDEBAR_HEAD_H, NAV_BAR_Y - (RADIO_HEADER_H + SIDEBAR_HEAD_H), C_LINE);
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

void drawStationRow(int rowY, RadioStation* st, bool playing, bool favorited, int clipY0, int clipY1) {
  // clipY0/clipY1: 该行在屏幕上的可见 y 区间 (已与列表区求交), 用于像素滚动时的半行裁剪
  if (!st || clipY1 <= clipY0) return;
  int x = RADIO_LIST_X, w = RADIO_LIST_W;
  uint16_t bg = playing ? C_ACCBG : C_CARD;
  tft.fillRect(x, clipY0, w, clipY1 - clipY0, bg);
  int top = max(rowY, clipY0), bot = min(rowY + RADIO_ROW_H, clipY1);
  if (bot <= top) return;
  // 直播点 (播放中, 设计稿 #E5484D)
  int tx = x + 12;
  if (playing) { tft.fillCircle(x + 13, rowY + 14, 3, C_LIVE); tx = x + 24; }
  // 台名 (上) + 分类·地区 (下, 12px 缩放小字)
  if (rowY + 6 >= top && rowY + 18 <= bot)
    font16.drawStringFast(tx, rowY + 6, st->name, C_TXT, bg);
  char info[52];
  const char* catl = stationCatLabel(st);
  if (playing) snprintf(info, sizeof(info), "正在播放 · %s · %s", catl, regionNameOf(st));
  else         snprintf(info, sizeof(info), "%s · %s", catl, regionNameOf(st));
  String inf = info; fitText(inf, w - 36 - tx);
  if (rowY + 26 >= top && rowY + 42 <= bot)
    font16.drawString(tx, rowY + 26, inf, playing ? C_ACC : C_DIM, bg);   // 16px 原生 (原 12px 缩放发虚)
  // 星标 (右缘, 收藏 #E39A1B)
  if (rowY + 5 >= top && rowY + 17 <= bot)
    font16.drawStringFast(x + w - 28, rowY + 5, favorited ? "★" : "☆", favorited ? C_FAV : C_DIM, bg);
  if (rowY + RADIO_ROW_H - 1 >= top && rowY + RADIO_ROW_H - 1 < bot)
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
  // 2026-10-22 问题③: listScrollTop 为"像素偏移" (与左列 regionScrollTop 一致的 1:1 跟手).
  // 行首像素偏移 = listScrollTop % ROW_H, 首行索引 = listScrollTop / ROW_H.
  int pixOff = listScrollTop % RADIO_ROW_H;
  int startRow = listScrollTop / RADIO_ROW_H;
  int first = (pixOff > 0 && startRow > 0) ? startRow - 1 : startRow;   // 顶半行
  int last = startRow + RADIO_STATION_ROWS;        // 底半行 (+1 行)
  if (first < 0) first = 0;
  if (last > count) last = count;
  for (int i = first; i < last; i++) {
    int rowY = y + i * RADIO_ROW_H - listScrollTop;
    int c0 = max(rowY, y), c1 = min(rowY + RADIO_ROW_H, y + h);
    drawStationRow(rowY, curStnAt(i),
                   curSidAt(i) == currentPlayingSid && radioPlaying, isFavSid(curSidAt(i)), c0, c1);
  }
  drawRadioScrollbar();
}

void drawRadioScrollbar() {
  int count = curCount();
  if (count <= RADIO_STATION_ROWS) return;
  int listH = RADIO_LIST_H;
  int totalPix = count * RADIO_ROW_H;
  int scrollPixMax = totalPix - listH;
  int thumbH = max(24, listH * listH / totalPix);
  int thumbY = RADIO_LIST_TOP + (listH - thumbH) * listScrollTop / scrollPixMax;
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
  font16.drawString(x + 26, cy - 8, vb, radioMuted ? C_LIVE : C_DIM, C_CARD);   // 16px 原生
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
    font16.drawString(nx, y + 25, radioPlaying ? "正在播放" : "已暂停", radioPlaying ? C_ACC : C_DIM, C_CARD);   // 16px 原生
  } else {
    font16.drawStringFast(nx, y + 7, "点击电台开始播放", C_DIM, C_CARD);
    font16.drawString(nx, y + 27, "左列选分类 · 右列点电台", C_DIM, C_CARD);   // 16px 原生
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
// 全屏播放视图 (2026-10-24 对齐设计稿 fs-spectrum):
//   顶栏38px: 返回 | "正在播放"直播点标签 | 收藏星
//   主体: 左L频谱(66px) + 中间(台名24px/频率大数字42px+MHz/meta/控制按钮) + 右R频谱
//   底部30px: "双击此处 · 返回列表"
//   L/R 各 30 条 54x4 横向 VU 条 (设计稿: canvas 66x160, 细密横条自下而上堆叠,
//   7 彩 青→绿→黄绿→黄→橙→红→紫 + 白峰值余晖 + 灰底槽), 40ms 级刷新
// ============================================================
#define FS_VU_N      30
#define FS_VU_BW     54          // 横条宽 (66px 画布内居中, 左右各留 6px)
#define FS_VU_BH     4           // 横条高 (设计稿 bh=4.0)
#define FS_VU_GAP    1           // 横条间距 (设计稿 gap=1.0)
#define FS_BASE_Y    256         // 频谱区底边 (设计稿 fs-spectrum 底)
#define FS_VU_BOT    10          // 最底条距底边 (设计稿 H-10)
#define FS_X_L       10          // L 频谱组左缘 (66px)
#define FS_X_R       404         // R 频谱组左缘 (480-66-10)
static const uint16_t FS_SEGCOLOR[7] = {   // 青·绿·黄绿·黄·橙·红·紫
  RGBC(0x00,0xBD,0xD4), RGBC(0x4C,0xAF,0x50), RGBC(0x8B,0xC3,0x4A), RGBC(0xFF,0xC1,0x07),
  RGBC(0xFF,0x98,0x00), RGBC(0xF4,0x43,0x36), RGBC(0x9C,0x27,0xB0) };
static const uint16_t FS_TRACK = RGBC(0xE7,0xEB,0xF1);   // 未点亮底槽 (rgba(130,150,185,.2) on C_BG)
static int  s_litL = 0, s_litR = 0, s_peakL = 0, s_peakR = 0;
static float s_smL = 0, s_smR = 0;
static unsigned long s_peakTL = 0, s_peakTR = 0;
static int fsGroupX(int side) { return side == 0 ? FS_X_L : FS_X_R; }
static void fsVuBar(int side, int i, uint16_t col) {   // 画第 i 条横条 (i>=0, 底部=0)
  int x0 = fsGroupX(side) + (66 - FS_VU_BW) / 2;       // 66px 画布内居中
  int y  = FS_BASE_Y - FS_VU_BOT - (i + 1) * (FS_VU_BH + FS_VU_GAP);
  tft.fillRoundRect(x0, y, FS_VU_BW, FS_VU_BH, 2, col);
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
  float gain = radioPlaying ? (radioVolume * 100 / 21) / 100.0f : 0.0f;  // 暂停/音量0 → VU 熄灭
  int litL = (int)(s_smL * FS_VU_N * gain + 0.5f); if (litL > FS_VU_N) litL = FS_VU_N; if (litL < 0) litL = 0;
  int litR = (int)(s_smR * FS_VU_N * gain + 0.5f); if (litR > FS_VU_N) litR = FS_VU_N; if (litR < 0) litR = 0;
  tft.startWrite();
  for (int side = 0; side < 2; side++) {
    int lit = side ? litR : litL;
    int& peak = side ? s_peakR : s_peakL;
    unsigned long& pt = side ? s_peakTR : s_peakTL;
    // 全画 30 条: 灰底槽 + 点亮(7彩, 高位轻闪) 
    for (int i = 0; i < FS_VU_N; i++) {
      if (i < lit) {
        bool flicker = (i >= lit - 3) && (random(100) < 14);   // 设计稿高位轻闪 0.14
        fsVuBar(side, i, flicker ? FS_SEGCOLOR[(i) * 7 / FS_VU_N]
                                 : FS_SEGCOLOR[(i) * 7 / FS_VU_N]);
      } else {
        fsVuBar(side, i, FS_TRACK);
      }
    }
    // 峰值余晖: 顶部 1.6px 白条 (设计稿 peak 高亮)
    if (lit >= peak) { peak = lit; pt = millis(); }
    else if (millis() - pt > 600) { peak = (peak > lit + 1) ? peak - 1 : lit; }   // 保持 600ms 后逐帧降
    if (peak > 0 && peak <= FS_VU_N) {
      int y = FS_BASE_Y - FS_VU_BOT - (peak) * (FS_VU_BH + FS_VU_GAP) + FS_VU_BH - 1;
      tft.fillRoundRect(fsGroupX(side) + (66 - FS_VU_BW) / 2, y, FS_VU_BW, 2, 1, TFT_WHITE);
    }
  }
  s_litL = litL; s_litR = litR;
  tft.endWrite();
}

static void drawFsFavChip() {
  bool fav = (currentPlayingSid >= 0) && isFavSid(currentPlayingSid);
  tft.fillRoundRect(410, 6, 62, 26, 13, C_CARD);
  font16.drawString(441 - 8, 7, fav ? "★" : "☆", fav ? C_FAV : C_DIM, C_CARD);   // 16px 原生 (顶栏中心 y=15)
}

void drawStationFullscreen() {
  tft.fillRect(0, 0, 480, NAV_BAR_Y, C_BG);
  RadioStation* st = (currentPlayingSid >= 0) ? stn(currentPlayingSid) : nullptr;
  if (!st) { showingFullscreen = false; return; }
  // ---- 顶栏 38px: 返回 | "正在播放" 直播点标签 | 收藏星 ----
  tft.fillRect(0, 0, 480, 38, C_CARD);
  tft.drawFastHLine(0, 38, 480, C_LINE);
  // 返回 chip (左上)
  tft.fillRoundRect(8, 6, 64, 26, 13, C_BG);
  tft.drawLine(20, 19, 25, 14, C_TXT); tft.drawLine(20, 19, 25, 24, C_TXT);
  font16.drawStringFast(30, 11, "返回", C_TXT, C_BG);
  // "正在播放" 标签 (中, 直播点闪烁)
  const char* stx = radioPlaying ? "正在播放" : "已暂停";
  int sw = font16.textWidth(stx);
  int sx = 240 - sw / 2 - 8;
  tft.fillCircle(sx, 19, 3, radioPlaying ? C_LIVE : C_DIM);
  font16.drawStringFast(sx + 8, 11, stx, radioPlaying ? C_ACC : C_DIM, C_CARD);
  drawFsFavChip();
  // ---- 主体: L 频谱 | 中间信息 | R 频谱 ----
  // 台名 16px (字库原生 font16 AA, 超宽截断; font24 已清除, 不用 64px 源缩放避免缺字回退放大发虚)
  String nm(st->name); fitText(nm, 280);
  font16.drawString(240 - font16.textWidth(nm.c_str()) / 2, 70, nm, C_TXT, C_BG);
  // 频率 24px (font32 64px 源含全部 ASCII 数字, 面积平均下采样清晰) + MHz 16px (字库原生字号; 无 FM 前缀)
  String fr = st->freq[0] ? String(st->freq) : String("网络电台");
  if (fr.startsWith("FM")) fr = fr.substring(2);
  fr.trim();
  String frUn = st->freq[0] ? "MHz" : "";
  int fw = almBigText(240, 113, fr, 24, RGBC(0x16,0x24,0x3F), C_BG, true);
  if (frUn.length())
    font16.drawString(240 + fw / 2 + 8, 128, frUn, C_ACC, C_BG);
  // meta 行: 地区 tag / 分类 tag / 网络 (12px)
  int my = 158;
  int mx = 240;
  const char* reg = regionNameOf(st);
  if (reg && reg[0]) {
    int rw = font16.textWidth(reg);
    tft.fillRoundRect(mx - rw / 2 - 8, my - 2, rw + 16, 18, 4, RGBC(0xEA,0xF1,0xFA));
    font16.drawStringFast(mx - rw / 2, my, reg, RGBC(0x4A,0x5F,0x87), RGBC(0xEA,0xF1,0xFA));
    mx += rw / 2 + 8 + 22;
  }
  const char* cat = stationCatLabel(st);
  if (cat && cat[0] && strcmp(cat, reg) != 0) {
    int cw2 = font16.textWidth(cat);
    tft.fillRoundRect(mx - cw2 / 2 - 8, my - 2, cw2 + 16, 18, 4, RGBC(0xEA,0xF1,0xFA));
    font16.drawStringFast(mx - cw2 / 2, my, cat, RGBC(0x4A,0x5F,0x87), RGBC(0xEA,0xF1,0xFA));
  }
  // 控制按钮: 音量− | 播放/暂停 | 音量+
  tft.fillCircle(FS_BTN_DN_X, FS_BTN_Y, FS_BTN_R, C_CARD);
  tft.fillRect(FS_BTN_DN_X - 7, FS_BTN_Y - 1, 14, 3, C_TXT);
  bool paused = !radioPlaying;
  uint16_t mcol = paused ? C_FAV : C_ACC;
  tft.fillCircle(FS_BTN_MAIN_X, FS_BTN_Y, FS_BTN_MAIN_R, mcol);
  if (!paused) {
    tft.fillRect(FS_BTN_MAIN_X - 7, FS_BTN_Y - 12, 5, 24, TFT_WHITE);
    tft.fillRect(FS_BTN_MAIN_X + 2, FS_BTN_Y - 12, 5, 24, TFT_WHITE);
  } else {
    tft.fillTriangle(FS_BTN_MAIN_X - 8, FS_BTN_Y - 12, FS_BTN_MAIN_X + 10, FS_BTN_Y, FS_BTN_MAIN_X - 8, FS_BTN_Y + 12, TFT_WHITE);
  }
  tft.fillCircle(FS_BTN_UP_X, FS_BTN_Y, FS_BTN_R, C_CARD);
  tft.drawLine(FS_BTN_UP_X - 7, FS_BTN_Y, FS_BTN_UP_X + 7, FS_BTN_Y, C_TXT);
  tft.drawLine(FS_BTN_UP_X, FS_BTN_Y - 7, FS_BTN_UP_X, FS_BTN_Y + 7, C_TXT);
  // 音量% 16px (字库原生; 主按钮下方)
  char vb[8]; snprintf(vb, sizeof(vb), "%d%%", radioVolume * 100 / 21);
  const char* vtx = radioMuted ? "静音" : vb;
  font16.drawString(240 - font16.textWidth(vtx) / 2, FS_BTN_Y + 26, vtx, radioMuted ? C_LIVE : C_DIM, C_BG);
  // L/R 声道标签 (设计稿 ch-label, canvas 下方)
  font16.drawString(FS_X_L + 30, FS_BASE_Y - 4, "L", C_DIM, C_BG);
  font16.drawString(FS_X_R + 30, FS_BASE_Y - 4, "R", C_DIM, C_BG);
  // ---- 频谱: L/R VU 横条 ----
  fsResetSpectrum();
  // ---- 底部提示条: "双击此处 · 返回列表" (16px 原生) ----
  tft.fillRect(0, NAV_BAR_Y, 480, 30, C_CARD);
  tft.drawFastHLine(0, NAV_BAR_Y, 480, C_LINE);
  const char* htx = "双击此处 · 返回列表";
  font16.drawString(240 - font16.textWidth(htx) / 2, NAV_BAR_Y + 7, htx, C_DIM, C_CARD);
}
