#!/usr/bin/env python3
"""
gen_masks.py - Generate antialiased 8-bit grayscale digit masks for the
ESP32-S3 split-flap clock (replaces the RGB565 approach in gen_digits.py).

Why a mask (not a fixed-color sprite)?
  The clock draws each digit card on a background that CHANGES during the
  flip animation: the top half is shaded (CARD_SHADE) in phase 1, plain
  card face (CARD_FACE) in phase 2. A fixed-background RGB565 sprite would
  paint over that shade and break the flip. So we store only the digit's
  luminance and composite it at draw time against whatever the current
  background is:

      out = bg * mask / 255        (mask 255 -> bg, mask 0 -> black digit)

  This gives true antialiasing that is correct in both flip phases and works
  with any card face / shade color.

Each digit '0'..'9' is rendered with a bold geometric font (Bahnschrift if
present, else Arial/Segoe UI) using 8x supersampling via LANCZOS downscale,
so the edges are smooth instead of FreeFont's jagged integer scaling.

Output:
  d:/ESP32-S3/include/digits.h
    - DigitMask0..9 : uint8_t bitmap (DIGIT_W x DIGIT_H, top-to-bottom rows)
                      0 = digit stroke, 255 = transparent background
    - DIGIT_W / DIGIT_H / DIGIT_BYTES defines

The mask is stored in natural (upright) orientation. The display applies the
same setRotation() to the sprite and to the rest of the UI, so digits stay
upright and consistent with the text.

Run:  python scripts/gen_masks.py
"""
from PIL import Image, ImageDraw, ImageFont
import os

DIGIT_W = 46
DIGIT_H = 88
FONT_PX = 240                 # large source point size for high-quality AA
MARGIN = 2                    # inner margin inside the digit box (小 => 数字更顶满牌面)
# Thin/elegant DIN look (Bahnschrift) at LIGHT weight -- NOT bold. No stroke
# added: the glyph's own (slender) strokes are what we want, filling the card.
BAHNSCHRIFT = r"C:\Windows\Fonts\bahnschrift.ttf"
BAHNS_WEIGHT = 300            # variable font Weight axis 300..700; 300 = light/thin
BAHNS_WIDTH  = 75             # variable font Width axis 75..100; <100 收窄字形,
                              # 让宽数字(0/4/8)在 46px 卡内也能顶满高度 => 0..9 顶高一致
# Fallbacks (regular weight only, never bold): used if Bahnschrift is absent.
FONT_CANDIDATES = [
    r"C:\Windows\Fonts\candaral.ttf",   # Candara Light
    r"C:\Windows\Fonts\segoeuil.ttf",   # Segoe UI Light
    r"C:\Windows\Fonts\consola.ttf",    # Consolas regular
    r"C:\Windows\Fonts\arial.ttf",
]
OUT = r"d:/ESP32-S3/include/digits.h"


def pick_font():
    """Return (path, is_bahns). Bahnschrift (variable DIN) is preferred."""
    if os.path.exists(BAHNSCHRIFT):
        return BAHNSCHRIFT, True
    for p in FONT_CANDIDATES:
        if os.path.exists(p):
            return p, False
    for n in ["Bahnschrift", "Candara", "Segoe UI", "Arial"]:
        try:
            ImageFont.truetype(n, 20)
            return (BAHNSCHRIFT, True) if n == "Bahnschrift" else (n, False)
        except Exception:
            pass
    raise RuntimeError("No usable font found")


def _common_scale(font):
    """Return ONE scale applied to every digit so 0..9 end up the SAME height.

    Root cause of the old bug: the scale was computed *per digit* as
    min(max_w/gw, max_h/gh). "1" was limited by its height (tall + narrow)
    while 0/4/8 were limited by their width (wide + shorter) -> "1" looked
    taller than the rest. Using a single scale for all digits (all share the
    same cap-height) makes every glyph the same pixel height; the scale is
    chosen so the *widest* digit just fits the card width.
    """
    d = ImageDraw.Draw(Image.new("L", (10, 10), 0))
    max_w = DIGIT_W - MARGIN * 2
    max_h = DIGIT_H - MARGIN * 2
    scale = max_h / d.textbbox((0, 0), "0", font=font)[3]  # start generous
    for ch in "0123456789":
        b = d.textbbox((0, 0), ch, font=font)
        scale = min(scale, max_w / (b[2] - b[0]), max_h / (b[3] - b[1]))
    return scale


def render_digit_mask(ch, scale=None):
    """Return a PIL 'L' image DIGIT_W x DIGIT_H: 0 = stroke, 255 = bg.
    scale: the shared scale (see _common_scale). If None, compute it here."""
    f, is_bahns = pick_font()
    font = ImageFont.truetype(f, FONT_PX)
    if scale is None:
        scale = _common_scale(font)
    if is_bahns:
        # Thin (weight 300), normal width -- the elegant DIN look.
        try:
            font.set_variation_by_axes([BAHNS_WEIGHT, BAHNS_WIDTH])
        except Exception:
            pass
    stroke_w = 0

    # Measure the glyph's exact bounding box (含描边).
    dummy = Image.new("L", (10, 10), 0)
    d = ImageDraw.Draw(dummy)
    bbox = d.textbbox((0, 0), ch, font=font, stroke_width=stroke_w)
    gw = bbox[2] - bbox[0]
    gh = bbox[3] - bbox[1]

    # Render the glyph (black on white) large, with a little padding.
    pad = 24
    big = Image.new("L", (gw + pad * 2, gh + pad * 2), 255)
    bd = ImageDraw.Draw(big)
    bd.text((pad - bbox[0], pad - bbox[1]), ch, font=font, fill=0,
            stroke_width=stroke_w, stroke_fill=0)

    # Downscale with LANCZOS (keeps smooth antialiased edges).
    # 所有数字共用同一个 scale (见 _common_scale): 因为 0..9 的 cap-height
    # 相同, 同一 scale 缩出后像素高度必然一致 => "1" 与其它数字同高,
    # 不再出现窄数字显得更矮/更长的视觉差。宽窄由各字形自身比例决定。
    nw = max(1, int(round(gw * scale)))
    nh = max(1, int(round(gh * scale)))
    small = big.resize((nw, nh), Image.LANCZOS)

    # Center the glyph in the final DIGIT_W x DIGIT_H canvas (255 = bg).
    canvas = Image.new("L", (DIGIT_W, DIGIT_H), 255)
    px = (DIGIT_W - nw) // 2
    py = (DIGIT_H - nh) // 2
    canvas.paste(small, (px, py))
    return canvas


def main():
    f, is_bahns = pick_font()
    print("Using font:", f, "(Bahnschrift thin w%d)" % BAHNS_WEIGHT if is_bahns else "(regular weight)")

    lines = []
    lines.append("// AUTO-GENERATED by scripts/gen_masks.py -- do not edit by hand")
    lines.append("// Antialiased 8-bit digit masks for the split-flap clock.")
    lines.append("// Each DigitMaskN is DIGIT_W x DIGIT_H (uint8_t): 0 = stroke, 255 = bg.")
    lines.append("// Composite against the current card background:")
    lines.append("//   out = bg * mask / 255   (mask 255 -> bg, mask 0 -> black digit)")
    lines.append("#ifndef DIGITS_H")
    lines.append("#define DIGITS_H")
    lines.append("")
    lines.append("#include <Arduino.h>")
    lines.append("")
    lines.append("#define DIGIT_W %d" % DIGIT_W)
    lines.append("#define DIGIT_H %d" % DIGIT_H)
    lines.append("#define DIGIT_BYTES (DIGIT_W * DIGIT_H)")
    lines.append("")
    # 统一缩放: 所有数字用同一个 scale => 0..9 像素高度一致 (见 _common_scale)。
    _f, _bahns = pick_font()
    _font = ImageFont.truetype(_f, FONT_PX)
    if _bahns:
        try:
            _font.set_variation_by_axes([BAHNS_WEIGHT, BAHNS_WIDTH])
        except Exception:
            pass
    _scale = _common_scale(_font)
    print("Common scale: %.4f (all digits -> same height, width=%dpx)" %
          (_scale, DIGIT_W))
    for i in range(10):
        img = render_digit_mask(chr(ord('0') + i), scale=_scale)
        lines.append("const uint8_t DigitMask%d[] PROGMEM = {" % i)
        row = []
        for y in range(img.height):
            for x in range(img.width):
                row.append(str(img.getpixel((x, y))))
                if len(row) == 16:
                    lines.append("  " + ", ".join(row) + ",")
                    row = []
        if row:
            lines.append("  " + ", ".join(row))
        lines.append("};")
        lines.append("")
    lines.append("const uint8_t *const DigitMasks[10] PROGMEM = {")
    lines.append("  DigitMask0, DigitMask1, DigitMask2, DigitMask3, DigitMask4,")
    lines.append("  DigitMask5, DigitMask6, DigitMask7, DigitMask8, DigitMask9");
    lines.append("};")
    lines.append("")
    lines.append("#endif // DIGITS_H")
    lines.append("")

    with open(OUT, "w", newline="\r\n") as fh:
        fh.write("\n".join(lines))
    print("Wrote", OUT)
    print("Mask size: %dx%d = %d bytes each, 10 masks" %
          (DIGIT_W, DIGIT_H, DIGIT_W * DIGIT_H))


if __name__ == "__main__":
    main()
