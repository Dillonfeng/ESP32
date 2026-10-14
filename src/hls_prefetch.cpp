// hls_prefetch.cpp — BUILDV5-STEP1 (2026-10-14): 设备端双连接并发可行性验证（临时诊断）
// 目标：播放 HLS 电台（主连接下载中）的同时，独立 WiFiClient 拉 m3u8 并下载分片测速，
// 验证 ESP32-S3 WiFi 栈双连接并发（PC 探测已证 1.97x，设备端需实测）。
// ⚠️ 本文件为 STEP1 临时诊断，验证完成后并入正式预取实现或删除。
#include "hls_prefetch.h"
#include <WiFi.h>
#include <Arduino.h>
#include "esp_task_wdt.h"  // BUILDV5-STEP1f: 豁免本任务 WDT（connect/下载阻塞期间不喂狗）

// 从 m3u8 取第一个含 .ts 的分片行，拼成完整 URL（分片行可能带 query，如 905-*.ts?txspiseq=...）
static bool getFirstSegment(const char* m3u8, String& segUrl) {
  String url = m3u8;
  int scheme = url.indexOf("://");
  if (scheme < 0) return false;
  String rest = url.substring(scheme + 3);
  int slash = rest.indexOf('/');
  String host = rest.substring(0, slash < 0 ? rest.length() : slash);
  String path = slash < 0 ? "/" : rest.substring(slash);
  String base = "http://" + host + path.substring(0, path.lastIndexOf('/') + 1);

  WiFiClient c;
  if (!c.connect(host.c_str(), 80, 4000)) return false;
  vTaskDelay(100);  // 等 TCP 稳定后再发请求（cap_step1g: connect 后立即 GET 被服务器 RST ERR 128）
  c.print("GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nUser-Agent: ESP32-Radio/1.0\r\nConnection: close\r\n\r\n");
  uint32_t t0 = millis();
  String line;
  while (c.connected() && millis() - t0 < 5000) {
    if (c.available()) {
      int ch = c.read();
      if (ch == '\n') {
        if (line.indexOf(".ts") >= 0) {
          String seg = line;
          seg.trim();
          if (seg.startsWith("http")) segUrl = seg;
          else if (seg.startsWith("/")) segUrl = "http://" + host + seg;
          else segUrl = base + seg;
          c.stop();
          return true;
        }
        line = "";
      } else if (ch >= 0) {
        line += (char)ch;
      }
    } else {
      vTaskDelay(1);  // 无条件让出 CPU（防 task_wdt 复位，busy-wait 会饿死 IDLE）
    }
  }
  c.stop();
  return false;
}

// 解析 URL 为 host/path（纯 http）
static bool splitUrl(const String& url, String& host, String& path) {
  int scheme = url.indexOf("://");
  if (scheme < 0) return false;
  String rest = url.substring(scheme + 3);
  int slash = rest.indexOf('/');
  host = rest.substring(0, slash < 0 ? rest.length() : slash);
  path = slash < 0 ? "/" : rest.substring(slash);
  return true;
}

// 独立连接下载单个分片并测速（丢弃数据不解析，纯测带宽）
static void hlsPrefetchTestTask(void* arg) {
  esp_task_wdt_delete(NULL);  // BUILDV5-STEP1f: 豁免本任务 WDT（WiFiClient.connect 阻塞数秒不喂狗 → TG1WDT 复位，cap_step1e 实证）
  String seg;
  if (!getFirstSegment("http://sk.cri.cn/905.m3u8", seg)) {
    Serial.println("[PREFETCH-TEST] m3u8 parse fail");
    vTaskDelete(NULL);
    return;
  }
  String host, path;
  if (!splitUrl(seg, host, path)) {
    Serial.println("[PREFETCH-TEST] seg url parse fail");
    vTaskDelete(NULL);
    return;
  }
  WiFiClient cli;
  if (!cli.connect(host.c_str(), 80, 4000)) {
    Serial.println("[PREFETCH-TEST] connect fail");
    vTaskDelete(NULL);
    return;
  }
  vTaskDelay(100);  // 等 TCP 稳定后再发请求（cap_step1g: connect 后立即 GET 被服务器 RST ERR 128）
  cli.print("GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nUser-Agent: ESP32-Radio/1.0\r\nConnection: close\r\n\r\n");
  uint32_t t0 = millis();
  uint32_t total = 0;
  uint8_t buf[8192];
  while (cli.connected() && millis() - t0 < 10000) {
    if (cli.available()) {
      // 内层连续读：批数据到达时不等 delay(1)，读尽 available 再让出（提高并发吞吐）
      while (cli.available() && millis() - t0 < 10000) {
        int n = cli.read(buf, sizeof(buf));
        if (n > 0) total += n; else break;
      }
    }
    vTaskDelay(1);  // 让出 CPU（防 task_wdt 复位 / IDLE 饿死）
  }
  uint32_t dt = millis() - t0;
  Serial.printf("[PREFETCH-TEST] seg=%s\n", seg.c_str());
  Serial.printf("[PREFETCH-TEST] %u bytes in %u ms = %u KB/s (并发：主连接播放中)\n",
                total, dt, dt ? (total * 1000 / dt / 1024) : 0);
  cli.stop();
  vTaskDelete(NULL);
}

void hlsPrefetchTestStart() {
  // BUILDV5-STEP1f: 任务栈 16384（8192 太小——WiFiClient+String+lwip 调用栈溢出 → InstrFetchProhibited，cap_step1f 实证）
  xTaskCreate(&hlsPrefetchTestTask, "hlsPrefetchTest", 16384, NULL, 2, NULL);
}
