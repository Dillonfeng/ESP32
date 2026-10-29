// ============================================================
// weather_ui.cpp -- 天气页 (480x320)
// 布局严格对齐设计稿 app_17gm0gncnrd (480x320 交互版):
//   顶栏 40px 红 #b3251a | 左实况区 x24 w208 | 右信息区 x248 w200
// 设备约束: 底部导航栏占 288..320, 主页面内容区压缩到 0..288 (卡片间隙保持 8px);
//   7 天预报二级页按设计稿全屏 480x320 覆盖 (含导航栏), "‹ 返回"回主页。
// ============================================================
#include "ui_common.h"
#include "gy39.h"

// ---- 设计稿配色 (RGB888 -> RGB565) ----
#define WX_BG       0xFFBD   // #fbf6e9 屏幕底色 (米白)
#define WX_RED      0xB123   // #b3251a 顶栏/预报按钮
#define WX_CREAM    0xFF58   // #fbe9c6 红底上的文字
#define WX_TEMP     0xA903   // #a8231a 温度/数值红
#define WX_WTXT     0x5A47   // #5a4a3a 天气/标签文字
#define WX_DIM      0x7B4A   // #7a6a55 次要文字
#define WX_SUN      0xB340   // #b26a00 紫外线/日出日落强调
#define WX_AQEDGE   0xBE0D   // #b9c26f AQI 卡描边
#define WX_AQBG     0xFFDC   // #f9fbe7 AQI 卡底
#define WX_AQTX     0x83A2   // #827717 AQI 药丸文字
#define WX_AQPBG    0xEFBD   // #e8f5e9 AQI 药丸底 (优/良)
#define WX_AQNUM    0x1AE4   // #1b5e20 AQI 数值 (优/良)
#define WX_DEDGE    0xDE53   // #d8c89a 详情卡描边
#define WX_DBG      0xFFDE   // #fffaf0 详情卡底
#define WX_META     0x8BCD   // #8a7a6a 元信息行
#define WX_DOT      0x4E1E   // #4fc3f7 定位点
#define WX_ORANGE   0xE280   // #e65100 轻度污染
#define WX_ORANGEBG 0xFF9C   // #fff3e0 轻度污染底
#define WX_REDBAD   0xB0E3   // #b71c1c 中度及以上
#define WX_REDBADBG 0xFE7A   // #ffcdd2 中度及以上底
#define WX_BTNBG    0xBA47   // 顶栏按钮底 (白 16% on 红)
#define WX_SEP      0xE696   // #e0d2b0 预报行虚线

// ---- 布局常量 (设计稿坐标, 纵向按 280->248 压缩, 横向不变) ----
#define WX_TOP_H     40
#define WX_LX        24      // 左区 x
#define WX_LW        208     // 左区宽
#define WX_RX        248     // 右区内容 x (设计稿 240+8)
#define WX_RW        200     // 右区内容宽
// 左区纵向: 图标 48..144 | 温度 152..204 | 天气 218 | UV 246 | 日出日落 270
#define WX_ICON_Y    48
#define WX_ICON_S    96
#define WX_TEMP_Y    152
#define WX_WTXT_Y    218
#define WX_UV_Y      246
#define WX_SUN_Y     270
// 右区纵向: AQI 48 | 详情 108 | 预警 184 | 预报入口 216 | 元信息 258 (间隙 8)
#define WX_AQ_Y      48
#define WX_AQ_H      52
#define WX_DET_Y     108
#define WX_DET_H     68
#define WX_ALT_Y     184
#define WX_ALT_H     24
#define WX_FC_Y      216
#define WX_FC_H      34
#define WX_META_Y    258

// 顶栏刷新按钮 (392,5,34,30); 时间右对齐到 x=468
#define WX_BTN_X     392
#define WX_BTN_Y     5
#define WX_BTN_W     34
#define WX_BTN_H     30

static uint8_t wSub = 0;          // 0=主页 1=7天预报二级页
static int     wLastMin = -1;     // 顶栏时钟分钟缓存 (分钟变才局部重绘)

// ---- 文字工具: 测量宽度 (与 almScaledText 同规则) ----
static int wMeasure(const String& s, int size) {
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
// 绘制 + 返回宽度; bold=重影仿粗体 (右移 1px 再画一次)
static int wText(int x, int ytop, const String& s, int size, uint16_t fg, uint16_t bg, bool bold) {
  int w = almScaledText(x, ytop, s, size, fg, bg, false);
  if (bold) almScaledText(x + 1, ytop, s, size, fg, bg, false);
  return w;
}

// ---- 图标 (24x24 索引位图缩放到任意尺寸) ----
// 米白底上白云/黄太阳对比度太低 -> 给轮廓加 1px(源) 深褐描边, 缩放后按比例变粗。
#define W_ICON_EDGE 0x3984   // #3b3025 深褐
static void wIcon(int type, int cx, int cy, int s, uint16_t bg) {
  static const uint16_t PAL[8] = {0, 0xFFE0, 0xFFFF, 0xC63C, 0x4DDF, 0x7DFF, 0xFD20, 0xBDF7};
  static uint16_t row[96];
  if (type < 0) type = 0; if (type > 7) type = 7;
  const uint8_t* src = W_ICON[type];
  int ox = cx - s / 2, oy = cy - s / 2;
  for (int dy = 0; dy < s; dy++) {
    int sy = (dy * 24) / s;
    for (int dx = 0; dx < s; dx++) {
      int sx = (dx * 24) / s;
      uint8_t idx = src[sy * 24 + sx];
      if (idx == 0) {
        bool near = (sx > 0  && src[sy * 24 + sx - 1]) ||
                    (sx < 23 && src[sy * 24 + sx + 1]) ||
                    (sy > 0  && src[(sy - 1) * 24 + sx]) ||
                    (sy < 23 && src[(sy + 1) * 24 + sx]);
        row[dx] = near ? W_ICON_EDGE : bg;
      } else {
        row[dx] = PAL[idx];
      }
    }
    tft.pushImage(ox, oy + dy, s, 1, row);
  }
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
static void aqiColors(int a, uint16_t& fg, uint16_t& bg) {
  if (a <= 100)      { fg = WX_AQNUM;  bg = WX_AQPBG; }
  else if (a <= 150) { fg = WX_ORANGE; bg = WX_ORANGEBG; }
  else               { fg = WX_REDBAD; bg = WX_REDBADBG; }
}

// 预警级别字 -> 药丸配色 (设计稿默认橙; 红/黄/蓝按气象预警国标色)
#define WX_ALT_FG   0xDA02   // #d84315 橙
#define WX_ALT_BG   0xFF5C   // #fbe9e7 橙底
static void alertColors(const char* cn, uint16_t& fg, uint16_t& bg) {
  if (!strcmp(cn, "红"))      { fg = 0xB0E3; bg = 0xFE7A; }   // #b71c1c / #ffcdd2
  else if (!strcmp(cn, "黄")) { fg = 0x8B60; bg = 0xFF9C; }   // #8d6e00 / #fff3e0
  else if (!strcmp(cn, "蓝")) { fg = 0x1C5C; bg = 0xE79F; }   // #1e88e5 / #e3f2fd
  else                        { fg = WX_ALT_FG; bg = WX_ALT_BG; }
}

// 按像素宽截断 UTF-8 字符串, 超宽加 ".."
static String truncW(const String& s, int size, int maxW) {
  int w = 0; String out;
  for (unsigned i = 0; i < s.length();) {
    unsigned cl = 1; uint8_t c0 = s[i];
    if (c0 >= 0xF0) cl = 4; else if (c0 >= 0xE0) cl = 3; else if (c0 >= 0xC0) cl = 2;
    int cw = (cl == 1) ? size * 7 / 10 : size;
    if (w + cw > maxW) { out += ".."; break; }
    out += s.substring(i, i + cl); w += cw; i += cl;
  }
  return out;
}

// 弹窗正文: 逐字按像素宽换行绘制, 返回行数
static int drawWrapped(int x, int y, int maxW, const String& s, int size, uint16_t fg, uint16_t bg, int maxLines, int lh) {
  int cx = x, lines = 1;
  for (unsigned i = 0; i < s.length();) {
    unsigned cl = 1; uint8_t c0 = s[i];
    if (c0 >= 0xF0) cl = 4; else if (c0 >= 0xE0) cl = 3; else if (c0 >= 0xC0) cl = 2;
    int cw = (cl == 1) ? size * 7 / 10 : size;
    if (cx + cw > x + maxW) {
      lines++;
      if (lines > maxLines) { almScaledText(cx, y, "..", size, fg, bg, false); return lines; }
      cx = x; y += lh;
    }
    almScaledText(cx, y, s.substring(i, i + cl), size, fg, bg, false);
    cx += cw; i += cl;
  }
  return lines;
}

// ---- 手绘小符号 (emoji 字体不支持, 用几何图形代替) ----
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
// 刷新: 圆圈 + 顶部箭头 (设计稿 🔄)
static void wRefreshIcon(int cx, int cy, uint16_t c) {
  tft.drawCircle(cx, cy, 8, c);
  tft.fillTriangle(cx, cy - 14, cx + 6, cy - 7, cx - 6, cy - 7, c);
}
// 预警: 三角 + "!" (设计稿 ⚠), bg 为药丸底色 (镂空感叹号)
static void wWarnIcon(int cx, int cy, uint16_t c, uint16_t bg) {
  tft.fillTriangle(cx, cy - 7, cx - 8, cy + 6, cx + 8, cy + 6, c);
  tft.fillRect(cx - 1, cy - 3, 3, 6, bg);
  tft.fillRect(cx - 1, cy + 4, 3, 2, bg);
}

// ---- mock 数据 (离线/未联网兜底; 联网后 weather::get() 优先) ----
static const struct {
  const char* city; const char* date; const char* cond; int icon;
  int temp; int feels; int hi; int lo;
  int humidity; int windSpeed; const char* windDir; int windScale;
  int pressure; int uv; float visKm;
  int aqi; int pm25; int pm10;
  const char* sunrise; const char* sunset;
  const char* day[7]; const char* dayDate[7]; const char* dayCond[7];
  int dayIcon[7]; int dayHi[7]; int dayLo[7]; int dayHum[7]; int dayAqi[7];
} WMOCK = {
  "广州", "10-04 周日", "多云", 1,
  25, 27, 28, 22,
  62, 18, "东南风", 3,
  1013, 5, 9.0f,
  42, 12, 28,
  "06:12", "18:34",
  {"今天","明天","周一","周二","周三","周四","周五"},
  {"10/04","10/05","10/06","10/07","10/08","10/09","10/10"},
  {"多云","多云","阴","雨","晴","雨","多云"},
  {0,1,2,4,6,4,1},
  {28,27,25,23,22,24,26},
  {22,21,20,19,18,20,22},
  {70,68,75,85,60,88,72},
  {42,55,70,88,112,95,60},
};

// 取当前天气数据: 联网/有缓存 -> weather::get(); 否则 mock.
static WData wData() {
  const WData& r = weather::get();
  if (r.ok) return r;
  const auto& m = WMOCK;
  WData d;
  d.ok = true;
  snprintf(d.city,    sizeof(d.city),    "%s", m.city);
  snprintf(d.date,    sizeof(d.date),    "%s", m.date);
  snprintf(d.cond,    sizeof(d.cond),    "%s", m.cond);
  snprintf(d.windDir, sizeof(d.windDir), "%s", m.windDir);
  snprintf(d.sunrise, 8, "%s", m.sunrise);
  snprintf(d.sunset,  8, "%s", m.sunset);
  d.icon = m.icon; d.temp = m.temp; d.feels = m.feels; d.hi = m.hi; d.lo = m.lo;
  d.humidity = m.humidity; d.windSpeed = m.windSpeed; d.windScale = m.windScale;
  d.pressure = m.pressure; d.uv = (float)m.uv; d.visKm = m.visKm;
  d.aqi = m.aqi; d.pm25 = m.pm25; d.pm10 = m.pm10;
  for (int i = 0; i < 7; i++) {
    snprintf(d.dayLabel[i], 8, "%s", m.day[i]);
    snprintf(d.dayDate[i],  6, "%s", m.dayDate[i]);
    snprintf(d.dayCond[i], 12, "%s", m.dayCond[i]);
    d.dayIcon[i] = m.dayIcon[i]; d.dayHi[i] = m.dayHi[i]; d.dayLo[i] = m.dayLo[i];
    d.dayHum[i] = m.dayHum[i]; d.dayAqi[i] = m.dayAqi[i];
  }
  return d;
}

// ---- 顶栏时间 (当前时钟, 分钟变局部重绘) ----
static void drawTopTime() {
  char tb[8] = "--:--";
  time_t now = time(nullptr);
  struct tm lt; epochToLocalNoLock(now, lt);   // BUILDV5-TZFIX: 无锁 UTC+8
  if (lt.tm_year + 1900 >= 2024) snprintf(tb, sizeof(tb), "%02d:%02d", lt.tm_hour, lt.tm_min);
  int wid = wMeasure(tb, 12);
  tft.fillRect(430, WX_BTN_Y, 50, WX_BTN_H, WX_RED);   // 清旧时间区 (不碰刷新按钮 392..426)
  almScaledText(468 - wid, 14, String(tb), 12, WX_CREAM, WX_RED, false);
}

// ---- 顶栏: 定位点+城市 (左) | 台风徽标+刷新按钮+时间 (右) ----
#define WX_TY_X      352   // 台风徽标 (352,8,26,24), 有台风时才画
#define WX_TY_Y      8
#define WX_TY_W      26
#define WX_TY_H      24
static void drawWeatherTopBar(const WData& w) {
  tft.fillRect(0, 0, 480, WX_TOP_H, WX_RED);
  tft.fillCircle(28, 20, 4, WX_DOT);
  // 左上角精确到区县: "深圳·龙岗"
  String loc = String(w.city);
  if (w.district[0]) { loc += "·"; loc += w.district; }
  almScaledText(38, 13, loc, 14, WX_CREAM, WX_RED, false);
  // 🌀 台风徽标 (设计稿: 有台风时显示; 点击查看台风弹窗)
  if (w.typhoon) {
    tft.fillRoundRect(WX_TY_X, WX_TY_Y, WX_TY_W, WX_TY_H, 6, WX_DOT);
    almScaledText(WX_TY_X + WX_TY_W / 2, WX_TY_Y + 5, "台", 13, WX_CREAM, WX_DOT, true);
  }
  tft.fillRoundRect(WX_BTN_X, WX_BTN_Y, WX_BTN_W, WX_BTN_H, 6, WX_BTNBG);
  wRefreshIcon(WX_BTN_X + WX_BTN_W / 2, WX_BTN_Y + WX_BTN_H / 2, WX_CREAM);
  drawTopTime();
}

// ---- 主页面 (0..288, 底部导航栏不动) ----
void drawWeatherScreen() {
  const WData w = wData();
  tft.fillRect(0, 0, 480, NAV_BAR_Y, WX_BG);

  drawWeatherTopBar(w);

  // ---- 左实况区 ----
  // 大图标 96px (24,48..144), 中心 (72,96)
  wIcon(w.icon, WX_LX + WX_ICON_S / 2, WX_ICON_Y + WX_ICON_S / 2, WX_ICON_S, WX_BG);

  // 温度行: "28" 52px 红 + "°" 26px + "体感 26°" 14px dim (基线对齐)
  int t = (int)(w.temp + (w.temp >= 0 ? 0.5f : -0.5f));
  char tb[8]; snprintf(tb, sizeof(tb), "%d", t);
  int tx = WX_LX + almBigText(WX_LX, WX_TEMP_Y, String(tb), 52, WX_TEMP, WX_BG, false);
  font16.drawBigChar(tx + 2, WX_TEMP_Y, 0x00B0, 26, WX_TEMP, WX_BG);   // °
  tx += 22;
  {
    String feels = String("体感 ") + (int)(w.feels + (w.feels >= 0 ? 0.5f : -0.5f)) + "°";
    almScaledText(tx + 8, WX_TEMP_Y + 38, feels, 14, WX_DIM, WX_BG, false);
  }

  // 天气文字 17px
  almScaledText(WX_LX, WX_WTXT_Y, String(w.cond), 17, WX_WTXT, WX_BG, false);

  // 紫外线 12px: label dim + 等级·指数 强调
  {
    int ux = WX_LX + wText(WX_LX, WX_UV_Y, "紫外线 ", 12, WX_DIM, WX_BG, false);
    char uvb[24];
    snprintf(uvb, sizeof(uvb), "%s · 指数 %d", uvLevel(w.uv), (int)(w.uv + 0.5f));
    wText(ux, WX_UV_Y, uvb, 12, WX_SUN, WX_BG, true);
  }

  // 日出日落 12px
  {
    int sx = WX_LX + wText(WX_LX, WX_SUN_Y, "日出 ", 12, WX_DIM, WX_BG, false);
    sx += wText(sx, WX_SUN_Y, String(w.sunrise), 12, WX_SUN, WX_BG, true);
    sx += wText(sx, WX_SUN_Y, " · 日落 ", 12, WX_DIM, WX_BG, false);
    wText(sx, WX_SUN_Y, String(w.sunset), 12, WX_SUN, WX_BG, true);
  }

  // ---- 右信息区 ----
  // AQI 卡 (248,48,200,52)
  tft.fillRoundRect(WX_RX, WX_AQ_Y, WX_RW, WX_AQ_H, 9, WX_AQBG);
  tft.drawRoundRect(WX_RX, WX_AQ_Y, WX_RW, WX_AQ_H, 9, WX_AQEDGE);
  if (w.aqi > 0) {
    uint16_t pfg, pbg; aqiColors(w.aqi, pfg, pbg);
    String s1 = String("空气 ") + aqiLevel(w.aqi);
    char nb[8]; snprintf(nb, sizeof(nb), "%d", w.aqi);
    int pw = 8 + wMeasure(s1, 14) + 4 + wMeasure(nb, 17) + 8;
    tft.fillRoundRect(WX_RX + 9, WX_AQ_Y + 7, pw, 21, 9, pbg);
    int px = WX_RX + 9 + 8;
    px += wText(px, WX_AQ_Y + 10, s1, 14, WX_AQTX, pbg, false) + 4;
    wText(px, WX_AQ_Y + 8, String(nb), 17, pfg, pbg, true);
    char pmb[32]; snprintf(pmb, sizeof(pmb), "PM2.5 %d · PM10 %d ug/m3", w.pm25, w.pm10);
    wText(WX_RX + 9, WX_AQ_Y + 35, pmb, 10, WX_DIM, WX_AQBG, false);
  } else {
    wText(WX_RX + 9, WX_AQ_Y + 10, "空气 --", 14, WX_META, WX_AQBG, false);
    wText(WX_RX + 9, WX_AQ_Y + 35, "空气质量数据不可用", 10, WX_DIM, WX_AQBG, false);
  }

  // 详情卡 (248,108,200,68): 湿度/风 + 气压/能见度 两行 13px
  tft.fillRoundRect(WX_RX, WX_DET_Y, WX_RW, WX_DET_H, 9, WX_DBG);
  tft.drawRoundRect(WX_RX, WX_DET_Y, WX_RW, WX_DET_H, 9, WX_DEDGE);
  {
    char vb[16];
    int dx = WX_RX + 9;
    dx += wText(dx, WX_DET_Y + 8, "湿度 ", 13, WX_WTXT, WX_DBG, false);
    snprintf(vb, sizeof(vb), "%d%%", w.humidity);
    dx += wText(dx, WX_DET_Y + 8, vb, 13, WX_TEMP, WX_DBG, true);
    dx += wText(dx, WX_DET_Y + 8, "  ", 13, WX_WTXT, WX_DBG, false);
    dx += wText(dx, WX_DET_Y + 8, String(w.windDir), 13, WX_WTXT, WX_DBG, false);
    snprintf(vb, sizeof(vb), " %d级", w.windScale);
    wText(dx, WX_DET_Y + 8, vb, 13, WX_TEMP, WX_DBG, true);

    dx = WX_RX + 9;
    dx += wText(dx, WX_DET_Y + 40, "气压 ", 13, WX_WTXT, WX_DBG, false);
    // 2026-10-25: GY-39 在线时用本地实测气压, 否则用 API 海平面气压
    const gy39::Data& gs = gy39::get();
    snprintf(vb, sizeof(vb), "%.0f", gs.ok ? gs.pressureHpa : w.pressure);
    dx += wText(dx, WX_DET_Y + 40, vb, 13, WX_TEMP, WX_DBG, true);
    dx += wText(dx, WX_DET_Y + 40, " hPa ", 13, WX_WTXT, WX_DBG, false);
    dx += wText(dx, WX_DET_Y + 40, "能见度 ", 13, WX_WTXT, WX_DBG, false);
    snprintf(vb, sizeof(vb), "%.0f", (double)w.visKm);
    dx += wText(dx, WX_DET_Y + 40, vb, 13, WX_TEMP, WX_DBG, true);
    wText(dx, WX_DET_Y + 40, " km", 13, WX_WTXT, WX_DBG, false);
  }

  // 预警药丸 (248,184,200,24): 有预警按级别色显示 ⚠+标题(截断), 点击查看详情; 无预警灰化占位
  if (w.alertCount > 0) {
    uint16_t afg, abg; alertColors(w.alertColor, afg, abg);
    tft.fillRoundRect(WX_RX, WX_ALT_Y, WX_RW, WX_ALT_H, 9, abg);
    tft.drawRoundRect(WX_RX, WX_ALT_Y, WX_RW, WX_ALT_H, 9, afg);
    wWarnIcon(WX_RX + 16, WX_ALT_Y + 12, afg, abg);
    String at = truncW(String(w.alertTitle), 12, WX_RW - 44);
    wText(WX_RX + 30, WX_ALT_Y + 6, at, 12, afg, abg, true);
  } else {
    tft.fillRoundRect(WX_RX, WX_ALT_Y, WX_RW, WX_ALT_H, 9, WX_DBG);
    tft.drawRoundRect(WX_RX, WX_ALT_Y, WX_RW, WX_ALT_H, 9, WX_DEDGE);
    almScaledText(WX_RX + WX_RW / 2, WX_ALT_Y + 6, "暂无气象预警", 12, WX_META, WX_DBG, true);
  }

  // 7 天预报入口 (248,216,200,34)
  tft.fillRoundRect(WX_RX, WX_FC_Y, WX_RW, WX_FC_H, 9, WX_RED);
  {
    String fb = "7 天预报";
    int fw = wMeasure(fb, 14);
    int fx = WX_RX + (WX_RW - fw - 12) / 2;
    wText(fx, WX_FC_Y + 10, fb, 14, WX_CREAM, WX_RED, true);
    wChevronRight(fx + fw + 6, WX_FC_Y + 11, 12, WX_CREAM);
  }

  // 元信息行 (248,258) 10px
  {
    char mb[48];
    snprintf(mb, sizeof(mb), "IP 定位 · %s 更新 · 和风", w.update);
    wText(WX_RX, WX_META_Y + 4, mb, 10, WX_META, WX_BG, false);
  }
}

// ---- 7 天预报二级页 (全屏 480x320, 覆盖导航栏) ----
static void drawWeatherForecastPage() {
  const WData w = wData();
  tft.fillRect(0, 0, 480, 320, WX_BG);

  // 顶栏: ‹ 返回 | 标题居中
  tft.fillRect(0, 0, 480, WX_TOP_H, WX_RED);
  tft.fillRoundRect(10, 6, 64, 28, 6, WX_BTNBG);
  wChevronLeft(20, 14, 12, WX_CREAM);
  almScaledText(34, 13, "返回", 13, WX_CREAM, WX_RED, false);
  {
    String tt = String("7 天预报 · ") + w.city;
    almScaledText(240, 12, tt, 15, WX_CREAM, WX_RED, true);
  }

  // 7 行, 每行 40px (y=40+i*40), 底部分隔虚线
  for (int i = 0; i < 7; i++) {
    int y = WX_TOP_H + i * 40;
    // 左: 日期标签 ("今天"红粗, 其它深色) + 日期
    int lx = 12;
    {
      String lb = String(w.dayLabel[i]);
      lx += wText(lx, y + 12, lb, 15, (i == 0) ? WX_TEMP : WX_WTXT, WX_BG, i == 0);
      lx += wText(lx, y + 12, " " + String(w.dayDate[i]), 15, WX_WTXT, WX_BG, false);
    }
    // 中: 图标 24px + 状况
    wIcon(w.dayIcon[i], 118, y + 20, 24, WX_BG);
    wText(136, y + 13, String(w.dayCond[i]), 14, WX_DIM, WX_BG, false);
    // 右 (右对齐到 468): 温度范围 16px 红粗
    char hb[16]; snprintf(hb, sizeof(hb), "%d~%d°", w.dayHi[i], w.dayLo[i]);
    int hx = 468 - wMeasure(hb, 16);
    wText(hx, y + 11, hb, 16, WX_TEMP, WX_BG, true);
    // 湿度 13px
    char hub[16]; snprintf(hub, sizeof(hub), "湿度 %d%%", w.dayHum[i]);
    int hux = hx - 12 - wMeasure(hub, 13);
    wText(hux, y + 13, hub, 13, WX_META, WX_BG, false);
    // AQI 小药丸 11px (无数据不画)
    if (w.dayAqi[i] > 0) {
      uint16_t pfg, pbg; aqiColors(w.dayAqi[i], pfg, pbg);
      char ab[16]; snprintf(ab, sizeof(ab), "%d %s", w.dayAqi[i], aqiLevel(w.dayAqi[i]));
      int aw = wMeasure(ab, 11) + 12;
      int ax = hux - 12 - aw;
      tft.fillRoundRect(ax, y + 12, aw, 15, 7, pbg);
      wText(ax + 6, y + 14, ab, 11, pfg, pbg, false);
    }
    // 分隔虚线 (最后一行不画)
    if (i < 6) for (int x = 12; x < 468; x += 2) tft.drawPixel(x, y + 39, WX_SEP);
  }
}

// ---- 详情弹窗 (预警 wSub=2 / 台风 wSub=3): 主页之上压暗罩+卡片, 点任意处关闭 ----
static void drawWeatherModal() {
  const WData w = wData();
  drawWeatherScreen();                       // 底页
  tft.fillRect(0, 0, 480, 320, 0x4208);      // 暗罩 (无 alpha, 深灰近似)
  // 卡片 (50,56,380,208)
  tft.fillRoundRect(50, 56, 380, 208, 12, WX_DBG);
  tft.drawRoundRect(50, 56, 380, 208, 12, WX_RED);
  const char* title = (wSub == 2) ? w.alertTitle : w.typhoonName;
  const char* body  = (wSub == 2) ? w.alertDesc  : w.typhoonDesc;
  wText(70, 76, String(title), 16, WX_RED, WX_DBG, true);
  if (wSub == 2 && w.alertTime[0]) {
    String ts = String(w.alertTime) + " 发布";
    wText(70, 100, ts, 11, WX_META, WX_DBG, false);
  }
  drawWrapped(70, 122, 340, String(body), 13, WX_WTXT, WX_DBG, 5, 20);
  // 关闭按钮 (335,224,75,28)
  tft.fillRoundRect(335, 224, 75, 28, 8, WX_RED);
  almScaledText(335 + 37, 231, "关闭", 13, WX_CREAM, WX_RED, true);
}

// ---- 触摸 (main.cpp handleTouch 路由过来) ----
void handleWeatherTap(uint16_t x, uint16_t y) {
  if (wSub >= 2) {
    // 弹窗: 点任意处关闭 (对齐设计稿"点关闭或空白处关闭")
    wSub = 0;
    modeDrawn[MODE_WEATHER] = false;
    navDirty = true;   // 弹窗覆盖了导航栏, 关闭时重绘
    return;
  }
  if (wSub == 1) {
    // 二级页: 仅"‹ 返回" (10,6,64,28)
    if (y < WX_TOP_H && x < 84) {
      wSub = 0;
      modeDrawn[MODE_WEATHER] = false;
      navDirty = true;   // 二级页覆盖了导航栏, 返回时重绘
    }
    return;
  }
  if (y < WX_TOP_H) {
    // 台风徽标 (352,8,26,24)
    if (x >= WX_TY_X - 4 && x <= WX_TY_X + WX_TY_W + 4) {
      if (wData().typhoon) { wSub = 3; modeDrawn[MODE_WEATHER] = false; }
      return;
    }
    // 刷新按钮 (392,5,34,30) + 少量余量
    if (x >= WX_BTN_X - 4 && x <= WX_BTN_X + WX_BTN_W + 4) weather::forceRefresh();
    return;
  }
  // 预警药丸 (248,184,200,24): 有预警时点击查看详情弹窗
  if (x >= WX_RX && x <= WX_RX + WX_RW && y >= WX_ALT_Y && y <= WX_ALT_Y + WX_ALT_H) {
    if (wData().alertCount > 0) { wSub = 2; modeDrawn[MODE_WEATHER] = false; }
    return;
  }
  // 7 天预报入口 (248,216,200,34)
  if (x >= WX_RX && x <= WX_RX + WX_RW && y >= WX_FC_Y && y <= WX_FC_Y + WX_FC_H) {
    wSub = 1;
    modeDrawn[MODE_WEATHER] = false;
  }
}

// 二级页/弹窗全屏覆盖时, 导航栏区域触摸不归导航 (main.cpp 据此跳过模式切换)
bool weatherCoverActive() { return currentMode == MODE_WEATHER && wSub != 0; }

void modeWeather() {
  // 数据刷新 (联网拉取 / 缓存加载 -> weather::seq 变化) 时重绘, 显示真实天气。
  // GY-39 的本地气压整变也触发重绘, 但节流 5s (避免每 1.5s 整页闪烁)。
  static unsigned lastWxSeq = 0xFFFFFFFF;
  static unsigned lastGySeq = 0xFFFFFFFF;
  static uint32_t lastDrawMs = 0;
  if (modeChanged) wSub = 0;              // 切进天气页总是回主页
  bool force  = !modeDrawn[MODE_WEATHER] || modeChanged;
  bool wxNew  = weather::seq() != lastWxSeq;
  bool gyNew  = gy39::seq() != lastGySeq && (uint32_t)(millis() - lastDrawMs) >= 5000;
  if (force || wxNew || gyNew) {
    lastWxSeq = weather::seq();
    lastGySeq = gy39::seq();
    lastDrawMs = millis();
    tft.startWrite();
    if (wSub >= 2)      drawWeatherModal();
    else if (wSub == 1) drawWeatherForecastPage();
    else                drawWeatherScreen();
    tft.endWrite();
    modeDrawn[MODE_WEATHER] = true;
    wLastMin = -1;   // 整页已含当前时间
  }
  // 顶栏时钟分钟级刷新 (仅主页; 只重绘时间小区域, 不整页)
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
