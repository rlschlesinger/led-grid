#!/usr/bin/env python3
"""Print-ready setup sticker: a QR code that joins the grid's setup network.

    python3 tools/setup-qr.py      (needs: pip install "qrcode[pil]")

Scanning it joins "LED-Grid"; the phone then opens the setup page by itself
(the grid answers the phone's captive-portal check), where you pick your home
Wi-Fi. Writes print/setup-qr.png, 2 in square at 300 dpi.
"""
import os, re
import qrcode
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = open(os.path.join(ROOT, "src", "main.cpp")).read()
ssid = re.search(r'AP_SSID = "([^"]+)"', src).group(1)
password = re.search(r'AP_PASS = "([^"]+)"', src).group(1)

qr = qrcode.QRCode(error_correction=qrcode.constants.ERROR_CORRECT_M, box_size=1, border=2)
qr.add_data(f"WIFI:T:WPA;S:{ssid};P:{password};;")
qr.make(fit=True)
code = qr.make_image(fill_color="black", back_color="white").convert("RGB")

DPI, SIZE = 300, 600                       # 2 in square
card = Image.new("RGB", (SIZE, SIZE), "white")
side = 440
card.paste(code.resize((side, side), Image.NEAREST), ((SIZE - side) // 2, 24))
draw = ImageDraw.Draw(card)
try:
    font = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 34)
    small = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 24)
except OSError:
    font = small = ImageFont.load_default()
for text, f, y in [("Scan to set up Wi-Fi", font, 482), (f'or join "{ssid}"  ·  {password}', small, 528)]:
    w = draw.textlength(text, font=f)
    draw.text(((SIZE - w) / 2, y), text, fill="black", font=f)
out = os.path.join(ROOT, "print", "setup-qr.png")
card.save(out, dpi=(DPI, DPI))
print(out)
