// ============================================================
// holiday_sync.h — 节假日(放假/补班) 在线同步 + LittleFS 缓存
// ============================================================
// 数据源: http://timor.tech/api/holiday/year/<year>  (纯 HTTP, 无 TLS)
//   返回: {"code":0,"holiday":{"MM-DD":{"holiday":true|false,
//            "name":"春节", ...}, ...}}
//   holiday:true  = 放假 (休)  ->  HOL_OFF
//   holiday:false = 补班 (班)  ->  HOL_WORK
//
// 同步节奏 (与"手机日历"一致):
//   1. 上电: 从 LittleFS 加载缓存 (无需联网)
//   2. 联网后: 立即同步 当年+次年; 之后每 24h 再同步一次
//   3. 浏览到某年且无缓存: 后台补同步 (在 loop 内完成, 不阻塞触摸)
// 拉取失败时保留上一次的好缓存, 绝不清空日历。
//
// 用法:
//   setup():  WiFi+NTP 完成后  holiday::init();
//   loop():   holiday::tick();
//   日历:     holiday::ensureYear(calYear);
//             uint8_t t = holiday::dayType(calYear, calMon, day);
#ifndef __HOLIDAY_SYNC_H__
#define __HOLIDAY_SYNC_H__

#include <Arduino.h>

// 单日状态
#define HOL_NONE 0   // 普通 (未标注)
#define HOL_OFF  1   // 放假 (休)
#define HOL_WORK 2   // 补班 (班)

namespace holiday {
  void init();            // setup() 里调用: 按当前时间加载 当年-1..+1 的缓存
  void tick();            // loop() 里调用: 24h 同步 + 浏览补同步
  void ensureYear(int y); // 浏览到某年时调用: 无缓存则排队网络同步
  uint8_t dayType(int y, int m, int d); // 返回 HOL_*
  const char* dayName(int y, int m, int d); // 节日/补班名称, 无则 ""
}

#endif
