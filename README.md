# ESP32-S3 智能网络收音机

基于 ESP32-S3 N16R8 的全功能网络收音机，支持央广电台、VU表显示、语音控制、天气预报等功能。

## 硬件清单

| 模块 | 型号 | 数量 | 用途 |
|------|------|------|------|
| 主控板 | ESP32-S3 N16R8 开发板 | 1 | 主控 (16MB Flash + 8MB PSRAM) |
| 音频DAC | PCM5102A | 1 | I2S 数字音频解码 |
| 功放模块 | PAM8403 | 1 | D类数字功放 (2x3W) |
| 麦克风 | INMP441 | 1 | I2S MEMS 麦克风 (语音控制) |
| 显示屏 | 4.0" SPI TFT 触摸屏 | 1 | ST7796S, 480x320, 带触摸 |
| 喇叭 | 4Ω 3W 全频喇叭 | 2 | 立体声输出 |

## 功能特性

### 📻 网络电台播放
- 支持 MP3、AAC、FLAC、M4A、HLS(m3u8) 等多种格式
- 预置 20+ 核心电台（央广系列、地方台、音乐台、国际台）
- 支持**批量导入 2000+ 电台**（JSON / M3U / CSV 格式）
- 电台收藏、分类浏览、搜索、最近播放
- 网页能播的电台基本都能播（HLS/MP3/AAC流）

### 🌐 Web管理界面
- 浏览器访问设备IP，图形化管理电台
- 批量上传电台文件（拖拽上传）
- 电台列表浏览、搜索、分类筛选
- 远程播放控制、系统设置

### 🎵 音频显示
- **双声道 VU 表** - 实时左右声道电平显示
- **立体声指示** - STEREO/MONO 状态显示
- **电台信息** - 名称、地区、码率、格式
- **音量条** - 可视化音量显示

### 🎙 小智AI语音控制
- 离线唤醒词："你好小智"
- 语音指令支持：
  - 播放/暂停/停止
  - 下一台/上一台
  - 音量+/音量-/音量XX%
  - 静音/取消静音
  - 播放XX电台
  - 天气预报
  - 几点了/现在时间
  - 打开电台列表/天气/主页
  - 心情模式

### 🌤 天气预报
- 和风天气API（免费版即可）
- 实时温度、体感温度、湿度、风速
- 3天天气预报
- 支持全国城市切换

### 😊 心情电台
- 开心、放松、助眠、活力、怀旧、冥想 6种心情模式
- 自动匹配对应风格的音乐电台

### 📺 网络电视（扩展）
- MJPEG 视频流播放
- 支持网络摄像头、IP电视

### 📱 操作方式
- 触摸屏操作
- 3个物理按键（音量+、音量-、播放/暂停/切台）
- 语音控制

## 硬件接线图

### ESP32-S3 → PCM5102A (I2S 音频输出)

| ESP32-S3 引脚 | PCM5102A 引脚 | 说明 |
|--------------|---------------|------|
| GPIO 6 | SCK (BCK) | I2S 位时钟 |
| GPIO 7 | LCK (WS) | I2S 左右声道时钟 |
| GPIO 8 | DIN | I2S 数据输入 |
| 3.3V | VCC | 电源 |
| GND | GND | 地 |
| GND | FLT | 滤波器选择 (接地=正常) |
| GND | DEMP | 去加重 (接地=关闭) |
| 3.3V | XSMT | 软静音 (接VCC=正常) |
| GND | FMT | 格式选择 (接地=I2S) |

### PCM5102A → PAM8403 → 喇叭

| PCM5102A | PAM8403 | 说明 |
|----------|---------|------|
| OUTL | L-IN | 左声道输入 |
| OUTR | R-IN | 右声道输入 |
| GND | GND | 共地 |

| PAM8403 | 喇叭 | 说明 |
|---------|------|------|
| L+ / L- | 左喇叭 | 左声道输出 |
| R+ / R- | 右喇叭 | 右声道输出 |
| VCC (5V) | USB 5V | 功放电源 |

### ESP32-S3 → INMP441 (I2S 麦克风输入)

| ESP32-S3 引脚 | INMP441 引脚 | 说明 |
|--------------|--------------|------|
| GPIO 16 | SCK | I2S 位时钟 |
| GPIO 15 | WS | I2S 字选择 |
| GPIO 17 | SD | 串行数据输出 |
| 3.3V | VDD | 电源 |
| GND | GND | 地 |
| GND | L/R | 左声道 (接地=左) |

### ESP32-S3 → ST7796S 4" 触摸屏

| ESP32-S3 引脚 | TFT 模块引脚 | 说明 |
|--------------|--------------|------|
| GPIO 12 | SCK | SPI 时钟 |
| GPIO 11 | SDA (MOSI) | SPI 数据 |
| GPIO 13 | SDO (MISO) | SPI 输入 (触摸用) |
| GPIO 10 | CS | 显示屏片选 |
| GPIO 9 | DC | 数据/命令选择 |
| GPIO 14 | RST | 复位 |
| GPIO 21 | BLK | 背光控制 |
| GPIO 20 | T_CS | 触摸屏片选 |
| GPIO 19 | T_IRQ | 触摸中断 |
| 3.3V | VCC | 电源 |
| GND | GND | 地 |

### 功能按键

| ESP32-S3 引脚 | 按键 | 功能 |
|--------------|------|------|
| GPIO 18 | KEY1 | 音量+ (短按) / 上一台 (长按) |
| GPIO 4 | KEY2 | 音量- (短按) / 下一台 (长按) |
| GPIO 5 | KEY3 | 播放/暂停 (短按) / 下一台 (中按) / 切页 (长按) |

## 软件架构

```
src/
├── main.cpp              # 主程序入口，系统整合
├── audio_player.cpp      # 音频播放器 (ESP32-audioI2S封装)
├── radio_stations.cpp    # 电台管理 (预置30+电台)
├── display_ui.cpp        # 显示UI (TFT_eSPI + LVGL)
├── weather.cpp           # 天气预报 (和风天气API)
└── voice_control.cpp     # 语音控制 (小智AI)

include/
├── config.h              # 全局配置 (引脚、WiFi、API Key等)
├── audio_player.h        # 音频播放器头文件
├── radio_stations.h      # 电台管理头文件
├── display_ui.h          # 显示UI头文件
├── weather.h             # 天气模块头文件
├── voice_control.h       # 语音控制头文件
├── User_Setup.h          # TFT_eSPI 配置
└── lv_conf.h             # LVGL 配置
```

## 开发环境搭建

### 方式一：PlatformIO (推荐)

1. 安装 VS Code + PlatformIO 插件
2. 打开项目文件夹
3. 修改 `include/config.h` 中的配置：
   - WiFi SSID 和密码
   - 和风天气 API Key
   - 小智 AI API Key (可选)
4. 编译并上传

```bash
pio run
pio run --target upload
pio device monitor
```

### 方式二：Arduino IDE

1. 安装 ESP32 开发板支持包 (v2.0.14+)
2. 安装以下库：
   - ESP32-audioI2S
   - TFT_eSPI
   - lvgl
   - ArduinoJson
3. 配置 TFT_eSPI (复制 `User_Setup.h` 到库目录)
4. 配置 LVGL (复制 `lv_conf.h` 到库目录)
5. 打开 `src/main.cpp` 并上传

## 配置说明

### WiFi 配置
编辑 `include/config.h`：
```c
#define WIFI_SSID     "你的WiFi名称"
#define WIFI_PASSWORD "你的WiFi密码"
```

### 和风天气API (免费)
1. 注册 https://dev.qweather.com/
2. 获取 API Key
3. 编辑 `include/config.h`：
```c
#define WEATHER_API_KEY "你的API Key"
#define WEATHER_CITY_ID "101010100"  // 城市ID，默认北京
```

常用城市ID：
- 北京: 101010100
- 上海: 101020100
- 广州: 101280101
- 深圳: 101280601
- 杭州: 101210101
- 成都: 101270101

### 小智AI语音
1. 访问 https://xiaozhi.dev/ 获取开发文档
2. 配置 API Key 和设备ID
3. 离线唤醒词建议使用 ESP-SR 库替换当前的简化实现

## 电台源说明

### 中央人民广播电台 (HLS m3u8)
- 中国之声、经济之声、音乐之声、经典音乐广播
- 中华之声、文艺之声、华夏之声、乡村之声
- 香港之声、中国交通广播、故事广播、阅读之声
- 体育之声

### 中国国际广播电台 CRI
- 环球资讯广播、轻松调频 EZFM、劲曲调频 Hit FM

### 地方电台
- 北京：新闻广播、交通广播、音乐广播
- 上海：新闻广播、东广新闻台、动感101
- 广东：新闻广播、羊城交通、音乐之声

### 网络音乐电台
- 500首华语经典、周杰伦频道、轻音乐、古典音乐
- BBC World Service、VOA慢速英语

### 添加自定义电台
编辑 `src/radio_stations.cpp` 中的 `loadDefaultStations()` 函数，添加：
```cpp
memset(&st, 0, sizeof(st));
strncpy(st.name, "电台名称", STATION_NAME_MAX_LEN - 1);
strncpy(st.url, "http://example.com/stream.m3u8", STATION_URL_MAX_LEN - 1);
strncpy(st.region, "地区", 31);
strncpy(st.genre, "音乐", 31);
st.format = STREAM_FORMAT_M3U8;  // 格式
st.bitrate = 128;                // 码率
st.favorite = true;              // 是否收藏
_stations.push_back(st);
```

## 批量电台管理 (v2.0)

### 支持的导入格式

| 格式 | 扩展名 | 说明 |
|------|--------|------|
| JSON | `.json` | 推荐，支持完整字段 |
| M3U  | `.m3u` / `.m3u8` | 通用播放列表格式 |
| CSV  | `.csv` | 逗号分隔，可用Excel编辑 |

### JSON格式规范

```json
{
  "version": 2,
  "count": 3,
  "stations": [
    {
      "n": "电台名称",
      "u": "http://example.com/stream.m3u8",
      "r": "北京",
      "g": "新闻",
      "b": 128,
      "f": 2
    }
  ]
}
```

字段说明：
- `n` / `name` - 电台名称 (必填, 最长64字符)
- `u` / `url` - 流地址 (必填, 最长256字符)
- `r` / `region` - 地区 (可选, 最长32字符)
- `g` / `genre` - 分类/流派 (可选, 最长32字符)
- `b` / `bitrate` - 码率 kbps (可选)
- `f` / `format` - 格式: 0=MP3, 1=AAC, 2=M3U8, 3=PLS, 4=OGG, 5=FLAC, 6=WAV, 7=M4A, 8=未知

### 方式一：Web界面上传（推荐）

1. 确保收音机已连接WiFi
2. 查看屏幕或串口获取设备IP地址
3. 电脑浏览器访问 `http://<设备IP>/`
4. 点击「批量上传」菜单
5. 拖拽或选择电台文件上传
6. 系统自动识别格式并导入

> 支持最大 2000 个电台，文件大小受 LittleFS 空间限制

### 方式二：PC端批量导入工具

使用 `tools/station_import.py` 将各种格式转换为设备兼容的JSON：

```bash
# 从M3U导入
python tools/station_import.py my_stations.m3u -o stations.json

# 从CSV导入
python tools/station_import.py my_stations.csv -f csv -o stations.json

# 从简单TXT导入 (格式: 名称,URL)
python tools/station_import.py list.txt -f txt -o stations.json

# 生成示例测试数据
python tools/station_import.py --sample 500 -o sample.json
```

工具功能：
- 自动识别电台格式
- 自动分类（新闻/音乐/财经/交通等）
- 自动地区识别
- URL去重
- 数据验证（检查名称、URL长度等）

### 方式三：直接烧录LittleFS镜像

1. 将准备好的 `stations.json` 放到 `data/` 目录
2. 使用 PlatformIO 命令烧录：
```bash
pio run -t uploadfs
```

## 常见问题

### 1. 没有声音怎么办？
- 检查 PCM5102A 接线是否正确
- 确认 XSMT 引脚接 3.3V (高电平=正常工作)
- 检查 PAM8403 电源和喇叭连接
- 串口监视器看是否有 "播放中" 状态

### 2. 电台连接失败？
- 确认 WiFi 正常连接
- 检查电台地址是否有效 (用 VLC 测试)
- 有些电台可能需要 HTTPS，确认证书支持
- 尝试更换码率较低的流地址

### 3. 触摸屏不准？
- 修改 `include/config.h` 中的触摸校准值
- 或使用 TFT_eSPI 的触摸校准示例进行校准

### 4. 内存不足？
- 确认使用的是 ESP32-S3 N16R8 (带8MB PSRAM)
- ESP32-audioI2S 库必须有 PSRAM 才能正常工作
- 减小 LVGL 缓冲区大小

### 5. 语音控制不工作？
- 检查 INMP441 接线 (注意是 I2S 输入)
- 当前版本是简化实现，仅作演示
- 建议集成 ESP-SR 离线语音识别库
- 或使用小智AI官方固件

## 技术栈

- **主控**: ESP32-S3 WROOM-1 N16R8
- **开发框架**: Arduino + PlatformIO
- **音频解码**: ESP32-audioI2S (HELIX MP3 + FAAD2 AAC)
- **显示驱动**: TFT_eSPI
- **UI 框架**: LVGL 9.x
- **触摸芯片**: XPT2046
- **天气API**: 和风天气
- **语音AI**: 小智AI

## 版本历史

### v2.0.0 (2026-08-31)
- 支持大批量电台管理 (最大2000个)
- LittleFS 文件系统存储电台数据
- 批量导入: JSON / M3U / CSV 三种格式
- Web管理界面: 浏览器上传、电台管理、播放控制
- 电台搜索、分类浏览、地区筛选
- 收藏和最近播放功能
- 分页显示，流畅滚动

### v1.0.0 (2026-08-31)
- 初始版本
- 网络电台播放 (MP3/AAC/HLS)
- VU表双声道显示
- 电台列表管理
- 天气预报
- 语音控制框架
- 心情电台模式
- 触摸屏UI

## 许可证

MIT License

## 参考资料

- [ESP32-audioI2S 库](https://github.com/schreibfaul1/ESP32-audioI2S)
- [TFT_eSPI 库](https://github.com/Bodmer/TFT_eSPI)
- [LVGL 图形库](https://lvgl.io/)
- [小智AI](https://xiaozhi.dev/)
- [和风天气开发平台](https://dev.qweather.com/)
- [央广电台直播源](https://www.cnr.cn/)
