/*
 * recorder.cpp - 卡带录音机 (v2, 2026-11-08)
 *
 * 交互 (对齐 recorder_ui/卡带录音机.html, 并按实机反馈修订):
 *   ① 收音机顶栏 REC 只负责「打开录音机界面」(recorderStartFromUi), 不立即录音。
 *   ② 界面内点底部琴键 REC 才开始录; 再点 REC / STOP 停止 -> 回到待机面板,
 *      可以反复录多卷 (每次 pickFileName 取下一个 /rec_XXXX.wav, 不再自动回放)。
 *   ③ PLAY 键: 打开「磁带索引」覆盖层, 选一盘磁带才播放 (满足"选 type 再播放")。
 *   ④ 退出路径 3 条: 面板右上 ✕ 圆钮 / 待机时 STOP 键 / BOOT 短按。
 *   ⑤ 单次录音上限 REC_MAX_SEC (=3600s/60min, config.h), 面板右侧印 "MAX 1H";
 *      到点自动停机(回待机, 不自动回放)。16kHz/16bit 单声道 ≈ 32KB/s -> 60min ≈ 115MB。
 *
 * 绘制/内存铁律 (AGENTS.md R1-R5):
 *   - deck_face.bin 300KB 背景在 PSRAM, 但【绝不直接 pushImage】(R3 八线 PSRAM 直推
 *     = 彩虹花屏)。全部经 pushBgRect() 把行块 memcpy 到内部 RAM 再 push (R3)。
 *   - 卷盘旋转【不再用 TFT_eSprite::pushRotated】(透明色 0xF81F 与 sprite 内部存储
 *     0x1FF8 字节序不一致 -> 扣色失败 -> 30x30 方块被整块推上屏, 这就是实机看到的
 *     "卷盘外围四方的外框"), 改为在本文件内部 RAM 的 patch 缓冲里做反向双线性采样,
 *     圆形裁剪 + 背景无缝衔接, 一次 pushImage 完成 (同时满足 R1/R2/R4)。
 *   - 所有动态元素(卷盘/计数器/电平/红光/琴键按下态)都是「内部 RAM 整帧合成 -> 单次
 *     pushImage」, 不做"先擦后画"两段式 (R2)。
 *   - 不常驻 PSRAM: 进界面 loadDeckFace(), 退出 freeDeckFace() (保护时钟大连续块)。
 */
#include <Arduino.h>
#include <stdarg.h>
#include <math.h>
#include <SD.h>
#include <LittleFS.h>
#include <TFT_eSPI.h>
#include <Audio.h>
#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <esp_err.h>
#include "config.h"
#include "recorder.h"
#include "sd_storage.h"
#include "touch_driver.h"   // s_touchDown / s_touchX / s_touchY (touchScanTask 单写, 本文件只读)
#include "deck_reels.h"     // 卡带卷盘 hub 位图 (30x30 RGB565)
#include "xfont.h"          // font16 (中文提示/磁带索引)
#include "backlight.h"      // 2026-11-08: 进录音机强制 100% 背光 (照片底图偏暗问题)

// ---- main.cpp 全局 ----
extern TFT_eSPI tft;
extern Audio audio;
extern XFont font16;
extern bool radioPlaying;
extern bool musicPlaying;
extern int  pendingPlaySid;
extern bool modeChanged;
extern bool navDirty;
extern bool navPeek;
extern SemaphoreHandle_t g_printMutex;
extern void carouselPoke();                  // main.cpp: 重置闲置轮播计时器

namespace {

// ============================================================
// 卡带面板布局 (逻辑坐标 480x320, 与 recorder_ui/卡带录音机.html 一致)
// ============================================================
static const int16_t REEL_LX = 144, REEL_LY = 94;   // 左卷盘中心
static const int16_t REEL_RX = 246, REEL_RY = 94;   // 右卷盘中心
static const int16_t REEL_R  = 15;                   // 卷盘半径

// 机械计数器窗
static const int16_t CNT_X = 381, CNT_Y = 34, CNT_W = 66, CNT_H = 27;
// RECORD 文字中心 (面板印刷)
static const int16_t REC_CX = 83, REC_CY = 233;
// 9 格 LED 电平表
static const int16_t LV_X0 = 370, LV_Y0 = 150, LV_BW = 6, LV_GAP = 4, LV_N = 9;
// 琴键
static const int16_t KEY_TOP = 252, KEY_BOT = 300;
static const int16_t KEY_BND[] = {63, 107, 151, 195, 250, 294, 342};
enum KeyId : uint8_t { KEY_REC = 0, KEY_REW, KEY_FF, KEY_PLAY, KEY_STOP, KEY_PAUSE, KEY_NONE = 0xFF };
// 右上角退出圆钮
static const int16_t EXIT_X = 434, EXIT_Y = 4, EXIT_W = 42, EXIT_H = 22;
// 右侧磁带信息区 (3 行)
static const int16_t PANEL_X = 362, PANEL_Y = 80, PANEL_W = 118, PANEL_H = 64;
// 左上角"选带"钮 (2026-11-08): PLAY 短按只播装入的那卷, 选带路径必须有一个看得见的入口
static const int16_t SEL_X = 8, SEL_Y = 4, SEL_W = 70, SEL_H = 22;

// 颜色 (RGB565)
static const uint16_t C_GREEN  = 0x4D72;   // #8fa961 绿
static const uint16_t C_AMBER  = 0xC9C8;   // #c58a43 琥珀
static const uint16_t C_RED    = 0xD286;   // #d0513c 红
static const uint16_t C_TAPE   = 0x39E7;   // #3b3a35 深灰 (磁带文字)
static const uint16_t C_LBL    = 0x630C;   // #6c675c 标签灰
static const uint16_t C_DIM    = 0x5AAB;   // #5d5850 暗灰
static const uint16_t C_SPEC   = 0x8410;   // #8c2f24 暗红 (RECORDING 字)
static const uint16_t C_CREAM  = 0xEF7D;   // #efe7d2 奶油 (磁带卡片)
static const uint16_t C_INK    = 0x2944;   // #2f2b23 深墨 (卡片主文字)
static const uint16_t C_SEL    = 0xE6B7;   // #e2d6b9 卡片选中行
static const uint16_t C_LINE   = 0x9C72;   // 卡片分隔线

// ============================================================
// 状态
// ============================================================
enum class RecState : uint8_t { IDLE, DECK, RECORDING, PLAYING };
RecState s_state = RecState::IDLE;

i2s_chan_handle_t s_rx = nullptr;
File     s_file;
char     s_fileName[20] = "";
uint32_t s_dataBytes = 0;
uint32_t s_startMs   = 0;

// BOOT 按键消抖状态 (低电平=按下)
uint8_t  s_btnStable = HIGH;
uint8_t  s_btnRaw    = HIGH;
uint32_t s_rawChgMs  = 0;
uint32_t s_pressMs   = 0;
bool     s_ignoreUntilRelease = false;
bool     s_touchReady = false;            // 触摸沿检测: 释放后才允许下一次触发
bool     s_blockTouchUntilRelease = false;
uint32_t s_noRunSince = 0;
bool     s_paused = false;

// ---- 卡带 UI 状态 ----
uint16_t* s_deckBg = nullptr;             // PSRAM: 480x320 RGB565 背景 (按需加载)
bool      s_deckLoaded = false;
uint16_t* s_bgChunk = nullptr;            // 内部 RAM: 背景中转块 (BG_ROWS 行)
uint16_t* s_patch   = nullptr;            // 内部 RAM: 动态元素整帧合成缓冲
uint16_t* s_toastBand = nullptr;          // 内部 RAM: 提示条整条横带 (480*BAND_H) 合成缓冲
int       s_pw = 0, s_ph = 0, s_px = 0, s_py = 0;
float     s_reelAng = 0.0f;
uint32_t  s_uiLastMs = 0;
int       s_lastSec  = -1;
uint16_t  s_level    = 0;
uint16_t  s_lvAcc    = 0;     // 两次绘制之间累计的峰值 (电平表灵敏度)
uint16_t  s_lvPeak   = 0;     // 峰值保持刻度
uint8_t   s_pressKey = KEY_NONE;
uint32_t  s_pressAt  = 0;
// 2026-11-08: PLAY 长按 = 打开磁带索引 (短按 = 播放当前"装入的"那一卷)
uint8_t   s_lpKey    = KEY_NONE;
uint32_t  s_lpAt     = 0;
bool      s_lpFired  = false;
static const uint32_t LP_MS = 600;
bool      s_recPaused = false; // 录音暂停 (只停写卡, I2S 继续读空转, 不写静音)
uint32_t  s_recAccumMs = 0;    // 已录音累计毫秒 (暂停期间不计)
uint32_t  s_recSegMs   = 0;    // 当前这段录音的起点 (millis)
int       s_delArm     = -1;   // 磁带索引里已"上膛"待删的行 (-1=无)
uint32_t  s_delArmMs   = 0;

static const int PATCH_MAXW = 128, PATCH_MAXH = 72;   // 面板文字区 118x64 要整块合成 (照片上不留色块)
static const int BG_ROWS    = 8;

// ---- 磁带索引 (TF 卡 /rec_XXXX.wav) ----
static const int TAPE_MAX = 40;
struct TapeInfo { char name[20]; uint32_t dur; };   // name 带前导 '/', dur 秒
TapeInfo s_tapes[TAPE_MAX];
int      s_tapeN   = 0;
int      s_curTape = 0;
bool     s_sheet   = false;
int      s_page    = 0;

// ---- 提示条 ----
char     s_toastMsg[40] = "";
uint32_t s_toastAt = 0;
bool     s_toastShown = false;
// 提示条固定几何 (2026-11-08): 宽度不随文字变化, 文字居中 —— 右缘不再紧贴最后一个字的
// 16px 字形块, 避免"XX 右上角一个色块"这类边缘毛刺。280 宽正好止于计数器(381)左侧。
static const int TOAST_H = 22;
static const int TOAST_Y = 30;
static const int TOAST_W = 280;
static const int TOAST_X = 100;
// 提示条所在的整条横带: 28..55 (选带/✕ 钮到 26, 卡片顶边 56, 正好夹在中间)。
// 2026-11-08 用户第三次报"XX 右上角色块": 干脆把这条带整条在内部 RAM 里合成、一次 pushImage,
// 这条带上的每个像素都只可能是提示条代码写的 —— 归因不再有别的可能。
static const int BAND_X = 0, BAND_W = 380;   // x 0..379: 右侧计数器从 381 起, 不互相压
static const int BAND_Y = 28, BAND_H = 28;   // y 28..55: 选带/✕ 钮到 26, 卡片顶边 56

// I2S 缓冲: 每次最多读 512 帧 (32ms @16kHz), 立体声槽 2*32bit
constexpr int REC_FRAMES = 512;
int32_t s_raw[REC_FRAMES * 2];
int16_t s_pcm[REC_FRAMES];

void logLine(const char* fmt, ...) {
  char buf[160];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
  if (g_printMutex) xSemaphoreTake(g_printMutex, portMAX_DELAY);
  Serial.println(buf);
  if (g_printMutex) xSemaphoreGive(g_printMutex);
}

// ---------- 失败诊断 (2026-11-08) ----------
// 用户无法提供串口日志, 所以失败原因要能"自己出来":
//   ① 屏上面板直接显示短码 (MIC ERR258 / FREE 27K) -> 用户拍张照片即可;
//   ② 同一份完整原因写 LittleFS /rec_fail.txt (断电不丢), 下次开机打到 boot 日志,
//      谁抓串口都能读到, 不需要用户配合操作终端。
char s_diag[2][24];

static void saveFailRecord(const char* msg) {
  File f = LittleFS.open("/rec_fail.txt", "w");
  if (!f) return;
  f.printf("%s\n", msg);
  f.close();
}

void drawPanelText();    // 前向声明 (下面 micInit 失败时要立刻把短码画到面板上)

static void micFail(const char* step, esp_err_t e, size_t fi, size_t mi) {
  snprintf(s_diag[0], sizeof(s_diag[0]), "MIC ERR%d", (int)e);
  snprintf(s_diag[1], sizeof(s_diag[1]), "FREE %uK", (unsigned)(fi >> 10));
  logLine("[REC] %s err=%s(%d) freeI=%u maxI=%u", step, esp_err_to_name(e), (int)e,
          (unsigned)fi, (unsigned)mi);
  char rec[96];
  snprintf(rec, sizeof(rec), "%s err=%s(%d) freeI=%u maxI=%u uptime=%lums",
           step, esp_err_to_name(e), (int)e, (unsigned)fi, (unsigned)mi, (unsigned long)millis());
  saveFailRecord(rec);
  drawPanelText();
}

// 开机打印上次失败原因 (读到即删, 避免每次都刷)
void reportPrevFail() {
  File f = LittleFS.open("/rec_fail.txt", "r");
  if (!f) return;
  String s = f.readString();
  f.close();
  s.trim();
  LittleFS.remove("/rec_fail.txt");
  if (s.length()) logLine("[REC] PREV FAIL -> %s", s.c_str());
}

// ---------- WAV (16kHz 单声道 16bit PCM) ----------
void writeWavHeader(File& f, uint32_t dataBytes) {
  uint8_t  h[44];
  uint32_t riffSize   = 36 + dataBytes;
  uint32_t fmtSize    = 16;
  uint16_t audioFmt   = 1;
  uint16_t channels   = 1;
  uint32_t sampleRate = MIC_SAMPLE_RATE;
  uint32_t byteRate   = MIC_SAMPLE_RATE * 2;
  uint16_t blockAlign = 2;
  uint16_t bits       = 16;
  memcpy(h + 0,  "RIFF", 4);        memcpy(h + 4,  &riffSize, 4);
  memcpy(h + 8,  "WAVE", 4);        memcpy(h + 12, "fmt ", 4);
  memcpy(h + 16, &fmtSize, 4);      memcpy(h + 20, &audioFmt, 2);
  memcpy(h + 22, &channels, 2);     memcpy(h + 24, &sampleRate, 4);
  memcpy(h + 28, &byteRate, 4);     memcpy(h + 32, &blockAlign, 2);
  memcpy(h + 34, &bits, 2);
  memcpy(h + 36, "data", 4);        memcpy(h + 40, &dataBytes, 4);
  f.write(h, 44);
}

bool pickFileName() {
  // 2026-11-08: 卷号只增不减 —— 取现有最大号 + 1, **不再填补已删除的号**。
  // (旧逻辑找第一个空号: 删掉 rec_0003 后新录音又叫 rec_0003 → 按名字降序排在列表下面,
  //  用户"找不到最新的录音"; 现在新录永远是最大号 = 列表第一行)
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
          if (strncmp(b, "rec_", 4) == 0) {
            const char* d = b + 4;
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
  // 连最新一卷被删掉也不能把号还回去: 已用过的最大号存 LittleFS /rec_seq.txt (SD 删文件不影响它)
  int seqN = 0;
  if (LittleFS.begin()) {
    File sf = LittleFS.open("/rec_seq.txt", "r");
    if (sf) { seqN = atoi(sf.readString().c_str()); sf.close(); }
  }
  int next = (maxN > seqN ? maxN : seqN) + 1;
  snprintf(s_fileName, sizeof(s_fileName), "/rec_%04d.wav", next);
  if (LittleFS.begin()) {
    File sf = LittleFS.open("/rec_seq.txt", "w");
    if (sf) { sf.print(next); sf.close(); }
  }
  return true;
}

// ---------- I2S1 RX ----------
// 2026-11-08 排查"总是麦克风初始化失败": 逐步骤打 esp_err_t 与内部 RAM 余量,
// 并把 DMA 缓冲 8×256 → 4×128 (内部 RAM 需求约 16KB → 4KB, 避免和 patch/带子抢内存)。
bool micInit() {
  if (s_rx) { i2s_channel_disable(s_rx); i2s_del_channel(s_rx); s_rx = nullptr; }  // 防残留句柄
  size_t fi0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t mi0 = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_MIC_PORT, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num  = 4;
  chan_cfg.dma_frame_num = 128;
  esp_err_t e = i2s_new_channel(&chan_cfg, NULL, &s_rx);
  if (e != ESP_OK) {
    s_rx = nullptr;
    micFail("i2s_new_channel", e, fi0, mi0);
    return false;
  }

  i2s_std_config_t std_cfg = {};
  std_cfg.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE);
  std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
  std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  std_cfg.gpio_cfg.bclk = (gpio_num_t)I2S_MIC_BCLK_PIN;
  std_cfg.gpio_cfg.ws   = (gpio_num_t)I2S_MIC_LRC_PIN;
  std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
  std_cfg.gpio_cfg.din  = (gpio_num_t)I2S_MIC_DIN_PIN;
  std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
  std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
  std_cfg.gpio_cfg.invert_flags.ws_inv   = false;
  e = i2s_channel_init_std_mode(s_rx, &std_cfg);
  if (e != ESP_OK) {
    i2s_del_channel(s_rx); s_rx = nullptr;
    micFail("i2s_init_std", e, heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return false;
  }
  e = i2s_channel_enable(s_rx);
  if (e != ESP_OK) {
    i2s_del_channel(s_rx); s_rx = nullptr;
    micFail("i2s_enable", e, heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return false;
  }
  s_diag[0][0] = 0; s_diag[1][0] = 0;          // 建链成功: 清掉面板上的失败短码
  logLine("[REC] mic ok, freeI=%u -> %u (maxBlock %u)", (unsigned)fi0,
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  return true;
}

void micDeinit() {
  if (!s_rx) return;
  i2s_channel_disable(s_rx);
  i2s_del_channel(s_rx);
  s_rx = nullptr;
}

// ============================================================
// 背景 / 缓冲 (R3: PSRAM 绝不直推)
// ============================================================
bool loadDeckFace() {
  if (s_deckLoaded && s_deckBg) return true;
  if (!LittleFS.begin()) { logLine("[REC] LittleFS mount FAIL"); return false; }
  File f = LittleFS.open("/deck_face.bin", "r");
  if (!f) { logLine("[REC] /deck_face.bin not found"); return false; }
  size_t need = 480u * 320u * 2u;
  if (f.size() != need) {
    logLine("[REC] deck_face size %u != %u", (unsigned)f.size(), (unsigned)need);
    f.close(); return false;
  }
  if (!s_deckBg) {
    s_deckBg = (uint16_t*)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_deckBg) { logLine("[REC] PSRAM alloc FAIL"); f.close(); return false; }
  }
  size_t rd = f.read((uint8_t*)s_deckBg, need);
  f.close();
  if (rd != need) { logLine("[REC] deck_face read %u/%u", (unsigned)rd, (unsigned)need); return false; }
  s_deckLoaded = true;
  logLine("[REC] deck_face loaded (%u bytes)", (unsigned)need);
  return true;
}

void ensureUiBuffers() {
  if (!s_patch) {
    // R4 说明: 这里不用 TFT_eSprite (sprite 在 PSRAM 时 pushSprite 会花屏),
    // 全部动态元素在下面的内部 RAM 线性缓冲里手工合成。
    s_patch = (uint16_t*)heap_caps_malloc(PATCH_MAXW * PATCH_MAXH * 2,
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (!s_bgChunk) {
    s_bgChunk = (uint16_t*)heap_caps_malloc(480 * BG_ROWS * 2,
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (!s_toastBand) {
    s_toastBand = (uint16_t*)heap_caps_malloc(BAND_W * BAND_H * 2,
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (!s_patch || !s_bgChunk || !s_toastBand) logLine("[REC] internal buf alloc FAIL");
}

// 从 PSRAM 背景取一块 (x,y,w,h) 经内部 RAM 中转推屏 (R3)
void pushBgRect(int x, int y, int w, int h) {
  if (!s_deckBg || !s_bgChunk) return;
  if (x < 0 || y < 0 || x + w > 480 || y + h > 320 || w <= 0 || h <= 0) return;
  tft.startWrite();
  for (int ry = 0; ry < h; ry += BG_ROWS) {
    int rows = h - ry; if (rows > BG_ROWS) rows = BG_ROWS;
    for (int r = 0; r < rows; r++) {
      memcpy(s_bgChunk + r * w, s_deckBg + (y + ry + r) * 480 + x, (size_t)w * 2);
    }
    tft.pushImage(x, y + ry, w, rows, s_bgChunk);
  }
  tft.endWrite();
}

void drawDeckBackground() { pushBgRect(0, 0, 480, 320); }

// ---------- patch 缓冲 (整帧合成 -> 单次 pushImage) ----------
bool patchBegin(int x, int y, int w, int h) {
  if (!s_deckBg || !s_patch) return false;
  if (x < 0 || y < 0 || x + w > 480 || y + h > 320) return false;
  if (w <= 0 || h <= 0 || w > PATCH_MAXW || h > PATCH_MAXH) return false;
  s_px = x; s_py = y; s_pw = w; s_ph = h;
  for (int r = 0; r < h; r++) {
    memcpy(s_patch + r * w, s_deckBg + (y + r) * 480 + x, (size_t)w * 2);
  }
  return true;
}

void patchPush() { tft.pushImage(s_px, s_py, s_pw, s_ph, s_patch); }

void patchFill(int x, int y, int w, int h, uint16_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > s_pw) w = s_pw - x;
  if (y + h > s_ph) h = s_ph - y;
  if (w <= 0 || h <= 0) return;
  for (int r = 0; r < h; r++) {
    uint16_t* p = s_patch + (y + r) * s_pw + x;
    for (int i = 0; i < w; i++) p[i] = c;
  }
}

static inline uint16_t blend565(uint16_t a, uint16_t b, float t) {
  if (t <= 0.0f) return a;
  if (t >= 1.0f) return b;
  int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  int r = (int)(ar + (br - ar) * t);
  int g = (int)(ag + (bg - ag) * t);
  int l = (int)(ab + (bb - ab) * t);
  if (r > 31) r = 31; if (g > 63) g = 63; if (l > 31) l = 31;
  return (uint16_t)((r << 11) | (g << 5) | l);
}

void patchBlend(int x, int y, uint16_t fg, float t) {
  if (x < 0 || y < 0 || x >= s_pw || y >= s_ph) return;
  s_patch[y * s_pw + x] = blend565(s_patch[y * s_pw + x], fg, t);
}

// ============================================================
// 卷盘旋转 (反向双线性采样, 圆形裁剪; 见文件头 R 说明)
// ============================================================
static inline uint16_t hubSample(const uint16_t* hub, float fx, float fy) {
  const int W = DECK_REEL_W, H = DECK_REEL_H;
  if (fx < 0) fx = 0; if (fy < 0) fy = 0;
  if (fx > W - 1) fx = W - 1; if (fy > H - 1) fy = H - 1;
  int ix = (int)fx, iy = (int)fy;
  int ix1 = (ix + 1 < W) ? ix + 1 : ix;
  int iy1 = (iy + 1 < H) ? iy + 1 : iy;
  float tx = fx - ix, ty = fy - iy;
  uint16_t p00 = hub[iy * W + ix],  p10 = hub[iy * W + ix1];
  uint16_t p01 = hub[iy1 * W + ix], p11 = hub[iy1 * W + ix1];
  float w00 = (1 - tx) * (1 - ty), w10 = tx * (1 - ty);
  float w01 = (1 - tx) * ty,       w11 = tx * ty;
  int r = (int)(((p00 >> 11) & 0x1F) * w00 + ((p10 >> 11) & 0x1F) * w10 +
                ((p01 >> 11) & 0x1F) * w01 + ((p11 >> 11) & 0x1F) * w11);
  int g = (int)(((p00 >> 5) & 0x3F) * w00 + ((p10 >> 5) & 0x3F) * w10 +
                ((p01 >> 5) & 0x3F) * w01 + ((p11 >> 5) & 0x3F) * w11);
  int b = (int)((p00 & 0x1F) * w00 + (p10 & 0x1F) * w10 +
                (p01 & 0x1F) * w01 + (p11 & 0x1F) * w11);
  // 2026-11-08: 卷盘资源来自同一张照片, 不做额外提亮 (提亮由背光 100% 负责, 免得高光过曝)
  return (uint16_t)((r << 11) | (g << 5) | b);
}

void drawReel(int16_t cx, int16_t cy, const float ca, const float sa, const uint16_t* hub) {
  const int PSZ = 34;                     // > 2*r + 边缘余量 (不能用 PS: xtensa specreg.h 有同名宏)
  if (!patchBegin(cx - PSZ / 2, cy - PSZ / 2, PSZ, PSZ)) return;
  const float rr = (float)REEL_R - 0.2f;
  const float rr2 = rr * rr;
  for (int y = 0; y < PSZ; y++) {
    for (int x = 0; x < PSZ; x++) {
      float dx = (float)x + 0.5f - PSZ * 0.5f;
      float dy = (float)y + 0.5f - PSZ * 0.5f;
      if (dx * dx + dy * dy > rr2) continue;          // 圆外保持背景 -> 无缝, 不会再出现方框
      float u =  dx * ca + dy * sa;                   // 反向旋转
      float v = -dx * sa + dy * ca;
      uint16_t c = hubSample(hub,
                             u + DECK_REEL_W * 0.5f - 0.5f,
                             v + DECK_REEL_H * 0.5f - 0.5f);
      if (c == DECK_REEL_TRANSP) continue;            // 资源透明色也不落屏 (圆边不出现品红描边)
      s_patch[y * PSZ + x] = c;
    }
  }
  patchPush();
}

void drawReels() {
  if (s_sheet) return;                    // 磁带索引卡片打开时所有"面板层"停画, 否则卷盘会压在卡片上
  float ca = cosf(s_reelAng), sa = sinf(s_reelAng);
  drawReel(REEL_LX, REEL_LY, ca, sa, DECK_REEL_L);
  drawReel(REEL_RX, REEL_RY, ca, sa, DECK_REEL_R);
}

// ============================================================
// 计数器 / 红光 / 电平 / 面板文字 / 琴键
// ============================================================
void drawCounter(uint32_t totalSec) {
  if (s_sheet) return;
  uint32_t mm = totalSec / 60; if (mm > 99) mm = 99;
  uint32_t ss = totalSec % 60;
  char buf[5];
  snprintf(buf, sizeof(buf), "%02u%02u", (unsigned)mm, (unsigned)ss);
  if (!patchBegin(CNT_X, CNT_Y, CNT_W, CNT_H)) return;
  patchFill(1, 1, CNT_W - 2, CNT_H - 2, 0x0841);
  int16_t cw = CNT_W / 4;
  for (int i = 1; i < 4; i++) patchFill(cw * i, 2, 1, CNT_H - 4, 0x0000);
  patchPush();

  tft.startWrite();
  tft.setTextColor(0xEF7D, 0x0841);
  tft.setTextFont(4);
  for (int i = 0; i < 4; i++) {
    int16_t x = CNT_X + cw * i + (cw - 12) / 2;
    tft.setCursor(x, CNT_Y + 5);
    tft.print(buf[i]);
  }
  tft.endWrite();
}

void drawRecGlow(bool recording) {
  if (s_sheet) return;
  const int GS = 38;                       // 214..252, 刚好不压到琴键顶边
  if (!patchBegin(REC_CX - GS / 2, REC_CY - GS / 2, GS, GS)) return;
  uint32_t now = millis();
  float k = recording ? (0.5f + 0.5f * (0.5f + 0.5f * sinf((float)now / 300.0f))) : 0.25f;
  float rOut = 8.0f + 10.0f * k;
  float amp  = recording ? 0.85f : 0.35f;
  for (int y = 0; y < GS; y++) {
    for (int x = 0; x < GS; x++) {
      float dx = (float)x + 0.5f - GS * 0.5f;
      float dy = (float)y + 0.5f - GS * 0.5f;
      float d  = sqrtf(dx * dx + dy * dy);
      if (d >= rOut) continue;
      float f = (rOut - d) / rOut;
      f = f * f * amp;
      patchBlend(x, y, 0xF800, f);
    }
  }
  patchPush();
}

void drawLevel() {
  if (s_sheet) return;
  const int LW = 92, LH = 16;
  if (!patchBegin(LV_X0 - 3, LV_Y0 - 3, LW, LH)) return;
  // 2026-11-08: 线性峰值对语音几乎不动(用户反馈"跳动的光标条不灵敏") → 改 dB 刻度 (-60..0dB)
  float db = (s_level > 0) ? (20.0f * log10f((float)s_level / 32768.0f)) : -90.0f;
  float norm = (db + 60.0f) / 60.0f;
  if (norm < 0.0f) norm = 0.0f; if (norm > 1.0f) norm = 1.0f;
  uint8_t lit = (uint8_t)(norm * LV_N + 0.5f);
  if (lit > LV_N) lit = LV_N;
  // 只画细刻度 (不做底色块, 不再是"收音机音频条那样的色块"贴在看板上)
  float pn = (float)s_lvPeak / 32768.0f;
  uint8_t pk = 0;
  if (s_lvPeak > 0) {
    float pdb = 20.0f * log10f(pn); float pnorm = (pdb + 60.0f) / 60.0f;
    if (pnorm < 0) pnorm = 0; if (pnorm > 1) pnorm = 1;
    pk = (uint8_t)(pnorm * (LV_N - 1) + 0.5f);
  }
  for (int i = 0; i < LV_N; i++) {
    int16_t x = i * (LV_BW + LV_GAP) + 3;
    bool on  = i < lit;
    bool hot = i >= LV_N - 3;
    if (on) patchFill(x, 3, LV_BW, 11, hot ? C_RED : (i >= LV_N - 6 ? C_AMBER : C_GREEN));
    else    patchFill(x, 11, LV_BW, 2, 0x39E7);          // 未亮刻度: 极细暗线, 看起来像印在面板上的表尺
    if (i == pk && lit > 0) patchFill(x, 1, LV_BW, 2, C_CREAM);   // 峰值保持针
  }
  patchPush();
}

const char* tapeBase(int idx) {
  if (idx < 0 || idx >= s_tapeN) return "---";
  return (s_tapes[idx].name[0] == '/') ? (s_tapes[idx].name + 1) : s_tapes[idx].name;
}

// 磁带的"卷号" = 文件名里的数字 (rec_0031.wav -> 31)。列表按名字降序, 卷号也跟着降序,
// 这样行号与文件名不再"反着"(用户 2026-11-08 反馈)。
int tapeNum(int idx) {
  if (idx < 0 || idx >= s_tapeN) return 0;
  const char* p = s_tapes[idx].name;
  while (*p && (*p < '0' || *p > '9')) p++;
  return atoi(p);
}

// 在照片底图上叠字: patch 先拷一块干净背景, 再用 XFont 的 fbDstBg 逐像素取底色合成
// → 字边缘融进金属/面板, 不会出现"一整块统一色块"(2026-11-08 用户反馈 16kHz/TAPE 有底色块)。
void drawPanelText() {
  if (s_sheet) return;
  // 2026-11-08 修: 面板用 8px 点阵字体(tft font2), 且 bg 色 == fg 色 => TFT_eSPI 走"透明背景",
  // 字直接压在照片上: 既没有奶油色块, 也不会像 font16 那样变粗变大、互相重叠/顶出面板。
  pushBgRect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H);   // 清旧字 (内部 RAM 中转)
  tft.startWrite();
  tft.setTextFont(2);

  // 面板净宽只有 PANEL_W=118px (x370..474): 左边标签 + 右边数值按实测宽度排, 相撞就不画右边那条
  // (2026-11-08 用户反馈录音态 "NEW TAPE" 与右对齐的 "MAX 1H" 叠在一起)。
  auto rowPair = [](const char* left, const char* right, int y, uint16_t lc, uint16_t rc) {
    int lw = tft.textWidth(left), rw = tft.textWidth(right);
    int rx = 474 - rw;
    if (rx < 370 + lw + 6) return;                 // 放不下: 宁可少一条信息, 也不让字叠在一起
    tft.setTextColor(lc, lc); tft.setCursor(370, y); tft.print(left);
    tft.setTextColor(rc, rc); tft.setCursor(rx, y);  tft.print(right);
  };

  if (s_state == RecState::RECORDING) {
    char mb[18];
    int mins = (REC_MAX_SEC + 59) / 60;
    if (mins >= 60 && mins % 60 == 0) snprintf(mb, sizeof(mb), "MAX %dH", mins / 60);
    else                              snprintf(mb, sizeof(mb), "MAX %dM", mins);
    // 首行标签短写 (REC/PAUSE), 上限放同一行右侧; "RECORDING" 太长放不下 MAX
    rowPair(s_recPaused ? "PAUSE" : "REC", mb, 88, C_SPEC, C_DIM);
    tft.setTextColor(C_TAPE, C_TAPE); tft.setCursor(370, 106); tft.print("NEW TAPE");
    tft.setTextColor(C_DIM, C_DIM);
    tft.setCursor(370, 124); tft.print((s_fileName[0] == '/') ? (s_fileName + 1) : s_fileName);
  } else if (s_diag[0][0]) {
    // 麦克风建立失败: 面板停着显示短码 (用户拍张照片即可, 不必提供串口日志)
    tft.setTextColor(C_AMBER, C_AMBER); tft.setCursor(370, 88);  tft.print("MIC FAIL");
    tft.setTextColor(C_DIM,   C_DIM);   tft.setCursor(370, 106); tft.print(s_diag[0]);
    tft.setCursor(370, 124); tft.print(s_diag[1]);
  } else {
    bool playing = (s_state == RecState::PLAYING);
    tft.setTextColor(playing ? C_SPEC : C_LBL, playing ? C_SPEC : C_LBL);
    tft.setCursor(370, 88);
    tft.print(playing ? (s_paused ? "PAUSED" : "PLAYING") : (s_tapeN ? "LOADED TAPE" : "EMPTY DECK"));

    char tb[18];
    if (s_tapeN == 0) snprintf(tb, sizeof(tb), "NO TAPE");
    else              snprintf(tb, sizeof(tb), "TAPE %02d", tapeNum(s_curTape));
    char db[12] = "";
    if (s_tapeN > 0) {
      uint32_t d = s_tapes[s_curTape].dur;
      snprintf(db, sizeof(db), "%02u:%02u", (unsigned)(d / 60), (unsigned)(d % 60));
    }
    rowPair(tb, db, 106, C_TAPE, C_DIM);
    tft.setTextColor(C_DIM, C_DIM);
    tft.setCursor(370, 124); tft.print(tapeBase(s_curTape));
  }
  tft.endWrite();
}

void drawKeyState(uint8_t kid, bool down) {
  if (s_sheet) return;
  if (kid > KEY_PAUSE) return;
  int16_t x0 = KEY_BND[kid], x1 = KEY_BND[kid + 1];
  if (!patchBegin(x0, KEY_TOP, x1 - x0, KEY_BOT - KEY_TOP)) return;
  if (down) {
    for (int y = 0; y < s_ph; y++) {
      float t = 0.55f;
      if (y < 10) t = 0.42f + 0.34f * (1.0f - (float)y / 10.0f);   // 顶部更深 = 下沉感
      for (int x = 0; x < s_pw; x++) patchBlend(x, y, 0x1082, t);
    }
    patchFill(0, 0, s_pw, 2, 0x18C3);
  }
  patchPush();
}

void drawStaticOverlays() {
  // 左上角"选带"钮 (打开磁带索引)
  tft.fillRoundRect(SEL_X, SEL_Y, SEL_W, SEL_H, 5, 0x18C3);
  tft.drawRoundRect(SEL_X, SEL_Y, SEL_W, SEL_H, 5, 0x5AAB);
  font16.drawString(SEL_X + 9, SEL_Y + 3, "选带", C_CREAM, 0x18C3);
  // 右上角退出圆钮 (✕)
  tft.fillRoundRect(EXIT_X, EXIT_Y, EXIT_W, EXIT_H, 5, 0x18C3);
  tft.drawRoundRect(EXIT_X, EXIT_Y, EXIT_W, EXIT_H, 5, 0x5AAB);
  int16_t cx = EXIT_X + EXIT_W / 2, cy = EXIT_Y + EXIT_H / 2;
  tft.drawLine(cx - 6, cy - 6, cx + 6, cy + 6, C_CREAM);
  tft.drawLine(cx + 6, cy - 6, cx - 6, cy + 6, C_CREAM);
  // 底部规格: 8px 点阵字体 + bg==fg (透明背景), 直接压在照片上, 不出色块也不会胀大
  tft.setTextFont(2);
  tft.setTextColor(C_LBL, C_LBL);
  tft.setCursor(370, 302);
  tft.print("16kHz  MONO");
}

// ============================================================
// 提示条 (中文 font16, 2s 后按背景整块擦除)
// ============================================================
void toastTick();

// 提示条所在整条横带 (y 28..55) 重新合成一次: drawBox=true 画提示条, false 只还原底图。
// 整带在内部 RAM 里画完再一次 pushImage —— 这条带上的每个像素都只可能来自这里。
void composeToastBand(bool drawBox) {
  if (!s_deckBg || !s_toastBand) return;
  for (int r = 0; r < BAND_H; r++)
    memcpy(s_toastBand + r * BAND_W, s_deckBg + (BAND_Y + r) * 480 + BAND_X, (size_t)BAND_W * 2);
  if (drawBox) {
    for (int r = 0; r < TOAST_H; r++) {
      uint16_t* p = s_toastBand + (TOAST_Y - BAND_Y + r) * BAND_W + (TOAST_X - BAND_X);
      for (int i = 0; i < TOAST_W; i++) p[i] = 0x18C3;
    }
    uint16_t* top = s_toastBand + (TOAST_Y - BAND_Y) * BAND_W + (TOAST_X - BAND_X);
    uint16_t* bot = s_toastBand + (TOAST_Y - BAND_Y + TOAST_H - 1) * BAND_W + (TOAST_X - BAND_X);
    for (int i = 0; i < TOAST_W; i++) { top[i] = 0x39E7; bot[i] = 0x39E7; }
    int tw = font16.textWidth(s_toastMsg);
    int tx = TOAST_X + (TOAST_W - tw) / 2;    // 居中: 两端留白, 最后一个字形块离边缘够远
    if (tx < TOAST_X + 6) tx = TOAST_X + 6;
    font16.fbBegin(s_toastBand, BAND_W, BAND_H, BAND_X, BAND_Y);   // 原点 = 带子左上角的屏幕坐标
    font16.drawString(tx, TOAST_Y + 3, s_toastMsg, C_CREAM, 0x18C3);
    font16.fbEnd();
  }
  tft.startWrite();
  tft.pushImage(BAND_X, BAND_Y, BAND_W, BAND_H, s_toastBand);
  tft.endWrite();
}

void showToast(const char* msg) {
  if (s_sheet) return;                         // 卡片打开时不弹 (会盖住卡片)
  if (s_toastShown) { composeToastBand(false); s_toastShown = false; }  // 旧条整带还原
  strncpy(s_toastMsg, msg, sizeof(s_toastMsg) - 1);
  s_toastMsg[sizeof(s_toastMsg) - 1] = 0;
  s_toastAt = millis();
  s_toastShown = false;
}

void toastTick() {
  if (s_toastMsg[0] == 0) return;
  if (!s_toastShown) {
    XFont::fbEnd();              // 防上一个界面(收音机)留下的帧缓冲目标把字画进别处
    composeToastBand(true);
    s_toastShown = true;
  }
  if (millis() - s_toastAt > 2000) {
    composeToastBand(false);
    s_toastMsg[0] = 0;
    s_toastShown = false;
  }
}

// ============================================================
// 磁带索引覆盖层 (PLAY 键 / 点行播放)
// ============================================================
static const int SH_X = 30, SH_Y = 56, SH_W = 420, SH_H = 236;
static const int SH_ROW_H = 36, SH_ROWS = 4;
struct SheetHit { int16_t x, y, w, h; uint8_t kind; int idx; };   // kind: 0=行 1=上一页 2=下一页
SheetHit s_hits[SH_ROWS * 2 + 2];      // 每行: 播放区 + 删除钮; 再加 上/下页
int      s_hitN = 0;

int sheetPages() { return (s_tapeN + SH_ROWS - 1) / SH_ROWS; }

void sheetButton(int16_t x, int16_t y, bool left) {
  tft.fillRoundRect(x, y, 26, 24, 4, C_SEL);
  tft.drawRoundRect(x, y, 26, 24, 4, C_LINE);
  int16_t cx = x + 13, cy = y + 12;
  if (left) {
    tft.drawLine(cx + 3, cy - 6, cx - 3, cy, C_INK);
    tft.drawLine(cx - 3, cy, cx + 3, cy + 6, C_INK);
  } else {
    tft.drawLine(cx - 3, cy - 6, cx + 3, cy, C_INK);
    tft.drawLine(cx + 3, cy, cx - 3, cy + 6, C_INK);
  }
}

void delButton(int16_t x, int16_t y, bool armed) {
  if (armed) {                              // 上膛: 才出现明显的琥珀底块
    tft.fillRoundRect(x, y, 20, 20, 4, C_AMBER);
    tft.drawRoundRect(x, y, 20, 20, 4, C_SPEC);
    tft.drawLine(x + 6, y + 6, x + 14, y + 14, C_CREAM);
    tft.drawLine(x + 14, y + 6, x + 6, y + 14, C_CREAM);
  } else {                                  // 平时只是一个淡叉, 不铺底块 (不再是"彩色色块")
    tft.drawLine(x + 6, y + 6, x + 14, y + 14, C_LBL);
    tft.drawLine(x + 14, y + 6, x + 6, y + 14, C_LBL);
  }
}

void drawSheet() {
  s_hitN = 0;
  int pages = sheetPages();
  if (pages < 1) pages = 1;
  if (s_page >= pages) s_page = pages - 1;
  if (s_page < 0) s_page = 0;

  tft.startWrite();
  // 卡片
  tft.fillRect(SH_X, SH_Y, SH_W, SH_H, C_CREAM);
  tft.drawRect(SH_X, SH_Y, SH_W, SH_H, C_LINE);
  tft.drawRect(SH_X + 2, SH_Y + 2, SH_W - 4, SH_H - 4, 0xE71C);
  // 标题
  font16.drawString(SH_X + 16, SH_Y + 9, "磁带索引", C_SPEC, C_CREAM);
  {
    char pb[16];
    snprintf(pb, sizeof(pb), "%d / %d", s_page + 1, pages);
    tft.setTextFont(2);
    tft.setTextColor(C_LBL, C_CREAM);
    tft.setCursor(SH_X + SH_W - 16 - tft.textWidth(pb), SH_Y + 13);
    tft.print(pb);
  }
  tft.drawFastHLine(SH_X + 16, SH_Y + 32, SH_W - 32, C_LINE);

  // 行
  if (s_tapeN == 0) {
    String s1 = "还没有磁带";
    String s2 = "按 REC 键录一卷";
    font16.drawString(SH_X + (SH_W - font16.textWidth(s1)) / 2, SH_Y + 86, s1, C_LBL, C_CREAM);
    font16.drawString(SH_X + (SH_W - font16.textWidth(s2)) / 2, SH_Y + 116, s2, C_DIM, C_CREAM);
  } else {
    char head[8];
    const int16_t DX = SH_X + SH_W - 38;            // 行内删除按钮 x
    for (int i = 0; i < SH_ROWS; i++) {
      int idx = s_page * SH_ROWS + i;
      if (idx >= s_tapeN) break;
      int16_t ry = SH_Y + 40 + i * SH_ROW_H;
      bool sel = (idx == s_curTape);
      bool arm = (idx == s_delArm);
      if (sel) {
        tft.fillRect(SH_X + 14, ry - 2, SH_W - 28, 32, C_SEL);
        tft.fillRect(SH_X + 14, ry - 2, 3, 32, C_SPEC);
      }
      uint16_t bgc = sel ? C_SEL : C_CREAM;
      // 行首数字 = 磁带自己的卷号 (与文件名一致), 不再是 01/02/03 递增 (用户反馈顺序反了)
      snprintf(head, sizeof(head), "%02d", tapeNum(idx));
      tft.setTextFont(2);
      tft.setTextColor(C_LBL, bgc);
      tft.setCursor(SH_X + 26, ry + 8);
      tft.print(head);                       // 行号
      tft.setTextColor(sel ? C_INK : C_TAPE, bgc);
      tft.setCursor(SH_X + 52, ry + 8);
      tft.print(tapeBase(idx));              // rec_0003.wav
      if (arm) {
        tft.setTextColor(C_SPEC, bgc);
        tft.setCursor(DX - 8 - tft.textWidth("DEL?"), ry + 8);
        tft.print("DEL?");
      } else {
        char db[12];
        uint32_t d = s_tapes[idx].dur;
        snprintf(db, sizeof(db), "%02u:%02u", (unsigned)(d / 60), (unsigned)(d % 60));
        tft.setTextColor(C_DIM, bgc);
        tft.setCursor(DX - 8 - tft.textWidth(db), ry + 8);
        tft.print(db);
      }
      delButton(DX, ry + 6, arm);            // 删除: 第一次点=上膛, 3s 内再点才真删
      s_hits[s_hitN++] = { DX, (int16_t)(ry + 6), 20, 20, 3, idx };   // 删除钮先入表 (它在行矩形内)
      s_hits[s_hitN++] = { (int16_t)(SH_X + 14), (int16_t)(ry - 2),
                           (int16_t)(SH_W - 28), 32, 0, idx };
    }
  }

  // 底部: 提示 + 翻页
  int16_t fy = SH_Y + SH_H - 34;
  font16.drawString(SH_X + 16, fy + 3, "点行播放 · X 删除(点两次)", C_DIM, C_CREAM);
  if (pages > 1) {
    sheetButton(SH_X + SH_W - 76, fy - 1, true);
    sheetButton(SH_X + SH_W - 44, fy - 1, false);
    s_hits[s_hitN++] = { (int16_t)(SH_X + SH_W - 76), (int16_t)(fy - 1), 26, 24, 1, 0 };
    s_hits[s_hitN++] = { (int16_t)(SH_X + SH_W - 44), (int16_t)(fy - 1), 26, 24, 2, 0 };
  }
  tft.endWrite();
}

// ============================================================
// 状态迁移
// ============================================================
void scanTapes();
void repaintDeck();
void startPlaybackAt(int idx);
void stopPlayback(bool exiting);
bool startRecordingNow();
void stopRecording(bool exiting);
void exitDeck();

void repaintDeck() {
  if (!s_deckBg) return;
  drawDeckBackground();
  drawStaticOverlays();
  drawCounter(0);
  drawPanelText();
  drawReels();
  drawRecGlow(s_state == RecState::RECORDING && !s_recPaused);
  drawLevel();
}

uint32_t counterSec() {
  if (s_state == RecState::RECORDING) {
    uint32_t ms = s_recAccumMs + (s_recPaused ? 0u : (millis() - s_recSegMs));
    return ms / 1000;                       // 暂停期间冻结 (不把暂停时间算进录音长度)
  }
  if (s_state == RecState::PLAYING)  return (uint32_t)audio.getAudioCurrentTime();
  return 0;
}

void scanTapes() {
  s_tapeN = 0;
  if (!sdstore::mounted()) return;
  File root = SD.open("/");
  if (!root) {
    logLine("[REC] tape scan: root open FAIL");
    return;
  }
  File f;
  while ((f = root.openNextFile()) && s_tapeN < TAPE_MAX) {
    const char* p = f.name();
    if (!p) { f.close(); continue; }
    const char* base = strrchr(p, '/');
    base = base ? (base + 1) : p;
    uint32_t sz = (uint32_t)f.size();
    if (strncmp(base, "rec_", 4) == 0 && sz > 44) {
      TapeInfo& t = s_tapes[s_tapeN];
      snprintf(t.name, sizeof(t.name), "/%s", base);
      t.dur = (sz - 44) / (MIC_SAMPLE_RATE * 2);
      s_tapeN++;
    }
    f.close();
  }
  if (f) f.close();          // TAPE_MAX 提前退出时最后一个句柄也要关 (SD_MAX_FILES=5)
  root.close();
  // 新的在前 (名字越大越新)
  for (int i = 1; i < s_tapeN; i++) {
    TapeInfo key = s_tapes[i];
    int j = i - 1;
    while (j >= 0 && strcmp(s_tapes[j].name, key.name) < 0) {
      s_tapes[j + 1] = s_tapes[j];
      j--;
    }
    s_tapes[j + 1] = key;
  }
  if (s_curTape >= s_tapeN) s_curTape = 0;
  logLine("[REC] tape scan: %d 卷", s_tapeN);
}

bool enterDeck() {
  if (!loadDeckFace()) {
    logLine("[REC] enter fail: deck_face");
    return false;
  }
  ensureUiBuffers();
  if (!s_patch || !s_bgChunk) return false;

  // 2026-11-08: 用户反馈"整个画面整体偏暗看不清" —— 根因是背光自动亮度 (GY-39 环境光):
  // 暗房间里只给 8~30% 亮度, 照片底图就看不清。录音机全屏页强制 100% (退出时还原)。
  backlight::setForceMax(true);

  s_state = RecState::DECK;
  s_sheet = false;
  s_page = 0;
  s_pressKey = KEY_NONE;
  s_reelAng = 0.0f;
  s_lastSec = -1;
  s_level = 0;
  s_lvAcc = 0;
  s_lvPeak = 0;
  s_paused = false;
  s_recPaused = false;
  s_recAccumMs = 0;
  s_delArm = -1;
  s_lpKey = KEY_NONE;
  s_lpFired = false;
  s_diag[0][0] = 0; s_diag[1][0] = 0;      // 新一次会话: 上次的失败短码不带到面板上
  s_toastMsg[0] = 0;
  s_toastShown = false;
  s_touchReady = false;
  s_uiLastMs = 0;
  navPeek = false;

  scanTapes();
  s_curTape = 0;
  repaintDeck();
  showToast("REC 录音 · 左上角选带");
  logLine("[REC] enter deck (%d 卷磁带) freeI=%u maxI=%u", s_tapeN,
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  return true;
}

void freeDeckFace() {
  if (s_deckBg) { heap_caps_free(s_deckBg); s_deckBg = nullptr; }
  if (s_patch)  { heap_caps_free(s_patch);  s_patch = nullptr; }
  if (s_bgChunk){ heap_caps_free(s_bgChunk); s_bgChunk = nullptr; }
  if (s_toastBand){ heap_caps_free(s_toastBand); s_toastBand = nullptr; }
  s_deckLoaded = false;
  s_pw = s_ph = 0;
}

void exitDeck() {
  if (s_state == RecState::RECORDING) stopRecording(true);
  else if (s_state == RecState::PLAYING) stopPlayback(true);
  s_sheet = false;
  s_state = RecState::IDLE;
  s_pressKey = KEY_NONE;
  s_lpKey = KEY_NONE;
  s_lpFired = false;
  freeDeckFace();
  backlight::setForceMax(false);      // 退出录音机: 背光交回自动亮度/夜间熄屏
  modeChanged = true;
  navDirty = true;
  carouselPoke();
  logLine("[REC] exit deck -> radio");
}

// ---------- 回放 ----------
void startPlaybackAt(int idx) {
  if (idx < 0 || idx >= s_tapeN) return;
  // 待机面板下电台可能还在播: 先停电台 (两个流不能同时占 I2S0)
  radioPlaying = false;
  pendingPlaySid = -1;
  musicPlaying = false;
  audio.stopSong();
  if (!audio.connecttoFS(SD, s_tapes[idx].name)) {
    logLine("[REC] playback connect FAIL %s", s_tapes[idx].name);
    showToast("播放失败");
    return;
  }
  s_curTape = idx;
  s_state   = RecState::PLAYING;
  s_startMs = millis();
  s_paused  = false;
  s_noRunSince = 0;
  s_lastSec = -1;
  s_uiLastMs = 0;
  musicPlaying = true;
  drawCounter(0);
  drawPanelText();
  char msg[24];
  snprintf(msg, sizeof(msg), "播放 TAPE %02d", tapeNum(idx));
  showToast(msg);
  logLine("[REC] playback %s", s_tapes[idx].name);
}

void stopPlayback(bool exiting) {
  if (s_state != RecState::PLAYING) return;
  audio.stopSong();
  musicPlaying = false;
  s_paused = false;
  s_state  = RecState::DECK;
  if (exiting) return;
  drawCounter(0);
  drawPanelText();
  showToast("已停止");
  logLine("[REC] playback end");
}

// ---------- 录音 ----------
bool startRecordingNow() {
  if (s_state != RecState::DECK) return false;
  if (!sdstore::mounted()) { showToast("未检测到 TF 卡"); logLine("[REC] start fail: no SD"); return false; }
  {   // 1 小时单卷最大 ~115MB, 余量低于 4MB 直接拒录 (否则会写出 0 字节坏文件)
    uint64_t tot = sdstore::totalBytes(), used = sdstore::usedBytes();
    if (tot > 0 && tot >= used && (tot - used) < (4ULL << 20)) {
      showToast("TF 卡空间不足");
      logLine("[REC] start fail: free %llu MB", (unsigned long long)((tot - used) >> 20));
      return false;
    }
  }

  radioPlaying  = false;
  musicPlaying  = false;
  pendingPlaySid = -1;
  audio.stopSong();
  navPeek = false;

  // 2026-11-08: 麦克风建立偶发失败 (同一台机器有时能有时不能, 疑似内部 RAM 余量临界 +
  // 残留句柄), 这里再重试一次; 具体失败原因由 micInit() 逐步骤的 err 码日志给出。
  if (!micInit()) {
    delay(60);
    if (!micInit()) {
      showToast("麦克风初始化失败");
      logLine("[REC] start fail: i2s (tried 2x)");
      return false;
    }
    logLine("[REC] mic init ok on retry");
  }
  if (!pickFileName()) { micDeinit(); showToast("存储已满"); return false; }
  s_file = SD.open(s_fileName, "w");
  if (!s_file) { micDeinit(); showToast("打开文件失败"); logLine("[REC] start fail: open"); return false; }

  s_dataBytes = 0;
  writeWavHeader(s_file, 0);
  s_startMs = millis();
  s_recAccumMs = 0;
  s_recSegMs   = millis();
  s_recPaused  = false;
  s_state   = RecState::RECORDING;
  s_level   = 0;
  s_lvAcc   = 0;
  s_lvPeak  = 0;
  s_lastSec = -1;
  s_uiLastMs = 0;
  drawCounter(0);
  drawPanelText();
  drawRecGlow(true);
  drawLevel();
  showToast("录音中");
  logLine("[REC] start -> %s", s_fileName);
  return true;
}

void stopRecording(bool exiting) {
  if (s_state != RecState::RECORDING) return;
  uint32_t sec = counterSec();                 // 按"实际录进卡里的时间"算 (暂停时间不计)
  s_recPaused = false;
  s_file.seek(0);
  writeWavHeader(s_file, s_dataBytes);
  s_file.close();
  micDeinit();
  s_state  = RecState::DECK;
  s_level  = 0;
  s_lvPeak = 0;
  logLine("[REC] stop %s, %lu bytes PCM (%.1fs)",
          s_fileName, (unsigned long)s_dataBytes, s_dataBytes / 32000.0f);
  if (exiting) return;
  scanTapes();                   // 新录的会排在最前 (名字最大)
  s_curTape = 0;
  drawCounter(0);
  drawPanelText();
  drawRecGlow(false);
  drawLevel();
  char msg[40];
  snprintf(msg, sizeof(msg), "已保存 %02u:%02u", (unsigned)(sec / 60), (unsigned)(sec % 60));
  showToast(msg);
}

void pumpAudio() {
  size_t br = 0;
  i2s_channel_read(s_rx, s_raw, sizeof(s_raw), &br, 0);
  int frames = (int)(br / 8);
  if (frames <= 0) return;
  if (s_recPaused) return;              // 暂停: 继续读走 I2S 数据(防 DMA 溢出), 但不写卡、不计时长

  uint16_t peak = 0;
  for (int i = 0; i < frames; i++) {
    int32_t v32 = s_raw[2 * i] >> REC_GAIN_SHIFT;
    if (v32 > 32767) v32 = 32767; else if (v32 < -32768) v32 = -32768;
    int16_t v = (int16_t)v32;
    s_pcm[i] = v;
    uint16_t a = (uint16_t)abs((int)v);
    if (a > peak) peak = a;
  }
  size_t bytes = (size_t)frames * 2;
  size_t wr = s_file.write((const uint8_t*)s_pcm, bytes);
  s_dataBytes += wr;
  if (wr != bytes) {
    logLine("[REC] SD write %u/%u -> abort", (unsigned)wr, (unsigned)bytes);
    stopRecording(false);
    return;
  }
  // 电平: 只累计"两次绘制之间的峰值", 包络衰减放到 animTick 按帧做 (否则每 loop 衰减=瞬间归零)
  if (peak > s_lvAcc) s_lvAcc = peak;
}

// ============================================================
// 交互
// ============================================================
uint8_t hitKey(int16_t tx, int16_t ty) {
  if (ty < KEY_TOP - 8 || ty > KEY_BOT + 6) return KEY_NONE;
  for (int i = 0; i < 6; i++) {
    if (tx >= KEY_BND[i] && tx < KEY_BND[i + 1]) return (uint8_t)i;
  }
  return KEY_NONE;
}

bool hitExit(int16_t tx, int16_t ty) {
  return (tx >= EXIT_X - 4 && tx <= EXIT_X + EXIT_W + 6 &&
          ty >= EXIT_Y - 4 && ty <= EXIT_Y + EXIT_H + 6);
}

void stepTape(int8_t delta) {
  if (s_tapeN == 0) { showToast("还没有磁带"); return; }
  int idx = s_curTape + delta;
  if (idx < 0) idx = 0;
  if (idx >= s_tapeN) idx = s_tapeN - 1;
  if (idx == s_curTape) { showToast(delta < 0 ? "已是第一卷" : "已是最后一卷"); return; }
  if (s_state == RecState::PLAYING) {
    audio.stopSong();
    musicPlaying = false;
    startPlaybackAt(idx);
  } else {
    s_curTape = idx;
    drawPanelText();
    char msg[24];
    snprintf(msg, sizeof(msg), "装入 TAPE %02d", tapeNum(idx));
    showToast(msg);
  }
}

// 短按 PLAY: 播放当前"装入的"那一卷 (用 ◀◀/▶▶ 或录完自动装第一卷一)
void playLoadedTape() {
  if (s_state != RecState::DECK) return;
  if (s_tapeN == 0) { showToast("还没有磁带, 按 REC 录一卷"); return; }
  startPlaybackAt(s_curTape);
}

void openSheet() {
  s_sheet = true;
  s_page = s_curTape / SH_ROWS;                 // 从当前装入的那卷所在页开始
  if (s_page < 0) s_page = 0;
  s_delArm = -1;
  // 卡片盖住琴键上半部, 先把琴键按下态还原, 免得关闭时残留半截按压色
  pushBgRect(KEY_BND[0], KEY_TOP, KEY_BND[6] - KEY_BND[0], KEY_BOT - KEY_TOP);
  s_pressKey = KEY_NONE;
  drawSheet();
}

void closeSheet() {
  s_sheet = false;
  s_delArm = -1;
  s_pressKey = KEY_NONE;
  // 2026-11-08: 卡片下面的东西被压了一整块(琴键/卷盘/面板字/电平/计数器/红光), 不再逐块拼,
  // 直接整页重铺 —— 少一块没还就会留下"卡片外的碎片"。
  repaintDeck();
}

bool sheetTap(int16_t tx, int16_t ty) {
  for (int i = 0; i < s_hitN; i++) {
    const SheetHit& h = s_hits[i];
    if (tx >= h.x && tx < h.x + h.w && ty >= h.y && ty < h.y + h.h) {
      if (h.kind == 0) {
        s_delArm = -1;
        s_curTape = h.idx;
        closeSheet();
        startPlaybackAt(h.idx);
      } else if (h.kind == 1) {
        s_delArm = -1;
        if (s_page > 0) { s_page--; drawSheet(); }
      } else if (h.kind == 2) {
        s_delArm = -1;
        if (s_page + 1 < sheetPages()) { s_page++; drawSheet(); }
      } else if (h.kind == 3) {
        // 删除磁带: 第一次点=上膛(按钮变琥珀色 + 行内提示"再点删除"), 3s 内再点才真删 SD 文件
        if (s_delArm == h.idx && millis() - s_delArmMs <= 3000) {
          const char* nm = s_tapes[h.idx].name;
          bool ok = SD.remove(nm);
          logLine("[REC] delete %s %s", nm, ok ? "OK" : "FAIL");
          s_delArm = -1;
          scanTapes();
          if (s_curTape >= s_tapeN) s_curTape = 0;
          int pages = sheetPages();
          if (s_page >= pages) s_page = pages > 0 ? pages - 1 : 0;
          drawSheet();
        } else {
          s_delArm = h.idx;
          s_delArmMs = millis();
          drawSheet();
        }
      }
      return true;
    }
  }
  if (tx < SH_X || tx > SH_X + SH_W || ty < SH_Y || ty > SH_Y + SH_H) {
    s_delArm = -1;
    closeSheet();                              // 点卡片外 = 关闭
  }
  return true;                                 // 卡片内空白处吞掉
}

// 回放中 ◀◀/▶▶ = 快退/快进 10 秒 (面板丝印就是 快进快退 符号); 待机时仍是换"装入的磁带"
void seekBy(int32_t delta) {
  if (s_state != RecState::PLAYING) return;
  int32_t cur = (int32_t)audio.getAudioCurrentTime();
  int32_t dur = (int32_t)s_tapes[s_curTape].dur;
  int32_t tgt = cur + delta;
  if (tgt < 0) { tgt = 0; showToast("已到开头"); }
  else if (dur > 0 && tgt >= dur) { tgt = dur > 1 ? dur - 1 : 0; showToast("已到结尾"); }
  else { char m[24]; snprintf(m, sizeof(m), delta > 0 ? "快进 %lds" : "快退 %lds",
                              (long)(delta > 0 ? delta : -delta)); showToast(m); }
  // 本工程实际链接的是 lib\ESP32-audioI2S (无 setAudioPlayPosition, 只有 setAudioPlayTime(sec))
  if (!audio.setAudioPlayTime((uint16_t)tgt)) showToast("无法定位");
}

void doKey(uint8_t k) {
  s_pressKey = k;
  s_pressAt = millis();
  drawKeyState(k, true);

  switch (k) {
    case KEY_REC:
      if (s_state == RecState::RECORDING) {
        stopRecording(false);
      } else if (s_state == RecState::PLAYING) {
        stopPlayback(false);
        startRecordingNow();
      } else {
        startRecordingNow();
      }
      break;

    case KEY_STOP:
      if (s_state == RecState::RECORDING) {
        stopRecording(false);
      } else if (s_state == RecState::PLAYING) {
        stopPlayback(false);
      } else {
        exitDeck();
        s_blockTouchUntilRelease = true;
      }
      break;

    case KEY_PLAY:
      if (s_state == RecState::RECORDING) {
        showToast("请先停止录音");
      } else {
        // 2026-11-08: 短按 = 播放"装入的"那一卷; 长按 = 打开磁带索引选带/删除。
        // 待机(DECK) 和回放(PLAYING) 都要能长按开索引 —— 只在 DECK 生效过一次, 用户按了半天没反应。
        s_lpKey = KEY_PLAY;
        s_lpAt = millis();
        s_lpFired = false;
      }
      break;

    case KEY_PAUSE:
      if (s_state == RecState::RECORDING) {
        if (!s_recPaused) {                                  // 录音暂停: 计数器冻结, 不写卡
          s_recAccumMs += millis() - s_recSegMs;
          s_recPaused = true;
          showToast("录音暂停");
        } else {                                             // 继续录音: 接着同一个 WAV 往下写
          s_recSegMs = millis();
          s_recPaused = false;
          showToast("继续录音");
        }
        drawPanelText();
        drawRecGlow(s_state == RecState::RECORDING && !s_recPaused);
        drawLevel();
      } else if (s_state == RecState::PLAYING) {
        if (audio.pauseResume()) {
          s_paused = !s_paused;
          s_noRunSince = 0;                          // 暂停/恢复都重新计时, 别让看门狗把暂停判成播完
          showToast(s_paused ? "已暂停" : "继续播放");
          drawPanelText();                           // 面板首行 PAUSED / PLAYING 跟着切
        }
      } else {
        showToast("无回放可暂停");
      }
      break;

    case KEY_REW:
      if (s_state == RecState::RECORDING) showToast("录音中不能切带");
      else if (s_state == RecState::PLAYING) seekBy(-10);
      else stepTape(-1);
      break;

    case KEY_FF:
      if (s_state == RecState::RECORDING) showToast("录音中不能切带");
      else if (s_state == RecState::PLAYING) seekBy(+10);
      else stepTape(+1);
      break;
  }
}

void handleTap(int16_t tx, int16_t ty) {
  if (s_sheet) { sheetTap(tx, ty); return; }
  if (hitExit(tx, ty)) {
    exitDeck();
    s_blockTouchUntilRelease = true;
    return;
  }
  // 左上角"选带"钮 = 打开磁带索引 (选带/删除); 录音中不给开
  if (tx >= SEL_X - 4 && tx <= SEL_X + SEL_W + 4 && ty >= SEL_Y - 4 && ty <= SEL_Y + SEL_H + 4) {
    if (s_state == RecState::RECORDING) showToast("录音中不能选带");
    else                                openSheet();
    return;
  }
  // 点面板上的 "TAPE 02 / 文件名" 那小块 = 直接打开磁带索引 (选带/删除)
  if (s_state != RecState::RECORDING &&
      tx >= PANEL_X && tx < PANEL_X + PANEL_W && ty >= PANEL_Y && ty < PANEL_Y + PANEL_H) {
    openSheet();
    return;
  }
  uint8_t k = hitKey(tx, ty);
  if (k != KEY_NONE) doKey(k);
}

// ---------- 动画 ----------
void animTick(bool rec) {
  if (s_sheet) return;                 // 卡片打开时冻结卷盘/计数器/电平 (面板层不许压卡片)
  uint32_t now = millis();
  uint32_t sec = counterSec();
  if (now - s_uiLastMs < 50) {                        // ~20fps (电平/卷盘跟手)
    if ((int)sec != s_lastSec) { s_lastSec = (int)sec; drawCounter(sec); }
    return;
  }
  s_uiLastMs = now;
  bool hold = (s_state == RecState::PLAYING && s_paused) ||
              (s_state == RecState::RECORDING && s_recPaused);
  if (!hold) {
    s_reelAng += rec ? 0.34f : 0.27f;
    drawReels();
  }
  if ((int)sec != s_lastSec) { s_lastSec = (int)sec; drawCounter(sec); }
  drawRecGlow(rec && !s_recPaused);
  if (rec) {
    if (s_recPaused) {                                // 暂停: 电平快速归零
      s_lvAcc = 0;
      s_level = (uint16_t)((uint32_t)s_level * 13 / 16);
      s_lvPeak = (uint16_t)((uint32_t)s_lvPeak * 13 / 16);
    } else {                                          // 瞬时峰起、按帧衰 → 说话时条会真跳
      if (s_lvAcc > s_level) s_level = s_lvAcc;
      else                   s_level = (uint16_t)((uint32_t)s_level * 13 / 16);
      s_lvAcc = 0;
      if (s_level > s_lvPeak) s_lvPeak = s_level;
      else                    s_lvPeak = (uint16_t)((uint32_t)s_lvPeak * 63 / 64);
    }
    drawLevel();
  }
}

// ---------- BOOT 按键状态机 ----------
void buttonTick() {
  uint32_t now  = millis();
  uint8_t  raw  = (uint8_t)digitalRead(REC_BTN_PIN);
  if (raw != s_btnRaw) { s_btnRaw = raw; s_rawChgMs = now; }
  if (raw != s_btnStable && (now - s_rawChgMs) >= KEY_DEBOUNCE_MS) {
    s_btnStable = raw;
    if (s_btnStable == LOW) {
      s_pressMs = now;
    } else {
      if (s_ignoreUntilRelease) s_ignoreUntilRelease = false;
      else if (s_state == RecState::RECORDING) stopRecording(false);
      else if (s_state == RecState::PLAYING)   stopPlayback(false);
      else if (s_state == RecState::DECK)      { exitDeck(); s_blockTouchUntilRelease = true; }
    }
  }
  if (s_btnStable == LOW && !s_ignoreUntilRelease && s_state == RecState::IDLE &&
      (now - s_pressMs) >= REC_LONGPRESS_MS) {
    if (enterDeck()) startRecordingNow();       // BOOT 长按 = 直接进界面并开录
    s_ignoreUntilRelease = true;
  }
}

} // anonymous namespace

// ============================================================
// 对外接口
// ============================================================
void recorderInit() {
  pinMode(REC_BTN_PIN, INPUT_PULLUP);
  s_btnStable = s_btnRaw = (uint8_t)digitalRead(REC_BTN_PIN);
  // 不常驻分配 PSRAM: deck_face 300KB 会切碎 PSRAM 连续块, 破坏时钟的大连续块需求。
  // 进界面 loadDeckFace(), 退出 freeDeckFace()。
  reportPrevFail();      // 上次录音失败的完整原因 (存 LittleFS, 断电不丢) -> boot 日志
}

bool recorderActive() { return s_state != RecState::IDLE; }

void recorderTick() {
  buttonTick();
  if (s_state == RecState::IDLE) {
    if (s_blockTouchUntilRelease && !s_touchDown) s_blockTouchUntilRelease = false;
    return;
  }

  // 触摸沿: 释放后才允许下一次触发 (进界面那一指不会被当成本界面点击)
  bool tap = false;
  int16_t tx = 0, ty = 0;
  if (s_touchDown) {
    if (s_touchReady) { s_touchReady = false; tap = true; tx = (int16_t)s_touchX; ty = (int16_t)s_touchY; }
  } else {
    s_touchReady = true;
  }
  if (tap) {
    handleTap(tx, ty);
    if (s_state == RecState::IDLE) return;         // 已退出
  }

  // 琴键按下态 240ms 后复位 (磁带索引卡片打开时不能重铺琴键下方背景, 否则会盖掉卡片)
  if (s_pressKey != KEY_NONE && millis() - s_pressAt > 240) {
    if (!s_sheet) drawKeyState(s_pressKey, false);
    s_pressKey = KEY_NONE;
  }

  // PLAY 长按: LP_MS 内按住 = 打开磁带索引 (待机/回放都行); 提前松手 = 待机时播放"装入的"那一卷
  if (s_lpKey != KEY_NONE) {
    if (s_touchDown) {
      if (!s_lpFired && s_state != RecState::RECORDING && millis() - s_lpAt >= LP_MS) {
        s_lpFired = true;
        openSheet();
      }
    } else {
      if (!s_lpFired) {
        if (s_state == RecState::DECK) playLoadedTape();
      }
      s_lpKey = KEY_NONE;
      s_lpFired = false;
    }
  }

  if (s_state == RecState::RECORDING) {
    pumpAudio();
    if (s_state != RecState::RECORDING) { toastTick(); return; }
    uint32_t elapsed = counterSec();             // 只算实际写入卡里的时长 (暂停不计)
    if (elapsed >= (uint32_t)REC_MAX_SEC) {        // 单次上限: 自动停机回到待机
      stopRecording(false);
      showToast("已到录制上限, 自动停机");
      toastTick();
      return;
    }
    animTick(true);
  } else if (s_state == RecState::PLAYING) {
    // 2026-11-08 修: 暂停后库的 isRunning() 就是 false, 旧看门狗 800ms 后判定"播完了" -> 一按暂停就自动变停止。
    // 暂停期间不跑看门狗 (用户主动静止, 不是故障); 恢复播放时 s_noRunSince 已在按键里清零。
    if (s_paused) {
      s_noRunSince = 0;
    } else if (!audio.isRunning()) {
      if (s_noRunSince == 0) s_noRunSince = millis();
      else if (millis() - s_noRunSince > 800) { stopPlayback(false); toastTick(); return; }
    } else {
      s_noRunSince = 0;
    }
    animTick(false);
  }
  toastTick();
}

void recorderStartFromUi() {
  if (s_state != RecState::IDLE) return;
  // 2026-11-08 修: 电台/节日音乐还在播时进入录音机会失败 (音频库占着 I2S0 与 PSRAM 缓冲,
  // loadDeckFace() 的 300KB 分配可能拿不到 -> enterDeck 直接返回 false, 用户只看到"点了没反应")。
  // 进界面前先停掉音频, 并把失败原因写成提示。
  if (radioPlaying || musicPlaying || pendingPlaySid >= 0) {
    radioPlaying = false;
    musicPlaying = false;
    pendingPlaySid = -1;
    audio.stopSong();
    logLine("[REC] enter: stop radio/music first");
  }
  if (!enterDeck()) {
    logLine("[REC] enter fail (deck_face / psram)");
  }
  // 顶栏 REC 只打开录音机界面, 不开始录制 (录制由面板琴键 REC 触发)
}

bool recorderBlockTouch() { return s_blockTouchUntilRelease; }
