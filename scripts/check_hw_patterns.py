#!/usr/bin/env python3
"""check_hw_patterns.py - ESP32 硬件"过程反模式"静态 lint

为什么需要它
------------
live preview (PIL) 是整帧原子渲染, 看不到硬件 SPI 推送过程:
  - 每个 tft.pushImage 是一次独立 SPI 事务, CS 切换间隙屏幕会瞬间"露白"
  - ST7796S 无 TE 同步, "先 fillCircle 擦白 → 再 drawLine 画针" 中间瞬间
    会被面板扫描到 → 局部闪
  - 八线 PSRAM 直接 pushImage 时 DMA 突发读取错乱 → 彩虹花屏
这些 preview 看不出, 只能靠烧录验证. 本 lint 在写代码阶段静态拦截,
避免再次踩坑.

5 条规则 (基于 2026-09-24 修的两处局部闪 + 历史花屏):
  R1 WARN  同函数内 pushImage/pushPSRAM >=2 次且无 tft.startWrite/endWrite 包裹
           → CS 切换间隙屏幕露白. 调用者若已包, 在函数顶部加 `// lint:skip-rule-1`
  R2 WARN  fillCircle(..., AC_FACE|0xFFFF) 整面擦白后同函数内还画
           drawLine/fillTriangle/pushImage/blitDialNum
           → 无 TE 时面板扫到中间瞬间白底, 局部闪. 改用 Sprite/内部 RAM 双缓冲
  R3 ERROR ps_malloc 分配的 PSRAM 缓冲直接传给 tft.pushImage (未经内部 RAM 中转)
           → 八线 PSRAM DMA 突发读取错乱, 彩虹花屏. 必须用 pushPSRAM 中转
  R4 WARN  TFT_eSprite.createSprite 周围未发现 _psram_enable=false /
           useInternalRam() / InternalRamSprite
           → octal PSRAM 默认走 PSRAM 缓冲, pushSprite 也可能花屏
  R5 ERROR initTFT()/setup() 函数体内无 tft.setSwapBytes(true)
           → BGR 字节序错乱, 文字彩色背景错位

用法:
  python scripts/check_hw_patterns.py            # 扫 <root>/src/main.cpp
  python scripts/check_hw_patterns.py path/to/main.cpp
  python scripts/check_hw_patterns.py --root d:/ESP32-S3

退出码: 0 = 无 ERROR (WARN 不阻塞); 1 = 有 ERROR
"""
import re
import sys
from pathlib import Path

# ---- 函数体提取 (花括号配对, 粗略忽略字符串/注释内花括号) ----
FUNC_DEF_RE = re.compile(
    r'^\s*(?:static\s+|inline\s+)*'
    r'(?:void|bool|int|unsigned\s+int|uint\d+_t|int\d+_t|size_t|float|double|char|const\s+char\*|uint16_t|uint8_t|uint32_t)\s+'
    r'(\w+)\s*\([^)]*\)\s*\{'
)

def iter_functions(src):
    """yield (func_name, body_text, def_line_no 1-based)."""
    lines = src.splitlines()
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]
        m = FUNC_DEF_RE.match(line)
        if not m:
            i += 1
            continue
        func_name = m.group(1)
        depth = line.count('{') - line.count('}')
        body_lines = [line]
        j = i + 1
        while j < n and depth > 0:
            l = lines[j]
            l_code = re.sub(r'//.*$', '', l)
            l_code = re.sub(r'"[^"]*"', '""', l_code)
            l_code = re.sub(r"'[^']*'", "''", l_code)
            depth += l_code.count('{') - l_code.count('}')
            body_lines.append(l)
            j += 1
        yield (func_name, '\n'.join(body_lines), i + 1)
        i = j


# ---- 规则正则 ----
PUSH_RE = re.compile(r'(?:tft\.pushImage|pushPSRAM)\s*\(')
STARTWRITE_RE = re.compile(r'tft\.startWrite\s*\(')
ENDWRITE_RE = re.compile(r'tft\.endWrite\s*\(')

PSRAM_ALLOC_RE = re.compile(
    r'(?:uint16_t|uint8_t|uint32_t|char|void)\s*\*\s*(\w+)\s*=\s*\([^)]*\)\s*'
    r'(?:ps_malloc|heap_caps_malloc_\w*|psram_malloc|ESP.*psram|ESP_PSRAM)\s*\('
)
PUSHIMAGE_ARG_RE = re.compile(r'tft\.pushImage\s*\(([^;]*?)\)\s*;')

FILLCIRCLE_WHITE_RE = re.compile(r'tft\.fillCircle\s*\([^)]*\b(AC_FACE|0xFFFF)\b')
DRAWGEOM_AFTER_RE = re.compile(r'tft\.(?:drawLine|fillTriangle|pushImage)|blitDialNum')

CREATESPRITE_RE = re.compile(r'\.createSprite\s*\(')
PSRAM_DISABLE_RE = re.compile(
    r'_psram_enable\s*=\s*false|useInternalRam\s*\(\)|InternalRamSprite'
)

SETSWAP_RE = re.compile(r'tft\.setSwapBytes\s*\(\s*true\s*\)')
SKIP_RULE_RE = re.compile(r'lint:\s*skip-rule-([12345])')


def _has_skip(body, n):
    """函数体是否标注 // lint:skip-rule-N."""
    for m in SKIP_RULE_RE.finditer(body):
        if m.group(1) == str(n):
            return True
    return False


def check_rule1(func_name, body, start_line, out):
    """R1: 同函数内 pushImage/pushPSRAM >=2 且无 startWrite/endWrite 包裹."""
    if _has_skip(body, 1):
        return
    push_hits = [(i, l) for i, l in enumerate(body.splitlines()) if PUSH_RE.search(l)]
    if len(push_hits) < 2:
        return
    has_sw = bool(STARTWRITE_RE.search(body))
    has_ew = bool(ENDWRITE_RE.search(body))
    if has_sw and has_ew:
        return
    line_nos = ', '.join(str(start_line + idx) for idx, _ in push_hits[:5])
    out.append(('R1', 'WARN', start_line, func_name,
        f'{len(push_hits)}x pushImage/pushPSRAM 在同一函数内, 未见 tft.startWrite()/endWrite() '
        f'包裹 (CS 切换间隙屏幕露白). 若调用者已包, 请在函数顶部加 // lint:skip-rule-1. '
        f'命中行: {line_nos}'))


def check_rule2(func_name, body, start_line, out):
    """R2: fillCircle(AC_FACE/0xFFFF) 整面擦白后同函数还画 drawLine/fillTriangle/pushImage/blitDialNum."""
    if _has_skip(body, 2):
        return
    lines = body.splitlines()
    white_at = None
    for i, l in enumerate(lines):
        if FILLCIRCLE_WHITE_RE.search(l):
            white_at = i
            break
    if white_at is None:
        return
    for i in range(white_at + 1, len(lines)):
        if DRAWGEOM_AFTER_RE.search(lines[i]):
            out.append(('R2', 'WARN', start_line + white_at, func_name,
                f'fillCircle(AC_FACE/0xFFFF) 整面擦白后, 同函数 line {start_line + i} '
                f'还画 drawLine/fillTriangle/pushImage (无 TE 同步, 面板扫到中间瞬间白底 → 局部闪). '
                f'建议: 整帧合成到 TFT_eSprite / 内部 RAM 缓冲后单次 pushImage.'))
            break


def check_rule3(src, main_path, out):
    """R3: ps_malloc 分配的 PSRAM 缓冲直接传给 tft.pushImage."""
    psram_vars = set()
    for m in PSRAM_ALLOC_RE.finditer(src):
        psram_vars.add(m.group(1))
    if not psram_vars:
        return
    for m in PUSHIMAGE_ARG_RE.finditer(src):
        args = m.group(1)
        for v in psram_vars:
            if re.search(r'\b' + re.escape(v) + r'\b', args):
                line_no = src[:m.start()].count('\n') + 1
                out.append(('R3', 'ERROR', line_no, v,
                    f'PSRAM 缓冲 "{v}" 直接传 tft.pushImage (八线 PSRAM DMA 突发读取错乱 → 彩虹花屏). '
                    f'必须用 pushPSRAM(x,y,w,h,src) 中转内部 RAM (memcpy 后再 pushImage).'))
                break


def check_rule4(src, out):
    """R4: createSprite 周围未显式禁用 PSRAM."""
    for m in CREATESPRITE_RE.finditer(src):
        line_no = src[:m.start()].count('\n') + 1
        ctx = src[max(0, m.start() - 800): m.end() + 800]
        if PSRAM_DISABLE_RE.search(ctx):
            continue
        # 看所在行的 sprite 变量声明类型
        line_start = src.rfind('\n', 0, m.start()) + 1
        decl_line = src[line_start: m.end()].split('\n')[0]
        if 'InternalRamSprite' in decl_line or 'useInternalRam' in decl_line:
            continue
        out.append(('R4', 'WARN', line_no, '',
            f'TFT_eSprite createSprite 周围 ±800 字符未发现 _psram_enable=false / '
            f'useInternalRam() / InternalRamSprite. octal PSRAM 默认走 PSRAM 缓冲, '
            f'pushSprite 也可能花屏.'))


def check_rule5(src, out):
    """R5: 全局未出现 tft.setSwapBytes(true) (initTFT/setup 间接调用也算, 全局扫描更实用).

    设计原因: setup() 调 initTFT(), 真正的 setSwapBytes 在 initTFT 里, setup 函数体本身没字样.
    粗扫全局更可靠.
    """
    if SETSWAP_RE.search(src):
        return
    # 找 initTFT 或 setup 函数所在行 (用于报告定位)
    fm = re.search(r'\b(?:void\s+)?(initTFT|setup)\s*\([^)]*\)\s*\{', src)
    line_no = src[: fm.start()].count('\n') + 1 if fm else 1
    func_name = fm.group(1) if fm else 'global'
    out.append(('R5', 'ERROR', line_no, func_name,
        f'整个 main.cpp 未发现 tft.setSwapBytes(true) (BGR 字节序错乱, 文字/底色彩色错位). '
        f'应在 initTFT() 内调用 tft.setSwapBytes(true).'))


def run(main_path):
    main_path = Path(main_path).resolve()
    if not main_path.exists():
        print(f'[check_hw_patterns] 找不到 {main_path}', file=sys.stderr)
        return 1
    src = main_path.read_text(encoding='utf-8', errors='replace')
    out = []
    for func_name, body, start_line in iter_functions(src):
        check_rule1(func_name, body, start_line, out)
        check_rule2(func_name, body, start_line, out)
    check_rule3(src, main_path, out)
    check_rule4(src, out)
    check_rule5(src, out)

    out.sort(key=lambda w: (w[0], w[2] if isinstance(w[2], int) else 0))
    errs = [w for w in out if w[1] == 'ERROR']
    warns = [w for w in out if w[1] == 'WARN']

    if not out:
        print(f'[check_hw_patterns] OK: {main_path.name} 通过 R1-R5 全部 5 条规则, 无警告.')
        return 0

    print(f'[check_hw_patterns] {main_path.name}: {len(out)} 条 ({len(errs)} ERROR / {len(warns)} WARN):')
    print()
    for rid, lvl, line, ctx, msg in out:
        print(f'  {rid} [{lvl}] {main_path.name}:{line}  ({ctx})')
        for m in msg.split('. '):
            print(f'      {m.rstrip(".")}.')
        print()
    print(f'[check_hw_patterns] {len(errs)} ERROR / {len(warns)} WARN  '
          f'(ERROR 阻塞, WARN 仅提示; 调用者已包 startWrite 请在函数顶部加 // lint:skip-rule-N)')
    return 1 if errs else 0


def _default_main_cpp():
    here = Path(__file__).resolve().parent
    return (here.parent / 'src' / 'main.cpp')

def main():
    args = sys.argv[1:]
    target = None
    for a in args:
        if a.startswith('--root='):
            root = Path(a[len('--root='):])
            target = root / 'src' / 'main.cpp'
        elif a.startswith('-'):
            pass
        else:
            target = a
    if target is None:
        target = _default_main_cpp()
    return run(target)


if __name__ == '__main__':
    sys.exit(main())
