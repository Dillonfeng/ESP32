#include "ui_common.h"

// ========== 时钟模块内部状态 (BUILD61: 从 main.cpp 拆出) ==========
static uint16_t *analogFace = nullptr;
static uint16_t *cardSram = nullptr;
static uint16_t *rollerSram = nullptr;
static uint16_t *bandSram = nullptr;
static uint16_t *bandBuf = nullptr;
static uint16_t *flapTmp = nullptr;
static uint16_t *analogFaceCache = nullptr;
// BUILDV5-CLKFIX-B (2026-10-17): 时钟缓冲池——FLAP/ANALOG 共享一块 boot 常驻内部 RAM 池。
// 根因(240s 抓包铁证): 样式切换(点击时钟页)走 allocClockBufs 样式互斥分支, free 旧样式缓冲后再
// 分配新样式; 播放期内部堆碎片化 maxBlock=40KB < analogFace 42.6KB → 切回指针失败 → fallback 大字
// ("指针时钟又没了")。修法 = boot 一次性分配 58.5KB 池(内部 RAM), FLAP 三缓冲 / ANALOG analogFace
// 都指向池内偏移, 样式切换只改偏移+重建内容, 零 malloc/free → 与碎片完全无关, 切换永远成功。
// 代价: 播放期内部空闲 81KB→66KB(池按 FLAP 最大需求常驻), 实施后抓包验证播音不受影响(验收标准)。
static uint16_t *clockPool = nullptr;   // 内部 RAM 池
// 池内偏移(元素) —— 数值对应 BAND_W=166,CARD_SRAM_H=123,CARD_W=56,ROLLER_W=16,CARD_H=120(宏在下方定义,此处用字面量)
static const size_t OFF_BAND   = 0;
static const size_t OFF_CARD   = (size_t)(166 * 123);              // bandSram 区 = 166*123 = 20418
static const size_t OFF_ROLLER = OFF_CARD + (size_t)(56 * 123);    // +cardSram 56*123 → 27306
static const size_t CLOCK_POOL_ELEMS = OFF_ROLLER + (size_t)(16 * 120); // +rollerSram 16*120 → 29226 (=58452B)
static bool     analogFaceBuilt = false;
struct FlipState {
  bool     flipping;
  char     shown;
  char     newD;
  int      clicks;
  bool     redrawn;
  uint32_t startMs;
};
static FlipState fstate[6];
static float rollerColB[ROLLER_W];
static bool flapReady = false;

// ============================================================
// modeClock / modeCalendar / modeWeather
// ============================================================

// 单张数字牌各 x 坐标 (索引 0..5 �?HH MM SS)
static const int cardX[6] = {CARD_X0, CARD_X1, CARD_X2, CARD_X3, CARD_X4, CARD_X5};

// 金属铰链 (直写屏幕�?: 5px 竖直渐变�?(高光->�?�?->暗沟->�?�?->反光) + 两�?? 3x5 �?轴钉�?
static void drawHingeCard(int x, int seamY, int w);

// 绘制�?张静态数字牌: 白底圆�?? + �?�?铰链 + 下缘投影 + 居中数字
// shadeTop: �?否给上半部加暗阴�? (用于翻页动画)
static void drawCard(int idx, char digit, bool shadeTop) {
  if (!cardSram) return;   // BUILD17: guard (alloc failed)
  int x = cardX[idx], y = CARD_TOP, w = CARD_W, h = CARD_H;
  int seamY = y + h / 2;
  // 下缘投影 (先画, 让牌�?�?)
  tft.fillRoundRect(x, y + 3, w, h, CARD_RADIUS, CARD_EDGE);
  // 牌面
  tft.fillRoundRect(x, y, w, h, CARD_RADIUS, CARD_FACE);
  // 翻页阴影: 覆盖上半�?
  if (shadeTop) {
    tft.fillRoundRect(x, y, w, h / 2, CARD_RADIUS, CARD_SHADE);
    tft.fillRect(x, seamY, w, 1, CARD_FACE); // 擦平铰链线�?�的阴影�?
  }
  // 抗锯齿数�? �?8 位灰度遮罩按当前背景(牌面/翻页阴影)逐�?�道混合后推送�??
  // mask=255 -> 背景�? mask=0 -> �?数字笔画)。�?��?�道混合才不偏色�?
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
  // 金属铰链 (5px 渐变�?+ 两�??�?轴�?? 必须在数字之后画, 否则�?数字盖�?? �?preview _draw_hinge �?�?
  drawHingeCard(x, seamY, w);
}

// ---- 两相 split-flap 翻页 (非阻�? �?preview live_preview/make_preview 逐帧�?�? ----
#define FLAP_BAND_LINE 0x7350   // (120,120,135) 翻片带边�?

static bool flapInRoundRect(int px, int py, int W, int H, int r) {
  if (px < 0 || px >= W || py < 0 || py >= H) return false;
  int cx = (px < r) ? r : (px > W - 1 - r) ? (W - 1 - r) : px;
  int cy = (py < r) ? r : (py > H - 1 - r) ? (H - 1 - r) : py;
  if (cx == px && cy == py) return true;
  int dx = px - cx, dy = py - cy;
  return dx * dx + dy * dy <= r * r;
}

static float flapEase(float p) {           // smoothstep: �?�?�?(preview _ease)
  if (p < 0) p = 0; if (p > 1) p = 1;
  return p * p * (3 - 2 * p);
}

// 翻页动画: 半牌不再预渲染进 PSRAM (octal PSRAM DMA 读取错乱 = "秒个位彩虹花屏" 根因),
// 改为每次按需由 renderHalfCard() 从 flash 数字遮罩现渲染进内部 RAM, 翻页全程只碰内部 RAM。
// flapInit() 只预计算齿轮列明暗, 无 PSRAM 分配, 不会失败。
static bool flapInit(void) {
  if (flapReady) return true;
  for (int x = 0; x < ROLLER_W; x++) {             // 齿轮列明暗 (preview _roller_col_profile)
    float t = (float)x / (ROLLER_W - 1);
    float g = 0.20f + 0.80f * sinf(t * (float)M_PI);
    float spec = 1.0f - fabsf(t - 0.5f) / 0.16f; if (spec < 0) spec = 0; spec *= 0.30f;
    rollerColB[x] = g + spec;
  }
  flapReady = true;
  return true;
}
// 现渲染单张半牌 (上/下, 56x60) 进内部 RAM out[]: 圆角白牌面 (角区=屏幕背景) + 8 位灰度数字遮罩。
// 从 flash (DigitMasks) 现算, 不经过 PSRAM, 彻底消除翻页时 octal PSRAM DMA 读取错乱 (彩虹花屏)。
static void renderHalfCard(int digit, bool top, uint16_t *out) {
  const int HH = CARD_H / 2;                       // 60
  const int r5 = (CARD_FACE >> 11), g6 = (CARD_FACE >> 5) & 0x3F, b5 = CARD_FACE & 0x1F;
  for (int y = 0; y < HH; y++) {
    int py = top ? y : (HH + y);                   // 全牌坐标 (0..119)
    for (int px = 0; px < CARD_W; px++)
      out[y * CARD_W + px] = flapInRoundRect(px, py, CARD_W, CARD_H, CARD_RADIUS)
        ? ((r5 << 11) | (g6 << 5) | b5) : 0x0000;
  }
  const uint8_t *mask = DigitMasks[digit];
  for (int dy = 0; dy < DIGIT_H; dy++)
    for (int dx = 0; dx < DIGIT_W; dx++) {
      int px = (CARD_W - DIGIT_W) / 2 + dx;
      int py = (CARD_H - DIGIT_H) / 2 + dy;        // 全牌坐标
      if (top ? (py < HH) : (py >= HH)) {          // 只落在本半牌内
        uint8_t m = pgm_read_byte(&mask[dy * DIGIT_W + dx]);
        int oy = top ? py : (py - HH);
        out[oy * CARD_W + px] = (((r5 * m) / 255) << 11) | (((g6 * m) / 255) << 5) | ((b5 * m) / 255);
      }
    }
}
// 翻片带: 旧半牌现渲染 (renderHalfCard, 内部 RAM) 后压缩到 bandH 高, 逐行变暗
// (preview _shade_band, LANCZOS≈面积平均)。不再读 PSRAM, 消除翻页彩虹花屏。
static void buildFlapBand(int oldD, bool top, int bandH, float strength, bool flipShade = false) {
  const int HH = CARD_H / 2;
  renderHalfCard(oldD, top, flapTmp);
  const uint16_t *src = flapTmp;
  int H2 = max(1, bandH);
  for (int yy = 0; yy < H2; yy++) {
    int from = (int)((long)yy * HH / H2);
    int to   = (int)((long)(yy + 1) * HH / H2);
    if (to <= from) to = from + 1;
    float t = (flipShade ? (float)(H2 - 1 - yy) : (float)yy) / max(1, H2 - 1);              // 0 靠近铰链 .. 1 远端
    float k = 1.0f - (0.12f + 0.70f * strength * t);    // 越远越暗
    for (int xx = 0; xx < CARD_W; xx++) {
      int sr = 0, sg = 0, sb = 0;
      for (int sy = from; sy < to; sy++) {
        uint16_t c = src[sy * CARD_W + xx];
        sr += (c >> 11); sg += ((c >> 5) & 0x3F); sb += (c & 0x1F);
      }
      int n = to - from;
      int rr = (int)((sr / n) * k), gg = (int)((sg / n) * k), bb = (int)((sb / n) * k);
      if (rr < 0) rr = 0; if (gg < 0) gg = 0; if (bb < 0) bb = 0;
      bandBuf[yy * CARD_W + xx] = (rr << 11) | (gg << 5) | bb;
    }
  }
}

// ---- 原子�?(atomic frame) 内部 RAM 缓冲 ----
// 根因 = **帧撕�?(frame tearing)**: ST7796S �?SPI 驱动�?*�?�? TE(撕�?�同�?)�?*, 屏幕
// 面板持续以自�?~60-120Hz �?描自己�??GRAM, �?MCU 同时�?里写像素 -> 两�?�不同�?��?�凡
// "每帧内�?�会�?"的区�?秒个位卡/两枚齿轮/指针秒针)�?描线与写入交�? -> 上半显示新数�?�?
// 下半显示旧数�?= 用户看到�?半旧半新 / 铰链糊成长条 / �?�? / 翻页乱序"; �?每帧不变"
// 的区�?�?分卡 1-5)GRAM 稳定 -> 无撕�? �?以看起来干净。preview(Pillow) �?渲染到�?�屏
// 图像后一次�??blit(= 天然帧同�?, 故完美�??
// �?�? = �?会动的区�?, �?帧内�?有像素先合成�?*�?块内�?RAM 缓冲**, �?*单�?? pushImage
// 整帧**(原子�? 无中间状�? —�??与已验证干净的指针表�?analogFace 同�?��??
//  (此前"PSRAM 彩虹花屏 = DMA �?PSRAM 取错数据"�?�?�? �?板�??USE_FSPI_PORT 同�??
//   spi.transfer(), 无异�?DMA, �?PSRAM 同�?��?�没�?�?; 真�?�根因就�?上面的帧撕�?��??
// 整张牌缓�? 56 x (120+3), �?�?(120) + 下缘投影露出�?3px �? �?�?pushImage 覆盖�?
#define CARD_SHAD_ROW 3
#define CARD_SRAM_H   (CARD_H + CARD_SHAD_ROW)
// cardSram buffer (declared at top; alloc'd by allocClockBufs)        // BUILD17: heap buffer (was static .bss), alloc'd by allocClockBufs
// 齿轮缓冲: 16 x 120
// rollerSram buffer (declared at top; alloc'd by allocClockBufs)      // BUILD17: heap buffer (was static .bss), alloc'd by allocClockBufs
// ---- 齿轮�?(atomic roller band): 两枚齿轮 + �?间两张�??(�?,3) 合成进一块内�?RAM
// 缓冲, �?*单�?? pushImage** 整帧。根�? 两齿�?若各�?�?�? pushImage (每帧 3 �?SPI 事务:
// 翻牌 + 左齿�?+ 右齿�?, �?TE 面板�?描线与写入交�? -> 右齿�?后推, �?描相位不�?)顶�??
// 出现"彩色撕�??/blur", 而左齿轮相位合�?�故干净(用户观察到的"右有左无")。合并为�?笔后
// 两齿�?同扫描相位原子更�?, 消除不�?�称。带�?= GAP1 齿轮左缘 .. GAP2 齿轮右缘�?
#define BAND_X0     (GAP1_CX - ROLLER_W / 2)      // 157
#define BAND_X1     (GAP2_CX + ROLLER_W / 2)      // 323
#define BAND_W      (BAND_X1 - BAND_X0)           // 166
// bandSram buffer (declared at top; alloc'd by allocClockBufs)        // BUILD17: heap buffer (was static .bss), alloc'd by allocClockBufs

// 把一行水平线画进整卡缓冲 (y �?[0, CARD_SRAM_H) �?
static void lineBufCard(int x0, int y, int x1, uint16_t col) {
  if (y < 0 || y >= CARD_SRAM_H) return;
  if (x0 < 0) x0 = 0; if (x1 >= CARD_W) x1 = CARD_W - 1;
  for (int x = x0; x <= x1; x++) cardSram[y * CARD_W + x] = col;
}
// 铰链 (整卡缓冲�?: �?�? 1px 细缝 + 两�?? 3x3 金属�?轴�?? 写进 cardSram (x 相�?�牌左�??
// �?drawHingeCard 逐像素一�?
static void hingeBufCard(int seamY, int w) {
  lineBufCard(4, seamY, w - 5, CARD_SEAM);   // 中央 1px 细缝 (两端转轴帽之间)
  const uint16_t kc[6] = { (uint16_t)HINGE_HI, (uint16_t)HINGE_HI, (uint16_t)HINGE_MID, (uint16_t)HINGE_MID, (uint16_t)HINGE_LO, (uint16_t)HINGE_LO };
  for (int r = 0; r < 6; r++) {
    for (int px = 0; px < 4; px++) {
      cardSram[(seamY - 3 + r) * CARD_W + px] = kc[r];              // 左转轴帽
      cardSram[(seamY - 3 + r) * CARD_W + (w - 4 + px)] = kc[r];    // 右转轴帽
    }
  }
}
// 圆�?�矩形填进整卡缓�? (�?tft.fillRoundRect 同几�? 角区�?flapInRoundRect 判断)
static void fillRoundRectCard(int ox, int oy, int w, int h, int r, uint16_t col) {
  for (int py = 0; py < h; py++) {
    int by = oy + py; if (by < 0 || by >= CARD_SRAM_H) continue;
    for (int px = 0; px < w; px++) {
      int bx = ox + px; if (bx < 0 || bx >= CARD_W) continue;
      if (flapInRoundRect(px, py, w, h, r)) cardSram[by * CARD_W + bx] = col;
    }
  }
}

// 铰链 (直写屏幕�?: �?�? 1px 细缝 + 两�?? 3x3 金属�?轴�??(受光高光->�?�?->暗沟 竖渐�?�?
// 细缝在牌面�?��?? 两�??帽是机�?�翻牌的�?轴�?�x=牌绝�?x, seamY=缝绝�?y, w=CARD_W�?
static void drawHingeCard(int x, int seamY, int w) {
  tft.drawFastHLine(x + 4, seamY, w - 8, CARD_SEAM);   // 中央 1px 细缝 (两端转轴帽之间)
  const uint16_t kc[6] = { (uint16_t)HINGE_HI, (uint16_t)HINGE_HI, (uint16_t)HINGE_MID, (uint16_t)HINGE_MID, (uint16_t)HINGE_LO, (uint16_t)HINGE_LO };  // 顶高光->中->底暗
  for (int r = 0; r < 6; r++) {
    for (int px = 0; px < 4; px++) {
      tft.fillRect(x + px,         seamY - 3 + r, 1, 1, kc[r]);   // 左转轴帽
      tft.fillRect(x + w - 4 + px, seamY - 3 + r, 1, 1, kc[r]);   // 右转轴帽
    }
  }
}

// 单张牌当前帧 (phase 1=上半翻片下落露新上半 / 2=下半翻片上翻补新下半)�?
// 整张�?牌面+�?旧上下半+翻片�?铰链�?铰链�?)先合成进 cardSram, �?*单�??** pushImage
// 整帧 => 原子�? 消除"�?�?->推半�?>推翻片带->画缝"多�?�写之间的面板扫描空�?(乱序/撕�??)�?
static void drawFlapFrame(int idx, char oldD, char newD, int phase, float p) {
  int x = cardX[idx], y = CARD_TOP, w = CARD_W, h = CARD_H;
  int half = h / 2, seamY = h / 2;              // 缓冲内坐�?(y=0 = CARD_TOP)
  // 先整块清成页面背�?�?: cardSram �?复用缓�?? 圆�?��??下缘投影两�?�区若不显式�?
  // 会残留上�?帧像素�??�?并推�?(花屏)。整块填黑后, 圆�?��??= �?= 页面底色�?
  for (int i = 0; i < CARD_W * CARD_SRAM_H; i++) cardSram[i] = COLOR_BG;
  // 下缘投影 (牌下露出 3px) + 牌面
  fillRoundRectCard(0, CARD_SHAD_ROW, w, h, CARD_RADIUS, CARD_EDGE);
  fillRoundRectCard(0, 0, w, h, CARD_RADIUS, CARD_FACE);
  int bandH = max(1, (int)((1.0f - p) * half + 0.5f));
  if (phase == 1) {
    renderHalfCard(newD - '0', true,  flapTmp);
    memcpy(&cardSram[0],                    flapTmp, CARD_W * half * 2);                      // 新上半
    renderHalfCard(oldD - '0', false, flapTmp);
    memcpy(&cardSram[seamY * CARD_W],       flapTmp, CARD_W * half * 2);                      // 旧下半
    buildFlapBand(oldD - '0', true, bandH, p);
    memcpy(&cardSram[(seamY - bandH) * CARD_W], bandBuf, CARD_W * bandH * 2);                // 旧上半翻片带
    lineBufCard(1, seamY - bandH, w - 2, FLAP_BAND_LINE);
  } else {
    renderHalfCard(newD - '0', true,  flapTmp);
    memcpy(&cardSram[0],                    flapTmp, CARD_W * half * 2);                      // 新上半
    renderHalfCard(newD - '0', false, flapTmp);
    memcpy(&cardSram[seamY * CARD_W],       flapTmp, CARD_W * half * 2);                      // 新下半
    buildFlapBand(oldD - '0', false, bandH, p, true);
    memcpy(&cardSram[(seamY + half - bandH) * CARD_W], bandBuf, CARD_W * bandH * 2);         // 旧下半翻片带
    lineBufCard(1, seamY + half - bandH, w - 2, FLAP_BAND_LINE);
  }
  // 金属铰链 (�?上层, 必须在半牌之后画, 否则�?数字盖�?? �?preview _draw_hinge �?�?
  hingeBufCard(seamY, w);
  tft.pushImage(x, y, w, CARD_SRAM_H, cardSram);   // 原子�? 整牌�?次推�?
}
// 间隙 3D 滚动齿轮 (16x120 圆柱, 齿纹�?scroll 下移, �?preview draw_roller �?�?
// 稳态静态牌 (整牌: 投影+白底+完整数字+铰链) 合成进 cardSram 再*单次* pushImage (原子帧)。
// 与 drawCard 逐像素一致, 但走内部 RAM 原子推屏。用于翻页"完成"瞬间: 若用 drawCard(多笔直写屏:
// 白底填充 -> 数字 push -> 画铰链) 会在"擦白 -> 画数字"之间被面板扫到 => 用户看到"每翻一次闪一下"。
static void drawCardStaticAtomic(int idx, char digit) {
  int x = cardX[idx], y = CARD_TOP, w = CARD_W, h = CARD_H;
  int half = h / 2, seamY = half;
  for (int i = 0; i < CARD_W * CARD_SRAM_H; i++) cardSram[i] = COLOR_BG;
  fillRoundRectCard(0, CARD_SHAD_ROW, w, h, CARD_RADIUS, CARD_EDGE);   // 下缘投影 (先画, 让牌压在它上面)
  fillRoundRectCard(0, 0, w, h, CARD_RADIUS, CARD_FACE);               // 白底牌面
  renderHalfCard(digit - '0', true,  flapTmp);
  memcpy(&cardSram[0],              flapTmp, CARD_W * half * 2);       // 完整数字上半
  renderHalfCard(digit - '0', false, flapTmp);
  memcpy(&cardSram[seamY * CARD_W], flapTmp, CARD_W * half * 2);       // 完整数字下半
  hingeBufCard(seamY, w);                                               // 铰链 (最上层, 必须在数字之后画)
  tft.pushImage(x, y, w, CARD_SRAM_H, cardSram);   // 原子帧: 整牌一次推 (消除"每翻一次闪一下")
}
static void drawRoller(int cx, float scroll) {
  if (!rollerSram) return;   // BUILD17: guard (alloc failed)
  const int x0 = cx - ROLLER_W / 2, H = CARD_H;
  float phase = fmodf(scroll, (float)ROLLER_PITCH);
  if (phase < 0) phase += ROLLER_PITCH;
  for (int yy = 0; yy < H; yy++) {
    float ly = fmodf((float)yy + phase, (float)ROLLER_PITCH);
    float th = (ly / ROLLER_PITCH) * 2.0f * (float)M_PI;
    float tf = 0.62f + 0.38f * cosf(th) - 0.08f * cosf(2.0f * th);   // �?形波�?
    for (int x = 0; x < ROLLER_W; x++) {
      float v = rollerColB[x] * tf; if (v > 1.0f) v = 1.0f;
      int c = (int)(24 + 210.0f * v); if (c < 0) c = 0; if (c > 255) c = 255;
      int cb = c + 8; if (cb > 255) cb = 255;
      rollerSram[yy * ROLLER_W + x] = (((c * 31) / 255) << 11) | (((c * 63) / 255) << 5) | ((cb * 31) / 255);
    }
  }
  uint16_t edge = (((40 * 31) / 255) << 11) | (((40 * 63) / 255) << 5) | ((52 * 31) / 255);  // (40,40,52)
  for (int yy = 0; yy < H; yy++) {
    rollerSram[yy * ROLLER_W + 0] = edge;
    rollerSram[yy * ROLLER_W + ROLLER_W - 1] = edge;
  }
  for (int x = 0; x < ROLLER_W; x++) {                          // 上下�?�?
    float t = (float)x / (ROLLER_W - 1);
    int c = (int)(40 + 150.0f * sinf(t * (float)M_PI));
    int cr = c - 24, cg = c - 24, cbb = c - 16;
    if (cr < 0) cr = 0; if (cg < 0) cg = 0; if (cbb < 0) cbb = 0;
    uint16_t cap = (((cr * 31) / 255) << 11) | (((cg * 63) / 255) << 5) | ((cbb * 31) / 255);
    rollerSram[0 * ROLLER_W + x] = cap;
    rollerSram[(H - 1) * ROLLER_W + x] = cap;
  }
  tft.pushImage(x0, CARD_TOP, ROLLER_W, H, rollerSram);   // 内部 RAM, 原子�?(消除齿轮滚动撕�??)
}

// ---- 齿轮带渲�?(合成�?bandSram, 不直写屏�? ----
static void bandFillRoundRect(int ox, int oy, int w, int h, int r, uint16_t col) {
  for (int py = 0; py < h; py++) {
    int by = oy + py; if (by < 0 || by >= CARD_SRAM_H) continue;
    for (int px = 0; px < w; px++) {
      int bx = ox + px; if (bx < 0 || bx >= BAND_W) continue;
      if (flapInRoundRect(px, py, w, h, r)) bandSram[by * BAND_W + bx] = col;
    }
  }
}
// 静�?�牌 (投影+白底+数字+铰链+�?) 渲染�?bandSram �?�?x (�?drawCard 同�?�几�?)
static void bandCard(int idx, char digit) {
  int lx = cardX[idx] - BAND_X0;
  int w = CARD_W, h = CARD_H, seamY = h / 2;
  bandFillRoundRect(lx, CARD_SHAD_ROW, w, h, CARD_RADIUS, CARD_EDGE);
  bandFillRoundRect(lx, 0, w, h, CARD_RADIUS, CARD_FACE);
  const uint8_t *mask = DigitMasks[digit - '0'];
  uint16_t bg = CARD_FACE;
  int r5 = bg >> 11, g6 = (bg >> 5) & 0x3F, b5 = bg & 0x1F;
  int nx = lx + (w - DIGIT_W) / 2, ny = (h - DIGIT_H) / 2;
  for (int dy = 0; dy < DIGIT_H; dy++)
    for (int dx = 0; dx < DIGIT_W; dx++) {
      uint8_t m = pgm_read_byte(&mask[dy * DIGIT_W + dx]);
      bandSram[(ny + dy) * BAND_W + (nx + dx)] = (((r5 * m) / 255) << 11) | (((g6 * m) / 255) << 5) | ((b5 * m) / 255);
    }
  // 铰链 (1px 细缝 + 两端 4x6 金属转轴帽; 与 drawHingeCard/hingeBufCard 一致)
  for (int px = 4; px <= w - 5; px++) bandSram[seamY * BAND_W + lx + px] = CARD_SEAM;   // 中央 1px 细缝 (两端转轴帽之间)
  {
    const uint16_t kc[6] = { (uint16_t)HINGE_HI, (uint16_t)HINGE_HI, (uint16_t)HINGE_MID, (uint16_t)HINGE_MID, (uint16_t)HINGE_LO, (uint16_t)HINGE_LO };
    for (int r = 0; r < 6; r++)
      for (int px = 0; px < 4; px++) {
        bandSram[(seamY - 3 + r) * BAND_W + lx + px] = kc[r];              // 左转轴帽
        bandSram[(seamY - 3 + r) * BAND_W + lx + w - 4 + px] = kc[r];      // 右转轴帽
      }
  }
}
// �?枚齿�?(3D 圆柱 + 齿纹 + �?�?) 渲染�?bandSram �?�?x (�?drawRoller 同�??)
static void bandRoller(int cx, float scroll) {
  int lx = cx - ROLLER_W / 2 - BAND_X0;
  int H = CARD_H;
  float phase = fmodf(scroll, (float)ROLLER_PITCH);
  if (phase < 0) phase += ROLLER_PITCH;
  for (int yy = 0; yy < H; yy++) {
    float ly = fmodf((float)yy + phase, (float)ROLLER_PITCH);
    float th = (ly / ROLLER_PITCH) * 2.0f * (float)M_PI;
    float tf = 0.62f + 0.38f * cosf(th) - 0.08f * cosf(2.0f * th);
    for (int x = 0; x < ROLLER_W; x++) {
      float v = rollerColB[x] * tf; if (v > 1.0f) v = 1.0f;
      int c = (int)(24 + 210.0f * v); if (c < 0) c = 0; if (c > 255) c = 255;
      int cb = c + 8; if (cb > 255) cb = 255;
      bandSram[yy * BAND_W + lx + x] = (((c * 31) / 255) << 11) | (((c * 63) / 255) << 5) | ((cb * 31) / 255);
    }
  }
  uint16_t edge = (((40 * 31) / 255) << 11) | (((40 * 63) / 255) << 5) | ((52 * 31) / 255);
  for (int yy = 0; yy < H; yy++) { bandSram[yy * BAND_W + lx] = edge; bandSram[yy * BAND_W + lx + ROLLER_W - 1] = edge; }
  for (int x = 0; x < ROLLER_W; x++) {
    float t = (float)x / (ROLLER_W - 1);
    int c = (int)(40 + 150.0f * sinf(t * (float)M_PI));
    int cr = c - 24, cg = c - 24, cbb = c - 16;
    if (cr < 0) cr = 0; if (cg < 0) cg = 0; if (cbb < 0) cbb = 0;
    uint16_t cap = (((cr * 31) / 255) << 11) | (((cg * 63) / 255) << 5) | ((cbb * 31) / 255);
    bandSram[0 * BAND_W + lx + x] = cap;
    bandSram[(H - 1) * BAND_W + lx + x] = cap;
  }
}
// 两枚齿轮 + �?间两张�??(2,3) 合成�?bandSram, 单�?? pushImage 整帧 (原子�?�?
// 仅当�?,3 不在翻页时调�?(翻页时由 drawFlapFrame 单独渲染, 避免覆盖其动�?�?
static void drawRollerBand(float scroll) {
  if (!bandSram || !cardSram || !rollerSram) return;   // BUILD17: guard (alloc failed)
  for (int i = 0; i < BAND_W * CARD_SRAM_H; i++) bandSram[i] = COLOR_BG;
  bandCard(2, fstate[2].shown);
  bandCard(3, fstate[3].shown);
  bandRoller(GAP1_CX, scroll);
  bandRoller(GAP2_CX, scroll);
  tft.pushImage(BAND_X0, CARD_TOP, BAND_W, CARD_SRAM_H, bandSram);   // 原子�? 两齿�?同相�?
}

// 每帧驱动: 6 位数字两相翻�?+ 齿轮滚动量累�?(�?preview live_preview.Sim.tick �?�?
static float rollLast = -1e6f;
static void tickFlapClock(const char *target) {
  if (!cardSram || !rollerSram || !bandBuf || !flapTmp) return;   // CLKFIX 2026-10-16: bandSram 缺失=降级(齿轮带空白, 数字翻页正常), 不再整体不翻页
  uint32_t now = millis();
  float scroll = 0.0f;
  for (int i = 0; i < 6; i++) {
    FlipState &fs = fstate[i];
    char newD = target[i];
    if (!fs.flipping && newD != fs.shown) {          // �?动翻�? (�?�? != 当前显示)
      fs.flipping = true; fs.newD = newD; fs.startMs = now; fs.clicks = 0; fs.redrawn = false;
    }
    if (!fs.flipping) {
      scroll += fs.clicks * ROLLER_PITCH;
      if (!fs.redrawn) { drawCard(i, fs.shown, false); fs.redrawn = true; }   // 稳�?�才重绘
      continue;
    }
    uint32_t el = now - fs.startMs;
    int phase; float p;
    if (el < (uint32_t)FLIP_TOP_MS) {
      phase = 1; p = (float)el / FLIP_TOP_MS;
    } else if (el < (uint32_t)(FLIP_TOP_MS + FLIP_BOT_MS)) {
      if (fs.clicks < 1) fs.clicks = 1;
      phase = 2; p = (float)(el - FLIP_TOP_MS) / FLIP_BOT_MS;
    } else {
      if (fs.clicks < 2) fs.clicks = 2;
      fs.shown = fs.newD; fs.flipping = false; fs.redrawn = false;   // 翻页完成
      scroll += fs.clicks * ROLLER_PITCH;
      drawCardStaticAtomic(i, fs.shown); fs.redrawn = true;   // 原子帧(内部 RAM 单次 push), 消除"每翻一次闪一下"
      continue;
    }
    scroll += fs.clicks * ROLLER_PITCH + flapEase(p) * ROLLER_PITCH;
    drawFlapFrame(i, fs.shown, fs.newD, phase, flapEase(p));
  }
  if (fabsf(scroll - rollLast) > 0.01f) {            // 滚动量变化才重绘齿轮
    if (!fstate[2].flipping && !fstate[3].flipping)
      drawRollerBand(scroll);        // 两齿�?�?间两�? �?次原子推�?(消除右齿�?顶�?�彩色撕�?
    else {
      drawRoller(GAP1_CX, scroll);   // �?/3 翻页�? �?回两齿轮�?立�??(避免覆盖翻页动画)
      drawRoller(GAP2_CX, scroll);
    }
    rollLast = scroll;
  }
}

// ========== 时钟样式实现 (0=机�?�翻�? 1=模拟指针) ==========
uint8_t getClockStyle() {
  uint8_t v = STYLE_FLAP;
  nvs_handle_t h;
  if (nvs_open("clockcfg", NVS_READONLY, &h) == ESP_OK) {
    nvs_get_u8(h, "style", &v);
    nvs_close(h);
  }
  if (v > STYLE_ANALOG) v = STYLE_FLAP;
  return v;
}
void saveClockStyle(uint8_t v) {
  nvs_handle_t h;
  if (nvs_open("clockcfg", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u8(h, "style", v);
    nvs_commit(h);
    nvs_close(h);
  }
}

// 顶右�? 当前时钟样式 + 操作提示 (黑底白字小字) �?�?DEBUG_UI 时显�?
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

// 模拟指针表盘 —�??白色表盘 + 黑指�?刻度/数字 + 红�?��??(�?make_preview.py 完全�?�?�?
// 数字用与翻页时钟同�?? Bahnschrift 位图�?�? (dialnum.h), 抗锯齿平�? 无�?�圈描边�?
static const uint16_t AC_FACE  = 0xFFFF;   // 表盘 (�?
static const uint16_t AC_TICK1 = 0x0000;   // 时标 (�?
static const uint16_t AC_TICK2 = 0x0000;   // 分标 (�?
static const uint16_t AC_H     = 0x0000;   // 时针 (�?
static const uint16_t AC_M     = 0x0000;   // 分针 (�?
static const uint16_t AC_S     = 0xF800;   // 秒针 (�?
static const uint16_t AC_HUB   = 0xF800;   // �?心枢�? (�?
static const uint16_t AC_NUM   = 0x0000;   // 数字 (�?

// 数字位图指针 (顺序�?dialnum.h: [0]="12",[1..9]="1".."9",[10]="10",[11]="11")
static const uint8_t *const DialNums[12] = {
  DialNum0, DialNum1, DialNum2, DialNum3, DialNum4, DialNum5, DialNum6,
  DialNum7, DialNum8, DialNum9, DialNum10, DialNum11
};

// ---- 指针表盘: 静�?�表�?(白圆+刻度+12数字) �?次�?�烘焙进内部 RAM 缓冲 ----
// 之后每帧�?tft.pushImage 这块缓冲 + 直渲 3 根指�?(�?drawAnalogClock 注释)�?
static void drawLineBuf(uint16_t *buf, int W, int x0, int y0, int x1, int y1, uint16_t col) {
  int dx = abs(x1 - x0), dy = -abs(y1 - y0);
  int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
  for (;;) {
    if (x0 >= 0 && y0 >= 0 && x0 < W && y0 < W) buf[y0 * W + x0] = col;
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}
// 实心 2px 线写进缓冲: 对中心线(Bresenham)逐像素画 3x3 实心方块. 任何方向都连续实心,
// 不会像"3 条 1px 平行线 offset 0/±1.5"那样在 ~45° 方向 (7~8 点/1~2 点) 因 (int) 截断
// 漏 1 行而对角出现"双线/白缝". 与 preview d.line(..., width=2) 的粗线目标一致.
static void thickLineBuf(uint16_t *buf, int W, int x0, int y0, int x1, int y1, uint16_t col) {
  int dx = abs(x1 - x0), dy = -abs(y1 - y0);
  int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
  for (;;) {
    for (int ox = -1; ox <= 1; ox++) for (int oy = -1; oy <= 1; oy++) {
      int px = x0 + ox, py = y0 + oy;
      if (px >= 0 && py >= 0 && px < W && py < W) buf[py * W + px] = col;
    }
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}
// 实心圆写进缓�?(带边界�?��??
static void fillCircleBuf(uint16_t *buf, int W, int H, int cx, int cy, int r, uint16_t col) {
  for (int y = -r; y <= r; y++) {
    int yy = cy + y; if (yy < 0 || yy >= H) continue;
    int hw = (int)(sqrt((double)(r * r - y * y)));
    for (int x = cx - hw; x <= cx + hw; x++) {
      if (x < 0 || x >= W) continue;
      buf[yy * W + x] = col;
    }
  }
}
// 实心三�?�形写进缓�??(边�?�号测�?? �?边�??-> 无内部缝�? 带边界�?��??
static void fillTriangleBuf(uint16_t *buf, int W, int H,
                            int x0, int y0, int x1, int y1, int x2, int y2, uint16_t col) {
  auto E = [](int ax, int ay, int bx, int by, int px, int py) {
    return (long)(bx - ax) * (py - ay) - (long)(by - ay) * (px - ax);
  };
  int minx = x0, maxx = x0, miny = y0, maxy = y0;
  auto upd = [&](int x, int y) { if (x < minx) minx = x; if (x > maxx) maxx = x; if (y < miny) miny = y; if (y > maxy) maxy = y; };
  upd(x1, y1); upd(x2, y2);
  minx = max(minx, 0); miny = max(miny, 0); maxx = min(maxx, W - 1); maxy = min(maxy, H - 1);
  for (int y = miny; y <= maxy; y++)
    for (int x = minx; x <= maxx; x++) {
      long e1 = E(x0, y0, x1, y1, x, y);
      long e2 = E(x1, y1, x2, y2, x, y);
      long e3 = E(x2, y2, x0, y0, x, y);
      if ((e1 >= 0 && e2 >= 0 && e3 >= 0) || (e1 <= 0 && e2 <= 0 && e3 <= 0))
        buf[y * W + x] = col;
    }
}
static void blitDialNumBuf(uint16_t *buf, int W, int cx, int cy, int idx, uint16_t face) {
  const uint8_t *mask = DialNums[idx];
  int w = pgm_read_byte(&DialNumW[idx]);
  int h = pgm_read_byte(&DialNumH[idx]);
  uint16_t r5 = face >> 11, g6 = (face >> 5) & 0x3F, b5 = face & 0x1F;
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
    int px = cx - w / 2 + x, py = cy - h / 2 + y;
    if (px < 0 || py < 0 || px >= W || py >= W) continue;
    uint8_t m = pgm_read_byte(&mask[y * w + x]);
    buf[py * W + px] = (((r5 * m) / 255) << 11) | (((g6 * m) / 255) << 5) | ((b5 * m) / 255);
  }
}
static void buildAnalogFace(int cx, int cy, int R) {
  const int S = 2 * R;
  for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
    int dx = x - R, dy = y - R;
    analogFace[y * S + x] = (dx * dx + dy * dy <= R * R) ? AC_FACE : COLOR_BG;  // 白圆�?+ 页底
  }
  for (int i = 0; i < 60; i++) {           // 60 分标 + 12 时标
    double a = M_PI * 2.0 * i / 60.0 - M_PI / 2.0;
    bool major = (i % 5 == 0);
    double ro = R - 2, ri = R - (major ? 10 : 5);
    drawLineBuf(analogFace, S, (int)(R + cosf(a) * ri), (int)(R + sinf(a) * ri),
                (int)(R + cosf(a) * ro), (int)(R + sinf(a) * ro), major ? AC_TICK1 : AC_TICK2);
  }
  for (int k = 1; k <= 12; k++) {          // 12 数字 (位图�?�?, �?心落在半�? R-20 �? �?preview �?�?
    double an = M_PI * 2.0 * k / 12.0 - M_PI / 2.0;
    int idx = (k == 12) ? 0 : k;           // 12->0, 10->10, 11->11, 1..9 -> 1..9
    blitDialNumBuf(analogFace, S, (int)(R + cosf(an) * (R - 20)), (int)(R + sinf(an) * (R - 20)), idx, AC_FACE);
  }
}

static void drawAnalogClock(int h, int m, int s) {
  if (!analogFace || !analogFaceCache) return;   // BUILD17: guard (alloc failed)
  unsigned long nowMs = millis();
  int cx = 240, cy = 108, R = 73;   // 指针表盘放大 (�?~32 / �?~184, 不压日期�?
  double t2h = h + m / 60.0 + s / 3600.0;
  double t2m = m + s / 60.0;
  // 秒针平滑扫秒: 整数秒用 NTP s (绝对位置正确, 与翻页秒个位一致), 分数部分用 millis (单一
  // 单调时钟, 每帧匀速 +6°/s 无跳变)。关键: 在每个 NTP 秒边界把"秒起点"锚到 millis,
  // 分数 = (nowMs - 锚点)/1000 —— 秒针既平滑又指向正确秒位, 与 live_preview 效果一致。
  // 旧写法 (s + (nowMs%1000)/1000): NTP 秒与 millis 相位不锁, 每"秒"回跳 => "前后摇摆"。
  // 纯 millis (nowMs%60000): 平滑但相对真实秒有固定相位偏移(从开机计), 指向错误秒位。
  static unsigned long lastSec = 0xFFFFFFFFUL, secAnchorMs = 0;
  if (s != (int)lastSec) { lastSec = (unsigned long)s; secAnchorMs = nowMs; }   // NTP 秒变了: 锚定该秒起点
  double t2s = (s + (nowMs - secAnchorMs) / 1000.0) / 60.0;

  // 每帧: �?(静�?�表�?白圆+刻度+12数字) + (3 根指�? 合成进�?�同�?块�?�内�?RAM 缓冲, �?
  // 【单次�?�pushImage 整帧。关�?整帧�?次推�?原子�? �?TE 同�?�时面板持续�?�? �?
  // "推表�?擦掉�? -> 再分几笔画针"会有�?间露针空�? -> 3 针闪�?+ 秒针像不动�??
  // 静�?�表盘只烘焙�?�?(analogFaceCache), 每帧 memcpy 复用: buildAnalogFace 全表重建
  // �?�?~50-100ms, 若每帧都重建会把实际 FPS 打到 <10, 秒针每帧推进 <0.1px �?视�??"冻结"�?
  // memcpy 42.6KB �?~1ms, 实际 FPS 回到 ~30 (ANALOG_FRAME_MS 门控), 秒针 6°/s 平滑�?秒�??
  if (!analogFaceBuilt) { buildAnalogFace(cx, cy, R); memcpy(analogFaceCache, analogFace, 146 * 146 * sizeof(uint16_t)); analogFaceBuilt = true; }
  memcpy(analogFace, analogFaceCache, 146 * 146 * sizeof(uint16_t));   // 还原静�?�表�?(清掉上一帧指�?
  int S = 2 * R, lcx = R, lcy = R;             // 缓冲内中�?= (73,73), 对应屏幕 (cx,cy)
  // 时针 / 分针 (棒�?�指�?: �?段最宽�?�向两�??渐细的�?�边�? + 圆头�? 画进缓冲)
  auto hand = [&](double ang, int len, int w, int tail, uint16_t col) {
    double a = M_PI * 2.0 * ang - M_PI / 2.0;
    float dx = cosf(a), dy = sinf(a);
    float ux = -sinf(a), uy = cosf(a);
    float hb = w / 2.0f;                 // �?段半�?
    float ht = fmaxf(1.0f, w * 0.30f);   // 两�??半�??
    float capD = len - ht;               // 圆帽起点(距中�?
    // 多边形顶�? 尾上A -> �?段左C -> 尖上E -> 尖下F -> �?段右D -> 尾下B
    int Ax = (int)(lcx + dx * (-tail) + ux * ht), Ay = (int)(lcy + dy * (-tail) + uy * ht);
    int Cx = (int)(lcx + ux * hb),          Cy = (int)(lcy + uy * hb);
    int Ex = (int)(lcx + dx * capD + ux * ht), Ey = (int)(lcy + dy * capD + uy * ht);
    int Fx = (int)(lcx + dx * capD - ux * ht), Fy = (int)(lcy + dy * capD - uy * ht);
    int Dx = (int)(lcx - ux * hb),          Dy = (int)(lcy - uy * hb);
    int Bx = (int)(lcx + dx * (-tail) - ux * ht), By = (int)(lcy + dy * (-tail) - uy * ht);
    fillTriangleBuf(analogFace, S, S, Ax, Ay, Cx, Cy, Ex, Ey, col);
    fillTriangleBuf(analogFace, S, S, Ax, Ay, Ex, Ey, Fx, Fy, col);
    fillTriangleBuf(analogFace, S, S, Ax, Ay, Fx, Fy, Dx, Dy, col);
    fillTriangleBuf(analogFace, S, S, Ax, Ay, Dx, Dy, Bx, By, col);
    // 尖�??圆帽
    fillCircleBuf(analogFace, S, S, (int)(lcx + dx * capD), (int)(lcy + dy * capD), (int)ht + 1, col);
  };
  hand(t2h / 12.0, 40, 4, 11, AC_H);
  hand(t2m / 60.0, 55, 3, 14, AC_M);
  // 秒针 (细线 + 尾�??配重�? 平滑�?�?)
  double sa = M_PI * 2.0 * t2s - M_PI / 2.0;
  int sex = lcx + (int)(cosf(sa) * 60), sey = lcy + (int)(sinf(sa) * 60);   // 先乘后取整 (旧写法 (int)cosf*60 截断后*60 => 秒针塌到中心看不见)
  // 秒针加粗到 ~2px (1px 在 4.0" 屏上看不清).
  // 旧写法用 3 条 1px 平行线 (垂向 offset 0/±1.5): 在 ~45° 方向 (7~8 点 / 1~2 点) offset 经
  // (int) 截断后垂直位移 ≈ 2px, 而 45° 相邻对角线行只相距 1px -> 中间漏 1 行 -> 出现"双线/白缝"
  // (角相关, 水平/垂直方向不出现). 改用 thickLineBuf (中心线逐像素 3x3 实心方块): 任何角度都
  // 连续实心, 与 preview d.line(width=2) 一致.
  thickLineBuf(analogFace, S, lcx, lcy, sex, sey, AC_S);
  fillCircleBuf(analogFace, S, S, lcx - (int)(cosf(sa) * 15), lcy - (int)(sinf(sa) * 15), 3, AC_S);
  // �?心枢�?
  fillCircleBuf(analogFace, S, S, lcx, lcy, 5, AC_HUB);
  fillCircleBuf(analogFace, S, S, lcx, lcy, 2, AC_FACE);

  // 整帧�?次推�?(原子�? 无中间露�?撕�??): 内部 RAM �? swapBytes=true, �?drawCard 同�??
  tft.pushImage(cx - R, cy - R, 2 * R, 2 * R, analogFace);

  drawModeHint(STYLE_ANALOG);
}

// 月相图标: 暗底圆盘 + �?�? (上弦�?�? / 下弦�?�?), �?暗交界为内缩�?�?黄经�?
void drawMoonIcon(int cx, int cy, int R, double phase) {
  double cosP = cos(2.0 * M_PI * phase);
  tft.fillCircle(cx, cy, R, MOON_DARK);
  for (int yy = -R; yy <= R; yy++) {
    double hw = sqrt((double)(R * R - yy * yy));   // 该�?�的圆盘半�??
    double xl, xr;
    if (phase <= 0.5) {                             // 上弦: �?部在�?
      xl = cosP * hw; if (xl < -hw) xl = -hw;
      xr = hw;
    } else {                                          // 下弦: �?部在�?
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

// 日期�? 两�?�布�?
//   �?: �?�?+星期 | 农历+干支
//   �?: 节气(当前/下一+天数) | 月相(图标+名称)
static void drawDateRow(int year, int mon, int mday, int wday) {
  if (!font16.isReady()) return;
  int x0 = DATE_LEFT, x1 = DATE_RIGHT;
  // 行1 分隔线 + 右半块(农历/干支/今日XX) 整体左移到左半块(日期+星期, 恒止于 x≈193)右侧空白处,
  // 而非居中(旧 (x0+x1)/2=240): 否则长节日名(如 "今日中秋节"=80px)会溢出圆角背景右缘(DATE_RIGHT=449)
  // 约 23px。移到 x=200 后, 右半块左移到 212, 右缘约 444, 全部落在背景内。与 make_preview.py 一致。
  int cx = 200;                 // �? 分隔�?x

  // 清场: 先把两�?�日期条整�??�?左右溢出�?)擦成背景色�??
  // 否则上一次更长的文字(�?1970-01-01 �?"今日元旦")会溢出圆角矩�?
  // 右缘 (x>DATE_RIGHT=449), 跨日重绘时右缘�?��??"�? 擦不�?-> 残影�?
  tft.fillRect(0, ROW1_TOP, 480, (ROW2_TOP + ROW2_H) - ROW1_TOP, COLOR_BG);

  // 农历 + 干支 (春节前取前一�? 与�?��?�一�?
  int ld[31]; nl_month_days(year, mon, ld);
  int v = ld[mday - 1];
  int lm = v / 100, ldn = v % 100;
  bool isLeap = (lm > 100); if (isLeap) lm -= 100;
  int ly = nl_zodiac_year(year, mon, mday);   // 干支/生肖�?(按春节分�? �?正农�?11/12月�??�?
  char lunar[32];
  snprintf(lunar, sizeof(lunar), "%s月%s%s", lunarMonths[lm - 1], lunarDays[ldn - 1],
           isLeap ? "(闰)" : "");
  char gz[16];
  snprintf(gz, sizeof(gz), "%s%s%s年", nl_tg_text[nl_tg(ly)], nl_dz_text[nl_dz(ly)], nl_sx_text[nl_dz(ly)]);
  char fest[16] = "";
  nl_festival(year, mon, mday, ly, lm, ldn, isLeap ? 1 : 0, fest, sizeof(fest));

  // 节气 (当前 + 下一, �?天�??
  int jqCur = -1, jqCurD = 0, jqNxt = -1, jqNxtD = 0;
  nl_jq_info(year, mon, mday, &jqCur, &jqCurD, &jqNxt, &jqNxtD);

  // 月相
  double mp = nl_moon_phase(year, mon, mday);

  // ---- �?: �?�?+星期 | 农历+干支+节日 ----
  int y1 = ROW1_TOP, h1 = ROW1_H, cy1 = y1 + h1 / 2;
  tft.fillRoundRect(x0, y1, x1 - x0, h1, 8, ROW1_BG);
  // �?: 日期(�? + 星期(�? �?preview �?致分�?绘制) | 农历(�? + 干支(�? + 节日
  // AA 字库: 逐字 advance + 抗锯齿合�? bg 传所画�?�底�? (ROW1_BG)
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
  if (fest[0]) {  // 当天节日: "今日XX" + 节日�? 紧跟干支之后
    char s[16];
    snprintf(s, sizeof(s), "今日%s", fest);
    font16.drawStringFast(gx + font16.textWidth(gz) + 6, cy1 - 8, s, eventColor(fest), ROW1_BG);
  }

  // ---- �?: 节气 | 月相 ----
  int y2 = ROW2_TOP, h2 = ROW2_H, cy2 = y2 + h2 / 2;
  tft.fillRoundRect(x0, y2, x1 - x0, h2, 8, DATE_STRIP);
  int tx = x0 + 12;
  if (jqCur >= 0 && jqNxt >= 0) {
    if (jqCurD == 0) {
      // 节气当天: "今日XX" + 节日�? 后接下一节气 (灰蓝)
      char s[16];
      snprintf(s, sizeof(s), "今日%s", nl_jq_text[jqCur]);
      font16.drawStringFast(tx, cy2 - 8, s, eventColor(nl_jq_text[jqCur]), DATE_STRIP);
      tx += font16.textWidth(s) + 10;
      char rest[24];
      snprintf(rest, sizeof(rest), "%s %d天后", nl_jq_text[jqNxt], jqNxtD);
      font16.drawStringFast(tx, cy2 - 8, rest, SOFT_C, DATE_STRIP);
    } else {
      char jqLine[48];
      snprintf(jqLine, sizeof(jqLine), "%s 已过%d天  %s %d天后",
               nl_jq_text[jqCur], jqCurD, nl_jq_text[jqNxt], jqNxtD);
      font16.drawStringFast(tx, cy2 - 8, jqLine, SOFT_C, DATE_STRIP);
    }
  } else {
    font16.drawStringFast(tx, cy2 - 8, "--", SOFT_C, DATE_STRIP);
  }
  // 右侧: 湿度 | 温度 | 月相�?| 月相图标 (�?右向�?, �?make_preview.py draw_date �?�?
  int mi = (int)(mp * 8.0 + 0.5) % 8;
  char hms[8], tms[8];
  snprintf(hms, sizeof(hms), "%d%%", mockHumidity);
  snprintf(tms, sizeof(tms), "%d°C", mockTempC);
  int hR = x1 - 12;                                   // 湿度右缘
  font16.drawStringFast(hR - font16.textWidth(hms), cy2 - 8, hms, humidColor(mockHumidity), DATE_STRIP);
  int tR = hR - font16.textWidth(hms) - 8;           // 温度右缘
  font16.drawStringFast(tR - font16.textWidth(tms), cy2 - 8, tms, tempColor(mockTempC), DATE_STRIP);
  int mR = tR - font16.textWidth(tms) - 8;           // 月相名右�?
  int mTw = font16.textWidth(nl_moon_text[mi]);
  font16.drawStringFast(mR - mTw, cy2 - 8, nl_moon_text[mi], 0xDE72, DATE_STRIP);  // 淡金
  int R = 9;
  int mcx = mR - mTw - 6 - R;
  drawMoonIcon(mcx, cy2, R, mp);
}

// ---- BUILD17: clock render buffers -> heap (lazy alloc on clock entry, free on leave) ----
// The 7 static uint16_t render buffers (~153KB) lived in .bss permanently, capping the internal
// heap's largest contiguous block at ~40KB -- below the mbedTLS HTTPS handshake peak (~48KB), so
// every https:// radio failed at start_ssl_client / "esp-aes: Failed to allocate". Converting them
// to malloc shrinks .bss, expanding the internal heap so the max free block reaches ~200KB in radio
// mode. Only the *active* style's buffers are allocated (the style-mutex pair), so the clock page
// keeps everything it needs and the radio page gets the full heap for TLS. No pixel/animation logic
// changes -- only where the buffers come from.
static bool clockBufsReady = false;   // BUILD17: buffers for the current style are allocated
void allocClockBufs() {
  // CLKFIX-B: 池方案——boot 一次性分配 58.5KB 内部 RAM 池 + PSRAM 辅助缓冲(analogFaceCache/
  // bandBuf/flapTmp)常驻; 样式切换只改池内偏移, 零 malloc/free → 播放期切样式/切页面永远成功。
  if (!clockPool) {
    clockPool = (uint16_t *)heap_caps_malloc(CLOCK_POOL_ELEMS * 2, MALLOC_CAP_INTERNAL);
    Serial.printf("[MEM] clock pool alloc %uB -> %p (internalFree=%uKB maxBlock=%uKB)\n",
                  (unsigned)(CLOCK_POOL_ELEMS * 2), (void*)clockPool,
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                  (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
  }
  // PSRAM 辅助缓冲 boot 常驻（FLAP/ANALOG 共用，样式切换不动）
  if (!analogFaceCache) analogFaceCache = (uint16_t *)heap_caps_malloc(146 * 146 * 2, MALLOC_CAP_SPIRAM);
  if (!bandBuf) bandBuf = (uint16_t *)ps_malloc((size_t)CARD_W * (CARD_H / 2) * 2);
  if (!flapTmp) flapTmp = (uint16_t *)ps_malloc((size_t)CARD_W * (CARD_H / 2) * 2);
  if (!clockPool) {
    clockBufsReady = false;
    Serial.printf("[MEM] clock pool alloc FAILED: internalFree=%uKB maxBlock=%uKB\n",
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                  (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    return;
  }
  // 样式 = 池内布局切换（零分配/释放）
  if (clockStyle == STYLE_FLAP) {
    bandSram   = clockPool + OFF_BAND;                 // 166*123 齿轮带
    cardSram   = clockPool + OFF_CARD;                 // 56*123 整卡
    rollerSram = clockPool + OFF_ROLLER;               // 16*120 齿轮
    analogFace = nullptr;                              // 池被 FLAP 占用, ANALOG 指针逻辑置空(不 free!)
    analogFaceBuilt = false;
    clockBufsReady = (cardSram && rollerSram && bandBuf && flapTmp);
  } else {
    analogFace = clockPool;                            // 146*146 ≤ 池, 用池首部
    bandSram   = nullptr; cardSram = nullptr; rollerSram = nullptr;  // 逻辑置空(不 free!)
    analogFaceBuilt = false;
    clockBufsReady = (analogFace && analogFaceCache);
  }
  Serial.printf("[MEM] allocClockBufs style=%d af=%p afc=%p bs=%p cs=%p rs=%p ready=%d\n",
                clockStyle, (void*)analogFace, (void*)analogFaceCache,
                (void*)bandSram, (void*)cardSram, (void*)rollerSram, clockBufsReady ? 1 : 0);
}
void freeClockBufs() {
  // CLKFIX-B: 缓冲全部常驻(池), 无独立块可释放。保留空实现防其他引用。
  Serial.printf("[MEM] freeClockBufs called (pool resident, no-op) af=%p\n", (void*)analogFace);
}

// BUILDV5-CLKFIX-A (2026-10-17): 时钟缓冲 boot 常驻——根治"播放期切时钟空白"。
// 依据: V5 已全 HTTP 无 TLS, BUILD17"离开时钟页释放缓冲给 TLS handshake"的理由已过时
// (main.cpp loop 的 freeClockBufs() 调用已删)。setup 末尾内存最干净时一次分配、永不释放
// → 播放期切时钟直接用常驻缓冲, 与内部 RAM 碎片(maxBlock 40KB)无关。
// allocClockBufs 本身幂等(判空才分配), 此处只调用 + 打 log(在 setup 单线程阶段, 无并发打印)。
void allocClockBufsResident() {
  allocClockBufs();
  Serial.printf("[MEM] clock buf resident style=%s internalFree=%uKB maxBlock=%uKB ready=%d\n",
                clockStyle == STYLE_FLAP ? "FLAP" : "ANALOG",
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                clockBufsReady ? 1 : 0);
}

void modeClock() {
  time_t now = time(nullptr);
  struct tm tm_info;
  epochToLocalNoLock(now, tm_info);   // BUILDV5-TZFIX: 无锁 UTC+8, 替代 localtime_r (tz锁×跨核中断崩溃)
  int h = tm_info.tm_hour, m = tm_info.tm_min, s = tm_info.tm_sec;
  int year = tm_info.tm_year + 1900;
  int mon = tm_info.tm_mon + 1;
  int mday = tm_info.tm_mday;
  int wday = tm_info.tm_wday;

  // 首�?�进�?: 清场 + 按当前样式绘�?(翻页 6 张牌 / 指针表盘) + 日期�?
  if (!modeDrawn[MODE_CLOCK] || modeChanged) {
    allocClockBufs();   // BUILD17: alloc the active style's clock buffers on clock-page entry (freed in loop() when leaving)
    tft.fillRect(0, 0, 480, NAV_BAR_Y, COLOR_BG);
    if (clockStyle == STYLE_FLAP) {
      char cur[7];
      snprintf(cur, sizeof(cur), "%02d%02d%02d", h, m, s);
      flapInit();                              // 预渲染半�?齿轮 (PSRAM); 失败则�??�?
      for (int i = 0; i < 6; i++) {
        // CLKFIX 2026-10-16: bandSram 缺失(降级)时 i=2/3 也走 drawCard, 避免中间两牌空白（原逻辑依赖齿轮带渲染）
        if (!(flapReady && bandSram && (i == 2 || i == 3))) drawCard(i, cur[i], false);
        fstate[i].flipping = false;
        fstate[i].shown = cur[i]; fstate[i].newD = cur[i];
        fstate[i].clicks = 0; fstate[i].redrawn = true; fstate[i].startMs = 0;
      }
      rollLast = 0.0f;
      if (flapReady) drawRollerBand(0.0f);   // 两齿�?+ �?间两�? (2,3) �?次原子推�?(稳�??
      drawModeHint(STYLE_FLAP);
    } else {
      // CLKFIX 2026-10-17: 仅当 analogFace 缺失(内部 RAM 不足)时首帧才画静态大字时间 fallback。
      // 原实现漏了 if 判断无条件画绿字(0x4FE3) → analogFace 正常时首帧绿字残留,
      // 表盘(146px)只覆盖中心, 左右两侧绿色时间残影残留 = 用户报告"指针时钟页面绿色时间小字"。
      if (!analogFace || !analogFaceCache) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
        int tw = font32.textWidth(buf);
        font32.drawStringFast(240 - tw / 2, 96, buf, 0x4FE3, COLOR_BG);
        Serial.println("[CLK] analog fallback first-frame");
      }
    }
    // 日期�?(农历/干支/节气/月相 均在 drawDateRow 内�?��??
    // 注意: 不画 "NTP OK" �?preview (make_preview.scene / draw_analog_scene) 均不显示,
    // �?件必须与预�?�一�?(预�?��??target)�?
    drawDateRow(year, mon, mday, wday);
    checkFestivalMusic(year, mon, mday);   // 节日/节气音乐 (每天�?�?
    prevH = h; prevM = m; prevS = s;
    modeDrawn[MODE_CLOCK] = true;
    return;
  }

  if (clockStyle == STYLE_ANALOG) {
    if (!analogFace || !analogFaceCache) {
      // CLKFIX 2026-10-16: internal RAM low (analogFace 42.6KB alloc failed) -> degrade to static big text time (no second hand)
      static uint32_t fbkLast = 0;
      if (millis() - fbkLast >= 1000) {
        fbkLast = millis();
        Serial.printf("[CLK] analog fallback tick af=%p afc=%p style=%d\n", (void*)analogFace, (void*)analogFaceCache, clockStyle);
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
        int tw = font32.textWidth(buf);
        font32.drawStringFast(240 - tw / 2, 96, buf, 0x4FE3, COLOR_BG);
      }
    } else {
    // 指针样式: 秒针平滑�?�?, 每帧重绘表盘�?
    // �?ANALOG_FRAME_MS(�?0 FPS) 门控: loop() 无节�? 若每�?~1000+ FPS)都重绘整�?
    // (~40KB) 会把 20MHz SPI 打满 -> 整面�?�?/撕�?��?�限�?~30 FPS, 秒针 6°/s 仍平滑�??
    // 整帧包在 startWrite/endWrite �? 减少 CS 切换�?�?�?
    static uint32_t anaLast = 0;
    uint32_t anow = millis();
    if (anow - anaLast >= ANALOG_FRAME_MS) {
      anaLast = anow;
      tft.startWrite();
      drawAnalogClock(h, m, s);
      tft.endWrite();
    }
    }
  } else {
    // 翻页样式: 驱动两相 split-flap (非阻�? + 齿轮滚动。稳态时几乎不重绘�??
    // �?FLAP_FRAME_MS(=preview TICK_MS=40) 门控: loop() 无节�? 若每�?都调用会�?
    // SPI 打满导致翻页�?�? 限到 ~25 FPS 与�?��?�同帧率。状态机�?millis() 计时,
    // �?限调用�?�率, 不改变翻�?齿轮时序�?
    static uint32_t flapLast = 0;
    uint32_t fnow = millis();
    if (flapReady && (fnow - flapLast >= FLAP_FRAME_MS)) {
      flapLast = fnow;
      char target[7];
      snprintf(target, sizeof(target), "%02d%02d%02d", h, m, s);
      prevH = h; prevM = m; prevS = s;
      // 整帧�?startWrite/endWrite: 每张�?齿轮都是"合成到内�?RAM 缓冲 + 单�?? pushImage
      // 整帧"(原子�?, 在同�? SPI 事务内完�? 消除帧撕�?半旧半新/乱序)与局部闪�?
      tft.startWrite();
      tickFlapClock(target);
      tft.endWrite();
    }
  }

  // 日期变化(跨日/跨月)时重绘日期条
  static int lastDay = -1, lastMon = -1, lastYear = -1;
  if (mday != lastDay || mon != lastMon || year != lastYear) {
    lastDay = mday; lastMon = mon; lastYear = year;
    drawDateRow(year, mon, mday, wday);
    checkFestivalMusic(year, mon, mday);   // 跨日 -> �?查节�?节气音乐
  }
}
