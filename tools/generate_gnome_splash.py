#!/usr/bin/env python3
"""
Garden gnome splash screen for T5 E-Reader.
Style: classic garden gnome - tall red pointed hat, white beard, rosy cheeks,
       cream/grey jacket, green trousers, dark brown boots.
Output: 540x960 1-bit BMP + C header for e-ink display.
"""
from PIL import Image, ImageDraw, ImageFont
import math, os

W, H = 540, 960
img = Image.new("1", (W, H), 1)   # 1=white background
draw = ImageDraw.Draw(img)

BLACK = 0
WHITE = 1

def filled_ellipse(cx, cy, rx, ry, fill=BLACK, outline=None, width=1):
    draw.ellipse([cx-rx, cy-ry, cx+rx, cy+ry], fill=fill, outline=outline, width=width)

def circle(cx, cy, r, fill=BLACK, outline=None, width=1):
    filled_ellipse(cx, cy, r, r, fill=fill, outline=outline, width=width)

def poly(pts, fill=BLACK, outline=None, width=1):
    draw.polygon(pts, fill=fill, outline=outline, width=width)

def rect(x, y, w, h, fill=BLACK, outline=None, width=1):
    draw.rectangle([x, y, x+w, y+h], fill=fill, outline=outline, width=width)

CX = W // 2

# ─── Fonts ───────────────────────────────────────────────────────────
try:
    font_title = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 38)
    font_sub   = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 20)
    font_ver   = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 17)
except Exception:
    font_title = font_sub = font_ver = ImageFont.load_default()

def centered_text(text, y, font, fill=BLACK):
    bb = draw.textbbox((0, 0), text, font=font)
    w = bb[2] - bb[0]
    draw.text((CX - w // 2, y), text, font=font, fill=fill)

# ─── Outer border ───────────────────────────────────────────────────
draw.rectangle([4, 4, W-5, H-5], outline=BLACK, width=3)
draw.rectangle([10, 10, W-11, H-11], outline=BLACK, width=1)

# ═════════════════════════════════════════════
# Title area (top)
# ═════════════════════════════════════════════
centered_text("T5 Reader", 28, font_title)
# Decorative double rule
draw.line([(30, 80), (W-30, 80)], fill=BLACK, width=3)
draw.line([(30, 85), (W-30, 85)], fill=BLACK, width=1)

# ═════════════════════════════════════════════
# GNOME (centered vertically in main canvas)
# Reference: tall pointed hat, round head, white beard, green pants, brown boots
# Gnome top: y=105, boots bottom: y=820
# ═════════════════════════════════════════════

# Vertical layout anchors
HAT_TIP_Y  = 105
HAT_BASE_Y = 370   # where brim sits on head
HEAD_CY    = 420
HEAD_RX    = 82
HEAD_RY    = 75
BODY_TOP   = HEAD_CY + HEAD_RY - 15
BODY_BOT   = 700
BELT_Y     = 570
LEG_BOT    = 780
BOOT_BOT   = 820

# ─── Hat (tall pointed red — rendered as dense diagonal hatching for "dark") ─
hat_pts = [
    (CX, HAT_TIP_Y),          # tip
    (CX - 88, HAT_BASE_Y),    # brim left
    (CX + 88, HAT_BASE_Y),    # brim right
]
poly(hat_pts, fill=BLACK)

# Hat highlight (left side lighter stripe — white triangular slash)
highlight_pts = [
    (CX - 12, HAT_TIP_Y + 30),
    (CX - 55, HAT_BASE_Y - 20),
    (CX - 36, HAT_BASE_Y - 20),
    (CX + 2,  HAT_TIP_Y + 30),
]
poly(highlight_pts, fill=WHITE)

# Brim band (slightly wider, white strip at base of hat)
draw.ellipse([CX - 98, HAT_BASE_Y - 12, CX + 98, HAT_BASE_Y + 12],
             fill=WHITE, outline=BLACK, width=2)
# Re-draw bottom part of hat to overlap brim area
# Hat shadow/depth on right side
poly([
    (CX + 10, HAT_TIP_Y + 28),
    (CX + 88, HAT_BASE_Y),
    (CX + 58, HAT_BASE_Y),
    (CX + 8,  HAT_TIP_Y + 28),
], fill=WHITE)  # lighter right face for 3D effect

# Brim (solidly drawn oval band)
draw.ellipse([CX - 100, HAT_BASE_Y - 14, CX + 100, HAT_BASE_Y + 14],
             outline=BLACK, width=3)

# ─── Ears ─────────────────────────────────────────────────────────────
for ex, ey in [(CX - HEAD_RX + 6, HEAD_CY + 5), (CX + HEAD_RX - 6, HEAD_CY + 5)]:
    circle(ex, ey, 22, fill=WHITE, outline=BLACK, width=3)
    circle(ex, ey + 4, 10, fill=WHITE, outline=BLACK, width=2)

# ─── Head ─────────────────────────────────────────────────────────────
draw.ellipse([CX - HEAD_RX, HEAD_CY - HEAD_RY, CX + HEAD_RX, HEAD_CY + HEAD_RY],
             fill=WHITE, outline=BLACK, width=4)

# Rosy cheeks (stippled circles)
for ccx in [CX - 44, CX + 44]:
    for r in range(14, 0, -2):
        density = 1 if r > 8 else 2
        for angle_i in range(0, 360, 360 // (r * density)):
            a = math.radians(angle_i)
            px = int(ccx + r * math.cos(a))
            py = int(HEAD_CY + 28 + r * math.sin(a) * 0.6)
            draw.point((px, py), fill=BLACK)

# Eyes (big friendly round eyes)
for ex in [CX - 28, CX + 28]:
    circle(ex, HEAD_CY - 8, 13, fill=BLACK)
    circle(ex + 3, HEAD_CY - 11, 5, fill=WHITE)   # catchlight

# Nose — round bulbous gnome nose
filled_ellipse(CX, HEAD_CY + 16, 16, 13, fill=WHITE, outline=BLACK, width=3)
# Nostrils
circle(CX - 6, HEAD_CY + 22, 3, fill=BLACK)
circle(CX + 6, HEAD_CY + 22, 3, fill=BLACK)

# Eyebrows (thick, expressive)
for ex, slant in [(CX - 28, +4), (CX + 28, -4)]:
    for t in range(4):
        draw.line([(ex - 16, HEAD_CY - 27 + slant + t),
                   (ex + 16, HEAD_CY - 27 - slant + t)], fill=BLACK, width=1)

# Smile
smile_pts = [(CX + int(32 * math.cos(math.radians(a))),
              HEAD_CY + 52 + int(18 * math.sin(math.radians(a))))
             for a in range(200, 341, 10)]
draw.line(smile_pts, fill=BLACK, width=5)

# ─── Mustache ─────────────────────────────────────────────────────────
for side in [-1, 1]:
    mu = [
        (CX + side * 4,  HEAD_CY + 38),
        (CX + side * 18, HEAD_CY + 34),
        (CX + side * 34, HEAD_CY + 38),
        (CX + side * 42, HEAD_CY + 46),
        (CX + side * 28, HEAD_CY + 52),
        (CX + side * 12, HEAD_CY + 50),
        (CX + side * 4,  HEAD_CY + 46),
    ]
    poly(mu, fill=BLACK)

# ─── Beard ─────────────────────────────────────────────────────────────
beard_pts = [
    (CX - 62, HEAD_CY + 50),
    (CX - 95, HEAD_CY + 120),
    (CX - 105, 590),
    (CX - 80, 650),
    (CX - 48, 620),
    (CX,      680),
    (CX + 48, 620),
    (CX + 80, 650),
    (CX + 105, 590),
    (CX + 95, HEAD_CY + 120),
    (CX + 62, HEAD_CY + 50),
]
poly(beard_pts, fill=WHITE, outline=BLACK, width=3)

# Beard texture — horizontal wave lines
for y in range(HEAD_CY + 68, 650, 12):
    pts = []
    for dx in range(-90, 91, 4):
        # Check if inside beard roughly
        norm = abs(dx) / 90
        bx_at_y = 90 * (1 - (y - HEAD_CY - 50) / 220) if y < HEAD_CY + 270 else 90
        if abs(dx) > bx_at_y * 1.1:
            continue
        pts.append((CX + dx, y + int(3 * math.sin((dx + y) * 0.12))))
    if len(pts) > 2:
        draw.line(pts, fill=BLACK, width=2)

# ─── Body / jacket ─────────────────────────────────────────────────────
jacket_pts = [
    (CX - 75, BODY_TOP),
    (CX - 110, BELT_Y),
    (CX - 80, BODY_BOT),
    (CX + 80, BODY_BOT),
    (CX + 110, BELT_Y),
    (CX + 75, BODY_TOP),
]
poly(jacket_pts, fill=WHITE, outline=BLACK, width=4)

# Jacket texture (vertical lines for linen/cloth feel)
for x in range(CX - 90, CX + 91, 8):
    y_top = BODY_TOP + max(0, (abs(x - CX) - 40) * 2)
    y_bot = BODY_BOT - max(0, (abs(x - CX) - 40) * 1)
    if y_top < y_bot:
        draw.line([(x, y_top + 5), (x, y_bot - 5)], fill=BLACK, width=1)

# Jacket lapel / centre line
draw.line([(CX, BODY_TOP + 10), (CX, BELT_Y - 10)], fill=BLACK, width=3)

# Jacket buttons (3)
for i, by in enumerate([BODY_TOP + 55, BODY_TOP + 105, BODY_TOP + 155]):
    circle(CX, by, 7, fill=WHITE, outline=BLACK, width=3)
    # cross-stitch detail
    draw.line([(CX - 3, by), (CX + 3, by)], fill=BLACK, width=1)
    draw.line([(CX, by - 3), (CX, by + 3)], fill=BLACK, width=1)

# ─── Belt ──────────────────────────────────────────────────────────────
rect(CX - 110, BELT_Y - 2, 220, 28, fill=BLACK)
# Buckle
rect(CX - 20, BELT_Y - 8, 40, 40, fill=WHITE, outline=BLACK, width=3)
rect(CX - 10, BELT_Y + 2, 20, 20, fill=BLACK)
draw.line([(CX, BELT_Y - 8), (CX, BELT_Y + 32)], fill=WHITE, width=2)

# ─── Arms ──────────────────────────────────────────────────────────────
# Left arm (hanging down, slight curve)
left_arm = [
    (CX - 75, BODY_TOP + 20),
    (CX - 118, BODY_TOP + 60),
    (CX - 132, BELT_Y + 40),
    (CX - 115, BELT_Y + 55),
    (CX - 100, BELT_Y + 20),
    (CX - 90, BODY_TOP + 40),
]
poly(left_arm, fill=WHITE, outline=BLACK, width=3)

# Right arm (slightly bent / across body)
right_arm = [
    (CX + 75, BODY_TOP + 20),
    (CX + 118, BODY_TOP + 60),
    (CX + 126, BELT_Y + 10),
    (CX + 108, BELT_Y + 28),
    (CX + 88, BELT_Y),
    (CX + 88, BODY_TOP + 40),
]
poly(right_arm, fill=WHITE, outline=BLACK, width=3)

# Hands
filled_ellipse(CX - 123, BELT_Y + 47, 18, 14, fill=WHITE, outline=BLACK, width=3)
filled_ellipse(CX + 118, BELT_Y + 19, 18, 14, fill=WHITE, outline=BLACK, width=3)

# Thumb suggestions
circle(CX - 134, BELT_Y + 40, 8, fill=WHITE, outline=BLACK, width=2)
circle(CX + 130, BELT_Y + 12, 8, fill=WHITE, outline=BLACK, width=2)

# ─── Legs / trousers (dark, dense hatching) ───────────────────────────
for leg_x, leg_w in [(CX - 78, 70), (CX + 8, 70)]:
    # Filled black for dark green trouser look
    rect(leg_x, BODY_BOT, leg_w, LEG_BOT - BODY_BOT, fill=BLACK)
    # Lighter inner line to give shape
    draw.line([(leg_x + leg_w // 2, BODY_BOT + 10),
               (leg_x + leg_w // 2, LEG_BOT - 8)], fill=WHITE, width=3)

# Trouser outline
draw.rectangle([CX - 78, BODY_BOT, CX - 8, LEG_BOT], outline=BLACK, width=3)
draw.rectangle([CX + 8,  BODY_BOT, CX + 78, LEG_BOT], outline=BLACK, width=3)

# ─── Boots ─────────────────────────────────────────────────────────────
for bx, bw in [(CX - 88, 82), (CX - 2, 82)]:
    # Shaft of boot
    rect(bx + 8, LEG_BOT - 10, bw - 16, 30, fill=BLACK)
    # Foot (rounded toe)
    boot_pts = [
        (bx,           BOOT_BOT - 20),
        (bx + bw + 12, BOOT_BOT - 20),
        (bx + bw + 14, BOOT_BOT - 8),
        (bx + bw + 10, BOOT_BOT + 4),
        (bx + 8,       BOOT_BOT + 4),
        (bx,           BOOT_BOT - 6),
    ]
    poly(boot_pts, fill=BLACK)
    # Boot shine
    draw.line([(bx + 20, BOOT_BOT - 14), (bx + bw - 5, BOOT_BOT - 14)],
              fill=WHITE, width=3)

# ═════════════════════════════════════════════
# Footer text area
# ═════════════════════════════════════════════
# Decorative double rule above footer
draw.line([(30, H - 98), (W-30, H - 98)], fill=BLACK, width=1)
draw.line([(30, H - 93), (W-30, H - 93)], fill=BLACK, width=3)

centered_text("Starting up...", H - 82, font_sub)
centered_text("v0.2.0", H - 54, font_ver)

# ─── Output ───────────────────────────────────────────────────────────
out_dir = os.path.dirname(os.path.abspath(__file__))
bmp_path = os.path.join(out_dir, "gnome_splash.bmp")
img.save(bmp_path)

# Preview PNG
img.convert("RGB").save(os.path.join(out_dir, "gnome_preview.png"))

# C header
raw = img.tobytes()
header_path = os.path.join(out_dir, "..", "include", "gnome_splash.h")
with open(header_path, "w") as f:
    f.write("// Auto-generated garden gnome splash — do not edit\n")
    f.write("// Generated by tools/generate_gnome_splash.py\n")
    f.write(f"// Image size: {W}x{H} px, {len(raw)} bytes (1-bit)\n\n")
    f.write("#pragma once\n#include <stdint.h>\n\n")
    f.write(f"#define SPLASH_WIDTH  {W}\n")
    f.write(f"#define SPLASH_HEIGHT {H}\n\n")
    f.write("static const uint8_t GNOME_SPLASH_BITMAP[] PROGMEM = {\n")
    for i in range(0, len(raw), 16):
        chunk = raw[i:i+16]
        f.write("  " + ", ".join(f"0x{b:02X}" for b in chunk) + ",\n")
    f.write("};\n")

black_px = sum(1 for p in img.getdata() if p == 0)
print(f"Saved BMP:    {bmp_path}")
print(f"Saved header: {header_path}")
print(f"Black coverage: {black_px / (W*H) * 100:.1f}%")
