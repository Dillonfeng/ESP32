# ESP32-S3 网络收音机 / 翻页时钟 — 项目记忆

> 本文件是 Cline 的自动加载记忆。新窗口打开 `D:\ESP32-S3` 时会自动读到，无需复制上下文。

## 硬件
- ESP32-S3-N16R8 + ST7796S 480×320 (rot1) + XPT2046 触摸 + PCM5102A (I2S) + INMP441 麦克风
- 功能：机械翻页(split-flap)时钟 + 万年历(农历/干支/节气/节日) + 网络收音机(HLS) + 天气

## 构建 (PlatformIO 已装)
- `pio` = `C:\Users\fhaiy\AppData\Roaming\Python\Python311\Scripts\pio.exe`
- 编译：`pio run`（当前编译通过，RAM ~18% / Flash ~21%）
- 烧固件：`pio run -t upload`
- 烧 LittleFS：`pio run -t uploadfs`（推送 `data/festival.json` 等）
- 串口：`pio run -t monitor`，115200，关注 `[Fest] festival.json loaded`

## 关键文件
| 文件 | 作用 |
|---|---|
| `src/main.cpp` | 主程序 (UI/时钟/收音机/天气) — 已手动改写为 split-flap 局部刷新 |
| `include/digits.h` | 翻页时钟数字遮罩 (46×88 8-bit，`gen_masks.py` 生成，Bahnschrift light) |
| `include/dialnum.h` | 指针表盘数字遮罩 (12 个：12,1..9,10,11；单数字 24×28 / 双 38×28，Bahnschrift 权重 450，`gen_dialnum.py` 生成) |
| `src/nongli.cpp` + `include/nongli.h` | 农历/节气/节日计算 (17 位表 + 积日算法) |
| `preview/make_preview.py` | PC 端 1:1 预览 (ST7796 模拟)，与 main.cpp 布局/配色一致 |
| `preview/nongli_data.py` | 预览用农历数据 (与 nongli.cpp 同表同算法) |
| `data/festival.json` | LittleFS 节日配置 (14 条，URL 待填) |
| `platformio.ini` | 构建配置 (esp32-s3-n16r8, 16MB, littlefs) |

## 调试铁律（用户 2026-09-20 确认）
1. **没有连硬件的环境时才靠 live preview 调试**：模拟器存在的意义就是"不能连硬件时"模拟上硬件后的效果。
2. **先在模拟器调好 → 统一烧录硬件**。
3. **连上硬件后，以模拟器上的效果为目标**（模拟器是 target）。
4. **模拟器上的每个改动，硬件 firmware 必须做同样改动**。若硬件因限制做不到，**则模拟器也不那么改**（改了也没用，硬件做不到 preview 再好看也无意义）。
   - ~~中文小字无抗锯齿~~ **已解决（2026-09-20）**：`font16` 已升级为 **8-bit 灰度抗锯齿字库**（见下"AA 中文字库"）。硬件中文小字现在**有 AA**，且与 preview 同源（都用 `msyhbd.ttc`），逐像素一致（`preview/_aa_verify.py` 验证：平均偏差 0.33/通道）。
   - 大数字（翻页牌 / 表盘数字）走 8-bit 灰度遮罩 `pushImage`，**硬件有抗锯齿**，preview 用 `digits.h`/`dialnum.h` 位图对齐即可（已一致）。

## AA 中文字库（font16 已升级，2026-09-20）
- **为什么**：preview 用 Pillow 矢量抗锯齿，旧硬件是 1-bit 位图无 AA，导致"实机中文小字发虚/拥挤"。规则 4 要求硬件做不到的别只在 preview 美化 → 直接把硬件升到 AA。
- **生成器 `scripts/gen_aa_font.py`**：用 `C:\Windows\Fonts\msyhbd.ttc`（与 preview **同源**）把每个字渲染成 **16×16 8-bit 灰度 alpha** + **逐字 advance**（= msyhbd `getlength`：CJK=16, 数字=10, 'A'=12, '-'=7, '.'=5, '°'=7...）。字集 = 可打印 ASCII + Latin-1 补充(°/·) + 常用符号(★☆等) + GB2312 一级常用字 + 源码/数据里出现的全部非 ASCII 可打印字符（~4088 字）。
  - 重生成：`python scripts/gen_aa_font.py` → 原地写 `data/font_simhei_16.font`（文件名不变，内容变为新 AA 二进制格式，~1.0 MB）。
- **文件格式（二进制）**：`magic "XAAX"(4) | ver(1)=1 | pad(1)=0 | n(2,BE)` + 每字 `[unicode(2,BE) | advance(1) | 16×16 alpha(256)]`。`XFont` 按 magic 自动识别；**旧的 base-64 1-bit 文件（无 magic）自动走旧路径**，所以 `font24`（`font_simhei_24.font`，1-bit）不受影响、继续工作。
- **`XFont`（`src/xfont.cpp`）**：新增 `loadAA`（整文件读进 PSRAM）+ `findAA`（unicode 二分）+ `textWidthAA`（逐字 advance 求和）+ `drawStringFastAA`（逐字 `compositeGlyph`）+ `compositeGlyph`（`out=(bg*(255-m)+fg*m)/255` 逐 565 通道合成后 `tft.pushImage(x,y,16,16,buf)`，与 `blitDialNum` 同款）。`drawString/drawStringFast/textWidth` 开头都加了 `if(_isAA)` 分发。
- **`main.cpp` `drawDateRow`**：所有 `font16.drawStringFast(...)` 调用**必须传所画行底色**（行1 传 `ROW1_BG`，行2 传 `DATE_STRIP`）作为 AA 合成背景；星期单独用 `TFT_CYAN` 绘制（与 preview 一致）；行2 右侧补上了 **湿度 | 温度 | 月相名**（`mockHumidity`/`mockTempC` 占位，接入天气 API 后替换，颜色用 `humidColor()`/`tempColor()`）。
- **验证**：`python preview/_aa_verify.py` 用 Python 精确模拟硬件的"烘焙16×16灰度+逐字合成+advance"再与 preview `d.text` 逐像素比对（输出 `verify_hw/preview/diff.png`）。当前平均偏差 0.33/通道（仅 AA 边缘个像素差，肉眼不可见）。

## ⚠️ 务必遵守的约定
0. **上节"调试铁律"优先**：模拟器与硬件必须互为目标+镜像；硬件做不到的效果不要只在模拟器美化。
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
3. **卡片底色**：preview 白牌面 `CARD_FACE_W=0xFFFF`；硬件 `CARD_FACE=0x6318`（蓝紫）。→ 硬件改成白，与 preview 一致。（待办，独立于 AA 字体）
4. **NTP OK**：翻页样式才显示（指针不显示）—— 两侧逻辑确认一致（已对齐，烧录验证）。
5. **"旦"残影**：`drawDateRow()` 已加整段清场 `fillRect(0, ROW1_TOP, 480, ...)`（已修，烧录验证）。
6. **节气配色**：两侧 `SOFT_C=0x94F6` 已一致（之前差异是残影+空温度位干扰观感）。

### 常规待办
- 烧录验证：`pio run -t upload` + `pio run -t uploadfs`，看串口 `[Fest] festival.json loaded`。
- 填 `data/festival.json` 里 14 条真实节日音乐/音效 URL。
