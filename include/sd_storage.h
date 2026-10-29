// ============================================================
// sd_storage.h — 板载 SPI TF 卡存储层 (SD 优先 / LittleFS 回退)
//
// 硬件: 板载 TF 卡座为 SPI 模式, CS 硬件焊死 GPIO6, 与显示屏/触摸共享
//       FSPI 总线 (SCLK=12 / MISO=13 / MOSI=11)。
//
// 约定:
//  - SD 挂载点 = "/sdcard"; SD.open("/stations.bin") 实际访问 /sdcard/stations.bin,
//    与 LittleFS 路径一致 → 上层模块只换 open 调用, 不改路径字符串。
//  - 挂载成功后所有项目数据文件 (电台库 / 节日音乐配置 / 天气缓存 / 节假日)
//    优先读写 SD; 卡片缺失或挂载失败时自动回退 LittleFS, 功能不降级。
//  - 本模块内部**不取** g_spiMutex: 所有调用都发生在已持锁的上下文
//    (loop 的 SPI 锁段 / setup 单任务阶段), 自行加锁会死锁。
// ============================================================
#pragma once
#include <Arduino.h>
#include <FS.h>

namespace sdstore {

// setup: 常驻挂载 TF 卡 (失败则继续用 LittleFS)。必须在 initTFT() 之后调用,
// 保证 FSPI 总线已按显示屏引脚启动。
bool init();

bool mounted();
const char* backend();            // "SD" / "LittleFS"

// 读: SD 存在该文件 -> SD; 否则 LittleFS。
File openRead(const char* path);
// 写: SD 已挂载 -> SD; 否则 LittleFS。
File openWrite(const char* path);

bool exists(const char* path);

// 首次使用 SD 时把 LittleFS 里的项目数据复制到卡上 (已存在则跳过)。
// 只复制小数据文件 (电台库/配置/缓存), 字库仍留在 Flash。
bool seedFromFlash();

bool makeDir(const char* path);

uint64_t totalBytes();
uint64_t usedBytes();

} // namespace sdstore
