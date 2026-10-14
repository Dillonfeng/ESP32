#include "ui_common.h"

static void wIcon(int type, int cx, int cy, int s, uint16_t bg) {
  static const uint16_t PAL[8] = {0, 0xFFE0, 0xFFFF, 0xC63C, 0x4DDF, 0x7DFF, 0xFD20, 0xBDF7};
  static uint16_t row[64];
  if (type < 0) type = 0; if (type > 7) type = 7;
  const uint8_t* src = W_ICON[type];
  int ox = cx - s / 2, oy = cy - s / 2;
  for (int dy = 0; dy < s; dy++) {
    int sy = (dy * 24) / s;
    for (int dx = 0; dx < s; dx++) {
      int sx = (dx * 24) / s;
      uint8_t idx = src[sy * 24 + sx];
      row[dx] = (idx == 0) ? bg : PAL[idx];
    }
    tft.pushImage(ox, oy + dy, s, 1, row);
  }
}

// 天气大温度: 两位 62x112 遮罩 (AlmMasks) 并排 = 124x112, bg 底 + fg 笔画.
// 与 almBigNum 同款合成 (mask: 0=笔画,255=底), 但背景色可配 (almBigNum 固定 ALM_PAPER; 天气用黑底).
// lint:skip-rule-1  (静态一次性绘制, 逐行推无闪烁风险)
static void wBigNum(int x0, int y0, int day, uint16_t fg, uint16_t bg) {
  static uint16_t row[ALM_DW * 2];   // 124 px 行缓冲 (内部 RAM)
  const uint8_t* m0 = AlmMasks[day / 10];
  const uint8_t* m1 = AlmMasks[day % 10];
  int fr = fg >> 11, fgc = (fg >> 5) & 0x3F, fb = fg & 0x1F;
  int br = bg >> 11, bgc = (bg >> 5) & 0x3F, bb = bg & 0x1F;
  for (int dy = 0; dy < ALM_DH; dy++) {
    for (int dx = 0; dx < ALM_DW; dx++) {
      uint8_t a = pgm_read_byte(&m0[dy * ALM_DW + dx]);   // 十位
      uint8_t c = pgm_read_byte(&m1[dy * ALM_DW + dx]);   // 个位
      int ra = (br * a + fr * (255 - a)) / 255, ga = ((bgc * a + fgc * (255 - a)) / 255) & 0x3F, ba = (bb * a + fb * (255 - a)) / 255 & 0x1F;
      int rc = (br * c + fr * (255 - c)) / 255, gc = ((bgc * c + fgc * (255 - c)) / 255) & 0x3F, bc = (bb * c + fb * (255 - c)) / 255 & 0x1F;
      row[dx] = (ra << 11) | (ga << 5) | ba;
      row[ALM_DW + dx] = (rc << 11) | (gc << 5) | bc;
    }
    tft.pushImage(x0, y0 + dy, ALM_DW * 2, 1, row);
  }
}

// 天气 mock 数据 (离线/未联网兜底; 联网后 weather::get() 优先). 与 preview WEATHER_MOCK 一致。
static const struct {
  const char* city;
  const char* date;
  const char* cond;
  int icon;
  int temp; int feels; int hi; int lo;
  int humidity; int windSpeed; const char* windDir; int windScale;
  int pressure; int uv;
  const char* sunrise; const char* sunset;
  const char* day[7];
  int dayIcon[7];
  int dayHi[7]; int dayLo[7];
} WMOCK = {
  "广州", "10-04 周日", "多云", 1,
  25, 27, 28, 22,
  62, 18, "东南风", 3,
  1013, 5,
  "06:12", "18:34",
  {"今天","明天","周一","周二","周三","周四","周五"},
  {0,1,2,4,6,4,1},
  {28,27,25,23,22,24,26},
  {22,21,20,19,18,20,22},
};

// 取当前天气数据: 联网/有缓存 -> weather::get(); 否则 mock. (统一成一个 WData 便于绘制)
static WData wData() {
  const WData& r = weather::get();
  if (r.ok) return r;
  const auto& m = WMOCK;  // WMOCK 是匿名 struct 实例, 用 auto 引用
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
  d.pressure = m.pressure; d.uv = (float)m.uv;
  for (int i = 0; i < 7; i++) {
    snprintf(d.dayLabel[i], 8, "%s", m.day[i]);
    d.dayIcon[i] = m.dayIcon[i]; d.dayHi[i] = m.dayHi[i]; d.dayLo[i] = m.dayLo[i];
  }
  return d;
}

// 单卡片: 顶行 label(左) + unit(右) 各 12px; 下方居中 24px 大字 value.
// 顶行与 value 行分离 -> 彻底避免 "1013 hPa" 类重叠 (旧版 unit 压在 value 上).
static void drawWeatherCard(int x, int y, int w, int h,
                            const char* label, const char* value,
                            uint16_t vcol, const char* unit) {
  tft.fillRoundRect(x, y, w, h, 8, W_PANEL);
  tft.drawRoundRect(x, y, w, h, 8, W_EDGE);
  // 顶行: label 左, unit 右 (12px, 同高, 互不重叠)
  almScaledText(x + 8, y + 7, String(label), 12, W_DIM, W_PANEL, false);
  if (unit && unit[0]) {
    int uw = font16.textWidth(unit);
    font16.drawStringFast(x + w - 8 - uw, y + 7, unit, W_DIM, W_PANEL);
  }
  // value 行: 居中 24px (数字/符号, almBigText 走 font32/font16 AA 下采样)
  almBigText(x + w / 2, y + 26, String(value), 24, vcol, W_PANEL, true);
}

void drawWeatherScreen() {
  const WData w = wData();
  tft.fillRect(0, 0, 480, NAV_BAR_Y, W_BG);

  // ---- 头部: 城市 (左) + 日期 + 更新时间 (右) ----
  almBigText(10, 30, String(w.city), 22, W_TEXT, W_BG, false);
  {
    String hdr = String(w.date) + "  " + w.update;
    int hw = font16.textWidth(hdr.c_str());
    font16.drawStringFast(470 - hw, 36, hdr.c_str(), W_DIM, W_BG);
  }

  // ---- 主区左: 大温度数字 (62x112, 白笔画 on 黑底) + 度号 ----
  int t = (int)(w.temp + 0.5); if (t < 0) t = 0; if (t > 99) t = 99;
  wBigNum(20, 52, t, W_TEXT, W_BG);              // 恒 2 位 (124px)
  font16.drawBigChar(20 + ALM_DW * 2 + 8, 60, 0x00B0, 22, W_TEXT, W_BG);   // ° 紧跟数字右缘

  // ---- 主区右: 大图标 (72px, 右上) ----
  wIcon(w.icon, 402, 108, 72, W_BG);

  // ---- 主区中: 状况 + 体感/最高最低/湿度/风力 (16px AA, 左锚点 x=165) ----
  font16.drawStringFast(165, 62,  w.cond,              W_TEXT,   W_BG);
  {
    String feels = String("体感 ") + (int)(w.feels + 0.5) + "°";
    font16.drawStringFast(165, 86,  feels.c_str(),     W_DIM,    W_BG);
  }
  {
    String range = String("最高 ") + (int)(w.hi + 0.5) + "°  最低 " + (int)(w.lo + 0.5) + "°";
    font16.drawStringFast(165, 110, range.c_str(),     W_TEXT,   W_BG);
  }
  {
    String hum = String("湿度 ") + w.humidity + "%";
    font16.drawStringFast(165, 134, hum.c_str(),       W_ACCENT, W_BG);
  }
  {
    String wind = String(w.windDir) + " " + w.windScale + "级";
    font16.drawStringFast(165, 158, wind.c_str(),      W_TEXT,   W_BG);
  }

  // ---- 卡片区: 4 张 (x=10,114,218,322, 宽 96, 高 50, y=178) ----
  {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", w.pressure);
    drawWeatherCard(10,  178, 96, 50, "气压", buf, W_TEXT, "hPa");
  }
  {
    char buf[6];
    snprintf(buf, sizeof(buf), "%.0f", w.uv);
    drawWeatherCard(114, 178, 96, 50, "紫外线", buf, W_SUN, "UV");
  }
  drawWeatherCard(218, 178, 96, 50, "日出", w.sunrise, W_TEXT, "");
  drawWeatherCard(322, 178, 96, 50, "日落", w.sunset,  W_TEXT, "");

  // ---- 7 天预报条 (y=232..287, 三行垂直居中, 均在导航栏 288 之上) ----
  tft.drawFastHLine(8, 232, 464, W_EDGE);
  for (int i = 0; i < 7; i++) {
    int cx = 40 + i * 68;
    // 行1: 星期标签 (今天=青, 其它=白; 14px, 顶 y234 -> 234..248)
    uint16_t dc = (i == 0) ? W_ACCENT : W_TEXT;
    almScaledText(cx, 234, String(w.dayLabel[i]), 14, dc, W_BG, true);
    // 行2: 图标 (20px, 中心 y262 -> 252..272)
    wIcon(w.dayIcon[i], cx, 262, 20, W_BG);
    // 行3: 高/低温 (13px, 顶 y274 -> 274..287, 居中)
    char hb[8], lb[8];
    snprintf(hb, sizeof(hb), "%d°", w.dayHi[i]);
    snprintf(lb, sizeof(lb), "%d°", w.dayLo[i]);
    int hw2 = font16.textWidth(hb), lw2 = font16.textWidth(lb);
    int x0 = cx - (hw2 + 4 + lw2) / 2;
    font16.drawStringFast(x0,         274, hb, W_TEXT, W_BG);
    font16.drawStringFast(x0 + hw2 + 4, 274, lb, W_DIM,  W_BG);
  }
}

void modeWeather() {
  // 数据刷新 (联网拉取 / 缓存加载 -> weather::seq 变化) 时重绘, 显示真实天气。
  static unsigned lastWxSeq = 0xFFFFFFFF;
  if (!modeDrawn[MODE_WEATHER] || modeChanged || weather::seq() != lastWxSeq) {
    lastWxSeq = weather::seq();
    tft.startWrite();
    drawWeatherScreen();
    tft.endWrite();
    modeDrawn[MODE_WEATHER] = true;
  }
}


// BUILD14: 收音机泵 + 看门狗 (全屏分支/列表分支 两处共用)
// 看门狗: 流中途死亡 (库内部断连后 isRunning 掉) 连续 8s 未 running -> 自动 stopRadio,
// 覆盖 "播一会儿自己断"。正常连流/缓冲期 isRunning 短暂为 false (连接超时上限 2s),
// 8s 宽限足够不误伤。
