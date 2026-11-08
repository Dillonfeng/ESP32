// ============================================================================
// radio_rec.cpp — 电台录音 (2026-11-08)
//
// 数据源: ESP32-audioI2S 的弱符号 audio_process_raw_samples()
//         (Audio.cpp L3954, 在音量/EQ/I2S 输出之前调用) —— 拿到的是解码后的
//         32bit 交错立体声原始 PCM, 16bit 有效值在**高半字**。
//
// 链路: audio 任务回调 --(左声道 >>16, 16bit)--> PSRAM 环形缓冲 --(主循环 tick)--> SD WAV
//
// 为什么不在回调里直接写 SD: 回调跑在库的 audio 任务里, 一旦卡住 I2S write,
//   正在播的电台就会断续。环冲只做几秒抖动缓冲, 真正的落盘在主循环里做。
//   SD 走独立 HSPI(SPI3)、屏幕走 FSPI(SPI2), 44.1k 单声道 88KB/s, 写卡远快于此。
//
// 文件: /rad_XXXX.wav, 16bit 单声道, 采样率 = 电台流自身的采样率 (不重采样),
//       卷号与麦克风录音共用 /rec_seq.txt 计数器, 所以不会撞号。
// ============================================================================
#include <Arduino.h>
#include <SD.h>
#include <LittleFS.h>
#include <string.h>
#include <esp_heap_caps.h>
#include "Audio.h"
#include "ui_common.h"      // extern Audio audio; extern bool radioPlaying;
#include "config.h"
#include "sd_storage.h"
#include "radio_rec.h"

namespace {

const size_t RING_SIZES[] = { 512 * 1024, 256 * 1024, 128 * 1024 };

uint8_t* s_ring = nullptr;
size_t   s_ringSize = 0;
volatile size_t   s_wr = 0;          // 单调递增字节计数 (audio 任务只写)
volatile size_t   s_rd = 0;          // 单调递增字节计数 (主循环只写)
volatile bool     s_on = false;
volatile uint32_t s_drop = 0;        // 环冲满丢弃字节
File     s_file;
char     s_name[24] = "";
uint32_t s_rate = 0;                 // 本卷采样率
uint32_t s_bytes = 0;                // 已落盘字节
bool     s_werr = false;
uint32_t s_tickMs = 0;

// ---- 卷号 (与麦克风录音共用序号: 扫 rec_/rad_ 两种前缀取最大, 再用 /rec_seq.txt 兜底) ----
int nextTapeNo() {
  int maxN = 0;
  if (sdstore::mounted()) {
    File root = SD.open("/");
    if (root) {
      File f;
      while ((f = root.openNextFile())) {
        const char* p = f.name();
        if (p) {
          const char* b = strrchr(p, '/');
          b = b ? (b + 1) : p;
          int skip = 0;
          if      (strncmp(b, "rec_", 4) == 0) skip = 4;
          else if (strncmp(b, "rad_", 4) == 0) skip = 4;
          if (skip) {
            const char* d = b + skip;
            while (*d && (*d < '0' || *d > '9')) d++;
            int n = atoi(d);
            if (n > maxN) maxN = n;
          }
        }
        f.close();
      }
      if (f) f.close();
      root.close();
    }
  }
  int seqN = 0;
  if (LittleFS.begin()) {
    File sf = LittleFS.open("/rec_seq.txt", "r");
    if (sf) { seqN = atoi(sf.readString().c_str()); sf.close(); }
  }
  int next = (maxN > seqN ? maxN : seqN) + 1;
  if (LittleFS.begin()) {
    File sf = LittleFS.open("/rec_seq.txt", "w");
    if (sf) { sf.print(next); sf.close(); }
  }
  return next;
}

// 16bit 单声道 WAV 头 (dataBytes=0 时先占位, 停止时 seek(0) 重写)
void wavHeader(File& f, uint32_t dataBytes) {
  uint8_t h[44];
  uint32_t riffSize = 36 + dataBytes;
  uint32_t byteRate = s_rate * 2;
  h[0]='R'; h[1]='I'; h[2]='F'; h[3]='F';
  h[4]=riffSize&0xFF; h[5]=(riffSize>>8)&0xFF; h[6]=(riffSize>>16)&0xFF; h[7]=(riffSize>>24)&0xFF;
  h[8]='W'; h[9]='A'; h[10]='V'; h[11]='e';
  h[12]='f'; h[13]='m'; h[14]='t'; h[15]=' ';
  h[16]=16; h[17]=0; h[18]=0; h[19]=0;                 // fmt chunk 16 字节
  h[20]=1;  h[21]=0;                                   // PCM
  h[22]=1;  h[23]=0;                                   // 单声道
  h[24]=s_rate&0xFF; h[25]=(s_rate>>8)&0xFF; h[26]=(s_rate>>16)&0xFF; h[27]=(s_rate>>24)&0xFF;
  h[28]=byteRate&0xFF; h[29]=(byteRate>>8)&0xFF; h[30]=(byteRate>>16)&0xFF; h[31]=(byteRate>>24)&0xFF;
  h[32]=2; h[33]=0;                                    // blockAlign = 2
  h[34]=16; h[35]=0;                                   // 16bit
  h[36]='d'; h[37]='a'; h[38]='t'; h[39]='a';
  h[40]=dataBytes&0xFF; h[41]=(dataBytes>>8)&0xFF; h[42]=(dataBytes>>16)&0xFF; h[43]=(dataBytes>>24)&0xFF;
  f.write(h, 44);
}

// 环冲 -> SD (最多 maxBytes, 避免一次占太久)
void drain(size_t maxBytes) {
  size_t avail = s_wr - s_rd;
  if (!avail || !s_file) return;
  size_t n = (avail > maxBytes) ? maxBytes : avail;
  size_t off = s_rd % s_ringSize;
  size_t chunk = s_ringSize - off;
  if (chunk > n) chunk = n;
  size_t wr = s_file.write(s_ring + off, chunk);
  if (wr == 0 && chunk > 0) { s_werr = true; return; }
  s_rd += wr;
  s_bytes += wr;
}

// audio 任务回调 (由 src/audio_raw_hook.cpp 的强符号转发进来)
void feedPcm(int32_t* buf, int16_t words) {
  if (!s_on || !s_ring) return;
  size_t frames = (size_t)words >> 1;              // 交错 L/R: words 是 32bit 个数
  size_t need = frames * 2;                        // 16bit 单声道
  if (s_ringSize - (s_wr - s_rd) < need) { s_drop += (uint32_t)need; return; }
  size_t w = s_wr % s_ringSize;
  for (size_t i = 0; i < frames; i++) {
    int16_t v = (int16_t)(buf[i * 2] >> 16);        // 取左声道 (16bit 有效值在高半字)
    s_ring[w] = (uint8_t)(v & 0xFF);
    if (++w >= s_ringSize) w = 0;
    s_ring[w] = (uint8_t)((v >> 8) & 0xFF);
    if (++w >= s_ringSize) w = 0;
  }
  s_wr += need;
}

}  // namespace

namespace radiorec {

void hookRawSamples(int32_t* buf, int16_t words) { feedPcm(buf, words); }

bool     active()      { return s_on; }
uint32_t sampleRate()  { return s_rate; }
uint32_t droppedBytes(){ return s_drop; }
const char* fileName() { return s_name; }
uint32_t elapsedSec()  { return s_rate ? (s_bytes / (2 * s_rate)) : 0; }

bool start() {
  if (s_on) return true;
  if (!radioPlaying) { Serial.println("[RAD] start fail: radio not playing"); return false; }
  if (!sdstore::mounted()) { Serial.println("[RAD] start fail: no SD"); return false; }
  {
    uint64_t tot = sdstore::totalBytes(), used = sdstore::usedBytes();
    if (tot > 0 && tot >= used && (tot - used) < (4ULL << 20)) {
      Serial.println("[RAD] start fail: SD full");
      return false;
    }
  }
  uint32_t rate = audio.getSampleRate();
  if (rate < 8000 || rate > 96000) rate = 44100;         // 取不到就按 44.1k
  s_rate = rate;

  // PSRAM 环冲 (几秒量): 512KB ≈ 44.1k 下 5.8s, 不占内部 RAM
  s_ring = nullptr;
  for (size_t want : RING_SIZES) {
    s_ring = (uint8_t*)heap_caps_malloc(want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ring) { s_ringSize = want; break; }
  }
  if (!s_ring) { Serial.println("[RAD] start fail: psram ring"); s_rate = 0; return false; }

  int no = nextTapeNo();
  snprintf(s_name, sizeof(s_name), "/rad_%04d.wav", no);
  s_file = SD.open(s_name, "w");
  if (!s_file) {
    Serial.printf("[RAD] start fail: open %s\n", s_name);
    heap_caps_free(s_ring); s_ring = nullptr; s_ringSize = 0; s_rate = 0;
    return false;
  }
  wavHeader(s_file, 0);                                  // 先占位, stop 时重写
  s_wr = s_rd = 0;
  s_bytes = 0;
  s_drop = 0;
  s_werr = false;
  s_on = true;
  s_tickMs = millis();
  Serial.printf("[RAD] start -> %s  %uHz mono  ring=%uKB\n", s_name, (unsigned)s_rate,
                (unsigned)(s_ringSize >> 10));
  return true;
}

void stop() {
  if (!s_on) return;
  s_on = false;                       // 先停生产者
  delay(20);                          // 等 audio 任务把这批样本写完 (回调周期 ~6ms)
  if (s_file) {
    drain(s_ringSize);                // 剩下的全部落盘
    s_file.seek(0);
    wavHeader(s_file, s_bytes);       // 补齐长度
    s_file.close();
  }
  Serial.printf("[RAD] saved %s  %us  %uB  rate=%u  drop=%u%s\n",
                s_name, (unsigned)(s_rate ? s_bytes / (2 * s_rate) : 0), (unsigned)s_bytes,
                (unsigned)s_rate, (unsigned)s_drop, s_werr ? "  WRITE-ERR" : "");
  if (s_ring) { heap_caps_free(s_ring); s_ring = nullptr; }
  s_ringSize = 0;
  s_rate = 0;
  s_werr = false;
}

void toggle() { if (s_on) stop(); else start(); }

void tick() {
  if (!s_on) return;
  if (millis() - s_tickMs < 8) return;      // 别每轮 loop 都进 FS
  s_tickMs = millis();

  drain(8192);                       // 每次最多 8KB (锁内只占几 ms)

  if (s_werr) { Serial.println("[RAD] stop: write error"); stop(); return; }

  // 电台停了/切台 -> 收尾保存 (不丢已录内容)
  if (!radioPlaying) {
    Serial.println("[RAD] stop: radio stopped");
    stop();
    return;
  }
  uint32_t rate = audio.getSampleRate();
  if (rate >= 8000 && rate <= 96000 && rate != s_rate) {
    Serial.printf("[RAD] stop: rate changed %u -> %u\n", (unsigned)s_rate, (unsigned)rate);
    stop();
    return;
  }
  if (elapsedSec() >= (uint32_t)REC_MAX_SEC) {
    Serial.println("[RAD] stop: reach 1h limit");
    stop();
    return;
  }
}

}  // namespace radiorec
