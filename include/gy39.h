// ============================================================
// gy39.h — GY-39 四合一气象传感器 (UART2: RX=GPIO7 / TX=GPIO8, 9600 8N1)
//
// 帧格式: 5A 5A <cmd> <len> <data...> <checksum>
//   总长 = len + 5, checksum = 前面所有字节累加 & 0xFF
//   cmd 0x15 (len=4) : 光照 4 字节 /100 = lux
//   cmd 0x45 (len=10): 温度 /100 ℃, 气压 4 字节 /100 Pa, 海拔 2 字节 m, 湿度 /100 %
//
// 指令: A5 82 27 = 连续输出温湿压海拔; A5 81 26 = 连续输出光照。
// 两条指令会互相顶掉"连续输出"模式, 因此采用交替下发 (各 3s 一次),
// 任一时刻收到的帧都照常解析。
//
// 海拔: 模块自带按 1013.25hPa 估算; 联网拿到天气 API 的 pressure_msl 后,
// 用 altitudeFromSeaLevel() 在当地重新修正。
// ============================================================
#pragma once
#include <Arduino.h>

namespace gy39 {

struct Data {
  bool  ok      = false;   // 温湿压有效 (最近 GY39_STALE_MS 内收到 0x45 帧)
  bool  luxOk   = false;   // 光照有效 (最近收到 0x15 帧)
  float tempC   = 0.0f;
  float humidity = 0.0f;   // %
  float pressureHpa = 0.0f;// 当地绝对气压 hPa
  float altitudeM = 0.0f;  // 模块按 1013.25hPa 估算的海拔 (m)
  float lux     = 0.0f;
  uint32_t tempMs = 0, luxMs = 0;
};

void init();     // setup: 打开 UART2 (幂等), 清空上电残留帧
void tick();     // loop: 交替下发指令 + 非阻塞解析
const Data& get();

// 每次整数量值变化 +1 (temperature/humidity/pressure/altitude), 供 UI 判断重绘
unsigned seq();

// 修正海拔: P=当地绝对气压(hPa), P0=当地海平面气压(hPa)
float altitudeFromSeaLevel(float pressureHpa, float seaLevelHpa);

} // namespace gy39
