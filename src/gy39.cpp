// ============================================================
// gy39.cpp — GY-39 四合一气象传感器 UART 驱动 (实现见 include/gy39.h)
// ============================================================
#include <Arduino.h>
#include <math.h>
#include "config.h"
#include "gy39.h"

namespace gy39 {

namespace {

HardwareSerial s_uart(2);
Data s_data;
unsigned s_seq = 0;

uint8_t  s_rx[32];
uint8_t  s_rxLen = 0;

// 最近一次用于判断 seq 的整数值
int s_lastTemp = 0x7FFF, s_lastHum = 0x7FFF, s_lastPres = 0x7FFF, s_lastAlt = 0x7FFF;

void bumpSeqIfChanged() {
  int t  = (int)lroundf(s_data.tempC);
  int h  = (int)lroundf(s_data.humidity);
  int p  = (int)lroundf(s_data.pressureHpa);
  int a  = (int)lroundf(s_data.altitudeM);
  if (t != s_lastTemp || h != s_lastHum || p != s_lastPres || a != s_lastAlt) {
    s_lastTemp = t; s_lastHum = h; s_lastPres = p; s_lastAlt = a;
    s_seq++;
  }
}

void handleFrame(const uint8_t* f, uint8_t len) {
  const uint8_t* d = f + 4;
  switch (f[2]) {
    case 0x15:                                  // 光照
      if (len >= 4) {
        uint32_t raw = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) |
                       ((uint32_t)d[2] << 8)  |  (uint32_t)d[3];
        s_data.lux = (float)raw / 100.0f;
        s_data.luxMs = millis();
        static bool s_luxLogged = false;
        if (!s_luxLogged) {
          s_luxLogged = true;
          Serial.printf("[GY39] first lux: %.2f lx\n", s_data.lux);
        }
      }
      break;
    case 0x45:                                  // 温度/气压/湿度/海拔
      if (len >= 10) {
        int16_t t = (int16_t)(((uint16_t)d[0] << 8) | d[1]);
        uint32_t pa = ((uint32_t)d[2] << 24) | ((uint32_t)d[3] << 16) |
                      ((uint32_t)d[4] << 8)  |  (uint32_t)d[5];
        // 实测帧: 5A 5A 45 0A 0B16(temp) 00990877(press) 17E0(hum) 0056(alt)
        // 字段顺序是 温度/气压/湿度/海拔 (湿度在前, 海拔在后)
        uint16_t hum = (uint16_t)(((uint16_t)d[6] << 8) | d[7]);
        int16_t alt  = (int16_t)(((uint16_t)d[8] << 8) | d[9]);
        s_data.tempC       = (float)t / 100.0f;
        s_data.pressureHpa = (float)pa / 10000.0f;    // 原始值 0.01Pa -> hPa
        s_data.altitudeM   = (float)alt;
        s_data.humidity    = (float)hum / 100.0f;
        s_data.tempMs      = millis();
        bumpSeqIfChanged();
        static bool s_firstLogged = false;
        if (!s_firstLogged) {
          s_firstLogged = true;
          Serial.printf("[GY39] first frame: T=%.2fC H=%.2f%% P=%.2fhPa alt=%.0fm\n",
                        s_data.tempC, s_data.humidity, s_data.pressureHpa, s_data.altitudeM);
        }
      }
      break;
    default:
      break;
  }
}

// 字节流 -> 帧; 校验错则左移一字节重新同步
void feed(uint8_t b) {
  if (s_rxLen == 0 && b != 0x5A) return;
  if (s_rxLen == 1 && b != 0x5A) { s_rxLen = 0; return; }
  if (s_rxLen >= sizeof(s_rx)) s_rxLen = 0;
  s_rx[s_rxLen++] = b;
  if (s_rxLen < 4) return;
  uint8_t len = s_rx[3];
  if (len > sizeof(s_rx) - 5) { s_rxLen = 0; return; }
  uint8_t total = (uint8_t)(len + 5);
  if (s_rxLen < total) return;
  uint8_t sum = 0;
  for (uint8_t i = 0; i < total - 1; i++) sum = (uint8_t)(sum + s_rx[i]);
  if (sum != s_rx[total - 1]) {
    memmove(s_rx, s_rx + 1, (size_t)(s_rxLen - 1));
    s_rxLen--;
    return;
  }
  handleFrame(s_rx, len);
  s_rxLen = 0;
}

void sendCmd(uint8_t cmd) {
  uint8_t f[3] = { 0xA5, cmd, (uint8_t)(cmd + 0xA5) };
  s_uart.write(f, 3);
}

} // anon

void init() {
  s_uart.begin(GY39_BAUD, SERIAL_8N1, GY39_RX_PIN, GY39_TX_PIN);
  delay(50);
  while (s_uart.available()) s_uart.read();   // 丢弃上电自动输出的残留帧
  s_rxLen = 0;
  Serial.printf("[GY39] UART2 %d baud RX=%d TX=%d\n", GY39_BAUD, GY39_RX_PIN, GY39_TX_PIN);
}

void tick() {
  // 1) 收帧 (非阻塞, 每次最多解析完当前缓冲)
  while (s_uart.available()) feed((uint8_t)s_uart.read());

  // 2) 周期切换模块输出模式 (0x82 温湿压海拔 / 0x81 光照)
  //    两条"连续输出"指令互斥, 模块没有"全部输出"指令, 只能轮流切换。
  //    不对称窗口: 温湿压 15s + 光照 105s, lux 最旧仅 ~15s -> 背光/夜间熄屏响应快;
  //    命令仍是每 60 秒一次, 不增加模块内部 flash 写入。
  uint32_t now = millis();
  static uint32_t s_nextSwitchMs = 1200;
  static bool s_inTempMode = false;
  if ((int32_t)(now - s_nextSwitchMs) >= 0) {
    if (s_inTempMode) {
      sendCmd(0x81);
      s_inTempMode = false;
      s_nextSwitchMs = now + GY39_LUX_DWELL_MS;
    } else {
      sendCmd(0x82);
      s_inTempMode = true;
      s_nextSwitchMs = now + GY39_TEMP_DWELL_MS;
    }
  }

  // 3) 新鲜度 (掉线/无模块时 UI 自动回退)
  s_data.ok    = (s_data.tempMs != 0) && (now - s_data.tempMs <= GY39_STALE_MS);
  s_data.luxOk = (s_data.luxMs  != 0) && (now - s_data.luxMs  <= GY39_LUX_STALE_MS);
}

const Data& get() { return s_data; }

unsigned seq() { return s_seq; }

float altitudeFromSeaLevel(float pressureHpa, float seaLevelHpa) {
  if (pressureHpa <= 0.0f || seaLevelHpa <= 0.0f) return 0.0f;
  return 44330.0f * (1.0f - powf(pressureHpa / seaLevelHpa, 0.19029f));  // 1/5.255
}

} // namespace gy39
