// ============================================================
// backlight.h — 显示屏背光 (GPIO21, LEDC PWM) 自动亮度
//
// 依据 GY-39 光照值自动调节亮度; 夜间 (持续低于阈值) 自动熄屏,
// 触摸时唤醒并吞掉该次点击 (避免熄屏状态下误触导航)。
// 传感器无数据时保持全亮, 不影响原有使用。
// ============================================================
#pragma once
#include <Arduino.h>

namespace backlight {

void init();                 // setup: LEDC 绑定 GPIO21, 初始 100%
void tick();                 // loop: 依 GY-39 lux 平滑调节 / 夜间熄屏
void setPercent(uint8_t pct); // 0..100 直接设定 (调试/手动)
uint8_t percent();
bool isOff();                // 已熄屏 (背光=0)
void wake();                 // 触摸唤醒: 强制点亮一段时间
void setKeepOn(bool on);      // 2026-11-03: 轮播等场景持续点亮 (不熄屏, 暗环境给低亮)
void setForceMax(bool on);    // 2026-11-08: 强制 100% (录音机等看照片底图的全屏页) + 不熄屏

} // namespace backlight
