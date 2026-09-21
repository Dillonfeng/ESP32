/*
 * ESP32-S3 网络收音机 - 主程序 v2
 * ===============================
 * 硬件: ESP32-S3-N16R8 + ST7796S 480x320 + XPT2046 + PCM5102A + INMP441
 * 
 * 参考设计: yoradio (widget布局), tftziku (中文字库), ESP32-audioI2S
 * 
 * 功能模块:
 * 1. 翻页时钟 (数码管风格数字 + 中文日期)
 * 2. 万年历 (农历/干支/生肖/节气/定制节日)
 * 3. 网络收音机 (HLS+HTTPS, 电台列表, 触摸控制)
 * 4. 天气显示
 * 
 * 切换方式: 底部触摸导航栏4个按钮
 *
 * ⚠️ 注意: 本文件已手动改写为"机械翻页(Split-Flap)时钟 + 局部刷新"。
 *    请勿再运行 scripts/rebuild_main.py —— 它会覆盖这些手动改动。
 */

#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <time.h>
#include <LittleFS.h>
#include <nvs.h>
#include <TFT_eSPI.h>   // 已包含 Fonts/GFXFF/* (FreeSansBold24pt7b 等)
#include <Audio.h>
#include <ArduinoJson.h>
#include "xfont.h"
#include "nongli.h"
#include "digits.h"
#include "dialnum.h"
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// 调试提示开关: 1=调试期显示"点屏幕切换"等提示; 正式产品改 0 全部隐藏
// (与 preview/make_preview.py 的 DEBUG_UI 保持一致)。
#define DEBUG_UI 0

// ========== 引脚定义 ==========
#define I2S_BCK_PIN     6
#define I2S_LRC_PIN     7
#define I2S_DOUT_PIN    8
#define STATUS_LED_PIN  2
#define TFT_BL_PIN      21

// ========== 全局对象 ==========
TFT_eSPI tft = TFT_eSPI();
XFont font16(&tft);
XFont font24(&tft);
Audio audio;

// ========== 触摸校准参数 ==========
uint16_t touchCalib[] = {194, 3767, 174, 3844, 0x01};

// ========== UI 页面定义 ==========
enum UIMode {
  MODE_CLOCK = 0,
  MODE_CALENDAR,
  MODE_RADIO,
  MODE_WEATHER,
  MODE_COUNT
};
UIMode currentMode = MODE_CLOCK;
bool modeChanged = true;
// 每个模式独立的"已绘制"标志，切换时全部重置为 false
bool modeDrawn[MODE_COUNT] = {false, false, false, false};

// ========== 导航栏布局 ==========
#define NAV_BAR_Y       288
#define NAV_BAR_HEIGHT  32
#define NAV_BTN_WIDTH   120
#define NAV_BTN_COUNT   4
const char* navLabels[] = {"时钟", "日历", "收音机", "天气"};

// ========== 颜色定义 ==========
#define COLOR_BG       TFT_BLACK
#define COLOR_TEXT     TFT_WHITE
#define COLOR_ACCENT   TFT_CYAN
#define COLOR_NAV_BG   0x2104
#define COLOR_NAV_SEL  0x4A69
#define COLOR_DIGIT    0x07FF  // 青色数码管
#define COLOR_DIM      0x7BEF  // 暗灰

// ========== WiFi/NTP 状态 ==========
bool wifiConnected = false;
bool ntpSynced = false;
bool wifiConnecting = false;
unsigned long wifiConnectStart = 0;
const char* ntpServer = "ntp.aliyun.com";
const int gmtOffsetSec = 8 * 3600;

// ========== 时间来源 ==========
// 通过 WiFi + NTP 同步时间 (需联网)。

// ========== 时钟状态 ==========
int prevH = -1, prevM = -1, prevS = -1;
bool colonBlink = true;

// 时钟样式 (0=机械翻页 1=模拟指针) — 前向声明, 实现见下方
enum ClockStyle { STYLE_FLAP = 0, STYLE_ANALOG = 1 };
static uint8_t clockStyle = STYLE_FLAP;
static uint8_t getClockStyle();
static void saveClockStyle(uint8_t v);

// ========== 收音机状态 ==========
// 二进制 stations.bin 格式 (v2):
//   Header(24) + RegionIdx(28*N) + StationRecords(变长)
#define RADIO_SIDEBAR_W    70
#define RADIO_HEADER_H     28
#define RADIO_PLAYBAR_H    80   // 底部播放栏高度
#define RADIO_LIST_X       70
#define RADIO_LIST_W       390  // 减去滚动条 + 收藏列
#define RADIO_SCROLLBAR_W  8
#define RADIO_FAV_W        24   // 每行收藏按钮宽度
#define RADIO_ROW_H        55
// 列表可见行数: 320 - header(28) - playbar(80) - nav(32) = 180px / 55 = 3.27 → 3
#define RADIO_STATION_ROWS 3
#define RADIO_LIST_TOP     (RADIO_HEADER_H)
#define RADIO_LIST_BOT     (NAV_BAR_Y - RADIO_PLAYBAR_H)
#define RADIO_LIST_H       (RADIO_LIST_BOT - RADIO_LIST_TOP)
#define RADIO_FULLSCREEN   1    // 是否显示电台全屏信息页

// 收藏: 用 station bin_offset 作为唯一 ID
#define MAX_FAVORITES 64
uint32_t favorites[MAX_FAVORITES];
int favoriteCount = 0;
bool favoritedChanged = false;
bool showingFullscreen = false;
unsigned long lastTapTime = 0;
int lastTapRow = -1;

struct RegionEntry {
  char name[17];            // UTF-8 名称, null-padded
  uint32_t first_offset;    // 在 station records section 的起始偏移
  uint16_t count;
};

struct StationView {        // 列表显示用 (不含 URL)
  char name[32];            // UTF-8, 截断显示
  char freq[13];            // UTF-8, 如 "FM105.8"
  uint8_t region_idx;
  uint8_t category_idx;     // 对应 region_entries 下标
  uint8_t r, g, b;
  uint32_t bin_offset;      // 在 stations.bin 中的完整记录偏移
};

// 全局数据
RegionEntry* regionEntries = nullptr;
int regionCount = 0;
StationView* curRegionStations = nullptr;
int curRegionCount = 0;
int curRegionIdx = 0;            // 当前选中的分类
int curRegionFirstOffset = 0;   // 当前分类在 bin 中的起点

// 播放状态
int currentPlayingIdx = -1;     // 当前播放的电台在 curRegionStations 中的下标
bool radioPlaying = false;
bool radioInited = false;
int radioVolume = 10;
bool musicPlaying = false;      // 节日/节气 网络流音乐正在播放

// 侧边栏滚动
int regionScrollTop = 0;        // 第一个可见的 region 下标

// 电台列表滚动
int listScrollTop = 0;          // 第一个可见的电台下标 (行号)
bool listScrollDirty = true;    // 需要重绘列表

// 触摸状态
// touchLastX/Y 已在 handleTouch 前的 static 变量中定义

// ========== 天气状态 ==========
bool weatherInited = false;
// 温度/湿度 (占位 mock 值, 接入天气 API 后替换; 默认与 make_preview.py 一致)
int mockTempC = 25;
int mockHumidity = 60;

// ========== 翻页时钟 (机械翻页牌 / Split-Flap) ==========
// 屏幕 480x480 (rot 1)。布局: 6 张白色翻页牌(HH:MM:SS) + 中央铰链线
// + 下方日期条。只重绘发生变化的数字牌(局部刷新, 不整屏刷新)。
// 数字: FreeSansBold24pt7b 缩放2 → '0' 约 48x70 px。
#define CARD_W        56    // 单张数字牌宽 (容纳 48px 数字 + 边距)
#define COLON_W       12    // 冒号宽
#define CARD_GAP      6     // 同组两数字牌间距
#define COLON_GAP     10    // 冒号两侧间距
#define CARD_TOP      48    // 翻页牌顶边
#define CARD_H        120   // 翻页牌高 (数字 70px 居中 + 上下留白)
#define CARD_RADIUS   10    // 圆角半径
#define DIGIT_SCALE   2     // FreeSansBold24 缩放 (24px -> 48px 宽)
#define DATE_LEFT     31    // 日期条左边 (与 CARD_X0 对齐)
#define DATE_RIGHT    449   // 日期条右边 (与末张牌右缘对齐)
#define ROW1_TOP      186   // 主日期行顶边 (公历/星期 | 农历/干支)
#define ROW1_H        46    // 主日期行高
#define ROW2_TOP      240   // 详情行顶边 (节气 | 月相)
#define ROW2_H        36    // 详情行高

// 翻页牌配色
#define CARD_FACE     0x6318  // 白  (0xE0,0xF8,0x08)
#define CARD_SEAM     0x2108  // 中央铰链暗缝
#define CARD_SHADE    0x1084  // 翻页时上半暗阴影
#define CARD_EDGE     0x39C6  // 卡片下缘投影
#define CARD_NUM      0x0000  // 数字黑色
#define DATE_STRIP    0x18E3  // 详情行底色 (近黑, 与导航栏一致)
#define ROW1_BG       0x2146  // 主日期行底色 (深灰蓝, 稍亮突出)
#define DATE_SEP      0x0821  // 日期分隔竖线
#define COLOR_GOLD    0xFE4C  // 干支金色
#define SOFT_C        0x94F6  // 灰蓝 (普通节气文字)

// 节日/节气 当天显示颜色 ("今日XX" 用, 未知事件 -> 灰蓝)
static uint16_t eventColor(const char* name) {
  if (!name || !name[0]) return SOFT_C;
  // 24 节气
  if (!strcmp(name,"冬至"))            return TFT_BLUE;
  if (!strcmp(name,"小寒")||!strcmp(name,"大寒")||!strcmp(name,"小雪")||
      !strcmp(name,"大雪")||!strcmp(name,"立冬")||!strcmp(name,"霜降")||
      !strcmp(name,"寒露"))            return TFT_BLUE;
  if (!strcmp(name,"夏至"))            return TFT_CYAN;
  if (!strcmp(name,"清明")||!strcmp(name,"谷雨")||!strcmp(name,"立春")||
      !strcmp(name,"雨水")||!strcmp(name,"惊蛰")||!strcmp(name,"立夏")||
      !strcmp(name,"白露"))            return TFT_GREEN;
  if (!strcmp(name,"小暑")||!strcmp(name,"大暑")||!strcmp(name,"立秋")||
      !strcmp(name,"处暑")||!strcmp(name,"小满")||!strcmp(name,"芒种"))
                                      return TFT_YELLOW;
  // 24 节气之外 (春分/秋分) 用金色
  if (!strcmp(name,"春分")||!strcmp(name,"秋分")) return COLOR_GOLD;
  // 节日
  if (!strcmp(name,"春节")||!strcmp(name,"元旦")||!strcmp(name,"国庆节")||
      !strcmp(name,"建党节")||!strcmp(name,"建军节")||!strcmp(name,"劳动节")||
      !strcmp(name,"情人节")||!strcmp(name,"圣诞节")) return TFT_RED;
  if (!strcmp(name,"元宵节")||!strcmp(name,"中秋节")||!strcmp(name,"重阳节")||
      !strcmp(name,"腊八节")||!strcmp(name,"小年")||!strcmp(name,"龙抬头")||
      !strcmp(name,"端午节")||!strcmp(name,"青年节")) return COLOR_GOLD;
  return SOFT_C;
}

// 温度颜色: 随数值变化 (越冷越蓝, 越热越红) — 天气接入后在 drawDateRow 使用
// 冰点以下用浅蓝 (0x6CF1) 而非纯蓝 (0x001F), 避免在深色背景上不可读
uint16_t tempColor(int c) {
  if (c <= 0)  return 0x6CF1;       // 冰点以下 - 浅蓝 (高对比度)
  if (c <= 8)  return TFT_CYAN;     // 冷
  if (c <= 15) return TFT_GREEN;    // 凉
  if (c <= 24) return COLOR_GOLD;   // 舒适
  if (c <= 29) return TFT_YELLOW;   // 热
  return TFT_RED;                   // 炎热 (>30°)
}

// 湿度颜色: 随数值变化 (过干偏黄, 舒适绿, 过湿偏蓝)
uint16_t humidColor(int h) {
  if (h <= 30) return TFT_YELLOW;   // 过干
  if (h <= 45) return COLOR_GOLD;   // 偏干
  if (h <= 65) return TFT_GREEN;    // 舒适
  if (h <= 80) return TFT_CYAN;     // 偏湿
  return 0x6CF1;                    // 过湿 - 浅蓝 (高对比度)
}
#define MOON_LIT      0xFF97  // 月相亮部
#define MOON_DARK     0x2146  // 月相暗部
#define MOON_EDGE     0x424B  // 月相轮廓

// 数字牌 x 坐标 (居中布局: 6 张牌 + 2 个冒号, 总宽 418, 左右各 31px 边距)
#define CARD_X0       31
#define CARD_X1       (CARD_X0 + CARD_W + CARD_GAP)              // 93
#define COLON_X       (CARD_X1 + CARD_W + COLON_GAP)             // 159
#define CARD_X2       (COLON_X + COLON_W + COLON_GAP)            // 181
#define CARD_X3       (CARD_X2 + CARD_W + CARD_GAP)              // 243
#define COLON_X2      (CARD_X3 + CARD_W + COLON_GAP)             // 309
#define CARD_X4       (COLON_X2 + COLON_W + COLON_GAP)           // 331
#define CARD_X5       (CARD_X4 + CARD_W + CARD_GAP)              // 393

// 每张数字牌的状态 (current=已显示的旧数字, 用于翻页)
struct FlipCard {
  char current;   // 当前显示的字符
  bool ready;     // 是否已完成初始绘制
};
FlipCard flipCard[6] = {
  {'0',false},{'0',false},{'0',false},{'0',false},{'0',false},{'0',false}
};

// 农历/星期 名称表
const char* lunarMonths[] = {"正","二","三","四","五","六","七","八","九","十","冬","腊"};
const char* lunarDays[] = {
  "初一","初二","初三","初四","初五","初六","初七","初八","初九","初十",
  "十一","十二","十三","十四","十五","十六","十七","十八","十九","二十",
  "廿一","廿二","廿三","廿四","廿五","廿六","廿七","廿八","廿九","三十"
};
const char* weekDaysFull[] = {"星期日","星期一","星期二","星期三","星期四","星期五","星期六"};

// ========== 函数声明 ==========
void initTFT();
void initTouch();
void drawNavBar();
void handleTouch();
void modeClock();
void modeCalendar();
void modeRadio();
void modeWeather();
void checkFestivalMusic(int year, int mon, int mday);
bool loadRadioDB();
void loadRegionStations(int regionIdx);
void playStationAt(int listIdx);
void stopRadio();
String readStationURL(uint32_t binOffset);
void drawRadioRegionSidebar();
void drawRadioList();
void drawRadioHeader();
void drawRadioNowPlaying();
void drawStationRow(int rowY, StationView& st, bool playing);
void drawRadioScrollbar();
void saveFavorites();
bool isFavorited(uint32_t binOffset);
void toggleFavorite(uint32_t binOffset);
void loadFavorites();
void drawRadioPlayBar();
void handleRadioTap(uint16_t tx, uint16_t ty);
void drawStationFullscreen();

// ============================================================
// 触摸状态变量 (边沿检测)
// ============================================================
static bool touchWasDown = false;
static int touchStartX = 0, touchStartY = 0;
static int touchLastX = 0, touchLastY = 0;
static unsigned long touchStartTime = 0;
static bool touchDidScroll = false;
static int touchScrollStartScrollTop = 0;

// ============================================================
// initTFT / initTouch / drawNavBar
// ============================================================
void initTFT() {
  tft.init();
  tft.setRotation(3);   // 180° 旋转 (原为 1)
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(1);
  pinMode(TFT_BL_PIN, OUTPUT);
  digitalWrite(TFT_BL_PIN, HIGH);
}

void initTouch() {
  tft.setTouch(touchCalib);
}

void drawNavBar() {
  tft.fillRect(0, NAV_BAR_Y, 480, NAV_BAR_HEIGHT, COLOR_NAV_BG);
  for (int i = 0; i < NAV_BTN_COUNT; i++) {
    int x = i * NAV_BTN_WIDTH;
    bool sel = (i == currentMode);
    if (sel) {
      tft.fillRect(x + 2, NAV_BAR_Y + 2, NAV_BTN_WIDTH - 4, NAV_BAR_HEIGHT - 4, COLOR_NAV_SEL);
    }
    uint16_t tc = sel ? TFT_YELLOW : 0xC618;
    int tw = font16.textWidth(navLabels[i]);
    font16.drawString(x + (NAV_BTN_WIDTH - tw) / 2, NAV_BAR_Y + 8, navLabels[i], tc, sel ? COLOR_NAV_SEL : COLOR_NAV_BG);
    if (i > 0) tft.drawFastVLine(x, NAV_BAR_Y + 2, NAV_BAR_HEIGHT - 4, 0x630C);
  }
}

// ============================================================
// handleTouch (边沿检测版)
// ============================================================
void handleTouch() {
  uint16_t tx, ty;
  int touched = tft.getTouch(&tx, &ty);
  bool isDown = (touched > 0);

  // 触摸补偿: getTouch 不随 setRotation 变换, 返回固定物理帧坐标。
  // 显示当前为 rotation(3) (相对原 rotation(1) 旋转 180°), 故 x/y 均需翻转。
  if (isDown) {
    tx = tft.width()  - tx;
    ty = tft.height() - ty;
  }

  if (isDown && !touchWasDown) {
    touchStartX = touchLastX = tx;
    touchStartY = touchLastY = ty;
    touchStartTime = millis();
    touchDidScroll = false;
    touchScrollStartScrollTop = listScrollTop;
  }

  if (!isDown && touchWasDown) {
    unsigned long dur = millis() - touchStartTime;
    int dy = touchLastY - touchStartY;
    int dx = touchLastX - touchStartX;
    touchWasDown = false;
    touchStartTime = 0;

    if (!touchDidScroll && dur < 800 && abs(dy) < 15 && abs(dx) < 15) {
      if (touchStartY >= NAV_BAR_Y) {
        int btnIdx = touchStartX / NAV_BTN_WIDTH;
        if (btnIdx >= 0 && btnIdx < MODE_COUNT) {
          if (btnIdx != currentMode) {
            currentMode = (UIMode)btnIdx;
            modeChanged = true;
            for (int i = 0; i < MODE_COUNT; i++) modeDrawn[i] = false;
            prevH = prevM = prevS = -1;
          }
          drawNavBar();
        }
      } else if (currentMode == MODE_RADIO) {
        handleRadioTap(touchStartX, touchStartY);
      } else if (currentMode == MODE_CLOCK && touchStartY < 30 && touchStartX < 100) {
        font16.drawString(100, 100, "WiFi配网模式", TFT_YELLOW, COLOR_BG);
        font16.drawString(80, 130, "手机连接: ESP32-Radio", TFT_WHITE, COLOR_BG);
        font16.drawString(100, 150, "密码: 12345678", TFT_WHITE, COLOR_BG);
        font16.drawString(80, 170, "然后访问 192.168.4.1", TFT_CYAN, COLOR_BG);
        WiFiManager wm;
        wm.setConfigPortalTimeout(180);
        wm.startConfigPortal("ESP32-Radio", "12345678");
        if (WiFi.status() == WL_CONNECTED) { wifiConnected = true; configTime(gmtOffsetSec, 0, ntpServer); }
        modeChanged = true; prevH = prevM = prevS = -1;
      } else if (currentMode == MODE_CLOCK && touchStartY < NAV_BAR_Y) {
        // 点击时钟页 (左上角 30x100 为配网热区) 切换 翻页/指针 样式
        clockStyle = (clockStyle == STYLE_FLAP) ? STYLE_ANALOG : STYLE_FLAP;
        saveClockStyle(clockStyle);
        modeChanged = true;
        prevH = prevM = prevS = -1;
        Serial.printf("clock style -> %s\n", clockStyle == STYLE_FLAP ? "flap" : "analog");
      }
      delay(150);
    }
    touchDidScroll = false;
  }

  if (isDown && currentMode == MODE_RADIO) {
    int dy = ty - touchStartY;
    if (abs(dy) > 10 && !touchDidScroll) touchDidScroll = true;
    if (touchDidScroll) {
      int newScroll = touchScrollStartScrollTop + dy / 3;
      newScroll = constrain(newScroll, 0, max(0, curRegionCount - RADIO_STATION_ROWS));
      if (newScroll != listScrollTop) { listScrollTop = newScroll; listScrollDirty = true; }
    }
    touchLastX = tx; touchLastY = ty;
  }

  // 边沿检测: 必须在所有检查完旧 touchWasDown 之后再更新,
  // 否则释放沿 (!isDown && touchWasDown) 永远为 false → 触摸无反应。
  touchWasDown = isDown;
}

// ============================================================
// modeClock / modeCalendar / modeWeather
// ============================================================

// 单张数字牌各 x 坐标 (索引 0..5 → HH MM SS)
static const int cardX[6] = {CARD_X0, CARD_X1, CARD_X2, CARD_X3, CARD_X4, CARD_X5};

// 绘制一张静态数字牌: 白底圆角 + 中央铰链线 + 下缘投影 + 居中数字
// shadeTop: 是否给上半部加暗阴影 (用于翻页动画)
static void drawCard(int idx, char digit, bool shadeTop) {
  int x = cardX[idx], y = CARD_TOP, w = CARD_W, h = CARD_H;
  int seamY = y + h / 2;
  // 下缘投影 (先画, 让牌浮起)
  tft.fillRoundRect(x, y + 3, w, h, CARD_RADIUS, CARD_EDGE);
  // 牌面
  tft.fillRoundRect(x, y, w, h, CARD_RADIUS, CARD_FACE);
  // 翻页阴影: 覆盖上半部
  if (shadeTop) {
    tft.fillRoundRect(x, y, w, h / 2, CARD_RADIUS, CARD_SHADE);
    tft.fillRect(x, seamY, w, 1, CARD_FACE); // 擦平铰链线处的阴影边
  }
  // 中央铰链线
  tft.drawFastHLine(x + 2, seamY, w - 4, CARD_SEAM);
  tft.drawFastHLine(x + 2, seamY + 1, w - 4, CARD_FACE);
  // 抗锯齿数字: 用 8 位灰度遮罩按当前背景(牌面/翻页阴影)逐通道混合后推送。
  // mask=255 -> 背景色, mask=0 -> 黑(数字笔画)。逐通道混合才不偏色。
  const uint8_t *mask = DigitMasks[digit - '0'];
  static uint16_t dgb[DIGIT_BYTES];
  uint16_t bg = shadeTop ? CARD_SHADE : CARD_FACE;
  uint16_t r5 = bg >> 11;
  uint16_t g6 = (bg >> 5) & 0x3F;
  uint16_t b5 = bg & 0x1F;
  for (int i = 0; i < DIGIT_BYTES; i++) {
    uint8_t m = pgm_read_byte(&mask[i]);
    dgb[i] = (((r5 * m) / 255) << 11) | (((g6 * m) / 255) << 5) | ((b5 * m) / 255);
  }
  tft.pushImage(x + (w - DIGIT_W) / 2, y + (h - DIGIT_H) / 2, DIGIT_W, DIGIT_H, dgb);
}

// "快门"翻页: 变暗 → 换数字 → 恢复。仅作用于单张牌(局部)。
// 本构建无 startWindow/endWindow 裁剪, 故用整牌重绘模拟翻页。
static void flipDigitAnim(int idx, char newDigit) {
  drawCard(idx, newDigit, true);   // 1) 上半变暗, 显示新数字
  delay(120);
  drawCard(idx, newDigit, false);  // 2) 恢复, 显示新数字
  delay(60);                       // 3) 短停顿, 让翻页更从容
}

// ========== 时钟样式实现 (0=机械翻页 1=模拟指针) ==========
static uint8_t getClockStyle() {
  uint8_t v = STYLE_FLAP;
  nvs_handle_t h;
  if (nvs_open("clockcfg", NVS_READONLY, &h) == ESP_OK) {
    nvs_get_u8(h, "style", &v);
    nvs_close(h);
  }
  if (v > STYLE_ANALOG) v = STYLE_FLAP;
  return v;
}
static void saveClockStyle(uint8_t v) {
  nvs_handle_t h;
  if (nvs_open("clockcfg", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u8(h, "style", v);
    nvs_commit(h);
    nvs_close(h);
  }
}

// 顶右侧: 当前时钟样式 + 操作提示 (黑底白字小字) — 仅 DEBUG_UI 时显示
static void drawModeHint(uint8_t style) {
#if !DEBUG_UI
  (void)style;
  return;
#else
  char s[24];
  snprintf(s, sizeof(s), "%s | 点屏幕切换", style == STYLE_FLAP ? "翻页" : "指针");
  int w = font16.textWidth(s);
  tft.fillRect(478 - w - 8, 6, w + 8, 22, COLOR_BG);
  font16.drawString(470 - w, 12, s, 0x7BEF, COLOR_BG);
#endif
}

// 模拟指针表盘 —— 白色表盘 + 黑指针/刻度/数字 + 红秒针 (与 make_preview.py 完全一致)。
// 数字用与翻页时钟同款 Bahnschrift 位图遮罩 (dialnum.h), 抗锯齿平滑; 无外圈描边。
static const uint16_t AC_FACE  = 0xFFFF;   // 表盘 (白)
static const uint16_t AC_TICK1 = 0x0000;   // 时标 (黑)
static const uint16_t AC_TICK2 = 0x0000;   // 分标 (黑)
static const uint16_t AC_H     = 0x0000;   // 时针 (黑)
static const uint16_t AC_M     = 0x0000;   // 分针 (黑)
static const uint16_t AC_S     = 0xF800;   // 秒针 (红)
static const uint16_t AC_HUB   = 0xF800;   // 中心枢纽 (红)
static const uint16_t AC_NUM   = 0x0000;   // 数字 (黑)

// 数字位图指针 (顺序同 dialnum.h: [0]="12",[1..9]="1".."9",[10]="10",[11]="11")
static const uint8_t *const DialNums[12] = {
  DialNum0, DialNum1, DialNum2, DialNum3, DialNum4, DialNum5, DialNum6,
  DialNum7, DialNum8, DialNum9, DialNum10, DialNum11
};

// 把某个数字位图按 8 位灰度遮罩合成到白色表盘, 用 pushImage 推屏 (与 drawCard 同款)。
// mask=255 -> 表盘色(白), mask=0 -> 黑(数字笔画)。cx/cy 为数字中心。
static void blitDialNum(int cx, int cy, int idx, uint16_t face) {
  const uint8_t *mask = DialNums[idx];
  int w = pgm_read_byte(&DialNumW[idx]);
  int h = pgm_read_byte(&DialNumH[idx]);
  static uint16_t nbuf[24 * 22];   // 最大数字盒 24x22
  uint16_t r5 = face >> 11, g6 = (face >> 5) & 0x3F, b5 = face & 0x1F;
  for (int i = 0; i < w * h; i++) {
    uint8_t m = pgm_read_byte(&mask[i]);
    nbuf[i] = (((r5 * m) / 255) << 11) | (((g6 * m) / 255) << 5) | ((b5 * m) / 255);
  }
  tft.pushImage(cx - w / 2, cy - h / 2, w, h, nbuf);
}

static void drawAnalogClock(int h, int m, int s) {
  unsigned long nowMs = millis();
  int cx = 240, cy = 108, R = 73;   // 指针表盘放大 (顶 ~32 / 底 ~184, 不压日期条)
  double t2h = h + m / 60.0 + s / 3600.0;
  double t2m = m + s / 60.0;
  double t2s = (s + (nowMs % 1000) / 1000.0) / 60.0;

  // 表盘 (白色, 无外圈描边)
  tft.fillCircle(cx, cy, R, AC_FACE);
  // 刻度: 60 分标 + 12 时标
  for (int i = 0; i < 60; i++) {
    double a = M_PI * 2.0 * i / 60.0 - M_PI / 2.0;
    bool major = (i % 5 == 0);
    double ro = R - 2, ri = R - (major ? 10 : 5);
    tft.drawLine((int)(cx + cosf(a) * ri), (int)(cy + sinf(a) * ri),
                 (int)(cx + cosf(a) * ro), (int)(cy + sinf(a) * ro),
                 major ? AC_TICK1 : AC_TICK2);
  }
  // 数字 12..11 (位图遮罩, 与翻页时钟同款字体; 中心落在半径 R-20 处, 与 make_preview.py 一致)
  for (int k = 1; k <= 12; k++) {
    double an = M_PI * 2.0 * k / 12.0 - M_PI / 2.0;
    int idx = (k == 12) ? 0 : k;   // 12->0, 10->10, 11->11, 1..9 -> 1..9 (DialNum[10]="10",[11]="11")
    int nx = (int)(cx + cosf(an) * (R - 20));
    int ny = (int)(cy + sinf(an) * (R - 20));
    blitDialNum(nx, ny, idx, AC_FACE);
  }
  // 时针 / 分针 (棒棒指针: 中段最宽、向两端渐细的多边形 + 圆头帽, 边缘平滑)
  auto hand = [&](double ang, int len, int w, int tail, uint16_t col) {
    double a = M_PI * 2.0 * ang - M_PI / 2.0;
    float dx = cosf(a), dy = sinf(a);
    float ux = -sinf(a), uy = cosf(a);
    float hb = w / 2.0f;                 // 中段半宽
    float ht = fmaxf(1.0f, w * 0.30f);   // 两端半宽
    float capD = len - ht;               // 圆帽起点(距中心)
    // 多边形顶点: 尾上A -> 中段左C -> 尖上E -> 尖下F -> 中段右D -> 尾下B
    int Ax = (int)(cx + dx * (-tail) + ux * ht), Ay = (int)(cy + dy * (-tail) + uy * ht);
    int Cx = (int)(cx + ux * hb),          Cy = (int)(cy + uy * hb);
    int Ex = (int)(cx + dx * capD + ux * ht), Ey = (int)(cy + dy * capD + uy * ht);
    int Fx = (int)(cx + dx * capD - ux * ht), Fy = (int)(cy + dy * capD - uy * ht);
    int Dx = (int)(cx - ux * hb),          Dy = (int)(cy - uy * hb);
    int Bx = (int)(cx + dx * (-tail) - ux * ht), By = (int)(cy + dy * (-tail) - uy * ht);
    tft.fillTriangle(Ax, Ay, Cx, Cy, Ex, Ey, col);
    tft.fillTriangle(Ax, Ay, Ex, Ey, Fx, Fy, col);
    tft.fillTriangle(Ax, Ay, Fx, Fy, Dx, Dy, col);
    tft.fillTriangle(Ax, Ay, Dx, Dy, Bx, By, col);
    // 尖端圆帽
    tft.fillCircle((int)(cx + dx * capD), (int)(cy + dy * capD), (int)ht + 1, col);
  };
  hand(t2h / 12.0, 40, 4, 11, AC_H);
  hand(t2m / 60.0, 55, 3, 14, AC_M);
  // 秒针 (细线, 平滑扫秒)
  double sa = M_PI * 2.0 * t2s - M_PI / 2.0;
  tft.drawLine(cx, cy, cx + (int)cosf(sa) * 60, cy + (int)sinf(sa) * 60, AC_S);
  tft.fillCircle(cx - (int)cosf(sa) * 15, cy - (int)sinf(sa) * 15, 3, AC_S);
  // 中心枢纽
  tft.fillCircle(cx, cy, 5, AC_HUB);
  tft.fillCircle(cx, cy, 2, AC_FACE);

  drawModeHint(STYLE_ANALOG);
}

// 月相图标: 暗底圆盘 + 亮部 (上弦亮右 / 下弦亮左), 亮暗交界为内缩椭圆(黄经差)
static void drawMoonIcon(int cx, int cy, int R, double phase) {
  double cosP = cos(2.0 * M_PI * phase);
  tft.fillCircle(cx, cy, R, MOON_DARK);
  for (int yy = -R; yy <= R; yy++) {
    double hw = sqrt((double)(R * R - yy * yy));   // 该行的圆盘半宽
    double xl, xr;
    if (phase <= 0.5) {                             // 上弦: 亮部在右
      xl = cosP * hw; if (xl < -hw) xl = -hw;
      xr = hw;
    } else {                                          // 下弦: 亮部在左
      xl = -hw;
      xr = -cosP * hw; if (xr > hw) xr = hw;
    }
    if (xr >= xl) {
      int x0 = (int)(cx + xl + 0.5);
      int x1 = (int)(cx + xr + 0.5);
      if (x1 >= x0) tft.drawFastHLine(x0, cy + yy, x1 - x0 + 1, MOON_LIT);
    }
  }
  tft.drawCircle(cx, cy, R, MOON_EDGE);
}

// 日期条: 两行布局
//   行1: 公历+星期 | 农历+干支
//   行2: 节气(当前/下一+天数) | 月相(图标+名称)
static void drawDateRow(int year, int mon, int mday, int wday) {
  if (!font16.isReady()) return;
  int x0 = DATE_LEFT, x1 = DATE_RIGHT;
  int cx = (x0 + x1) / 2;                 // 行1 分隔线 x

  // 清场: 先把两行日期条整段(含左右溢出区)擦成背景色。
  // 否则上一次更长的文字(如 1970-01-01 的 "今日元旦")会溢出圆角矩形
  // 右缘 (x>DATE_RIGHT=449), 跨日重绘时右缘外的 "旦" 擦不掉 -> 残影。
  tft.fillRect(0, ROW1_TOP, 480, (ROW2_TOP + ROW2_H) - ROW1_TOP, COLOR_BG);

  // 农历 + 干支 (春节前取前一年, 与预览一致)
  int ld[31]; nl_month_days(year, mon, ld);
  int v = ld[mday - 1];
  int lm = v / 100, ldn = v % 100;
  bool isLeap = (lm > 100); if (isLeap) lm -= 100;
  int ly = year; if (lm >= 10 && mon <= 2) ly--;
  char lunar[32];
  snprintf(lunar, sizeof(lunar), "%s月%s%s", lunarMonths[lm - 1], lunarDays[ldn - 1],
           isLeap ? "(闰)" : "");
  char gz[16];
  snprintf(gz, sizeof(gz), "%s%s%s年", nl_tg_text[nl_tg(ly)], nl_dz_text[nl_dz(ly)],
           nl_sx_text[nl_dz(ly)]);
  char fest[16] = "";
  nl_festival(year, mon, mday, ly, lm, ldn, isLeap ? 1 : 0, fest, sizeof(fest));

  // 节气 (当前 + 下一, 含天数)
  int jqCur = -1, jqCurD = 0, jqNxt = -1, jqNxtD = 0;
  nl_jq_info(year, mon, mday, &jqCur, &jqCurD, &jqNxt, &jqNxtD);

  // 月相
  double mp = nl_moon_phase(year, mon, mday);

  // ---- 行1: 公历+星期 | 农历+干支+节日 ----
  int y1 = ROW1_TOP, h1 = ROW1_H, cy1 = y1 + h1 / 2;
  tft.fillRoundRect(x0, y1, x1 - x0, h1, 8, ROW1_BG);
  // 行1: 日期(白) + 星期(青, 与 preview 一致分开绘制) | 农历(白) + 干支(金) + 节日
  // AA 字库: 逐字 advance + 抗锯齿合成, bg 传所画行底色 (ROW1_BG)
  char date[16];
  snprintf(date, sizeof(date), "%d-%02d-%02d", year, mon, mday);
  font16.drawStringFast(x0 + 12, cy1 - 8, date, TFT_WHITE, ROW1_BG);
  int wx = x0 + 12 + font16.textWidth(date) + 8;
  font16.drawStringFast(wx, cy1 - 8, weekDaysFull[wday], TFT_CYAN, ROW1_BG);
  tft.drawFastVLine(cx, y1 + 9, h1 - 18, DATE_SEP);
  font16.drawStringFast(cx + 12, cy1 - 8, lunar, TFT_WHITE, ROW1_BG);
  int lw = font16.textWidth(lunar);
  int gx = cx + 12 + lw + 6;
  font16.drawStringFast(gx, cy1 - 8, gz, COLOR_GOLD, ROW1_BG);
  if (fest[0]) {  // 当天节日: "今日XX" + 节日色, 紧跟干支之后
    char s[16];
    snprintf(s, sizeof(s), "今日%s", fest);
    font16.drawStringFast(gx + font16.textWidth(gz) + 6, cy1 - 8, s, eventColor(fest), ROW1_BG);
  }

  // ---- 行2: 节气 | 月相 ----
  int y2 = ROW2_TOP, h2 = ROW2_H, cy2 = y2 + h2 / 2;
  tft.fillRoundRect(x0, y2, x1 - x0, h2, 8, DATE_STRIP);
  int tx = x0 + 12;
  if (jqCur >= 0 && jqNxt >= 0) {
    if (jqCurD == 0) {
      // 节气当天: "今日XX" + 节日色, 后接下一节气 (灰蓝)
      char s[16];
      snprintf(s, sizeof(s), "今日%s", nl_jq_text[jqCur]);
      font16.drawStringFast(tx, cy2 - 8, s, eventColor(nl_jq_text[jqCur]), DATE_STRIP);
      tx += font16.textWidth(s) + 10;
      char rest[24];
      snprintf(rest, sizeof(rest), "%s %d天后", nl_jq_text[jqNxt], jqNxtD);
      font16.drawStringFast(tx, cy2 - 8, rest, SOFT_C, DATE_STRIP);
    } else {
      char jqLine[48];
      snprintf(jqLine, sizeof(jqLine), "%s 已过%d天   %s %d天后",
               nl_jq_text[jqCur], jqCurD, nl_jq_text[jqNxt], jqNxtD);
      font16.drawStringFast(tx, cy2 - 8, jqLine, SOFT_C, DATE_STRIP);
    }
  } else {
    font16.drawStringFast(tx, cy2 - 8, "--", SOFT_C, DATE_STRIP);
  }
  // 右侧: 湿度 | 温度 | 月相名 | 月相图标 (自右向左, 与 make_preview.py draw_date 一致)
  int mi = (int)(mp * 8.0 + 0.5) % 8;
  char hms[8], tms[8];
  snprintf(hms, sizeof(hms), "%d%%", mockHumidity);
  snprintf(tms, sizeof(tms), "%d°C", mockTempC);
  int hR = x1 - 12;                                   // 湿度右缘
  font16.drawStringFast(hR - font16.textWidth(hms), cy2 - 8, hms, humidColor(mockHumidity), DATE_STRIP);
  int tR = hR - font16.textWidth(hms) - 8;           // 温度右缘
  font16.drawStringFast(tR - font16.textWidth(tms), cy2 - 8, tms, tempColor(mockTempC), DATE_STRIP);
  int mR = tR - font16.textWidth(tms) - 8;           // 月相名右缘
  int mTw = font16.textWidth(nl_moon_text[mi]);
  font16.drawStringFast(mR - mTw, cy2 - 8, nl_moon_text[mi], 0xDE72, DATE_STRIP);  // 淡金
  int R = 9;
  int mcx = mR - mTw - 6 - R;
  drawMoonIcon(mcx, cy2, R, mp);
}

void modeClock() {
  time_t now = time(nullptr);
  struct tm tm_info;
  localtime_r(&now, &tm_info);
  int h = tm_info.tm_hour, m = tm_info.tm_min, s = tm_info.tm_sec;
  int year = tm_info.tm_year + 1900;
  int mon = tm_info.tm_mon + 1;
  int mday = tm_info.tm_mday;
  int wday = tm_info.tm_wday;

  // 首次进入: 清场 + 按当前样式绘制 (翻页 6 张牌 / 指针表盘) + 日期条
  if (!modeDrawn[MODE_CLOCK] || modeChanged) {
    tft.fillRect(0, 0, 480, NAV_BAR_Y, COLOR_BG);
    if (clockStyle == STYLE_FLAP) {
      char cur[7];
      snprintf(cur, sizeof(cur), "%02d%02d%02d", h, m, s);
      for (int i = 0; i < 6; i++) { drawCard(i, cur[i], false); flipCard[i].current = cur[i]; }
      drawModeHint(STYLE_FLAP);
    } else {
      drawAnalogClock(h, m, s);
    }
    // 顶部状态行 (WiFi 状态) — 仅翻页样式显示, 指针样式表盘更大不需要
    if (clockStyle == STYLE_FLAP) {
      tft.setTextColor(WiFi.status() == WL_CONNECTED ? TFT_CYAN : 0xF800);
      tft.setFreeFont(&FreeSansBold24pt7b);
      tft.setTextSize(1);
      tft.setTextDatum(TL_DATUM);
      tft.drawString(WiFi.status() == WL_CONNECTED ? "NTP OK" : "NTP --", 10, 20);
    }
    // 日期条 (农历/干支/节气/月相 均在 drawDateRow 内计算)
    drawDateRow(year, mon, mday, wday);
    checkFestivalMusic(year, mon, mday);   // 节日/节气音乐 (每天一次)
    prevH = h; prevM = m; prevS = s;
    modeDrawn[MODE_CLOCK] = true;
    return;
  }

  if (clockStyle == STYLE_ANALOG) {
    // 指针样式: 秒针平滑扫秒, 每帧重绘表盘
    drawAnalogClock(h, m, s);
  } else {
    // 翻页样式: 目标时间字符串 HH MM SS (6 位 + null)
    char target[7];
    snprintf(target, sizeof(target), "%02d%02d%02d", h, m, s);

    if (h != prevH || m != prevM || s != prevS) {
      prevH = h; prevM = m; prevS = s;
      // 从个位(秒)向高位级联翻页; 秒的进位会自然触发分/时重绘
      for (int i = 5; i >= 0; i--) {
        if (target[i] != flipCard[i].current) {
          flipDigitAnim(i, target[i]);
          flipCard[i].current = target[i];
        }
      }
    }
  }

  // 日期变化(跨日/跨月)时重绘日期条
  static int lastDay = -1, lastMon = -1, lastYear = -1;
  if (mday != lastDay || mon != lastMon || year != lastYear) {
    lastDay = mday; lastMon = mon; lastYear = year;
    drawDateRow(year, mon, mday, wday);
    checkFestivalMusic(year, mon, mday);   // 跨日 -> 检查节日/节气音乐
  }
}

void modeCalendar() {
  if (!modeDrawn[MODE_CALENDAR] || modeChanged) {
    tft.fillRect(0, 0, 480, NAV_BAR_Y, COLOR_BG);
    font24.drawString(160, 100, "万年历", TFT_CYAN, COLOR_BG);
    modeDrawn[MODE_CALENDAR] = true;
  }
}

void drawWeatherScreen() {
  tft.fillRect(0, 0, 480, NAV_BAR_Y, COLOR_BG);
  font16.drawString(10, 5, "天气", COLOR_ACCENT, COLOR_BG);
  font24.drawString(100, 100, "待接入和风天气API", TFT_YELLOW, COLOR_BG);
}

void modeWeather() {
  if (!modeDrawn[MODE_WEATHER] || modeChanged) {
    drawWeatherScreen();
    modeDrawn[MODE_WEATHER] = true;
  }
}

void modeRadio() {
  if (!radioInited) return;

  if (showingFullscreen) {
    if (!modeDrawn[MODE_RADIO]) {
      drawRadioHeader(); drawRadioRegionSidebar(); drawRadioList(); drawRadioPlayBar(); drawStationFullscreen();
      modeDrawn[MODE_RADIO] = true;
    }
    if (radioPlaying) audio.loop();
    return;
  }

  if (!modeDrawn[MODE_RADIO] || modeChanged) {
    tft.fillRect(0, 0, 480, NAV_BAR_Y, COLOR_BG);
    drawRadioHeader(); drawRadioRegionSidebar(); drawRadioList(); drawRadioPlayBar();
    modeDrawn[MODE_RADIO] = true; listScrollDirty = false;
  }

  if (listScrollDirty) {
    drawRadioList(); drawRadioPlayBar(); listScrollDirty = false;
  }

  if (radioPlaying) audio.loop();
}

// ============================================================
// 收音机数据加载
// ============================================================
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
  Serial.printf("stations.bin: %d regions, %lu stations\n", numRegions, numStations);
  f.seek(regionIdxOffset);
  regionEntries = new RegionEntry[numRegions];
  for (uint16_t i = 0; i < numRegions; i++) {
    f.read((uint8_t*)regionEntries[i].name, 16); regionEntries[i].name[16] = 0;
    regionEntries[i].first_offset = f.read() | (f.read() << 8) | (f.read() << 16) | (f.read() << 24);
    regionEntries[i].count = f.read() | (f.read() << 8);
    f.seek(f.position() + 6);
  }
  regionCount = numRegions; curRegionIdx = 0;
  loadRegionStations(curRegionIdx);
  radioInited = true;
  f.close(); return true;
}

void loadRegionStations(int regionIdx) {
  if (regionIdx < 0 || regionIdx >= regionCount) return;
  File f = LittleFS.open("/stations.bin", "r");
  if (!f) return;
  uint32_t offset = regionEntries[regionIdx].first_offset;
  uint16_t count = regionEntries[regionIdx].count;
  if (curRegionStations) { delete[] curRegionStations; curRegionStations = nullptr; }
  curRegionStations = new StationView[count]; curRegionCount = count;
  f.seek(offset);
  for (int i = 0; i < count; i++) {
    uint32_t recordStart = f.position();
    uint8_t nameLen = f.read(); uint8_t rawName[61]; f.read(rawName, nameLen); rawName[nameLen] = 0;
    if (nameLen <= 31) memcpy(curRegionStations[i].name, rawName, nameLen + 1);
    else { memcpy(curRegionStations[i].name, rawName, 28); curRegionStations[i].name[28]=curRegionStations[i].name[29]=curRegionStations[i].name[30]='.'; curRegionStations[i].name[31]=0; }
    uint16_t urlLen = f.read() | (f.read() << 8); f.seek(f.position() + urlLen);
    curRegionStations[i].region_idx = f.read();
    uint8_t catLen = f.read(); f.seek(f.position() + catLen);
    uint8_t freqLen = f.read(); uint8_t rawFreq[21]; f.read(rawFreq, freqLen); rawFreq[freqLen] = 0;
    if (freqLen <= 12) memcpy(curRegionStations[i].freq, rawFreq, freqLen + 1);
    else { memcpy(curRegionStations[i].freq, rawFreq, 12); curRegionStations[i].freq[12] = 0; }
    curRegionStations[i].r = f.read(); curRegionStations[i].g = f.read(); curRegionStations[i].b = f.read();
    curRegionStations[i].bin_offset = recordStart;
  }
  f.close();
}

String readStationURL(uint32_t binOffset) {
  File f = LittleFS.open("/stations.bin", "r");
  if (!f) return "";
  f.seek(binOffset);
  uint8_t nameLen = f.read(); f.seek(f.position() + nameLen);
  uint16_t urlLen = f.read() | (f.read() << 8);
  uint8_t urlBuf[257]; f.read(urlBuf, min(urlLen, (uint16_t)256));
  urlBuf[min(urlLen, (uint16_t)256)] = 0; f.close();
  return String((char*)urlBuf);
}

void playStationAt(int listIdx) {
  if (listIdx < 0 || listIdx >= curRegionCount) return;
  if (currentPlayingIdx == listIdx && radioPlaying) return;
  String url = readStationURL(curRegionStations[listIdx].bin_offset);
  if (url.length() == 0) return;
  if (radioPlaying) audio.stopSong();
  musicPlaying = false;               // 点电台 -> 停掉节日音乐 (收音机优先)
  Serial.printf("Playing [%d]: %s\n", listIdx, curRegionStations[listIdx].name);
  audio.connecttohost(url.c_str());
  radioPlaying = true; currentPlayingIdx = listIdx;
  digitalWrite(STATUS_LED_PIN, HIGH);
}

void stopRadio() { audio.stopSong(); radioPlaying = false; digitalWrite(STATUS_LED_PIN, LOW); }

// ============================================================
// 节日/节气 音乐 (网络流, 与收音机同一路径, 不占 flash)
// 注意: 与收音机共用同一个 audio 解码器, 二者互斥; 收音机始终优先。
// ============================================================
// 音乐 URL 配置来自 LittleFS 文件 /festival.json (用 `pio run -t uploadfs`
// 上传, 改配置无需重新编译固件, 改完上传后重启即可生效):
//   { "festival_music": { "春节": "http://...", "冬至": "http://..." } }
//   URL 为空 = 该事件不播音乐。文件缺失/损坏 = 不播 (显示不受影响)。
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

// 当天事件名: 优先节日, 其次节气(仅节气当天), 无则空
static String todayEventName(int year, int mon, int mday) {
  int ld[31]; nl_month_days(year, mon, ld);
  int v = ld[mday - 1];
  int lm = v / 100, ldn = v % 100;
  bool isLeap = (lm > 100); if (isLeap) lm -= 100;
  int ly = year; if (lm >= 10 && mon <= 2) ly--;   // 春节前取前一年 (与 drawDateRow 一致)
  char fest[16] = "";
  nl_festival(year, mon, mday, ly, lm, ldn, isLeap ? 1 : 0, fest, sizeof(fest));
  if (fest[0]) return String(fest);
  int jqCur = -1, jqCurD = 0;
  if (nl_jq_info(year, mon, mday, &jqCur, &jqCurD, nullptr, nullptr) && jqCurD == 0)
    return String(nl_jq_text[jqCur]);
  return String();
}

// 每天只跑一次: 有音乐配置且已联网且收音机未在播 -> 开播; 无事件则停掉昨天的流
void checkFestivalMusic(int year, int mon, int mday) {
  static int lastY = -1, lastM = -1, lastD = -1;
  if (year == lastY && mon == lastM && mday == lastD) return;
  lastY = year; lastM = mon; lastD = mday;

  String ev = todayEventName(year, mon, mday);
  if (ev.length() == 0) {
    if (musicPlaying) {  // 今天没有事件, 停掉
      audio.stopSong(); musicPlaying = false;
      if (!radioPlaying) digitalWrite(STATUS_LED_PIN, LOW);
    }
    return;
  }
  String url = festivalUrl(ev.c_str());
  if (url.length() == 0) return;      // 未配置该事件的音乐
  if (!wifiConnected) return;         // 离线不播 (显示不受影响)
  if (radioPlaying) return;           // 收音机优先
  audio.stopSong();
  Serial.printf("Festival music: %s -> %s\n", ev.c_str(), url.c_str());
  audio.connecttohost(url.c_str());
  musicPlaying = true;
  digitalWrite(STATUS_LED_PIN, HIGH);
}

// ============================================================
// 收藏
// ============================================================
void loadFavorites() {
  File f = LittleFS.open("/favorites.bin", "r");
  if (!f) { favoriteCount = 0; return; }
  favoriteCount = 0;
  while (f.available() && favoriteCount < MAX_FAVORITES) {
    uint32_t off = f.read() | (f.read() << 8) | (f.read() << 16) | (f.read() << 24);
    favorites[favoriteCount++] = off;
  }
  f.close();
  Serial.printf("Loaded %d favorites\n", favoriteCount);
}

void saveFavorites() {
  File f = LittleFS.open("/favorites.bin", "w");
  if (!f) return;
  for (int i = 0; i < favoriteCount; i++) {
    uint32_t off = favorites[i];
    f.write(off & 0xFF); f.write((off >> 8) & 0xFF); f.write((off >> 16) & 0xFF); f.write((off >> 24) & 0xFF);
  }
  f.close();
}

bool isFavorited(uint32_t binOffset) {
  for (int i = 0; i < favoriteCount; i++) if (favorites[i] == binOffset) return true;
  return false;
}

void toggleFavorite(uint32_t binOffset) {
  for (int i = 0; i < favoriteCount; i++) {
    if (favorites[i] == binOffset) {
      for (int j = i; j < favoriteCount - 1; j++) favorites[j] = favorites[j+1];
      favoriteCount--; saveFavorites(); return;
    }
  }
  if (favoriteCount < MAX_FAVORITES) { favorites[favoriteCount++] = binOffset; saveFavorites(); }
}

// ============================================================
// Audio 回调
// ============================================================
void audio_info(const char* info) { Serial.printf("info: %s\n", info); }
void audio_showstation(const char* info) { Serial.printf("station: %s\n", info); }
void audio_showstreamtitle(const char* info) { Serial.printf("stream: %s\n", info); }

// ============================================================
// setup / loop
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("ESP32-S3 Smart Radio Starting...");

  // ================== 上电自检 (BOOT SELF-TEST) ==================
  // 用 TFT_eSPI 内置字体(不依赖 LittleFS / 自定义 XFont)在屏幕上逐步显示,
  // 这样在没有串口监视器时也能直接看出黑屏停在哪一步。
  initTFT();
  tft.setTextFont(2);                 // 内置 16px 字体 (User_Setup.h 中 LOAD_FONT2)
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(0, 0);
  tft.println("ESP32-S3 Radio");
  tft.println("Boot self-test...");

  // 1) 文件系统 (黑屏最常见原因: 未烧录文件系统 uploadfs)
  bool fsOk = LittleFS.begin();
  Serial.printf("LittleFS: %s\n", fsOk ? "OK" : "FAIL");
  tft.print("FS: ");
  if (fsOk) tft.print(String(LittleFS.totalBytes() / 1024)); else tft.print("FAIL");
  tft.println(" KB");

  // 1b) 节日/节气 音乐配置 (可选, 缺失/损坏则不播)
  if (fsOk) loadFestivalConfig();

  // 1c) 时钟样式 (NVS 持久化: 翻页 / 指针, 点击时钟页可切换)
  clockStyle = getClockStyle();
  Serial.printf("clock style: %s\n", clockStyle == STYLE_FLAP ? "flap" : "analog");

  // 2) 触摸
  initTouch();

  // 3) 中文字库 (加载失败 -> 后续所有中文都是空白, 看起来像黑屏)
  bool f16 = font16.reInit("/font_simhei_16.font");
  bool f24 = font24.reInit("/font_simhei_24.font");
  Serial.printf("font16: %s  font24: %s\n", f16 ? "OK" : "FAIL", f24 ? "OK" : "FAIL");
  tft.print("Font16: "); tft.println(f16 ? "OK" : "MISSING");
  tft.print("Font24: "); tft.println(f24 ? "OK" : "MISSING");

  // 4) WiFi + NTP
  Serial.println("Connecting WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin("EX-Dillon", "yjsd@1224");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.printf("WiFi IP: %s\n", WiFi.localIP().toString().c_str());
    configTime(gmtOffsetSec, 0, ntpServer);
    delay(1000);
    tft.print("WiFi: " + WiFi.localIP().toString());
    tft.println();
  } else {
    Serial.println("WiFi connect failed, continue offline");
    tft.print("WiFi: ");
    tft.println("FAIL");
  }

  // 5) 电台数据库 (bad format / 内存不足时 new[] 会崩溃)
  Serial.println("Loading radio DB...");
  bool dbOk = loadRadioDB();
  if (dbOk) tft.printf("%d reg / %d stn\n", regionCount, curRegionCount);
  else tft.println("DB: MISSING");
  Serial.printf("DB regions=%d\n", regionCount);

  loadFavorites();
  audio.setPinout(I2S_BCK_PIN, I2S_LRC_PIN, I2S_DOUT_PIN);
  audio.setBufsize(16384, 65536);
  audio.setConnectionTimeout(200, 2000);

  tft.println("READY - start main UI");
  Serial.println("Setup done.");
  modeChanged = true;
}

void loop() {
  handleTouch();
  switch (currentMode) {
    case MODE_CLOCK: modeClock(); break;
    case MODE_CALENDAR: modeCalendar(); break;
    case MODE_RADIO: modeRadio(); break;
    case MODE_WEATHER: modeWeather(); break;
  }
  // 节日音乐在任意界面都继续播 (收音机只在 modeRadio 内泵送, 此处不重复)
  if (musicPlaying) audio.loop();

  // 只在 modeChanged 时重绘导航栏, 消除每帧重绘引起的闪烁
  if (modeChanged) { drawNavBar(); modeChanged = false; }
}
// ============================================================
// handleRadioTap: 处理收音机模式的点击事件 (滚动已在 handleTouch 处理)
// ============================================================
void handleRadioTap(uint16_t tx, uint16_t ty) {
  // 侧边栏分类
  if (tx < RADIO_SIDEBAR_W) {
    int regionY = ty - RADIO_HEADER_H;
    if (regionY >= 0) {
      int clicked = regionScrollTop + (regionY / 18);
      if (clicked >= 0 && clicked < regionCount && clicked != curRegionIdx) {
        curRegionIdx = clicked;
        loadRegionStations(curRegionIdx);
        listScrollTop = 0;
        listScrollDirty = true;
        modeDrawn[MODE_RADIO] = false;
      }
    }
    return;
  }

  // 播放栏按钮
  if (ty >= RADIO_LIST_BOT && ty < NAV_BAR_Y) {
    int barX = RADIO_LIST_X, barW = RADIO_LIST_W;
    if (tx >= barX && tx < barX + barW) {
      int relX = tx - barX;
      if (relX < 36) {
        // 收藏
        if (currentPlayingIdx >= 0) {
          toggleFavorite(curRegionStations[currentPlayingIdx].bin_offset);
          listScrollDirty = true;
        }
      } else if (relX < 76) {
        // 上一台
        if (currentPlayingIdx > 0) playStationAt(currentPlayingIdx - 1);
      } else if (relX < 116) {
        // 播放/暂停
        if (radioPlaying) stopRadio();
        else if (currentPlayingIdx >= 0) playStationAt(currentPlayingIdx);
        listScrollDirty = true;
      } else if (relX < 156) {
        // 下一台
        if (currentPlayingIdx < curRegionCount - 1) playStationAt(currentPlayingIdx + 1);
      }
    }
    return;
  }

  // 滚动条点击 → 跳转
  if (ty >= RADIO_LIST_TOP && ty < RADIO_LIST_BOT &&
      tx >= RADIO_LIST_X + RADIO_LIST_W && tx < RADIO_LIST_X + RADIO_LIST_W + RADIO_SCROLLBAR_W) {
    int scrollRange = max(1, curRegionCount - RADIO_STATION_ROWS);
    int listH = RADIO_LIST_BOT - RADIO_LIST_TOP;
    int scrollPos = (ty - RADIO_LIST_TOP) * scrollRange / listH;
    scrollPos = constrain(scrollPos, 0, max(0, curRegionCount - RADIO_STATION_ROWS));
    if (scrollPos != listScrollTop) {
      listScrollTop = scrollPos;
      listScrollDirty = true;
    }
    return;
  }

  // 全屏模式下点击 → 关闭
  if (showingFullscreen) {
    showingFullscreen = false;
    modeDrawn[MODE_RADIO] = false;
    return;
  }

  // 列表区域: 单击播放, 双击全屏
  if (ty >= RADIO_LIST_TOP && ty < RADIO_LIST_BOT && tx >= RADIO_LIST_X) {
    int listY = ty - RADIO_LIST_TOP;
    int rowIdx = listScrollTop + (listY / RADIO_ROW_H);
    if (rowIdx >= 0 && rowIdx < curRegionCount) {
      unsigned long now = millis();
      if (now - lastTapTime < 350 && lastTapRow == rowIdx) {
        showingFullscreen = true;
        modeDrawn[MODE_RADIO] = false;
      } else {
        playStationAt(rowIdx);
        listScrollDirty = true;
      }
      lastTapTime = now;
      lastTapRow = rowIdx;
    }
    return;
  }
}

void drawRadioHeader() {
  tft.fillRect(0, 0, 480, RADIO_HEADER_H, 0x1042);

  char title[40];
  snprintf(title, sizeof(title), "收音机 · %s (%d台)",
           regionEntries[curRegionIdx].name, curRegionCount);
  font16.drawString(6, 6, title, TFT_WHITE, 0x1042);

  if (wifiConnected) {
    String ip = WiFi.localIP().toString();
    font16.drawString(470 - font16.textWidth(ip.c_str()), 6, ip.c_str(), COLOR_DIM, 0x1042);
  } else {
    font16.drawString(470 - 48, 6, "未联网", TFT_RED, 0x1042);
  }

  tft.drawFastVLine(RADIO_SIDEBAR_W - 1, RADIO_HEADER_H, RADIO_LIST_BOT - RADIO_HEADER_H, 0x3186);
}

void drawRadioRegionSidebar() {
  int y = RADIO_HEADER_H;
  int visibleCount = (RADIO_LIST_BOT - RADIO_HEADER_H) / 18;

  tft.fillRect(0, RADIO_HEADER_H, RADIO_SIDEBAR_W, RADIO_LIST_BOT - RADIO_HEADER_H, 0x0821);

  for (int i = 0; i < visibleCount && (regionScrollTop + i) < regionCount; i++) {
    int idx = regionScrollTop + i;
    const char* name = regionEntries[idx].name;
    uint16_t bg = (idx == curRegionIdx) ? 0x210C : 0x0821;
    uint16_t fg = (idx == curRegionIdx) ? TFT_YELLOW : COLOR_DIM;

    tft.fillRect(0, y, RADIO_SIDEBAR_W - 1, 18, bg);
    if (idx == curRegionIdx) {
      tft.drawFastVLine(0, y, 18, TFT_YELLOW);
    }
    font16.drawString(4, y + 2, name, fg, bg);
    y += 18;
  }

  tft.drawFastVLine(RADIO_SIDEBAR_W - 1, RADIO_HEADER_H, RADIO_LIST_BOT - RADIO_HEADER_H, 0x3186);
}

void drawRadioScrollbar() {
  int listH = RADIO_LIST_BOT - RADIO_LIST_TOP;
  int scrollRange = max(1, curRegionCount - RADIO_STATION_ROWS);

  // 背景轨道
  tft.fillRect(RADIO_LIST_X + RADIO_LIST_W, RADIO_LIST_TOP, RADIO_SCROLLBAR_W, listH, 0x1082);

  // 滑块
  int thumbH = max(20, listH * RADIO_STATION_ROWS / curRegionCount);
  int thumbY = RADIO_LIST_TOP;
  if (scrollRange > 0) {
    thumbY = RADIO_LIST_TOP + (listH - thumbH) * listScrollTop / scrollRange;
  }
  tft.fillRect(RADIO_LIST_X + RADIO_LIST_W + 1, thumbY, RADIO_SCROLLBAR_W - 2, thumbH, 0xFFFF);
}

void drawRadioList() {
  int listX = RADIO_LIST_X;
  int listY = RADIO_LIST_TOP;
  int listH = RADIO_LIST_BOT - RADIO_LIST_TOP;

  tft.fillRect(listX, listY, RADIO_LIST_W, listH, COLOR_BG);

  if (curRegionCount == 0) {
    font24.drawString(listX + 50, listY + 50, "暂无电台", TFT_RED, COLOR_BG);
    return;
  }

  int startRow = listScrollTop;
  int endRow = startRow + RADIO_STATION_ROWS;
  if (endRow > curRegionCount) endRow = curRegionCount;

  for (int i = startRow; i < endRow; i++) {
    int rowY = listY + (i - startRow) * RADIO_ROW_H;
    bool playing = (i == currentPlayingIdx && radioPlaying);
    drawStationRow(rowY, curRegionStations[i], playing);
  }

  drawRadioScrollbar();
}

void drawStationRow(int rowY, StationView& st, bool playing) {
  int listX = RADIO_LIST_X;
  uint16_t bg = playing ? 0x1843 : COLOR_BG;

  tft.fillRect(listX, rowY, RADIO_LIST_W, RADIO_ROW_H, bg);

  // 圆形图标
  int iconCx = listX + 22;
  int iconCy = rowY + RADIO_ROW_H / 2;
  uint16_t col = ((uint16_t)st.r << 11) | ((uint16_t)(st.g >> 2) << 5) | (uint16_t)(st.b >> 3);
  if (col == 0) col = 0xCCCC;
  tft.fillCircle(iconCx, iconCy, 16, col);
  tft.drawCircle(iconCx, iconCy, 17, 0xFFFF);

  // 图标内文字
  char iconText[5];
  if (strlen(st.freq) >= 2) {
    int flen = strlen(st.freq);
    strncpy(iconText, st.freq + flen - 2, 2);
    iconText[2] = 0;
  } else if (strlen(st.name) > 0) {
    int clen = (uint8_t)st.name[0] >= 0xE0 ? 3 : ((uint8_t)st.name[0] >= 0xC0 ? 2 : 1);
    strncpy(iconText, st.name, clen);
    iconText[clen] = 0;
  } else {
    strcpy(iconText, "?");
  }
  font16.drawString(iconCx - 8, iconCy - 8, iconText, TFT_WHITE, col);

  // 电台名
  int nameX = listX + 46;
  font16.drawString(nameX, rowY + 4, st.name, TFT_WHITE, bg);

  // 频率/地区
  char info[40];
  snprintf(info, sizeof(info), "%s · %s", st.freq, regionEntries[st.region_idx].name);
  font16.drawString(nameX, rowY + 28, info, COLOR_DIM, bg);

  // 播放/收藏图标
  if (playing) {
    tft.fillTriangle(listX + RADIO_LIST_W - 14, rowY + 16,
                     listX + RADIO_LIST_W - 6, rowY + 12,
                     listX + RADIO_LIST_W - 6, rowY + 20, TFT_GREEN);
  } else {
    // 爱心
    uint16_t hc = isFavorited(st.bin_offset) ? TFT_RED : COLOR_DIM;
    int ax = listX + RADIO_LIST_W - 14;
    int ay = rowY + RADIO_ROW_H / 2 - 2;
    // 简化爱心: 用两个圆 + 三角形
    tft.fillCircle(ax - 4, ay - 2, 4, hc);
    tft.fillCircle(ax + 4, ay - 2, 4, hc);
    tft.fillTriangle(ax - 8, ay, ax + 8, ay, ax, ay + 8, hc);
  }

  // 行分隔线
  tft.drawFastHLine(listX, rowY + RADIO_ROW_H - 1, RADIO_LIST_W, 0x2104);
}

void drawRadioPlayBar() {
  int barY = RADIO_LIST_BOT;
  int barH = RADIO_PLAYBAR_H;
  int barX = RADIO_LIST_X;
  int barW = RADIO_LIST_W;

  // 播放栏背景
  tft.fillRect(barX, barY, barW, barH, 0x0821);
  tft.drawFastHLine(barX, barY, barW, 0x3186);

  if (currentPlayingIdx < 0 || currentPlayingIdx >= curRegionCount) {
    font16.drawString(barX + 10, barY + 20, "点击电台开始播放", COLOR_DIM, 0x0821);
    return;
  }

  StationView& st = curRegionStations[currentPlayingIdx];

  // 播放状态标签
  uint16_t labelColor = radioPlaying ? 0x07E0 : 0xFBE0;
  tft.fillRect(barX + 4, barY + 8, 28, 14, labelColor);
  uint16_t labelFg = radioPlaying ? COLOR_BG : COLOR_BG;
  font16.drawString(barX + 8, barY + 10, radioPlaying ? "LIVE" : "PAUSE", labelFg, labelColor);

  // 当前电台名
  font16.drawString(barX + 40, barY + 8, st.name, TFT_WHITE, 0x0821);
  char info[40];
  snprintf(info, sizeof(info), "%s · %s", st.freq, regionEntries[st.region_idx].name);
  font16.drawString(barX + 40, barY + 26, info, COLOR_DIM, 0x0821);

  // 右侧收藏按钮
  bool fav = isFavorited(st.bin_offset);
  uint16_t hc = fav ? TFT_RED : 0x5A5A;
  int fx = barX + barW - 18;
  int fy = barY + 30;
  tft.fillCircle(fx - 4, fy - 2, 4, hc);
  tft.fillCircle(fx + 4, fy - 2, 4, hc);
  tft.fillTriangle(fx - 8, fy, fx + 8, fy, fx, fy + 8, hc);

  // 控件按钮 (播放栏底部): 收藏 | 上 | 播放/暂停 | 下
  int ctrlY = barY + 44;
  tft.drawFastHLine(barX, ctrlY - 2, barW, 0x2104);

  // 上一台
  int px = barX + 40;
  tft.fillTriangle(px + 8, ctrlY + 8, px + 24, ctrlY + 2, px + 24, ctrlY + 14, TFT_WHITE);
  tft.fillRect(px + 22, ctrlY + 2, 4, 12, TFT_WHITE);

  // 播放/暂停
  int ppX = barX + 72;
  int ppCx = ppX + 20;
  int ppCy = ctrlY + 8;
  uint16_t ppBg = radioPlaying ? 0xFBE0 : 0x07E0;
  tft.fillCircle(ppCx, ppCy, 14, ppBg);
  if (radioPlaying) {
    // 暂停: 两竖条
    tft.fillRect(ppCx - 6, ppCy - 5, 4, 10, COLOR_BG);
    tft.fillRect(ppCx + 2, ppCy - 5, 4, 10, COLOR_BG);
  } else {
    // 播放: 三角形
    tft.fillTriangle(ppCx - 4, ppCy - 7, ppCx + 6, ppCy, ppCx - 4, ppCy + 7, COLOR_BG);
  }

  // 下一台
  int nx = barX + 120;
  tft.fillTriangle(nx + 8, ctrlY + 2, nx + 24, ctrlY + 8, nx + 8, ctrlY + 14, TFT_WHITE);
  tft.fillRect(nx, ctrlY + 2, 4, 12, TFT_WHITE);
}

void drawStationFullscreen() {
  int cx = 240, cy = 140;

  // 半透明遮罩
  tft.fillRect(0, 0, 480, NAV_BAR_Y, 0x8000);

  if (currentPlayingIdx < 0 || currentPlayingIdx >= curRegionCount) return;
  StationView& st = curRegionStations[currentPlayingIdx];

  // 卡片背景
  tft.fillRoundRect(60, 40, 360, 200, 8, 0x1042);

  // 大圆形图标
  uint16_t col = ((uint16_t)st.r << 11) | ((uint16_t)(st.g >> 2) << 5) | (uint16_t)(st.b >> 3);
  if (col == 0) col = 0xCCCC;
  tft.fillCircle(cx, 80, 40, col);

  char iconText[5];
  if (strlen(st.freq) >= 2) {
    int flen = strlen(st.freq);
    strncpy(iconText, st.freq + flen - 2, 2);
    iconText[2] = 0;
  } else {
    strcpy(iconText, "?");
  }
  font16.drawString(cx - 12, 72, iconText, TFT_WHITE, col);

  // 电台名
  font24.drawString(cx - font24.textWidth(st.name) / 2, 130, st.name, TFT_WHITE, 0x1042);

  // 详细信息
  char info[80];
  snprintf(info, sizeof(info), "%s · %s", st.freq, regionEntries[st.region_idx].name);
  font16.drawString(cx - font16.textWidth(info) / 2, 165, info, COLOR_DIM, 0x1042);

  // 收藏按钮
  bool fav = isFavorited(st.bin_offset);
  font16.drawString(cx - 20, 195, fav ? "★ 已收藏" : "☆ 收藏", fav ? TFT_YELLOW : COLOR_DIM, 0x1042);

  // 提示 (点任意处关闭)
  font16.drawString(cx - 48, 225, "点屏幕任意处关闭", COLOR_DIM, 0x1042);
}
