# ESP32-S3 网络收音机 / 翻页时钟 — 项目记忆

> 本文件是 Cline 的自动加载记忆。新窗口打开 `D:\ESP32-S3` 时会自动读到，无需复制上下文。

## 硬件
- ESP32-S3-N16R8 + ST7796S 480×320 (rot1) + XPT2046 触摸 + PCM5102A (I2S) + INMP441 麦克风
- 功能：机械翻页(split-flap)时钟 + 万年历(农历/干支/节气/节日) + 网络收音机(HLS) + 天气

## 构建 (PlatformIO 已装)
- `pio` = `C:\Users\fhaiy\AppData\Roaming\Python\Python311\Scripts\pio.exe`
- 编译：`pio run`（当前编译通过，RAM ~21% / Flash ~24%）
- 烧固件：`pio run -t upload`
- 烧 LittleFS：`pio run -t uploadfs`（推送 `data/festival.json` 等）
- 串口：`pio run -t monitor`，115200，关注 `[Fest] festival.json loaded`
- **脱离终端烧录（终端集成抽风时的可靠法，2026-09-25 验证）**：前台 `pio run -t upload` 会被 shell 集成反复掐掉，且之前失败的 `_bootlog.py`/`pio` 子进程会**独占 COM5** → 后续 esptool 卡在 `Connecting...`（**端口抢占死锁**）。解法 = `scripts/flash2.py` + `scripts/_kill_port.ps1`：
  ```powershell
  # ① 先杀掉一切占用 COM5 的进程（esptool/pio/遗留 python）
  # ② 再脱离终端启动 flash2.py（它自己也会先 _kill_port 再烧 + 抓 boot log）
  $py="C:\Users\fhaiy\AppData\Local\Programs\Python\Python311\python.exe"
  Start-Process -FilePath $py -ArgumentList "D:\ESP32-S3\scripts\flash2.py" -WindowStyle Hidden
  ```
  - `flash2.py`：先跑 `_kill_port.ps1`（排除**自身 PID**，否则会把自己也杀了）→ `pio run -t upload --upload-port COM5`（流式写 `upload2.log`）→ `_bootlog.py COM5` 抓 boot（写 `boot2.log`）。全程进度用 `read_files` 读 `flash2.log`/`upload2.log`/`boot2.log`（**不走终端**，避免被掐）。
  - **⚠️ 坑**：`Start-Process` 的 python 路径必须真实存在（`C:\Python311\python.exe` 不存在！真路径见上）；`_kill_port.ps1` 必须排除调用方自身 PID（`flash2.py` 也是 python.exe）；`capture_output` 会缓冲到进程结束才可见，所以**流式** `stdout=up` 才能边烧边看。
  - 成功判据：`upload2.log` 出现 `Hash of data verified` + 100% + `boot2.log` 出现 `ESP32-S3 Smart Radio Starting...` / `font16: OK  font24: OK` / `Setup done.`。

## 关键文件
| 文件 | 作用 |
|---|---|
| `src/main.cpp` | 主程序 (UI/时钟/收音机/天气) — 已手动改写为 split-flap 局部刷新 |
| `include/digits.h` | 翻页时钟数字遮罩 (46×88 8-bit，`gen_masks.py` 生成，Bahnschrift light) |
| `include/dialnum.h` | 指针表盘数字遮罩 (12 个：12,1..9,10,11；单数字 24×28 / 双 38×28，Bahnschrift 权重 450，`gen_dialnum.py` 生成) |
| `include/almnum.h` | 黄历大数字遮罩 (10 个 62×112 8-bit 灰度，`AlmMasks[]` 指针表，Bahnschrift 权重 700，~400KB PROGMEM；`almBigNum` 合成两位=124×112 绿笔画 on 纸底) |
| `src/nongli.cpp` + `include/nongli.h` | 农历/节气/节日计算 (17 位表 + 积日算法) |
| `preview/make_preview.py` | PC 端 1:1 预览 (ST7796 模拟)，与 main.cpp 布局/配色一致 |
| `preview/nongli_data.py` | 预览用农历数据 (与 nongli.cpp 同表同算法) |
| `src/holiday_sync.cpp` + `include/holiday_sync.h` | 节假日(放假/补班)在线同步 + LittleFS 缓存 (timor.tech API) |
| `preview/sync_holidays.py` | 预览端节假日同步镜像 (拉取同一 API → 存 `preview/holidays_<year>.json`) |
| `data/festival.json` | LittleFS 节日配置 (14 条，URL 待填) |
| `scripts/check_hw_patterns.py` | 硬件反模式 lint (R1-R5: startWrite 包裹/双缓冲/PSRAM 中转/Sprite 禁 PSRAM/setSwapBytes)；`live_preview.py` 启动前自动跑 |
| `platformio.ini` | 构建配置 (esp32-s3-n16r8, 16MB, littlefs) |

## 调试铁律（用户 2026-09-20 确认）
1. **没有连硬件的环境时才靠 live preview 调试**：模拟器存在的意义就是"不能连硬件时"模拟上硬件后的效果。
2. **先在模拟器调好 → 统一烧录硬件**。
3. **连上硬件后，以模拟器上的效果为目标**（模拟器是 target）。
4. **模拟器上的每个改动，硬件 firmware 必须做同样改动**。若硬件因限制做不到，**则模拟器也不那么改**（改了也没用，硬件做不到 preview 再好看也无意义）。
   - ~~中文小字无抗锯齿~~ **已解决（2026-09-20）**：`font16` 已升级为 **8-bit 灰度抗锯齿字库**（见下"AA 中文字库"）。硬件中文小字现在**有 AA**，且与 preview 同源（都用 `msyhbd.ttc`），逐像素一致（`preview/_aa_verify.py` 验证：平均偏差 0.33/通道）。
   - 大数字（翻页牌 / 表盘数字）走 8-bit 灰度遮罩 `pushImage`，**硬件有抗锯齿**，preview 用 `digits.h`/`dialnum.h` 位图对齐即可（已一致）。
5. **同一函数内多次 `tft.pushImage`/`pushPSRAM` 必须包 `tft.startWrite()` ... `tft.endWrite()`**（2026-09-24 加，硬件反模式 R1）：每次 pushImage 是独立 SPI 事务，CS 切换间隙屏幕会瞬间"露白"——**preview 看不到**（PIL 整帧原子渲染），只在硬件烧录才暴露（典型症状：翻页牌"翻页期间局部闪"）。修法：调用者用 `tft.startWrite(); func(); tft.endWrite();` 包整段；若已包，被调函数顶部加 `// lint:skip-rule-1` 注释。已验证干净：`modeClock` 翻页分支包 `startWrite; tickFlapClock(target); endWrite`。
6. **凡"会动的区域"（指针/翻页/齿轮）必须整帧合成到内部 RAM 缓冲再单次 `pushImage`**（2026-09-24 加，硬件反模式 R2）：不要"先 `fillCircle` 整面擦白 → 再 `drawLine`/`fillTriangle` 画前景"。ST7796S 无 TE 同步、面板持续扫描，"推背景 → 画前景"中间瞬间被扫到 → **局部闪**。preview 看不到（PIL 整帧原子）。修法：`static uint16_t buf[W*H]` 内部 RAM（146×146=42.6KB 可承担），画完整帧后一次 `pushImage`。已验证干净：`drawCard`(`cardSram`) / `drawAnalogClock`(`analogFace`) / `drawRoller`(`rollerSram`) 都是单 pushImage 整帧。
7. **PSRAM 缓冲绝不能直接传 `tft.pushImage`**（2026-09-22 加，硬件反模式 R3）：ESP32-S3 八线 (octal) PSRAM 的 SPI-DMA 突发读取取到错乱数据 → **彩虹花屏**。用 `pushPSRAM(x,y,w,h,src)` 中转内部 RAM（`memcpy` 到 `sramPush` 静态内部 RAM 缓冲再 `pushImage`）。PSRAM 仍可用来**存**预渲染半牌/齿轮（省内部 RAM），只是推屏前中转一次。`TFT_eSprite` 同理：octal PSRAM 的 `pushSprite` 也可能花屏，需 `_psram_enable=false` 或 `InternalRamSprite` 子类强制走内部 SRAM（硬件反模式 R4）。
8. **`tft.setSwapBytes(true)` 必调**（硬件反模式 R5）：ESP32 小端 uint16_t `0x07FE` 在内存是 `0xFE 0x07`；ST7796 期望 RGB565 高字节前。不 swap → BGR 字节序错乱（文字彩色背景错位）。`initTFT()` 内调一次全局生效。
9. **`scripts/check_hw_patterns.py` 静态 lint 自动拦截 R1-R5**（2026-09-24 加）：5 条规则对应上述 5-8 条 + R2 整面擦白缺双缓冲。`preview/live_preview.py` 启动前自动跑（exit 0 静默；有 stdout 就打印；ERROR → 5s 延时提示，WARN 仅提示不阻塞）。**手改 `src/main.cpp` 后必跑 `python scripts/check_hw_patterns.py` 自检**（应输出 0 警告）。规则误报时在函数顶部加 `// lint:skip-rule-N`（N=规则号）注释跳过。

## AA 中文字库（font16 已升级，2026-09-20）
- **为什么**：preview 用 Pillow 矢量抗锯齿，旧硬件是 1-bit 位图无 AA，导致"实机中文小字发虚/拥挤"。规则 4 要求硬件做不到的别只在 preview 美化 → 直接把硬件升到 AA。
- **生成器 `scripts/gen_aa_font.py`**：用 `C:\Windows\Fonts\msyhbd.ttc`（与 preview **同源**）把每个字渲染成 **16×16 8-bit 灰度 alpha** + **逐字 advance**（= msyhbd `getlength`：CJK=16, 数字=10, 'A'=12, '-'=7, '.'=5, '°'=7...）。字集 = 可打印 ASCII + Latin-1 补充(°/·) + 常用符号(★☆等) + GB2312 一级常用字 + 源码/数据里出现的全部非 ASCII 可打印字符（~4088 字）。
  - 重生成：`python scripts/gen_aa_font.py` → 原地写 `data/font_simhei_16.font`（文件名不变，内容变为新 AA 二进制格式，~1.0 MB）。
- **文件格式（二进制）**：`magic "XAAX"(4) | ver(1)=1 | pad(1)=0 | n(2,BE)` + 每字 `[unicode(2,BE) | advance(1) | 16×16 alpha(256)]`。`XFont` 按 magic 自动识别；**旧的 base-64 1-bit 文件（无 magic）自动走旧路径**，所以 `font24`（`font_simhei_24.font`，1-bit）不受影响、继续工作。
- **`XFont`（`src/xfont.cpp`）**：新增 `loadAA`（整文件读进 PSRAM）+ `findAA`（unicode 二分）+ `textWidthAA`（逐字 advance 求和）+ `drawStringFastAA`（逐字 `compositeGlyph`）+ `compositeGlyph`（`out=(bg*(255-m)+fg*m)/255` 逐 565 通道合成后 `tft.pushImage(x,y,16,16,buf)`，与 `blitDialNum` 同款）。`drawString/drawStringFast/textWidth` 开头都加了 `if(_isAA)` 分发。
- **`main.cpp` `drawDateRow`**：所有 `font16.drawStringFast(...)` 调用**必须传所画行底色**（行1 传 `ROW1_BG`，行2 传 `DATE_STRIP`）作为 AA 合成背景；星期单独用 `TFT_CYAN` 绘制（与 preview 一致）；行2 右侧补上了 **湿度 | 温度 | 月相名**（`mockHumidity`/`mockTempC` 占位，接入天气 API 后替换，颜色用 `humidColor()`/`tempColor()`）。
- **验证**：`python preview/_aa_verify.py` 用 Python 精确模拟硬件的"烘焙16×16灰度+逐字合成+advance"再与 preview `d.text` 逐像素比对（输出 `verify_hw/preview/diff.png`）。当前平均偏差 0.33/通道（仅 AA 边缘个像素差，肉眼不可见）。
- **⚠️ 字节序 bug 修复（2026-09-22，硬件烧录验证）**：`loadAA()` 之前把 `n` 与每字 `unicode` 按**小端**读（`blob[6]|(blob[7]<<8)`），但文件格式是**大端**（`0x0FF8`=4088）→ count 读成 63503 → 尺寸校验失败 → **`font16` 一直静默加载失败**（boot log `font16: FAIL`），实机所有 16px AA 中文空白（preview 用 `struct.unpack(">H",…)` 读对，所以 preview 正常、硬件空白 = lockstep 破口）。修法 = `loadAA()` 里 count 与 `_aaUni[i]` 改成**大端**：`count=(blob[6]<<8)|blob[7]`、`_aaUni[i]=(g[0]<<8)|g[1]`。修复后实机 boot log 显示 `[XFont] AA 字库已加载 (4088 字…)` + `font16: OK`。**今后凡是解析 XAAX 字库头/字段，一律按大端**（`gen_aa_font.py`/`make_preview.py` 都是 BE）。调试用 `python preview/_bootlog.py COM5` 抓串口（正常 boot：EN=~RTS 低、GPIO0=~DTR 高）。

## ⚠️ 务必遵守的约定
0. **上节"调试铁律"优先**：模拟器与硬件必须互为目标+镜像；硬件做不到的效果不要只在模拟器美化。
0a. **用户在场且"现场/现象"敏感时（如正在播放、正在断流重现），任何会复位设备的操作（烧录/复位/长按 EN）必须先征得用户同意，或先抓 log 再动设备**（2026-10-24 教训：断流现场未抓 log 就烧录 → 现场破坏 + 中途 TaskStop 烧录留下半写 flash 黑屏）。**烧录一旦开始必须烧完，严禁中途 TaskStop**（esptool 擦写中中断 = flash 部分写入 → 设备黑屏无法启动）。
1. **`main.cpp` 与 `make_preview.py` 必须 lockstep**：颜色值、月相取模、节气/节日配色、农历逻辑两侧一致，否则预览与实机不符。
2. **不要运行 `scripts/rebuild_main.py`** — 会覆盖 main.cpp 的手动 split-flap 改动。
3. 预览用环境变量覆盖 mock 值：
   ```powershell
   cd D:\ESP32-S3\preview
   $env:PREVIEW_TEMP="-2"; $env:PREVIEW_HUMIDITY="95"; python make_preview.py
   $env:PREVIEW_TEMP=$null; $env:PREVIEW_HUMIDITY=$null
   ```
   结果写到 `preview_after.png`（可另存 `temp_cold.png` 等做对比）。
4. **实时预览窗口**（像机械翻页那样动起来、无需烧录）：
   ```powershell
   cd D:\ESP32-S3\preview
   python live_preview.py
   ```
   - 弹出 tkinter 窗口，按**当前真实时间**实时走秒：翻页样式看齿轮滚动+翻片，指针样式看秒针平滑扫秒。
   - **按空格 或 `s` 键** 在 机械翻页 ⇄ 指针表盘 之间实时切换（对应真机点屏幕切换）。
   - 环境变量 `PREVIEW_TIME=HH:MM:SS` 可指定起始时刻。

## 已完成 (历史)
- **收音机全屏字体发虚根治 + 音量粒度细化（2026-10-24；编译 SUCCESS 90.87s，烧录 COM4 SUCCESS 85.48s，lint 0 警告，preview lockstep 完成；待用户实机验收）**：
  - **用户 2 问题**：① 全屏字体发虚（"把所有的字都用字库里有的字体"）；② 音量调节粒度太粗。
  - **① 字体发虚根因 = 用了字库没有的字号（缩放/放大路径）**：频率 42px `almBigText`（drawBigChar dst 上限 40 clamp + 16px 源非整倍上采样发虚）、MHz 13px/音量 12px/底部 12px 走 `almScaledText` 面积平均降采样、台名 24px 走 almBigText 回退 font16 放大 24px——全部非字库原生字号。
  - **font24 已彻底清除（决定性事实）**：`data/font_simhei_24.font` **不存在**（data 只有 16/64）；`XFont font24(&tft)` 定义只存在于 `src/*.bak` 备份；`ui_common.h` L114 `extern XFont font24;` 是清除后残留 → 首次编译 `undefined reference to font24`。**修法 = 台名改 font16 16px 原生 AA + 删除 extern 残留**（不再重建 font24；电台名汉字不在 font32 361 字子集，64px 源缩放会缺字回退 16px 源放大同样发虚）。
  - **改后字号（全部字库原生）**：台名 `font16` 16px（居中 y=70，fitText 默认 font16 测宽）；频率 `almBigText(240,113,fr,24,...,true)`（**font32 64px 源面积平均下采样，数字清晰**）+ MHz `font16` 16px（240+fw/2+8, 128）；收藏星 `font16` 16px（441-8, 7 顶栏中心 y=15）；音量% `font16` 16px（FS_BTN_Y+26）；底部提示 `font16` 16px（NAV_BAR_Y+7）；播放条音量%（cy-8）/副行"正在播放"（y+25）/"左列选分类"（y+27）；列表行 info `font16` 16px（rowY+26，裁剪 rowY+26..rowY+42）。
  - **② 音量粒度**：`radioVolumeDelta` 两处 ±5→±1（radio_ui.cpp 播放条+全屏按钮）；范围仍 0..21（显示 % = radioVolume*100/21）；live_preview 音量步进 ±5→±1 同步。
  - **验证**：`check_hw_patterns.py` R1-R5 0 警告；`pio run` SUCCESS（90.87s，err 仅 weather_ui.cpp 循环变量 warning 无碍）；`flash_font.log` = `Hash of data verified` + `[SUCCESS] 85.48s`（COM4）；preview 出图逐项核对：台名 16px（中心 78）/频率 24px（125）/MHz 16px（136）/100%（234）/底部（305）/★（15）全部正确。
  - **⚠️ 待用户实机验收**：① 全屏所有字清晰（台名 16px 略小于设计稿 24px 但原生 AA，频率 24px 大数字突出）；② 音量 ±1 档细调；③ 播放条/列表行 info 16px 两行不重叠（48px 行高：台名 +6..+22、info +26..+42）。
  - **⚠️ 教训**：**extern 声明不产生定义，删定义留 extern → 链接失败**；清字体先查 data/ 文件 + 全工程定义/引用三处一致。preview `hw_glyphs` 是矢量渲染不受字库限制，仅镜像布局字号。
- **触摸漂移根因根治：SPI 触摸频率 8MHz→2.5MHz + 日期偏移 tm_mon bug（2026-10-24；两次编译+烧录实机验证通过，用户确认"都正常了"）**：
  - **① 触摸漂移/单双击误判根因 = `SPI_TOUCH_FREQUENCY 8000000` 超 XPT2046 规格**：
    - **历史**：BUILDV5-TOUCHSPEED 为提速 getTouch（旧架构在 main loop 拖帧率）把 2.5→8MHz。触摸移入独立任务+V3 状态机后提速前提已不成立，但 8MHz 一直留着。
    - **铁证链**：用户点 30 号正中心(112,189) → 8MHz 下 `press(115,175) raw(2240,1184)`：**y 偏上 14px**（raw rx=2240 vs 期望 2419，偏 179 ADC）；校准中心残差 devY=-111 raw（≈8px）；30 号格子 y=174..204 仅 30px 高 → 点 30 号常被识别成 23 号（r3）。**降回 2.5MHz 后** `press(107,187) raw(2405,1122)`：**y 只差 2px，30 号正确命中**。x 方向基本准（107 vs 112）。
    - **根因**：XPT2046 手册串行时钟上限 2.5MHz（TFT_eSPI 默认 2500000）；8MHz 超规格 → ADC 转换建立不足 → **raw 系统性偏低（Y 通道尤甚）→ 屏幕中下部坐标偏上 8~14px** → 30px 高格子跨行误判 + 抖动影响单双击。
    - **修法**：`include/User_Setup.h` `#define SPI_TOUCH_FREQUENCY 2500000`（保留 8MHz 注释 + 还原对照说明）。**教训：改动"提速"类参数前先查外设数据手册规格上限；性能前提消失后要回退实验参数。**
  - **② 日期显示 +1 月（10/24 显示 11/24）= `epochToLocalNoLock` 的 tm_mon 存了 1-based 月份**：
    - **铁证（PC 复算）**：2026-10-24 00:00 UTC epoch=1792800000 → days=20750 → civil_from_days：yoe=26, y=2026, doy=237, mp=7 → Hinnant `m = mp+(mp<10?3:-9) = 10` **是 1-based 月份**；代码直接存入 `tm_mon`（须 0-based）→ 调用方 `tm_mon+1` 显示 11 月。
    - **修法**（`src/ui_common.h` L55-57）：`int m1 = mp+(mp<10?3:-9); out.tm_mon = m1-1; out.tm_year = y+(m1<=2?1:0)-1900;`（年修正用 1-based m1 判断，与 Hinnant `y+=(m<=2)` 一致——原代码用误存值判断碰巧对）。
    - **审计**：全部 9 处 `tm_mon` 调用方均 `tm_mon+1`（0-based 约定）✓；`ymdFromDays`（*mo 1-based 返回）无此问题；`weekdayFromYMD/daysFromYMD` 入参 1-based 正确。
    - **⚠️ 教训：移植 Hinnant civil_from_days 时，`m` 是 1-based，存 tm 结构必须 -1；年份修正判断也用 1-based 的 m。**
  - **调试过程沉淀**：触摸排查完整链路 = 校准 raw 审计（4 角线性好但中心残差 -111）→ 对比 press raw vs 物理位置（决定性）→ 查采样参数超规格。**新工具 `scripts/_read_serial.py`（纯读串口不复位，参数 port seconds）**：_run_cap.py 每次硬复位设备（用户体验差），纯读模式保留现场抓点击 log。
- **收音机 UI 三问题对齐设计稿（2026-10-22；build_ui6 SUCCESS 65.55s，lint R1-R5 0 警告；preview lockstep 完成，未烧录实机）**：
  - **用户 3 问题**：① 左列无"我的"类别及子菜单；② 双击电台全屏播放页与设计稿不一致；③ 右列电台列表滚动方式（行索引跳变）与左列（像素 1:1 跟手）不一致。设计稿 = https://4m5gmbx3ra8da.doubao.link/app/app_17g7jt0w5v1（已下载源码 `D:\ESP32-S3\design_ref\index.html` + `app.zip`）。
  - **宏统一（前置根因）**：main.cpp 与 ui_common.h **重复定义同一批 RADIO_* 布局宏且值不同**（main 旧值 70/28/80/45/22/24 vs ui_common 新 150/24/48/48/28/26）→ 绘制区与触摸命中区错位（"选不中/不灵敏"布局层根因）。修：删 main.cpp 全部重复 RADIO_* + ANALOG_FRAME_MS，唯一权威 ui_common.h。
  - **① 左列三分组**：RadioCat 加 `int group`（0=频道 1=省市 2=我的）；ui_common.h 加 `RADIO_GROUP_H 18 / RADIO_GROUP_CNT 3` + sideRowTop/sideTotalH 声明。buildRadioCats 重排（radio_db.cpp）：组0 全国+中央/国际/海外 → 组1 置顶省(RPINNED"广东")+RORDER[33省]+"其它" → 组2 收藏→个人(extra is_user)→伴音(电视伴音 label"伴音")→历史。drawRadioRegionSidebar 画组标题（GNAMES[]={"频道","省市","我的"}，C_GROUP 弱化色；**组标题行区域 = 分类行顶边之上 GROUP_H**：`gy=top+sideRowTop(i)-RADIO_GROUP_H`）；handleRadioTap 侧栏命中逐分类行 sideRowTop 比对（组标题不可点）。
  - **② 全屏对齐设计稿**：radioVolumeDelta(int d)（0..21，自动解除静音）；频谱 L/R 竖条（FS_SEG_N=30/FS_X_L=10/FS_X_R=404/FS_BASE_Y=256/FS_MAX_H=160，7 彩段自下而上 + 峰值余晖）；顶栏38px（返回 chip / "正在播放"直播点 / 收藏星 drawFsFavChip）；台名 24px + **频率 42px（RGBC(0x16,0x24,0x3F)）+MHz** + meta tag（RGBC(0xEA,0xF1,0xFA)/0x4A5F87）+ 三按钮（FS_BTN_Y=200，−/播放暂停/音量+，FS_BTN_DN_X=168/MAIN=226/UP=284，R=22/26）+ 音量% + 底部30px"双击此处 · 返回列表"；命中：返回(8,6..72,32)|收藏星(410,6..472,32)|按钮区|底部双击 300ms lastFsTap 返回，**其余区域不响应**。
  - **③ 右列像素滚动**：listScrollTop 语义改"像素偏移"。drawRadioList：pixOff=listScrollTop%ROW_H、startRow=/ROW_H、顶半行 first=startRow-1、底半行 last=startRow+ROWS、drawStationRow(rowY) 带 clipY0/c1 半行裁剪（TFT_eSPI **无 setClipRect**，手动裁剪）；命中 rowIdx=(listScrollTop+(ty-LIST_TOP))/ROW_H；drawRadioScrollbar 像素（totalPix=count*ROW_H、thumbH=max(24,listH*listH/totalPix)）；拖动 1:1 像素 + 吸附 snap=(listScrollTop+ROW_H/2)/ROW_H*ROW_H。
  - **⚠️ sideTotalH 越界 bug（本次发现并修）**：旧 `sideRowTop(radioCatCount)` 读残留槽 group（越界，多算 1 个组标题 18px）。修 = `if(count<=0)return 0; i=count-1; return sideRowTop(i)+行高(i)`。preview side_total_h 同款正确公式。
  - **preview lockstep 全量完成**：make_preview.py 常量改浅色（**精确 565 值由 RGBC 换算**：C_SIDE 0xECF0F4→0xEF9E / C_ACCBG 0xE1F3FA→0xE79F / C_LINE 0xE5E7EB→0xE73D / C_ACC 0x0E97C4→0x0CB8 / C_TXT 0x1F2937→0x1946 / C_DIM 0x6B7280→0x6B90 / C_FAV 0xE39A1B→0xE4C3 / C_LIVE 0xE5484D→0xE249 / C_GROUP 0x9AA5B0→0x9D36 / C_THUMB 0xC4CCD4→0xC67A / C_BG 0xF6F8FA→0xF7DF；**g 通道必须 <<5**）；三分组模型（cats 加 group：组0 全国+央/际/海外、组1 置顶+RORDER+其它、组2 收藏/个人/伴音/历史）+ side_row_top/side_total_h（**i 必须 < len(cats)，total 用 i=n-1**）；draw_radio_sidebar 组标题（gy=top+side_row_top(i)-R_GROUP_H）+ R_GROUP_H=18/R_SPECIAL_H=28/R_ROW_H_SIDE=26；draw_radio_station_row clip_y0/c1 半行裁剪 + 直播点/台名/分类·地区（播放中前缀"正在播放 · "）/星标；draw_radio_list 像素滚动；draw_radio_scrollbar 像素（thumbH=24）；draw_radio_header 浅色"网络收音机"+光标+时间|音量%|离线；draw_radio_playbar 新布局（EQ 迷你条|台名/正在播放|音量 icon+%|右缘圆钮）；draw_radio_fullscreen 全新（顶栏/频谱静态竖条/频率42/MHz/meta/三按钮/底部提示；**全屏时不画导航栏**，镜像固件）；live_preview 侧栏/列表命中、拖拽（右列 1:1 像素+吸附、左列 side_row_top/side_total_h）、滚动条跳转像素、全屏命中（返回/收藏/三按钮/底部双击 300ms）、播放条命中（圆钮/音量/台名进全屏）、状态 radio_vol/radio_last_fs_ms。
  - **验证**：build_ui6 SUCCESS 65.55s（含 sideTotalH 修复）；lint R1-R5 0 警告；py_compile make_preview/live_preview 通过；preview 出图逐图核对：preview_radio_idle 三分组标题"频道/省市"显示 + 浅色列表 + 新播放条；preview_radio_fullscreen 顶栏返回/正在播放/★ + 频谱 + 频率"网络电台"+MHz + 广东/新闻 tag + −/‖/+ 三钮 + 100% + 底部"双击此处·返回列表"。⚠️ PIL：C:\Python314 无 PIL 且 site-packages 只读 → 用 `D:\ESP32-S3\.platformio\penv\Scripts\python.exe -m pip install pillow`（venv 可写），跑 preview 用 platformio 的 python。
  - **⚠️ 未烧录实机**：COM4 待确认后烧录；待用户验收：三分组显示与组序、"我的"组（收藏/个人/伴音/历史）、全屏布局/双击底部返回/音量±、右列 1:1 跟手滚动与吸附。**"助眠"分类**：stations.bin 无 region，设计稿有示例（雨声/海浪/白噪音），待拍板：占位空分类/暂不加/扩 extra 台。全屏"正在播放"直播点 blink 动效当前静态实现。
- **黄历弹层 宜/忌"重复项"根治 + 西方节日大数字变红（BUILD23，2026-10-02；编译 SUCCESS RAM 32.3% / Flash 52.3%，实机 boot 验证 BUILD23-YIJI-FEST + [Hol] cached 2025/2026/2027 + Setup done.，无 crash）**：
  - **用户报告 2 个问题**：① "10月28号有两个出行"（宜项重复）；② "西方节日能显示了，为啥没显示节假日颜色"（万圣节等大数字没变红）。
  - **① 宜/忌 重复项（根因 = YI 池含重复项 + 旧 3 连续取法）**：
    - `ALM_YI_ITEMS[12] = {嫁娶,出行,移徙,入宅,装修,动土,栽种,祭祀,安床,会友,嫁娶,出行}` —— **嫁娶 在 idx 0 与 10、出行 在 idx 1 与 11 各出现两次**。
    - 旧固件取 3 连续项 `ALM_YI_ITEMS[dayStd%12], [+1], [+2]`；**10/28 的 `dayStd%12=11`** → idx `11,0,1` = **出行/嫁娶/出行**（= 用户看到的"两个出行"）。
    - **修法 = 去重取 3（镜像 preview 已有的 seen-set 循环）**：`for k in 0..11` 取 `ITEMS[(dayStd+k)%12]`，跳过已出现的项，凑满 3 个停。宜/忌 两侧同样处理。**这是 lockstep 破口**：preview（`make_preview.py` L1678-1691）早已有去重，固件一直没同步。修后 10/28 → 宜 **出行/嫁娶/移徙**（无重复）。
  - **② 西方节日大数字变红（根因 = 颜色只由"放假/补班"驱动，不含节日列表）**：
    - 弹层大数字颜色块（`drawAlmPopup`）旧逻辑：`holiday::dayType==HOL_OFF/WORK → 红 / 周末 → 紫 / 平日 → 绿`。政府节假日（国庆/春节/除夕）红，但**西方节日（万圣节 10/31、光棍节 11/11、黑五、母亲节…）不是放假 → 不红**（只右上节日框有名字）。
    - **修法 = 大数字颜色加一条 `else if (fest[0]) numCol = ALM_RED;`**（`fest` 是 `almHolidayFest` 结果，非空 = 右上节日框有名字，含所有节日）。放在 放假/补班 之后、周末之前 → **任何节日（含西方）大数字变红**。`nl_festival` 只返真节日（不含节气），故不会误把节气变红。
    - **两侧 lockstep**：固件 `drawAlmPopup` numCol 块 + 预览 `draw_almanac_popup` `num565` 块 同步加 `elif fest` 分支。
  - **验证**：`check_hw_patterns.py` R1-R5 全过（0 警告）；`pio run` **SUCCESS**（RAM 32.3% / Flash 52.3%）；preview 出图逐张核对：10/31 万圣节 大数字 **31 红** + 宜(移徙/入宅/装修)忌(嫁娶/动土/安葬)；11/11 光棍节 **11 红**；10/28 平日 **28 绿**（无假阳性）；宜/忌 各 3 项全不重复。烧录 `flash2.py`（upload exit=0 / uploadfs exit=0），`boot2.log` = `BUILD23-YIJI-FEST` + `font32: OK (361 字)` + `[Hol] cached 2025/2026/2027` + `Setup done.` 无 crash。
  - **⚠️ 待用户上机确认**：① 双击 10/31 大数字"31"变红 + 宜忌无重复；② 双击 11/11"11"变红、11/27 黑五/5/10 母亲节/6/21 父亲节 同样红；③ 10/28 等平日大数字仍绿、宜忌无重复（不再"两个出行"）；④ 政府节假日（国庆/春节）大数字仍红。
- **西方小节日补全 + 2027 除夕显示 + 全面节日检查（BUILD22，2026-10-02；编译 SUCCESS RAM 33.0% / Flash 52.5%，实机 boot 验证 BUILD22-WEST-FEST + font32 361字/1445KB，无 crash）**：
  - **用户报告 3 个小问题**：① 西方小节日没显示（如"万圣节"）；② 2026 除夕显示了但 2027 除夕没标注；③ 要全面检查类似情况。
  - **① 西方节日补全（`src/nongli.cpp` `nl_festival` + 预览 `preview/nongli_data.py` `festival_of` 两侧 lockstep）**：
    - 原来只有 中国公历/农历节日 + 美国感恩节 + 复活节。新增：
      - **固定公历**：`{10,31,"万圣节"}` + `{11,11,"光棍节"}`（加进 `SOLAR_FESTS[]` / `SOLAR_FESTIVALS`）。
      - **浮动**：**黑色星期五** = 美国感恩节(11月第4个周四)次日(周五)（⚠️ 不能用"11月第5个周五"，多数年份 11 月只有 4 个周五会算错）；**母亲节** = 5月第2个周日；**父亲节** = 6月第3个周日（公式 `1 + (6 - dn_days(y,m,1)%7 + 7)%7 + (nth-1)*7`，与 C++/Python 两侧一致）。
    - 验证（`scripts/_fest_audit.py`）：万圣节 2025-2027、黑五 2026-2028、母亲节/父亲节 2026-2027 全部解析正确，且与既有固定节日**无日期冲突**（黑五/母亲/父亲 均非固定节日日）。
  - **② 2027 除夕根治（根因 = 2027 节假日 JSON 为空）**：
    - **决定性数据**：`data/holidays_2027.json` = `{"code":0,"holiday":{}}`（国务院 2027 放假安排未发布 → JSON 空）。旧逻辑 `almHolidayFest` 靠 `holiday::dayName == "除夕"`（JSON）判定除夕，2027 无数据 → 走不到；且 2027-02-05 非 `HOL_OFF`（空 JSON）→ `if(dayType!=HOL_OFF) return` 早退 → 除夕不显示。
    - **农历表其实是对的**（`scripts/_check_chuxi.py` 验证：2026 正月初一=2/17→除夕 2/16 ✓；2027 正月初一=2/6→除夕 2/5 ✓）。所以**不必依赖 JSON**，直接由内置农历表算除夕。
    - **修法 = 新增 `int nl_is_chuxi(year,mon,day)`**（`src/nongli.cpp` + `include/nongli.h` 声明）：查该年"正月初一"公历日序（`nl_month_days` 找 `101`），除夕 = 正月初一 - 1 天，比较公历月/日。`almHolidayFest` 里 `bool isChuxi = (JSON==除夕) || nl_is_chuxi(y,m,d)`（放在 `almRealFest` 之前，防 2026 农历表差一天误返春节）。
    - **预览 lockstep**：`make_preview.py` `_alm_fest_name` 同款——`is_chuxi = (JSON==除夕) or (次日 lunar_solar_to_lunar 是 正月初一)`。
    - 验证（`scripts/_fest_audit.py` section 3）：2025-01-28 / 2026-02-16 / **2027-02-05** / 2028-01-25 / 2029-02-12 各年**恰好一天**除夕，无假阳性。
  - **③ 全面检查 + font32 补齐**：
    - `scripts/_charset_check.py` 审计所有节日名用字对 font16(6989字全 GB2312，全覆盖) 与 font32(64px)。**font16 全部覆盖**（缺字走 font16 回退，节日框一直能渲染）；font32 缺 情人节/愚人节/建党节/平安夜/感恩节/复活节/龙抬头/黑色星期五 部分字。
    - **修 `scripts/gen_aa_font32.py` `build_charset()`** 追加 `"万黑棍色情愚党平夜感恩复活抬头条"`（覆盖新西方节日 + 既有节日缺字，全部升到 64px 不降级）。重生成 `data/font_simhei_64.font` = **361 字 / 1.48MB**（PSRAM 安全）。
    - **⚠️ 用 `C:\Python314\python.exe`（有 PIL）跑生成器**，Python311 无 PIL。
  - **验证**：`check_hw_patterns.py` R1–R5 全过（0 警告）；`pio run` **SUCCESS**（RAM 33.0% / Flash 52.5%）；`flash2.py` 烧固件 + LittleFS 成功（`upload exit=0` / `uploadfs exit=0`）；`boot2.log` = `BUILD22-WEST-FEST` + `font32: OK (361 字/1445 KB)` + `font16: OK` + `Setup done.` + `[Hol] cached 2025(33)/2026(39)/2027(0)`。预览出图（`scripts/_fest_preview.py`）：万圣节/2027除夕/母亲节/黑五/2026除夕 弹层逐张核对，节日框名字正确 + 花月名·节气并排不重叠（issue 3 保持）。
  - **⚠️ 待用户上机确认**：① 双击 10/31 弹层节日框"万圣节"；② 双击 **2027-02-05** 节日框"除夕"（2027 JSON 为空也显示）；③ 11/27"黑色星期五"、5/10"母亲节"、6/21"父亲节"、11/11"光棍节" 各自弹层正确；④ 既有节日（情人节/感恩节/龙抬头等）仍正常。
- **黄历顶行/节日框"字被截断"根治 + 春节整段节日名（BUILD21，2026-10-02；编译 SUCCESS RAM 32.3% / Flash 52.2%，实机 boot 验证 BUILD21-ALM-RUN 运行正常 + font32 345字/1381KB）**：
  - **② 顶行"农历正月初三"底横被截一半（用户实机确认，根因=小字 16px 源 area-average 降采样把细笔画塌成 1px）**：
    - 用户澄清：截断的是**最上面黑色的 13px 小字"农历正月初三"**（及右上角节日框），**不是**下面红色的大数字（之前误查了 64px 红字）。
    - **根因（决定性数据）**：顶行小字走 `almScaledText` → `font16.drawScaledChar` = **16px 源 area-average 降到 13px**。m-profile dump 证明："三"在 16px 字形里底横只占**源行 14-15（2 行、~50% 灰度）**，16→13 面积平均把**这 2 行塌成 1 个目标行** → 底横变 **1px**，而上面两横是 2px → 视觉上"底横被截一半"。而 24px 大字（2026/红 19/正月）走 `almBigText` → **64px `font32` 源**下采样，4x 分辨率 → 笔画保持 2px，故干净。**preview 用同款 16px 源降采样其实也是 1px 底横**（只是 24-bit 屏/照片放大看不明显），与硬件同病同源 = 不是 lockstep 破口，是**真降采样缺陷**。
    - **修法（两侧 lockstep，复用 `almBigText` 已验证的 64px 路径）**：`almScaledText` 每字改为 `if (font32.isReady() && font32.hasGlyph(cp)) font32.drawBigChar(sx,ytop,cp,size,…); else font16.drawBigChar(…)`（= `almBigText` 同款回退）；preview `hw_small` 每字改 `big_glyph(img,cx,ytop,code,size,…)`（`big_glyph` 已内置 font32 64px 源→font16 回退 + 同款 area-average）。64px 源降到 13/14/15px 笔画保持 2px，"三"底横不再截断。**验证**：m-profile 复算 "三" 64px→13px 三横 = 2px/2px/**2px**（修前 2px/2px/**1px**）；preview 出图顶行"三"底横满、节日框"春节"笔画干净。
    - **⚠️ 前置：64px 字库原本只有 320 字，缺 农/历/干/支/期/今/后**（小字常用字）→ 缺字会回退 16px 源（= 老毛病）。**修 `scripts/gen_aa_font32.py` `build_charset()`** 追加两行：`"农历干支期今后"` + `"除夕妇女节儿童建军植树教师青年母亲父护士"`（覆盖小字+节日框可能出现的政府 JSON 节日名）。重生成 `data/font_simhei_64.font` = **345 字 / 1.35MB**（PSRAM 安全）。⚠️ 用 `C:\Python314\python.exe`（有 PIL）跑生成器，Python311 无 PIL。
    - **⚠️ 未覆盖的 64px 缺字（假/周/平/花）**：只出现在**图例**（平日/周末/节假日，走 `font16.drawStringFast` 16px，不经 `almScaledText`）或未渲染，**不受本次改动影响**。
  - **① 春节整段（2/15–23）节日名应统一"春节"，但实机部分天显示"初三/初二"（= 旧 BUILD20 固件，未烧录）**：
    - 根因 = `almHolidayFest` 的**向前/向后游标循环每次把 `ny,nm,nd` 重新初始化为 `y,m,d`**（只查 `cur±1`，3 天窗口），游标 `ay/bm` 不前进 → 走不到整段放假区间 → 回退到 `holiday::dayName`（政府 JSON 当天名="初三"）。**修法 = 循环用运行游标 `ay/am/ad`（向前）与 `by/bm/bd`（向后）**（L1862/L1867），整段 HOL_OFF 都走 `almRealFest` 取"春节"。
    - **注意**：用户两张实机照片（顶行"初二/初三"、节日框"初三"）都是**旧 BUILD20**（本会话改完未烧录）。**本次烧录后实机应为"春节"整段**——待用户上机确认。
  - **烧录（终端集成抽风，双重 detach 法）**：`pio run` 前台会被 shell 集成掐 + 进程树杀死（log 卡在 Linking）→ 改用 `scripts/_build22.py`（hidden Start-Process，内含 `Popen(pio run)` + 轮询 flush log）跑编译，`read_files` 读 `build23.log` 确认 `[SUCCESS] 39.57s`；再 `Start-Process flash2.py -WindowStyle Hidden` 烧固件+LittleFS（`upload exit=0` + `uploadfs exit=0`），`read_files` 读 `boot2.log` 确认 `BUILD21-ALM-RUN` + `font32: OK`（345 字/1381KB）+ 全系统干净无 crash。⚠️ **监控期间只用 `read_files`（不走终端）+ hidden `time.sleep`**，任何新前台命令会掐掉正在跑的 pio/flash。
  - **⚠️ 待用户上机确认**：① 日历页双击 2/18–2/21 → 顶行"农历正月初X"**底横不再截断**（三横等宽 2px）；② 节日框 2/15–2/23 全部显示"**春节**"（不再是"初二/初三"）；③ 其他小字（干支·X / 农历·X月 / 星期 / 今日X）笔画干净。
- **黄历/日期条 节日名"乱码(keep-alive 等)"根治 + 2/16 除夕显示（BUILD20，2026-10-01；编译 SUCCESS RAM 32.3% / Flash 52.2%，实机 boot 验证 BUILD20-ALM-FEST 运行正常）**：
  - **根因 = `holiday_sync.cpp` `fillYear()` 的 use-after-free**：原来用 `String nameStr` 累加再存**指针**进 `name[mm][dd]`。`String` 每次 `realloc` 都会**释放旧缓冲**，于是之前写入的 `name[][]` 指针全部悬空。音频任务(Core 0)复用被释放的块做 HTTP 缓冲 → 显示任务(Core 1)读到 "keep-alive" / "Connection: Keep-ali" 等 HTTP 头碎片。
  - **修①（乱码）**：`fillYear()` 改用**固定 `char namePool[1024]`（.bss，按值拷贝）**，4 年池共 ~4KB，指针恒有效。
  - **修②（除夕）**：用户要求 2/16（正月初一前一天）显示"除夕"。根因 = **农历表对 2026 差一天**（`nl_festival` 把 2/16 记成正月初一 → 先误返"春节"）。**修法**：`almHolidayFest()` 里把"当天自己的节假日名 `holiday::dayName` == '除夕'（政府 JSON 数据，已按值稳定存储）"**提前到 `almRealFest` 之前**判定。每年 JSON 只有一天名为"除夕"，故早返回安全精确；其余整段（2/15、2/17–23）仍走整段统一名"春节"（用户已确认 2/19/20/21=春节）。
  - **两侧 lockstep**：固件 `src/main.cpp` `almHolidayFest` + 预览 `preview/make_preview.py` `_alm_fest_name` 同步（除夕判定都在 `festival_of` 之前）。验证脚本 `scripts/_verify_chuxi2.py`（**ASCII-hex 输出**，避开终端 CJK 乱码）逐日核对：2/16=除夕 / 2/15,17,19-21=春节 / 9/27=中秋节 / 10/2-6=国庆节，全 OK。
  - **⚠️ 用户问"乱码和之前释放更多 RAM 有关吗？"**：**无关**。乱码是**运行时内存 bug（use-after-free）**，与"释放更多 RAM"的优化操作无因果——是 `String realloc` 释放旧缓冲后指针悬空、被音频任务跨核复用导致的。释放 RAM 的操作不会造成悬空指针；两者只是恰好同期出现。
  - **⚠️ 踩坑**：① 农历表（`LUNAR_INFO` 17 位表）对 **2026 差一天**（把 2/16 记成正月初一，实际初一=2/17，以政府 JSON 为准）。**节日/除夕判定优先用政府 holidays JSON（权威），不要依赖农历表**。② 本会话**终端 shell 集成严重抽风**（每条命令误报 exit 1、CJK 输出乱码成 UTF-16 交错）——但命令**实际有执行**（python 有 traceback、pio 有 build log）。**对策**：用 `read_files` 读 build/flash/boot log 判断真实结果（不走终端）；验证脚本用 **ASCII-hex** 输出避开 CJK 乱码。
  - **⚠️ 待用户上机确认**：双击 2/16 应显示"除夕"；2/15,17-23 仍"春节"；9/27"中秋节"、10/2-6"国庆节"；且非节日日期（如 2/15 之前、普通工作日）不再出现 "keep-alive" 等乱码。
- **收音机"三种失效模式"诊断 + 控制流修复（BUILD15，2026-09-27）**：
  - **背景（用户报告 3 种失效）**：① 点"中国之声"→ **LoadProhibited crash**；② 点"广东股市"/"枣庄综合"→ **无反应**（转圈后无声音）；③ **播放/显示不同步**（播放条显示 A 台但实际在播 B 台）。
  - **① crash 是"旧实验固件"，非当前 BUILD14/15**：`_boot_new/v7/v3/v4/v5.txt` 的 crash（ELF SHA `a3e543e0`，`Core1 LoadProhibited`，`EXCVADDR=0xe267f4e2`）都带 `DBG psram=8189KB radioStn=0x3d9d1238` 打印——**当前 `src/main.cpp` 已无这些 DBG 行**（那是被废弃的"把 `radioStn` 存 PSRAM 并做指针范围检查"实验）。**当前 BUILD14（`bootcap40.log`）boot 干净**：`radio_meta: 1670 stn` → `Setup done.` → 播 CCTV-13(AAC)+怀集音乐之声(MP3, 持续 `syncword found`)，堆稳定 49–51KB 无 crash。→ crash 已在当前固件消失，**无需修**（若再复现须抓当前 SHA 的 dump 重定位）。
  - **② "无反应"根因 = 两个（当前 BUILD14）**：
    - **(a) `setConnectionTimeout(200, 2000)`**：`m_timeout_ms`（纯 HTTP 的 TCP connect 超时）设 **200ms**——跨省/大延迟电台服务器 connect 常 >200ms → `connect()` 直接超时 → `connecttohost` 返回 false → 表现"无反应"。修法 = 改 **`setConnectionTimeout(2000, 2000)`**（纯 HTTP 给 2s）。
    - **(b) 看门狗 `radioWasRunAt` 是 `static` 且从不随新连接重置**：`pumpRadio()` 里 `if (audio.isRunning()) radioWasRunAt=millis(); else if (millis()-radioWasRunAt > 8000) stopRadio();`——若上次 `millis()-radioWasRunAt` 已接近 8s（或初始 0 导致开机后首次 pump 即 `millis()>8000`），**新流刚缓冲（`isRunning` 短暂 false）就会被 `stopRadio()` 秒杀** → "连上又没声"。修法 = 把 `radioWasRunAt` 提为**全局**，且**每次 `connecttohost` 成功后 `radioWasRunAt = millis()`**（新流从连接时刻起获得完整 8s 宽限）。
  - **③ 播放/显示不同步根因 = 播放条不随 `currentPlayingSid` 刷新**：延迟起播在 `modeRadio()` 里 `connecttohost` 返回后才置 `currentPlayingSid=sid`，但**没有任何地方置 `listScrollDirty`** → `drawRadioPlayBar()`/`drawRadioList()` 只在 `listScrollDirty`/`modeChanged` 时才重绘 → 播放条停留在旧台名。修法 = 在 connect 成功/失败两处都加 **`listScrollDirty = true;`**（下一帧 `modeRadio()` 的 `if (listScrollDirty){ drawRadioList(); drawRadioPlayBar(); }` 即重绘，播放条与列表高亮与 `currentPlayingSid` 同步）。
  - **本次改动（`src/main.cpp`，3 处，纯 ASCII 注释）**：① `radioWasRunAt` 提全局（删 `static`）；② connect 成功分支加 `radioWasRunAt = millis();` + `listScrollDirty = true;`，失败分支加 `listScrollDirty = true;`；③ `setConnectionTimeout(200,2000)`→`setConnectionTimeout(2000,2000)`。**未改** `Audio.cpp`（v3.0.12e 库），也未升级库——根因都在 `main.cpp` 控制流。
  - **✅ 已编译+烧录+boot 验证（2026-09-27 22:09~22:11）**：终端 shell 集成虽报 `✖ Invalid input`，但**命令实际有执行**（`echo hi` 能出输出只是回显被误判）→ 改用 `Start-Process pio.exe … -RedirectStandardOutput build15b.log -WindowStyle Hidden`（脱离终端编译）+ `Start-Process python.exe scripts\flash2.py -WindowStyle Hidden`（烧录），全程用 `read_files` 读 `build15b.log`/`flash2.log`/`boot2.log` 跟踪。编译 **SUCCESS（RAM 74.2% / Flash 24.6%，与 BUILD14 持平）**；`flash2.log` = `upload exit=0` + `uploadfs exit=0`；`boot2.log` 出现 **`ESP32-S3 Smart Radio Starting... BUILD15-RADIO-FIX`**（版本串一并从 BUILD14 升 BUILD15，见 `setup()` L2261）+ `font16: OK  font24: OK` + `radio_meta: 1670 stn` + `Setup done.`，且开机自动播元旦节日音乐 `lhttp.qtfm.cn port 80 → established in 44 ms → MP3 decoder init`（纯 HTTP 无 TLS OOM）、`[Hol] cached 2025/2026/2027`。
  - **⚠️ 待用户上机确认（点屏行为，串口看不到）**：① 点 广东股市/枣庄综合 应能连上播放（不再"无反应"，connect 超时 200ms→2s）；② 点 中国之声 不再 crash（crash 本属旧实验固件，当前固件本就干净）；③ 切换电台后播放条台名与所播台一致（`listScrollDirty` 已随 connect 成功/失败置脏）；④ 新流连接缓冲期不再被看门狗秒杀（`radioWasRunAt` 随每次 connect 重置）。
  - **⚠️ 教训**：① "总是停/无反应"先分清是 **connect 超时太短**（`setConnectionTimeout` 第 1 参=纯 HTTP TCP connect 超时，200ms 太苛刻）还是**看门狗误杀**（`isRunning` 在缓冲期本就短暂 false，看门狗计时基必须随新连接重置）——两者都表现为"点了没声"，但修法完全不同。② **播放条/列表高亮是"脏标记驱动重绘"**（`listScrollDirty`），任何改变 `currentPlayingSid` 的地方（连上/停/失败）都必须置脏，否则显示与实际脱节（与之前 `modeChanged` 忘清同一类"标志位不同步"坑）。③ 老 crash dump 里的 `DBG` 打印若在当前源码里已不存在 → 是**废弃实验固件**，别在当前固件上追它的 backtrace（PC 对不上符号）。
- **收音机"总是停" + 节假日 301 根治（2026-09-27，根因=设备无 TLS 能力，全改纯 HTTP + 节假日离线化；编译 SUCCESS RAM 74.2%/Flash 24.6%，实机连续播放验证）**：
  - **根因（决定性）**：本设备 (ESP32-S3 N16R8) 空闲内部 RAM 只剩 ~52KB，**完全无法做 TLS**——boot log 反复见 `esp-aes: Failed to allocate memory`（电台 HTTPS 流）+ `esp-sha: Failed to allocate buf memory`→`start_ssl_client: -1`（节假日 HTTPS 同步）。**任何 https:// 电台流都必失败→"总是停"**。探测 (`scripts/_probe_http.py` 逐 host 80 端口 GET) 确认 **52/94 个 HTTPS host 支持纯 HTTP，覆盖 917 电台**（含用户最常点的 `lhttp.qtfm.cn` 343 / `lhttp-hw.qtfm.cn` 254 / `lhttp.qingting.fm` 138 / `piccpndali.v.myalicdn.com` 47）。
  - **① 电台：`data/stations.bin` 全改纯 HTTP**（关键：固件读的是 **`stations.bin` 二进制，不是 `stations.json`**！只改 festival.json 永远修不好主列表）。`scripts/_rewrite_bin_http.py`：用 `gen_radio_meta.py` 同款已验证解析器**原样解析现有 bin**（1652 站/40 region），只把 url `https://`→`http://`，其余字段/顺序/region 全保留，重算 offset 重打包（**先做字节级 round-trip 校验 = 逐字节一致，证明无损**）。翻转 **1015 条** URL，体积 156988→155973。`radio_meta.bin` **无需重建**：它按 **sid 索引**引用 1652 条 bin 记录，我保序→"全部"列表 1652 条自动变 HTTP（重建反会因 `(name,url)` 匹配不上而坏）。
  - **② 节假日：离线化（不再联网）**：timor.tech 纯 HTTP 会 301 跳 https，`HTTPClient` 不跟跨协议 301，且设备又无 TLS → 在线同步**永远失败**。改 `src/holiday_sync.cpp` `fetchAndCache()` 为**直接 `return false`**（读 LittleFS 预置缓存，不发任何网络请求），删 `#include <WiFiClientSecure.h>`/`<HTTPClient.h>` + 死代码。PC 端 `preview/sync_holidays.py`（HTTPS 在 PC 可用）拉好 `holidays_2025/2026/2027.json` 放 `data/`，`uploadfs` 烧进 LittleFS。**boot 后 `[Hol] cached 2025(33)/2026(39)/2027(0)` 全部离线加载**，日历 休/班 角标正常。
  - **③ 音频库自动选 client**：`ESP32-audioI2S` `connecttohost(url)` 按 scheme 选——`http://`→纯 `WiFiClient`（无 TLS），`https://`→`WiFiClientSecure`（OOM 元凶）。URL 全改 `http://` 后**天然走纯 HTTP，零 TLS**。
  - **验证（实机串口，决定性）**：`pio run` SUCCESS (RAM 74.2%/Flash 24.6%)；`pio run -t uploadfs` Hash verified；boot log **零 esp-aes/esp-sha/esp-tls/301**；`stations.bin: 40 regions, 1652 stations`；`[Hol] cached 2025/2026/2027`；连播多台全走 port 80——`CCTV-13`(piccpndali.m3u8)→AAC decoder、`怀集音乐之声`(lhttp.qingting.fm MP3)→`stream ready`+**连续 `syncword found`**（MP3 帧持续解码=持续播放，非"播一会儿断"）；堆稳定 ~49–51KB 无 OOM。**与"又停了"相反**。
  - **⚠️ 教训**：① **修电台前必须查固件到底读哪个文件**（`stations.bin` 不是 `stations.json`）——改了没用的文件=白改。② 改二进制 db 先做**字节级 round-trip 校验**（parse→pack 不改 = 逐字节一致）再真改，防解析器丢数据（曾遇 3 字节差异：name 含非 UTF-8 字节被 `.decode('utf-8','replace')` 改成 U+FFFD；**解法=name/region/cat/freq 全存 raw bytes，只碰 url**）。③ **设备无 TLS 时，一切网络请求（流/同步）都要离线化或改纯 HTTP**；`HTTPClient` 不跟跨协议 301 是个隐形坑（纯 http 会 301→https）。④ 烧固件用 `Start-Process pio.exe`（脱离终端）或 `scripts/flash2.py`，`pio run -t uploadfs` 推 `data/`。
  - **工具留存**：`scripts/_rewrite_bin_http.py`（bin 改写，带 round-trip 自检）、`scripts/_probe_http.py`（host HTTP 探测）、`preview/_run_cap.py`（复位+串口捕获，参数 `COM5 <秒数>`）、`data_stations.bin.bak`（改前备份）。
  - **⚠️ 待用户确认**：实机双击任意电台应**持续播放不中断**；日历页 休/班 角标显示 2025–2027 数据。
- **黄历弹层再设计（2026-09-25，粗体大数字 + 宜/忌单行 + 星期双框 + 内层装饰框；两侧 lockstep，编译 SUCCESS RAM 69.5%/Flash 24.8%）**：
  - **背景**：在上一版"纸质黄历"弹层基础上再改 5 处：① 大数字从 46×88 升到**粗体 62×112**（新 `include/almnum.h`，10 个 8-bit 遮罩 + `AlmMasks[]` 指针表，Bahnschrift **权重 700**，~400KB PROGMEM）；② 12 时辰宜忌格 **删掉**，改**宜/忌 单行**（各 3 项，按 `dayStd%12` 循环取）；③ 星期X 改**花纹双框**（外 `ALM_BORDER` 中绿 + 内 `ALM_BORDER2` 浅绿）；④ 日期行**对称**（年左对齐 / 星期居中 / 月右对齐）；⑤ 面板加**内层装饰框** `ALM_BORDER2`（传统黄历双线框）。
  - **新常量**：`ALM_BORDER2=0x8BC4`（浅绿，内框/星期内框）+ `ALM_DW=62/ALM_DH=112`（almnum.h）；删旧 `ALM_GZ2/ALM_HS/ALM_HB`（时辰吉凶池），改 `ALM_YI_ITEMS[12]`/`ALM_JI_ITEMS[12]`（宜/忌项池）。
  - **固件 `src/main.cpp`**：`#include "almnum.h"`；`almBigNum(x0,y0,day)` 改用 `AlmMasks` 逐行流式合成 124×112（`static uint16_t row[124]` 248B 内部 RAM，`day<10` 十位留纸底，每行单次 `pushImage`）；`drawAlmPopup` 绘制块重写——外层框 `drawRoundRect(...,ALM_BORDER)` + 内层装饰框 `drawRoundRect(PX+4,PT+4,PW-9,PH-9,6,ALM_BORDER2)`；标题 y=PT+10；大数字 @(cx-62,PT+28)；左右竖排 x=42/438 (PT+32, 14px/字)；日期行 dy0=PT+152（`年`左对齐 x=42 / `星期X` 双框居中（外框 `bx,dy0-2..bx+bw-1,dy0+17` + 内框 `bx+1,dy0-1..bx+bw-2,dy0+16`）/ `月`右对齐 x=438）；方位 2×2 左(gx1=36,136×20) + 月亮单框右(cx+68..cx+204，图标 @cx-8,gy0+2 + 名 @cx,gy0+30 居中)；宜/忌行 yj0=PT+208（`宜:`绿 + 3 项 / `忌:`暗红 `ALM_BAD` + 3 项，`half=(438-42)/2` 左右半区，项距 `half/3`）。删除 `stem0`/`curHour`/`isToday`（时辰高亮逻辑）。
  - **预览 `preview/make_preview.py`（逐像素镜像）**：新增 `ALMNUM_H` 路径 + `load_almnum()`（解析 `ALM_DW/ALM_DH`/`AlmMask[i]` → `ALM_MASKS`）+ `ALM_BORDER2` + `ALM_YI_ITEMS/ALM_JI_ITEMS`（同 565 值）；`alm_big_number(day,fg565,bg565)` 改用 `ALM_MASKS`（62×112，565 原值逐通道混合→`c565`，与 `almBigNum` 同款）；`draw_almanac_popup` 绘制块重写（`d.rounded_rectangle` 双层框 / `d.rectangle` 星期双框 / 宜忌 `hw_text` 单行）。
  - **验证**：`check_hw_patterns.py` R1–R5 全过（0 警告）；`pio run` **SUCCESS**（RAM 69.5% / Flash **24.8%**，+~1KB 来自 almnum.h 329KB PROGMEM 数字表——注：mask 占 PROGMEM/Flash 不占 RAM，RAM 与上一版基本持平）；`py_compile` make_preview 通过；`preview_alm_workday.png` 逐像素核对：双层边框、粗体 "25" 绿笔画 on 米纸底、左右竖排、星期X 双框、月亮单框（图标+盈凸月）、宜/忌 单行（宜:绿 / 忌:暗红）全部正确。
  - **布局微调 + 星期 bug（2026-09-25，用户实机反馈；两侧 lockstep，编译 SUCCESS RAM 69.5%/Flash 24.8% 不变）**：
    - **① 标题 `丙午马年` 太靠顶、压住内层装饰框线**：C++ 标题顶锚点 `PT+2`(行18) 与内层框 `PT+4`(行20) 重叠；预览用 `PT+10` 中锚(顶≈行22) 同样压框。**修法**：C++ 顶锚点 `PT+2`→**`PT+8`**(行24，与内层框行20 留 4px 空隙)；预览中锚点 `PT+10`→**`PT+16`**(=硬件顶锚点+8，lockstep 逐像素)。
    - **② 左右竖排贴边且不对称**：旧用左 `x=42` / 右 `x=438`（相对左缘对称，但 14px 字左锚点使右列右缘到 451，**贴到**内层框右缘 451，且距左缘 18px 而距右缘仅 4px → 观感偏右）。**修法 = 以中心 `cx=240` 为轴对称**：C++ `almVCol(42,…)/almVCol(438,…)` → `almVCol(cx-178,…)`/`almVCol(cx+178,…)`(=62/418)；预览同款 `cx-178`/`cx+178`。左右各距内层框约 24px，以中心大数字为轴左右对称。
    - **③ 中间星期显示"星期期"（预览 mapping bug，硬件正确）**：硬件 `wday="星期%s" % weekHdr[tm_wday]`（`tm_wday` 0=日，`weekHdr[0]="日"`）**正确**；但预览 `WEEK_FULL[date.weekday()][1]` 有**两处错**：`date.weekday()` 是 0=一..6=日 而 `WEEK_FULL` 是 0=日..6=六（索引差一）；且 `[1]` 取的是"星期X"的**第 2 个字符="期"**（应为末字）。**修法**：预览改 `WEEK_FULL[(date.weekday()+1) % 7][-1]`（先对齐索引再取**末字**，如"五"）。验证：2026-09-25(周五)→`星期五`、09-20(周日)→`星期日` 等全对。
    - **验证**：`check_hw_patterns.py` 0 警告；`pio run` SUCCESS（RAM/Flash 不变，仅改 3 个坐标 + 1 处 preview 字符串索引 + 注释）；`python make_preview.py`/手工 `draw_almanac_popup` 出图逐像素核对：标题在框下、左右竖排对称内移、星期"星期五"正确。
    - **⚠️ 踩坑**：预览星期 bug 是 **`WEEK_FULL[i][1]` 取错字符位**——`"星期四"` 的 `[1]`="期"（不是"四"），要取末字 `[-1]`；且 `date.weekday()`(0=Mon) 与 `WEEK_FULL`(0=Sun) 索引差一，必须 `+1)%7`。硬件 `tm_wday` 本就 0=Sun 与 `weekHdr` 对齐，故硬件一直正确——**lockstep 破口仅出在 preview**。
  - **⚠️ 踩坑**：① `almnum.h` 生成后 `AlmMask[i]` 数组 + `AlmMasks[]` 指针表 = **~400KB PROGMEM**（10×62×112×1B），Flash 24%→24.8%（安全）。② 大数字合成**方向**沿用已修正约定 `mask: 0=笔画(fg),255=底(bg)`，合成 = `out = bg*m + fg*(255-m)`（m=0→fg 绿笔画, m=255→bg 纸底）——与 `digits.h`/`dialnum.h` 同约定，C++/Python 两侧一致。③ 宜/忌项循环取 3 项：`pool[dayStd%12], pool[(dayStd+1)%12], pool[(dayStd+2)%12]`（两侧同公式）。
  - **⚠️ 待烧录确认**：实机双击某天弹新黄历——粗体大数字 / 宜忌单行 / 星期双框 / 内层装饰框 与 live_preview 逐像素一致；单击仍只选中。
- **黄历弹层重设计为"纸质黄历"布局（2026-09-25，两侧 lockstep，编译 SUCCESS RAM 69.5%/Flash 23.7%）**：
  - **背景**：用户给参考截图（408×535 奶油纸面传统黄历），要求把旧"老式黄历"弹层（标题/干支年/月相/值神冲煞/宜忌3项）重做成**纸+绿**风格。
  - **新布局（面板 24,16..456,272 = 432×256，配色 565）**：`ALM_PAPER=0xE73A`(米纸) / `ALM_BORDER=0x646B`(中绿框) / `ALM_GREEN=0x43E8`(主绿: 标题/大数字/吉) / `ALM_GREEN2=0x7CED`(浅绿: 竖排/日期行/忌) / `ALM_HL=0x2AE6`(当前时辰高亮底) / `ALM_BAD=0x9D98`(凶)。内容自上而下：① 标题 `丙午马年`(16px 居中) ② **大数字 day**（92×88，两位 46×88 遮罩并排，绿笔画 on 纸）③ 左右**竖排**（左`农历X月X` / 右`丁酉月癸巳日`，14px/字 `almVCol`）④ 日期行 `2026 | 星期X(带边框) | 9月`(16px) ⑤ **2×2 方位+月亮**（3列×2行 136×20 网格：喜神东南/财神正南 | 月亮图标+月相名 | 福神正南/阳贵东南）⑥ **12 时辰宜忌**（12px/字×3行：干/支/吉凶；左 2×2 标签 时宜/辰忌；当前时辰深绿底高亮）⑦ 右下角"关闭"按钮（**几何与 handleCalendarTap 完全一致，未改** → live_preview/交互无需动）。
  - **时辰吉凶规则**：`ALM_HS`(丑午未=吉) / `ALM_HB`(子卯酉=凶) / 其余=中；时辰干 = 五鼠遁 `stem0=(dayStd%10)%5*2`，各时辰顺推 `TG2[(stem0+i)%10]`（参考 2026-09-16 癸巳日 → 壬子… 校验一致）。
  - **固件 `src/main.cpp`**：新增 `almBigNum(x0,y0,day)`（逐行流式合成 92px 行缓冲→每行 `pushImage`，**只占 368B 内部 RAM**，`day<10` 时十位留纸底）+ `almVCol(x,y0,s,col,sz)`（逐 UTF-8 字 `drawScaledChar` 竖排）+ 重写 `drawAlmPopup`。删除旧 `ALM_YI/JI/SHA/DEITY/AUSP/TITLE/GZ/OK/DIM`。函数顶部 `// lint:skip-rule-1`（弹层一次性静态绘制，无连续动态区域）。
  - **预览 `preview/make_preview.py`（逐像素镜像）**：`ALM_*` 同 565 值（另存 `ALM_PAPER_565/ALM_GREEN_565` 原值供大数字合成）+ `alm_big_number(day,fg565,bg565)` + 重写 `draw_almanac_popup`（`aa_compose_scaled_glyph` 竖排/时辰格、`hw_text` 中文、`draw_moon`）。
  - **⚠️ 踩坑（本次）**：① **遮罩混合方向反了**——`digits.h` 约定 `0=笔画,255=底`，正确合成 = `(bg*m + fg*(255-m))/255`（m=0→fg, m=255→bg）；初版写成 `(bg*(255-m)+fg*m)` 导致数字变成"绿底挖空"（C++ 与 Python 都错，已双双修正）。② **Python RGB→565 精度**：大数字合成必须用**原始 565 常量**（`0xE73A`/`0x43E8`）逐通道算再 `c565()` 转 RGB，不能从 RGB 元组重建 565（`c565(0x43E8)`→RGB→565=0x3857≠0x43E8）；绿通道是 **6 位**（`>>2`）不是 3 位。③ C++ 竖排 `rcol` 曾误用 `"%s%s%s%s"`（4 占位 2 参数）→ 改 `"%s月%s日"`。
  - **验证**：`check_hw_patterns.py` R1–R5 全过（0 警告）；`pio run` **SUCCESS**（RAM 69.5% / Flash 23.7%）；`py_compile` make_preview+live_preview 通过；`preview_alm_workday.png` 逐像素核对：大数字绿笔画(65,125,65)=`c565(0x43E8)`、纸底(230,230,213)=`c565(0xE73A)`，与参考图一致。
  - **⚠️ 待烧录确认**：实机双击某天弹"纸质黄历"，纸面/大数字/竖排/时辰格/关闭按钮与 live_preview 逐像素一致；单击仍只选中。
- **日历"双击某天 → 老式黄历"弹层（2026-09-25，两侧 lockstep，编译 SUCCESS RAM 69.4%/Flash 23.7%）**：
  - **交互规格（用户 2026-09-25 定）**：日历页**单击某天 = 只选中**（底部信息条显示该日农历，原流程不变）；**400ms 内双击同一天 = 打开该日黄历**（暖白纸面弹层覆盖整页）；黄历内**右下角"关闭"按钮 = 返回日历**（不再"点任意处关闭"）。
  - **固件 `src/main.cpp`**：
    - 状态：`calAlmOpen` + 双击判定 `calLastTapDay/Y/M/Ms`（`millis()`，<400ms 且日/年/月全同 = 双击）。
    - `drawAlmPopup(y,m,d)`（~L1524，前置声明在 drawCalendar 前）：几何 `PX=24,PW=432,PT=16,PH=256` 圆角 10 纸面 `ALM_PAPER=0xFFE0` + 棕边 `ALM_BORDER=0xA1E0`。内容：标题"黄历"(暗红 `0xE259`) / **干支年** `丙午马年`(`nl_zodiac_year` 按春节分界) / **干支月+日** `丁酉月 癸巳日` / 农历 `八月十五 星期四` / 月相图标(`drawMoonIcon`)+名称 / 分隔线 / **值神**(青龙…金匮 6 池按 dayStd%6, 吉绿/凶暗红) + **冲X煞X** / 宜3项(绿 `0x271C`) + 忌3项(暗红 `0x9D98`)（宜忌按 dayStd%12 循环取 3，规则化装饰性）。右下角"关闭"按钮 `ALM_DIM=0x77A6` 底 + 纸色字。
    - **干支算法（2026-09-16 手算验证 = 丙午马年/丁酉月/癸巳日）**：干支日 `(JDN+49)%60`；月支 = 当前节气奇数化 `&~1` 后 `/2+1`（小寒=0 → 子月，寅=2）；月干 = 五虎遁 `(2+(yg%5)*2)%10` 起寅月递增（yg=(lyz%10+6)%10）；年干支/生肖用真实农历年。
    - `handleCalendarTap`：`calAlmOpen` 时**仅命中关闭按钮**（几何与绘制处同款 `cbW=aa宽("关闭")+20` 等式）才关；日期网格命中时按 400ms 双击判定开/只选中。
    - **月相文字左缘 `cx+10`**（250）：避开月亮图标右缘（203），4 字名（64px）止于 314 仍在面板内。
  - **预览 `preview/make_preview.py`（逐像素镜像）**：`ALM_*` 同 565 值 + `alm_jdn`/`alm_pil`（走已有 `lunar_solar_to_lunar`/`solar_terms`/`moon_phase`，与 nongli.cpp 同表同算法）+ `draw_almanac_popup(img,d,y,m,dd)`（`hw_text` 中锚点 = 硬件左上锚点 ytop+8）；`draw_calendar_scene(..., almanac=False)` 新参数，`almanac=True` 时全屏覆盖弹层（镜像 `if (calAlmOpen) { drawAlmPopup; return; }`）。`main()` 出 3 张示例：`preview_alm_workday/alm_weekend/alm_month25.png`。
  - **`preview/live_preview.py`**：`cal_alm_open` + `cal_last_tap`/`cal_last_tap_ms`（`time.time()*1000`，同 400ms 规则）；`_clk_on_click` 头部：弹层开 → 仅命中关闭按钮（同几何 +2 容差）才关；网格命中 → 选中 + 双击判定；渲染传 `almanac=cal_alm_open[0]`。
  - **验证**：`check_hw_patterns.py` R1–R5 全过（0 警告）；`pio run` **SUCCESS**（RAM 69.4% / Flash 23.7%，+~2KB 字符串表，无新增缓冲）；preview 出图成功（PREVIEW_EXIT=0）；`preview/_alm_verify.py` 逐像素核对 **ALL OK**（面板/边框/纸色/关闭按钮边角/标题/干支/宜忌/月相全过；月相 2026-09-25 phase=0.4675 近满月 → 暗面 <1px 被取整舍掉全亮是正确行为，验证脚本已按相位感知）；`py_compile` live_preview+make_preview 通过。
  - **⚠️ 踩坑记录**：① C++ `const char* a1 = X, a2 = X` 只在 a1 绑定指针类型 → 编译报 ambiguous overload，必须逐行声明。② `drawCalendar` 先于 `drawAlmPopup` 调用需前置声明。③ 验证月相不能要求"暗部像素 >0"（近满月相位暗面亚像素被整数取整舍掉）。④ 编辑器工具对 `\\` 反斜杠有转义歧义，main.cpp/make_preview.py 的 CJK 大段插入用 Python 字节级脚本（`scripts/_add_almanac.py` 等，CRLF-aware + 幂等）。
  - **⚠️ 待烧录确认**：实机日历页双击某天弹黄历、内容正确、关闭按钮返回；单击仍只选中显示农历。
- **导航栏"选中项"配色统一（2026-09-25，折中方案：选中=黄字 / 未选=白字 + 背景高亮块，两侧 lockstep）**：
  - **背景**：固件 `drawNavBar()` 旧写法 `uint16_t tc = sel ? TFT_YELLOW : 0xC618`——选中**黄字**、未选**深灰绿字**；而 `make_preview.py` `draw_nav()` 是**全部白字**、只靠 `NAV_SEL` 背景块区分选中 → 实机"时钟"（选中）显黄、预览显白，**lockstep 破口**，观感两边不一致。
  - **用户决策 = 折中**：选中项**黄字**、未选项**白字**，外加 `NAV_SEL` 背景高亮块（选中项既有黄字又有背景块，未选纯白字）。两边统一成这个效果。
  - **① 固件 `src/main.cpp` L421**：`sel ? TFT_YELLOW : 0xC618` → `sel ? TFT_YELLOW : TFT_WHITE`（未选从深灰绿 `0xC618` 改纯白 `TFT_WHITE`；选中黄字不变，背景块 `COLOR_NAV_SEL` 不动）。
  - **② 预览 `preview/make_preview.py` L632**：`draw_nav` 文字 `fill=(255,255,255)` → `fill=c565(0xFFE0) if i == sel else (255,255,255)`。**用 `c565(0xFFE0)` 而非近似纯黄**：`0xFFE0` 正是固件 `TFT_YELLOW` 的 565 值，经 `c565()` 转 RGB 后逐像素=硬件面板对该色号的显示，保证 lockstep 逐像素一致（不是"看起来像的黄"）。
  - **③ 死生成器 `scripts/rebuild_main.py` L45 同步**：同款 `TFT_YELLOW : 0xC618` → `TFT_YELLOW : TFT_WHITE`（该文件 AGENTS.md 明令禁止运行，但保持 lockstep 一致性）。
  - **验证**：`check_hw_patterns.py` R1–R5 全过（0 警告）；`make_preview.py` 出图后**逐像素核对导航栏**——"时钟"(选中)=`(240,240,0)`黄、"日历/收音机/天气"(未选)=`(240,240,240)`白、选中背景块=`(64,64,64)`(`NAV_SEL`)，三项全对。`pio run` 通过（**RAM 69.4% / Flash 23.7%**，仅改 1 个颜色常量、无新增缓冲）。
  - **⚠️ 待烧录确认**：实机"时钟"=黄字+背景块、其余三项=白字（与 live_preview 逐像素一致）。
  - **⚠️ 教训/流程坑**：本次卡在 **build 反复失败**——真因不是代码，是 **Windows 终端 shell 集成抽风**（误报 exit 1 + 抓不回输出），且**每发一条新终端命令就会掐掉上一条正在跑的 `pio run`**（冷编译 44~120s 赶不上跑完就被杀）。**解法**：用 `Start-Process pio.exe -ArgumentList run -RedirectStandardOutput build.log -WindowStyle Hidden -PassThru` 把 build 跑成**脱离终端的独立进程**，之后只读 `build.log`（用 `read_files`，不碰终端）查结果，build 就不会被误杀。
- **秒针"7~8 点方向双线" + "今日中秋节"溢出背景（2026-09-25，用户实机确认；接上一条秒针双线修复）**：
  - **① 秒针 7~8 点仍是双线（角相关）**：上一条"补中心线(0/±1)"只解决水平/垂直方向；**在 ~45° 方向（7~8 点、1~2 点）offset `(int)(±1.5)` 截断后垂直位移≈2px**，而 45° 相邻对角线行只相距 1px → 中心线(0)与 offset 线(±2)之间仍**漏 1 行对角** → 双线/白缝（仅斜 45° 出现，故水平/垂直看不见）。**根因不是扫秒**，是离散像素下"3 条 1px 平行线"在 45° 的几何空隙。
    - **修法 = 新增 `thickLineBuf(buf,W,x0,y0,x1,y1,col)`**：对中心 Bresenham 线**逐像素画 3×3 实心方块**（任何角度都连续实心，无方向性漏行），秒针改单句 `thickLineBuf(analogFace,S,lcx,lcy,sex,sey,AC_S)`（替换旧 3 条 `drawLineBuf`）。与 preview `d.line(width=2)` 的粗线目标一致。
    - **验证**：Python 模拟 45°(7:30) 角度——旧 3 条 offset 线 **84 个线内白洞**，`thickLineBuf` **0 白洞**，PIL `width=2` 0 白洞（一致）。`pio run` 通过（**RAM 69.4% / Flash 23.7%**，仅 +1 函数、无新增缓冲）；check_hw_patterns R1–R5 0 警告。**待烧录确认**：秒针在 7~8 点/1~2 点仍为实心红线、无双线。
  - **② "今日中秋节" 超出文字背景**：行1 右侧 `今日XX` 紧跟干支，长节日名（中秋节=80px）右缘到 **x=472，溢出圆角背景右缘 DATE_RIGHT=449 约 23px**。
    - **修法 = 行1 分隔线左移 `cx=200`**（原 `(x0+x1)/2=240`），把右半块(农历/干支/今日XX)整体左移进左半块(日期+星期，恒止于 x≈193)右侧空白：`今日中秋节` 右缘到 **432（距右缘 17px 余量）**，全部落在背景内。`cx` 仅用于行1，行2 右侧锚定不受影响。
    - **lockstep**：`make_preview.py` `draw_date` 与 `live_preview.py` `_draw_date_1bit` 的 `cx` 同步改 200。验证：背景右缘(x≥449)外非背景像素 = **0**（无溢出）。
  - **⚠️ 教训**：① 用"3 条 1px 平行线"模拟粗线在**斜 45°** 会因整数截断漏对角行（水平/垂直看不出）——硬件粗线统一用**中心线逐像素填 3×3 实心块**（`thickLineBuf`），任何角度都实心。② 日期条右半块若贴"居中分隔线"放，长节日名会溢出背景右缘；分隔线左移到左半块右侧空白即可腾出右缘空间。
- **秒针"前后摇摆着往前走"（2026-09-25，用户视频确认；接上一条秒针可见修复）**：
  - **症状**：红色秒针已能看见，但走针不是一路平滑向前，而是**每往前扫一下就抖一下/回跳一下**（"前后摇摆着往前走"）。
  - **根因 = 秒针位置混用两个不同相位的时钟源**：`double t2s = (s + (nowMs % 1000) / 1000.0) / 60.0` 里 `s` 是 **NTP 秒**（RTC 时间跳变），`nowMs % 1000` 是 **millis 毫秒**（独立硬件定时器）。两者相位不锁：NTP 秒在 millis 的某个随机相位上从 30→31（比如 millis=250ms），于是 t2s 从 `30.249/60` 瞬间跳到 `(31+0.250)/60 = 31.25/60` —— **每"秒"都跳一次整秒**（跳前=本秒末尾 0.999 附近，跳后=下一秒 0.25 附近，回跳 0.75 秒），视觉上就是秒针走一步抖一下。
  - **修法 = "NTP 整数秒 + millis 分数(锚定)"**（`src/main.cpp` `drawAnalogClock`）：
    ```c
    static unsigned long lastSec = 0xFFFFFFFFUL, secAnchorMs = 0;
    if (s != (int)lastSec) { lastSec = (unsigned long)s; secAnchorMs = nowMs; } // NTP 秒变: 锚定该秒起点
    double t2s = (s + (nowMs - secAnchorMs) / 1000.0) / 60.0;
    ```
    **整数秒用 NTP `s`**（绝对秒位正确、与翻页秒个位一致），**分数部分用 `millis`**（单一单调时钟，每帧匀速 +6°/s 无跳变）。关键技巧：在每个 NTP 秒边界把"该秒起点"锚到 `millis`（`secAnchorMs`），于是分数 = `(nowMs - 锚点)/1000` 在 [0,1) 内单调增长 —— 秒针**既平滑又指向正确秒位**，与 `live_preview`（用单一真实墙钟 `se + ms_real/1000`）效果一致。
    - 为什么不能"纯 millis"（`nowMs%60000`）：纯它虽平滑，但 `millis` 从**开机**计，相对真实秒有固定相位偏移（最多差 ~1 秒/6°），秒针会指向**错误秒位**（且与翻页时钟的 NTP 秒个位不同步）——已弃用该写法。
    - 为什么不能"NTP 秒 + millis%1000"（旧写法）：NTP 秒与 millis 相位不锁，NTP 秒在 millis 的随机相位上递增 → 每"秒"回跳/前冲 ≈1 秒 = "前后摇摆"。
  - **验证**：`pio run` 通过（RAM 69.4% / Flash 23.7%，+4 字节 static）；烧 COM5（Hash verified）。**待上机确认**：秒针平滑匀速扫秒、不再前后摇摆、且指向的秒位与真实秒一致。
  - **⚠️ 教训**：**连续平滑指针 = "墙钟整数秒(定位) + 单一单调时钟的分数(走针)"，且分数要在每个整数秒边界重新锚定**。纯"墙钟整数秒 + 毫秒余数"会因两时钟源相位不锁而每"秒"跳变（摇摆）；纯"单调时钟"虽平滑但相对墙钟有固定偏移（指错秒位）。`live_preview` 之所以干净，是 PC 上 `time()` 秒和毫秒同源、天然锁定。
- **"每翻一次闪一下" + 指针秒针仍不可见（2026-09-25，实机照片确认；接上一条彩虹花屏修复）**：
  - **症状**：上一条把翻页彩虹花屏修好后，用户反馈 ① 指针时钟**还是看不到红色秒针**（时/分针正常）；② 翻页时钟**每翻一下，那张卡就闪一下**（彩色 blur 已无）。
  - **① 秒针根因 = 位移运算里的 cast 位置错**：`int sex = lcx + (int)cosf(sa) * 60` 里 `(int)cosf(sa)` 先把 cos∈[-1,1] **截断成 int**（除正负横轴外全 = 0），**再** ×60 → 对几乎所有角度 `(int)cosf=0`、`(int)sinf=0` → 秒针端点塌到圆心、被中心枢纽盖住 → 几乎整圈看不见（只在正 12/3/6/9 点瞬间闪过）。时/分针走 `hand()`（`dx=cosf` 先乘后取整）所以正常。**修法**：端点(×60)与尾部(×15)都改成**先乘后取整** `(int)(cosf(sa) * 60)` / `(int)(cosf(sa) * 15)`。preview 本就是对的（`cx + math.cos(sa) * 60`），lockstep 恢复一致。
  - **② "每翻一次闪一下" 根因 = 翻页"完成"瞬间用了非原子的 `drawCard()`**：`tickFlapClock` 翻页完成分支（`fs.flipping=false`）调 `drawCard(i, fs.shown, false)`，而 `drawCard` 是**多笔直写屏**：`fillRoundRect(白底投影)` → `fillRoundRect(白牌面)` → `pushImage(数字)` → `drawHingeCard`。白牌面填充把卡擦白，若 ST7796S 无 TE 的面板扫描恰好落在"擦白之后、数字 push 之前"的空窗 → 拍到一张**空白白卡** = 用户看到的"每翻一次闪一下"。翻页**过程中**是原子的（`drawFlapFrame` 整卡合成进 `cardSram` 单次 push），所以只有"完成"那一下闪。
  - **② 修法 = 新增 `drawCardStaticAtomic(idx,digit)`（原子稳态牌）**：把最终静态牌（下缘投影 + 白底 + 完整数字(上/下半 `renderHalfCard`) + 铰链 `hingeBufCard`）**逐像素与 `drawCard` 相同**地合成进 `cardSram`，再**单次** `tft.pushImage` 整牌（内部 RAM 原子帧）。翻页完成分支改用 `drawCardStaticAtomic(i, fs.shown)`。像素与 `drawCard` 完全一致，只是把多笔直写换成一笔原子推，消除"擦白→画数字"之间的扫描空窗。
  - **验证**：`check_hw_patterns.py` R1–R5 全过（0 警告）；`pio run` 通过（**RAM 69.4% / Flash 23.7%**，未新增缓冲）；`preview_analog.png` 秒针 = 完整红线（中心→约 5 点 + 尾部配重 + 红色中心枢纽）。已烧 COM5。
  - **⚠️ 教训**：① `(int)cosf(x) * N` 是**先截断后乘**（结果几乎全 0），要 `(int)(cosf(x) * N)` 先乘后截断——位移/坐标计算里 cast 必须包住整个浮点表达式。② **"原子帧"规则同样适用于翻页"完成"瞬间**：刚翻完的稳态牌必须单次 `pushImage` 整帧，不能退回 `drawCard` 的多笔直写屏（擦白→画前景的扫描空窗 = "每翻一次闪一下"）。
- **秒个位"彩虹花屏" + 翻页彩色 blur + 指针秒针不可见（2026-09-25，实机照片确认）**：
  - **症状（用户照片）**：① 第 6 张牌（秒个位，每秒翻）下半部是**彩虹花屏**（上半"3"干净，下半噪点）；② 翻页时背景有**彩色 blur**；③ 指针时钟**看不到秒针**。
  - **①② 根因 = 翻页渲染路径仍经 PSRAM**：`halfTop`/`halfBot`（预渲染半牌）+ `bandBuf`（翻片带）全是 `ps_malloc` 的 **octal PSRAM**。之前的"彩虹花屏"修复只把**最终 pushImage** 换成内部 RAM（`cardSram`），但 `buildFlapBand`/`drawFlapFrame` 仍**直接从 PSRAM 读**半牌像素做压缩/翻片。秒个位每秒翻、相2 大量读 `halfBot`（旧下半）→ 八线 PSRAM SPI-DMA 突发读错乱 → 彩虹。卡片 1~5 不翻所以干净。
  - **①② 修法 = 翻页全程只碰内部 RAM（彻底弃用 PSRAM 预渲染）**：删除 `ps_malloc` 的 `halfTop/halfBot/bandBuf`，新增内部 RAM `flapTmp[CARD_W*60]`(6.7KB 中转) + `bandBuf[CARD_W*60]`（改内部 RAM）；新增 `renderHalfCard(digit,top,out)`——**每次按需**从 flash `DigitMasks` 现渲染单张半牌（圆角白牌面+8位灰度数字，与 `flapInit` 旧逻辑同几何）进内部 RAM；`buildFlapBand` 先 `renderHalfCard(oldD,…)` 再压缩；`drawFlapFrame` 相1/相2 都改 `renderHalfCard` + `memcpy` 进 `cardSram`。`flapInit()` 只算齿轮列明暗，**无 PSRAM 分配、不再可能失败**（`flapReady` 恒真）。
  - **③ 根因 = 秒针太细**：`drawAnalogClock` 秒针用 `drawLineBuf` 画 **1px** 红线，在 4.0" 屏（480px 宽）上细到看不清（preview 在显示器放大看才清楚）→ 误以为"没有秒针"。
  - **③ 修法（两侧 lockstep）= 秒针加粗到 ~2px**：`main.cpp` 秒针改画**两条**平行线（沿垂直方向 offset ±1.5px：`ox=-sinf(sa)*1.5, oy=cosf(sa)*1.5`），合成 ~2px 宽；`make_preview.py` 秒针 `d.line(…, width=1)` → `width=2`。
  - **验证**：`pio run` 通过（**RAM 69.4%**〔+`flapTmp` 6.7KB，安全〕/ Flash 23.7%）；`check_hw_patterns.py` R1–R5 全过（0 警告）；`make_preview.py` 出图，`preview_analog.png` 秒针清晰可见（红 2px + 尾部配重 + 中心枢纽）；grep 确认 `main.cpp` 翻页路径已无 `ps_malloc/halfTop/halfBot/pushPSRAM` 残留。**待烧录确认**：秒个位翻页无彩虹、翻页无彩色 blur、指针秒针可见且平滑扫秒。
  - **⚠️ 教训**：内部 RAM 中转只解决"**最终 pushImage 的源**"——若上游 `memcpy/逐像素读` 的**源本身在 PSRAM**（本例 `buildFlapBand` 读 `halfBot`），octal PSRAM DMA 读错乱照样花屏。**凡会动的区域，整条像素路径（源→压缩/合成→缓冲→pushImage）都要走内部 RAM**，不能只中转最后一步。半牌这种"内容稳定"的数据可**按需现算**（flash 掩码→内部 RAM），比预渲染存 PSRAM 更稳。1px 线在 4.0" 屏不可见，指针类细线至少 2px。
- **全屏闪烁根因：`loop()` 忘了清 `modeChanged`（2026-09-25，用户报告"除导航栏外整个屏幕都在闪，指针时钟也闪"）**：
  - **根因**：`modeChanged` 置位后（启动 `setup()` / 切页面 / 切时钟样式 / WiFi 重连）从未被清除 → 每个模式的入口 `if (!modeDrawn[i] || modeChanged)` **每帧（loop 无节流，~1000+ FPS）都为真** → 整页（fillRect 清场 + 全部内容，480×270≈260KB/帧）被以满速反复重绘。导航栏用 `navDirty`（每帧清）所以不闪——正好对上"除了导航栏都在闪"的症状。翻页样式也被连带：`tickFlapClock` 每帧调用、`FLAP_FRAME_MS` 门控失效；指针样式每帧整面重绘 → 把 20MHz SPI 打满（与之前"指针整面闪烁"同一症状的另一根因）。
  - **修法**：`loop()` 的 `switch (currentMode){...}` 之后加 `modeChanged = false;`（一行，含注释）。之后各模式只在首次进入/切换时整页重绘，指针恢复 33ms 节流、翻页恢复 40ms 节流。
  - **验证**：`pio run` 通过；烧 COM5（Hash verified）；boot log 干净（无 crash）。**待上机确认**：各页面稳态不闪，指针秒针平滑扫秒，翻页每秒翻一次。
  - **⚠️ 教训**：这类"标志位驱动重绘 + 无节流 loop"的 bug **preview 永远看不到**（PIL 整帧原子渲染、无 SPI 带宽上限）。今后任何 `xxxChanged/xxxDirty` 标志必须在 loop 末尾清，或入口函数内清。
- **翻页第二相"下半数字更新方向"修正（2026-09-24，用户指出"下半从下往上变，不符合机械原理"）**：
  - **问题**：翻页分两相——相1（上半翻片下压，露新上半）方向一直对；**相2（下半翻片上翻，补新下半）** 之前把"旧下半翻片带"贴在 `seamY`（下半区域**顶端**），`bandH` 随 p→1 收缩 → 新数字从**底边往上**露出。真·翻页里下半翻片铰链在**底边**、向上折，新数字应从**铰链缝(上半区域顶)往下**露出（与相1对称）。
  - **修法（两侧 lockstep，仅 2 处几何 + 阴影方向）**：
    - `src/main.cpp` `drawFlapFrame` 相2：旧下半翻片带锚点从 `seamY` 改**底边** `(seamY + half - bandH)`；折边线 `lineBufCard` 从 `seamY + bandH` 改 `seamY + half - bandH`。`buildFlapBand` 加 `bool flipShade=false` 参数（相2 传 `true`）：阴影 `t` 由 `yy` 改 `H2-1-yy`（相2 翻片带**顶端(靠缝端)=暗、底端(铰链端)=亮**，与相1 互为镜像；相1 保持 `yy` 不变）。
    - `preview/make_preview.py` `draw_flap_card` 相2：`img.paste(band,(x, seamY))` 改 `(x, y + h - bandH)`、折边线同步；`_shade_band` 加 `flip=False` 参数，相2 传 `flip=True`（`t=(H-1-yy)/…`）。
  - **验证**：`pio run` 通过（RAM 65.3% / Flash 23.7%，未新增缓冲）；`check_hw_patterns.py` R1–R5 0 警告。预览逐行核对（5→3，相2 p=0.5）：下半区域**顶 30 行=新数字"3"(dNEW≈0 精确匹配)**、**底 30 行=旧"5"翻片带** → 新数字从**顶(缝)往下**露出，符合机械原理；p=0.25 旧带占多数、p=0.8 仅剩底边一条窄带。**待烧录确认**：实机下半翻片新数字从上往下变。
  - **⚠️ 铁律**：改含中文的 `src/main.cpp` 一律用 **Python 字节级 I/O**（`open(...,'rb')`/`'wb'`，见下节），绝不用 PowerShell `Get-Content/Set-Content`（会按 ANSI 重编码致中文乱码）。本次即全程 Python `str.replace` 字节替换。
- **⚠️ 铰链改回"细缝 + 两端小金属转轴帽" + main.cpp 编码事故抢救（2026-09-24）**：
  - **① 铰链改回极简**（用户给参考图：细缝 + 两端转轴帽，不要大金属条）：
    - 新几何：中央 **1px 细缝** `CARD_SEAM`（x+4..x+w-5）+ 两端 **4×6 金属转轴帽**（x..x+3 / x+w-4..x+w-1，跨 seamY-3..seamY+2，6 行竖渐变 高光×2→中×2→暗沟×2）。删掉旧的 5px 渐变条 + 3×5 转轴钉。⚠️ 帽要**够大够深**才读得出"转轴"：3×3 太小/太淡在白牌面上消失，扩到 4×6 后清晰。
    - **常量（16-bit RGB565，两侧同值）**：`CARD_SEAM=0x3A48`（细缝）+ 两端转轴帽 `HINGE_HI=0x9514`(顶高光) / `HINGE_MID=0x4ACB`(中) / `HINGE_LO=0x1945`(底暗沟)（删除 `HINGE_SH/HINGE_REF/HINGE_PIN`）。⚠️ 转轴帽要**偏深**（白牌面上浅色的帽会消失、读不出"转轴"）：用户参考图确认=两端要有**清晰的深色金属转轴钉**。⚠️ 之前 `0x123013/0x2A4A2B` 是 **24-bit 越界**（`uint16_t` 截断成错色、`c565()` 也错），务必用 ≤0xFFFF 的 565 值。
    - **三处渲染同步**：`drawHingeCard`（直写屏）、`hingeBufCard`（cardSram）、`bandCard` 内联（bandSram）——均 = 1 条细缝 `CARD_SEAM`（x+4..x+w-5）+ 两端 **4×6 帽**（`kc[6]={HI,HI,MID,MID,LO,LO}`，6 行 × 4 列）。`main.cpp` 里 `kc[]` 各值必须 `(uint16_t)` 强转，否则 565 值超 int 范围会 narrowing 报错。
    - **预览 lockstep**：`make_preview.py` `HINGE_*=c565(0x…)` 同值 + `_draw_hinge` 改 1 条 `d.line`(seam, x+4..x+w-5) + 两端各 6 行 4px 宽 `d.line`(帽, kc 同 `kc[6]`)。
    - **验证**：`pio run` 通过（RAM 65.3% / Flash 23.7%）；`check_hw_patterns.py` R1–R5 0 警告；`preview_after.png` 逐像素核对：seam 行中段=细缝色(0x3A48)、两端各 4px=帽中段色(HINGE_MID)；帽 6 行=高光×2/中×2/暗沟×2（y105..110）；牌面中心仍白；翻片帧(p1/p2)帽保持、翻片带压向铰链。**待烧录确认**：实机=细缝+两端 4×6 金属转轴帽（与参考图一致）。
  - **② main.cpp 编码事故 + 抢救**：用 PowerShell `Get-Content -Raw | -replace | Set-Content`（无 `-Encoding`）改 main.cpp → 整文件被按系统 ANSI(GBK) 重编码，**所有中文（注释+字符串字面量）乱码/丢失**。git/origin 只有**旧版**（无 split-flap 手动工作）→ **不能 `git checkout` 恢复**（会丢全部手动改动）；VS Code Local History 无 main.cpp 备份；`.pio`/`src_backup` 均旧版。
    - **抢救法**：文件字节仍在盘上 → `raw.decode('gbk','replace').encode('gbk','replace').decode('utf-8','replace')` 还原约 90% 中文（ASCII 代码 100% 完好），写回 UTF-8。剩 **1557 个字符**永久丢失（PowerShell 首读 UTF-8 当 GBK 时丢的字节）。
    - **补全**：33 个**功能性字符串字面量**（断行号、会编译失败+屏幕乱字）按 git 旧版/nongli.cpp 逐条重写（navLabels/lunarMonths/weekDaysFull/eventColor 节气+节日/日期格式/收音机/日历/收藏 等）。剩 **~370 个注释**中文仍乱（`?` 残留）——**纯外观，不影响编译/烧录/屏幕**（所有代码行 + 所有字符串字面量已确认 0 残留）。
    - **⚠️ 铁律补充**：改含中文的源文件**绝不用 PowerShell `Get-Content/Set-Content`**（默认按 ANSI 重编码）。要用 Python（`open(...,'rb').read()` 处理字节、`open(...,'wb')` 写回）或 `editor` 工具。
  - **翻页效果**：本次**未改**翻页动画（`buildFlapBand`/`_shade_band` 透视压缩+变暗机制保留，与参考图"9"翻片一致）。
- **金属铰链 + 翻页再放慢（2026-09-24，用户要求"铰链更大/金属色/金属质感 + 翻页再慢"）**：
  - **① 铰链变大 + 金属色**：原"中央铰链"只是 1px 暗缝 `CARD_SEAM` + 1px 白线（`CARD_FACE`），两端 2×3 深色销。现改为 **5px 竖直渐变金属条**（模拟抛光钢受光：顶 `HINGE_HI=0x875E` 受光高光 → `HINGE_SH=0x6B50` 亮面 → `CARD_SEAM=0x5AC8` 中央暗沟 → `HINGE_SH` 亮面 → `HINGE_REF=0x9168` 底反光条），两端 **3×5 转轴钉** `HINGE_PIN=0x4308`。铰链从 2px 扩到 5px，白牌面上的金属条对比明显、有立体金属感。
  - **硬件三处同步**（`src/main.cpp`，全部在数字/半牌之后画，避免被盖住）：
    - 新增 `drawHingeCard(x,seamY,w)`（直写屏幕，`drawCard()` 末尾调用）+ `hingeBufCard(seamY,w)`（写 `cardSram`，`drawFlapFrame()` 末尾调用，依赖 `lineBufCard` 故其声明须在前）+ `bandCard()` 内联同款循环（写 `bandSram`）。三处几何一致：条从 `x+2..x+w-3`，钉在 `x..x+2` 与 `x+w-3..x+w-1`，均跨 `seamY±2`。
    - 常量：`CARD_SEAM` 由 `0x2108` 改 `0x5AC8`（中央暗沟），新增 `HINGE_HI/HINGE_SH/HINGE_REF/HINGE_PIN`。
  - **预览 lockstep**（`preview/make_preview.py`）：新增 `HINGE_*` 常量（同 565 值）+ `_draw_hinge(d,x,seamY,w)`（5 行 `d.line` + 6 个 `d.rectangle` 钉），`draw_flap_card()` 末尾改调 `_draw_hinge`（替换旧的 2 条线 + 4 个销）。`preview/_mp.py` 是无人引用的死拷贝，未改（`live_preview.py` 直接 `import make_preview as mp`）。
  - **② 翻页再慢**：`FLIP_TOP_MS`/`FLIP_BOT_MS` 由 `260/220` 改 **`400/340`**，三处同步：`src/main.cpp`、`preview/live_preview.py`（同名常量）、`preview/make_preview.py`（GIF `P1,P2=400,340`，帧数 `15→22` 覆盖完整两相）。齿轮滚动量公式不变（`clicks*PITCH + ease(p)*PITCH`），只是每相时间变长 → 滚动更慢更清晰。
  - **验证**：`pio run` 编译通过（**RAM 65.3% / Flash 23.7%**，与改前一致——未新增缓冲）；`scripts/check_hw_patterns.py` R1–R5 全过（0 警告）；`python make_preview.py` 无报错，逐像素核对 `preview_after.png` 中心缝 5 行 = `HINGE_HI/SH/SEAM/SH/REF`、两端钉 = `HINGE_PIN`、缝上方 y-3 仍为白牌面。**待烧录确认**：实机铰链呈金属条（更大更立体）、翻页节奏更慢。
- **两处"彩色撕裂"修复（2026-09-24，无 TE 扫描撕裂类，与"彩虹花屏"不同源）**：
  - **① 右齿轮顶行彩色 blur（翻页滚动时）**：根因 = 每帧 3 笔独立 `pushImage`（翻牌 + 左齿轮 + 右齿轮），无 TE 面板扫描线与写入交错 → 后推的右齿轮(分/秒间隙)顶行出现新/旧混合彩色 smear（长度随机）；左齿轮因扫描相位合适故干净。preview 看不到（PIL 整帧原子）。修法 = **齿轮带原子推屏**：新增 `bandSram[BAND_W×CARD_SRAM_H]`（BAND_W=166，从 GAP1 齿轮左缘到 GAP2 齿轮右缘，内部 RAM ~40.6KB）+ `bandCard()`/`bandRoller()`/`drawRollerBand()`——把两枚齿轮 + 中间两张牌(2,3) 合成进一块缓冲后**单次** `tft.pushImage(157,48,166,123)`，两齿轮同扫描相位原子更新，消除左右不对称。`tickFlapClock` 滚动分支：牌2/3 未翻页走 `drawRollerBand`，翻页中退回 `drawRoller`×2（避免覆盖翻页动画）；`modeClock` 初始化分支跳过 `drawCard(2/3)` 改由 `drawRollerBand(0)` 渲染。RAM 52.8%→65.3%。
  - **② 切时钟样式时 收音机/天气 之间彩色竖条（长度随机）**：根因 = 翻页⇄指针切换置 `modeChanged=true` → `loop()` 里连带**重绘导航栏**（fillRect + 4 个 AA 字 + 4 条分隔线多笔 SPI），与整页重绘交错撕裂，条落在 x=360 分隔线处（扫描位置随机→长度随机）。修法 = **导航栏重绘与页面重绘解耦**：新增 `navDirty`（初始 true），`loop()` 改 `if (navDirty) { drawNavBar(); navDirty = false; }`；**仅**导航项 tap 命中（`btnIdx != currentMode`）时置 `navDirty=true`，样式切换**不**置 → 切样式只重绘页面、不碰导航栏。
  - **验证**：`pio run` 编译通过（RAM 65.3% / Flash 23.7%）；`scripts/check_hw_patterns.py` R1–R5 全过。**待烧录确认**：① 右齿轮顶行滚动不再彩色 blur（左右对称干净）；② 切时钟样式不再出现导航区彩色竖条。
  - **铁律补充**：无 TE 面板上**同一逻辑帧内相邻动态区域的多个独立 pushImage 会撕裂**（新/旧混合=彩色 smear，长度随机=扫描位置随机）——preview 永远看不到。修法 = 把同帧内必须同时更新的相邻区域**合并进一块内部 RAM 缓冲、单次 pushImage**（本例齿轮带 166×123）；跨"逻辑帧"的整页重绘（如导航栏）不要与页面重绘在同一帧并发。
- **真·机械翻页动画 + 3D 滚动齿轮（2026-09-21，固件与 preview 逐帧对齐）**：
  - **背景**：旧硬件 `flipDigitAnim` 是"暗快门"（整牌画新数字+上半变暗→恢复），不露旧下半，与 preview `draw_flap_card` 真·翻页不一致。本次把 preview 的完整两相动画 + 齿轮**整体移植**进 `main.cpp`，替换旧 `flipCard`/`flipDigitAnim`/`.current`（已删除）。
  - **`src/main.cpp` 新增 4 函数（~L516–699，`__FLAP_ANCHOR__` 处）**：
    - `flapInit()`：预渲染 10 个数字的上/下半牌（白底圆角 + 8 位灰度数字遮罩合成，`flapInRoundRect` 圆角）+ 齿轮列明暗 `rollerColB[16]`（`0.20+0.80·sin(πt)` 中心亮 + 中央窄高光 `·0.30`，= preview `_roller_col_profile`）。**全部存 PSRAM**（`ps_malloc`，失败→退化无齿轮/无动画，打印 `[Flap] PSRAM alloc failed`）。预算 ~26.8KB。
    - `buildFlapBand()`：旧半牌(56×60)竖向压缩到 bandH，逐行线性变暗 `k=1-(0.12+0.70·strength·t)`（t=0 铰链→1 远端，= preview `_shade_band` 的 cosθ+背面变暗）。
    - `drawFlapFrame()`：两相绘制（phase1 贴 `halfTop[new]`+`halfBot[old]`+旧上半翻片带压向铰链；phase2 贴 `halfTop[new]`+`halfBot[new]`+旧下半翻片带）。**索引 = `&halfTop[d*CARD_W*half]`**（`half=CARD_H/2=60`，注意不是 `HH`）。翻片带边线 `FLAP_BAND_LINE=0x7350`，铰链线 `drawFastHLine(x+2,seamY,w-4,CARD_SEAM)`。
    - `drawRoller()`：间隙 3D 圆柱（16×120），齿纹 `tf=0.62+0.38·cos(θ)-0.08·cos(2θ)` 梯形波随 `scroll` 下移，两端端盖+左右轮廓 `(40,40,52)`，`pushImage(x0,CARD_TOP,...)`（= preview `draw_roller`）。
    - `tickFlapClock()`：**非阻塞状态机**（`fstate[6]`），每帧推进 phase1(400ms)→phase2(340ms)→完成（2026-09-24 再放慢，原 260/220）；滚动量 `clicks*ROLLER_PITCH + flapEase(p)*ROLLER_PITCH`（一相=一格，`ROLLER_PITCH=14`），仅在滚动量变化 >0.01 时重绘两枚齿轮。稳态几乎不重绘（省 SPI 带宽）。
  - **`modeClock()` 集成**：首次进入分支 `flapInit()`+初始化 `fstate[]`+画 6 张静态牌+静态齿轮；每帧分支 `if (flapReady) tickFlapClock(target)`（替换旧阻塞 `flipDigitAnim`）。指针样式分支不变。
  - **常量（= preview）**：`FLIP_TOP_MS=400`/`FLIP_BOT_MS=340`（2026-09-24 再放慢版，原 260/220，更早 150/130）/`ROLLER_W=16`/`ROLLER_PITCH=14`/`GAP1_CX=165`/`GAP2_CX=315`（`COLON_X(+COLON_X2)+COLON_W/2`）。`CARD_W=56/CARD_H=120/CARD_TOP=48/CARD_RADIUS=10`。
  - **验证**：`pio run` 编译通过（RAM 21.6% / Flash 23.8%）。`ps_malloc` 是 ESP32 Arduino core 的 PSRAM 分配函数（**不是** `psram_malloc`，后者不存在→首编报错已修）。preview `draw_flap_card`/`draw_roller` 逐行核对一致。
- **指针表盘整面闪烁修复（2026-09-22，实机照片确认）**：
  - **根因**：`modeClock()` 的指针分支**无节流**，`loop()` 每轮（~1000+ FPS）都调 `drawAnalogClock()` → 整张表盘（`fillCircle` 白底≈33KB + 60 刻度 + 12 数字 `pushImage` + 3 针）≈40KB 全量重绘，把 **20MHz SPI 打满** → 秒针走针时**整面闪烁/撕裂**。翻页分支已有 `FLAP_FRAME_MS=40` 节流，指针分支漏了。
  - **修法**：`main.cpp` 新增 `#define ANALOG_FRAME_MS 33`（≈30 FPS），指针分支按 `millis()` 门控：`if (anow-anaLast >= ANALOG_FRAME_MS) { tft.startWrite(); drawAnalogClock(h,m,s); tft.endWrite(); }`。秒针 6°/s → 0.2°/帧，仍平滑；整帧包 `startWrite/endWrite` 减少 CS 切换。与翻页节流同理。
  - **验证**：`pio run` 编译通过（RAM 21.6% / Flash 23.6%）。待烧录确认实机秒针不闪。
- **指针表盘"3 针闪烁 + 秒针不动" & 翻页牌"铰链消失" 修复（2026-09-23，实机照片确认）**：
  - **症状**：数字重影修好后，① 3 根指针不停闪烁、秒针像不动；② 翻页牌中间的铰链缝消失了（翻页动画时还有，稳态数字卡上没了）。
  - **② 铰链消失根因 = `drawCard()` 里铰链线画在数字 `pushImage` 之前**：大数字（48×70，占牌中央 y+25..y+95）盖住了中央铰链线（seamY=y+60）→ 缝被数字吃掉。`drawFlapFrame()` 顺序是对的（缝画在 push 之后），只有稳态 `drawCard()` 错了。preview `draw_flap_card` 也是**先数字后缝**（L556-558）。**修法**：`drawCard()` 把两条 `drawFastHLine`(seamY/seamY+1) 从数字 pushImage **之前**移到**之后**。
  - **① 3 针闪烁 + 秒针不动 根因 = 非原子帧（帧撕裂）**：上一版 `drawAnalogClock` 先把静态表盘 `tft.pushImage`（这步**擦掉了针**）推完，再**分多次** `tft.drawLine/fillTriangle/fillCircle` 画 3 针。ST7796S 无 TE 同步、面板持续扫描，"推表盘→画针"中间有空窗 → 面板扫描到时针/分针/秒针被擦掉的那一小段 → 3 针闪；秒针最快，多数帧被拍到"已擦未画"→ 看着不动。
    - **修法 = 整帧合成到同一块内部 RAM 缓冲再单次 pushImage（原子帧）**：`drawAnalogClock()` 每帧 `buildAnalogFace()` 重建白圆+刻度+12数字（同时清掉上一帧针），再自写 `fillCircleBuf`/`fillTriangleBuf`（边符号测试+裁剪）把时/分/秒针+枢纽直接画进 `analogFace`，最后**一次** `tft.pushImage(cx-R,cy-R,146,146,analogFace)` 推整帧。与已验证干净的翻页时钟同款（单 pushImage，无中间状态），彻底消除闪烁/撕裂。秒针 6°/s 仍平滑。
  - **验证**：`pio run` 编译通过（RAM 36.5% / Flash 23.7%）；烧 COM5 成功（Hash verified）；`_bootlog.py` 干净 boot（`clock style: analog`、无 crash）。
  - **铁律补充**：**凡"会动的区域"（指针/翻页/齿轮），一帧内所有像素必须先合成到一块内部 RAM 缓冲，再单次 `pushImage` 整帧**——不要"推背景 → 再分笔画前景"，无 TE 时必撕裂/闪烁。静态卡片的铰链线**必须画在数字之后**。
- **指针表盘"数字重影 + 秒针不随动" 根因修复（2026-09-23，实机照片确认，替代 `TFT_eSprite` 方案）**：
  - **症状**：指针表盘数字重影/双重（和之前翻页花屏同一类损坏），且秒针像不动。
  - **真正根因 = `TFT_eSPI& g = analogSpr` 基类引用**：`fillCircle` / `fillTriangle` 在 `TFT_eSprite` 里**没有被覆写**（`Sprite.h` 只覆写 `drawPixel/drawChar/drawLine/drawFastVLine/drawFastHLine/fillRect/...`）。所以经基类引用 `g.fillCircle(...)` 执行的是 `TFT_eSPI::fillCircle` → **直写真实面板**（绝对坐标），`g.fillTriangle`（4 块棒棒针）同理直写面板；只有 `g.drawFastHLine`（表盘白圆/刻度）与 `g.drawLine`（秒针）落到 sprite 缓冲。于是面板被写入一圈散乱的圆帽/枢纽，随后 `pushSprite` 用（部分过时的）sprite 缓冲**覆盖**面板 → 数字重影 + 秒针位置错乱/不随动。
  - **修法（弃用 Sprite）**：`main.cpp` 删除 `InternalRamSprite analogSpr`，改为：`static uint16_t analogFace[146*146]`（内部 RAM，42.6KB）+ `analogFaceReady`；`buildAnalogFace()` 一次性烘焙**静态表盘**（白圆盘 + 页底 `COLOR_BG` + 60 刻度(自写 `drawLineBuf` Bresenham) + 12 数字 `blitDialNumBuf`，直接写 `analogFace`，绕开 `pushImage` 裁剪/swap 歧义）；`drawAnalogClock` 每帧只 `tft.pushImage(cx-R,cy-R,146,146,analogFace)` + 直渲 3 针（`tft.fillTriangle/fillCircle/drawLine`，全走**虚函数安全**，与已验证干净的翻页 `drawCard` 同款路径）。不再整面 `fillCircle` 擦白（消除"露白"闪），数字/刻度/针全部干净、秒针平滑扫秒。
  - **验证**：`pio run` 编译通过（RAM **36.5%**〔+`analogFace` 42.6KB〕/ Flash 23.7%）；`pio run -t upload --upload-port COM5` 烧录成功（"Hash of data verified"）；`python preview/_bootlog.py COM5` 干净 boot（`clock style: flap`、PSRAM enabled、font16/font24 OK、WiFi OK、无 crash）。
  - **铁律补充**：**凡 `TFT_eSPI` 派生 Sprite，绝不要用 `TFT_eSPI&` 基类引用去调 `fillCircle`/`fillTriangle`（非虚）——经基类引用会直写面板**。要么用派生类对象/指针直接调，要么（本项目做法）干脆不建 Sprite、直接 `tft` 渲染。
- **翻页牌/齿轮"彩虹花屏"修复（2026-09-22，用户澄清后定位，实机照片确认）**：
  - **用户澄清（关键）**：卡片 1~5（时/分/秒的十位）不翻页所以**干净**；第 6 卡（秒个位，每秒翻）显示**彩虹静态**；两枚齿轮（每秒随翻页滚动）也**彩虹静态**。这就是用户一直说的"翻页/指针秒走的时候闪"。**不是相机模糊**。
  - **根因 = `tft.pushImage()` 直接从 PSRAM 源读**：ESP32-S3 上 SPI-DMA 对**八线(octal)PSRAM** 的突发读取取到错乱数据 → 彩虹花屏。这正是 TFT_eSPI `Sprite.cpp` 的约束（PSRAM sprite 仅当 `!DMA_Enabled`）。铁证：**干净区域**（`drawCard` 的 `dgb`、`blitDialNum` 的 `nbuf`、`compositeGlyph` 的 `_buf`）全是**内部 RAM**；**花屏区域**（`halfTop`/`halfBot`/`bandBuf`/`rollerBuf`）全是 **PSRAM**（`ps_malloc`），一一对应。
  - **修法**：`main.cpp` 新增内部 RAM 中转缓冲 `static uint16_t sramPush[CARD_W*(CARD_H/2)]`(3360px) + `pushPSRAM(x,y,w,h,src)`（`memcpy` 到内部 RAM 再 `pushImage`，与已验证干净的 `drawCard` 同款）。把 `drawFlapFrame()` 的 4 处（新/旧上半、新/旧下半、翻片带）与 `drawRoller()` 末尾的 `tft.pushImage(..., PSRAM缓冲)` 全部换成 `pushPSRAM(...)`。PSRAM 仍用来**存**预渲染半牌/齿轮（省内部 RAM），只是推屏前中转一次。
  - **验证**：`pio run` 编译通过（RAM 23.7%〔+`sramPush` 6.7KB〕/ Flash 23.6%）。**待烧录确认**：秒个位翻页 + 齿轮滚动不再彩虹，秒针（指针样式）不整面闪。
  - **⚠️ 仍未定位**：白露/秋分/八月十二等**中文小字"糊到背景"**是**另一个**问题（其 `pushImage` 来自内部 RAM `_buf`，非 PSRAM）→ 需上机标定排查（见下"上机待验证"），不要与花屏混为一谈。
  - **上机待验证**（烧录后 `pio run -t monitor` 115200）：① 秒个位翻/齿轮滚 = 干净无彩虹？② 指针样式秒针 = 不再整面闪？③ 中文小字 = 是否仍糊（若仍糊，做 565 色卡 + 串口 dump "中" 16×16 原字节 vs `font_simhei_16.font` 期望，定位是字库/合成/显示伽马哪一层）。
- **`data/festival.json` 填 14 条节日音乐 URL（2026-09-21）**：
  - **播放路径**：`checkFestivalMusic()` → `festivalUrl(name)`（查 `festivalDoc["festival_music"][name]`）→ `audio.connecttohost(url)`（**与电台同一网络流路径**，MP3/HLS，无本地 LittleFS 播放分支）。每天一次，需联网、且收音机未在播。
  - **URL 来源 = 复用 `stations.json` 已验证的同 CDN 电台流**（qingting.fm / qtfm.cn 直连 MP3、CCTV-15 音乐 HLS、airport-lounge、chinesemusicworld），逐条 `Range` GET 探测返回真实音帧（`ff…`/`#EXTM`）后才写入。
  - **⚠️ 键必须与 `nl_festival()` 输出**逐字一致**，否则静默不播**：13 个键直接来自 `LUNAR_FESTS`/`SOLAR_FESTS`/`nl_jq_text`（春节/元旦/元宵节/劳动节/端午节/国庆节/中秋节/重阳节/圣诞节/春分/秋分/夏至/冬至）；**清明节要写成 `清明`**（4/4 不在节日表，走节气分支 `nl_jq_text[6]="清明"`，不是"清明节"）。
  - **烧录**：`pio run -t uploadfs`（推 `data/`）。
- **节假日(放假/补班)在线同步 + LittleFS 缓存（2026-09-21，手机日历模式）**：

- **节假日(放假/补班)在线同步 + LittleFS 缓存（2026-09-21，手机日历模式）**：
  - **数据源**：`http://timor.tech/api/holiday/year/<year>`（**纯 HTTP 无 TLS** → 用 `WiFiClient`+`HTTPClient`，省 ~30KB RAM）。字段：`MM-DD` → `{holiday:bool, name}`；`holiday:true`=**放假(休)**，`holiday:false`=**补班(班)**（`after` 区分前/后补班）。2027 在国务院发布前返回空 `{}` → 靠定时重新同步自动拾取后续公告。
  - **固件 `src/holiday_sync.cpp` + `include/holiday_sync.h`**（namespace `holiday`）：
    - `init()`（setup，WiFi/NTP 后调用）：幂等 `LittleFS.begin()` 得 `fsReady`，按当前时间加载 当年-1..+1 的 `/holidays_<year>.json` 缓存进内存表 `YearData`（紧凑 `type[13][32]` + 名称池 `String`，`findYear` 线性找）。
    - `tick()`（loop 每帧调用）：联网后**首次**同步 当年+次年；之后**每 24h**（`millis()` 防回绕）再同步。`syncing` 防重入。
    - `ensureYear(y)`（`drawCalendar` 调用）：浏览到某年且无缓存 → 入队，**在线立即**后台补同步（不在 draw 里阻塞）。
    - `dayType(y,m,d)` → `HOL_NONE/OFF/WORK`；`dayName(y,m,d)` → 名称。拉取失败**保留上次好缓存**（绝不清空日历）。缓存上限 4 年，超出淘汰最旧（删 LittleFS 文件）。
  - **`main.cpp`**：`#include "holiday_sync.h"`；`setup()` WiFi 分支后 `holiday::init()`；`loop()` `handleTouch()` 后 `holiday::tick()`；`drawCalendar()` 开头 `holiday::ensureYear(calYear)`。cell 循环高亮优先级：**今天(青框) > 放假 `CAL_OFF=0xF800`红+下划线 > 补班 `CAL_WORK=0x6CF1`浅蓝+下划线 > 周末红 > 白**（`tft.fillRect(cx-6, cy+11, 12, 2, color)` 画 2px 下划线）。
  - **预览镜像（lockstep）**：`preview/sync_holidays.py`（urllib 拉同一 API → 存 `preview/holidays_<year>.json`，失败保留旧缓存；`python sync_holidays.py [--offline]`）。`make_preview.py`：`load_holidays(year)`/`hol_day_type()` 读该 JSON；`draw_calendar_scene` cell 循环用同一 `HOL_OFF/HOL_WORK`（= `c565(0xF800)`/`c565(0x6CF1)`）+ 2px 下划线；`main()` 开头 best-effort 自动同步（镜像固件 boot 同步，离线用缓存不阻塞）。`live_preview.py` 启动同样 best-effort 同步。
  - **验证**：`pio run` 编译通过（RAM 21.4% / Flash 23.7%）。`python make_preview.py` 出 `preview_calendar.png`：9月 20(补班,周日)→浅蓝、25/26/27(中秋放假)→红、21(今天)→青框，其余周末红/平日白，逐像素核对一致。
  - **注意**：`preview/holidays_*.json` 是**运行生成物**（不进 git 也行）；预览/固件都靠"联网自动同步"刷新，离线退回缓存。
- **日历两处修复（2026-09-21，生肖年 + 休班角标缩小，两侧 lockstep）**：
  - **① 生肖/干支年 bug（2027-01-01 被算成"蛇年"，春节又跳到"羊年"）**：旧规则 `if (lm>=10 && mon<=2) ly--`（农历11/12月即公历1月时取前一年）在**农历11月/12月但还没到春节**时误判——2027-01-01 真实是农历丙午(马)年冬月廿四，却被减一年算成乙巳(蛇)。
    - **修法 = 按"春节(正月初一)"分界**：新增 `nongli` 的 `int nl_zodiac_year(int year,int mon,int day)`（`include/nongli.h` + `src/nongli.cpp`）——查该农历年"正月初一"落在公历的年内日序（1月→`nl_month_days` 找 `101`，2月同理），当日 `doy >= sf_doy` 取当年，否则取前一年。`main.cpp` 三处（`drawDateRow`/`drawCalendar` 信息条/`todayEventName`）全部改用 `nl_zodiac_year(...)`。
    - **预览（lockstep）**：`lunar_solar_to_lunar` 本来就返回**真实农历年 `ly`**，其边界 = 正月初一（积日累年法），所以直接 `lyz = ly` / `slyz = sly`（删掉 `-1 if (lm>=10...)`）。已验证：2026-12-31→丙午马、**2027-01-01→丙午马**（修复前是蛇）、2027-02-06(正月初一)→丁未羊、2028-01-22→丁未羊（2028春节在2/10，2/1前仍是羊）。`nl_festival` 不按 `ly` 匹配（只按农历月/日 + 公历月/日），**节日/音乐不受影响**。
  - **② 休/班角标太大**：原来用 **16px** AA 字在 64×30 单元里 `drawStringFast(cx+15, cy-14)`，和 16px 日期数字挤在一起。
    - **修法 = 缩放到 12px**：新增 `XFont::drawScaledChar(x,y,ch,dst,fg,bg) const`（`include/xfont.h` public + `src/xfont.cpp`）——把 16×16 AA 灰度字形**面积平均**缩到 `dst×dst`（`dst=12`→每目标像素平均 2×2 源块），逐通道 565 合成 `(bg*(255-m)+fg*m)/255`（与 `compositeGlyph` 同款）后 `pushImage`。`main.cpp` cell 循环改用 `font16.drawScaledChar(cx+19, cy-14, 0x4F11 /*休*/ : 0x73ED /*班*/, 12, fc, CAL_LIGHT_BG)`。
    - **预览（lockstep）**：`make_preview.py` 新增 `aa_compose_scaled_glyph(img,x,y,ch,dst,fg,bg)`——读**同一份** `data/font_simhei_16.font`（AA blob，magic `XAAX`，与硬件 `font16` 同源）做同样的面积平均+逐通道合成，逐像素一致（字库缺失则跳过不崩）。`draw_calendar_scene` cell 循环改用 `aa_compose_scaled_glyph(img, cx+19, cy-14, 0x4F11:0x73ED, 12, fc, CAL_LIGHT_BG)`。
  - **验证**：`pio run` 编译通过（RAM 21.6% / Flash 23.8%）。`python make_preview.py` 无报错；缩放假字 bbox=12×12（16px 版是 16×16），放大看 20→班、26→休 都贴右上角不压数字；生肖年按上表逐项核对一致。
- **日历页浅色主题 + table 网格线（2026-09-21，仅日历页，其余页仍深色）**：
  - **范围**（用户确认）：只把**万年历页**改成浅色（白底）+ 加 **1px 表格网格线**；时钟/收音机/天气/导航栏**保持深色**。不做全局切换、不占 NVS。
  - **配色（两侧 565 完全一致，见 `main.cpp` `CAL_*` / `make_preview.py` `CAL_*`）**：`CAL_LIGHT_BG=0xFFFF` 白底；`CAL_GRID=0xC65A` 浅灰蓝网格线；`CAL_HDR=0x5ACB` 星期表头/箭头（深灰）；`CAL_TITLE=0x12D9` 月份标题/"今日"（蓝）；`CAL_NORM=0x31A6` 普通日期数字（深灰）；`CAL_STRIP_L=0xEF9F` 底部信息条底色（浅蓝灰）；`CAL_GOLD_L=0xCC21` 干支（深金，白底可读）；`CAL_WORK_L=0x01F4` 补班（深蓝，白底可读）。放假 `0xF800`/周末 `0xF800` 红、今天青框+黑字**保持不变**（白底下同样可读）。
  - **`main.cpp` `drawCalendar()`**：① 整页背景 `fillRect(0,0,480,NAV_BAR_Y, CAL_LIGHT_BG)`（原来 `COLOR_BG` 黑）；② 顶部箭头/月份/星期表头用 `CAL_HDR`/`CAL_TITLE`；③ 网格线：8 条竖线（`drawFastVLine(gx, CAL_GRID_TOP, 6*CAL_CELL_H+1, CAL_GRID)`，`gx=CAL_X0+i*CAL_CELL_W`）+ 7 条横线（`drawFastHLine(CAL_X0, gy, (CAL_X1-CAL_X0)+1, CAL_GRID)`，`gy=CAL_GRID_TOP+r*CAL_CELL_H`）——画在数字**之前**；④ cell 数字底色参数从 `COLOR_BG` 改 `CAL_LIGHT_BG`，补班用 `CAL_WORK_L`、平日用 `CAL_NORM`；⑤ 底部信息条 `fillRoundRect(..., CAL_STRIP_L)` + 干支 `CAL_GOLD_L`/农历 `CAL_NORM`/今日 `CAL_TITLE`。
  - **`make_preview.py` `draw_calendar_scene`（lockstep）**：`d.rectangle([0,0,479,NAV_Y-1], fill=CAL_LIGHT_BG)`；同色表头/标题；8 竖 + 7 横 `d.line(..., fill=CAL_GRID)`（画在数字前）；cell 补班 `HOL_WORK_L`、平日 `CAL_NORM`；信息条 `CAL_STRIP_L`+`CAL_GOLD_L`/`CAL_NORM`/`CAL_TITLE`。`live_preview.py` 复用 `mp.draw_calendar_scene`，无需改。
  - **验证**：`pio run` 编译通过（RAM 21.4% / Flash 23.7%，与节前一致）。`python make_preview.py` 出 `preview_calendar.png`，逐像素核对：上区 1439/1740 为白、网格线（竖/横、顶 y54/底 y234）齐、月份标题蓝色、今天青框在 (112,159)、导航栏仍深灰 (32,32,32) → 仅日历页变浅。
- **日历三项交互（2026-09-21，点击选日 / 年月下拉 / 休班角标，两侧 lockstep）**：
  - **① 点击某天 → 底部信息条显示该天农历**（不再固定"今天"）：`main.cpp` 新增 `calSelY/calSelM/calSelD`（默认=系统今天）；`drawCalendar()` 底部信息条改用**选中日**算农历：若选中日=今天 → `今日农历X月X`，否则 → `M月D日 农历X月X`（干支年按选中日所在农历岁计算，春节前仍取前一年）。选中日若≠今天，cell 上画 **1px 蓝色边框**（`tft.drawRect(cx-14,cy-13,29,27, CAL_TITLE)`）。`make_preview.py` `draw_calendar_scene(year, month, today=None, selected=None, ...)` 同步。
  - **② 点击月份标题 → 年月选择弹层**（替代纯 ←/→ 翻月；←/→ 仍可用）：`main.cpp` 新增 `drawCalPicker()` + `handlePickerTap()` + 状态 `calPickerOpen/calPickY/calPickM`。布局（`PICK_*` 常量，`make_preview.py` 同步）：白底圆角面板 `(20,40)-(460,250)`，顶部 `选择年月` 标题 + **`< 年份 >` 步进**（2000~2099），中部 **6 列×2 行月份网格**（选中月青色高亮，其余浅蓝灰），底部 `取消`/`确定`（确定=蓝底白字）。**「确定」文字垂直居中（2026-09-21 修）**：按钮 `y=PICK_PB-34..PICK_PB-8`（中心 `PICK_PB-21`=229），原文字按硬件左上锚点 `PICK_PB-22`(=228) 落位，字形 16px 顶到 `228..243` 比按钮中心**低 7px**、顶到按钮下沿（preview 用 `PICK_PB-22+8` 同样偏低 7px）。修法=文字顶 y 改 **`PICK_PB-29`**（=中心-8，硬件左上锚点）、preview 用 **`PICK_PB-21`**（中间锚点中心 229），取消/确定两字同改。验证：preview 实测文字 y 中心=229（=按钮中心）、x 中心 339≈340。`handleCalendarTap()`：弹层打开→`handlePickerTap`（年份步进/选月/确定/取消/点面板外关闭）；关闭态 `ty<CAL_GRID_TOP` 时左/右 1/3=上一月/下一月、**中间 1/3（月份标题）=打开弹层**；网格区命中某天→`calSel*` 选中（用 `calFirstW`/`calDim` 反算 day）。`live_preview.py` `_clk_on_click()` 逐条镜像。
  - **③ 调休/假期改用右上角小 "休"/"班" 角标**（**去掉下划线**）：`main.cpp` cell 循环 `ht==HOL_OFF` 画红"休"、`ht==HOL_WORK` 画深蓝"班"，位置 `drawScaledChar(cx+19, cy-14, 0x4F11:0x73ED, 12, ...)`（cell 右上，**缩放 12px**，不越界、不压数字）；数字颜色保持（休=红 `CAL_OFF`、班=深蓝 `CAL_WORK_L`）。`make_preview.py` `aa_compose_scaled_glyph(img, cx+19, cy-14, 0x4F11:0x73ED, 12, ...)` 同步（读同一份 AA blob，逐像素一致），**删除原来的 2px 下划线** `d.rectangle(...)`。
  - **④ "今日" 按钮（一键回到今天，2026-09-21 加）**：翻月后没有返回今天的手段 → 在**底部信息条右端**加 "今日" 按钮。`main.cpp` `drawCalendar()`：`btW=textWidth("今日")+16; btX=CAL_X1-8-btW; btY=stripTop+5; btH=24`；`atToday=(calYear==ty && calMon==tmn)` 时**青色圆角高亮+黑字**（= 已在今天月），否则浅蓝灰底+蓝字（`CAL_TITLE`）。`handleCalendarTap()` 末尾新增命中区：`ty∈[stripTop, stripTop+34]` 且 `tx∈[btX-2, btX+btW+2]` → 取 `time(nullptr)` 设 `calYear/calMon/calSel*` 为今天 → `drawCalendar()`。`make_preview.py` `draw_calendar_scene` 画同款按钮（`at_today=(year==today.year and month==today.month)`，高亮 `CYAN`/字 `BLACK`）；`live_preview.py` `_clk_on_click()` 末尾镜像点击（点 → `cal_ym=今天月`、`cal_sel=今天`）。`BLACK=c565(0x0000)` 新常量（= 固件 `TFT_BLACK`）。
  - **验证**：`pio run` 编译通过（RAM 21.6% / Flash 23.8%）。`python make_preview.py` 无报错；今天月→按钮青色高亮、非今天月→浅蓝灰底蓝字，右端不压农历文字；`nl_zodiac_year` 生肖年逐项核对一致（2027-01-01→丙午马）。
- **时钟样式切换（机械翻页 ↔ 模拟指针）**：
  - `clockStyle`（0=翻页 `STYLE_FLAP` / 1=指针 `STYLE_ANALOG`），存 NVS 分区 `clockcfg`（key=`style`），重启保持。
  - 交互：**点击时钟页任意处**（底部导航栏、左上角 30×100 配网热区除外）切换样式。见 `handleTouch()` tap 分支。
  - 指针表盘 `drawAnalogClock()`：**白色表盘、无外圈描边** + 黑针 + 红秒针 + 数字 12..11；圆心 (240,108) R=73（顶 ~32/底 ~184，不压日期条）；60 分标+12 时标+数字；时/分/秒针（秒针平滑扫秒，每帧重绘）；配色常量 `AC_*`（`AC_FACE`=白、`AC_H/M`=黑、`AC_S/HUB`=红、`AC_NUM`=黑）。**指针样式不显示 "NTP OK"**（翻页样式才显示），`draw_analog_scene` 同步。
  - **数字用位图遮罩（非矢量字体）**：与翻页时钟**同款的 Bahnschrift 字体位图** `DialNum0..11`（`include/dialnum.h`）。`main.cpp` 用 `DialNums[12]` 指针数组 + `blitDialNum()`（按 8-bit 灰度遮罩合成到白色表盘后 `tft.pushImage()`，与 `drawCard` 的 `DigitMask` 完全同款）；数字中心落在半径 `R-20` 处（索引映射：12→0，10→10，11→11，1..9→1..9）。**盒子尺寸：单数字 16×22、双数字 24×22**（之前 24×28/38×28 太大 → 相邻小时位中心距只有 ~12–27px，双数字互相重叠/截断，是"两个9、没有11、10截断"的根因；缩小后互不重叠且贴着刻度环内侧）。**字体与翻页数字完全一致：Bahnschrift 权重 300 + 宽度 75**（= `gen_masks.py` 的设置，满足"指针数字用和数字一样字体"）。**抗锯齿不靠加粗，靠 300pt 超采样 + LANCZOS 降采样**（`gen_dialnum.py` 用 FONT_PX=300 渲染后再 resize），即使小盒边缘也平滑。`make_preview.py` 同步用 `load_dialnum()` 解析 `dialnum.h` + `get_dialnum()` 合成 + `img.paste()` 贴到 `(nx-w/2, ny-h/2)`，保证实机/预览数字逐像素一致。改字体/尺寸需重跑 `scripts/gen_dialnum.py` 重新生成 `dialnum.h`。
  - 时/分针为**棒棒(baton)指针**：中段最宽、向两端渐细的细长多边形 + 圆头帽（`main.cpp` `hand()` 用 `fillTriangle` 拼四块，`make_preview.py` `_draw_hand` 用 `polygon`），边缘平滑无锯齿（参考图二）。
  - 顶右侧 `drawModeHint()` 显示当前样式 + "点屏幕切换" 提示，**受 `DEBUG_UI` 开关控制**。
  - **`DEBUG_UI`**（`main.cpp` `#define DEBUG_UI` + `make_preview.py` 顶部 `DEBUG_UI`，两处必须一致）：1=调试期显示提示；**正式产品改成 0** 即可隐藏屏幕上所有调试提示（点屏幕切换等）。注意 `live_preview.py` 窗口下方的蓝色"时钟/日历…[空格]切换…"只是模拟窗口的辅助标签，**不会写进实机**，属产品安全，无需删除。
  - `make_preview.py` 同步（lockstep）：`draw_analog_clock`/`draw_analog_scene`/`draw_mode_hint`；运行时输出 `preview_after.png`(翻页,当前时间) + `preview_analog.png`(指针) + `preview_anim.gif`。
- **触摸无反应 bug 修复（2026-09-20）**：`handleTouch()` 中 `touchWasDown = isDown` 原本在**释放沿检查之前**就执行，导致 `if (!isDown && touchWasDown)` 永远为 false（`touchWasDown` 刚被设为 `isDown`，两者矛盾）→ 所有 tap 事件（翻页/指针切换、导航栏、收音机）全部无响应。修复：将 `touchWasDown = isDown` 移到函数末尾（所有旧值检查完毕之后），并移除释放沿块末尾多余的 `return;`。编译通过（RAM 18.8% / Flash 23.2%），待烧录验证。
- 冷色对比度：冰点以下温度 / 过湿(>80%) 由纯蓝 `0x001F` 改浅蓝 `0x6CF1`（深色背景可读）。见 `tempColor`/`humidColor`(main.cpp) 与 `temp_color`/`humid_color`(make_preview.py)。
- 修 2 个编译错误：`nongli.cpp` 加 `#include <cstdio>`；main.cpp 月相取模 `(int)(mp*8.0+0.5) % 8`（double 不能用 `%`）。
- 删除调试时间机制：移除整套 `USE_NTP=0` / `LOCAL_*` / `local_time()`，现固定走 WiFi+NTP。
- `FEST_URL` 表已改为 LittleFS `festival.json` loader + `festivalUrl()` 查询。

## 待办
### ⭐ 优先级 0：让 模拟器 ↔ 硬件 真正对齐
1. ~~**中文小字字体**~~ **✅ 已解决（2026-09-20）**：硬件 `font16` 升级为 8-bit 灰度 AA 字库（同源 `msyhbd.ttc` + 逐字 advance），与 preview 逐像素一致（见上"AA 中文字库"）。原"让 preview 改成 1-bit 去贴硬件"的方向**作废**——硬件现在能做到 AA 了。
2. ~~**温度/湿度**~~ **✅ 已解决（2026-09-20）**：`drawDateRow()` 行2 右侧已补 湿度/温度（`mockHumidity`/`mockTempC` 占位，`humidColor()`/`tempColor()` 配色，与 preview 一致）。接入天气 API 后把 mock 值换成真实值即可。
3. ~~**卡片底色**~~ **✅ 已解决（2026-09-21）**：硬件 `CARD_FACE` 由 `0x6318`(蓝紫) 改 **`0xFFFF`**(白)，与 preview `CARD_FACE_W=0xFFFF` 一致；数字遮罩 `out=bg*mask/255`（mask0=黑笔画/255=白底）→ 白底黑字，与 preview 逐像素一致。编译通过。
   - **✅ 翻页动画差异已解决（2026-09-21）**：硬件 `flipDigitAnim`（暗快门）已**整体替换**为与 preview `draw_flap_card` **逐帧一致的真·机械翻页**——见下"真·翻页动画 + 3D 滚动齿轮"。phase1 贴 `halfTop[new]`+`halfBot[old]`（露"上半新+下半旧"），旧半牌压成翻片带向铰链翻；phase2 同理补全新下半。两相时序 `FLIP_TOP_MS=400`/`FLIP_BOT_MS=340`（2026-09-24 再放慢，原 260/220，更早 150/130；preview `live_preview.py` 同名常量 + `make_preview.py` `P1,P2=400,340` 同步），缓动 `flapEase`(smoothstep) 与 preview `_ease` 一致；齿轮滚动量 `clicks*PITCH + ease(p)*PITCH`（一相=一格）。
4. **NTP OK**：翻页样式才显示（指针不显示）—— 两侧逻辑确认一致（已对齐，烧录验证）。
5. **"旦"残影**：`drawDateRow()` 已加整段清场 `fillRect(0, ROW1_TOP, 480, ...)`（已修，烧录验证）。
6. **节气配色**：两侧 `SOFT_C=0x94F6` 已一致（之前差异是残影+空温度位干扰观感）。

### 常规待办
- 烧录验证：`pio run -t upload`（固件，真·翻页动画）+ `pio run -t uploadfs`（推 `data/festival.json` 等），看串口 `[Fest] festival.json loaded`；连硬件看翻页动画（上半新/下半旧 + 翻片带）、两间隙 3D 齿轮滚动、不闪不卡。
- ~~填 `data/festival.json` 里 14 条真实节日音乐 URL~~ **✅ 已填（2026-09-21，复用 stations.json 同 CDN 已验证流；`清明` 键特殊）**。


## 2026-10-11 编译通道修复（决定性，HLS 主线恢复的前置）
- **根因**：pioarduino 6.2.0 自升级后，core deps 硬编码 tool-scons=4.11.1，但其分发的 scons 包是过滤版（缺 MSCommon/ninja/docbook），且每次编译 get_core_package_dir→install 都会重装并清理 packages\tool-scons → scons 引擎加载工具链报 `SCons.Tool.FortranCommon` 缺失 → 所有编译 40s 内 FAILED（此坑吞掉了 netTask 心跳与万年历修复的烧录）。
- **解法（已生效，编译 SUCCESS 534.73s）**：
  1. `D:\ESP32-S3\tools\tool-scons-full` = 用用户下载的 `C:\Users\fhaiy\Downloads\scons-local-4.11.1.tar.gz` 完整解压（含 FortranCommon；MSCommon/ninja 官方包本就排除，无需补）。
  2. 改 `D:\ESP32-S3\.platformio\penv\Lib\site-packages\platformio\package\manager\core.py` 的 `get_core_package_dir`：`if name=="tool-scons" and isdir(r"D:\ESP32-S3\tools\tool-scons-full"): return r"D:\ESP32-S3\tools\tool-scons-full"`（绕过其包管理）。
  3. `PLATFORMIO_DISABLE_UPDATE_NOTIFIER=1` 用户级环境变量已持久化（防再自升级）。
- **编译命令**：`$env:PLATFORMIO_CORE_DIR='D:\ESP32-S3\.platformio'; $env:PLATFORMIO_PENV_DIR='D:\ESP32-S3\.platformio\penv'; $env:PLATFORMIO_DISABLE_UPDATE_NOTIFIER='1'; Start-Process 'D:\ESP32-S3\.platformio\penv\Scripts\platformio.exe' -ArgumentList 'run','-e','s3v5','-t','upload','--upload-port','COM4' -WorkingDirectory 'D:\ESP32-S3' -RedirectStandardOutput logs\build_*.log -RedirectStandardError logs\build_*_err.log -WindowStyle Hidden`。
- **验证铁证**：`logs\scons_probe.txt`（scons.py 探针）：SConsFile=tool-scons-full\scons-local-4.11.1\SCons\__init__.py + FortranOK。`logs\build_probe3.log` = [SUCCESS] Took 534.73 秒，firmware.bin/firmware.factory.bin 已生成，**未烧录**（用户要求明天再烧）。
- **探针残留**：tool-scons-full\scons.py 顶部有写 scons_probe.txt 的探针（无害，可留）；core.py 的 patch 是持久修复，勿回退。

## 2026-10-11 待烧录/待验证清单（明天开始点）
1. **烧录**（含 main.cpp netTask 心跳 [NET-BEAT]2s/[NET-BLK]>500ms + calendar_ui.cpp 万年历命中区修复）→ 抓 boot log（应无 crash）。
2. **万年历验收**：切 2027 年 5 月点 30/31（log 应 `calendar sel -> 2027-5-30/31`）；10/27-31 也应可选。
3. **HLS 主线**：播放环球资讯（sid 269），抓 60-90s log，看 [NET-BEAT]/[NET-BLK]——定位 28s 静默阻塞点（此前 cap_step3b_ts.log 铁证：44.5-72.7s 全静默含 touch 停扫 = 网络读阻塞卡死任务链）。
4. **STEP4 双连接预取**（用户已认可方向）：播分片 N 并行预取 N+1，切换零断流。PSRAM 预算：heap free≈140KB、internalFree≈137KB、在线 PSRAM 余量~1.4MB；第二 NetworkClient 实现细节未定稿。NET-BLK 铁证对齐后实施。
5. **编译速度**：用户质疑全量编译。本次 534s 含全量 IDF 组件；明天验证改 main.cpp 后是否增量（audioI2S 库不应全量重编）。
6. **历史遗留**：指针时钟不显示（用户问能否放 PSRAM——秒针动态刷新需内部 RAM 或双缓冲策略）；右侧电台列表拖动整屏刷新 vs 左侧局部滚动不一致；SD 卡启用（GPIO6 与 PCM5102 共用，用户计划改 GPIO 后启用）。

## 2026-10-11 晚 更新：指针时钟已显示（用户实机确认）
- 烧录含指针时钟的固件后，用户确认**指针时钟已经显示**（主菜单可见、在走动）。历史遗留"指针时钟一直不显示"从待办中划掉。
- 指针时钟实现注意（用户此前提问）：秒针动态刷新，画面不能放 PSRAM 直接推屏（octal PSRAM SPI-DMA 突发读花屏，R3）；当前实现应走内部 RAM 整帧合成 + 单次 pushImage（R2），保持该路线即可。


## HLS 卡顿根治：死锁修复 + FIX11 双缓冲预取 + FIX12 AAC 风暴重同步 + FIX12b 采样率降噪（2026-10-12）
- **用户铁律**：HLS 电台（环球资讯 sk.cri.cn 905.m3u8 / 经济之声 ngcdn002.cnr.cn jjzs/index.m3u8）"播放几秒即无声/周期性卡顿"必须根治；非 HLS 正常；log 铁证不臆测；反对 patch by patch；已接受双连接预取方向。
- **死锁修复（BUILDV5-DEADLOCK-FIX，Audio.cpp L5577）**：InBuff 残留 17931B（差 69B 到 mb=18000）→ playAudioData isStream 循环 `bytesToDecode < getMaxBlockSize()` 永不满足 → sendBytes 永不被调 → 永不切 prefetch → 永久静默。门槛 18000→188（TS 包粒度）。修复后 90s 验证：prefetch↔InBuff 每 ~10s 循环、i2sBytes 持续。
- **FIX11（L5224 预取目标 + L4062 轮询 3s）**：修复后暴露"每切换周期 1s 静默+5.5s 慢速爬升"——根因=第 3 分片被误写进 prefetch（与第 2 分片竞争）→ 切回 InBuff 时空。修法：prefetch 已有货时第 3 分片写 InBuff；轮询 10s→3s。修复后 90s 环球资讯：0 静默/0 切换/全程满速。
- **FIX12（L6500 sendBytes 错误路径 + Audio.h m_aacErrStrike）**：分片边界 AAC 帧错位 → libfaad 逐帧喂垃圾试错（每失败跳 1 字节）→ 该秒吞吐降 31%。修法：连续 2 次解码错误 → 整段重扫 findNextSync（ns<0 返回 len 跳过整段 / ns>0 跳到 sync / 否则跳 1B）；成功复位计数；framesize==0 也纳入。修复后 90s 经济之声：0 静默但 60-71s 跨 2 分片 58 次错误风暴（54-60s 网络/解码完全正常铁证 → **cnr 流该时段内容畸形**，设备侧只能加速重同步无法消除）。
- **FIX12b（L7053 setSampleRate）**：采样率不变时跳过日志+IIR 重算（原库每帧都调：AAC 解码成功路径每帧打日志+IIR 重算+50 行 D 日志；风暴时 58 次刷屏拖慢恢复）。IIR 系数仅依赖采样率，未变时旧系数有效，跳过安全。**编译烧录 SUCCESS（128s）后 90s 经济之声验证：setSampleRate 58→0、AAC 错误 88→23、i2sBytes 全程 382976-387072 满速、0 静默、0 dip、0 timeout（904 行 vs 上轮 1504 行）**——FIX12+FIX12b 已把设备侧压到极限。
- **⚠️ 已知边界**：cnr 10s 大分片偶发整段畸形（60-71s 实证）是服务器内容问题，设备侧 FIX12/FIX12b 只能快速重同步；若听感仍明显需双连接预取+分片健康检查（跳过畸形分片）。环球资讯（3s 小分片）干净。



## HLS 卡顿根治：死锁修复 + FIX11 双缓冲预取 + FIX12 AAC 风暴重同步 + FIX12b 采样率降噪（2026-10-12）
- **用户铁律**：HLS 电台（环球资讯 sk.cri.cn 905.m3u8 / 经济之声 ngcdn002.cnr.cn jjzs/index.m3u8）"播放几秒即无声/周期性卡顿"必须根治；非 HLS 正常；log 铁证不臆测；反对 patch by patch；已接受双连接预取方向。
- **死锁修复（BUILDV5-DEADLOCK-FIX，Audio.cpp L5577）**：InBuff 残留 17931B（差 69B 到 mb=18000）→ playAudioData isStream 循环 `bytesToDecode < getMaxBlockSize()` 永不满足 → sendBytes 永不被调 → 永不切 prefetch → 永久静默。门槛 18000→188（TS 包粒度）。修复后 90s 验证：prefetch↔InBuff 每 ~10s 循环、i2sBytes 持续。
- **FIX11（L5224 预取目标 + L4062 轮询 3s）**：修复后暴露"每切换周期 1s 静默+5.5s 慢速爬升"——根因=第 3 分片被误写进 prefetch（与第 2 分片竞争）→ 切回 InBuff 时空。修法：prefetch 已有货时第 3 分片写 InBuff；轮询 10s→3s。修复后 90s 环球资讯：0 静默/0 切换/全程满速。
- **FIX12（L6500 sendBytes 错误路径 + Audio.h m_aacErrStrike）**：分片边界 AAC 帧错位 → libfaad 逐帧喂垃圾试错（每失败跳 1 字节）→ 该秒吞吐降 31%。修法：连续 2 次解码错误 → 整段重扫 findNextSync（ns<0 返回 len 跳过整段 / ns>0 跳到 sync / 否则跳 1B）；成功复位计数；framesize==0 也纳入。修复后 90s 经济之声：0 静默但 60-71s 跨 2 分片 58 次错误风暴（54-60s 网络/解码完全正常铁证 → **cnr 流该时段内容畸形**，设备侧只能加速重同步无法消除）。
- **FIX12b（L7053 setSampleRate）**：采样率不变时跳过日志+IIR 重算（原库每帧都调：AAC 解码成功路径每帧打日志+IIR 重算+50 行 D 日志；风暴时 58 次刷屏拖慢恢复）。IIR 系数仅依赖采样率，未变时旧系数有效，跳过安全。**编译烧录 SUCCESS（128s）后 90s 经济之声验证：setSampleRate 58→0、AAC 错误 88→23、i2sBytes 全程 382976-387072 满速、0 静默、0 dip、0 timeout（904 行 vs 上轮 1504 行）**——FIX12+FIX12b 已把设备侧压到极限。
- **⚠️ 已知边界**：cnr 10s 大分片偶发整段畸形（60-71s 实证）是服务器内容问题，设备侧 FIX12/FIX12b 只能快速重同步；若听感仍明显需双连接预取+分片健康检查（跳过畸形分片）。环球资讯（3s 小分片）干净。



## 播放中随机重启根治：TASK_WDT 复位 + netTask 豁免（2026-10-12）
- **用户报告**："播放的时候设备重启了"（任意电台，播放中偶发）。
- **根因（配置+代码铁证）**：`sdkconfig.s3v5` = `CONFIG_ESP_TASK_WDT_EN=y` + `CONFIG_ESP_TASK_WDT_TIMEOUT_S=5` + **`CONFIG_ESP_TASK_WDT_PANIC` 未设**（超时=直接复位芯片，非 panic）。netTask（Core 1，prio 2）泵 `audio.loop()` **从不喂狗**（`esp_task_wdt_reset` 无）——cnr 分片切换网络阻塞 7-9s（main.cpp L667/L804 已有注释实证）→ `audio.loop()` 单次调用 >5s → **TASK_WDT 复位 → 设备重启**。与"播放时重启"（netTask 只在 radioPlaying/musicPlaying 时泵 audio.loop）完全吻合。
- **为何 90s 验证没抓到**：验证时段网络正常（分片切换 <500ms，NET-BLK=0），阻塞是 cnr 服务器偶发；用户实听时恰好遇到阻塞时段。
- **修复（BUILDV5-TWDTRESET，main.cpp netTask 顶部 + include esp_task_wdt.h）**：`esp_task_wdt_delete(NULL)` 豁免 netTask 的 TASK_WDT——阻塞≠死锁，audio.loop 内部有网络超时会自行恢复，**宁可短暂无声/卡顿，绝不复位重启**。其他任务（main loop/touchScan）仍受 WDT 保护。
- **验证**：编译烧录 SUCCESS（91s）；120s 经济之声（`logs\cap_hls_1_120.log`，1259 行）：**0 静默 / 0 重启痕迹 / NET-BEAT 60 次 dt=0-188ms（audio.loop 正常）/ heap=137KB 稳定 / AAC err 22（快速消化）**——正常播放完全不受影响；阻塞时段"不重启"由机制保证（已豁免），实听验证待用户。
- **⚠️ 遗留**：豁免 WDT 只防重启，阻塞 7-9s 期间仍会短暂无声（该时段播放连接网络挂起）；根治需缩短 audio.loop 阻塞时长（HTTP read 超时收紧）或双连接预取（播放连接不阻塞）。



## 600s 长抓包分析（2026-10-12 晚，TWDTRESET+FIX12b 版）——明日待办：小修复列表轮询干扰
- **背景**：用户实听 5 台切换（环球资讯 sk.cri.cn→经典音乐 cnr dszs→经济之声 cnr jjzs→环球资讯→河北新闻 hebxw→环球资讯→河北新闻），报告"切换电台时有偶尔卡顿、破音、打嘟噜"。抓 600s（`logs\cap_hls_2_600.log`，6498 行）。
- **全局统计**：i2sBytes=0（完全静默）**0 次**；Writing timeout（I2S 欠载）**0 次**；AAC 解码错误 17 次（分散，FIX12 快速消化）；NET-BLK（>500ms 阻塞）1 次（3532ms）；吞吐 dip 21 次其中**深 dip 仅 2 次**。
- **切换点（6 处）全部浅 dip、1s 内恢复**：97.9s→cnr(229376)、168.6s→cri(290816)、295.2s→hebtv(342016 几乎无感)、351.8s→cri(**79872** 切换点最深)、463.5s→hebtv(198656)。**切换本身不是主要问题**。
- **深 dip 根因 ①（38.21s，i2sBytes=71680 dt=1397ms）**：InBuff+prefetch **双缓冲同时耗尽** → 37.93s 起重拉 m3u8 列表（readPlayListData 11 行）+ prepare_first_m3u8_url 移除旧分片 → 38.18s switch to prefetch → 38.22s 4ms 后 switch back（prefetch 也快空）→ 缓冲重爬。**疑似与 FIX11 的列表轮询 10s→3s 相关**：轮询触发新列表解析/旧分片移除，播放缓冲短暂被清。**明日小修复对象**（Audio.cpp 轮询逻辑，只动 L4062 附近 no_host_timer 分支，不动架构）。
- **深 dip 根因 ②（268.84s，i2sBytes=67584 dt=4138ms）**：**HTTP chunked 读阻塞 3532ms**（`NET-BLK audio.loop took 3532ms`，268.61s 打印；阻塞段 266-268.6s 无任何心跳/吞吐日志 → audio.loop 卡在 getChunkSize/chunked 读挂起）→ 缓冲耗尽（st=0, in=1107）→ 恢复后重爬（guard in 12589→16000）。**根因在网络（服务器/CDN chunked 读挂起），设备不可控**——TWDTRESET 已保证此场景表现为卡顿而非重启。
- **prefetch 机制验证（工作正常）**：pref 采样 590 次，最大 65535，非零 316 次；switch to prefetch 8 次 / switch back 8 次。**FIX11 双缓冲预取实际生效**（早前 90s 验证 0 静默 0 dip 与其一致）。
- **破音/打嘟噜机制**：双缓冲耗尽 → I2S 输出中断恢复（PCM5102A 输出停→续 = 爆音/嘟噜）。两个深 dip 即用户听到的 2 次明显卡顿（其余 598s 基本满速）。
- **双连接决策依据（未实施，待定）**：双连接可吸收 268s 型（网络阻塞，预取连接独立缓冲），但对 38s 型（列表轮询干扰）帮助有限；若阻塞是服务器侧，双连接两连接都可能堵。**当前证据不支撑立即上双连接**——600s 仅 2 个深 dip、0 静默、0 欠载。
- **明日计划**：① 小修复 38s 型（列表轮询干扰，Audio.cpp 轮询逻辑）→ 编译烧录 → 抓 600s+ 长 log 对比 dip 次数；② 若深 dip 只剩 268s 型（网络阻塞），与用户确认是否可接受或评估双连接；③ 待办：实听确认 TWDTRESET 后不再重启。
- **git 状态**：已本地提交 c385928（18 文件，Audio.cpp/Audio.h 强制入库）；push 失败（本机无法访问 github.com:443）；已生成 `patch_out/0001-...patch`（808KB）+ `patch_out/esp32-full.bundle`（7.3MB 完整历史）供跨机递交。


## BUILD 流水（2026-10-13 晚，V5 双连接前夜）
### FIX15 无声根治（用户已确认，2026-10-13）
- **现象**：每次重启/上电后**第一次**点环球资讯(HLS 48k AAC)无声，切其他电台也无声；**先点"全部"里 500 华语经典(MP3 44.1k 直连)就有声**，之后任何电台正常；点复位可恢复。
- **log 诊断链（全铁证）**：DECPEAK=1.3e9 → PEAKCHK 非零 → **GAIN 诊断 limL=1 limR=1 vc=1 iir=1 pre0=0** → POSTCHK=0 → peak=0。数据在 **IIR_filter 处被消零**。
- **根因**：settings.IIR_FILTER=true（默认开 3-band EQ）但项目未用 EQ；biquad coeffs/pre_gain（结构体默认 0.0）只在 IIR_calculateCoefficients（= setSampleRate 采样率变化分支）计算。首播环球资讯 48kHz == 初始采样率 → 跳过 → coeffs=0、pre_gain=0 → biquad 输出恒 0 → 静音。先点 MP3(44.1k≠48k) 触发计算 → 之后有声。与用户实测 100% 吻合。
- **修复（2 处）**：Audio.h IIR_FILTER=false；udiolib_structs.hpp pre_gain=1.0。testTone(串口'8'/'9' 1kHz 直写 I2S)证明物理链路 OK。

### FIX16 预取加深（2026-10-13，验证缓冲原理——缓解有效，根治需双连接）
- **改动**：BUILDV5_PREFETCH_TARGET=218000（≈2 分片≈6s）；三处预取触发点（包循环内/分片完成/chunkFinished）统一水位判断 !m_playingFromPrefetch && prefetch.bufferFilled() < BUILDV5_PREFETCH_TARGET。
- **验证（环球资讯 90s 抓包）**：
  - 网络较好（烧录后立即）：dip **17→7 次**（启动 2+真实 5），双缓冲 in=65535 pref=65535 双满——**缓冲加深有效，改善 70%**。
  - 网络拥塞（用户听感无改善时，22:51）：dip **21 次，精确 ~10s 周期**（每 10s 深度 dip 110K→恢复 280K 对），且 **pref=0 恒空**。
- **根因（单连接根本限制，决定性）**：预取触发依赖"分片下载完成"(yteCounter==fileSize)。拥塞时下载速率(27KB/s) < 播放消耗(110KB/s) → 分片永远完成不了 → 预取永不启动 → pref=0 → FIX16 目标形同虚设。**下载速率 ≤ 播放速率时缓冲无法积累——无论预取目标多深**。
- **结论（明天起点）**：缓冲加深=缓解（已验证）；**根治=真双连接**（连接 B 独立并行下载 N+1，不受连接 A 拥塞拖累）。

## 双连接改造小步规划（2026-10-14 起，每步编译烧录+90s 抓包验证）
- **STEP0 基线**：FIX16 固件当前状态留档（cap_hls_x_90.log 21dip/10s 周期）；确认编译通道（scons 修复勿回退）。
- **STEP1 预取连接基础设施**：新增 m_prefetchClient(WiFiClient) + prefetch 下载状态机最小框架（能开连接、能下载字节到 prefetch）。验证：编译过、不破坏现有播放。
- **STEP2 并行下载验证**：prefClient 播放中持续下载下一分片到 prefetch（独立于主连接）。验证：log 显示双连接同时在下载（速率翻倍证据）、**拥塞时段 pref 也能累积**（单连接做不到的）。
- **STEP3 零断流切换**：主连接分片 N 播完→无缝切 prefetch(N+1 就绪)→主连接下载目标切 N+2。验证：90s dip 目标=启动 2 + 0 真实。
- **STEP4 分片序列管理**：预取 URL 从 playlist 顺序取；MEDIA-SEQUENCE 跳变处理。验证：10 分钟级播放零断流、无 AAC 错误风暴。
- **STEP5 综合验证**：经济之声/经典音乐多台验证、30 分钟长抓包零 dip、预取深度/切换阈值参数调优、触摸/翻页时钟不受影响。
- **风险**：PSRAM 余量 1.48MB（prefetch 1.12MB 已有，第二个 client 缓冲用既有 prefetch 不新增）；两个连接共享缓冲需现有 lock 互斥；netTask 内并行下载注意看门狗（TWDTRESET 已豁免）。

## 当前文件状态（2026-10-13 22:52）
- lib\ESP32-audioI2S\src\Audio.cpp = FIX11/12/12b/FIX13/FIX15/FIX16 + 每秒 peak 诊断（BUILDV5: i2sBytes/s=… peak=…）+ DIAG4/DIAG7/DIAGX 少量遗留。FIX14 已回滚。
- lib\ESP32-audioI2S\src\Audio.h IIR_FILTER=false；udiolib_structs.hpp pre_gain=1.0。
- .pio\build\s3v5\firmware.bin @22:24:33（FIX16）。
- logs 关键：cap_hls_x_90.log(22:52, 21dip/10s 周期, pref=0)、cap_hls_2_90.log(22:30, 7dip, 双满)、uild_fix16.log(SUCCESS 117.2s)。
- 串口键：'1'经济之声 '2'环球资讯 '3'经典音乐 '6'怀集(MP3) '8'/'9' testTone 'x'纯抓包 'C'触摸校准；抓包 scripts\_cap_hls.py <key> <dur> COM4（第三参必须显式 COM4）。
- 编译模板（PowerShell 设 PLATFORMIO_CORE_DIR/PENV_DIR/DISABLE_UPDATE_NOTIFIER 后 Start-Process platformio.exe run -e s3v5 -t upload --upload-port COM4，RedirectStandardOutput logs\build_*.log -WindowStyle Hidden；SUCCESS 后核验 firmware.bin LastWriteTime）。
### 双连接可行性 PC 探测（2026-10-13 23:00，拥塞时段，决定性）
- **背景（用户关键质疑）**：说"拥塞下载 27KB/s"，但用户家网络是好的——是环球资讯服务器的问题吗？双连接能解决吗？不能靠猜，PC 实测。
- **探测方法**（scripts\_probe_dual.py，PC 直连同网络）：拉 m3u8 取最新 3 分片，单连接顺序下载 vs 2 worker 并行下载，测合并速率 vs 播放需求 35.5KB/s。
- **结果（铁证）**：
  - PC 单连接 = **926KB/s**（远超播放需求 35.5KB/s）→ **服务器根本不慢/不限速**
  - PC 双连接 = **1827KB/s = 1.97x** → 服务器 **per-connection 行为，双连接机制有效**
- **设备端真实情况（cap_hls_x_90.log）**：now playing 分片切换**精确每 3.00s 一次**（t=1.574/4.576/7.580/10.582/...）→ **设备下载能力 ≥ 播放**，分片准时到——**也不是设备 WiFi/网络瓶颈**。
- **卡顿根因（重新定性）**：不是网络/服务器/下载速率，是**预取调度失效**——断流时刻 in=24010 pref=0（缓冲只 0.7s）。机制：预取触发依赖"分片完成"(byteCounter==fileSize)，而列表轮询(FIX11 3s)每 3s 换新分片 → 预取目标永远在追 → **prefetch 恒空** → 每次分片切换撞缓冲临界 → 周期断流（10s = 3 分片）。
- **双连接结论**：**可以解决**（下载能力足够 + 双连接让 N+1 独立并行下载 → pref 恒有货 → 切换零断流），但**前提**：连接 B 在设备上能真持续下载（不被主连接/轮询相位拖住）。**STEP0 必须先加"预取连接每秒下载字节"诊断实测**，确认连接 B 独立下载速率，再实施双连接——避免又靠猜。
- 遗留疑问：InBuff bufferFilled 显示恒 65535（uint16 截断？还是真实上限？）——STEP0 一并确认（若 InBuff 容量被限 64KB，需检查 AudioBuffer 配置 DEFAULT_MAIN_BUFFER_SIZE=1000000 是否生效）。
