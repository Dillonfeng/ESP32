#include "ui_common.h"

// ---------- 万年历页�?(布局�?preview draw_calendar_scene 完全�?�? ----------
#define CAL_X0       16
#define CAL_CELL_W   64
#define CAL_CELL_H   30
#define CAL_X1       (CAL_X0 + 7 * CAL_CELL_W)   // 464
#define CAL_GRID_TOP 54
#define CAL_WKND     0xF800   // 周末 - �?
static const char* weekHdr[] = {"日", "一", "二", "三", "四", "五", "六"};  // 0=周日
static int calYear, calMon;   // 当前显示月份 (初�?��??首�?�进入�??= 今天)
static int calSelY, calSelM, calSelD;
static int calPrevSelY = 0, calPrevSelM = 0, calPrevSelD = 0;   // prev selected day (partial refresh)   // 选中的日�?(默�??=今天); 底部信息条显示它的农�?
static int calFirstW = 0, calDim = 30;
static bool calAlmOpen = false;
static int calLastTapDay = 0, calLastTapY = 0, calLastTapM = 0;  // 上一次点某天的日/年/月 (双击判定)
static unsigned long calLastTapMs = 0;                             // 上一次点某天的时刻 (400ms 内再点同一天=双击)       // 黄历弹层是否打开 (点某天 -> 选中日黄历)  // 上�?�绘制�?? 1号星�?0=�? / 当月天数
static bool calPickerOpen = false;      // 年月选择弹层�?否打�?
static int calPickY = 2000, calPickM = 1; // 选择器临时�??
// 年月选择弹层几何 (�?make_preview.py PICK_* �?�?
#define PICK_PANEL_X  20
#define PICK_PANEL_W  440
#define PICK_PT       40
#define PICK_PB       250
#define PICK_MCOL_W   60
#define PICK_MROW_H   40
#define PICK_MGRID_X  60          // (480 - 6*60)/2
#define PICK_MTOP     128


// ---------- 黄历 (米纸 + 红/褐 新设计) 弹层: 双击某天打开 (镜像 preview make_preview.py draw_almanac_popup) ----------
// 配色 (565 值, 与 preview ALM_* 一致):
#define ALM_PAPER     0xEF9C  // 纸面 (暖白米纸)
#define ALM_RED       0xB9C5  // 主红 (大数字 / 月份 / 忌 / 节日框)
#define ALM_DARK      0x3985  // 深褐 (标题 / 干支 / 星期)
#define ALM_LINE      0xCCB0  // 细线 (四角括号 / 分栏线 / 星期框 / 图例)
#define ALM_JI        0x6D0C  // 吉 (绿, "宜"标签底)
#define ALM_RING      0xE5F5  // 大数字装饰双环 (淡粉)
#define ALM_WKND      0x8A52  // 周末图例点 (褐红, 区别于 平日深褐 / 节假日主红)
// 宜/忌 项池 (12, 2字/项); 装饰性 dayStd%12 顺序取 3 项 (两侧同公式).
static const char* ALM_YI_ITEMS[12] = {"嫁娶","出行","移徙","入宅","装修","动土","栽种","祭祀","安床","会友","嫁娶","出行"};
static const char* ALM_JI_ITEMS[12] = {"开张","开业","嫁娶","动土","安葬","出行","安床","入宅","词讼","破土","开光","会友"};
static int almJDN(int y, int m, int d) {
  int a = (14 - m) / 12;
  int yy = y + 4800 - a;
  int mm = m + 12 * a - 3;
  return d + (153 * mm + 2) / 5 + 365 * yy + yy / 4 - yy / 100 + yy / 400 - 32045;
}

static void drawAlmPopup(int y, int m, int d);   // 前置声明 (drawCalendar 先调用, 定义在后)
// ---- calendar bottom info strip (shared by full-page draw + partial refresh) ----
static void drawCalBottomStrip(int ty, int tmn, int td) {
  int ld[31]; nl_month_days(calSelY, calSelM, ld);
  int v = ld[calSelD - 1];
  int lm = v / 100, ldn = v % 100;
  bool isLeap = (lm > 100); if (isLeap) lm -= 100;
  int ly = nl_zodiac_year(calSelY, calSelM, calSelD);   // 干支/生肖�?(按春节分�? �?正农�?11/12月�??�?
  int stripTop = CAL_GRID_TOP + 6 * CAL_CELL_H + 8;
  tft.fillRoundRect(CAL_X0, stripTop, CAL_X1 - CAL_X0, 34, 8, CAL_STRIP_L);
  int cys = stripTop + 17;
  char gz[16];
  snprintf(gz, sizeof(gz), "%s%s%s年", nl_tg_text[nl_tg(ly)], nl_dz_text[nl_dz(ly)], nl_sx_text[nl_dz(ly)]);
  int x = CAL_X0 + 12;
  font16.drawStringFast(x, cys - 8, gz, CAL_GOLD_L, CAL_STRIP_L);
  x += font16.textWidth(gz) + 12;
  // 选中�?= 今天 -> "今日农历X月X"; 否则 "M月D�?农历X月X"
  char lmS[40];
  if (calSelY == ty && calSelM == tmn && calSelD == td) {
    snprintf(lmS, sizeof(lmS), "今日农历%s月%s%s", lunarMonths[lm - 1], lunarDays[ldn - 1],
             isLeap ? "(闰)" : "");
  } else {
    snprintf(lmS, sizeof(lmS), "%d月%d日 农历%s月%s%s", calSelM, calSelD, lunarMonths[lm - 1],
             lunarDays[ldn - 1], isLeap ? "(闰)" : "");
  }
  font16.drawStringFast(x, cys - 8, lmS, CAL_NORM, CAL_STRIP_L);
  // "今日" 按钮 (信息条右�?: �?�?回到今�??(当前�?+ 选中今天)�?
  // 已在今天�?在月时高�?�?, 点击 = 把�?�中日重�?为今天�??
  {
    const char* btTxt = "今日";
    int btW = font16.textWidth(btTxt) + 16;
    int btX = CAL_X1 - 8 - btW;
    int btY = stripTop + 5, btH = 24;
    bool atToday = (calYear == ty && calMon == tmn);
    tft.fillRoundRect(btX, btY, btW, btH, 6, atToday ? TFT_CYAN : CAL_STRIP_L);
    font16.drawStringFast(btX + 8, cys - 8, btTxt, atToday ? TFT_BLACK : CAL_TITLE,
                          atToday ? TFT_CYAN : CAL_STRIP_L);
  }
}
// selection changed: erase old border + draw new + redraw bottom strip (no full-page redraw)
static void redrawCalSelection(int ty, int tmn, int td) {
  bool prevIsToday = (calPrevSelY == ty && calPrevSelM == tmn && calPrevSelD == td);
  if (calPrevSelY == calYear && calPrevSelM == calMon && calPrevSelD >= 1 && calPrevSelD <= calDim && !prevIsToday) {
    int r = (calFirstW + calPrevSelD - 1) / 7;
    if (r < 6) {
      int c = (calFirstW + calPrevSelD - 1) % 7;
      int cx = CAL_X0 + c * CAL_CELL_W + CAL_CELL_W / 2;
      int cy = CAL_GRID_TOP + r * CAL_CELL_H + CAL_CELL_H / 2;
      tft.drawRect(cx - 14, cy - 13, 29, 27, CAL_LIGHT_BG);   // erase old selection border
    }
  }
  bool newIsToday = (calSelY == ty && calSelM == tmn && calSelD == td);
  if (calSelY == calYear && calSelM == calMon && calSelD >= 1 && calSelD <= calDim && !newIsToday) {
    int r = (calFirstW + calSelD - 1) / 7;
    if (r < 6) {
      int c = (calFirstW + calSelD - 1) % 7;
      int cx = CAL_X0 + c * CAL_CELL_W + CAL_CELL_W / 2;
      int cy = CAL_GRID_TOP + r * CAL_CELL_H + CAL_CELL_H / 2;
      tft.drawRect(cx - 14, cy - 13, 29, 27, CAL_TITLE);      // draw new selection border
    }
  }
  calPrevSelY = calSelY; calPrevSelM = calSelM; calPrevSelD = calSelD;
  drawCalBottomStrip(ty, tmn, td);
}
void drawCalendar() {
  if (!font16.isReady()) return;
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  int ty = t.tm_year + 1900, tmn = t.tm_mon + 1, td = t.tm_mday;
  if (calYear <= 0) { calYear = ty; calMon = tmn; }
  if (calSelY <= 0) { calSelY = ty; calSelM = tmn; calSelD = td; }   // 默�?��?�中今天
  holiday::ensureYear(calYear);   // 浏�?�到某年且无缓�??-> 排队网络同�??(在线时立即做)
  tft.fillRect(0, 0, 480, NAV_BAR_Y, CAL_LIGHT_BG);
  if (calPickerOpen) { drawCalPicker(); return; }
  if (calAlmOpen) { drawAlmPopup(calSelY, calSelM, calSelD); return; }
  // 顶部: 上月 | 月份标�??(点击选年�? | 下月
  font16.drawStringFast(14, 16, "<", CAL_HDR, CAL_LIGHT_BG);
  char mt[16];
  snprintf(mt, sizeof(mt), "%d年%d月", calYear, calMon);
  int mw = font16.textWidth(mt);
  font16.drawStringFast(240 - mw / 2, 16, mt, CAL_TITLE, CAL_LIGHT_BG);
  font16.drawStringFast(446, 16, ">", CAL_HDR, CAL_LIGHT_BG);
  // 星期表头
  for (int i = 0; i < 7; i++) {
    int cx = CAL_X0 + i * CAL_CELL_W + CAL_CELL_W / 2;
    font16.drawStringFast(cx - 8, 34, weekHdr[i], CAL_HDR, CAL_LIGHT_BG);
  }
  // table 网格�?(7 �?× 6 �?
  for (int i = 0; i <= 7; i++) {
    int gx = CAL_X0 + i * CAL_CELL_W;
    tft.drawFastVLine(gx, CAL_GRID_TOP, 6 * CAL_CELL_H + 1, CAL_GRID);
  }
  for (int r = 0; r <= 6; r++) {
    int gy = CAL_GRID_TOP + r * CAL_CELL_H;
    tft.drawFastHLine(CAL_X0, gy, (CAL_X1 - CAL_X0) + 1, CAL_GRID);
  }
  // 日期网格 (周日列起)
  static const int mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int dim = mdays[calMon - 1];
  bool leap = (calYear % 4 == 0 && calYear % 100 != 0) || (calYear % 400 == 0);
  if (calMon == 2 && leap) dim = 29;
  calDim = dim;
  // 该月 1 号的星期 (0=周日), �?mktime 计算, 跨月/跨年都�?��??
  int firstW = 0;
  {
    struct tm t1;
    memset(&t1, 0, sizeof(t1));
    t1.tm_year = calYear - 1900;
    t1.tm_mon = calMon - 1;
    t1.tm_mday = 1;
    t1.tm_isdst = -1;
    time_t tt1 = mktime(&t1);
    struct tm t1l;
    localtime_r(&tt1, &t1l);
    firstW = t1l.tm_wday;   // 0=Sun
  }
  calFirstW = firstW;
  char dn[3];
  for (int day = 1; day <= dim; day++) {
    int r = (firstW + day - 1) / 7;
    if (r >= 6) break;
    int c = (firstW + day - 1) % 7;
    int cx = CAL_X0 + c * CAL_CELL_W + CAL_CELL_W / 2;
    int cy = CAL_GRID_TOP + r * CAL_CELL_H + CAL_CELL_H / 2;
    snprintf(dn, sizeof(dn), "%d", day);
    int tw = font16.textWidth(dn);
    bool isToday = (ty == calYear && tmn == calMon && td == day);
    if (isToday) {                                            // 今天: 青色圆�?�高�?
      tft.fillRoundRect(cx - 13, cy - 12, 26, 24, 6, TFT_CYAN);
      font16.drawStringFast(cx - tw / 2, cy - 8, dn, TFT_BLACK, TFT_CYAN);
      { int htd = holiday::dayType(calYear, calMon, day);   // 2026-09-28: today-that-is-a-holiday still shows the badge (isToday branch used to skip it -> 9/27 had no xiu)
        if (htd == HOL_OFF || htd == HOL_WORK)
          font16.drawScaledChar(cx + 19, cy - 14, htd == HOL_OFF ? 0x4F11 : 0x73ED, 12,
                                (htd == HOL_OFF) ? CAL_OFF : CAL_WORK_L, TFT_CYAN); }
    } else {
      int ht = holiday::dayType(calYear, calMon, day);
      uint16_t fc = (ht == HOL_OFF) ? CAL_OFF
                     : (ht == HOL_WORK) ? CAL_WORK_L
                     : ((c == 0 || c == 6) ? CAL_WKND : CAL_NORM);
      font16.drawStringFast(cx - tw / 2, cy - 8, dn, fc, CAL_LIGHT_BG);
      if (ht == HOL_OFF || ht == HOL_WORK) {                 // 右上角小 "�?/"�? 角标 (缩放 12px, 替代下划�?
        // 0x4F11=�? 0x73ED=�?(�?Unicode 码点绘制, 避免多字�?char 字面量�?��??
        font16.drawScaledChar(cx + 19, cy - 14, ht == HOL_OFF ? 0x4F11 : 0x73ED, 12, fc, CAL_LIGHT_BG);
      }
    }
    if (calSelY == calYear && calSelM == calMon && calSelD == day && !isToday) {  // 选中�? 蓝色边�??
      tft.drawRect(cx - 14, cy - 13, 29, 27, CAL_TITLE);
    }
  }
  // 底部信息�? 干支�?+ 选中日的农历 (点击某天即显示那�? 默�??=今天)
  drawCalBottomStrip(ty, tmn, td);
  calPrevSelY = calSelY; calPrevSelM = calSelM; calPrevSelD = calSelD;
}

// 年月选择弹层: 顶部 < 年份 > 步进 + 6x2 月份网格 + �?�?/取消
void drawCalPicker() {
  tft.fillRect(0, 0, 480, NAV_BAR_Y, CAL_LIGHT_BG);
  tft.fillRoundRect(PICK_PANEL_X, PICK_PT, PICK_PANEL_W, PICK_PB - PICK_PT, 12, CAL_LIGHT_BG);
  tft.drawRoundRect(PICK_PANEL_X, PICK_PT, PICK_PANEL_W, PICK_PB - PICK_PT, 12, CAL_GRID);
  font16.drawStringFast(240 - font16.textWidth("选择年月") / 2, PICK_PT + 12, "选择年月", CAL_HDR, CAL_LIGHT_BG);
  // 年份步进: < 2026 >
  font16.drawStringFast(40, 92, "<", CAL_HDR, CAL_LIGHT_BG);
  char ys[8];
  snprintf(ys, sizeof(ys), "%d", calPickY);
  font16.drawStringFast(240 - font16.textWidth(ys) / 2, 88, ys, CAL_TITLE, CAL_LIGHT_BG);
  font16.drawStringFast(420, 92, ">", CAL_HDR, CAL_LIGHT_BG);
  // 月份网格 6 �?x 2 �?
  char mlab[8];
  for (int m = 1; m <= 12; m++) {
    int col = (m - 1) % 6;
    int row = (m - 1) / 6;
    int bx = PICK_MGRID_X + col * PICK_MCOL_W;
    int by = PICK_MTOP + row * PICK_MROW_H;
    snprintf(mlab, sizeof(mlab), "%d月", m);
    int lw = font16.textWidth(mlab);
    if (m == calPickM) {
      tft.fillRoundRect(bx + 3, by + 3, PICK_MCOL_W - 6, PICK_MROW_H - 6, 8, TFT_CYAN);
      font16.drawStringFast(bx + PICK_MCOL_W / 2 - lw / 2, by + PICK_MROW_H / 2 - 8,
                            mlab, CAL_TITLE, TFT_CYAN);
    } else {
      tft.fillRoundRect(bx + 3, by + 3, PICK_MCOL_W - 6, PICK_MROW_H - 6, 8, CAL_STRIP_L);
      font16.drawStringFast(bx + PICK_MCOL_W / 2 - lw / 2, by + PICK_MROW_H / 2 - 8,
                            mlab, CAL_NORM, CAL_STRIP_L);
    }
  }
  // 按钮: 取消 | �?�?  (文字垂直居中�?�?�? 按钮: 按钮 y=PICK_PB-34..PICK_PB-8, �?�?=PICK_PB-21;
  //  �?�? AA 16px 字形按左上锚�? �?y = �?�?-8 = PICK_PB-29)
  font16.drawStringFast(150 - font16.textWidth("取消") / 2, PICK_PB - 29, "取消", CAL_HDR, CAL_LIGHT_BG);
  tft.fillRoundRect(280, PICK_PB - 34, 120, 26, 8, CAL_TITLE);
  font16.drawStringFast(340 - font16.textWidth("确定") / 2, PICK_PB - 29, "确定", CAL_LIGHT_BG, CAL_TITLE);
}

// 放大中文/数字文字 (逐字 drawBigChar): 镜像 preview big_text. ASCII advance = dst*7/10,

// 其它 = dst. (x,y) = 左上角; center=true 时以 x 为水平中心. 返回总宽(像素).

int almBigText(int x, int y, const String& s, int dst, uint16_t fg, uint16_t bg, bool center) {

  int total = 0;

  for (unsigned i = 0; i < s.length(); i++) {

    uint8_t c0 = s[i];

    uint16_t cp;

    int adv;

    if (c0 < 0x80) { cp = c0; adv = dst * 7 / 10; }

    else if (c0 < 0xE0) { cp = ((c0 & 0x1F) << 6) | (s[i + 1] & 0x3F); i++; adv = dst; }

    else if (c0 < 0xF0) { cp = ((c0 & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); i += 2; adv = dst; }

    else { i += 3; continue; }

    total += adv;

  }

  if (center) x -= total / 2;

  int sx = x;

  for (unsigned i = 0; i < s.length(); i++) {

    uint8_t c0 = s[i];

    uint16_t cp;

    int adv;

    if (c0 < 0x80) { cp = c0; adv = dst * 7 / 10; }

    else if (c0 < 0xE0) { cp = ((c0 & 0x1F) << 6) | (s[i + 1] & 0x3F); i++; adv = dst; }

    else if (c0 < 0xF0) { cp = ((c0 & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); i += 2; adv = dst; }

    else { i += 3; continue; }

    if (font32.isReady() && font32.hasGlyph(cp)) font32.drawBigChar(sx, y, cp, dst, fg, bg);
    else font16.drawBigChar(sx, y, cp, dst, fg, bg);   // 缺字回退 16px

    sx += adv;

  }

  return total;

}



// 小字 (逐字 drawScaledChar, 16px AA 面积平均缩放到 size): 镜像 preview hw_small.
// (x,ytop) = 左上角; ASCII advance = size*7/10, 其它 = size; center=true 时以 x 为中心. 返回总宽.
int almScaledText(int x, int ytop, const String& s, int size, uint16_t fg, uint16_t bg, bool center) {
  int total = 0;
  for (unsigned i = 0; i < s.length(); i++) {
    uint8_t c0 = s[i];
    if (c0 < 0x80) { total += size * 7 / 10; }
    else if (c0 < 0xE0) { i++; total += size; }
    else if (c0 < 0xF0) { i += 2; total += size; }
    else { i += 3; }
  }
  if (center) x -= total / 2;
  int sx = x;
  for (unsigned i = 0; i < s.length(); i++) {
    uint8_t c0 = s[i]; uint16_t cp; int adv;
    if (c0 < 0x80) { cp = c0; adv = size * 7 / 10; }
    else if (c0 < 0xE0) { cp = ((c0 & 0x1F) << 6) | (s[i + 1] & 0x3F); i++; adv = size; }
    else if (c0 < 0xF0) { cp = ((c0 & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); i += 2; adv = size; }
    else { i += 3; continue; }
    // BUILD21: 小字也走 64px 字库(font32)下采样(同 almBigText 路径), 而不是 16px 源。
    // 根因: 16px 源 area-average 降到 13px 会把只占源行14-15 的 2px 笔画(如"三"底横)塌成 1px ->
    //       视觉上"底横被截一半"。64px 源有 4x 分辨率, 降采样后笔画保持 2px, 与 24px 大字一样干净。
    // 缺字回退 font16(16px 源), 与 almBigText 的回退策略一致。
    if (font32.isReady() && font32.hasGlyph(cp)) font32.drawBigChar(sx, ytop, cp, size, fg, bg);
    else font16.drawBigChar(sx, ytop, cp, size, fg, bg);
    sx += adv;
  }
  return total;
}

// 粗体大数字: 两位 62x112 8bit 遮罩 (almnum.h, Bahnschrift w700) 并排 = 124x112, 纸底 + 红笔画.

// 逐行流式合成 (每行 124px 临时行缓冲 248B, 不占大块静态 RAM), 行内单次 pushImage.

// day<10 时十位留纸底 (遮罩值 255). 整体 124 宽由调用者定位.

// lint:skip-rule-1  (弹层为一次性静态绘制, 无连续动态区域, 逐行推无闪烁风险)

static void almBigNum(int x0, int y0, int day, uint16_t col) {

  static uint16_t row[ALM_DW * 2];   // 124 px 行缓冲 (内部 RAM)

  const uint8_t* m0 = AlmMasks[day / 10];

  const uint8_t* m1 = AlmMasks[day % 10];

  int pr = ALM_PAPER >> 11, pg = (ALM_PAPER >> 5) & 0x3F, pb = ALM_PAPER & 0x1F;

  int fr = col >> 11, fg = (col >> 5) & 0x3F, fb = col & 0x1F;   // 筒单颜色随日期类型 (red/purple/dark)

  for (int dy = 0; dy < ALM_DH; dy++) {

    for (int dx = 0; dx < ALM_DW; dx++) {

      uint8_t a = pgm_read_byte(&m0[dy * ALM_DW + dx]);   // 十位; 0=笔画,255=底 (前导零: day<10 显 "0")

      uint8_t c = pgm_read_byte(&m1[dy * ALM_DW + dx]);

      int ra = (pr * a + fr * (255 - a)) / 255, ga = ((pg * a + fg * (255 - a)) / 255) & 0x3F, ba = (pb * a + fb * (255 - a)) / 255 & 0x1F;

      int rc = (pr * c + fr * (255 - c)) / 255, gc = ((pg * c + fg * (255 - c)) / 255) & 0x3F, bc = (pb * c + fb * (255 - c)) / 255 & 0x1F;

      row[dx] = (ra << 11) | (ga << 5) | ba;

      row[ALM_DW + dx] = (rc << 11) | (gc << 5) | bc;

    }

    tft.pushImage(x0, y0 + dy, ALM_DW * 2, 1, row);

  }

}



// 黄历 (米纸+红/褐 新设计) 弹层: 覆盖日历页, 点任意处关闭 (无关闭按钮).

// 逐像素镜像 preview make_preview.py draw_almanac_popup.

// 布局 (面板 PX=24,PT=16,432x272; 四角花纹括号):

//   顶行        y30 (年份 big 32 | 干支X年 | 月份 big 26 红 + 月 | 节气 | 右上 节日红框)

//   三列中部    (左 农历X月X big24 红 + 农历·花名月 / 中央 双环 + 大数字 day 红 / 右 干支X日 big24 + 干支·生肖)

//   底栏        分隔线 y230 + 星期框(左) + 宜[吉]3项/忌[凶]3项(中) + 平日/周末/节假日 图例(右)

// lint:skip-rule-1  (弹层为一次性静态绘制: 双击打开后无连续动态区域, 逐笔/逐行推无闪烁风险)
// ---- almanac festival box: whole contiguous holiday run shares one festival name ----
// (Spring Festival run 2/15-23 all show "chunjie"; National Day 10/1-7 all show "guoqingjie".)
static void almAddDay(int* y, int* m, int* d, int delta) {
  struct tm t; memset(&t, 0, sizeof(t));
  t.tm_year = *y - 1900; t.tm_mon = *m - 1; t.tm_mday = *d; t.tm_isdst = -1;
  time_t tt = mktime(&t) + (long)delta * 86400L;
  struct tm r; localtime_r(&tt, &r);
  *y = r.tm_year + 1900; *m = r.tm_mon + 1; *d = r.tm_mday;
}

static bool almRealFest(int y, int m, int d, char* f, int fsz) {
  int ld[31]; nl_month_days(y, m, ld);
  int v = ld[d - 1]; int lm = v / 100, ldn = v % 100;
  bool isLeap = (lm > 100); if (isLeap) lm -= 100;
  f[0] = 0;
  return nl_festival(y, m, d, nl_zodiac_year(y, m, d), lm, ldn, isLeap ? 1 : 0, f, fsz) == 1;
}

static const char* almHolidayFest(int y, int m, int d, char* buf, int bufsz) {
  char f[16];
  // BUILD21: 除夕 = 正月初一前一天。两条判据(任一命中即为除夕):
  //   (a) holidays JSON 当天名 == "除夕" (政府数据, 2026 等已发布的年);
  //   (b) nl_is_chuxi(y,m,d) 内置农历表算出"该年正月初一 - 1 天"。
  //   (b) 不依赖 JSON —— 2027 的 JSON 为空(节假日未发布)时, 除夕(2/5)也能显示。
  //   必须放在 almRealFest **之前**: 农历表对 2026 差一天(把 2/16 记成正月初一),
  //   若放后面, almRealFest 会先误返"春节"。
  const char* chuxiName = holiday::dayName(y, m, d);
  bool isChuxi = (chuxiName && strcmp(chuxiName, "除夕") == 0) || nl_is_chuxi(y, m, d);
  if (isChuxi) { snprintf(buf, bufsz, "除夕"); return buf; }
  if (almRealFest(y, m, d, f, sizeof(f))) { snprintf(buf, bufsz, "%s", f); return buf; }
  if (holiday::dayType(y, m, d) != HOL_OFF) { buf[0] = 0; return buf; }
  int ay = y, am = m, ad = d;
  for (int i = 0; i < 40; i++) {
    int ny = ay, nm = am, nd = ad; almAddDay(&ny, &nm, &nd, -1);   // BUILD21: from running ay (not y), walk whole run
    if (holiday::dayType(ny, nm, nd) == HOL_OFF) { ay = ny; am = nm; ad = nd; } else break;
  }
  int by = y, bm = m, bd = d;
  for (int i = 0; i < 40; i++) {
    int ny = by, nm = bm, nd = bd; almAddDay(&ny, &nm, &nd, 1);   // BUILD21: from running by (not y), walk whole run
    if (holiday::dayType(ny, nm, nd) == HOL_OFF) { by = ny; bm = nm; bd = nd; } else break;
  }
  int cy = ay, cm = am, cd = ad;
  for (int i = 0; i < 40; i++) {
    bool past = (cy > by) || (cy == by && (cm > bm || (cm == bm && cd > bd)));
    if (past) break;
    char cf[16];
    if (almRealFest(cy, cm, cd, cf, sizeof(cf))) { snprintf(buf, bufsz, "%s", cf); return buf; }
    almAddDay(&cy, &cm, &cd, 1);
  }
  const char* hol = holiday::dayName(y, m, d);
  if (hol && hol[0]) snprintf(buf, bufsz, "%s", hol); else buf[0] = 0;
  return buf;
}
static void drawAlmPopup(int y, int m, int d) {
  // lint:skip-rule-1  (alm popup: one-shot static draw, no continuous dynamic region)

  const int PX = 24, PW = 432, PT = 16, PH = 272;

  int cx = 240;

  int ld[31]; nl_month_days(y, m, ld);

  int v = ld[d - 1];

  int lm = v / 100, ldn = v % 100;

  bool isLeap = (lm > 100); if (isLeap) lm -= 100;

  int lyz = nl_zodiac_year(y, m, d);

  static const char* TG2[10] = {"甲","乙","丙","丁","戊","己","庚","辛","壬","癸"};

  static const char* DZ2[12] = {"子","丑","寅","卯","辰","巳","午","未","申","酉","戌","亥"};

  static const char* ZODIAC_D[12] = {"鼠","牛","虎","兔","龙","蛇","马","羊","猴","鸡","狗","猪"};

  static const char* PMON[12] = {"正月","杏月","桃月","槐月","榴月","荷月","兰月","桂月","菊月","阳月","冬月","腊月"};

  char ygz[24];

  snprintf(ygz, sizeof(ygz), "%s%s%s", nl_tg_text[nl_tg(lyz)], nl_dz_text[nl_dz(lyz)], nl_sx_text[nl_dz(lyz)]);

  int dayStd = (almJDN(y, m, d) + 49) % 60;

  int jqCur = -1;

  int _jqCurD = 0, _jqNxt = -1, _jqNxtD = 0;

  nl_jq_info(y, m, d, &jqCur, &_jqCurD, &_jqNxt, &_jqNxtD);

  char dgz[8]; snprintf(dgz, sizeof(dgz), "%s%s", TG2[dayStd % 10], DZ2[dayStd % 12]);

  char pmon[8];

  if (isLeap) snprintf(pmon, sizeof(pmon), "闰%s", PMON[(lm - 1) % 12]);

  else snprintf(pmon, sizeof(pmon), "%s", PMON[(lm - 1) % 12]);

  char lcol_main[16]; snprintf(lcol_main, sizeof(lcol_main), "%s月%s", lunarMonths[lm - 1], lunarDays[ldn - 1]);

  struct tm t0; memset(&t0, 0, sizeof(t0)); t0.tm_year = y - 1900; t0.tm_mon = m - 1; t0.tm_mday = d; t0.tm_isdst = -1;

  time_t tt0 = mktime(&t0); struct tm tl; localtime_r(&tt0, &tl);

  const char* wday = weekHdr[tl.tm_wday];

  char yistr[8]; snprintf(yistr, sizeof(yistr), "%d", y);

  char mistr[8]; snprintf(mistr, sizeof(mistr), "%d", m);

  char fest[16] = "";

  almHolidayFest(y, m, d, fest, sizeof(fest));   // festival name for the whole holiday run
  // BUILD23: dedup yi/ji (mirror make_preview.py seen-set). Old 3-consecutive pick
  // hit YI pool dups 嫁娶(0,10)/出行(1,11) -> 10/28 (dayStd%12=11) gave 出行/嫁娶/出行.
  const char* yi3[3];
  {
    int n = 0;
    for (int k = 0; k < 12 && n < 3; k++) {
      const char* it = ALM_YI_ITEMS[(dayStd + k) % 12];
      int dup = 0;
      for (int j = 0; j < n; j++) if (strcmp(yi3[j], it) == 0) { dup = 1; break; }
      if (!dup) yi3[n++] = it;
    }
  }

  const char* ji3[3];
  {
    int n = 0;
    for (int k = 0; k < 12 && n < 3; k++) {
      const char* it = ALM_JI_ITEMS[(dayStd + k) % 12];
      int dup = 0;
      for (int j = 0; j < n; j++) if (strcmp(ji3[j], it) == 0) { dup = 1; break; }
      if (!dup) ji3[n++] = it;
    }
  }

  // ---- 背景 + 四角括号 ----

  tft.fillRect(PX, PT, PW, PH, ALM_PAPER);

  {

    const int L = 16;

    tft.drawFastHLine(PX, PT, L, ALM_LINE); tft.drawFastVLine(PX, PT, L, ALM_LINE);

    tft.drawFastHLine(PX + PW - 1 - L, PT, L, ALM_LINE); tft.drawFastVLine(PX + PW - 1, PT, L, ALM_LINE);

    tft.drawFastHLine(PX, PT + PH - 1, L, ALM_LINE); tft.drawFastVLine(PX, PT + PH - 1 - L, L, ALM_LINE);

    tft.drawFastHLine(PX + PW - 1 - L, PT + PH - 1, L, ALM_LINE); tft.drawFastVLine(PX + PW - 1, PT + PH - 1 - L, L, ALM_LINE);

  }

  // ---- 顶行 (y=30): 年份 干支X年 | 月份 月 | 农历X月X | 花名月·节气后/今日X | 右上 节日框 ----
  almBigText(PX + 14, 30, String(yistr), 24, ALM_DARK, ALM_PAPER, false);  // 2026
  int ygzX = PX + 14 + 4 * (24 * 7 / 10) + 8;
  almScaledText(ygzX, 36, String(ygz), 13, ALM_DARK, ALM_PAPER, false);  // 丙午马 (13px)
  const int monX = 162;
  almBigText(monX, 30, String(mistr), 24, ALM_RED, ALM_PAPER, false);  // 10 (大红)
  almScaledText(monX + 2 * (24 * 7 / 10) + 4, 36, "月", 13, ALM_RED, ALM_PAPER, false);  // 月
  const int lunX = 218;
  String lunStr2 = String("农历") + lunarMonths[lm - 1] + "月" + lunarDays[ldn - 1];
  almScaledText(lunX, 36, lunStr2, 13, ALM_DARK, ALM_PAPER, false);  // 农历X月X
  char pjText[24];
  if (jqCur < 0) snprintf(pjText, sizeof(pjText), "%s", pmon);
  else if (_jqCurD == 0) snprintf(pjText, sizeof(pjText), "今日%s", nl_jq_text[jqCur]);  // 节气当日 -> 今日X
  else snprintf(pjText, sizeof(pjText), "%s·%s后", pmon, nl_jq_text[jqCur]);  // 之后 -> X后
  int lunW = font16.textWidth(lunStr2) * 13 / 16;
  int pjW = font16.textWidth(String(pjText)) * 13 / 16;
  int pjX = lunX + lunW + 6;
  int fw = fest[0] ? (font16.textWidth(String(fest)) + 14) : 0;
  int boxLeft = fest[0] ? (PX + PW - 12 - fw) : (PX + PW - 14);
  if (pjX + pjW > boxLeft - 4) pjX = max(lunX + lunW + 2, boxLeft - 4 - pjW);  // 不越过节日框
  // BUILD21: 花名月·节气 节日当天也显示(与右上节日框并排, 靠左由上方 clamp 保证不重叠)。
  // 旧版 if(!fest[0]) 会在节日天隐藏它 -> 用户看到"节日那天节气/花月名消失"。
  almScaledText(pjX, 36, String(pjText), 13, ALM_DARK, ALM_PAPER, false);  // 花名月·节气后 / 今日X
  if (fest[0]) {
    tft.drawRect(PX + PW - 12 - fw, 28, fw, 26, ALM_RED);
    almScaledText(PX + PW - 12 - fw + 7, 34, String(fest), 15, ALM_RED, ALM_PAPER, false);
  }
  // ---- 三列中部 (左列 农历 红 / 右列 干支 深褐; 中央环 + 大数字 红; 环中心 y=148) ----
  const int LC = 90, RC = 390;
  uint16_t numCol;
  {
    int _htd = holiday::dayType(y, m, d);
    if (_htd == HOL_OFF || _htd == HOL_WORK) numCol = ALM_RED;   // 放假/补班 -> 红
    else if (fest[0]) numCol = ALM_RED;                          // BUILD23: 任何节日(含西方 万圣节/黑五/母亲节...) -> 红 (镜像 preview fest)
    else if (tl.tm_wday == 0 || tl.tm_wday == 6) numCol = ALM_WKND;
    else numCol = ALM_JI;
  }
  almBigText(LC, 122, String(lcol_main), 24, numCol, ALM_PAPER, true);
  almScaledText(LC, 166, String("农历·") + pmon, 14, ALM_DARK, ALM_PAPER, true);  // 农历·桂月
  almBigText(RC, 122, String(dgz) + "日", 24, ALM_DARK, ALM_PAPER, true);  // 壬寅日 (深褐)
  almScaledText(RC, 166, String("干支·") + ZODIAC_D[dayStd % 12], 14, ALM_DARK, ALM_PAPER, true);  // 干支·虎
  tft.drawCircle(cx, 148, 72, ALM_RING);  // 外环
  tft.drawCircle(cx, 148, 57, ALM_RING);  // 内环
  almBigNum(cx - ALM_DW, 148 - ALM_DH / 2, d, numCol);  // 大数字 (中心对齐环中心 148)
  // ---- 底栏 ----
  tft.drawFastHLine(PX + 10, 230, PW - 21, ALM_LINE);
  {
    almScaledText(PX + 12, 240, "星期", 18, ALM_DARK, ALM_PAPER, false);  // 星期 (18px, 底栏垂直中忉)
    almBigText(PX + 54, 234, String(wday), 30, ALM_DARK, ALM_PAPER, false);  // 六 (30px, 底栏垂直中忉)
  }

  {

    int tx = 150;

    tft.fillRect(tx, 236, 16, 16, ALM_JI);

    font16.drawStringFast(tx, 236, String("宜"), ALM_PAPER, ALM_JI);

    int xi = tx + 24;

    for (int i = 0; i < 3; i++) { font16.drawStringFast(xi, 236, String(yi3[i]), ALM_DARK, ALM_PAPER); xi += font16.textWidth(String(yi3[i])) + 6; }

    tft.fillRect(tx, 258, 16, 16, ALM_RED);

    font16.drawStringFast(tx, 258, String("忌"), ALM_PAPER, ALM_RED);

    xi = tx + 24;

    for (int i = 0; i < 3; i++) { font16.drawStringFast(xi, 258, String(ji3[i]), ALM_DARK, ALM_PAPER); xi += font16.textWidth(String(ji3[i])) + 6; }

  }

  {

    int lx = PX + PW - 14 - 130;

    static const uint16_t lg[3] = {ALM_JI, ALM_WKND, ALM_RED};

    static const char* nm[3] = {"平日", "周末", "节假日"};

    for (int r = 0; r < 3; r++) {

      int yy = 234 + r * 18;

      tft.fillCircle(lx + 3, yy + 8, 3, lg[r]);

      font16.drawStringFast(lx + 12, yy, String(nm[r]), lg[r], ALM_PAPER);

    }

  }

  // moon (legend-right): icon + name
  {
    double mp = nl_moon_phase(y, m, d);
    int mi = (int)(mp * 8.0 + 0.5) % 8;
    drawMoonIcon(415, 250, 9, mp);
    font16.drawStringFast(415 - font16.textWidth(nl_moon_text[mi]) / 2, 264, nl_moon_text[mi], 0xDE72, ALM_PAPER);
  }
}

void modeCalendar() {
  if (!modeDrawn[MODE_CALENDAR] || modeChanged) {
    drawCalendar();
    modeDrawn[MODE_CALENDAR] = true;
  }
}

// 年月选择弹层的点击�?��??
void handlePickerTap(uint16_t tx, uint16_t ty);
// 日历页点�?
//  - 关闭�? 点月份标�?= 打开年月选择; 点某�?= 选中(显示其农�?; �?�?1/3 = 上一�?下一�?
void handleCalendarTap(uint16_t tx, uint16_t ty) {
  if (calAlmOpen) {          // 黄历弹层打开: 点任意处关闭 (无关闭按钮, 镜像 preview)
    if (tx >= 24 && tx < 24 + 432 && ty >= 16 && ty < 16 + 272) {
      calAlmOpen = false;
      drawCalendar();
      Serial.printf("almanac closed\n");
    }
    return;
  }
  if (calPickerOpen) {
    handlePickerTap(tx, ty);
    return;
  }
  if (ty < CAL_GRID_TOP) {
    // 顶部标�?��?? 点中�?1/3 (月份标�??) 打开选择�? 左右 1/3 = 上一�?下一�?
    if (tx < CAL_X1 / 3) {
      calMon--;
      if (calMon < 1) { calMon = 12; calYear--; }
      drawCalendar();
      Serial.printf("calendar -> %d-%d\n", calYear, calMon);
    } else if (tx > (CAL_X1 * 2) / 3) {
      calMon++;
      if (calMon > 12) { calMon = 1; calYear++; }
      drawCalendar();
      Serial.printf("calendar -> %d-%d\n", calYear, calMon);
    } else {
      calPickY = calYear; calPickM = calMon;
      calPickerOpen = true;
      drawCalendar();
    }
    return;
  }
  // 日期网格�? 命中某天 -> 选中 (显示其农�?
  if (ty >= CAL_GRID_TOP && ty < CAL_GRID_TOP + 6 * CAL_CELL_H + 6 && tx >= CAL_X0 && tx < CAL_X1) {
    int c = (tx - CAL_X0) / CAL_CELL_W;
    int rr = (ty - CAL_GRID_TOP) / CAL_CELL_H;
    if (rr > 5) rr = 5;   // BUILDV5-CALFIX: y 234..239 归最后一行
    int idx = rr * 7 + c;                 // 0..41, 对应 1�?起算
    int day = idx - calFirstW + 1;
    if (day >= 1 && day <= calDim) {
      unsigned long nowMs2 = millis();
      bool isDbl = (calLastTapDay == day && calLastTapY == calYear && calLastTapM == calMon &&
                    nowMs2 - calLastTapMs < 600);   // double-tap same day within 600ms -> almanac
      calLastTapDay = day; calLastTapY = calYear; calLastTapM = calMon; calLastTapMs = nowMs2;
      if (isDbl) {
        calSelY = calYear; calSelM = calMon; calSelD = day;
        calAlmOpen = true;
        drawCalendar();                    // full redraw shows the almanac popup
        Serial.printf("calendar almanac -> %d-%d-%d\n", calYear, calMon, day);
      } else {
        calSelY = calYear; calSelM = calMon; calSelD = day;   // single tap = select
        time_t now2 = time(nullptr); struct tm tt2; localtime_r(&now2, &tt2);
        redrawCalSelection(tt2.tm_year + 1900, tt2.tm_mon + 1, tt2.tm_mday);   // partial: no full flash
        Serial.printf("calendar sel -> %d-%d-%d\n", calYear, calMon, day);
      }
    }
    return;
  }
  // 底部信息�?"今日" 按钮: �?�?回到今�??(跳到今天�?在月 + 选中今天)
  {
    int stripTop = CAL_GRID_TOP + 6 * CAL_CELL_H + 8;
    if (ty >= stripTop && ty < stripTop + 34) {
      int btW = font16.textWidth("今日") + 16;
      int btX = CAL_X1 - 8 - btW;
      if (tx >= btX - 2 && tx < btX + btW + 2) {
        time_t now = time(nullptr);
        struct tm t;
        localtime_r(&now, &t);
        calYear = t.tm_year + 1900; calMon = t.tm_mon + 1;
        calSelY = calYear; calSelM = calMon; calSelD = t.tm_mday;
        drawCalendar();
        Serial.printf("calendar -> today %d-%d-%d\n", calYear, calMon, calSelD);
      }
    }
  }
}

// 年月选择弹层的点击�?��??
void handlePickerTap(uint16_t tx, uint16_t ty) {
  if (ty >= PICK_PT && ty <= PICK_PB && tx >= PICK_PANEL_X && tx < PICK_PANEL_X + PICK_PANEL_W) {
    if (ty < PICK_PT + 40) return;   // 标�?��??("选择年月"), 点了不�?��??
    // 年份步进�?(PICK_PT ~ 网格顶部): < �? > �?
    if (ty >= PICK_PT + 40 && ty < PICK_MTOP) {
      if (tx < 240) { calPickY--; if (calPickY < 2000) calPickY = 2000; }
      else if (tx > 240) { calPickY++; if (calPickY > 2099) calPickY = 2099; }
      drawCalendar();
      return;
    }
    // 月份网格
    if (ty >= PICK_MTOP && ty < PICK_MTOP + 2 * PICK_MROW_H && tx >= PICK_MGRID_X && tx < PICK_MGRID_X + 6 * PICK_MCOL_W) {
      int mc = (tx - PICK_MGRID_X) / PICK_MCOL_W;
      int mr = (ty - PICK_MTOP) / PICK_MROW_H;
      int m = mr * 6 + mc + 1;
      if (m >= 1 && m <= 12) { calPickM = m; drawCalendar(); }
      return;
    }
    // 按钮�?
    if (ty > PICK_PB - PICK_MROW_H) {
      if (tx < 240) {   // 取消
        calPickerOpen = false;
        drawCalendar();
      } else {          // �?�?
        calYear = calPickY; calMon = calPickM;
        calPickerOpen = false;
        // 选中日默�? 今天�?在月 -> 今天; 否则 -> 1�?
        time_t now = time(nullptr);
        struct tm t;
        localtime_r(&now, &t);
        int ty = t.tm_year + 1900, tmn = t.tm_mon + 1, td = t.tm_mday;
        if (ty == calYear && tmn == calMon) { calSelY = calYear; calSelM = calMon; calSelD = td; }
        else { calSelY = calYear; calSelM = calMon; calSelD = 1; }
        drawCalendar();
        Serial.printf("picker -> %d-%d\n", calYear, calMon);
      }
      return;
    }
    return;   // 面板内但�?命中具体区�??(如标�?缝隙) -> 忽略, 不关�?
  }
  // 点面板�??: 关闭弹层
  calPickerOpen = false;
  drawCalendar();
}

// ============================================================================
// 天气页面 (BUILD-WEATHER, 2026-10-04)
// 布局 (480 x 288, 导航栏占 288..320):
//   头部     y34  城市 + 右侧日期 + 更新时间
//   主区左   y50..185  大温度数字 (62x112, 2位) + 度号 + 天气图标 (右) + 状况文字
//   主区中   y140..184  湿度 / 风力 两行 16px AA
//   卡片区   y190..234  4 卡片: 气压 | AQI | UV | 能见度 (96px 宽, 间距 8)
//   预报条   y248..286  7 天 (今天/明天/周一..), 图标 + 高/低温
// 数据: 全部 mock (接入和风 API 后替换)。与 preview/make_preview.py draw_weather_scene 逐像素一致。
// lint:skip-rule-1  (静态页面, 一次性绘制, 无连续动态区域)
// ============================================================================

// 天气配色 (RGB565, 与 preview draw_weather_scene 完全一致)
#define W_PANEL   0x2104            // 卡片底 深灰蓝
#define W_TEXT    TFT_WHITE         // 主文字 白
#define W_ACCENT  0x07FF            // 强调 青
#define W_CLOUD   TFT_WHITE         // 云 白
#define W_SNOW    0x7DFF            // 雪 淡蓝

// 天气图标: 24x24 像素位图 (wicons.h, 生成器 scripts/gen_wicons.py, 与 preview/wicons.py 同源).
// 中心 cx,cy, 尺寸 s (最近邻缩放; idx0=透明 -> bg). 与 preview w_icon() 逐像素一致.
// lint:skip-rule-1  (静态一次性绘制, 逐行推无闪烁风险)
