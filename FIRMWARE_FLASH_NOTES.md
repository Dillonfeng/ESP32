# ESP32-S3 烧录流程 (已验证可用)

## 环境
- **Windows** + PowerShell
- **PlatformIO**: `pio` 在 PATH 中 (`C:\Users\fhaiy\AppData\Roaming\Python\Python311\Scripts\pio.exe`)
- **COM 端口**: COM5 (ESP32-S3 USB)
- **Python**: `C:\Python314\python.exe` (PATH 中的 python)

## 烧录命令 (在 PowerShell 中执行)

```powershell
# 编译 + 烧录 (一步到位)
pio run -t upload -e esp32-s3-devkitc-1

# 仅编译 (不烧录)
pio run -e esp32-s3-devkitc-1

# 仅烧录 (已编译)
pio run -t upload -e esp32-s3-devkitc-1
```

## 关键注意事项
1. **不要用 `&&` 连接命令** — PowerShell 不支持，用 `;` 分隔
2. **不要引用 `.pio\penv\Scripts\python.exe`** — 该路径不存在，直接用 `pio` 命令
3. **不要运行 `scripts/rebuild_main.py`** — 它会覆盖手动编辑
4. **文件编码**: `src/main.cpp` 是 **GBK** 编码，PowerShell 用 `Get-Content -Encoding UTF8` 可能乱码但能看结构
5. **PowerShell 中 `$` 变量会被 shell 吞掉** — 避免在 -Command 参数里用 `$var`，改用数组索引 `[0..n]` 或 `Select-Object`
6. **read_files 工具有缓存问题** — 同一文件多次读取可能返回 "outdated"，用 PowerShell `Get-Content` 替代
7. **烧录成功标志**: 输出包含 `Hash of data verified.` + `[SUCCESS]`

## 成功示例输出
```
Writing at 0x0014d133 (100 %)
Wrote 1307952 bytes (874475 compressed) at 0x00010000 in 12.1 seconds...
Hash of data verified.
Leaving...
Hard resetting via RTS pin...
========================= [SUCCESS] Took 39.15 seconds =========================
```

## 硬件信息
- 板子: ESP32-S3-N16R8 (16MB Flash, 8MB PSRAM)
- 屏幕: ST7796S 480x320 (SPI, rotation=1 横屏)
- 触摸: XPT2046 (共享SPI, T_CS=GPIO1, T_IRQ=GPIO38)
- 背光: GPIO21
- 当前固件大小: ~1.3MB Flash (20%)
