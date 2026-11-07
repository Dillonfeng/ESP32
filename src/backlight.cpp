// ============================================================
// backlight.cpp — 背光自动亮度 + 夜间熄屏 (实现见 include/backlight.h)
// ============================================================
#include <Arduino.h>
#include <math.h>
#include "config.h"
#include "backlight.h"
#include "gy39.h"

namespace backlight {

namespace {

bool     s_ready = false;
bool     s_off   = false;
uint8_t  s_duty  = 100;      // 当前实际亮度 (0..100)
uint8_t  s_target = 100;     // 目标亮度
uint32_t s_lastEval = 0;
uint32_t s_lastStep = 0;
uint32_t s_darkSince = 0;
uint32_t s_forceUntil = 0;
bool     s_keepOn = false;     // 2026-11-03: 轮播等场景持续点亮, 抑制夜间熄屏

// lux -> 亮度 曲线 (对数区间线性插值)
struct Pt { float lux; uint8_t pct; };
const Pt CURVE[] = {
  {    4.0f,   8 },
  {   20.0f,  15 },
  {  100.0f,  30 },
  {  400.0f,  55 },
  { 2000.0f,  90 },
  { 8000.0f, 100 },
};
const int CURVE_N = (int)(sizeof(CURVE) / sizeof(CURVE[0]));

void apply(uint8_t pct) {
  const uint32_t maxDuty = (1u << BL_PWM_BITS) - 1u;
  ledcWrite(TFT_BL_PIN, (uint32_t)pct * maxDuty / 100u);
}

uint8_t mapLux(float lux) {
  if (lux <= CURVE[0].lux) return CURVE[0].pct;
  if (lux >= CURVE[CURVE_N - 1].lux) return CURVE[CURVE_N - 1].pct;
  float lx = log10f(lux);
  for (int i = 1; i < CURVE_N; i++) {
    if (lux <= CURVE[i].lux) {
      float l0 = log10f(CURVE[i - 1].lux), l1 = log10f(CURVE[i].lux);
      float k = (lx - l0) / (l1 - l0);
      return (uint8_t)(CURVE[i - 1].pct + k * (CURVE[i].pct - CURVE[i - 1].pct) + 0.5f);
    }
  }
  return 100;
}

} // anon

void init() {
  ledcAttach(TFT_BL_PIN, BL_PWM_FREQ, BL_PWM_BITS);
  s_ready = true;
  s_off = false;
  s_duty = s_target = 100;
  s_darkSince = 0;
  s_forceUntil = 0;
  apply(100);
}

void setPercent(uint8_t pct) {
  if (pct > 100) pct = 100;
  s_target = s_duty = pct;
  s_off = (pct == 0);
  apply(pct);
}

uint8_t percent() { return s_duty; }

bool isOff() { return s_off; }

void wake() {
  s_off = false;
  s_target = s_duty = 25;         // 唤醒后先给低亮, 由 tick() 按环境光继续调整
  s_darkSince = millis();         // 重新开始计时 (仍黑则 BL_NIGHT_HOLD_MS 后再熄)
  s_forceUntil = millis() + BL_WAKE_MS;
  apply(s_duty);
}

// 2026-11-03: 轮播等需要持续可见的场景: 抑制夜间熄屏, 暗环境给低亮(25%), 亮环境按曲线。
void setKeepOn(bool on) {
  s_keepOn = on;
  if (on) {
    s_off = false;
    s_darkSince = 0;
  }
}

void tick() {
  if (!s_ready) return;
  const uint32_t now = millis();
  const gy39::Data& g = gy39::get();

  // 1) 每 250ms 评估一次环境光 (lux 由 GY-39 光照窗口提供, 最旧约 15s)
  if (now - s_lastEval >= 250) {
    s_lastEval = now;
    if (g.luxOk) {
      if (s_keepOn) {
        // 轮播持续亮: 暗环境给 25% (可見且不刺眼), 亮环境走曲线
        s_off = false;
        s_darkSince = 0;
        s_target = (g.lux < BL_NIGHT_LUX) ? 25 : mapLux(g.lux);
      } else if (g.lux < BL_NIGHT_LUX) {
        if (s_darkSince == 0) {
          s_darkSince = now;
        } else if (!s_off && now - s_darkSince >= BL_NIGHT_HOLD_MS && now > s_forceUntil) {
          s_off = true;
          s_target = 0;               // 夜间自动熄屏
        }
      } else {
        s_darkSince = 0;
        if (s_off && g.lux > BL_NIGHT_LUX * 2.0f) s_off = false;   // 天亮回滞恢复
        if (!s_off && now > s_forceUntil) s_target = mapLux(g.lux);
      }
    }
  }

  // 2) 平滑过渡 (~4%/100ms, 比之前快一倍; 全量程约 2.5 秒, 仍无明暗跳动)
  if (now - s_lastStep >= 100) {
    s_lastStep = now;
    if (s_duty < s_target) {
      s_duty = (uint8_t)min<int>(s_target, s_duty + 4);
      apply(s_duty);
    } else if (s_duty > s_target) {
      s_duty = (uint8_t)max<int>(s_target, s_duty - 4);
      apply(s_duty);
    }
  }
}

} // namespace backlight
