/*
 * ui_common.h -- 共享声明头 (BUILD61: main.cpp 按功能拆分)
 * 所有拆出的模块 (.cpp) include 本文件, 获取全局对象/变量/常量/函数声明。
 */
#ifndef UI_COMMON_H
#define UI_COMMON_H

#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <time.h>
#include <LittleFS.h>
#include <SD.h>
#include <nvs.h>
#include <TFT_eSPI.h>
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
using weather::WData;
#include "esp_heap_caps.h"
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEBUG_UI 0

#define I2S_BCK_PIN     6
#define I2S_LRC_PIN     7
#define I2S_DOUT_PIN    8
#define STATUS_LED_PIN  2
#define TFT_BL_PIN      21

extern TFT_eSPI tft;
extern XFont font16;
extern XFont font24;
extern XFont font32;
extern Audio audio;

// BUILDV5-HLSFIX: 全局 SPI 总线互斥——XPT2046 触摸采样(touchScanTask) 与
// TFT_eSPI 显示绘制(main loop) 共享 FSPI 总线；TFT_eSPI 的 locked 标志是单线程
// 假设、SPIClass beginTransaction 锁在 IDF5.5 多核 SMP 下不可靠（cap_fix8
// 链A 实证 pushSwapBytePixels 数据竞争崩溃）。两方都必须持同一把锁才能保证
// SPI 寄存器/DMA 访问串行。
extern SemaphoreHandle_t g_spiMutex;

extern uint16_t touchCalib[];

enum UIMode {
  MODE_CLOCK = 0,
  MODE_CALENDAR,
  MODE_RADIO,
  MODE_WEATHER,
  MODE_COUNT
};
extern UIMode currentMode;
extern bool modeChanged;
extern bool modeDrawn[MODE_COUNT];
extern bool navDirty;

#define NAV_BAR_Y       288
#define NAV_BAR_HEIGHT  32
#define NAV_BTN_WIDTH   120
#define NAV_BTN_COUNT   4
extern const char* navLabels[];

#define COLOR_BG       TFT_BLACK
#define COLOR_TEXT     TFT_WHITE
#define COLOR_ACCENT   TFT_CYAN
#define COLOR_NAV_BG   0x2104
#define COLOR_NAV_SEL  0x4A69
#define COLOR_DIGIT    0x07FF
#define COLOR_DIM      0x7BEF

extern bool wifiConnected;
extern bool ntpSynced;
extern bool wifiConnecting;
extern unsigned long wifiConnectStart;
extern const char* ntpServer;
extern const int gmtOffsetSec;

extern int prevH, prevM, prevS;
extern bool colonBlink;

enum ClockStyle { STYLE_FLAP = 0, STYLE_ANALOG = 1 };
extern uint8_t clockStyle;
uint8_t getClockStyle();
void saveClockStyle(uint8_t v);

// 2026-10-20 UI 重构 (对齐 doubao app_17g7jt0w5v1 设计稿, 浅色主题):
// 状态栏 24px | 左列 150px 分类 | 右列 330px (小标题 24 + 电台行 48×4) | 播放条 48px
#define RADIO_SIDEBAR_W    150
#define RADIO_HEADER_H     24
#define RADIO_SUBHDR_H     24
#define RADIO_PLAYBAR_H    48
#define RADIO_LIST_X       (RADIO_SIDEBAR_W)
#define RADIO_LIST_W       (480 - RADIO_SIDEBAR_W)
#define RADIO_SCROLLBAR_W  5
#define RADIO_LIST_TOP     (RADIO_HEADER_H + RADIO_SUBHDR_H)
#define RADIO_LIST_BOT     (NAV_BAR_Y - RADIO_PLAYBAR_H)
#define RADIO_LIST_H       (RADIO_LIST_BOT - RADIO_LIST_TOP)
#define RADIO_SPECIAL_CNT  4
#define RADIO_SPECIAL_H    28
#define RADIO_ROW_H_SIDE   26
#define RADIO_REGION_TOP   (RADIO_HEADER_H + RADIO_SPECIAL_CNT * RADIO_SPECIAL_H)
#define RADIO_ROW_H        48
#define RADIO_STATION_ROWS 4
#define RADIO_FULLSCREEN   1
#define RADIO_DBL_MS       500
#define RADIO_PLAY_FLUSH_MS 600
struct RegionEntry {
  char name[17];
  uint32_t first_offset;
  uint16_t count;
};
extern RegionEntry* regionEntries;
extern int regionCount;
#define RMAX_STN 1700
struct RadioStation {
  char name[32];
  char freq[13];
  char region[18];
  char cat[12];
  uint8_t r, g, b;
  bool is_user;
  bool in_bin;
  uint32_t url_offset;
};
extern RadioStation* radioStn;
extern int radioStnTotal;
#define RMAX_CATS 48
struct RadioCat {
  char label[18];
  char key[18];
  bool meta;
  int  count;
  uint16_t* sids;
};
extern RadioCat radioCats[RMAX_CATS];
extern int radioCatCount;
#define RMAX_ALL 1700
extern uint16_t allSeq[RMAX_ALL]; extern int allCount;
#define RMAX_FAV 64
extern uint16_t favSids[RMAX_FAV]; extern int favCount;
#define RMAX_HIST 48
extern uint16_t histSids[RMAX_HIST]; extern int histCount;
extern int radioBinTotal;
extern int extraCount;
#define RMAX_EXTRA 24
extern char extraUrl[RMAX_EXTRA][260]; extern int extraUrlCount;
static const char* RADIO_CATNAMES[] = { "交通","综合","经济","新闻","文艺","音乐","体育","农村","影视","教育","生活" };
static const uint16_t RADIO_CATCOLS[] = { 0x34DB, 0x324B, 0xF4E2, 0xE267, 0xE3E4, 0x9AD6, 0x2E6E, 0x256C, 0xD2A0, 0x2C17, 0x1DF3 };
#define RADIO_CATN (sizeof(RADIO_CATNAMES) / sizeof(RADIO_CATNAMES[0]))
static const char* RLBL[] = { "全国", "收藏", "历史", "个人" };
#define RPINNED "广东"
extern char radioPinnedUsed[18];
static const char* RORDER[] = { "山东","浙江","河北","江苏","河南","吉林","四川","安徽","山西","辽宁","湖北","云南","新疆","黑龙江","湖南","江西","广西","内蒙古","贵州","福建","北京","陕西","青海","上海","香港","重庆","宁夏","甘肃","台湾","海南","西藏","天津","澳门" };
static const int RORDER_N = 33;
extern int currentCat;
extern int currentPlayingSid;
extern bool radioPlaying;
extern bool radioInited;
extern int radioVolume;
extern bool radioMuted;          // 播放条静音 (点按音量区切换)
extern uint16_t g_vuL, g_vuR;    // audio_process_i2s 钩子采样 L/R 峰值 (0..32767)
extern bool musicPlaying;
extern int regionScrollTop;
extern int listScrollTop;
extern bool showingFullscreen;
extern unsigned long lastTapTime;
extern int lastTapRow;
extern int pendingPlaySid;
extern unsigned long pendingPlayAt;
extern bool listScrollDirty;
extern bool sideScrollDirty;

extern bool weatherInited;
extern int mockTempC;
extern int mockHumidity;

extern const char* lunarMonths[];
extern const char* lunarDays[];
extern const char* weekDaysFull[];

int almBigText(int x, int y, const String& s, int dst, uint16_t fg, uint16_t bg, bool center);
int almScaledText(int x, int ytop, const String& s, int size, uint16_t fg, uint16_t bg, bool center);
void drawMoonIcon(int cx, int cy, int R, double phase);
uint16_t tempColor(int c);
uint16_t humidColor(int h);
uint16_t eventColor(const char* name);

void initTFT();
void initTouch();
void drawNavBar();
void handleTouch();
void modeClock();
void allocClockBufs();
void allocClockBufsResident();  // BUILDV5-CLKFIX-A: boot 常驻分配(永不释放, 根治播放期切时钟空白)
void freeClockBufs();
void modeCalendar();
void drawCalendar();
void drawCalPicker();
void handleCalendarTap(uint16_t tx, uint16_t ty);
void handlePickerTap(uint16_t tx, uint16_t ty);
void modeWeather();
void drawWeatherScreen();
void modeRadio();
void pumpRadio();
void handleRadioTap(uint16_t tx, uint16_t ty);
void drawRadioHeader();
void drawRadioRegionSidebar();
void drawStationRow(int rowY, RadioStation* st, bool playing, bool favorited);
void drawRadioList();
void drawRadioPlayBar();
void drawStationFullscreen();
void drawRadioScrollbar();
bool loadRadioDB();
bool loadRadioMeta();
String readURLForSid(int sid);
void playStationAt(int sid);
void stopRadio();
void radioPause();               // 暂停 (保留 currentPlayingSid, 播放条可续播)
void radioToggleMute();          // 静音切换
void radioToggleFav(int sid);    // 星标收藏/取消 (NVS 持久化 + 收藏分类实时同步)
void radioPushHist(int sid);     // 成功起播后自动记历史 (保留最近 30 个, NVS 持久化)
void radioNvsLoad();             // 开机读回 NVS 收藏/历史
void buildRadioCats();
const char* radioPinnedKey();
RadioStation* stn(int sid);
RadioCat* catv(int i);
int  curCount();
uint16_t curSidAt(int i);
RadioStation* curStnAt(int i);
const char* catLabel(int i);
const char* regionNameOf(const RadioStation* s);
const char* iconChar(const RadioStation* s);
void truncCopy(char* dst, int n, const uint8_t* s, int len);
uint16_t catColor565(const char* cat);
bool isFavSid(int sid);
void checkFestivalMusic(int year, int mon, int mday);
void loadFestivalConfig();
void sdProbe();

// ========== 时钟布局常量 ==========
#define CARD_W        56
#define COLON_W       12
#define CARD_GAP      6
#define COLON_GAP     10
#define CARD_TOP      48
#define CARD_H        120
#define CARD_RADIUS   10
#define DIGIT_SCALE   2
#define DATE_LEFT     31
#define DATE_RIGHT    449
#define ROW1_TOP      186
#define ROW1_H        46
#define ROW2_TOP      240
#define ROW2_H        36
#define CARD_X0       31
#define CARD_X1       (CARD_X0 + CARD_W + CARD_GAP)
#define COLON_X       (CARD_X1 + CARD_W + COLON_GAP)
#define CARD_X2       (COLON_X + COLON_W + COLON_GAP)
#define CARD_X3       (CARD_X2 + CARD_W + CARD_GAP)
#define COLON_X2      (CARD_X3 + CARD_W + COLON_GAP)
#define CARD_X4       (COLON_X2 + COLON_W + COLON_GAP)
#define CARD_X5       (CARD_X4 + CARD_W + CARD_GAP)
#define FLIP_TOP_MS   400
#define FLIP_BOT_MS   340
#define FLAP_FRAME_MS 40
#define ANALOG_FRAME_MS 50  // BUILDV5-HLSFIX: 33→50ms（15fps→20fps，秒针仍平滑 6°/s；
                            // 减 pushImage 频率 → 压 TFT_eSPI SMP 低频竞态（cap_fix12 1次/164s）
#define ROLLER_W      16
#define ROLLER_PITCH  14
#define GAP1_CX       (COLON_X  + COLON_W / 2)
#define GAP2_CX       (COLON_X2 + COLON_W / 2)
#define CARD_FACE     0xFFFF
#define CARD_SEAM     0x3A48
#define HINGE_HI      0x9514
#define HINGE_MID     0x4ACB
#define HINGE_LO      0x1945
#define CARD_SHADE    0x1084
#define CARD_EDGE     0x39C6
#define CARD_NUM      0x0000
#define DATE_STRIP    0x18E3
#define ROW1_BG       0x2146
#define DATE_SEP      0x0821
#define COLOR_GOLD    0xFE4C
#define SOFT_C        0x94F6
#define MOON_LIT      0xFF97
#define MOON_DARK     0x2146
#define MOON_EDGE     0x424B
#define FLAP_BAND_LINE 0x7350

// ========== 日历/黄历 常量 ==========
#define CAL_OFF       0xF800
#define CAL_WORK      0x6CF1
#define CAL_LIGHT_BG  0xFFFF
#define CAL_GRID      0xC65A
#define CAL_HDR       0x5ACB
#define CAL_TITLE     0x12D9
#define CAL_NORM      0x31A6
#define CAL_STRIP_L   0xEF9F
#define CAL_GOLD_L    0xCC21
#define CAL_WORK_L    0x01F4
#define CAL_X0        16
#define CAL_CELL_W    64
#define CAL_CELL_H    30
#define CAL_X1        (CAL_X0 + 7 * CAL_CELL_W)
#define CAL_GRID_TOP  54
#define CAL_WKND      0xF800
#define PICK_PANEL_X  20
#define PICK_PANEL_W  440
#define PICK_PT       40
#define PICK_PB       250
#define PICK_MCOL_W   60
#define PICK_MROW_H   40
#define PICK_MGRID_X  60
#define PICK_MTOP     128
#define ALM_PAPER     0xEF9C
#define ALM_RED       0xB9C5
#define ALM_DARK      0x3985
#define ALM_LINE      0xCCB0
#define ALM_JI        0x6D0C
#define ALM_RING      0xE5F5
#define ALM_WKND      0x8A52


// ========== 天气模块常量 ==========
#define W_BG      COLOR_BG
#define W_PANEL   0x2104
#define W_EDGE    0x4208
#define W_TEXT    TFT_WHITE
#define W_DIM     0xBDF7
#define W_ACCENT  0x07FF
#define W_SUN     0xFFE0


#endif // UI_COMMON_H
