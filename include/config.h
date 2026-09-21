/*
 * ESP32-S3 网络收音机 - 全局配置文件
 * 硬件: ESP32-S3 N16R8 + PCM5102A + PAM8403 + INMP441 + ST7796S 4"触摸屏
 */

#ifndef _RADIO_CONFIG_H_
#define _RADIO_CONFIG_H_

#include <Arduino.h>

// ============================================================
// 版本信息
// ============================================================
#define RADIO_VERSION       "2.0.0"
#define RADIO_NAME          "Smart Internet Radio"

// ============================================================
// 文件系统配置 (LittleFS)
// ============================================================
#define USE_LITTLEFS        1
#define STATIONS_FILE       "/stations.json"     // 电台数据文件
#define FAVORITES_FILE      "/favorites.json"    // 收藏文件
#define RECENT_FILE         "/recent.json"       // 最近播放
#define SETTINGS_FILE       "/settings.json"     // 设置文件
#define STATIONS_UPLOAD_DIR "/upload/"           // 上传临时目录

// ============================================================
// WiFi 配置
// ============================================================
#define WIFI_SSID           "YourWiFiSSID"
#define WIFI_PASSWORD       "YourWiFiPassword"
#define WIFI_MAX_RETRY      20

// ============================================================
// 引脚分配 - I2S0 (音频输出 → PCM5102A DAC)
// ============================================================
#define I2S_BCLK_PIN        6       // I2S位时钟 → PCM5102A BCK
#define I2S_LRC_PIN         7       // I2S左右声道时钟 → PCM5102A LCK
#define I2S_DOUT_PIN        8       // I2S数据输出 → PCM5102A DIN
#define I2S_PORT            I2S_NUM_0

// PCM5102A 其他引脚（可选，根据模块）
// PCM5102A 模块通常有: VCC, GND, SCK, BCK, LCK, DIN, FLT, DEMP, XSMT, FMT
// FLT: 滤波器选择 (接地=延迟低, 接VCC=延迟高)
// DEMP: 去加重 (接地=关闭)
// XSMT: 软静音 (接地=静音, 接VCC=正常)
// FMT: 格式选择 (接地=I2S, 接VCC=左对齐)
#define PCM5102_XSMT_PIN    -1      // 软静音控制引脚 (-1表示不使用)

// ============================================================
// 引脚分配 - I2S1 (音频输入 ← INMP441 麦克风)
// ============================================================
#define I2S_MIC_BCLK_PIN    16      // I2S位时钟 ← INMP441 SCK
#define I2S_MIC_LRC_PIN     15      // I2S左右声道时钟 ← INMP441 WS
#define I2S_MIC_DIN_PIN     17      // I2S数据输入 ← INMP441 SD
#define I2S_MIC_PORT        I2S_NUM_1

// INMP441 麦克风配置
#define MIC_SAMPLE_RATE     16000   // 采样率 16kHz (语音识别用)
#define MIC_SAMPLE_BITS     32      // 采样位深 32bit
#define MIC_BUFFER_SIZE     512     // 缓冲区大小

// ============================================================
// 引脚分配 - SPI 显示屏 ST7796S 480x320
// ============================================================
#define TFT_SCK_PIN         12      // SPI时钟 → SCK
#define TFT_MOSI_PIN        11      // SPI数据输出 → SDI
#define TFT_MISO_PIN        13      // SPI数据输入 → SDO (触摸共享)
#define TFT_CS_PIN          10      // 显示屏片选 → CS
#define TFT_DC_PIN          9       // 数据/命令选择 → DC
#define TFT_RST_PIN         14      // 复位 → RESET
#define TFT_BL_PIN          21      // 背光控制 → LED
#define TFT_SPI_FREQ        40000000 // 40MHz SPI时钟

// 显示屏参数
#define TFT_WIDTH           480
#define TFT_HEIGHT          320
#define TFT_ROTATION        1       // 横屏显示 (0=0°, 1=90°, 2=180°, 3=270°)
#define TFT_DRIVER          ST7796

// ============================================================
// 引脚分配 - 触摸屏 XPT2046
// ============================================================
#define TOUCH_CS_PIN        1       // 触摸片选 → T_CS (GPIO20被USB占用,改用GPIO1)
#define TOUCH_IRQ_PIN       38      // 触摸中断 → T_IRQ (GPIO19被USB占用,改用GPIO38)
#define TOUCH_SPI_FREQ      2500000 // 2.5MHz SPI时钟

// 触摸校准值 (需要根据实际屏幕校准)
#define TOUCH_X_MIN         300
#define TOUCH_X_MAX         3900
#define TOUCH_Y_MIN         200
#define TOUCH_Y_MAX         3700
#define TOUCH_INVERT_X      false
#define TOUCH_INVERT_Y      true

// ============================================================
// 引脚分配 - 功能按键
// ============================================================
#define KEY1_PIN            18      // 按键1 - 音量+/功能
#define KEY2_PIN            4       // 按键2 - 音量-/功能
#define KEY3_PIN            5       // 按键3 - 模式切换/播放暂停
#define KEY_DEBOUNCE_MS     50      // 按键消抖时间

// ============================================================
// 引脚分配 - 状态指示
// ============================================================
#define STATUS_LED_PIN      2       // 状态LED (板载)
#define LED_BLINK_INTERVAL  1000    // LED闪烁间隔(ms)

// ============================================================
// 音频播放配置
// ============================================================
#define AUDIO_SAMPLE_RATE   44100   // 音频采样率
#define AUDIO_BITS_PER_SAMPLE 16    // 音频位深
#define AUDIO_VOLUME_DEFAULT 80     // 默认音量 (0-100)
#define AUDIO_VOLUME_MIN    0
#define AUDIO_VOLUME_MAX    100
#define AUDIO_BUFFER_SIZE   4096    // 音频缓冲区 (需要PSRAM支持)

// VU表配置
#define VU_BAR_COUNT        20      // 每个声道的光柱数量
#define VU_PEAK_HOLD_MS     500     // 峰值保持时间
#define VU_UPDATE_INTERVAL  30      // VU更新间隔(ms)

// ============================================================
// 天气预报配置
// ============================================================
// 和风天气API (免费: https://dev.qweather.com/)
#define WEATHER_API_ENABLED     1
#define WEATHER_API_KEY         "YOUR_QWEATHER_API_KEY"
#define WEATHER_CITY_ID         "101010100"  // 默认城市: 北京
#define WEATHER_UPDATE_INTERVAL 1800000      // 30分钟更新一次
#define WEATHER_LANG            "zh"         // 语言: 中文

// ============================================================
// 小智AI语音控制配置
// ============================================================
#define VOICE_CONTROL_ENABLED   1
#define XIAOZHI_WAKE_WORD       "你好小智"
#define XIAOZHI_API_ENDPOINT    "wss://api.xiaozhi.dev/v1/chat"
#define XIAOZHI_API_KEY         "YOUR_XIAOZHI_API_KEY"
#define XIAOZHI_DEVICE_ID       "esp32s3-radio-001"

// ============================================================
// 网络电视配置 (MJPEG流)
// ============================================================
#define NETWORK_TV_ENABLED      1
#define TV_BUFFER_SIZE          8192
#define TV_FPS_TARGET           15

// ============================================================
// 电台配置
// ============================================================
#define MAX_STATIONS            2000    // 最大电台数量 (支持1600+)
#define STATION_NAME_MAX_LEN    64      // 电台名称最大长度
#define STATION_URL_MAX_LEN     256     // 电台URL最大长度
#define STATION_GENRE_MAX_LEN   32      // 流派/分类最大长度
#define STATION_REGION_MAX_LEN  32      // 地区最大长度
#define RECENT_MAX_COUNT        20      // 最近播放电台数
#define SEARCH_RESULT_MAX       100     // 搜索结果最大数量
#define STATION_PAGE_SIZE       50      // 每分页加载数量
#define STATION_INDEX_CHARS     "ABCDEFGHIJKLMNOPQRSTUVWXYZ#"  // 字母索引

// ============================================================
// NVS 存储键名
// ============================================================
#define NVS_NAMESPACE           "radio"
#define NVS_KEY_VOLUME          "volume"
#define NVS_KEY_STATION         "station"
#define NVS_KEY_WIFI_SSID       "wifi_ssid"
#define NVS_KEY_WIFI_PASS       "wifi_pass"
#define NVS_KEY_CITY            "city"
#define NVS_KEY_BRIGHTNESS      "brightness"

// ============================================================
// 调试宏
// ============================================================
#define DEBUG_SERIAL            Serial
#define DEBUG_PRINT(x)          DEBUG_SERIAL.print(x)
#define DEBUG_PRINTLN(x)        DEBUG_SERIAL.println(x)
#define DEBUG_PRINTF(fmt, ...)  DEBUG_SERIAL.printf(fmt, ##__VA_ARGS__)

#endif // _RADIO_CONFIG_H_
