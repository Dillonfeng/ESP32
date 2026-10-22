# ESP32-S3 网络收音机 / 翻页时钟（V5）

基于 ESP32-S3 N16R8 的多功能桌面设备：机械翻页(split-flap)时钟、万年历（农历/干支/节气/节日）、网络收音机（HLS/MP3/AAC）、天气显示。

> 本文档对应 BUILDV5 分支（当前 HEAD：BUILDV5-CLKFIX-A，2026-10-20）。
> 详细开发记忆见 `AGENTS.md`（含调试铁律、硬件反模式 R1-R5、AA 中文字库格式说明）。

## 硬件清单

| 模块 | 型号 | 数量 | 用途 |
|------|------|------|------|
| 主控板 | ESP32-S3 N16R8 开发板 | 1 | 主控 (16MB Flash + 8MB PSRAM) |
| 显示屏 | 4.0" SPI TFT 触摸屏 | 1 | ST7796S, 480x320, 带 XPT2046 触摸 |
| 音频DAC | PCM5102A | 1 | I2S 数字音频解码 |
| 功放模块 | PAM8403 | 1 | D类数字功放 (2x3W) |
| 麦克风 | INMP441 | 1 | I2S MEMS 麦克风 (语音扩展) |
| 喇叭 | 4Ω 3W 全频喇叭 | 2 | 立体声输出 |

## 功能特性

### 🕐 机械翻页时钟 (split-flap)
- 翻页牌式数字遮罩（`include/digits.h`，46×88 8-bit 灰度，Bahnschrift）
- 指针表盘样式（`include/dialnum.h`，12 个数字遮罩）
- 两种样式通过触摸点击实时切换
- 局部刷新 + 整帧合成（R2），时钟页干净无闪烁
- **时钟缓冲 boot 常驻**（CLKFIX-A）：翻页/指针缓冲在 setup 最干净时一次性分配，播放期切时钟不再依赖新分配，根治切页空白/齿轮带缺失

### 📅 万年历 / 黄历
- 农历、干支、节气、节日计算（`src/nongli.cpp`，17 位表 + 积日算法）
- 节假日（放假/补班）离线缓存（`data/holidays_2025/2026/2027.json`，LittleFS）
- 黄历弹层：粗体大数字 + 宜/忌单行 + 星期双框 + 月相（`include/almnum.h` 62×112 遮罩）
- 中西节日配色（西方节日大数字变红，如万圣节/光棍节/黑五/母亲节等）

### 📻 网络电台播放
- 电台库 `data/stations.bin`（1652 站 / 40 地区，纯 HTTP，设备无 TLS 能力故全站 http://）
- 支持 HLS(m3u8) / MP3 / AAC 流，央视、CRI、地方台、网络音乐台
- 多任务架构：`netTask` 独立下载泵 + `touchScanTask` 独立触摸任务 + 主 loop 显示
- 播放条带 ▶/暂停/静音按钮，音量百分比与 UI 状态同步（radioVolume）

### 🌤 天气显示
- 和风天气 API（`src/weather_sync.cpp` + `src/weather_ui.cpp`）

### 🖱 触摸
- 自写 XPT2046 驱动（`src/touch_driver.cpp`，PENIRQ 中断 + SPI 采样独立任务）
- PENIRQ-WAKE：XPT2046 复位后 GPIO38 卡死高电平时，SPI 初始化后手动拉低 GPIO38 + 发读 X 命令唤醒触摸控制器
- 引脚见下方接线图（TOUCH_CS=GPIO1 / TOUCH_IRQ=GPIO38，GPIO38 为 octal PSRAM SPICS1 复用脚）

### 🈶 AA 中文字库
- 16px 8-bit 灰度抗锯齿字库（`data/font_simhei_16.font`，XAAX 大端格式，~4088 字）
- 64px 大字号（`data/font_simhei_64.font`，Flash 直读，用于黄历大数字/节日框；原 24px `font_simhei_24.font` 已移除）
- 生成器 `scripts/gen_aa_font.py` / `gen_aa_font32.py`（C:\Python314，需 PIL）

## 硬件接线

### ESP32-S3 → ST7796S 4" 触摸屏

| ESP32-S3 引脚 | TFT 模块引脚 | 说明 |
|--------------|--------------|------|
| GPIO 12 | SCK | SPI 时钟 |
| GPIO 11 | SDA (MOSI) | SPI 数据 |
| GPIO 13 | SDO (MISO) | SPI 输入 (触摸共享) |
| GPIO 10 | CS | 显示屏片选 |
| GPIO 9 | DC | 数据/命令选择 |
| GPIO 14 | RST | 复位 |
| GPIO 21 | BLK | 背光控制 |
| GPIO 1 | T_CS | 触摸片选 (XPT2046) |
| GPIO 38 | T_IRQ | 触摸中断 (PENIRQ) |
| 3.3V | VCC | 电源 |
| GND | GND | 地 |

### ESP32-S3 → PCM5102A (I2S 音频输出)

| ESP32-S3 引脚 | PCM5102A 引脚 | 说明 |
|--------------|---------------|------|
| GPIO 6 | SCK (BCK) | I2S 位时钟 |
| GPIO 7 | LCK (WS) | I2S 左右声道时钟 |
| GPIO 8 | DIN | I2S 数据输入 |
| 3.3V | VCC | 电源 |
| GND | GND | 地 |
| GND | FLT / DEMP / FMT | 滤波器/去加重/格式 (接地=正常) |
| 3.3V | XSMT | 软静音 (接VCC=正常) |

### ESP32-S3 → INMP441 (I2S 麦克风输入)

| ESP32-S3 引脚 | INMP441 引脚 | 说明 |
|--------------|--------------|------|
| GPIO 16 | SCK | I2S 位时钟 |
| GPIO 15 | WS | I2S 字选择 |
| GPIO 17 | SD | 串行数据输出 |
| 3.3V | VDD | 电源 |
| GND | GND | 地 |
| GND | L/R | 左声道 (接地=左) |

### 功能按键 / LED

| ESP32-S3 引脚 | 功能 |
|--------------|------|
| GPIO 18 | KEY1 |
| GPIO 4 | KEY2 |
| GPIO 5 | KEY3 |
| GPIO 2 | 状态 LED (板载) |

## 软件架构

```
src/
├── main.cpp              # 主程序入口，多任务整合 (netTask 下载泵 + touchScanTask + 主loop)
├── clock_ui.cpp          # 时钟 UI（翻页/指针，buildAnalogFace 缓存 + 整帧合成）
├── calendar_ui.cpp       # 万年历 UI（月份表格/选中/黄历弹层）
├── radio_ui.cpp          # 收音机 UI（地区/电台列表、播放条 ▶/暂停/静音、滚动）
├── radio_db.cpp          # 电台数据库 (stations.bin 解析)
├── touch_driver.cpp      # 自写 XPT2046 驱动（PENIRQ 中断 + SPI 采样）
├── tft_touch.cpp         # TFT_eSPI 触摸封装（校准映射）
├── holiday_sync.cpp      # 节假日离线缓存 (LittleFS)
├── weather_sync.cpp      # 天气同步 (和风 API)
├── weather_ui.cpp        # 天气显示
├── nongli.cpp            # 农历/干支/节气/节日计算
├── festival.cpp          # 节日表
├── geo_pin.cpp           # 地理位置/固定台
├── xfont.cpp             # XFont AA 字库 (XAAX 大端格式, loadAA/findAA/合成)
└── ui_common.h           # 模块共享声明 (BUILD61)

include/
├── config.h              # 全局配置（引脚、WiFi、API Key）
├── User_Setup.h          # TFT_eSPI 配置 (TOUCH_CS=1 / TOUCH_IRQ=38)
├── tft_setup.h           # 屏幕初始化封装
├── touch_driver.h        # 触摸驱动头
├── digits.h / dialnum.h  # 翻页/表盘数字遮罩
├── almnum.h              # 黄历大数字遮罩 (62×112)
├── nongli.h              # 农历头
└── ...                   # 其余模块头文件

data/                     # LittleFS 文件
├── stations.bin          # 电台库 (1652 站, 纯 HTTP)
├── radio_meta.bin        # 电台元数据 (sid 索引)
├── stations.json         # 电台 JSON (源数据)
├── festival.json         # 节日配置
├── holidays_2025/2026/2027.json  # 节假日缓存
└── font_simhei_16/64.font        # AA 字库
```

## 构建与烧录 (PlatformIO)

```bash
# 编译（必须指定 -e s3v5，默认 env 会联网下载框架卡死）
pio run -e s3v5

# 烧固件（COM 口按实际，历史为 COM4/COM5）
pio run -e s3v5 -t upload --upload-port COM4

# 烧 LittleFS（data/ 下的字库、电台库、节假日缓存）
pio run -e s3v5 -t uploadfs --upload-port COM4

# 串口监视
pio run -e s3v5 -t monitor
```

> 终端集成抽风时的可靠烧录法（脱离终端）：
> ```powershell
> Start-Process -FilePath "E:\Home PC\ESP32-S3\.platformio\penv\Scripts\platformio.exe" `
>   -ArgumentList "run","-e","s3v5","-t","upload","--upload-port","COM4" -WindowStyle Hidden
> ```
> 或使用 `scripts/flash2.py`（内含端口清理 + 烧录 + 抓 boot log）。
> 注意：`C:\Python311\python.exe` 不存在，真路径是 `C:\Users\fhaiy\AppData\Local\Programs\Python\Python311\python.exe`。

## 调试铁律（用户确认，详见 AGENTS.md）

1. 没有连硬件的环境时才用 `preview/live_preview.py` 调试；先在模拟器调好 → 统一烧录硬件。
2. 连上硬件后以模拟器效果为目标；模拟器每个改动，硬件 firmware 必须同步（lockstep）。
3. `main.cpp` 与 `make_preview.py` 必须 lockstep：颜色、月相、节气/节日配色、农历逻辑两侧一致。
4. 不要运行 `scripts/rebuild_main.py`（会覆盖 main.cpp 手动 split-flap 改动）。
5. 手改 `src/main.cpp` 后必跑 `python scripts/check_hw_patterns.py` 自检（R1-R5 硬件反模式）。
6. 解析 XAAX 字库头/字段一律按大端。

## 版本历史

### BUILDV5-CLKFIX-A (2026-10-20) — 当前
- 时钟缓冲 boot 常驻（`allocClockBufsResident`）：setup 最干净时一次性分配翻页/指针缓冲，删除 `freeClockBufs` 调用（V5 全 HTTP 无 TLS，原 BUILD17 释放理由已过时），根治播放期切时钟页空白/齿轮带缺失
- PENIRQ-WAKE：XPT2046 复位后 GPIO38 卡死高电平时，SPI 初始化后手动拉低 + 发读 X 命令唤醒
- 删除 `font_simhei_24.font`（只保留 16px + 64px AA 字库）
- 删除 `autoPlayOnceTask`（boot 自动播环球资讯与播放条暂停按钮冲突）
- 删除 `hls_prefetch.*`（STEP1 临时双缓冲预取实验代码彻底清理）
- 播放条加 ▶/暂停/静音按钮，音量状态 radioVolume 与 UI 同步
- Audio.cpp/h 大改：v5 I2S1 + 连接/缓冲逻辑重构；touch_driver/clock_ui/radio_db 配套调整
- esp_log 全关（NONE），只保留自有 Serial.printf；加 boot + 60s 节流内存基线打印

### BUILDV5 clean baseline (2026-10-14)
- 多任务拆分：netTask 独立下载泵（audio.loop 不再占 main loop）+ touchScanTask 独立触摸任务
- 自写 XPT2046 驱动（PENIRQ 中断方案，替换 TFT_eSPI 轮询）
- HLS 双缓冲预取（FIX11/12/12b）+ TWDTRESET 防网络阻塞复位
- 全局 SPI 总线互斥 + 全局打印锁
- 触摸 SPI 提速 2.5→8MHz（getTouch 92ms→30ms，帧率 25-30fps）
- **待解决**：HLS 长时卡顿（待双连接正式方案）、触摸漂移/单双击误判（低概率）

### BUILD23 (2026-10-02)
- 黄历弹层 宜/忌 重复项根治 + 西方节日大数字变红

### BUILD22 (2026-10-02)
- 西方小节日补全（万圣节/光棍节/黑五/母亲节/父亲节）+ 2027 除夕显示 + font32 补齐

### BUILD21 (2026-10-02)
- 黄历顶行/节日框"字被截断"根治（64px 源降采样）+ 春节整段节日名

### BUILD20 (2026-10-01)
- 节假日乱码(use-after-free)根治 + 2/16 除夕显示

### BUILD15 (2026-09-27)
- 收音机"三种失效模式"诊断：connect 超时 200ms→2s、看门狗重置、播放条刷新

### 收音机"总是停"根治 (2026-09-27)
- 设备无 TLS 能力 → 电台库全改纯 HTTP（`_rewrite_bin_http.py`）+ 节假日离线化

### v2.0.0 (2026-08-31)
- 批量电台管理 (最大2000个)、LittleFS、Web 管理界面、搜索/分类/收藏

### v1.0.0 (2026-08-31)
- 初始版本：网络电台播放、VU 表、天气预报、语音控制框架、触摸屏 UI

## 许可证

MIT License
