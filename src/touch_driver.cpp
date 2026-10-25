#include "touch_driver.h"
#include "ui_common.h"    // extern TFT_eSPI tft
#include "User_Setup.h"   // TOUCH_CS / SPI_TOUCH_FREQUENCY
#include <Preferences.h>  // BUILDV5-CALIB: NVS 持久化触摸校准
#include "soc/gpio_reg.h"
#include "soc/io_mux_reg.h"

// ============================================================
// BUILDV5-TOUCHDRV: 自写 XPT2046 驱动（绕开 TFT_eSPI 的 FreeRTOS SPI_MUTEX）
// ============================================================

volatile bool     s_touchDown = false;
volatile uint16_t s_touchX = 0;
volatile uint16_t s_touchY = 0;
volatile uint16_t g_touchRawRX = 0; // BUILDV5-CALIB
volatile uint16_t g_touchRawRY = 0;

static SPIClass* s_spi = nullptr;

// BUILDV5-CALIB: 触摸校准参数（convertRawXY rotate 分支语义）
//   flags bit0=rotate(ry→xx/rx→yy) bit1=inv_x bit2=inv_y
//   rotate: xx=(ry-x0)*W/x1 ; yy=(rx-y0)*H/y1
//   no-rot: xx=(rx-x0)*W/x1 ; yy=(ry-y0)*H/y1
// 默认值 = 历史标定 {194,3767,174,3844,0x01}；NVS 有保存值时覆盖。
static uint16_t s_touchCalib[5] = {194, 3767, 174, 3844, 0x01};

void touchDriverInit() {
  if (s_spi) return;                  // 只初始化一次
  s_spi = &tft.getSPIinstance();      // 与显示共用 FSPI 总线（不同 CS），bus lock 互斥
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, HIGH);       // 触摸片选默认释放
  // CALFIX-PENIRQ（2026-10-17，硬件信号方案）：GPIO4 = XPT2046 PENIRQ（按下拉低/抬起拉高）。
  //   2026-10-21：GPIO38 实测物理引脚点击变 0 但芯片 IN1 恒 1 → GPIO38 是 octal PSRAM 的
  //   SPICS1 复用脚，pad 被 PSRAM 控制器钳制 → 换 GPIO4（普通 GPIO，无复用）。
  //   PENIRQ 开漏输出 → 内部上拉。按下/抬起由硬件信号判定（替代 Z2 压力采样），
  //   XY 采样前永远没有 0xC0(Z2) 事务 → 彻底绕开已证实的残留污染链。
  pinMode(TOUCH_IRQ, INPUT_PULLUP);
  // BUILDV5-CALIB: 读取 NVS 校准（校准模式写入，重启后自动生效）
  Preferences pref;
  if (pref.begin("tcal", true)) {
    uint16_t v[5];
    if (pref.getBytes("v", v, sizeof(v)) == sizeof(v)) {
      memcpy(s_touchCalib, v, sizeof(v));
      Serial.printf("[TOUCH] NVS calib {%u,%u,%u,%u,0x%02X}\n",
                    v[0], v[1], v[2], v[3], v[4]);
    }
    pref.end();
  }
}

// CALFIX-PENIRQ: 原始 ADC → 屏幕坐标（抽取自原 touchDriverRead 内联块，PENIRQ 采样与校准共用）
static bool convertRawXY(uint16_t rx, uint16_t ry, uint16_t* scrX, uint16_t* scrY) {
  uint16_t x0 = s_touchCalib[0], x1 = s_touchCalib[1];
  uint16_t y0 = s_touchCalib[2], y1 = s_touchCalib[3];
  uint8_t fl = s_touchCalib[4];
  int32_t a, b;                       // 防下溢用有符号
  if (fl & 0x01) {                    // rotate: ry→屏幕X, rx→屏幕Y
    a = (fl & 0x02) ? (int32_t)(x0 + x1) - ry : (int32_t)ry - x0;   // inv_x
    b = (fl & 0x04) ? (int32_t)(y0 + y1) - rx : (int32_t)rx - y0;   // inv_y
  } else {                            // 直连: rx→屏幕X, ry→屏幕Y
    a = (fl & 0x02) ? (int32_t)(x0 + x1) - rx : (int32_t)rx - x0;   // inv_x
    b = (fl & 0x04) ? (int32_t)(y0 + y1) - ry : (int32_t)ry - y0;   // inv_y
  }
  if (a < 0) a = 0; if (b < 0) b = 0;
  uint16_t xx = (uint16_t)(((uint32_t)a * 480u) / x1);
  uint16_t yy = (uint16_t)(((uint32_t)b * 320u) / y1);
  if (xx >= 480 || yy >= 320) return false;
  *scrX = xx;
  *scrY = yy;
  return true;
}

// 单次 Z1 压力采样（简化自 TFT_eSPI getTouchRawZ，只读一次转换）
// XPT2046：无触摸时 Z 开路 → 4095；触摸时下降（数百~数千）。
// CALFIX-PD 已回退（2026-10-17）：PD=01 假设被 TFT_eSPI 原版源码推翻
//   （.pio/libdeps/s3v5/TFT_eSPI/Extensions/Touch.cpp 原版即用 0xB0/0xD0/0x90
//   PD=00 且稳定工作）。跳变根因另查（两次采样一致性检查缺失，见 touchScanTask）。
static uint16_t touchReadZ() {
  uint8_t cmds[3] = {0xB0, 0x00, 0x00};
  uint8_t rx[3];
  s_spi->beginTransaction(SPISettings(SPI_TOUCH_FREQUENCY, MSBFIRST, SPI_MODE0));
  digitalWrite(TOUCH_CS, LOW);
  for (int i = 0; i < 3; i++) rx[i] = s_spi->transfer(cmds[i]);
  digitalWrite(TOUCH_CS, HIGH);
  s_spi->endTransaction();
  uint16_t z = (uint16_t)((rx[1] << 5) | (0x1F & (rx[2] >> 3)));  // 12-bit Z1
  if (z >= 4000) z = 0;   // 接近开路 = 无触摸
  return z;
}

// BUILDV5-STEP3-TFIX4: 单次 Z2 压力采样（XPT2046 0xC0 命令）。
// TFT_eSPI 标准压力判断 = z1 - z2 差值（远大于单读 z1 的鲁棒性）。
static uint16_t touchReadZ2() {
  uint8_t cmds[3] = {0xC0, 0x00, 0x00};
  uint8_t rx[3];
  s_spi->beginTransaction(SPISettings(SPI_TOUCH_FREQUENCY, MSBFIRST, SPI_MODE0));
  digitalWrite(TOUCH_CS, LOW);
  for (int i = 0; i < 3; i++) rx[i] = s_spi->transfer(cmds[i]);
  digitalWrite(TOUCH_CS, HIGH);
  s_spi->endTransaction();
  uint16_t z = (uint16_t)((rx[1] << 5) | (0x1F & (rx[2] >> 3)));  // 12-bit Z2
  if (z >= 4000) z = 0;
  return z;
}

// 原始 X/Y 采样（逐字节复制 TFT_eSPI getTouchRaw 的 17 字节全双工序列：
// 4 次 X 采样取最后一次 → 4 次 Y 采样取最后一次；0xD0/0x90 与 TFT_eSPI 原版一致）
static void touchReadRawXY(uint16_t* x, uint16_t* y) {
  static const uint8_t cmds[17] = {0xD0,0,0xD0,0,0xD0,0,0xD0,0,0x90,0,0x90,0,0x90,0,0x90,0,0};
  uint8_t rx[17];
  s_spi->beginTransaction(SPISettings(SPI_TOUCH_FREQUENCY, MSBFIRST, SPI_MODE0));
  digitalWrite(TOUCH_CS, LOW);
  for (int i = 0; i < 17; i++) rx[i] = s_spi->transfer(cmds[i]);
  digitalWrite(TOUCH_CS, HIGH);
  s_spi->endTransaction();
  // X: transfer#8 高8位 + transfer#9 低5位；Y: transfer#16 高8位 + transfer#17 低5位
  *x = (uint16_t)((rx[7] << 5) | (0x1F & (rx[8] >> 3)));
  *y = (uint16_t)((rx[15] << 5) | (0x1F & (rx[16] >> 3)));
}

bool touchDriverRead(uint16_t* scrX, uint16_t* scrY, uint16_t* z) {
  if (!s_spi) return false;
  // CALFIX-PENIRQ（2026-10-17，硬件信号方案，取代 SEPXY-P）：
  //   按下/抬起 = GPIO4 PENIRQ 电平（低=按下），不再做 Z1/Z2 压力采样。
  //   采样完成后重读 PENIRQ 确认（转换期间 PENIRQ 可能短暂抖动）。
  bool irqLow = (digitalRead(TOUCH_IRQ) == LOW);
  if (!irqLow) return false;   // 抬起/无触摸

  // MEDIAN-FILTER（2026-10-18）：采样 5 次，去掉最大值和最小值，取中间 3 次的平均值
  // 业界标准做法（野火/战舰/INAIRSPACE）：过滤偶发跳变噪声（如 raw X=3072/0xC00）
  const int N = 5;
  uint16_t xs[N], ys[N];
  for (int i = 0; i < N; i++) {
    touchReadRawXY(&xs[i], &ys[i]);
  }
  // 采样后等待 PENIRQ 恢复 LOW（BUILDUI9-TSM-FIX3, 2026-10-24）。XPT2046 转换期间
  // PENIRQ 被内部禁用（悬空拉高），转换结束恢复需时且可能 >100µs——log 铁证
  // （cap_pin.log）：电平打印按下期间 gpio4=0 正常，但 SPI 采样后 100µs 复核读到 HIGH。
  // 改为轮询等待恢复（Linux ads7846 penirq_recheck 思想），最长 5ms；手指按着则等到
  // LOW 确认有效，真抬起则超时 reject（正确拒绝）。
  uint32_t recheckT0 = micros();
  int irqAfter = 1;
  while ((uint32_t)(micros() - recheckT0) < 5000) {
    irqAfter = digitalRead(TOUCH_IRQ);
    if (irqAfter == LOW) break;
    delayMicroseconds(50);
  }
  if (irqAfter != LOW) {
    Serial.printf("[TS-DBG] sampled 5x but PENIRQ not LOW within 5ms -> reject\n");
    return false;   // 真抬起/卡住 → 无效
  }

  // 排序 X 和 Y（简单冒泡，N=5 足够快）
  for (int i = 0; i < N - 1; i++) {
    for (int j = i + 1; j < N; j++) {
      if (xs[i] > xs[j]) { uint16_t t = xs[i]; xs[i] = xs[j]; xs[j] = t; }
      if (ys[i] > ys[j]) { uint16_t t = ys[i]; ys[i] = ys[j]; ys[j] = t; }
    }
  }
  // 去掉最大值和最小值（xs[0]/ys[0] 和 xs[4]/ys[4]），取中间 3 次平均（xs[1..3]/ys[1..3]）
  uint16_t rx = (uint16_t)((xs[1] + xs[2] + xs[3]) / 3);
  uint16_t ry = (uint16_t)((ys[1] + ys[2] + ys[3]) / 3);
  g_touchRawRX = rx; g_touchRawRY = ry; // BUILDV5-CALIB: 原始值供坐标校准诊断
  if (z) *z = 0;   // PENIRQ 方案无 Z 读数（按下/抬起由硬件信号判定）
  if (!convertRawXY(rx, ry, scrX, scrY)) return false;
  return true;
}

// BUILDUI9-TSM (2026-10-24): 业界状态机消抖（Linux ads7846 同款：PENIRQ 仅作唤醒信号，
// 按下/抬起有效性由"连续 N 次 good 读数"确认——debounce_rep=3，不依赖中断边沿）。
// 放弃 ui9b 的"10ms ISR 抖动合并"（简单时间窗口非业界做法，丢边沿风险）。
static SemaphoreHandle_t s_touchSem = nullptr;

void IRAM_ATTR onTouchIrq() {
  // 中断内只唤醒任务（give 计数信号量），不做任何判定/去抖/SPI（中断上下文安全）
  BaseType_t hp = pdFALSE;
  xSemaphoreGiveFromISR(s_touchSem, &hp);
  portYIELD_FROM_ISR(hp);
}

// 触摸扫描任务（BUILDUI9-TSM v2，2026-10-24）：电平 tick 状态机。
// 铁证（cap_pin.log）：GPIO4 电平读取正常（点击 gpio4=0/松开=1），但中断边沿唤醒
// 不可靠（按下沿 sem 恒 0 → 漏唤醒）+ SPI 转换期间 PENIRQ 禁用导致采样复核撞恢复窗。
// v2 设计：10ms tick 轻量轮询电平（digitalRead 不碰 SPI，无 Z 污染、无阻塞）——
// 按下/抬起各需连续 3 tick 一致（30ms 消抖，Linux debounce 语义）；确认按下后才做
// 一次 SPI 采样（touchDriverRead 内部等待式复核），确认抬起前不碰 SPI。ISR 保留
// 仅作唤醒加速（give 计数信号量），10ms 轮询兜底不漏边沿。
void touchScanTask(void* arg) {
  touchDriverInit();
  if (!s_touchSem) s_touchSem = xSemaphoreCreateCounting(4, 0);
  attachInterrupt(digitalPinToInterrupt(TOUCH_IRQ), onTouchIrq, CHANGE);
  Serial.printf("[PENIRQ] boot initial GPIO4=%d (level-tick state machine)\n", digitalRead(TOUCH_IRQ));

  enum { TS_IDLE, TS_CONFIRM, TS_PRESSED, TS_RELEASING } st = TS_IDLE;
  uint16_t lastOkX = 0, lastOkY = 0;
  uint32_t lastOkAt = 0;
  uint32_t lastProc = 0;
  uint32_t lastMoveAt = 0;
  uint32_t downAt = 0;
  uint32_t releaseSince = 0;

  while (true) {
    // ISR 加速唤醒 or 10ms 兜底 tick（v2-fix 曾改为固定 vTaskDelay(10)+take(0) 清空信号量，
    // 铁证（cap_cal2）：点击无任何 [TSM] 事件——10ms 离散采样对快速轻点不利，按下窗口
    // 短时凑不够 3 个采样点 → 漏按。恢复 ISR 唤醒路径：按下沿 give → 立即密集采样）
    xSemaphoreTake(s_touchSem, pdMS_TO_TICKS(10));
    uint32_t now = millis();
    if ((uint32_t)(now - lastProc) < 5) continue;   // 5ms 节流：PENIRQ 抖动风暴（实测一次按下
                                                    // 766 次 CHANGE 中断）给满信号量会让任务无间隔
                                                    // 连跑；采样间留 5ms 让 PENIRQ 有恢复窗口
    lastProc = now;
    bool irqLow = (digitalRead(TOUCH_IRQ) == LOW); // 电平轮询（无 SPI）
    switch (st) {
      case TS_IDLE:
        if (irqLow) {
          // 按下沿 → 立即做第一次 SPI 采样（等待式复核）
          uint16_t sx = 0, sy = 0, sz = 0;
          if (g_spiMutex) xSemaphoreTake(g_spiMutex, portMAX_DELAY);
          bool ok = touchDriverRead(&sx, &sy, &sz);
          if (g_spiMutex) xSemaphoreGive(g_spiMutex);
          if (ok) {
            lastOkX = sx; lastOkY = sy; lastOkAt = now;
            st = TS_CONFIRM;
          }
        }
        break;

      case TS_CONFIRM:
        if (!irqLow) { st = TS_IDLE; break; }       // 抖动/已抬起 → 丢弃
        if ((uint32_t)(now - lastOkAt) < 2) break;  // 两次采样间隔 ≥2ms
        {
          uint16_t sx = 0, sy = 0, sz = 0;
          if (g_spiMutex) xSemaphoreTake(g_spiMutex, portMAX_DELAY);
          bool ok = touchDriverRead(&sx, &sy, &sz);
          if (g_spiMutex) xSemaphoreGive(g_spiMutex);
          if (!ok) { lastOkAt = now; break; }       // 坏读数刷新重试
          // 两次采样坐标一致（差 < 60）→ 确认按下（业界 ads7846 双采样一致性语义）
          uint16_t dx = (sx > lastOkX) ? (sx - lastOkX) : (lastOkX - sx);
          uint16_t dy = (sy > lastOkY) ? (sy - lastOkY) : (lastOkY - sy);
          if (dx + dy < 60) {
            s_touchDown = true;
            s_touchX = sx; s_touchY = sy;
            downAt = now;
            Serial.printf("[TSM] press(%u,%u) raw(%u,%u) 2x-consistent\n",
                          sx, sy, g_touchRawRX, g_touchRawRY);
            st = TS_PRESSED;
          } else { lastOkX = sx; lastOkY = sy; lastOkAt = now; }  // 不一致→以新点为准重试
        }
        break;

      case TS_PRESSED:
        if (!irqLow) { releaseSince = now; st = TS_RELEASING; }
        else {
          if ((uint32_t)(now - lastMoveAt) >= 20) {   // 拖动：每 20ms 刷新坐标
            lastMoveAt = now;
            uint16_t sx = 0, sy = 0, sz = 0;
            if (g_spiMutex) xSemaphoreTake(g_spiMutex, portMAX_DELAY);
            bool ok = touchDriverRead(&sx, &sy, &sz);
            if (g_spiMutex) xSemaphoreGive(g_spiMutex);
            if (ok) { s_touchX = sx; s_touchY = sy; }
          }
        }
        break;

      case TS_RELEASING:
        if (!irqLow) {
          if ((uint32_t)(now - releaseSince) >= 20) {  // 抬起：PENIRQ 高持续 ≥20ms 才确认
            s_touchDown = false;
            Serial.printf("[TSM] release dur=%lums\n", (unsigned long)(now - downAt));
            st = TS_IDLE;
          }
        } else { st = TS_PRESSED; }                    // 抖动回按 → 回到保持
        break;
    }
  }
}

// BUILDV5-STEP3-TFIX4: 原始触摸数据转储（串口键 'T'）——无触摸基线 vs 触摸对照。
// CALFIX-PENIRQ（2026-10-17）：改用 GPIO38 PENIRQ 判定，删除 Z 采样（污染 XY）。
void touchDiag(int seconds) {
  Serial.println("[DIAG] touch raw dump: keep still 2s, then tap 全国, then tap center");
  uint32_t t0 = millis();
  while (millis() - t0 < (uint32_t)seconds * 1000) {
    if (g_spiMutex) xSemaphoreTake(g_spiMutex, portMAX_DELAY);
    bool irqLow = (digitalRead(TOUCH_IRQ) == LOW);
    uint16_t rx = 0, ry = 0;
    touchReadRawXY(&rx, &ry);   // 无 Z 前置，采样干净
    if (g_spiMutex) xSemaphoreGive(g_spiMutex);
    Serial.printf("[DIAG] irq=%d rx=%u ry=%u\n", irqLow ? 1 : 0, rx, ry);
    delay(400);
  }
  Serial.println("[DIAG] dump done");
}

// ============================================================
// BUILDV5-CALIB: 图形化触摸校准（串口键 'C' 触发，main loop 调用）
//   屏幕依次显示 4 个角目标十字 → 用户按住 ~1s → 采集原始 ADC 平均
//   → 计算 min/max + rotate/invert → NVS 保存 → 立即生效
// ============================================================

// 等待用户在 (tx,ty) 目标处点击并采样（BUILDV5-CALIB2 重写）：
//   按下即开始累积原始 ADC，抬手即完成（≥3 个采样即可）；每次采样画一个
//   绿色进度点（底部），用户看到反馈就知道在采。不再要求"按住 1 秒"。
// CALFIX-PENIRQ（2026-10-17）：按下/抬起判定改用 GPIO38 PENIRQ（低=按下），
//   删除 z2 采样——Z(0xC0) 会污染紧接的 XY(0xD0)（残留 >3ms 铁证）→ 校准原始
//   值不再被污染（此前校准可能在跳变值上拟合）。
static bool calibSampleCorner(uint16_t* outRX, uint16_t* outRY) {
  uint32_t sumX = 0, sumY = 0;
  int n = 0;
  bool wasDown = false;
  uint32_t t0 = millis();
  while (millis() - t0 < 15000) {
    if (g_spiMutex) xSemaphoreTake(g_spiMutex, portMAX_DELAY);
    bool irqLow = (digitalRead(TOUCH_IRQ) == LOW);   // PENIRQ 低 = 按下
    if (irqLow) {
      uint16_t rx = 0, ry = 0;
      touchReadRawXY(&rx, &ry);                       // 无 Z 前置，采样干净
      sumX += rx; sumY += ry;
      n++;
      wasDown = true;
      if (n <= 20) tft.fillCircle(30 + n * 20, 300, 5, TFT_GREEN); // 进度点
    }
    if (g_spiMutex) xSemaphoreGive(g_spiMutex);
    if (wasDown && !irqLow) break;   // 按过且已抬手 → 完成本次点击
    if (n >= 20) break;
    delay(5);
  }
  // BUILDV5-CALIB4: 无论以何种方式完成，等手指完全抬起（≤800ms），
  // 否则按住残留会被下一个点的采样循环当成"按下"→ 点错位/少点
  // （用户实测只点 3 个就 saved：点1 按住采满 20 → 画点2 → 残留被采成点2）
  uint32_t up0 = millis();
  while (millis() - up0 < 800) {
    if (g_spiMutex) xSemaphoreTake(g_spiMutex, portMAX_DELAY);
    bool irqLow = (digitalRead(TOUCH_IRQ) == LOW);
    if (g_spiMutex) xSemaphoreGive(g_spiMutex);
    if (!irqLow) break;
    delay(20);
  }
  if (n < 3) { Serial.printf("[CALIB] corner: only %d samples, retry\n", n); return false; }
  *outRX = sumX / n; *outRY = sumY / n;
  return true;
}

// 画校准目标十字 + 提示
static void calibDrawTarget(int idx, int cx, int cy) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(20, 10);
  tft.printf("Touch Calibrate (%d/5)", idx + 1);
  tft.setCursor(20, 32);
  tft.println("Tap the cross center once");
  tft.setCursor(20, 54);
  tft.println("(green dots = samples)");
  // 十字 + 外圈
  tft.drawLine(cx - 40, cy, cx + 40, cy, TFT_GREEN);
  tft.drawLine(cx, cy - 40, cx, cy + 40, TFT_GREEN);
  tft.drawCircle(cx, cy, 18, TFT_YELLOW);
  tft.fillCircle(cx, cy, 4, TFT_YELLOW);
}

void touchRunCalibration() {
  Serial.println("[CALIB] === Touch Calibration Start (5-point LS) ===");
  // 5 个目标：左中上 / 右中上 / 右中下 / 左中下 / 中心（内部点，避开边缘盲区）
  static const int T[5][2] = {{100,100},{380,100},{380,240},{100,240},{240,160}};
  uint16_t cornerRX[5], cornerRY[5];
  for (int i = 0; i < 5; i++) {
    calibDrawTarget(i, T[i][0], T[i][1]);
    if (!calibSampleCorner(&cornerRX[i], &cornerRY[i])) {
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.setCursor(20, 200);
      tft.println("NO TOUCH - tap again");
      Serial.printf("[CALIB] corner %d TIMEOUT (retry)\n", i);
      delay(800);
      i--;
      continue;
    }
    Serial.printf("[CALIB] corner %d @(%3u,%3u): raw rx=%u ry=%u\n",
                  i, T[i][0], T[i][1], cornerRX[i], cornerRY[i]);
  }

  // CALIB9 计算：4 个边缘锚点对角平均斜率 + 外推 clamp [0,4095]。
  // ① 不用最小二乘（中心点若残差大反而拉偏斜率 → 外推超 ADC 上限 4095 → 误报 FAIL）；
  // ② 外推值超界必须 clamp 而不是 FAIL（本面板 raw 跨度 ~3800-4000，边缘外推必越界）。
  // 屏幕 X (100→380) 由 ry 决定：取 P1(100,ry0)→P2(380,ry1) 与 P4(100,ry3)→P3(380,ry2)
  // 两段斜率平均；锚定 4 点均值（x̄=240）。
  double kx = ((double)cornerRY[1] - cornerRY[0] + cornerRY[2] - cornerRY[3]) / 560.0;
  double ryAvg = ((double)cornerRY[0] + cornerRY[1] + cornerRY[2] + cornerRY[3]) / 4.0;
  double bx = ryAvg - kx * 240.0;
  double ryX0 = bx, ryX480 = kx * 480.0 + bx;
  // 屏幕 Y (100→240) 由 rx 决定：P2(100,rx1)→P3(240,rx2) 与 P1(100,rx0)→P4(240,rx3) 平均
  double ky = ((double)cornerRX[2] - cornerRX[1] + cornerRX[3] - cornerRX[0]) / 280.0;
  double rxAvg = ((double)cornerRX[0] + cornerRX[1] + cornerRX[2] + cornerRX[3]) / 4.0;
  double by = rxAvg - ky * 170.0;   // 4 锚点 y 均值 = (100+100+240+240)/4 = 170（勿用 160）
  double rxY0 = by, rxY320 = ky * 320.0 + by;
  // 方向判定（clamp 前）
  uint8_t flags = 0x01;                                    // rotate
  if (ryX480 < ryX0) flags |= 0x02;                        // inv_x
  if (rxY320 < rxY0) flags |= 0x04;                        // inv_y
  // clamp 到 ADC 有效范围 [0,4095]（防越界误报 FAIL）
  if (ryX0 < 0) ryX0 = 0; if (ryX0 > 4095) ryX0 = 4095;
  if (ryX480 < 0) ryX480 = 0; if (ryX480 > 4095) ryX480 = 4095;
  if (rxY0 < 0) rxY0 = 0; if (rxY0 > 4095) rxY0 = 4095;
  if (rxY320 < 0) rxY320 = 0; if (rxY320 > 4095) rxY320 = 4095;
  uint16_t x0 = (uint16_t)(ryX0 < ryX480 ? ryX0 : ryX480);
  uint16_t x1 = (uint16_t)fabs(ryX480 - ryX0);
  uint16_t y0 = (uint16_t)(rxY0 < rxY320 ? rxY0 : rxY320);
  uint16_t y1 = (uint16_t)fabs(rxY320 - rxY0);
  if (x1 == 0 || y1 == 0) {
    Serial.println("[CALIB] FAIL: span zero, keep old calib");
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(20, 100);
    tft.println("CALIB FAIL: bad span!");
    delay(5000);
    return;
  }
  // 保存 + 立即生效
  uint16_t v[5] = {x0, x1, y0, y1, flags};
  memcpy(s_touchCalib, v, sizeof(v));
  Preferences pref;
  if (pref.begin("tcal", false)) {
    pref.putBytes("v", v, sizeof(v));
    pref.end();
    Serial.println("[CALIB] saved to NVS");
  } else {
    Serial.println("[CALIB] WARN: NVS save failed");
  }
  Serial.printf("[CALIB] RESULT: {%u,%u,%u,%u,0x%02X}\n",
                v[0], v[1], v[2], v[3], v[4]);
  // 拟合残差（中心：预测 raw vs 实际中心采样，仅日志）
  double devX = (double)cornerRY[4] - (kx * 240 + bx);
  double devY = (double)cornerRX[4] - (ky * 160 + by);
  Serial.printf("[CALIB] center fit residual raw=(%.1f,%.1f)\n", devX, devY);

  // 结果界面（3 秒后回主界面）
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(20, 10);
  tft.println("Calibration Saved");
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setCursor(20, 40);
  tft.printf("{%u,%u,%u,%u,0x%02X}", v[0], v[1], v[2], v[3], v[4]);
  tft.setCursor(20, 70);
  tft.println("Returning to main UI...");
  delay(3000);
}
