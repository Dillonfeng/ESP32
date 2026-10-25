/*
 * ESP32-S3 网络收音�?- 主程�?v2
 * ===============================
 * �?�?: ESP32-S3-N16R8 + ST7796S 480x320 + XPT2046 + PCM5102A + INMP441
 * 
 * 参�?��?��?? yoradio (widget布局), tftziku (�?文字�?), ESP32-audioI2S
 * 
 * 功能模块:
 * 1. 翻页时钟 (数码管�?�格数�??+ �?文日�?)
 * 2. 万年�?(农历/干支/生肖/节气/定制节日)
 * 3. 网络收音�?(HLS+HTTPS, 电台列表, 触摸控制)
 * 4. 天气显示
 * 
 * 切换方式: 底部触摸导航�?�?按�??
 *
 * ⚠️ 注意: �?文件已手动改写�??机�?�翻�?(Split-Flap)时钟 + �?部刷�?�?
 *    请勿再运�?scripts/rebuild_main.py —�??它会覆盖这些手动改动�?
 */

#include <Arduino.h>
#include <esp_log.h>          // BUILDV5-STEP3-TFIX2: esp_log_level_set 全局日志开关
#include <SPI.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <time.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>
#include <nvs.h>
#include <TFT_eSPI.h>   // 已包�?Fonts/GFXFF/* (FreeSansBold24pt7b �?
#include <Audio.h>
#include <ArduinoJson.h>
#include "xfont.h"
#include "nongli.h"
#include "digits.h"
#include "dialnum.h"
#include "almnum.h"
#include "holiday_sync.h"
#include "geo_pin.h"
#include "weather_sync.h"
#include "wicons.h"
using weather::WData;  // WData 在 weather 命名空间内, 天气页绘制代码按裸名使用
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"     // BUILDV5-TWDTRESET: netTask 豁免 TASK_WDT（防网络阻塞复位）
#include "ui_common.h"   // BUILD61: 拆出模块共享声明
#include "touch_driver.h"  // BUILDV5-TOUCHDRV: 自写 XPT2046 驱动（触摸独立任务）
#include "driver/i2s.h"
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// 调试提示�?�? 1=调试期显�?点屏幕切�?等提�? 正式产品�?0 全部隐藏
// (�?preview/make_preview.py �?DEBUG_UI 保持�?�?�?
#define DEBUG_UI 0

// ========== 引脚定义 ==========
#define I2S_BCK_PIN     6
#define I2S_LRC_PIN     7
#define I2S_DOUT_PIN    8
#define STATUS_LED_PIN  2
#define TFT_BL_PIN      21

// ========== 全局对象 ==========
TFT_eSPI tft = TFT_eSPI();
// 指针表盘: 每帧�?(静�?�表�?白圆+刻度+12数字) + (3 根指�? 全部合成到这块内�?RAM 缓冲
// (146x146 = 42.6KB), 再�?�单次�?�pushImage 整帧。关�?整帧�?次推�?原子�? �?TE 同�?��??
// 面板持续�?�?, �?推表�?擦掉�? -> 再分几笔画针"会有�?间露针空�? -> 3 针闪�?+ 秒针
// 像不动�?�合成到同一缓冲�?次推, 与已验证干净的翻页时钟同�?(�?pushImage, 无中间状�?�?
// 静�?�表盘缓�?(白圆+刻度+12数字), �?次�?�构建后每帧 memcpy 复用�?
// 原实现每帧调 buildAnalogFace 重建全表 (~50-100ms on ESP32-S3), 实际 FPS 远低�?
// ANALOG_FRAME_MS �?�? (30 FPS), 秒针每帧仅推�?<0.1px �?视�?��??冻结"�?
// 缓存后每帧只 memcpy 42.6KB (~1ms) + �?3 �? 实际 FPS 回到 ~30, 秒针平滑�?秒�??
XFont font16(&tft);
XFont font32(&tft);   // 64px AA 字库 (黄历大字; ver3, ~1.0MB PSRAM; Flash 直读)
// Legacy audioI2S (3.0.12e + HLS BUILD42/46/48 patches): default ctor,
// external I2S DAC configured via setPinout() in setup.
Audio audio(1);  // BUILDV5-TEST: I2S1 (was I2S0, silent on v5)

// BUILDV5-HLSFIX: 全局 SPI 总线互斥（见 ui_common.h 注释）——touchScanTask 采样
// 与 main loop 显示绘制串行化，消除 FSPI 并发崩溃（cap_fix8 链A）。
SemaphoreHandle_t g_spiMutex = nullptr;
// BUILDV5-HLSFIX: 全局打印锁——Audio 库 AUDIO_LOG_IMPL 与 main 侧残留打印共用
// （cap_fix11 链B 实证：变参模板每实例一把 static 锁 → 跨实例不互斥 → UART 崩溃）。
// main 侧常规打印已删，仅错误级/一次性保留并走本锁。
SemaphoreHandle_t g_printMutex = nullptr;

// ========== 触摸校准参数 ==========
uint16_t touchCalib[] = {194, 3767, 174, 3844, 0x01};

// ========== UI 页面定义 ==========
UIMode currentMode = MODE_CLOCK;
bool modeChanged = true;
// 每个模式�?立�??已绘�?标志，切换时全部重置�?false
bool modeDrawn[MODE_COUNT] = {false, false, false, false};
// 导航栏重绘标�? 仅在"选中的�?�航项变�?"时重绘�?�时钟样式切�?翻页<->指针)不改变�?��??
// 选中�? 若�?�时也重绘�?�航�? �?fillRect+4 �?AA �?4 条分隔线 多笔 SPI 会与整页重绘
// 交错, �?TE 面板�?描撕�? -> 收音�?天气 之间出现"彩色竖条"(长度随机)。故样式切换�?�?
// modeChanged(重绘页面), 不置 navDirty(不动导航�?�?
bool navDirty = true;

// ========== 导航栏布�? ==========
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
#define COLOR_DIGIT    0x07FF  // 青色数码�?
#define COLOR_DIM      0x7BEF  // 暗灰

// ========== WiFi/NTP 状�??==========
bool wifiConnected = false;
bool ntpSynced = false;
bool wifiConnecting = false;
unsigned long wifiConnectStart = 0;
const char* ntpServer = "ntp.aliyun.com";
const int gmtOffsetSec = 8 * 3600;

// ========== 时间来源 ==========
// 通过 WiFi + NTP 同�?�时�? (�?联网)�?

// ========== 时钟状�??==========
int prevH = -1, prevM = -1, prevS = -1;
bool colonBlink = true;

// 时钟样式 (0=机�?�翻�? 1=模拟指针) �?前向声明, 实现见下�?
uint8_t clockStyle = STYLE_FLAP;

// ========== 收音机状�?==========
// 二进�?stations.bin 格式 (v2):
//   Header(24) + RegionIdx(28*N) + StationRecords(变长)
// 2026-10-22: 布局宏统一由 ui_common.h 提供 (150/24/48/48/28/26), 删掉本文件的重复定义
//   (旧值 70/28/80/45/22/24 是 2026-10-20 重构前布局; 重复定义曾导致 radio_ui.cpp
//   按新值绘制、main.cpp 按旧值判断触摸分区 -> 命中区与绘制区错位)
RegionEntry* regionEntries = nullptr;
int regionCount = 0;
#define RMAX_STN 1700
RadioStation* radioStn = nullptr;   // PSRAM: 1700*84B ~= 142KB, 内部 DRAM 放不下
int radioStnTotal = 0;
#define RMAX_CATS 48
RadioCat radioCats[RMAX_CATS];
int radioCatCount = 0;
#define RMAX_ALL 1700
uint16_t allSeq[RMAX_ALL];      int allCount = 0;
#define RMAX_FAV 64
uint16_t favSids[RMAX_FAV];     int favCount = 0;
#define RMAX_HIST 48
uint16_t histSids[RMAX_HIST];   int histCount = 0;
int radioBinTotal = 0;
int extraCount = 0;            // radio_meta 里额外(用户)电台数, buildRadioCats 用
#define RMAX_EXTRA 24
char extraUrl[RMAX_EXTRA][260]; int extraUrlCount = 0;
char radioPinnedUsed[18] = "";  // 最近一次 buildRadioCats 用的置顶省名, loop 用它判断定位是否变化需重建
int currentCat = 0;
int currentPlayingSid = -1;   // 播放中的全局 sid (-1=无)
bool radioPlaying = false;
bool radioInited = false;
int radioVolume = 10;
bool musicPlaying = false;
int regionScrollTop = 0;
int listScrollTop = 0;
// 触摸状态 (原 favorites/showingFullscreen 等)
bool showingFullscreen = false;
unsigned long lastTapTime = 0;
int lastTapRow = -1;
int pendingPlaySid = -1;          // 延迟起播 sid: 双击窗口(RADIO_DBL_MS)关闭后才真正连流
unsigned long pendingPlayAt = 0;  // 起播请求的时间戳
          // �?�?�?�?见的电台下标 (行号)
bool listScrollDirty = true;
bool sideScrollDirty = true;    // 侧栏(整列)需局部刷新 (2026-09-27)    // �?要重绘列�?

// 触摸状�??
// touchLastX/Y 已在 handleTouch 前的 static 变量�?定�??

// ========== 天气状�??==========
bool weatherInited = false;
// 温度/湿度 (占位 mock �? 接入天气 API 后替�? 默�?��??make_preview.py �?�?
int mockTempC = 25;
int mockHumidity = 60;

// ========== 翻页时钟 (机�?�翻页�??/ Split-Flap) ==========
// 屏幕 480x480 (rot 1)。布�?: 6 张白色翻页牌(HH:MM:SS) + �?�?铰链�?
// + 下方日期条�?�只重绘发生变化的数字牌(�?部刷�? 不整屏刷�?�?
// 数字: FreeSansBold24pt7b 缩放2 �?'0' �?48x70 px�?
#define CARD_W        56    // 单张数字牌�?? (容纳 48px 数字 + 边距)
#define COLON_W       12    // 冒号�?
#define CARD_GAP      6     // 同组两数字牌间距
#define COLON_GAP     10    // 冒号两侧间距
#define CARD_TOP      48    // 翻页牌顶�?
#define CARD_H        120   // 翻页牌高 (数字 70px 居中 + 上下留白)
#define CARD_RADIUS   10    // 圆�?�半�?
#define DIGIT_SCALE   2     // FreeSansBold24 缩放 (24px -> 48px �?
#define DATE_LEFT     31    // 日期条左�?(�?CARD_X0 对齐)
#define DATE_RIGHT    449   // 日期条右�?(与末张牌右缘对齐)
#define ROW1_TOP      186   // 主日期�?�顶�? (�?�?/星期 | 农历/干支)
#define ROW1_H        46    // 主日期�?��??
#define ROW2_TOP      240   // 详情行顶�?(节气 | 月相)
#define ROW2_H        36    // 详情行高

// 翻页牌配�?
#define CARD_FACE     0xFFFF  // 白底牌面 (�?preview CARD_FACE_W=0xFFFF �?�? 黑字)
#define CARD_SEAM     0x3A48  // 铰链: �?�? 1px 细缝 (冷灰, 白底上近乎隐�?
#define HINGE_HI      0x9514  // 铰链: 两�??�?轴帽受光高�??(�?�?)
#define HINGE_MID     0x4ACB  // 铰链: �?轴帽�?�?(�?�?)
#define HINGE_LO      0x1945  // 铰链: �?轴帽底�??(暗沟)
#define CARD_SHADE    0x1084  // 翻页时上半暗阴影
#define CARD_EDGE     0x39C6  // 卡片下缘投影
#define CARD_NUM      0x0000  // 数字黑色
#define DATE_STRIP    0x18E3  // 详情行底�?(近黑, 与�?�航栏一�?
#define ROW1_BG       0x2146  // 主日期�?�底�? (深灰�? 稍亮突出)
#define DATE_SEP      0x0821  // 日期分隔竖线
#define COLOR_GOLD    0xFE4C  // 干支金色
#define SOFT_C        0x94F6  // 灰蓝 (�?通节气文�?
// 节假日同步高�?(�?preview make_preview.py HOL_* �?�?
#define CAL_OFF       0xF800  // 放假(�? - �?(与周�?同�?? 表示休息)
#define CAL_WORK      0x6CF1  // 补班(�? - 浅蓝 (黑底高可�? �?tempColor 冰点�?
// 日历浅色主�?? (仅日历页; 其余页面仍深�? �?�?make_preview.py CAL_* 同�??
#define CAL_LIGHT_BG  0xFFFF  // 页面白底
#define CAL_GRID      0xC65A  // table 网格�?(浅灰�?
#define CAL_HDR       0x5ACB  // 星期表头 / �?�? (深灰)
#define CAL_TITLE     0x12D9  // 月份标�?? / "今日" (�?
#define CAL_NORM      0x31A6  // �?通日期数�?(深灰)
#define CAL_STRIP_L   0xEF9F  // 底部信息条底�?(浅蓝�?
#define CAL_GOLD_L    0xCC21  // 干支 (深金, 白底�?�?)
#define CAL_WORK_L    0x01F4  // 补班 (深蓝, 白底�?�?)

// 节日/节气 当天显示颜色 ("今日XX" �? �?知事�? -> 灰蓝)
uint16_t eventColor(const char* name) {
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
  // 24 节气之�?? (春分/秋分) 用金�?
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

// 温度颜色: 随数值变�?(越冷越蓝, 越热越红) �?天气接入后在 drawDateRow 使用
// 冰点以下用浅�?(0x6CF1) 而非�?�? (0x001F), 避免在深色背�?上不�?�?
uint16_t tempColor(int c) {
  if (c <= 0)  return 0x6CF1;       // 冰点以下 - 浅蓝 (高�?�比�?)
  if (c <= 8)  return TFT_CYAN;     // �?
  if (c <= 15) return TFT_GREEN;    // �?
  if (c <= 24) return COLOR_GOLD;   // 舒�??
  if (c <= 29) return TFT_YELLOW;   // �?
  return TFT_RED;                   // 炎热 (>30°)
}

// 湿度颜色: 随数值变�?(过干偏黄, 舒�?�绿, 过湿偏蓝)
uint16_t humidColor(int h) {
  if (h <= 30) return TFT_YELLOW;   // 过干
  if (h <= 45) return COLOR_GOLD;   // 偏干
  if (h <= 65) return TFT_GREEN;    // 舒�??
  if (h <= 80) return TFT_CYAN;     // 偏湿
  return 0x6CF1;                    // 过湿 - 浅蓝 (高�?�比�?)
}
#define MOON_LIT      0xFF97  // 月相�?�?
#define MOON_DARK     0x2146  // 月相暗部
#define MOON_EDGE     0x424B  // 月相�?�?

// 数字�?x 坐标 (居中布局: 6 张牌 + 2 �?冒�?? 总�?? 418, 左右�?31px 边距)
#define CARD_X0       31
#define CARD_X1       (CARD_X0 + CARD_W + CARD_GAP)              // 93
#define COLON_X       (CARD_X1 + CARD_W + COLON_GAP)             // 159
#define CARD_X2       (COLON_X + COLON_W + COLON_GAP)            // 181
#define CARD_X3       (CARD_X2 + CARD_W + CARD_GAP)              // 243
#define COLON_X2      (CARD_X3 + CARD_W + COLON_GAP)             // 309
#define CARD_X4       (COLON_X2 + COLON_W + COLON_GAP)           // 331
#define CARD_X5       (CARD_X4 + CARD_W + CARD_GAP)              // 393

// 两相 split-flap 翻页时序 & 滚动齿轮 (�?preview live_preview / make_preview �?�?
// 再放慢版 (�?260/220): 让翻�?齿轮滚动更清晰可�?
#define FLIP_TOP_MS   400
#define FLIP_BOT_MS   340
// 翻页刷新帧间�?(ms) = preview live_preview �?TICK_MS=40 (~25 FPS)�?
// loop() 无节�? tickFlapClock 若每�?都�??(~1000+ FPS) 会把 SPI 打满,
// 每帧 ~13KB 牌面重绘丢帧 -> 翻页�?抖�?�按 40ms 门控调用, 与�?��?�同帧率�?
// 状�?�机�?�?�?millis() 计时, �?�?"调用频率"不改变动画时序�??
#define FLAP_FRAME_MS 40
// 指针样式刷新帧间�?(ms) �?30 FPS。�?�针�?平滑�?�?, 每帧都会重绘整张表盘
// (~40KB: fillCircle 白底 + 60 刻度 + 12 数字 + 指针)。loop() 无节流时 (~1000+ FPS)
// 会把 20MHz SPI 打满 -> 整面�?�?/撕�?��?�限�?~30 FPS, 秒针 6°/s => 0.2°/�?
// 依然平滑, 与翻页的 FLAP_FRAME_MS 节流同理�? 2026-10-22: ANALOG_FRAME_MS 由 ui_common.h 统一 (50), 删本行
#define ROLLER_W      16
#define ROLLER_PITCH  14
#define GAP1_CX       (COLON_X  + COLON_W / 2)   // 165 (= preview GAP1_CX)
#define GAP2_CX       (COLON_X2 + COLON_W / 2)   // 315 (= preview GAP2_CX)

// 两相 split-flap 翻页 (非阻塞状态机), �?preview live_preview.Sim �?�?

// 农历/星期 名称�?
const char* lunarMonths[] = {"正","二","三","四","五","六","七","八","九","十","冬","腊"};
const char* lunarDays[] = {
  "初一","初二","初三","初四","初五","初六","初七","初八","初九","初十",
  "十一","十二","十三","十四","十五","十六","十七","十八","十九","二十",
  "廿一","廿二","廿三","廿四","廿五","廿六","廿七","廿八","廿九","三十"
};
const char* weekDaysFull[] = {"星期日","星期一","星期二","星期三","星期四","星期五","星期六"};


// ============================================================
// 触摸状�?�变�?(边沿�?�?
// ============================================================
static bool touchWasDown = false;
static int touchStartX = 0, touchStartY = 0;
static int touchLastX = 0, touchLastY = 0;
static unsigned long touchStartTime = 0;
static bool touchDidScroll = false;
static int touchScrollStartScrollTop = 0;
static int touchScrollZone = 0;         // 0=无 1=右侧电台列表 2=左侧地区侧栏 (镜像 live_preview)
static int touchScrollStartRegion = 0;  // 侧栏拖拽起始 regionScrollTop

// ============================================================
// initTFT / initTouch / drawNavBar
// ============================================================
void initTFT() {
  tft.init();
  tft.setRotation(3);   // 180° 旋转 (原为 1)
  // ⚠️ ST7796 �?BGR 顺序 (User_Setup.h TFT_BGR), 必须 swap 字节�?
  // ESP32 小�?? uint16_t 0x07FE 在内存是 0xFE 0x07; pushImage �?swap 就推
  // 0xFE 0x07, ST7796 读成 0xFE07 (B=31,G=57,R=7) = 蓝色, �?应是青�??
  // 之前 drawCard/blitDialNum 用黑字白�?(0x0000/0xFFFF swap 后仍�?�?/�?
  // 看不出错, 但日期条彩色�?彩色背景就暴露字节序错乱 (字体颜色�?彩色背景).
  tft.setSwapBytes(true);
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(1);
  pinMode(TFT_BL_PIN, OUTPUT);
  digitalWrite(TFT_BL_PIN, HIGH);
}

void initTouch() {
  // BUILDV5-BASE1/STEP2: 触摸独立任务（BASE1 实证：单任务同步采样被 audio.loop
  // 阻塞拖死 → 点不到电台/拖动不灵敏）。touchScanTask 采样 80ms、任务层包
  // g_spiMutex（与 main loop 绘制互斥），main loop 只读共享状态。
  // CALFIX-CORE（2026-10-17，crash dump 铁证）：xTaskCreate → 固定 core 1。
  //   crash: assert xTaskPriorityDisinherit tasks.c:5156 (pxTCB == pxCurrentTCBs[core])
  //   Backtrace: spiEndTransaction ← SPIClass::endTransaction ← TFT_eSPI::end_nin_write
  //             ← modeRadio ← loop ← loopTask（main loop 释放 SPI 锁时崩溃）。
  //   根因：touchScanTask 无核心固定 → 调度器双核迁移 → 与 loopTask(core1) 跨核并发
  //   Take/Give g_spiMutex/TFT spi_mutex/SPI spi_mutex（三层嵌套）→ ESP-IDF 5.x
   //   FreeRTOS 双核优先级继承断言 bug（netTask 已同款固定 core1，见 L710）。
  //   拖动列表=触摸采样+高频重绘同时压锁→触发；日历点击低频未触发。
  touchDriverInit();
  xTaskCreate(&touchScanTask, "touchScan", 4096, NULL, 3, NULL);  // 改回不固定核（验证：固定core1后屏幕完全没反应）
}

// BUILDV5-NETTASK: 独立下载泵任务（单任务调 audio.loop，main loop 彻底不调）。
// 根因（cap_diag8/10/11/cap_td1 实证）：下载泵在 main loop → audio.loop() 每次调用
// 阻塞 ~60-150ms（分片切换 httpPrint/网络等待）→ 帧率拖到 1fps → 下载 12KB/s <<
// 播放消耗 110KB/s → 缓冲无法积累 → 卡顿。netTask 高频泵（5ms 周期 200 次/s）：
// 单次阻塞只影响 netTask 自身，UI 帧率恢复；下载速率 = 泵频率 × 每次推进包数
// → 潜在 300KB/s（受网络带宽与水位限制）。切台时 main loop 先置 radioPlaying=false
// 停泵 → 完成切台 → 置 true 恢复，避免 stopSong/connecttohost 与 loop 状态机并发。
void netTask(void* arg) {
  // BUILDV5-TWDTRESET (2026-10-12): 豁免本任务 TASK_WDT——netTask 泵 audio.loop 时，
  // cnr 分片切换网络阻塞 7-9s（L667/L804 实证）会触发 CONFIG_ESP_TASK_WDT_TIMEOUT_S=5
  // 超时（PANIC 未设→直接复位芯片）→ 设备播放中随机重启。阻塞≠死锁：audio.loop 内部
  // 有网络超时会自行恢复，宁可短暂无声/卡顿，绝不复位重启。
  esp_task_wdt_delete(NULL);
  // BUILDV5-HLSFIX: 常规遥测打印已删除（生产模式，UART 竞争根除）
  // BUILDV5-STEP4-DIAG: 心跳诊断——定位分片切换 28s 静默（cap_step3b 实证）：
  //  audio.loop 单次调用 >500ms 打 [NET-BLK]（网络阻塞点）；2s 打 [NET-BEAT]（netTask 存活+耗时）。
  uint32_t lastBeat = 0;
  uint32_t lastDt = 0;
  while (true) {
    if (radioPlaying || musicPlaying) {
      uint32_t t0 = millis();
      audio.loop();
      uint32_t dt = millis() - t0;
      lastDt = dt;
      if (dt > 500) {
        Serial.printf("[NET-BLK] audio.loop took %ums heap=%u\n", dt, (unsigned)ESP.getFreeHeap());
      }
      if (millis() - lastBeat >= 2000) {
        lastBeat = millis();
        // BUILDV5-BUFFLOG (2026-10-24): 缓冲水位诊断——断流取证（inBufferFilled/free）。
        // 播放断流 = 水位耗尽假设的验证点：断流瞬间 filled 应接近 0。
        Serial.printf("[NET-BEAT] dt=%ums heap=%u buff_f=%u buff_free=%u\n", lastDt, (unsigned)ESP.getFreeHeap(),
                      (unsigned)audio.inBufferFilled(), (unsigned)audio.inBufferFree());
      }
    }
    vTaskDelay(15 / portTICK_PERIOD_MS); // BUILDV5-HLSFIX: 10→15ms（进一步减跨核唤醒频率；
    // 下载在 netRead 内部阻塞读，泵间隙不影响速率；压 esp_crosscore_isr 异常）
  }
}

void drawNavBar() {
  tft.fillRect(0, NAV_BAR_Y, 480, NAV_BAR_HEIGHT, COLOR_NAV_BG);
  for (int i = 0; i < NAV_BTN_COUNT; i++) {
    int x = i * NAV_BTN_WIDTH;
    bool sel = (i == currentMode);
    if (sel) {
      tft.fillRect(x + 2, NAV_BAR_Y + 2, NAV_BTN_WIDTH - 4, NAV_BAR_HEIGHT - 4, COLOR_NAV_SEL);
    }
    uint16_t tc = sel ? TFT_YELLOW : TFT_WHITE;   // 选中=黄字, 未选=白字 (与 live preview 一致)
    int tw = font16.textWidth(navLabels[i]);
    font16.drawString(x + (NAV_BTN_WIDTH - tw) / 2, NAV_BAR_Y + 8, navLabels[i], tc, sel ? COLOR_NAV_SEL : COLOR_NAV_BG);
    if (i > 0) tft.drawFastVLine(x, NAV_BAR_Y + 2, NAV_BAR_HEIGHT - 4, 0x630C);
  }
}

// ============================================================
// handleTouch (边沿检测版) —— BUILDV5-TOUCHDRV: 触摸独立任务
// ============================================================
// 触摸扫描在 touchScanTask（touch_driver.cpp，自写 XPT2046 驱动）独立运行，
// main loop 只读共享状态（s_touchDown/s_touchX/s_touchY）——触摸不再阻塞主循环，
// 音频泵（每帧开头高频 10 次）与触摸彻底解耦，播放电台时操作屏幕不再卡顿。
void handleTouch() {
  // BUILDV5-TOUCHDRV: 读共享触摸状态（touchScanTask 独立采样，main loop 只读；
  // 触摸不再被 audio.loop 阻塞 → 点击/拖动灵敏）
  bool isDown = s_touchDown;
  uint16_t tx = s_touchX, ty = s_touchY;
  // 触摸坐标 = 屏幕坐标 (getTouch 已按 rotation(3) 校准), 与 live preview 1:1 一致, 无需翻转.
  // 之前此处曾多翻 180 度 -> 点底部导航栏被映射到顶部, 点日历变成切换时钟样式 (已移除).
  if (isDown && !touchWasDown) {
    touchStartX = touchLastX = tx;
    touchStartY = touchLastY = ty;
    touchStartTime = millis();
    touchDidScroll = false;
    touchScrollStartScrollTop = listScrollTop;
    // 拖拽分区 (镜像 live_preview _on_press): 左侧地区侧栏 / 右侧电台列表 都可上下拖动
    if (currentMode == MODE_RADIO) {
      if (tx < RADIO_SIDEBAR_W && ty >= RADIO_HEADER_H + SIDEBAR_HEAD_H && ty < NAV_BAR_Y) {
        touchScrollZone = 2; touchScrollStartRegion = regionScrollTop;
      } else if (tx >= RADIO_LIST_X && ty >= RADIO_LIST_TOP && ty < RADIO_LIST_BOT) {
        touchScrollZone = 1;
      } else touchScrollZone = 0;
    } else touchScrollZone = 0;
  }

  if (!isDown && touchWasDown) {
    unsigned long dur = millis() - touchStartTime;
    int dy = touchLastY - touchStartY;
    int dx = touchLastX - touchStartX;
    touchWasDown = false;
    touchStartTime = 0;

    // 2026-09-27: 侧栏拖拽结束吸附 -> 视口顶所在行的行顶对齐视口顶, 避免停在半行
    // 2026-10-22: 行偏移含分组标题行, 统一用 sideRowTop/sideTotalH
    if (touchDidScroll && touchScrollZone == 2 && currentMode == MODE_RADIO) {
      int totalH = sideTotalH();
      int smax = max(0, totalH - (NAV_BAR_Y - (RADIO_HEADER_H + SIDEBAR_HEAD_H)));
      int snap = 0;
      for (int k = 0; k < radioCatCount; k++) {
        int t = sideRowTop(k);
        if (t <= regionScrollTop) snap = t;
      }
      if (snap > smax) snap = smax;
      if (snap != regionScrollTop) { regionScrollTop = snap; sideScrollDirty = true; }
    }
    // 2026-10-22 问题③: 右列拖拽结束同样吸附到行顶 (与左列一致, 不悬停在半行)
    if (touchDidScroll && touchScrollZone == 1 && currentMode == MODE_RADIO) {
      int totalPix = curCount() * RADIO_ROW_H;
      int smax = max(0, totalPix - RADIO_LIST_H);
      int snap = (listScrollTop + RADIO_ROW_H / 2) / RADIO_ROW_H * RADIO_ROW_H;
      if (snap > smax) snap = smax;
      if (snap != listScrollTop) { listScrollTop = snap; listScrollDirty = true; }
    }
    if (touchDidScroll) touchScrollZone = 0;   // 拖拽结束 -> 复位分区 (避免下次按下误判)
    if (!touchDidScroll && dur < 800 && abs(dy) < 15 && abs(dx) < 15) {
      Serial.printf("[TOUCH] tap @ (%d,%d) dur=%lu mode=%d\n", touchStartX, touchStartY, (unsigned long)dur, (int)currentMode);  // 临时调试: 确认触摸坐标与屏幕坐标一致
      if (touchStartY >= NAV_BAR_Y) {
        int btnIdx = touchStartX / NAV_BTN_WIDTH;
        if (btnIdx >= 0 && btnIdx < MODE_COUNT) {
          if (btnIdx != currentMode) {
            currentMode = (UIMode)btnIdx;
            modeChanged = true;
            navDirty = true;   // 选中项变�?-> 导航栏高�?�?重绘 (样式切换不走这里)
            for (int i = 0; i < MODE_COUNT; i++) modeDrawn[i] = false;
            prevH = prevM = prevS = -1;
          }
          drawNavBar();
        }
      } else if (currentMode == MODE_RADIO) {
        handleRadioTap(touchStartX, touchStartY);
      } else if (currentMode == MODE_CALENDAR && touchStartY < NAV_BAR_Y) {
        // 点日历页: �?1/3 = 上一�? �?1/3 = 下一�?(�?preview �?�?/ �?�?�?
        handleCalendarTap(touchStartX, touchStartY);
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
        // 点击时钟�?(左上�?30x100 为配网热�? 切换 翻页/指针 样式
        clockStyle = (clockStyle == STYLE_FLAP) ? STYLE_ANALOG : STYLE_FLAP;
        saveClockStyle(clockStyle);
        modeChanged = true;
        prevH = prevM = prevS = -1;
        Serial.printf("clock style -> %s\n", clockStyle == STYLE_FLAP ? "flap" : "analog");
      }
      // fix(2026-09-27): 旧版此处 delay(150) 在每次 tap 释放后阻塞主循环 150ms。
      // 快速双击时, 第二下的 按下+释放 整个落在该 150ms 阻塞内 -> 主循环没采样到 -> 第二下丢失,
      // 双击永远不触发(=用户"快速双击打不开播放界面"的根因)。触摸本身已有边沿检测(getTouch
      // 的 50ms _pressTime 保持), 无需这层 150ms 阻塞去防抖。删掉后主循环能立即采样第二下。
    }
    touchDidScroll = false;
  }

  if (isDown && currentMode == MODE_RADIO) {
    int dy = ty - touchStartY;
    if (abs(dy) > 10 && !touchDidScroll) touchDidScroll = true;
    if (touchDidScroll) {
      if (touchScrollZone == 2) {
        // 2026-09-27: 侧栏整列(分组标题+分类行)一起滚动, 像素级 1:1 跟手(最灵敏);
        // 局部刷新: 只重绘 150x264 侧栏 (sideScrollDirty), 不整页 (旧版每行整页重绘, SPI 要 ~100ms
        // 阻塞主循环, 拖拽采样跟不上 -> 不灵敏/卡顿)。2026-10-22: 总高用 sideTotalH (含组标题行)
        int totalH = sideTotalH();
        int smax = max(0, totalH - (NAV_BAR_Y - (RADIO_HEADER_H + SIDEBAR_HEAD_H)));
        int ns = constrain(touchScrollStartRegion - dy, 0, smax);
        if (ns != regionScrollTop) { regionScrollTop = ns; sideScrollDirty = true; }
      } else if (touchScrollZone == 1) {
        // 自然滚动(上划=看下面更多电台)。2026-10-22 问题③: 与左侧统一为像素级
        // 1:1 跟手 —— listScrollTop 从"行索引"改为"像素偏移" (绘制/命中按像素反算)。
        int totalPix = curCount() * RADIO_ROW_H;
        int smax = max(0, totalPix - RADIO_LIST_H);
        int ns = touchScrollStartScrollTop - dy;
        if (ns < 0) ns = 0;
        if (ns > smax) ns = smax;
        if (ns != listScrollTop) { listScrollTop = ns; listScrollDirty = true; }
      }
    }
    touchLastX = tx; touchLastY = ty;
  }

  // 边沿�?�? 必须在所有�??查完�?touchWasDown 之后再更�?
  // 否则释放�?(!isDown && touchWasDown) 永远�?false �?触摸无反应�??
  touchWasDown = isDown;
}

// 2026-10-20: autoPlayOnceTask (boot 自动播放环球资讯 + 10s 兜底重连) 已删除。
// 它是 V81 代理调试期的验证任务, 与新播放条"暂停"按钮直接冲突
// (用户暂停后 10s 内被强制重新连流, 暂停形同虚设)。现恢复手动控制, 播放条 ▶ 起播。

void setup() {
  Serial.begin(115200);
  delay(500);
  // BUILDUI8-CRASH-DIAG: 临时全开 esp_log（含 WDT "Print CPU" / esp_timer / lwip），定位 boot 后 ~7s 卡死持锁者
  // esp_log_level_set("*", ESP_LOG_NONE); // 关所有 esp_log 打印（Audio 库 S2DBG 刷屏），我们自己的 Serial.printf 不受影响
  sdProbe();  // DIAG: read the on-board SD card (CS=GPIO6, shared SPI bus) and dump contents
  // BUILD41-DIAG-AAC-RATE: BUILD40 confirmed fixed_mclk=0 does NOT fix the
  // slow playback (MEAS still ~16000). All I2S driver register tweaks failed
  // (BUILD38 tx_update pulse, BUILD39 stop+update+start, BUILD40 fixed_mclk=0)
  // — the divider writes land in APB shadow but the HW clock doesn't move.
  // This build adds a decisive diagnostic to split "AAC decoder slow" vs
  // "I2S HW clock slow": log AAC = samples actually decoded & pushed into
  // m_outBuff per second (m_aacSampCumul accumulator). If AAC=48000 but
  // MEAS=16000, the decoder is fine and the I2S HW is the bottleneck (samples
  // back up in i2s_write's blocking queue). If AAC=16000 too, the decoder
  // itself is starved (HLS fetch/CPU) and no I2S driver fix can help.
  Serial.println("ESP32-S3 Smart Radio Starting... BUILDV5-STEP3");
  // BUILDV5-RESETDIAG: 打印上次复位原因（区分 brownout/WDT/panic/外部复位）
  // esp_reset_reason(): 0=UNKNOWN 1=POWERON 2=EXT 3=SW 4=panic 5=INT_WDT 6=TASK_WDT 7=WDT 8=deep 9=glitch 10=brownout
  {
    esp_reset_reason_t rr = esp_reset_reason();
    Serial.printf("RESET_REASON=%d\n", (int)rr);
  }

  // ================== 上电�?�? (BOOT SELF-TEST) ==================
  // �?TFT_eSPI 内置字体(不依�?LittleFS / �?定�??XFont)在屏幕上逐�?�显�?,
  // 这样在没有串口监视器时也能直接看出黑屏停在哪�?步�??
  initTFT();

  // PENIRQ-WAKE（2026-10-18）：XPT2046 复位后 PENIRQ 输出级可能卡住（一直高电平），
  //   手动短接 GPIO38→GND 可"唤醒"。在 SPI 总线初始化后做：先拉低 GPIO4（2026-10-21：
  //   GPIO38 实测点击变 0 但芯片 IN1 恒 1 = octal PSRAM SPICS1 复用钳制，改用 GPIO4），
  //   再通过 SPI 发个读 X 的命令"唤醒" XPT2046，然后改回输入上拉。
  Serial.println("[PENIRQ] wake up: GPIO4 LOW + SPI wakeup...");
  pinMode(TOUCH_IRQ, OUTPUT);
  digitalWrite(TOUCH_IRQ, LOW);
  delay(100);
  // 通过 SPI 发个读 X 的命令"唤醒" XPT2046
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, LOW);
  tft.getSPIinstance().transfer(0xD0);  // 读 X 命令
  tft.getSPIinstance().transfer(0);
  tft.getSPIinstance().transfer(0);
  digitalWrite(TOUCH_CS, HIGH);
  delay(100);
  pinMode(TOUCH_IRQ, INPUT_PULLUP);
  delay(10);
  Serial.printf("[PENIRQ] after wake: GPIO4=%d\n", digitalRead(TOUCH_IRQ));

  tft.setTextFont(2);                 // 内置 16px 字体 (User_Setup.h �?LOAD_FONT2)
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(0, 0);
  tft.println("ESP32-S3 Radio");
  tft.println("Boot self-test...");

  // 1) 文件系统 (黑屏�?常�?�原�?: �?烧录文件系�??uploadfs)
  bool fsOk = LittleFS.begin();
  Serial.printf("LittleFS: %s\n", fsOk ? "OK" : "FAIL");
  tft.print("FS: ");
  if (fsOk) tft.print(String(LittleFS.totalBytes() / 1024)); else tft.print("FAIL");
  tft.println(" KB");

  // 1b) 节日/节气 音乐配置 (�?�? 缺失/损坏则不�?
  if (fsOk) loadFestivalConfig();

  // 1c) 时钟样式 (NVS 持久�? 翻页 / 指针, 点击时钟页可切换)
  clockStyle = getClockStyle();
  Serial.printf("clock style: %s\n", clockStyle == STYLE_FLAP ? "flap" : "analog");

  // 2) 触摸
  initTouch();

  // 3) �?文字�? (加载失败 -> 后续�?有中文都�?空�?? 看起来像黑屏)
  bool f16 = font16.reInit("/font_simhei_16.font");
  font32.setFlashMode(true);   // BUILDV5-FONTF: 64px 黄历字库 Flash 直读 → 释放 1.48MB PSRAM 给音频深缓冲 (2026-10-17 恢复; 加诊断验证字形数据)
  bool f32 = font32.reInit("/font_simhei_64.font");
  Serial.printf("font16: %s  font32: %s\n", f16 ? "OK" : "FAIL", f32 ? "OK" : "FAIL");
  tft.print("Font16: "); tft.println(f16 ? "OK" : "MISSING");

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

  // 4b) 节假�?放假/补班) 同�??: 加载缓存; 联网后由 loop 里的 tick() 定时同�??
  holiday::init();
  geo::init();   // geo: IP-locate province for radio sidebar pin (cache read; tick() fetches after wifi)
  weather::init();   // weather: open-meteo 实时天气 (读缓存; tick() 联网后拉取 + 30min 刷新)

  // 5) 电台数据�?(bad format / 内存不足�?new[] 会崩�?
  Serial.println("Loading radio DB...");
  bool dbOk = loadRadioDB();
  bool metaOk = loadRadioMeta();
  if (dbOk && metaOk) tft.printf("%d cat / %d stn\n", radioCatCount, radioStnTotal);
  else tft.println("DB: MISSING");
  Serial.printf("DB cats=%d stn=%d\n", radioCatCount, radioStnTotal);
  audio.setPinout(I2S_BCK_PIN, I2S_LRC_PIN, I2S_DOUT_PIN);
  radioVolume = 21;     // 2026-10-20: 音量状态与 UI 同步 (播放条显示百分比, 点按可静音)
  audio.setVolume(21); // BUILDV5-FIX: v4 lib defaults volume=0 (silent); v2/v3 defaulted to 21=max
  // HLS buffer tuning (CNR/CRI m3u8, 10s segments / separate HTTP GET each):
  // 400KB PSRAM buffer ~= 25s @128kbps. Old 64KB setting gave only ~3.5s
  // headroom -> any segment-fetch hiccup underran the I2S DMA (stutter /
  // "打嘟噜"), worst right after a station switch. Must be set BEFORE the
  // first connecttohost (buffer is allocated lazily in setDefaults).
  // v4 default AudioBuffer = 65535*10 + 65535 = 704KB PSRAM, larger than BUILD69's 400KB;
  // no setBufsize needed (not exposed by class Audio in v4).
  Serial.printf("[MEM] pre-audio: psramFree=%uKB\n",
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  audio.setConnectionTimeout(2000, 2000);   // 2s connect timeout for cross-region radio servers

  tft.println("READY - start main UI");
  Serial.println("Setup done.");
  // CLKFIX 2026-10-16: boot 内存基线——定位"播放后内部 RAM 碎片化(maxBlock 38KB)导致时钟缓冲分配失败"的碎片源
  Serial.printf("[MEM] boot: internalFree=%uKB internalMax=%uKB psramFree=%uKB psramMax=%uKB\n",
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
  // BUILDV5-CLKFIX-A (2026-10-17): 时钟缓冲 boot 常驻。此时堆最干净(maxBlock 最大)、
  // 任务未创建、音频未连接 → 分配必然成功 → 播放期切时钟直接用常驻缓冲(与碎片无关)。
  allocClockBufsResident();
  // BUILDV5-NETTASK: 独立下载泵任务（见 netTask 定义注释）。固定 Core 1 + 优先级 2。
  // 根因（cap_diagx/fix3 实证）：①任意核落 Core 0 被 audioTask 解码抢占 → callDt 130-718ms；
  // ②pin Core 1+prio1 与 loopTask 同级轮转被 2900fps UI 抢占 → callDt 563-896ms。
  // 修法 = pin Core 1 + prio 2：netTask 抢占 UI 获得真实执行时间（TS 解析 ~0.05ms/包），
  // 每次调用 <10ms 即让出 → 泵 200/s 全速下载，UI 帧率略降（仍 >>30fps 显示需求）。
  // BUILDV5-STEP3: 恢复 netTask 独立下载泵（STEP2 抓包实证：单任务泵下 audio.loop()
  // 分片切换网络阻塞 7-9s → task_wdt 看门狗 + 缓冲耗尽无声 + main loop 卡死触摸丢失）。
  // audio.loop 的阻塞只影响 netTask，main loop 保持响应。固定 Core 1 + prio 2。
  BaseType_t netOk = xTaskCreatePinnedToCore(&netTask, "netTask", 8192, NULL, 2, NULL, 1);
  // 2026-10-20: autoPlayOnce 任务已随 autoPlayOnceTask 删除 (与播放条暂停按钮冲突, 见上注释)
  // BUILDV5-HLSFIX: 全局 SPI 互斥——touchScanTask 与 main loop 显示绘制共享 FSPI
  // 总线（TFT_eSPI locked 标志单线程假设 + SPIClass 锁 IDF5.5 不可靠 → cap_fix8
  // 链A pushSwapBytePixels 崩溃）。main loop 绘制段(touch消费+modeXxx)与 touch
  // 采样都持本锁 → SPI 访问串行。
  g_spiMutex = xSemaphoreCreateMutex();
  g_printMutex = xSemaphoreCreateMutex();
  // BUILDV5-HLSFIX: 预热 newlib tzset——cap_fix9 链C 实证 main loop(modeClock 时间格式化)
  // 与另一核首次并发调 localtime_r → _tzset_unlocked 崩溃。
  // BUILDV5-TZFIX (2026-10-22): 全部 localtime_r 调用已改为无锁 epochToLocalNoLock,
  // 不再触碰 newlib tz 锁, 此预热窗口已消除; 保留空预热注释占位 (无锁函数无需预热)。
  Serial.printf("[NET] spiMutex=%d printMutex=%d\n", g_spiMutex != nullptr, g_printMutex != nullptr);
  modeChanged = true;
}

void loop() {
  // BUILDV5-HLSFIX: 常规打印已删除（生产模式）——多任务并发打印 → UART 竞争崩溃
  // （cap_fix8/9/10/11 链B 实证）。仅保留错误级与一次性状态打印。
  static bool s_locksPrinted = false;
  if (!s_locksPrinted) {
    s_locksPrinted = true;
    if (g_printMutex) xSemaphoreTake(g_printMutex, portMAX_DELAY);
    Serial.printf("[LOCKS] spiMutex=%d printMutex=%d\n", g_spiMutex != nullptr, g_printMutex != nullptr);
    if (g_printMutex) xSemaphoreGive(g_printMutex);
  }
  // CLKFIX 2026-10-16: 播放期内存基线(60s 节流)——定位"播放后内部 RAM 碎片化(maxBlock<42.6KB)导致时钟缓冲失败"的碎片源
  // BUILDV5-CLKFIX-A: 增加播放期 internalFree 最低点采样(2s 节流, 60s 打印)——预分配时钟缓冲后
  // 音频余量是否够的硬指标(回写计划 §2.4: 预分配后 FLAP≈66KB/ANALOG≈80KB)。
  static uint32_t s_memLast = 0;
  static uint32_t s_lowLast = 0;
  static uint32_t s_lowMin = 0xFFFFFFFF;
  if (millis() - s_lowLast >= 2000) {
    s_lowLast = millis();
    uint32_t f = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (f < s_lowMin) s_lowMin = f;
  }
  if (millis() - s_memLast >= 60000) {
    s_memLast = millis();
    if (g_printMutex) xSemaphoreTake(g_printMutex, portMAX_DELAY);
    Serial.printf("[MEM] run: internalFree=%uKB internalMax=%uKB psramFree=%uKB psramMax=%uKB lowmin=%uKB\n",
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                  (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                  (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024),
                  (unsigned)(s_lowMin / 1024));
    if (g_printMutex) xSemaphoreGive(g_printMutex);
    s_lowMin = 0xFFFFFFFF;  // 重置, 下一个 60s 窗口
  }
  // NOTE: DEBUG-AUTO-PLAY removed 2026-10-09 (BUILD60) -- boot now shows the
  // clock UI (flap/analog per NVS) instead of hijacking into radio mode.
  // HLS stutter is addressed structurally by the HLS fetcher task (BUILD60+).
  // s_autoPump is only set by the serial switch command ('1'-'6') below.
  static bool s_autoPump = false;
  // BUILDV5-HLSFIX: 原 s_autoPump 心跳打印块已删除（生产模式；netTask 独立泵 audio.loop）
    // DEBUG-SERIAL-SWITCH: switch test stream over serial without touching the
    // UI. '1'=经济之声(CNR jjzs)  '2'=环球资讯(CRI 905)  '3'=经典音乐广播(CNR dszs)
    if (Serial.available()) {
      char c = (char)Serial.read();
      const char* sw = nullptr;
      if (c == '1') sw = "http://ngcdn002.cnr.cn/live/jjzs/index.m3u8";
      else if (c == '2') sw = "http://sk.cri.cn/905.m3u8";
      else if (c == '3') sw = "http://ngcdn002.cnr.cn/live/dszs/index.m3u8";
      // BUILD50-AAC-TEST: HTTPS ximalaya WITHOUT transcode -> bare .aac segments.
      // Used to test whether the xmcdn 418 block targets only .ts requests.
      else if (c == '4') sw = "https://live.ximalaya.com/radio-first-page-app/live/2781/64.m3u8"; // 成都潮台 .aac
      else if (c == '5') sw = "https://live.ximalaya.com/radio-first-page-app/live/15007/64.m3u8"; // 新闻听天下 .aac
      else if (c == '6') sw = "http://lhttp.qingting.fm/live/4804/64k.mp3"; // BUILD56: 怀集音乐之声 (non-HLS control, MP3 direct)
      else if (c == '7') { // BUILDV5-DIAG: dump GPIO func_out_sel for I2S pins
        Serial.printf("[GPIO] 6.func_sel=%d 7.func_sel=%d 8.func_sel=%d\n",
                      (int)GPIO.func_out_sel_cfg[6].func_sel,
                      (int)GPIO.func_out_sel_cfg[7].func_sel,
                      (int)GPIO.func_out_sel_cfg[8].func_sel);
        Serial.printf("[GPIO] 6.sig=%d 7.sig=%d 8.sig=%d (I2S0: BCK=12? WS=13? DOUT=14?)\n",
                      (int)I2S0O_BCK_OUT_IDX, (int)I2S0O_WS_OUT_IDX, (int)I2S0O_SD_OUT_IDX);
      }
      else if (c == '8') { audio.testTone1k(4, 0); } // BUILDV5-TONE: right-aligned 1kHz (if silent -> PCM takes high16)
      else if (c == '9') { audio.testTone1k(4, 1); } // BUILDV5-TONE: left-aligned 1kHz (known audible)
      else if (c == 't' || c == 'T') {               // BUILDV5-STEP3-TFIX4: 原始触摸数据转储
        touchDiag(15);
      }
      else if (c == 'c' || c == 'C') {               // BUILDV5-CALIB: 图形化触摸校准
        radioPlaying = false; musicPlaying = false;
        audio.stopSong();
        touchRunCalibration();
        modeChanged = true;                          // 校准后强制重绘，恢复主界面
        navDirty = true;                             // CALIB7: 导航栏由 navDirty 控制，校准期间被黑屏擦掉必须重绘
      }
      if (sw) {
        Serial.printf("[DEBUG] serial switch -> %c\n", c);
        radioPlaying = false;   // BUILDV5-NETTASK: 先停 netTask 泵（避免切台与 loop 并发）
        musicPlaying = false;
        audio.stopSong();
        if (audio.connecttohost(sw)) {
          radioPlaying = true;
          musicPlaying = false;
          s_autoPump = true;
          digitalWrite(STATUS_LED_PIN, HIGH);
        }
      }
  }
  // BUILDV5-NETTASK: 下载泵已挪到独立 netTask（audio.loop 高频推进），main loop
  // 不再调 audio.loop（单任务调库状态机 → 无并发；UI 帧率不再被 audio.loop 阻塞
  // 拖死）。main loop 只做触摸消费 + UI 绘制。
  // BUILDV5-HLSFIX: 触摸消费+绘制整体持 g_spiMutex——与 touchScanTask 采样互斥，
  // 消除 FSPI 总线并发崩溃（cap_fix8 链A：TFT_eSPI pushSwapBytePixels 数据竞争）。
  // 单锁无死锁（touch 不持其他锁；锁内 tft beginTransaction 的 SPI_MUTEX 不与
  // touch 竞争——touch 在 g_spiMutex 外等待）。
  if (g_spiMutex) xSemaphoreTake(g_spiMutex, portMAX_DELAY);
  handleTouch();
  uint32_t t1 = micros();
  holiday::tick();   // 节假�? 联网后�?��?�同�?+ �?24h + 浏�?�补同�??
  geo::tick();   // geo: locate province after wifi; refresh every 24h (non-blocking fetch)
  weather::tick();   // weather: fetch open-meteo after wifi; refresh every 30min (non-blocking)
  uint32_t t2 = micros();
  // Re-pin radio cats once the geolocated province becomes available / changes (mobile: pinnedProvince).
  // Compare against the key used at last build; if different, rebuild cats + repaint both panes.
  if (radioInited && currentMode == MODE_RADIO &&
      strcmp(radioPinnedKey(), radioPinnedUsed) != 0) {
    buildRadioCats();
    if (currentCat >= radioCatCount) currentCat = 0;   // old index may be out of range after rebuild
    tft.startWrite(); drawRadioRegionSidebar(); tft.endWrite();   // R1: wrap sidebar partial-refresh
    drawRadioList(); drawRadioPlayBar();   // repaint right pane (list + playbar)
  }

  switch (currentMode) {
    case MODE_CLOCK: modeClock(); break;
    case MODE_CALENDAR: modeCalendar(); break;
    case MODE_RADIO: modeRadio(); break;
    case MODE_WEATHER: modeWeather(); break;
  }
  uint32_t t3 = micros();
  // fix(2026-09-25): modeChanged 必须在每帧末尾清掉。旧版 loop() 忘了清
  // -> 每个 mode 的 `if (!modeDrawn[i] || modeChanged)` 每帧(~1000+ FPS)都整页重绘
  // -> 全屏闪烁(导航栏用 navDirty 不受影响)。与 preview 无关(预览整帧原子渲染)。
  modeChanged = false;
  // BUILDV5-CLKFIX-A (2026-10-17): 时钟缓冲 boot 常驻(allocClockBufsResident, setup 时分配)。
  // 原 BUILD17 "离开时钟页释放缓冲, 让内部堆最大连续块长到 ~200KB 给 HTTPS radio TLS handshake"
  // 的理由在 V5 已过时(全 HTTP 无 TLS) → 删除 freeClockBufs() 调用, 缓冲常驻,
  // 播放期切时钟不依赖任何新分配(根治切时钟空白/齿轮带缺失)。
  // freeClockBufs() 保留为空实现, 防其他引用。
  if (false) freeClockBufs();
  // 节日音乐在任意界面都继续 (收音机只由 netTask 泵; 此处不再调 audio.loop)
  if (musicPlaying) {
    if (!audio.isRunning()) {   // BUILD14: 节日流死 -> 停 (checkFestivalMusic 负责重试)
      musicPlaying = false;
      if (!radioPlaying) digitalWrite(STATUS_LED_PIN, LOW);
    }
  }

  // �?�?"导航选中项变�?(navDirty)时重绘�?�航�?: 既避免每帧重绘的�?�?, 也避免时钟样�?
  // 切换(不改变�?�中�?时�??重绘导航栏�?�成的彩色撕裂竖条�??
  if (navDirty) { drawNavBar(); navDirty = false; }
  // BUILDV5-HLSFIX: 锁释放移到本行——覆盖 L798 drawNavBar（cap_fix10 实证 navDirty
  // 重绘在锁外与 touchScanTask 并发 → 链A TFT pushSwapBytePixels 崩溃）
  if (g_spiMutex) xSemaphoreGive(g_spiMutex);

  // BUILDV5-STEP3: audio.loop 已由 netTask 独立泵（main loop 不再调——单任务泵下
  // 分片切换网络阻塞 7-9s 卡死 main loop：task_wdt + 触摸丢失 + 缓冲耗尽无声，
  // 见 cap_step2_diag.log [237418] task_wdt）。
  // BUILDUI8-WDTFIX (2026-10-24): IDLE1 饿死修复——loop() 无让位点，loopTask 连续
  // 占用 CPU1 → IDLE1 5s 无调度 → task_wdt 触发（" - IDLE1 (CPU 1)"）→ backtrace
  // 循环（panic=false 不重启，设备假死；时钟模式无网络阻塞让位，radio 模式有，故
  // "播放正常、切时钟崩溃"）。每轮 vTaskDelay(1)=10ms 让位，UI 100fps 足够，
  // 触摸(touchScanTask)/网络(netTask)走独立任务不受影响。
  vTaskDelay(1);
}

