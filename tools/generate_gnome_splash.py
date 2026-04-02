#!/usr/bin/env python3
"""Regenerate the startup gnome splash assets.

The firmware consumes a 1-bit bitmap header generated from the canonical
artwork preview. The current artwork source is the nicer reading gnome:
`tools/gnome_final_preview.png`.
"""

from pathlib import Path
import os

from PIL import Image

W, H = 540, 960
# The baked footer text in the source image does not begin until roughly y=941,
# but we intentionally keep a dedicated footer region below the static art for
# runtime-rendered status + version text on the device.
ART_HEIGHT = 870
SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
SOURCE_IMAGE = Path(os.environ.get("GNOME_SPLASH_SOURCE", SCRIPT_DIR / "gnome_final_preview.png"))
BMP_PATH = SCRIPT_DIR / "gnome_splash.bmp"
PREVIEW_PATH = SCRIPT_DIR / "gnome_preview.png"
HEADER_PATH = REPO_ROOT / "include" / "gnome_splash.h"


def load_source_image() -> Image.Image:
    if not SOURCE_IMAGE.exists():
        raise FileNotFoundError(f"Splash source image not found: {SOURCE_IMAGE}")

    img = Image.open(SOURCE_IMAGE).convert("RGB")
    if img.size != (W, H):
        img = img.resize((W, H), Image.Resampling.LANCZOS)
    return img


def main() -> None:
    img_rgb = load_source_image()
    img_rgb = img_rgb.crop((0, 0, W, ART_HEIGHT))
    img_1bit = img_rgb.convert("1")

    BMP_PATH.parent.mkdir(parents=True, exist_ok=True)
    HEADER_PATH.parent.mkdir(parents=True, exist_ok=True)

    img_1bit.save(BMP_PATH)
    img_rgb.save(PREVIEW_PATH)

    raw = img_1bit.tobytes()
    with open(HEADER_PATH, "w", encoding="utf-8") as f:
        f.write("// Auto-generated from tools/generate_gnome_splash.py — do not edit\n")
        f.write(f"// Source image: {SOURCE_IMAGE.name}\n")
        f.write(f"// Image size: {W}x{ART_HEIGHT} px, {len(raw)} bytes (1-bit)\n\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write(f"#define SPLASH_WIDTH      {W}\n")
        f.write(f"#define SPLASH_ART_HEIGHT {ART_HEIGHT}\n\n")
        f.write("static const uint8_t GNOME_SPLASH_BITMAP[] PROGMEM = {\n")
        for i in range(0, len(raw), 16):
            chunk = raw[i:i + 16]
            f.write("  " + ", ".join(f"0x{b:02X}" for b in chunk) + ",\n")
        f.write("};\n")

    black_px = sum(1 for p in img_1bit.getdata() if p == 0)
    print(f"Source:   {SOURCE_IMAGE}")
    print(f"Saved BMP:    {BMP_PATH}")
    print(f"Saved preview: {PREVIEW_PATH}")
    print(f"Saved header:  {HEADER_PATH}")
    print(f"Black coverage: {black_px / (W * ART_HEIGHT) * 100:.1f}%")


if __name__ == "__main__":
    main()
