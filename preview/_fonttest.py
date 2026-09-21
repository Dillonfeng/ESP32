import os
import importlib.util
from PIL import Image, ImageDraw, ImageFont

spec = importlib.util.spec_from_file_location("gm", r"D:\ESP32-S3\scripts\gen_masks.py")
gm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gm)

cands = [
    (r"C:\Windows\Fonts\arialnb.ttf", "Arial Narrow Bold"),
    (r"C:\Windows\Fonts\consolab.ttf", "Consolas Bold"),
    (r"C:\Windows\Fonts\verdanab.ttf", "Verdana Bold"),
    (r"C:\Windows\Fonts\ariblk.ttf", "Arial Black"),
    (r"C:\Windows\Fonts\bahnschrift.ttf", "Bahnschrift (cur)"),
]
LABEL_FONT = ImageFont.truetype(r"C:\Windows\Fonts\consola.ttf", 20)

def render(ch, font, px):
    f = ImageFont.truetype(font, px)
    bb = ImageDraw.Draw(Image.new("L", (5, 5))).textbbox((0, 0), ch, font=f)
    gw, gh = bb[2] - bb[0], bb[3] - bb[1]
    pad = 24
    big = Image.new("L", (gw + pad * 2, gh + pad * 2), 255)
    ImageDraw.Draw(big).text((pad - bb[0], pad - bb[1]), ch, font=f, fill=0)
    # fill the 80px box as tightly as possible (height-limited for tall card)
    scale = min(80 / gw, 80 / gh)
    small = big.resize((max(1, round(gw * scale)), max(1, round(gh * scale))), Image.LANCZOS)
    c = Image.new("L", (46, 88), 255)
    c.paste(small, ((46 - small.width) // 2, (88 - small.height) // 2))
    return c.convert("RGB")

for path, name in cands:
    if not os.path.exists(path):
        continue
    img = Image.new("RGB", (580, 150), (18, 18, 22))
    d = ImageDraw.Draw(img)
    d.text((8, 4), name, font=LABEL_FONT, fill=(255, 255, 255))
    # six 56x120 cards in a row, white
    for i, ch in enumerate("123456"):
        x = 10 + i * 58
        d.rounded_rectangle([x, 30, x + 56, 150], radius=10, fill=(255, 255, 255))
        d.line([x + 2, 30 + 60, x + 54, 30 + 60], fill=(120, 120, 120))
        img.paste(render(ch, path, 200), (x + 5, 30 + 16))
    out = r"D:\ESP32-S3\preview\cardtest_%s.png" % name.replace(" ", "_").replace("(", "").replace(")", "")
    img.save(out)
    print("wrote", out)
print("all done")

