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
// 2026-10-25 二次调整: BCK/LRC 从 43/44 挪到 18/5。
//   GPIO43/44 = UART0 的 RXD0/TXD0, 硬件直连 CH340X USB 串口;
//   I2S 一旦占用这两脚, CH340 就再也收不到任何 Serial 日志。
//   18/5 原为 KEY1/KEY3 按键 (固件中从未读取), 现改作 I2S 时钟,
//   接线时请拔掉 KEY1/KEY3 的按键排线 (按键会把时钟线拉到 GND)。
//   DOUT 保持 45 不变 (45 是 strapping 脚, 启动后作普通输出正常)。
// 历史: 6/7/8 → 43/44/45 (GPIO6=TF卡CS, 7/8=GY-39), 现 43/44 归还 UART0。
// ============================================================
#define I2S_BCLK_PIN        18      // I2S位时钟 → PCM5102A BCK (原 43)
#define I2S_LRC_PIN         5       // I2S左右声道时钟 → PCM5102A LCK (原 44)
#define I2S_DOUT_PIN        45      // I2S数据输出 → PCM5102A DIN
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
// 引脚分配 - 板载 SPI TF 卡 (2026-10-25 实测更正)
// 原理图 + 实测: TF 座接在模块相邻脚 pin6~9 ——
//   IO6=SD_CS / IO7=SD_MOSI / IO15=SD_SCK / IO16=SD_MISO
// 之前的 11/12/13 是显示屏的 SDI/SCK/SDO (与触摸共用), 接 SD 永远 CMD0 无应答。
// 显示走 FSPI(SPI2), SD 走独立 HSPI(SPI3), 两套引脚互不干扰。
// ============================================================
#define SD_CS_PIN           6
#define SD_SCK_PIN          15
#define SD_MOSI_PIN         7
#define SD_MISO_PIN         16
#define SD_SPI_FREQ         1000000   // 兜底 1MHz (长线/劣质卡不稳时回退到这一级)
#define SD_SPI_FAST_FREQ    20000000  // 2026-11-01: 首选 20MHz (SD 库内部仍用 400kHz 完成 CMD0/ACMD41)
#define SD_MOUNT_POINT      "/sdcard"
#define SD_MAX_FILES        5

// ============================================================
// 引脚分配 - GY-39 四合一气象传感器 (UART2)
// 2026-10-25 (最终): CT(模块TX) -> GPIO8 = UART2 RX; DR(模块RX) -> GPIO2 = UART2 TX。
//   GY-39 出厂不一定自动输出, 需固件发 A5 81 26 / A5 82 27 开启连续输出。
//   GPIO2 因此归 GY-39 使用, 单色状态灯取消, 状态指示只保留板载 WS2812(GPIO48)。
//   GPIO7 是 TF 卡 SD_MOSI, 不能接 GY-39。
// 模块仅可 3.3V 供电。
// ============================================================
#define GY39_RX_PIN         8       // 只收: CT(模块TX) -> GPIO8
#define GY39_TX_PIN         2       // 发命令: DR(模块RX) -> GPIO2 (原单色灯脚, 灯已取消)
#define GY39_BAUD           9600
// GY-39 的两条"连续输出"指令互斥: 发 0x81 只出光照帧, 发 0x82 只出温湿压海拔帧,
// 没有"同时输出"的指令, 所以固件按窗口周期切换。
// 历史上 15s/105s 与 60s/60s 两版: 温湿度有一半时间收不到帧, 室内温度最长要等 60~105s
// 才更新一次 -> 体感"很不实时"。现改为"温湿压长窗 + 光照短窗":
//   0x82 60s (室内温湿度持续 ~2s 一帧, 几乎实时) → 0x81 只停 5s 取几帧光照 → 回到 0x82。
//   温湿度最长中断 5s; lux 每 ~65s 刷新一次 (自动背光/夜间熄屏的秒级响应不受影响)。
#define GY39_TEMP_DWELL_MS  60000   // 0x82 温湿压海拔窗口
#define GY39_LUX_DWELL_MS   5000    // 0x81 光照窗口 (只取几帧足够背光用)
#define GY39_STALE_MS       150000  // 温湿压失效阈值 (须 > 一个完整周期 65s)
#define GY39_LUX_STALE_MS   90000   // 光照失效阈值 (须 > 一个完整周期 65s)

// ============================================================
// 背光自动亮度 / 夜间熄屏 (GPIO21, LEDC PWM)
// ============================================================
#define BL_PWM_FREQ         5000
#define BL_PWM_BITS         10      // 0..1023
#define BL_NIGHT_LUX        10.0f   // 低于该光照视为"夜间" (关灯后室内残余光通常 5~15lx)
#define BL_NIGHT_HOLD_MS    5000    // 持续黑暗多久后熄屏 (5s, 触发更灵敏)
#define BL_WAKE_MS          20000   // 触摸唤醒后强制点亮时长 (黑暗环境里保证有足够操作时间)

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
#define TOUCH_IRQ_PIN       4       // 触摸中断 → T_IRQ (GPIO38被PSRAM SPICS1钳制,改用GPIO4)
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
// 2026-10-25: KEY1(18)/KEY3(5) 已改作 I2S0 BCK/LRC (见上), 按键功能取消;
// KEY2(4) 早已因与触摸 T_IRQ 冲突移除。现仅剩 BOOT(GPIO0) 物理按键。
// ============================================================
#define KEY_BOOT_PIN        0       // BOOT 按键 (strapping, 仅输入)
#define KEY_DEBOUNCE_MS     50      // 按键消抖时间

// ============================================================
// 引脚分配 - 状态指示
// ============================================================
#define STATUS_LED_PIN      -1      // 单色状态LED 已取消: GPIO2 改作 GY-39 UART2-TX (DR)
#define STATUS_RGB_PIN      48      // 板载 WS2812B-5050RGB (原理图 LED2, DIN←IO48)
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
#define WEATHER_API_KEY         "49ae5235d64149ebaf6f15a8ece324d9"   // 与 WinCalender 同一 key
#define WEATHER_API_HOST        "p85khy6vmp.re.qweatherapi.com"        // 和风控制台分配的专属 API Host
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
