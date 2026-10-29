// ============================================================
// sd_storage.cpp — 板载 SPI TF 卡存储层 (实现见 include/sd_storage.h)
// ============================================================
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <LittleFS.h>
#include <TFT_eSPI.h>
#include "config.h"
#include "sd_storage.h"

extern TFT_eSPI tft;       // main.cpp 中的显示对象 (其 spi 实例已绑定 FSPI 引脚)

namespace sdstore {

namespace {

bool     s_mounted = false;

// 需要放到卡上的项目数据文件 (与 data/ 目录及运行期缓存一致)
const char* SEEDS[] = {
  "/stations.bin",
  "/radio_meta.bin",
  "/festival.json",
  "/holidays_2025.json",
  "/holidays_2026.json",
  "/holidays_2027.json",
  "/weather_cache.json",
};
const int SEED_N = (int)(sizeof(SEEDS) / sizeof(SEEDS[0]));

} // anon

bool init() {
  if (s_mounted) return true;
  Serial.println("[SD] mount TF card: HSPI(SPI3) CS=6 SCK=15 MOSI=7 MISO=16");

  // 2026-10-25 原理图复核 + 实测: TF 座接在 ESP32-S3 模块的相邻脚 pin6~9 ——
  //   IO6=SD_CS / IO7=SD_MOSI / IO15=SD_SCK / IO16=SD_MISO
  // 之前假设的 11/12/13 是显示屏的 SDI/SCK/SDO (与触摸共用), 所以 CMD0 永远无应答。
  // 显示屏用 FSPI(SPI2)，这里让 SD 走独立的 HSPI(SPI3)，两套引脚互不影响，
  // 不用在运行期反复切 FSPI 的引脚 (那样会打断显示/触摸)。
  static SPIClass sdSpi(HSPI);
  sdSpi.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN);   // SCK, MISO, MOSI

  // CS 预充电: 先拉高 10ms 再初始化 (防止上电时 CS 悬空被误选中)
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  delay(10);

  // 1MHz 已实测可挂载; 初始化成功后 SD 库沿用该时钟 (音频流约 16~32KB/s, 够用)。
  Serial.println("[SD] init try 1000000 Hz...");
  s_mounted = SD.begin(SD_CS_PIN, sdSpi, 1000000, SD_MOUNT_POINT, SD_MAX_FILES, false);
  Serial.printf("[SD]   -> %s\n", s_mounted ? "OK" : "fail");

  if (!s_mounted) {
    Serial.println("[SD] mount FAIL (no card / not FAT?) -> fallback LittleFS");
    return false;
  }
  Serial.printf("[SD] OK total=%lluMB used=%lluMB\n",
                (unsigned long long)(SD.totalBytes() / (1024ULL * 1024ULL)),
                (unsigned long long)(SD.usedBytes() / (1024ULL * 1024ULL)));
  return true;
}

bool mounted() { return s_mounted; }

const char* backend() { return s_mounted ? "SD" : "LittleFS"; }

File openRead(const char* path) {
  if (s_mounted && SD.exists(path)) return SD.open(path, "r");
  return LittleFS.open(path, "r");
}

File openWrite(const char* path) {
  if (s_mounted) return SD.open(path, "w");
  return LittleFS.open(path, "w");
}

bool exists(const char* path) {
  if (s_mounted && SD.exists(path)) return true;
  return LittleFS.exists(path);
}

bool makeDir(const char* path) {
  if (!s_mounted) return false;
  if (SD.exists(path)) return true;
  return SD.mkdir(path);
}

bool seedFromFlash() {
  if (!s_mounted) return false;
  bool allOk = true;
  uint8_t buf[512];
  for (int i = 0; i < SEED_N; i++) {
    const char* p = SEEDS[i];
    if (SD.exists(p)) {
      // 2026-10-25: 二进制电台库可能因断电/半拷贝变成坏文件, 直接以卡为准会让
      // 电台列表几乎全空(实测 "bad format" -> 只剩 24 台)。这里校验魔数,
      // 坏文件删除后由 Flash 版重建; JSON 等文本文件仍以卡上为准(可能是用户编辑的)。
      const char* magic = nullptr;
      if      (!strcmp(p, "/stations.bin"))  magic = "ESP32RDB";
      else if (!strcmp(p, "/radio_meta.bin")) magic = "ESP32RDT";
      if (magic) {
        File chk = SD.open(p, "r");
        char hdr[8] = {0};
        int n = chk ? (int)chk.read((uint8_t*)hdr, 8) : 0;
        if (chk) chk.close();
        if (n == 8 && memcmp(hdr, magic, 8) == 0) continue;   // 正常, 保留卡上版本
        Serial.printf("[SD] %s 校验失败 (读 %d B) -> 用 Flash 版覆盖\n", p, n);
        SD.remove(p);
      } else {
        continue;
      }
    }
    if (!LittleFS.exists(p)) continue;      // Flash 里也没有 (如尚未生成的缓存)
    File src = LittleFS.open(p, "r");
    if (!src) { allOk = false; continue; }
    File dst = SD.open(p, "w");
    if (!dst) { src.close(); allOk = false; Serial.printf("[SD] seed %s: open write FAIL\n", p); continue; }
    size_t total = 0, n;
    while ((n = src.read(buf, sizeof(buf))) > 0) {
      if (dst.write(buf, n) != n) { allOk = false; break; }
      total += n;
    }
    dst.close();
    src.close();
    Serial.printf("[SD] seed %-22s %u B\n", p, (unsigned)total);
  }
  makeDir("/music");                        // 节日/节气音乐存放目录
  return allOk;
}

uint64_t totalBytes() { return s_mounted ? (uint64_t)SD.totalBytes() : (uint64_t)LittleFS.totalBytes(); }
uint64_t usedBytes()  { return s_mounted ? (uint64_t)SD.usedBytes()  : (uint64_t)LittleFS.usedBytes();  }

} // namespace sdstore
