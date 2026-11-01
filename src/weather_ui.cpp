// ============================================================
// weather_ui.cpp -- 天气页 (480x320, 深色主题 V2)
// 严格对齐设计稿 app_17gm0gncnrd (交互版 480x320):
//   深色底 #15202e / 顶栏 #0e1a26 / 卡片 #223147 边 #3d5670 / 温度金 #ffd166
//   顶栏: 定位点+城市区县(可点切城) | 设置/预警/台风/刷新按钮 + 时间
//   左实况区: 128px 矢量图标 / 温度(带一位小数) / 天气 / 紫外线 / 日出日落
//   右信息区: AQI 卡 / 详情卡 / 预警药丸 / 7天预报入口 / 室内(GY-39)元信息
//   弹窗: 台风(风速/气压/相对位置/移向/风圈) / 预警(发布单位/正文/严重程度) / 选城
//   7 天预报二级页全屏覆盖, "‹ 返回"回主页。
// 主页全屏 (功能栏默认隐藏, 点顶栏齿轮唤出, 5s 自动收起)。
// 图标: 16 类设计稿 PNG 位图 (wicons16.c, RGB565 预合成到 WX_BG)。
// ============================================================
#include "ui_common.h"
#include "gy39.h"
#include "wicons16.h"   // 16 类天气图标位图 (设计稿 PNG)
#include <math.h>

// ---- 设计稿配色 (RGB888 -> RGB565) ----
#define WX_BG       0x1105   // #15202e 屏幕底
#define WX_TOP      0x08C4   // #0e1a26 顶栏底
#define WX_CREAM    0xFF58   // #fbe9c6 顶栏文字
#define WX_TEMP     0xFE8C   // #ffd166 温度/数字金
#define WX_WTXT     0xDF3E   // #dce7f2 天气文字
#define WX_DIM      0xAE12   // #a9bdd0 次要文字
#define WX_SUN      0xFE4D   // #ffc86b UV/日出日落强调
#define WX_CARD     0x2188   // #223147 卡片底
#define WX_CEDGE    0x3AAE   // #3d5670 卡片描边
#define WX_AQTX     0x8EF1   // #8fdc8f AQI 药丸字 (优/良)
#define WX_AQPBG    0x19C5   // #1f3a2a AQI 药丸底 (优/良)
#define WX_AQNUM    0x7ED0   // #7ddb82 AQI 数字 (优/良)
#define WX_DETV     0xFDA9   // #ffb74d 详情数值/今天/温度范围
#define WX_ALTFG    0xFC4C   // #ff8a65 预警字
#define WX_ALTBG    0x4923   // #4a2418 预警底
#define WX_ALTEDGE  0xE243   // #e64a19 预警边
#define WX_BTNRED   0xC1C5   // #c73a2a 7天预报按钮
#define WX_META     0x9D98   // #9fb0c3 元信息
#define WX_DOT      0x4E1E   // #4fc3f7 定位点/台风标
#define WX_SEP      0x29EA   // #2c3d52 预报行虚线
#define WX_LIST     0xCEDC   // #cfd8e3 列表文字
#define WX_TOPBTN   0x31E9   // 顶栏按钮底 (白 16% on #0e1a26)
#define WX_AQOBG    0x3962   // #3a2c16 AQI 轻度底
#define WX_CITYBG   0x1947   // #1a2838 城市按钮底
#define WX_SHADE    0x0883   // 弹窗遮罩 (~#0c1018, 42% 暗化近似)

// ---- 布局常量 (设计稿 480x320, 主页全屏, 功能栏默认隐藏) ----
#define WX_TOP_H     40
#define WX_LX        24      // 左区 x
#define WX_LW        208     // 左区宽
#define WX_RX        232     // 右区 x (左扩 16px, 给详情卡左列多一个中文字宽)
#define WX_RW        236     // 右区宽 (232..468, 右边距 12)
// 左区: 图标 46..166 (120px) | 温度 170..214 | 天气 220 | UV 244 | 日出日落 266 (全部 <=282, 让出 288 起的功能栏)
#define WX_ICON_Y    46
#define WX_ICON_S    120
#define WX_TEMP_Y    170
#define WX_WTXT_Y    220
#define WX_UV_Y      244
#define WX_SUN_Y     266
// 右区: AQI 46..106 | 详情卡 114..214 (3 行, 含室内) | 预警 222..250 | 预报入口 258..286
#define WX_AQ_Y      46
#define WX_AQ_H      60
#define WX_DET_Y     114
#define WX_DET_H     100
#define WX_ALT_Y     222
#define WX_ALT_H     28
#define WX_FC_Y      258
#define WX_FC_H      28

// 顶栏右侧按钮 (34x30, y5): 设置 262 | 预警 303 | 台风 344 | 刷新 385; 时间右对齐 460
#define WX_BSET_X    262
#define WX_BALT_X    303
#define WX_BTY_X     344
#define WX_BTN_X     385
#define WX_BTN_Y     5
#define WX_BTN_W     34
#define WX_BTN_H     30

static uint8_t wSub = 0;          // 0=主页 1=7天预报 2=预警弹窗 3=台风弹窗 4=城市弹窗
static int     wLastMin = -1;     // 顶栏时钟分钟缓存
// 功能栏唤出 (全屏主页: 点顶栏齿轮唤出底部导航, 5s 无操作自动收起)
static bool     wNavPeek   = false;
static uint32_t wNavPeekAt = 0;   // NAV_PEEK_MS 定义在 ui_common.h (2026-10-31 统一)

// ---- 文字工具 ----
// 清晰度修复: size<=16 直接用 16px 原生 AA 字库 (font16.drawStringFastAA, 逐字真实 advance,
// 不做 64px->小字降采样)。此前全部走 almScaledText -> font32(64px 源) 缩到 12~15px,
// 每目标像素平均 5x5 源块 -> 笔画糊成灰边, 就是"小字看不清"的根因。
#define WX_SMALL_MAX 16
static bool wSmallMode(int size) { return size <= WX_SMALL_MAX && font16.isReady(); }

static int wMeasure(const String& s, int size) {
  if (wSmallMode(size)) return font16.textWidth(s);
  int total = 0;
  for (unsigned i = 0; i < s.length(); i++) {
    uint8_t c0 = s[i];
    if (c0 < 0x80) total += size * 7 / 10;
    else if (c0 < 0xE0) { i++; total += size; }
    else if (c0 < 0xF0) { i += 2; total += size; }
    else i += 3;
  }
  return total;
}
static int wText(int x, int ytop, const String& s, int size, uint16_t fg, uint16_t bg, bool bold) {
  if (wSmallMode(size)) {
    // 原生 16px AA 直出 (drawStringFast 在 AA 模式下即 drawStringFastAA)
    (void)bold;   // 灰度合成带底色填充, 1px 偏移二描会擦掉上一描, 不做伪粗体
    font16.drawStringFast(x, ytop, s, fg, bg);
    return font16.textWidth(s);
  }
  int w = almScaledText(x, ytop, s, size, fg, bg, false);
  if (bold) almScaledText(x + 1, ytop, s, size, fg, bg, false);
  return w;
}
// 单字宽 (原生 16px 时取真实 advance, 保证换行/截断与绘制一致)
static int wCharW(const String& ch, int size) {
  return wSmallMode(size) ? font16.textWidth(ch) : wMeasure(ch, size);
}
// 按像素宽截断 UTF-8, 超宽加 ".."
static String truncW(const String& s, int size, int maxW) {
  int w = 0; String out;
  for (unsigned i = 0; i < s.length();) {
    unsigned cl = 1; uint8_t c0 = s[i];
    if (c0 >= 0xF0) cl = 4; else if (c0 >= 0xE0) cl = 3; else if (c0 >= 0xC0) cl = 2;
    String one = s.substring(i, i + cl);
    int cw = wCharW(one, size);
    if (w + cw > maxW) { out += ".."; break; }
    out += one; w += cw; i += cl;
  }
  return out;
}
// 弹窗正文逐字换行, 返回行数
static int drawWrapped(int x, int y, int maxW, const String& s, int size, uint16_t fg, uint16_t bg, int maxLines, int lh) {
  int cx = x, lines = 1;
  for (unsigned i = 0; i < s.length();) {
    unsigned cl = 1; uint8_t c0 = s[i];
    if (c0 >= 0xF0) cl = 4; else if (c0 >= 0xE0) cl = 3; else if (c0 >= 0xC0) cl = 2;
    String one = s.substring(i, i + cl);
    int cw = wCharW(one, size);
    if (cx + cw > x + maxW) {
      lines++;
      if (lines > maxLines) { wText(cx, y, "..", size, fg, bg, false); return lines; }
      cx = x; y += lh;
    }
    wText(cx, y, one, size, fg, bg, false);
    cx += cw; i += cl;
  }
  return lines;
}

// ============================================================
// 16 类天气图标 —— 设计稿 PNG 位图 (wicons16.c, 512x512 RGBA -> RGB565,
// 透明底已预合成到 WX_BG)。大图 120px (主页), 小图 28px (7天页)。
// ============================================================
// 图标主入口: type 0..15, (cx,cy) 中心, s 边长 (>=60 用 120 图, 否则 28 图)
static void wIcon(int type, int cx, int cy, int s, uint16_t bg) {
  if (type < 0) type = 0; if (type > 15) type = 15;
  (void)bg;   // 位图已预合成到底色
  if (s >= 60) tft.pushImage(cx - WICON16_BIG / 2, cy - WICON16_BIG / 2, WICON16_BIG, WICON16_BIG, wiconBig[type]);
  else         tft.pushImage(cx - WICON16_SMALL / 2, cy - WICON16_SMALL / 2, WICON16_SMALL, WICON16_SMALL, wiconSmall[type]);
}

// ---- 等级映射 ----
static const char* uvLevel(float uv) {
  if (uv < 3) return "弱"; if (uv < 6) return "中等"; if (uv < 8) return "强";
  if (uv < 11) return "很强"; return "极强";
}
static const char* aqiLevel(int a) {
  if (a <= 50) return "优"; if (a <= 100) return "良"; if (a <= 150) return "轻度";
  if (a <= 200) return "中度"; if (a <= 300) return "重度"; return "严重";
}
// AQI 药丸配色 (设计稿: 优/良 绿, 轻度 橙, 中度+ 红)
static void aqiColors(int a, uint16_t& fg, uint16_t& bg) {
  if (a <= 100)      { fg = WX_AQNUM;  bg = WX_AQPBG; }
  else if (a <= 150) { fg = WX_DETV;   bg = WX_AQOBG; }
  else               { fg = WX_ALTFG;  bg = WX_ALTBG; }
}
// 预警级别字 -> 配色 (设计稿默认 #ff8a65 on #4a2418 边 #e64a19)
static void alertColors(const char* cn, uint16_t& fg, uint16_t& bg, uint16_t& edge) {
  if (!strcmp(cn, "红"))      { fg = 0xE184; bg = 0x4823; edge = 0xE184; }   // 红调
  else if (!strcmp(cn, "黄")) { fg = 0xFE47; bg = 0x4A23; edge = 0xFE47; }   // 黄调
  else if (!strcmp(cn, "蓝")) { fg = 0x4E1E; bg = 0x1947; edge = 0x4E1E; }   // 蓝调
  else                        { fg = WX_ALTFG; bg = WX_ALTBG; edge = WX_ALTEDGE; }
}

// ---- 手绘小符号 (emoji 字体不支持) ----
static void wChevronRight(int x, int y, int h, uint16_t c) {
  for (int i = 0; i < 2; i++) {
    tft.drawLine(x + i, y, x + h / 2 + i, y + h / 2, c);
    tft.drawLine(x + h / 2 + i, y + h / 2, x + i, y + h, c);
  }
}
static void wChevronLeft(int x, int y, int h, uint16_t c) {
  for (int i = 0; i < 2; i++) {
    tft.drawLine(x + h / 2 + i, y, x + i, y + h / 2, c);
    tft.drawLine(x + i, y + h / 2, x + h / 2 + i, y + h, c);
  }
}
// 刷新: 圆弧 + 切向箭头 (↻) —— 旧版"圆圈 + 顶部三角"容易被看成水滴图标
static void wRefreshIcon(int cx, int cy, uint16_t c) {
  const float D = 3.14159265f / 180.0f, R = 7.0f;
  for (int a = 20; a <= 300; a += 3) {           // 缺口留在右上 (300..380)
    float r = a * D;
    tft.drawPixel(cx + (int)lroundf(cosf(r) * R), cy + (int)lroundf(sinf(r) * R), c);
    tft.drawPixel(cx + (int)lroundf(cosf(r) * (R - 1)), cy + (int)lroundf(sinf(r) * (R - 1)), c);
  }
  // 箭头头 (弧末端 300° 处, 指向顺时针切向)
  float re = 300 * D;
  int px = cx + (int)lroundf(cosf(re) * R), py = cy + (int)lroundf(sinf(re) * R);
  float vx = -sinf(re), vy = cosf(re);            // 顺时针切向 (屏幕坐标 y 向下)
  int tipx = px + (int)lroundf(vx * 4), tipy = py + (int)lroundf(vy * 4);
  int nx = (int)lroundf(-vy * 4), ny = (int)lroundf(vx * 4);
  tft.fillTriangle(tipx, tipy, px + nx, py + ny, px - nx, py - ny, c);
}
// 预警: 三角 + "!" (设计稿 ⚠)
static void wWarnIcon(int cx, int cy, uint16_t c, uint16_t bg) {
  tft.fillTriangle(cx, cy - 7, cx - 8, cy + 6, cx + 8, cy + 6, c);
  tft.fillRect(cx - 1, cy - 3, 3, 6, bg);
  tft.fillRect(cx - 1, cy + 4, 3, 2, bg);
}
// 台风: 同心旋风圈 (设计稿 🌀, 蓝 #4fc3f7)
static void wTyphoonIcon(int cx, int cy, uint16_t c) {
  tft.drawCircle(cx, cy, 8, c);
  tft.drawCircle(cx, cy, 5, c);
  tft.fillCircle(cx, cy, 2, c);
}
// 设置: 齿轮 (设计稿 ⚙)
static void wGearIcon(int cx, int cy, uint16_t c) {
  tft.drawCircle(cx, cy, 6, c);
  tft.fillCircle(cx, cy, 2, c);
  for (int i = 0; i < 8; i++) {
    float a = i * 3.14159265f / 4.0f;
    tft.drawLine(cx + (int)(cosf(a) * 7), cy + (int)(sinf(a) * 7),
                 cx + (int)(cosf(a) * 9), cy + (int)(sinf(a) * 9), c);
  }
}

// ---- mock 数据 (离线/未联网兜底) ----
static const struct {
  const char* city; const char* district; const char* cond; int icon;
  float temp; float feels; int hi; int lo;
  int humidity; int windSpeed; const char* windDir; int windScale;
  int pressure; int uv; float visKm;
  int aqi; int pm25; int pm10;
  const char* sunrise; const char* sunset;
  const char* day[7]; const char* dayDate[7]; const char* dayCond[7];
  int dayIcon[7]; int dayHi[7]; int dayLo[7]; int dayHum[7]; int dayAqi[7];
} WMOCK = {
  "深圳", "南山", "多云", 1,
  28.5f, 26.4f, 28, 22,
  62, 18, "东南", 3,
  1008, 4, 9.0f,
  42, 12, 28,
  "06:15", "17:42",
  {"今天","明天","周六","周日","周一","周二","周三"},
  {"10/22","10/23","10/24","10/25","10/26","10/27","10/28"},
  {"多云","晴","晴","多云","小雨","中雨","阴"},
  {1,0,0,1,4,5,3},
  {25,26,27,25,24,23,24},
  {18,19,20,19,18,17,18},
  {72,68,65,70,85,88,75},
  {42,55,70,88,112,95,60},
};

static WData wData() {
  const WData& r = weather::get();
  if (r.ok) return r;
  const auto& m = WMOCK;
  WData d;
  d.ok = true;
  snprintf(d.city,    sizeof(d.city),    "%s", m.city);
  snprintf(d.district,sizeof(d.district),"%s", m.district);
  snprintf(d.cond,    sizeof(d.cond),    "%s", m.cond);
  snprintf(d.windDir, sizeof(d.windDir), "%s", m.windDir);
  snprintf(d.date,    sizeof(d.date),    "10-22 周四");
  snprintf(d.update,  8, "12:03");
  snprintf(d.sunrise, 8, "%s", m.sunrise);
  snprintf(d.sunset,  8, "%s", m.sunset);
  d.icon = m.icon; d.temp = m.temp; d.feels = m.feels; d.hi = m.hi; d.lo = m.lo;
  d.humidity = m.humidity; d.windSpeed = m.windSpeed; d.windScale = m.windScale;
  d.pressure = m.pressure; d.uv = (float)m.uv; d.visKm = m.visKm;
  d.aqi = m.aqi; d.pm25 = m.pm25; d.pm10 = m.pm10;
  for (int i = 0; i < 7; i++) {
    snprintf(d.dayLabel[i], 8, "%s", m.day[i]);
    snprintf(d.dayDate[i],  6, "%s", m.dayDate[i]);
    snprintf(d.dayCond[i], sizeof(d.dayCond[i]), "%s", m.dayCond[i]);
    d.dayIcon[i] = m.dayIcon[i]; d.dayHi[i] = m.dayHi[i]; d.dayLo[i] = m.dayLo[i];
    d.dayHum[i] = m.dayHum[i]; d.dayAqi[i] = m.dayAqi[i];
  }
  return d;
}

// ---- 顶栏时间 (16px 原生字库, 清晰无缩放) ----
static void drawTopTime() {
  char tb[8] = "--:--";
  time_t now = time(nullptr);
  struct tm lt; epochToLocalNoLock(now, lt);   // BUILDV5-TZFIX: 无锁 UTC+8
  if (lt.tm_year + 1900 >= 2024) snprintf(tb, sizeof(tb), "%02d:%02d", lt.tm_hour, lt.tm_min);
  tft.fillRect(420, WX_BTN_Y, 48, WX_BTN_H, WX_TOP);   // 从 420 起, 不压到刷新按钮 (385..419)
  int wid = wMeasure(tb, 16);
  wText(468 - wid, 12, String(tb), 16, WX_CREAM, WX_TOP, false);
}

// ---- 顶栏: 定位点+城市区县 (左, 可点切城) | 设置/预警/台风/刷新 + 时间 (右) ----
static void drawWeatherTopBar(const WData& w) {
  tft.fillRect(0, 0, 480, WX_TOP_H, WX_TOP);
  tft.fillCircle(24, 20, 4, WX_DOT);
  // 设计稿: "深圳 南山" 空格分隔, 点击弹城市选择
  String loc = String(w.city);
  if (w.district[0]) { loc += " "; loc += w.district; }
  wText(34, 12, truncW(loc, 16, 200), 16, WX_CREAM, WX_TOP, false);   // 上限 200px, 不压到齿轮键
  // 设置 (装饰, 对齐设计稿)
  tft.fillRoundRect(WX_BSET_X, WX_BTN_Y, WX_BTN_W, WX_BTN_H, 6, WX_TOPBTN);
  wGearIcon(WX_BSET_X + 17, WX_BTN_Y + 15, WX_CREAM);
  // ⚠ 预警按钮 (有预警才显示)
  if (w.alertCount > 0) {
    tft.fillRoundRect(WX_BALT_X, WX_BTN_Y, WX_BTN_W, WX_BTN_H, 6, WX_TOPBTN);
    wWarnIcon(WX_BALT_X + 17, WX_BTN_Y + 15, WX_DETV, WX_TOPBTN);
  }
  // 🌀 台风按钮 (有台风才显示)
  if (w.typhoon) {
    tft.fillRoundRect(WX_BTY_X, WX_BTN_Y, WX_BTN_W, WX_BTN_H, 6, WX_TOPBTN);
    wTyphoonIcon(WX_BTY_X + 17, WX_BTN_Y + 15, WX_DOT);
  }
  // 刷新
  tft.fillRoundRect(WX_BTN_X, WX_BTN_Y, WX_BTN_W, WX_BTN_H, 6, WX_TOPBTN);
  wRefreshIcon(WX_BTN_X + WX_BTN_W / 2, WX_BTN_Y + WX_BTN_H / 2, WX_CREAM);
  drawTopTime();
}

// ---- 详情卡 (232,114,236,100): 三行两列 16px 原生字库 ----
// 行1 湿度 | 气压   行2 风向风力 | 能见度   行3 室内(GY-39 温/湿) | 海拔
// 卡片左扩 16px: 左列 (cx0..cx1) 加宽一个中文字, 4 字风向也能带"风"字; 右列位置不变。
// 局部刷新只擦数值的小矩形并重画文字, 不整卡填充 (填充整卡会闪)。
static void drawDetailChrome() {
  tft.fillRoundRect(WX_RX, WX_DET_Y, WX_RW, WX_DET_H, 9, WX_CARD);
  tft.drawRoundRect(WX_RX, WX_DET_Y, WX_RW, WX_DET_H, 9, WX_CEDGE);
}
// 数值区原位更新: 直接覆盖旧字 (每个字形自带底色填充), 只有新字变短时才擦掉尾部残留,
// 不做整块 fillRect —— 整块擦除会先出现一片空白再写字, 肉眼就是"闪一下"。
static void drawDetailValues(const WData& w) {
  const gy39::Data& gs = gy39::get();
  const int cx0 = WX_RX + 10, cx1 = WX_RX + 128, rgt = WX_RX + WX_RW - 10;
  const int ry1 = WX_DET_Y + 12, ry2 = WX_DET_Y + 44, ry3 = WX_DET_Y + 76;
  static int pwHum = 0, pwPres = 0, pwWind = 0, pwVis = 0, pwIndoor = 0, axAlt = -1;
  char vb[24];
  // 行1 左: 湿度
  {
    int dx = cx0 + wText(cx0, ry1, "湿度 ", 16, WX_LIST, WX_CARD, false);
    snprintf(vb, sizeof(vb), "%d%%", w.humidity);
    int vw = wText(dx, ry1, vb, 16, WX_DETV, WX_CARD, true);
    if (pwHum > vw) tft.fillRect(dx + vw, ry1, pwHum - vw, 16, WX_CARD);   // 变短才擦尾
    pwHum = vw;
  }
  // 行1 右: 气压 (GY-39 在线用本地实测, 否则 API 海平面气压)
  {
    int dx = cx1 + wText(cx1, ry1, "气压 ", 16, WX_LIST, WX_CARD, false);
    snprintf(vb, sizeof(vb), "%.0fhPa", gs.ok ? gs.pressureHpa : w.pressure);
    int vw = wText(dx, ry1, vb, 16, WX_DETV, WX_CARD, true);
    if (pwPres > vw) tft.fillRect(dx + vw, ry1, pwPres - vw, 16, WX_CARD);
    pwPres = vw;
  }
  // 行2 左: 风向风力: 风向 + "风" 用列表色 (不跟风级同色), 只有风级数值强调;
  // 左列已加宽一个中文字, 4 字方位名 (如"东南偏南") 也能完整带"风"字。
  {
    const String dir = String(w.windDir);
    const String lv  = String(w.windScale) + "级";
    int ww = wText(cx0, ry2, dir, 16, WX_LIST, WX_CARD, false);
    ww += wText(cx0 + ww, ry2, "风", 16, WX_LIST, WX_CARD, false) + 1;
    ww += wText(cx0 + ww, ry2, lv, 16, WX_DETV, WX_CARD, true);
    if (pwWind > ww) tft.fillRect(cx0 + ww, ry2, pwWind - ww, 16, WX_CARD);
    pwWind = ww;
  }
  // 行2 右: 能见度
  {
    int dx = cx1 + wText(cx1, ry2, "能见度 ", 16, WX_LIST, WX_CARD, false);
    snprintf(vb, sizeof(vb), "%.0fkm", (double)w.visKm);
    int vw = wText(dx, ry2, vb, 16, WX_DETV, WX_CARD, true);
    if (pwVis > vw) tft.fillRect(dx + vw, ry2, pwVis - vw, 16, WX_CARD);
    pwVis = vw;
  }
  // 行3: 室内 GY-39 温/湿 (左) + 海拔 (右对齐到卡片内缘) — 数据缺失时 "--"
  {
    if (gs.ok) snprintf(vb, sizeof(vb), "室内 %.1f° %.0f%%", (double)gs.tempC, (double)gs.humidity);
    else       snprintf(vb, sizeof(vb), "室内 --° --%%");
    int iw = wText(cx0, ry3, String(vb), 16, WX_META, WX_CARD, false);
    if (pwIndoor > iw) tft.fillRect(cx0 + iw, ry3, pwIndoor - iw, 16, WX_CARD);
    pwIndoor = iw;
    char ab[20];
    // 海拔: 优先 open-meteo 模型地形海拔 (只随坐标变, 稳定); 无则退回 GY-39 气压反算
    int elev = (w.elevM > 0) ? w.elevM : (gs.ok ? (int)(gs.altitudeM + 0.5f) : 0);
    if (elev > 0) snprintf(ab, sizeof(ab), "海拔 %dm", elev);
    else          snprintf(ab, sizeof(ab), "海拔 --m");
    String as = String(ab);
    int aw = wMeasure(as, 16);
    int ax = rgt - aw;
    if (axAlt >= 0 && ax > axAlt) tft.fillRect(axAlt, ry3, ax - axAlt, 16, WX_CARD);   // 变窄: 起点右移, 擦掉左侧旧字头
    wText(ax, ry3, as, 16, WX_META, WX_CARD, false);
    axAlt = ax;
  }
}
// 显示值是否变化 (GY 每 2s 更新但数值常年不动 -> 不变就不重画, 彻底消除周期闪烁)
static bool detailValuesChanged(const WData& w) {
  const gy39::Data& gs = gy39::get();
  static int lHum = -1, lPres = -1, lT10 = -9999, lIH = -9999, lAlt = -9999, lWind = -1, lVis = -1;
  static char lDir[16] = "";
  int hum  = w.humidity;
  int pres = (int)((gs.ok ? gs.pressureHpa : (float)w.pressure) + 0.5f);
  int t10  = gs.ok ? (int)(gs.tempC * 10 + 0.5f) : -9999;
  int ih   = gs.ok ? (int)(gs.humidity + 0.5f) : -9999;
  int alt  = (w.elevM > 0) ? w.elevM : (gs.ok ? (int)(gs.altitudeM + 0.5f) : -9999);
  int vis  = (int)(w.visKm + 0.5f);
  bool ch = hum != lHum || pres != lPres || t10 != lT10 || ih != lIH || alt != lAlt ||
            w.windScale != lWind || vis != lVis || strcmp(w.windDir, lDir) != 0;   // 风向变化也要重画
  lHum = hum; lPres = pres; lT10 = t10; lIH = ih; lAlt = alt; lWind = w.windScale; lVis = vis;
  snprintf(lDir, sizeof(lDir), "%s", w.windDir);
  return ch;
}
static void drawDetailCard(const WData& w) {
  drawDetailChrome();
  drawDetailValues(w);
}

// ---- 日出日落行 (独立函数: 功能栏收起后局部恢复用) ----
static void drawSunLine(const WData& w) {
  int sx = WX_LX + wText(WX_LX, WX_SUN_Y, "日出 ", 16, WX_DIM, WX_BG, false);
  sx += wText(sx, WX_SUN_Y, String(w.sunrise), 16, WX_SUN, WX_BG, true);
  sx += wText(sx, WX_SUN_Y, " · 日落 ", 16, WX_DIM, WX_BG, false);
  wText(sx, WX_SUN_Y, String(w.sunset), 16, WX_SUN, WX_BG, true);
}

// ---- 功能栏收起后恢复: 288..320 里只有底色 (内容全部在 286 以上) ----
static void restoreBottom() {
  tft.startWrite();
  tft.fillRect(0, NAV_BAR_Y, 480, 320 - NAV_BAR_Y, WX_BG);
  tft.endWrite();
}

// ---- 主页面 (全屏 480x320, 功能栏默认隐藏, 齿轮唤出) ----
// soft=true: 主页已在屏上时的数据刷新 —— 不整屏填充底色, 只擦会缩小的文字块后原位重画。
// 整屏 fillRect 会让画面先空白一小段再重画, 肉眼就是"闪一下"(风向/文字刷新时最明显);
// 卡片各自 fillRoundRect 覆盖自身, 图标位图自带底色且覆盖整个 120px 方块, 故无需全屏清底。
static void drawWeatherScreenEx(bool soft) {
  const WData w = wData();
  static int lastIcon = -1;
  if (!soft) {
    tft.fillRect(0, 0, 480, 320, WX_BG);
  }
  // 软刷新去闪烁: 只有"数值真的变了"的区块才擦+重画, 未变化的整块跳过。
  // 大块 fillRect 后再写字会让该区域先空白再出字, 肉眼就是"闪一下"(风向刷新时最明显)。
  int t10 = (int)(w.temp * 10 + 0.5f), f10 = (int)(w.feels * 10 + 0.5f);
  static int lT10 = -9999, lF10 = -9999;
  bool tempCh = !soft || t10 != lT10 || f10 != lF10;
  lT10 = t10; lF10 = f10;
  static char lCond[24] = "";
  bool condCh = !soft || strcmp(w.cond, lCond) != 0;
  snprintf(lCond, sizeof(lCond), "%s", w.cond);
  static int lUv = -9999;
  int uvKey = (int)(w.uv * 10 + 0.5f) * 2 + (weather::isDaytime() ? 1 : 0);
  bool uvCh = !soft || uvKey != lUv;
  lUv = uvKey;
  static char lSun[24] = "";
  char sunK[24]; snprintf(sunK, sizeof(sunK), "%s%s", w.sunrise, w.sunset);
  bool sunCh = !soft || strcmp(sunK, lSun) != 0;
  snprintf(lSun, sizeof(lSun), "%s", sunK);
  if (tempCh) tft.fillRect(WX_LX, WX_TEMP_Y, WX_LW, 44, WX_BG);       // 温度/体感
  if (condCh) tft.fillRect(WX_LX, WX_WTXT_Y - 2, WX_LW, 26, WX_BG);   // 天气文字
  if (uvCh)   tft.fillRect(WX_LX, WX_UV_Y - 1, WX_LW, 20, WX_BG);     // 紫外线
  if (sunCh)  tft.fillRect(WX_LX, WX_SUN_Y - 1, WX_LW, 20, WX_BG);    // 日出日落

  // 顶栏整条 480x40 填充代价大: 只有城市/预警/台风状态变化时才重画
  {
    static char lLoc[48] = "";
    static int  lTopA = -1;
    static bool lTopTy = false;
    String loc = String(w.city);
    if (w.district[0]) { loc += " "; loc += w.district; }
    if (!soft || strcmp(loc.c_str(), lLoc) != 0 || w.alertCount != lTopA || w.typhoon != lTopTy) {
      drawWeatherTopBar(w);
    }
    snprintf(lLoc, sizeof(lLoc), "%s", loc.c_str());
    lTopA = w.alertCount; lTopTy = w.typhoon;
  }

  // ---- 左实况区 ----
  // 大图标 112px (24,46..158), 中心 (80,102)
  if (!soft || w.icon != lastIcon)
    wIcon(w.icon, WX_LX + WX_ICON_S / 2, WX_ICON_Y + WX_ICON_S / 2, WX_ICON_S, WX_BG);
  lastIcon = w.icon;

  // 温度行: "28.5" 44px 金 + "°" 22px + "体感 26.4°" 16px
  if (tempCh) {
    char tb[12]; snprintf(tb, sizeof(tb), "%.1f", (double)w.temp);
    // 字库的 '.' 在 44px 等宽字形格里几乎占满 advance (30px), 整串画会在小数点后
    // 留出 ~18px 大缝。这里整数/小数分开画: 小数点用 5x5 实心块贴基线, 间距手工收紧。
    const char* dot = strchr(tb, '.');
    String ip = dot ? String(tb).substring(0, (unsigned)(dot - tb)) : String(tb);
    int tx = WX_LX + almBigText(WX_LX, WX_TEMP_Y, ip, 44, WX_TEMP, WX_BG, false);
    if (dot) {
      tft.fillRect(tx + 2, WX_TEMP_Y + 35, 5, 5, WX_TEMP);              // 小数点
      tx += 10;
      tx += almBigText(tx, WX_TEMP_Y, String(dot + 1), 44, WX_TEMP, WX_BG, false);
    }
    font16.drawBigChar(tx + 2, WX_TEMP_Y, 0x00B0, 22, WX_TEMP, WX_BG);   // °
    tx += 20;
    char fb[20]; snprintf(fb, sizeof(fb), "体感 %.1f°", (double)w.feels);
    wText(tx + 4, WX_TEMP_Y + 28, String(fb), 16, WX_DIM, WX_BG, false);
  }

  // 天气文字 20px
  if (condCh) almScaledText(WX_LX, WX_WTXT_Y, String(w.cond), 20, WX_WTXT, WX_BG, false);

  // 紫外线 16px: label dim + 等级·指数 强调
  // 对齐 WinCalender: 夜间 (或 uv<=0) 显示"弱"; 白天显示等级 + 指数。
  if (uvCh) {
    int ux = WX_LX + wText(WX_LX, WX_UV_Y, "紫外线 ", 16, WX_DIM, WX_BG, false);
    char uvb[24];
    if (w.uv <= 0.0f || !weather::isDaytime()) {
      snprintf(uvb, sizeof(uvb), "弱");
    } else {
      snprintf(uvb, sizeof(uvb), "%s · 指数 %d", uvLevel(w.uv), (int)(w.uv + 0.5f));
    }
    wText(ux, WX_UV_Y, uvb, 16, WX_SUN, WX_BG, true);
  }

  // 日出日落 16px
  if (sunCh) drawSunLine(w);

  // ---- 右信息区 ----
  // AQI 卡: 软刷新且 AQI/PM 未变时不重画整卡 (避免整卡填充闪烁)
  static int lAqi = -1, lPm25 = -1, lPm10 = -1;
  if (!soft || w.aqi != lAqi || w.pm25 != lPm25 || w.pm10 != lPm10) {
    tft.fillRoundRect(WX_RX, WX_AQ_Y, WX_RW, WX_AQ_H, 9, WX_CARD);
    tft.drawRoundRect(WX_RX, WX_AQ_Y, WX_RW, WX_AQ_H, 9, WX_CEDGE);
    if (w.aqi > 0) {
      uint16_t pfg, pbg; aqiColors(w.aqi, pfg, pbg);
      String s1 = String("空气 ") + aqiLevel(w.aqi);
      char nb[8]; snprintf(nb, sizeof(nb), "%d", w.aqi);
      int pw = 8 + wMeasure(s1, 16) + 4 + wMeasure(nb, 19) + 8;
      tft.fillRoundRect(WX_RX + 9, WX_AQ_Y + 7, pw, 22, 9, pbg);
      int px = WX_RX + 9 + 8;
      px += wText(px, WX_AQ_Y + 10, s1, 16, WX_AQTX, pbg, false) + 4;
      wText(px, WX_AQ_Y + 8, String(nb), 19, pfg, pbg, true);
      char pmb[32]; snprintf(pmb, sizeof(pmb), "PM2.5 %d · PM10 %d", w.pm25, w.pm10);
      wText(WX_RX + 9, WX_AQ_Y + 36, pmb, 16, WX_META, WX_CARD, false);
    } else {
      wText(WX_RX + 9, WX_AQ_Y + 10, "空气 --", 16, WX_META, WX_CARD, false);
      wText(WX_RX + 9, WX_AQ_Y + 36, "空气质量数据不可用", 16, WX_META, WX_CARD, false);
    }
    lAqi = w.aqi; lPm25 = w.pm25; lPm10 = w.pm10;
  }

  // 详情卡 (232,114,236,100): 三行两列 (含室内)
  // 软刷新只原位覆盖变化的风向/数值, 不整卡填充 (fillRoundRect 会让整卡闪一下)
  if (soft) drawDetailValues(w);
  else      drawDetailCard(w);

  // 预警药丸: 按内容签名判断, 未变化不重画
  {
    static char lAlert[64] = "";
    char ak[64]; snprintf(ak, sizeof(ak), "%d|%s|%s", w.alertCount, w.alertTitle, w.alertColor);
    if (!soft || strcmp(ak, lAlert) != 0) {
      if (w.alertCount > 0) {
        uint16_t afg, abg, aedge; alertColors(w.alertColor, afg, abg, aedge);
        tft.fillRoundRect(WX_RX, WX_ALT_Y, WX_RW, WX_ALT_H, 9, abg);
        tft.drawRoundRect(WX_RX, WX_ALT_Y, WX_RW, WX_ALT_H, 9, aedge);
        wWarnIcon(WX_RX + 16, WX_ALT_Y + 14, afg, abg);
        String at = truncW(String(w.alertTitle), 16, WX_RW - 46);
        wText(WX_RX + 30, WX_ALT_Y + 5, at, 16, afg, abg, true);
      } else {
        tft.fillRoundRect(WX_RX, WX_ALT_Y, WX_RW, WX_ALT_H, 9, WX_CARD);
        tft.drawRoundRect(WX_RX, WX_ALT_Y, WX_RW, WX_ALT_H, 9, WX_CEDGE);
        wText(WX_RX + WX_RW / 2 - wMeasure("暂无气象预警", 16) / 2, WX_ALT_Y + 5,
              "暂无气象预警", 16, WX_META, WX_CARD, true);
      }
      snprintf(lAlert, sizeof(lAlert), "%s", ak);
    }
  }

  // 7 天预报入口 (静态内容, 软刷新不重画)
  if (!soft) {
    tft.fillRoundRect(WX_RX, WX_FC_Y, WX_RW, WX_FC_H, 9, WX_BTNRED);
    String fb = "7 天预报";
    int fw = wMeasure(fb, 16);
    int fx = WX_RX + (WX_RW - fw - 12) / 2;
    wText(fx, WX_FC_Y + 5, fb, 16, WX_CREAM, WX_BTNRED, true);
    wChevronRight(fx + fw + 5, WX_FC_Y + 8, 12, WX_CREAM);
  }
}

void drawWeatherScreen() { drawWeatherScreenEx(false); }

// ---- 7 天预报二级页 (全屏 480x320, 覆盖导航栏) ----
static void drawWeatherForecastPage() {
  const WData w = wData();
  tft.fillRect(0, 0, 480, 320, WX_BG);

  // 顶栏: ‹ 返回 | 标题居中
  tft.fillRect(0, 0, 480, WX_TOP_H, WX_TOP);
  tft.fillRoundRect(10, 6, 64, 28, 6, WX_TOPBTN);
  wChevronLeft(20, 14, 12, WX_CREAM);
  wText(34, 12, "返回", 16, WX_CREAM, WX_TOP, false);
  {
    String tt = String("7 天预报 · ") + w.city + w.district;
    int tw = wMeasure(tt, 16);
    wText(240 - tw / 2, 12, tt, 16, WX_CREAM, WX_TOP, false);
  }

  // 7 行, 每行 40px (y=40+i*40), 底部虚线 #2c3d52
  for (int i = 0; i < 7; i++) {
    int y = WX_TOP_H + i * 40;
    // 左: 日期标签 ("今天"金粗, 其它 #cfd8e3) + 日期
    int lx = 12;
    lx += wText(lx, y + 12, String(w.dayLabel[i]), 16, (i == 0) ? WX_DETV : WX_LIST, WX_BG, i == 0);
    wText(lx + 4, y + 12, String(w.dayDate[i]), 16, WX_LIST, WX_BG, false);
    // 中: 图标 28px + 状况
    wIcon(w.dayIcon[i], 118, y + 20, 28, WX_BG);
    wText(140, y + 12, String(w.dayCond[i]), 16, WX_META, WX_BG, false);
    // 右 (右对齐到 468): 温度范围 低~高 16px 金粗
    char hb[16]; snprintf(hb, sizeof(hb), "%d~%d°", w.dayLo[i], w.dayHi[i]);
    int hx = 468 - wMeasure(hb, 16);
    wText(hx, y + 11, hb, 16, WX_DETV, WX_BG, true);
    // 湿度 16px (open-meteo 日均相对湿度, 缺失显示 "--")
    char hub[16];
    if (w.dayHum[i] > 0) snprintf(hub, sizeof(hub), "湿度 %d%%", w.dayHum[i]);
    else                 snprintf(hub, sizeof(hub), "湿度 --");
    int hux = hx - 12 - wMeasure(hub, 16);
    wText(hux, y + 12, hub, 16, WX_META, WX_BG, false);
    // AQI 小药丸 16px (无数据不画)
    if (w.dayAqi[i] > 0) {
      uint16_t pfg, pbg; aqiColors(w.dayAqi[i], pfg, pbg);
      char ab[16]; snprintf(ab, sizeof(ab), "%d %s", w.dayAqi[i], aqiLevel(w.dayAqi[i]));
      int aw = wMeasure(ab, 16) + 12;
      int ax = hux - 12 - aw;
      tft.fillRoundRect(ax, y + 9, aw, 21, 7, pbg);
      wText(ax + 6, y + 12, ab, 16, pfg, pbg, false);
    }
    // 分隔虚线 (最后一行不画)
    if (i < 6) for (int x = 12; x < 468; x += 2) tft.drawPixel(x, y + 39, WX_SEP);
  }
}

// ---- 弹窗通用: 暗罩 + 卡片框, 返回内容区顶部 y ----
static int wModalCard(int cy, int ch, const String& title) {
  tft.fillRect(0, 0, 480, 320, WX_SHADE);    // 暗罩 (覆盖全屏, 不需先重绘底页)
  tft.fillRoundRect(25, cy, 430, ch, 10, WX_CARD);
  tft.drawRoundRect(25, cy, 430, ch, 10, WX_CEDGE);
  // 关闭按钮 右上
  tft.fillRoundRect(385, cy + 7, 56, 26, 6, WX_CARD);
  tft.drawRoundRect(385, cy + 7, 56, 26, 6, WX_CEDGE);
  int cw = wMeasure("关闭", 16);
  wText(385 + (56 - cw) / 2, cy + 11, "关闭", 16, WX_META, WX_CARD, false);
  wText(43, cy + 18, title, 16, WX_DETV, WX_CARD, true);
  return cy + 48;
}

// ---- 台风弹窗 (全部活跃台风一次列出; 内容超出窗口 -> 出滚动条, 可上下拖动) ----
#define TY_CARD_Y   24
#define TY_CARD_MAXH 272                 // 卡片最高 (超出则内容滚动)
#define TY_CARD_MINH 150                 // 卡片最低 (内容很少时也居中成一个像样的卡)
#define TY_LINE_H   22                   // 行高 (16px 字 + 6px 间距)
static int wTyScroll = 0;                // 内容滚动偏移 (px)
static int tyCardY = TY_CARD_Y;          // 本次绘制实际卡片 y/h (随内容自适应)
static int tyCardH = TY_CARD_MAXH;
static int tyViewTop() { return tyCardY + 48; }
static int tyViewH()   { return tyCardH - 80; }

static bool tyHasR64(const WData::TyStorm& s) {
  return s.r64[0] || s.r64[1] || s.r64[2] || s.r64[3];
}
// 内容总高: 每个台风 4 行 (有 12 级风圈 +1), 台风之间 1 空行
static int tyContentH(const WData& w) {
  int lines = 0;
  for (int i = 0; i < w.tyCount; i++) lines += 4 + (tyHasR64(w.ty[i]) ? 1 : 0);
  if (lines <= 0) return 0;
  lines += w.tyCount - 1;
  return (lines - 1) * TY_LINE_H + 16;
}
// 相对定位行 (按当前定位算距离/方位; 无定位坐标则退化为经纬度)
static void tyLocLine(const WData::TyStorm& s, char* out, size_t n) {
  float la = weather::locLat(), lo = weather::locLon();
  if (la || lo) {
    float dLa = (s.lat - la) * 3.14159265f / 180.0f;
    float dLo = (s.lon - lo) * 3.14159265f / 180.0f;
    float a = sinf(dLa / 2) * sinf(dLa / 2) +
              cosf(la * 3.14159265f / 180.0f) * cosf(s.lat * 3.14159265f / 180.0f) *
              sinf(dLo / 2) * sinf(dLo / 2);
    int km = (int)(2 * 6371.0f * atan2f(sqrtf(a), sqrtf(1 - a)) + 0.5f);
    float brg = atan2f(dLo * cosf((la + s.lat) / 2 * 3.14159265f / 180.0f), dLa) * 180.0f / 3.14159265f;
    if (brg < 0) brg += 360.0f;
    static const char* D8[8] = {"北","东北","东","东南","南","西南","西","西北"};
    const char* dir = D8[(int)((brg + 22.5f) / 45.0f) & 7];
    int dd = (km >= 100) ? ((km / 10) * 10) : km;   // 同 HTA: 百公里以上取整到 10
    snprintf(out, n, "位于%s侧 %d km", dir, dd);
  } else {
    snprintf(out, n, "中心 %.1fN %.1fE", (double)s.lat, (double)s.lon);
  }
}
// 滚动条 (内容高于视口才画)
static void drawTyScrollbar(int top, int viewH, int total) {
  int maxs = total - viewH;
  if (maxs <= 0) return;
  tft.fillRoundRect(447, top, 5, viewH, 2, WX_SEP);           // 轨道
  int th = (int)((long)viewH * viewH / total);
  if (th < 18) th = 18;
  int y0 = top + (viewH - th) * wTyScroll / maxs;
  tft.fillRoundRect(447, y0, 5, th, 2, WX_DOT);               // 滑块
}
// 内容区绘制 (可滚动): 只画落在视口内的整行
static void drawTyphoonContent(int top, int viewH, int scroll) {
  const WData w = wData();
  if (w.tyCount <= 0) {                      // 只有预警没有实况: 正文兜底
    drawWrapped(43, top, 394, String(w.typhoonDesc), 16, WX_LIST, WX_CARD, 4, 24);
    return;
  }
  int li = 0;
  char lb[96];
  for (int i = 0; i < w.tyCount; i++) {
    const WData::TyStorm& s = w.ty[i];
    auto emit = [&](const char* txt, uint16_t col) {
      int yy = top + li * TY_LINE_H - scroll;
      if (yy >= top - 6 && yy + 16 <= top + viewH + 6)
        wText(43, yy, String(txt), 16, col, WX_CARD, false);
      li++;
    };
    if (w.tyCount > 1) snprintf(lb, sizeof(lb), "%d. %s（%s）", i + 1, s.name, s.type);
    else               snprintf(lb, sizeof(lb), "%s（%s）", s.name, s.type);
    emit(lb, WX_DETV);
    snprintf(lb, sizeof(lb), "风速 %d m/s  气压 %d hPa", s.wind, s.press);
    emit(lb, WX_LIST);
    tyLocLine(s, lb, sizeof(lb));
    emit(lb, WX_LIST);
    if (s.moveDir[0]) snprintf(lb, sizeof(lb), "向 %s %d km/h", s.moveDir, s.moveSpeed);
    else              snprintf(lb, sizeof(lb), "移速 %d km/h", s.moveSpeed);
    emit(lb, WX_LIST);
    if (tyHasR64(s)) {
      snprintf(lb, sizeof(lb), "12级风圈 东北%d/东南%d/西南%d/西北%d km",
               s.r64[0], s.r64[1], s.r64[2], s.r64[3]);
      emit(lb, WX_LIST);
    }
    if (i + 1 < w.tyCount) li++;            // 台风之间留一空行
  }
}
static void drawTyphoonModal() {
  const WData w = wData();
  String tt = String("台风详情 · ");
  if (w.tyCount > 0) tt += w.ty[0].name;
  else               tt += w.typhoonName;
  if (w.tyCount > 1) { tt += " 等"; tt += w.tyCount; tt += "个"; }
  // 卡片高随内容自适应: 少 (< MINH 也保持观感) / 多 (> MAXH 则滚动)
  int total = tyContentH(w);
  int h = (w.tyCount <= 0) ? 220 : (total + 80);   // 无实况只有预警正文时给足高度
  if (h > TY_CARD_MAXH) h = TY_CARD_MAXH;
  if (h < TY_CARD_MINH) h = TY_CARD_MINH;
  tyCardH = h;
  tyCardY = (320 - h) / 2;
  wModalCard(tyCardY, tyCardH, tt);
  int viewTop = tyViewTop(), viewH = tyViewH();
  int maxs = total - viewH; if (maxs < 0) maxs = 0;
  if (wTyScroll > maxs) wTyScroll = maxs;
  if (wTyScroll < 0) wTyScroll = 0;
  drawTyphoonContent(viewTop, viewH, wTyScroll);
  drawTyScrollbar(viewTop, viewH, total);
  if (w.tyTime[0]) {
    char hint[64];
    if (maxs > 0) snprintf(hint, sizeof(hint), "%s 更新 · 上下拖动查看", w.tyTime);
    else          snprintf(hint, sizeof(hint), "%s 更新 · 点击空白处关闭", w.tyTime);
    wText(43, tyCardY + tyCardH - 22, hint, 16, WX_META, WX_CARD, false);
  }
}

// ---- 预警弹窗 (设计稿: 标题/发布单位+时间/正文/严重程度) ----
static void drawAlertModal() {
  const WData w = wData();
  int y = wModalCard(64, 192, String(w.alertTitle));
  // 发布单位 + 发布时间
  if (w.alertSender[0] || w.alertTime[0]) {
    String sb;
    if (w.alertSender[0]) sb += w.alertSender;
    if (w.alertTime[0])  { if (sb.length()) sb += " "; sb += w.alertTime; }
    sb += " 发布";
    wText(43, y, sb, 16, WX_META, WX_CARD, false);
    y += 22;
  }
  // 正文 (最多 4 行)
  y += drawWrapped(43, y, 394, String(w.alertDesc), 16, WX_LIST, WX_CARD, 4, 22) * 22;
  // 严重程度 · 更新时间
  String tail;
  if (w.alertSeverity[0]) { tail = String("严重程度：") + w.alertSeverity; }
  if (w.alertTime[0]) { if (tail.length()) tail += " · "; tail += String("更新时间 ") + (w.alertTime[5] ? w.alertTime + 6 : w.alertTime); }
  if (tail.length()) wText(43, 64 + 192 - 28, tail, 16, WX_META, WX_CARD, false);
}

// ---- 城市选择弹窗 (设计稿: 8 城点选切换; 加"自动定位"恢复) ----
struct CityOpt { const char* city; const char* dist; float lat, lon; };
static const CityOpt CITY_OPTS[] = {
  {"深圳","南山",22.53f,114.06f}, {"广州","",23.13f,113.26f},
  {"上海","",31.23f,121.47f},    {"北京","",39.90f,116.40f},
  {"杭州","",30.27f,120.15f},    {"成都","",30.57f,104.07f},
  {"武汉","",30.59f,114.30f},    {"南京","",32.06f,118.80f},
};
#define CITY_OPT_N 8
// 按钮几何: 2 行 x 4 列 (w92 h32), 第 3 行"自动定位"
#define CITY_BTN_W   92
#define CITY_BTN_H   32
#define CITY_BTN_X0  43
#define CITY_BTN_Y0  128
#define CITY_BTN_DX  100
#define CITY_BTN_DY  40
#define CITY_AUTO_Y  (CITY_BTN_Y0 + 2 * CITY_BTN_DY)
static void drawCityModal() {
  int y = wModalCard(54, 212, "选择城市");
  wText(43, y, "点选切换并刷新；自动定位后无需手选", 16, WX_META, WX_CARD, false);
  for (int i = 0; i < CITY_OPT_N; i++) {
    int bx = CITY_BTN_X0 + (i % 4) * CITY_BTN_DX;
    int by = CITY_BTN_Y0 + (i / 4) * CITY_BTN_DY;
    tft.fillRoundRect(bx, by, CITY_BTN_W, CITY_BTN_H, 8, WX_CITYBG);
    tft.drawRoundRect(bx, by, CITY_BTN_W, CITY_BTN_H, 8, WX_CEDGE);
    String cn = String(CITY_OPTS[i].city);
    int cw = wMeasure(cn, 16);
    wText(bx + (CITY_BTN_W - cw) / 2, by + 8, cn, 16, WX_LIST, WX_CITYBG, true);
  }
  // 自动定位 (恢复)
  tft.fillRoundRect(CITY_BTN_X0, CITY_AUTO_Y, 120, CITY_BTN_H, 8, WX_CITYBG);
  tft.drawRoundRect(CITY_BTN_X0, CITY_AUTO_Y, 120, CITY_BTN_H, 8, WX_DOT);
  int aw = wMeasure("自动定位", 16);
  wText(CITY_BTN_X0 + (120 - aw) / 2, CITY_AUTO_Y + 8, "自动定位", 16, WX_DOT, WX_CITYBG, true);
}

// ---- 触摸 ----
void handleWeatherTap(uint16_t x, uint16_t y) {
  if (wSub == 4) {
    // 城市弹窗: 点按钮选城, 其它处关闭
    for (int i = 0; i < CITY_OPT_N; i++) {
      int bx = CITY_BTN_X0 + (i % 4) * CITY_BTN_DX;
      int by = CITY_BTN_Y0 + (i / 4) * CITY_BTN_DY;
      if (x >= bx && x <= bx + CITY_BTN_W && y >= by && y <= by + CITY_BTN_H) {
        weather::setManualCity(CITY_OPTS[i].city, CITY_OPTS[i].dist, CITY_OPTS[i].lat, CITY_OPTS[i].lon);
        break;
      }
    }
    if (x >= CITY_BTN_X0 && x <= CITY_BTN_X0 + 120 && y >= CITY_AUTO_Y && y <= CITY_AUTO_Y + CITY_BTN_H)
      weather::setManualCity(nullptr, "", 0, 0);   // 恢复自动定位
    wSub = 0;
    modeDrawn[MODE_WEATHER] = false;
    navDirty = true;
    return;
  }
  if (wSub == 3) {
    // 台风弹窗: 卡片内上下拖动滚动(不关闭); 点关闭键或卡片外才关闭
    bool inCard  = (x >= 25 && x <= 455 && y >= tyCardY && y <= tyCardY + tyCardH);
    bool inClose = (x >= 385 && x <= 441 && y >= tyCardY + 7 && y <= tyCardY + 33);
    if (inCard && !inClose) return;
    wSub = 0;
    modeDrawn[MODE_WEATHER] = false;
    navDirty = true;
    return;
  }
  if (wSub == 2) {
    // 预警弹窗: 点任意处关闭
    wSub = 0;
    modeDrawn[MODE_WEATHER] = false;
    navDirty = true;
    return;
  }
  if (wSub == 1) {
    if (y < WX_TOP_H && x < 84) {
      wSub = 0;
      modeDrawn[MODE_WEATHER] = false;
      navDirty = true;
    }
    return;
  }
  // 底部功能栏区 (全屏主页: 唤出后点按钮切页; 未唤出则忽略)
  if (y >= NAV_BAR_Y) {
    if (wNavPeek) {
      wNavPeek = false;
      int idx = x / NAV_BTN_WIDTH;
      if (idx >= 0 && idx < MODE_COUNT && idx != (int)currentMode) {
        currentMode = (UIMode)idx;
        modeChanged = true;
        navDirty = true;
        for (int i = 0; i < MODE_COUNT; i++) modeDrawn[i] = false;
        prevH = prevM = prevS = -1;
      } else {
        restoreBottom();   // 点当前页/空白: 收起并恢复底部内容
      }
    }
    return;
  }
  if (y < WX_TOP_H) {
    // 城市名 (左区, 点击切城)
    if (x < 200) { wSub = 4; modeDrawn[MODE_WEATHER] = false; return; }
    // 齿轮: 唤出/收起底部功能栏 (5s 无操作自动收起)
    if (x >= WX_BSET_X - 4 && x <= WX_BSET_X + WX_BTN_W + 4) {
      wNavPeek = !wNavPeek;
      if (wNavPeek) { wNavPeekAt = millis(); drawNavBar(); }
      else restoreBottom();
      return;
    }
    // 预警按钮 (有预警时)
    if (x >= WX_BALT_X - 4 && x <= WX_BALT_X + WX_BTN_W + 4) {
      if (wData().alertCount > 0) { wSub = 2; modeDrawn[MODE_WEATHER] = false; }
      return;
    }
    // 台风按钮 (有台风时)
    if (x >= WX_BTY_X - 4 && x <= WX_BTY_X + WX_BTN_W + 4) {
      if (wData().typhoon) { wSub = 3; wTyScroll = 0; modeDrawn[MODE_WEATHER] = false; }
      return;
    }
    // 刷新按钮
    if (x >= WX_BTN_X - 4 && x <= WX_BTN_X + WX_BTN_W + 4) weather::forceRefresh();
    return;
  }
  // 预警药丸 (248,188,200,24)
  if (x >= WX_RX && x <= WX_RX + WX_RW && y >= WX_ALT_Y && y <= WX_ALT_Y + WX_ALT_H) {
    if (wData().alertCount > 0) { wSub = 2; modeDrawn[MODE_WEATHER] = false; }
    return;
  }
  // 7 天预报入口 (248,220,200,34)
  if (x >= WX_RX && x <= WX_RX + WX_RW && y >= WX_FC_Y && y <= WX_FC_Y + WX_FC_H) {
    wSub = 1;
    modeDrawn[MODE_WEATHER] = false;
  }
}

// ---- 台风弹窗滚动接口 (main.cpp 触摸拖动调用) ----
bool weatherTyphoonModalOpen() { return wSub == 3; }
int  weatherTyphoonScrollTop() { return wTyScroll; }
void weatherTyphoonScrollTo(int v) {
  if (wSub != 3) return;
  const WData w = wData();
  int total = tyContentH(w);
  int viewTop = tyViewTop(), viewH = tyViewH();
  int maxs = total - viewH; if (maxs < 0) maxs = 0;
  if (v < 0) v = 0;
  if (v > maxs) v = maxs;
  if (v == wTyScroll) return;
  wTyScroll = v;
  // 局部重绘内容区 (不重画遮罩/卡片框, 拖动时不整屏闪)
  tft.startWrite();
  tft.fillRect(31, viewTop, 416, viewH, WX_CARD);
  drawTyphoonContent(viewTop, viewH, wTyScroll);
  drawTyScrollbar(viewTop, viewH, total);
  tft.endWrite();
}

// 天气主页全屏 + 二级页/弹窗覆盖: 底部区域触摸全部归天气页处理
bool weatherCoverActive() { return currentMode == MODE_WEATHER; }

void modeWeather() {
  // 数据刷新 (联网拉取 / 缓存加载 -> weather::seq 变化) 时重绘。
  // 主页已在屏上时走 soft 刷新: 不整屏填底, 只擦/重画变化的文字块, 避免"闪一下"。
  // GY-39 每 2s 出新值, 详情卡按 2s 节流局部原位刷新 (只覆盖变化的数值, 不整卡填充)。
  static unsigned lastWxSeq = 0xFFFFFFFF;
  static unsigned lastGySeq = 0xFFFFFFFF;
  static uint32_t lastGyDrawMs = 0;
  if (modeChanged) wSub = 0;              // 切进天气页总是回主页
  bool force  = !modeDrawn[MODE_WEATHER] || modeChanged;
  bool wxNew  = weather::seq() != lastWxSeq;
  bool gyNew  = gy39::seq() != lastGySeq && (uint32_t)(millis() - lastGyDrawMs) >= 2000;
  if (force || wxNew) {
    lastWxSeq = weather::seq();
    lastGySeq = gy39::seq();
    lastGyDrawMs = millis();
    tft.startWrite();
    if      (wSub == 1) drawWeatherForecastPage();
    else if (wSub == 2) drawAlertModal();
    else if (wSub == 3) drawTyphoonModal();
    else if (wSub == 4) drawCityModal();
    else                drawWeatherScreenEx(!force);   // 已在屏上则软刷新, 不整屏清底
    if (wNavPeek) drawNavBar();   // 整页重绘会擦掉唤出的功能栏, 补画
    tft.endWrite();
    modeDrawn[MODE_WEATHER] = true;
    wLastMin = -1;
  } else if (gyNew && wSub == 0) {
    // 仅主页局部刷新: 原位覆盖数值 (不擦整块); 数值没变则完全不写屏
    lastGySeq = gy39::seq();
    lastGyDrawMs = millis();
    const WData w = wData();
    if (detailValuesChanged(w)) {
      tft.startWrite();
      drawDetailValues(w);
      tft.endWrite();
    }
  }
  // 功能栏唤出 5s 无操作自动收起, 恢复底部内容
  if (wNavPeek && (uint32_t)(millis() - wNavPeekAt) >= NAV_PEEK_MS) {
    wNavPeek = false;
    if (wSub == 0) restoreBottom();
  }
  // 顶栏时钟分钟级刷新 (仅主页)
  if (wSub == 0) {
    time_t now = time(nullptr);
    struct tm lt; epochToLocalNoLock(now, lt);
    if (lt.tm_year + 1900 >= 2024 && lt.tm_min != wLastMin) {
      wLastMin = lt.tm_min;
      tft.startWrite();
      drawTopTime();
      tft.endWrite();
    }
  }
}
