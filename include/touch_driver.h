#pragma once
#include <Arduino.h>

// ============================================================
// BUILDV5-TOUCHDRV: 自写 XPT2046 触摸驱动（独立任务版）
// ============================================================
// 背景：TFT_eSPI::getTouch() 内部用 FreeRTOS 优先级继承互斥量（spi_mutex）保护 SPI，
// 跨任务并发调用 → ESP-IDF 5.5 多核 xTaskPriorityDisinherit 断言崩溃
// （cap_touch1/3/4 三次实证 RESET_REASON=4 循环）。业界正解 = 绕开库的锁：
// 本驱动直接用 tft.getSPIinstance() 的 SPIClass 显式事务（beginTransaction/transfer/
// endTransaction）。SPIClass 内部用 std::mutex（无优先级继承）+ ESP-IDF bus lock
// （事务级硬件互斥）→ 跨任务并发安全，不触发 FreeRTOS 断言。
// 协议序列与校准公式逐字节复制自 TFT_eSPI Extensions/Touch.cpp（getTouchRaw 17 字节
// 序列 + convertRawXY rotate 分支），坐标输出与 tft.getTouch 完全一致。
// ============================================================

// 触摸共享状态（touchScanTask 单写，main loop 单读）
extern volatile bool     s_touchDown;
extern volatile uint16_t s_touchX;
extern volatile uint16_t s_touchY;
// BUILDV5-CALIB: 原始 ADC 采样值（校准诊断用，touchScanTask 单写）
extern volatile uint16_t g_touchRawRX;
extern volatile uint16_t g_touchRawRY;

// 初始化：拿 SPI 实例 + 配置 TOUCH_CS
void touchDriverInit();

// 读一次：返回 true=已按下（校准后屏幕坐标写入 scrX/scrY，压力写入 z）
bool touchDriverRead(uint16_t* scrX, uint16_t* scrY, uint16_t* z);

// 触摸扫描任务（独立 FreeRTOS 任务，20ms 采样，写共享状态）
void touchScanTask(void* arg);

// BUILDV5-CALIB: 图形化触摸校准（串口 'C' 触发；点 4 角 → NVS 保存 → 立即生效）
void touchRunCalibration();
void touchDiag(int seconds);   // BUILDV5-STEP3-TFIX4: 原始触摸数据转储（串口键 'T'）
