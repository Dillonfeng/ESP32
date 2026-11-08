// ============================================================================
// audio_raw_hook.cpp — 把音频库的 audio_process_raw_samples 弱符号换成强定义
//
// 为什么要单独一个文件: ESP32-audioI2S 的 Audio.h **把该函数声明成 weak**,
// 只要在同一个 TU 里 include 了 Audio.h, 我们自己写的定义也会被标成 W(弱) ——
// 两个弱定义谁生效取决于链接顺序, 太脆。本文件不 include Audio.h, 只按签名
// 定义一个强符号 (nm 里是 T), 链接器必然用它。
//
// 回调时机 (Audio.cpp L3954): 解码后原始 PCM, 在 VU/EQ/音量 之前, 每次
//   拿到 readWords 个 32bit 交错立体声样本 (16bit 有效值在高半字)。
// 该回调跑在库的 audio 任务里, 所以只往 PSRAM 环冲里塞, 绝不在这里写 SD。
// ============================================================================
#include <stdint.h>
#include "radio_rec.h"

void audio_process_raw_samples(int32_t* outBuff, int16_t validSamples) {
  radiorec::hookRawSamples(outBuff, validSamples);
}
