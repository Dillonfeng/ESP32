// ============================================================================
// radio_rec.cpp — 电台录音 (2026-11-08)
//
// 数据源: ESP32-audioI2S 的弱符号 audio_process_raw_samples()
//         (Audio.cpp L3954, 在 VU/EQ/音量/I2S 输出之前调用) —— 拿到的是解码后的
//         32bit 交错立体声原始 PCM, 16bit 有效值在**高半字**
//         (库内 calculateVUlevel 用 mag>>23 归一 0..255, 就是高半字 16bit 的证据;
//          src/radio_ui.cpp 的 audio_process_i2s 也是 >>16 取 VU, 实机有电平)。
//
// 声道: 跟随流 = **写双声道 (2ch)**。库在 playChunk 里永远按 L/R 成对处理
//       (Gain/stereo2mono 都是 buff[i*2], buff[i*2+1]), 单声道源由解码器复制成
//       L==R 再进来, 所以"不降采样、不丢声道"的忠实做法就是原样写 2ch。
//
// 链路: audio 任务回调 --(L/R 各 >>16, 16bit)--> PSRAM 环形缓冲
//       --(主循环 tick, 8KB/次)--> SD WAV
//
// 为什么不在回调里直接写 SD: 回调跑在库的 audio 任务里, 一旦卡住 I2S write,
//   正在播的电台就会断续。环冲只做几秒抖动缓冲, 真正的落盘在主循环里做。
//   SD 走独立 HSPI(SPI3)、屏幕走 FSPI(SPI2), 44.1k 立体声 176KB/s, 写卡远快于此。
//
// 文件: /rad_XXXX.wav, 16bit 立体声 (2ch), 采样率 = 电台流自身的采样率 (不重采样),
//       卷号与麦克风录音共用 /rec_seq.txt 计数器, 所以不会撞号。
//
// 2026-11-08 新增自检 (排查"文件有了但回放没声音"):
//   录音时在回调里统计 L/R 各自峰值与帧数; 收尾时把文件回读一遍, 核对
//   RIFF/data 长度并算文件里前 2KB 的实际峰值 -> 写 /rad_diag.txt + 串口,
//   下次开机 radiorec::init() 把它打出来 (用户不会抓日志, 拔插电源即可)。
//   判据: pkL/pkR 有值但 peakF=0 -> 录进去的是静音 (写盘/跨核可见性问题);
//         peakF 也有值 -> 文件本身有声音, 无声在播放侧。
// ============================================================================
#include <Arduino.h>
#include <SD.h>
#include <LittleFS.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "Audio.h"
#include "ui_common.h"      // extern Audio audio; extern bool radioPlaying; extern g_spiMutex;
#include "config.h"
#include "sd_storage.h"
#include "radio_rec.h"

namespace {

// 立体声 4 字节/帧: 512KB ≈ 2.9s @44.1k (写盘任务独立后够了, 也别抢太多 PSRAM)
const size_t RING_SIZES[] = { 512 * 1024, 256 * 1024, 128 * 1024 };
const size_t OUT_BYTES_PER_FRAME = 4;         // 16bit × 2ch

uint8_t* s_ring = nullptr;
size_t   s_ringSize = 0;
volatile size_t   s_wr = 0;          // 单调递增字节计数 (audio 任务只写)
volatile size_t   s_rd = 0;          // 单调递增字节计数 (写盘任务只写)
volatile bool     s_on = false;
volatile uint32_t s_drop = 0;        // 环冲满丢弃字节
File     s_file;
char     s_name[24] = "";
uint32_t s_rate = 0;                 // 本卷采样率
uint32_t s_bytes = 0;                // 已落盘字节
bool     s_werr = false;
uint32_t s_tickMs = 0;

// 写盘任务 (2026-11-08: 从主循环挪出 —— 主循环里写 8KB/8ms 会长时间占住 CPU/SPI,
// 电台流一抖就"打嘟噜"; 现在只有这个任务碰 SD)
volatile bool s_taskOn = false;
TaskHandle_t  s_task = nullptr;
volatile uint32_t s_wrMaxMs = 0;     // 单次写卡最长耗时 (诊断)
uint32_t s_statMs = 0;
uint32_t s_lastBytes = 0;            // 写盘看门狗: 2s 一字节没写 -> 判定卡住, 收尾
uint32_t s_stallMs = 0;

// 自检统计 (回调里累加)
uint64_t s_frames = 0;
uint32_t s_pkL = 0, s_pkR = 0;
uint64_t s_lrDiff = 0;               // L!=R 的帧数 (0 = 源本身是双单声道)

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

// 16bit 立体声 WAV 头 (dataBytes=0 时先占位, 停止时 seek(0) 重写)
void wavHeader(File& f, uint32_t dataBytes) {
  uint8_t h[44];
  uint32_t riffSize  = 36 + dataBytes;
  uint32_t byteRate  = s_rate * OUT_BYTES_PER_FRAME;
  h[0]='R'; h[1]='I'; h[2]='F'; h[3]='F';
  h[4]=riffSize&0xFF; h[5]=(riffSize>>8)&0xFF; h[6]=(riffSize>>16)&0xFF; h[7]=(riffSize>>24)&0xFF;
  h[8]='W'; h[9]='A'; h[10]='V'; h[11]='e';
  h[12]='f'; h[13]='m'; h[14]='t'; h[15]=' ';
  h[16]=16; h[17]=0; h[18]=0; h[19]=0;                 // fmt chunk 16 字节
  h[20]=1;  h[21]=0;                                   // PCM
  h[22]=2;  h[23]=0;                                   // 2 声道 (跟随流)
  h[24]=s_rate&0xFF; h[25]=(s_rate>>8)&0xFF; h[26]=(s_rate>>16)&0xFF; h[27]=(s_rate>>24)&0xFF;
  h[28]=byteRate&0xFF; h[29]=(byteRate>>8)&0xFF; h[30]=(byteRate>>16)&0xFF; h[31]=(byteRate>>24)&0xFF;
  h[32]=OUT_BYTES_PER_FRAME; h[33]=0;                  // blockAlign = 4
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

// 写盘任务: 每 15ms 醒一次把环冲写向 SD; 每次最多 8KB 且马上释放锁,
// 绝不长时间独占 (主循环/netTask 都不受影响)
void writerTask(void*) {
  while (s_taskOn) {
    int burst = 0;
    while (s_taskOn && s_on && (s_wr != s_rd) && burst < 8 && !s_werr) {
      if (g_spiMutex && xSemaphoreTake(g_spiMutex, pdMS_TO_TICKS(30)) != pdTRUE) break;
      uint32_t t0 = millis();
      drain(8192);
      uint32_t dt = millis() - t0;
      xSemaphoreGive(g_spiMutex);
      if (dt > s_wrMaxMs) s_wrMaxMs = dt;
      burst++;
    }
    vTaskDelay(pdMS_TO_TICKS(15));
  }
  s_task = nullptr;
  vTaskDelete(nullptr);
}

// audio 任务回调 (由 src/audio_raw_hook.cpp 的强符号转发进来)
void feedPcm(int32_t* buf, int16_t words) {
  if (!s_on || !s_ring) return;
  size_t frames = (size_t)words >> 1;              // 交错 L/R: words 是 32bit 个数
  size_t need = frames * OUT_BYTES_PER_FRAME;
  if (s_ringSize - (s_wr - s_rd) < need) { s_drop += (uint32_t)need; return; }
  size_t w = s_wr % s_ringSize;
  uint32_t pkL = s_pkL, pkR = s_pkR;
  uint64_t diff = 0;
  for (size_t i = 0; i < frames; i++) {
    int16_t vl = (int16_t)(buf[i * 2] >> 16);      // 16bit 有效值在高半字
    int16_t vr = (int16_t)(buf[i * 2 + 1] >> 16);
    uint32_t aL = (uint32_t)(vl < 0 ? -(int32_t)vl : (int32_t)vl);
    uint32_t aR = (uint32_t)(vr < 0 ? -(int32_t)vr : (int32_t)vr);
    if (aL > pkL) pkL = aL;
    if (aR > pkR) pkR = aR;
    if (vl != vr) diff++;
    s_ring[w] = (uint8_t)(vl & 0xFF);        if (++w >= s_ringSize) w = 0;
    s_ring[w] = (uint8_t)((vl >> 8) & 0xFF); if (++w >= s_ringSize) w = 0;
    s_ring[w] = (uint8_t)(vr & 0xFF);        if (++w >= s_ringSize) w = 0;
    s_ring[w] = (uint8_t)((vr >> 8) & 0xFF); if (++w >= s_ringSize) w = 0;
  }
  s_wr += need;
  s_pkL = pkL; s_pkR = pkR;
  s_lrDiff += diff;
  s_frames += frames;
}

// 收尾自检: 回读刚写完的文件, 核对头 + 量内容峰值
void verifyFile() {
  uint32_t fsz = 0, hdrData = 0, hdrRate = 0, hdrCh = 0, bits = 0;
  uint32_t peakF = 0;
  int seekOk = 0;
  File r = SD.open(s_name, "r");
  if (r) {
    fsz = (uint32_t)r.size();
    uint8_t h[44] = {0};
    if (r.seek(0)) seekOk = 1;                      // File::seek 返回 bool (true=成功)
    if (seekOk && r.read(h, 44) == 44) {
      hdrCh   = (uint32_t)(h[22] | (h[23] << 8));
      hdrRate = (uint32_t)h[24] | ((uint32_t)h[25] << 8) | ((uint32_t)h[26] << 16) | ((uint32_t)h[27] << 24);
      bits    = (uint32_t)(h[34] | (h[35] << 8));
      hdrData = (uint32_t)h[40] | ((uint32_t)h[41] << 8) | ((uint32_t)h[42] << 16) | ((uint32_t)h[43] << 24);
    }
    if (r.seek(44)) {
      uint8_t b[2048];
      size_t n = r.read(b, sizeof(b));
      for (size_t i = 0; i + 1 < n; i += 2) {
        int16_t v = (int16_t)(b[i] | (b[i + 1] << 8));
        uint32_t a = (uint32_t)(v < 0 ? -(int32_t)v : (int32_t)v);
        if (a > peakF) peakF = a;
      }
    }
    r.close();
  }
  char line[220];
  snprintf(line, sizeof(line),
           "%s fsz=%u exp=%u seek=%d hdrData=%u hdrRate=%u ch=%u bits=%u | peakF=%u pkL=%u pkR=%u lrDiff=%llu frames=%llu drop=%u%s",
           s_name, (unsigned)fsz, (unsigned)(44 + s_bytes), seekOk,
           (unsigned)hdrData, (unsigned)hdrRate, (unsigned)hdrCh, (unsigned)bits,
           (unsigned)peakF, (unsigned)s_pkL, (unsigned)s_pkR,
           (unsigned long long)s_lrDiff, (unsigned long long)s_frames,
           (unsigned)s_drop, s_werr ? " WRITE-ERR" : "");
  Serial.printf("[RAD] diag %s\n", line);
  // 存到 SD (LittleFS 只放字体/配置, 别占; SD 上开机再读出来打印)
  File d = SD.open("/rad_diag.txt", "w");
  if (d) { d.print(line); d.close(); }
}

// 读 WAV 头 + 抽查内容峰值 (整卷等分 16 段各读 512B), 用来判断"这卷录进去到底有没有声音"
bool radFileInfo(File& f, uint32_t sz, uint32_t* rate, uint16_t* ch, uint16_t* bits,
                 uint32_t* dataLen, uint32_t* peak) {
  *rate = 0; *ch = 0; *bits = 0; *dataLen = 0; *peak = 0;
  if (sz < 44) return false;
  uint8_t h[44] = {0};
  if (!f.seek(0)) return false;
  if (f.read(h, 44) != 44) return false;
  if (memcmp(h, "RIFF", 4) != 0) return false;
  *ch   = (uint16_t)(h[22] | (h[23] << 8));
  *rate = (uint32_t)h[24] | ((uint32_t)h[25] << 8) | ((uint32_t)h[26] << 16) | ((uint32_t)h[27] << 24);
  *bits = (uint16_t)(h[34] | (h[35] << 8));
  *dataLen = (uint32_t)h[40] | ((uint32_t)h[41] << 8) | ((uint32_t)h[42] << 16) | ((uint32_t)h[43] << 24);
  uint32_t step = sz / 16;
  if (step < 4096) step = 4096;
  uint8_t buf[512];
  uint32_t pk = 0;
  for (uint32_t off = 44; off + 2 < sz; off += step) {
    if (!f.seek(off)) break;
    size_t n = f.read(buf, sizeof(buf));
    for (size_t i = 0; i + 1 < n; i += 2) {
      int16_t v = (int16_t)(buf[i] | (buf[i + 1] << 8));
      uint32_t a = (uint32_t)(v < 0 ? -(int32_t)v : (int32_t)v);
      if (a > pk) pk = a;
    }
  }
  *peak = pk;
  return true;
}

}  // namespace

namespace radiorec {

void init() {
  // ① 卡上到底有什么: 每卷 rad_ 的头 + 内容峰值 (抽查 16 段, 判断"录进去有没有声音")
  if (sdstore::mounted()) {
    int nRec = 0, nRad = 0;
    File root = SD.open("/");
    if (root) {
      File f;
      while ((f = root.openNextFile())) {
        const char* p = f.name();
        if (p) {
          const char* b = strrchr(p, '/');
          b = b ? (b + 1) : p;
          if (strncmp(b, "rec_", 4) == 0) nRec++;
          if (strncmp(b, "rad_", 4) == 0) {
            nRad++;
            uint32_t sz = (uint32_t)f.size();
            uint32_t rate = 0, dl = 0, pk = 0, dur = 0;
            uint16_t ch = 0, bits = 0;
            if (radFileInfo(f, sz, &rate, &ch, &bits, &dl, &pk)) {
              uint32_t bps = rate * (ch ? ch : 1) * (bits ? bits / 8 : 2);
              dur = bps ? dl / bps : 0;
            }
            Serial.printf("[RAD]   %s sz=%uK rate=%u ch=%u bits=%u dur=%us data=%uK pk=%u\n",
                          b, (unsigned)(sz >> 10), (unsigned)rate, (unsigned)ch,
                          (unsigned)bits, (unsigned)dur, (unsigned)(dl >> 10), (unsigned)pk);
          }
        }
        f.close();
      }
      if (f) f.close();
      root.close();
    }
    Serial.printf("[RAD] sd tapes rec=%d rad=%d\n", nRec, nRad);

    // ② 上次录完的自检 (存 SD, LittleFS 空间紧)
    File d = SD.open("/rad_diag.txt", "r");
    if (d) {
      String s = d.readString();
      d.close();
      s.trim();
      if (s.length()) Serial.printf("[RAD] PREV diag %s\n", s.c_str());
    }
  } else {
    Serial.println("[RAD] init: SD not mounted");
  }
  // ③ 断电/复位打断的录音: 头里 data 长度还是 0, 但数据已经写进去了 ->
  // 按真实文件大小补头, 否则这卷在录音机索引里既没时长也播不出来
  if (sdstore::mounted()) {
    File root = SD.open("/");
    if (root) {
      File f;
      while ((f = root.openNextFile())) {
        const char* p = f.name();
        if (p) {
          const char* b = strrchr(p, '/');
          b = b ? (b + 1) : p;
          if (strncmp(b, "rad_", 4) == 0) {
            uint32_t sz = (uint32_t)f.size();
            if (sz > 44) {
              uint8_t h[44] = {0};
              f.seek(0);
              if (f.read(h, 44) == 44) {
                uint32_t dl = (uint32_t)h[40] | ((uint32_t)h[41] << 8) |
                              ((uint32_t)h[42] << 16) | ((uint32_t)h[43] << 24);
                if (dl != sz - 44) {
                  uint32_t riff = sz - 8;
                  h[4]=riff&0xFF; h[5]=(riff>>8)&0xFF; h[6]=(riff>>16)&0xFF; h[7]=(riff>>24)&0xFF;
                  uint32_t nd = sz - 44;
                  h[40]=nd&0xFF; h[41]=(nd>>8)&0xFF; h[42]=(nd>>16)&0xFF; h[43]=(nd>>24)&0xFF;
                  f.seek(0);
                  f.write(h, 44);
                  Serial.printf("[RAD] repair /%s  data %u -> %u\n", b, (unsigned)dl, (unsigned)nd);
                }
              }
            }
          }
        }
        f.close();
      }
      if (f) f.close();
      root.close();
    }
  }
}

void hookRawSamples(int32_t* buf, int16_t words) { feedPcm(buf, words); }

bool     active()      { return s_on; }
uint32_t sampleRate()  { return s_rate; }
uint32_t droppedBytes(){ return s_drop; }
const char* fileName() { return s_name; }
uint32_t elapsedSec()  { return s_rate ? (uint32_t)(s_frames / s_rate) : 0; }

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

  // PSRAM 环冲 (立体声 4B/帧: 1MB ≈ 5.8s @44.1k), 逐级回退, 不占内部 RAM
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
  s_frames = 0;
  s_pkL = s_pkR = 0;
  s_lrDiff = 0;
  s_wrMaxMs = 0;
  s_statMs = millis();
  s_lastBytes = 0;
  s_stallMs = millis();
  s_on = true;
  s_tickMs = millis();
  // 写盘独立任务 (core0, 优先级 1): 主循环不再碰 SD
  s_taskOn = true;
  xTaskCreatePinnedToCore(&writerTask, "radWriter", 4096, nullptr, 1, &s_task, 0);
  Serial.printf("[RAD] start -> %s  %uHz stereo ring=%uKB file=%uB\n", s_name, (unsigned)s_rate,
                (unsigned)(s_ringSize >> 10), (unsigned)s_file.size());
  return true;
}

void stop() {
  if (!s_on) return;
  s_on = false;                       // 先停生产者
  delay(20);                          // 等 audio 任务把这批样本写完 (回调周期 ~6ms)
  if (s_taskOn) {                     // 再停写盘任务 (它 15ms 一轮, 最多占锁 30ms)
    s_taskOn = false;
    delay(60);
  }
  if (g_spiMutex) xSemaphoreTake(g_spiMutex, portMAX_DELAY);
  if (s_file) {
    drain(s_ringSize);                // 剩下的全部落盘
    s_file.flush();
    s_file.seek(0);
    wavHeader(s_file, s_bytes);       // 补齐长度
    s_file.close();
  }
  Serial.printf("[RAD] saved %s  %us  %uB  rate=%u  drop=%u  wrMax=%ums%s\n",
                s_name, (unsigned)(s_rate ? s_frames / s_rate : 0), (unsigned)s_bytes,
                (unsigned)s_rate, (unsigned)s_drop, (unsigned)s_wrMaxMs,
                s_werr ? "  WRITE-ERR" : "");
  verifyFile();
  if (g_spiMutex) xSemaphoreGive(g_spiMutex);
  if (s_ring) { heap_caps_free(s_ring); s_ring = nullptr; }
  s_ringSize = 0;
  s_rate = 0;
  s_werr = false;
}

void toggle() { if (s_on) stop(); else start(); }

void tick() {
  if (!s_on) return;
  if (millis() - s_tickMs < 8) return;      // 别每轮 loop 都进 (只做便宜的状态判断)
  s_tickMs = millis();

  if (millis() - s_statMs >= 15000) {       // 录音期间每 15s 一行诊断 (看是否掉数据)
    s_statMs = millis();
    Serial.printf("[RAD] stat %us bytes=%u drop=%u wrMax=%ums\n",
                  (unsigned)elapsedSec(), (unsigned)s_bytes, (unsigned)s_drop,
                  (unsigned)s_wrMaxMs);
  }

  if (s_werr) { Serial.println("[RAD] stop: write error"); stop(); return; }

  // 写盘看门狗: 有样本进来但 2s 一字节没落盘 -> 卡住/写不进去, 宁可停也别留坏文件
  if (s_bytes != s_lastBytes) {
    s_lastBytes = s_bytes;
    s_stallMs = millis();
  } else if (s_frames > 0 && millis() - s_stallMs > 2000) {
    Serial.println("[RAD] stop: no data written 2s");
    stop();
    return;
  }

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
  if (s_frames >= (uint64_t)REC_MAX_SEC * (uint64_t)s_rate) {
    Serial.println("[RAD] stop: reach 1h limit");
    stop();
    return;
  }
}

}  // namespace radiorec
