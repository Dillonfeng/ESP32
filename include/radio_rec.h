#pragma once
#include <stdint.h>

// 电台录音 (2026-11-08)
// 数据来源 = ESP32-audioI2S 的弱符号 audio_process_raw_samples (解码后的原始 PCM,
//   在音量/EQ 之前), 按流自身的采样率 + 声道数 (库恒为 L/R 成对 -> 16bit 立体声)
//   录成 WAV, 存 /rad_XXXX.wav, 与麦克风录音共用一套卷号, 录音机页只负责播放这些卷。
//
// 关键约束 (为何要环形缓冲):
//   该回调跑在库的 audio 任务里, 在这里直接写 SD 会阻塞 I2S 输出 -> 电台会断续。
//   所以回调只把样本塞进 PSRAM 环冲 (几秒的量), 由主循环 radiorec::tick() 落盘。
//   SD 走独立 HSPI(SPI3), 与显示 FSPI 互不干扰; 44.1k 单声道只 88KB/s, 写卡远快于此。
namespace radiorec {

bool     active();          // 正在录电台
void     init();            // setup 调用: 打印上次录音的自检结果 (/rad_diag.txt)
bool     start();           // 开始录当前在播的电台 (需 radioPlaying)
void     stop();            // 收尾: 补齐 WAV 头 + 释放环冲
void     toggle();          // 录音键语义 (UI 用)
void     tick();            // 主循环调用: 环冲 -> SD, 并做停止条件判断
uint32_t elapsedSec();      // 已录秒数 (不含暂停/断流空档)
uint32_t sampleRate();      // 本卷采样率 (Hz)
uint32_t droppedBytes();    // 环冲溢出丢弃字节数 (诊断)
const char* fileName();     // "/rec_0012.wav"

// 由 audio 任务回调调用 (见 src/audio_raw_hook.cpp: 强定义覆盖库里的弱符号)
void hookRawSamples(int32_t* buf, int16_t words);

}  // namespace radiorec
