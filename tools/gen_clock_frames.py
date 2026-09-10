#!/usr/bin/env python3
"""
Downloads the 64 vanilla Minecraft clock item textures (clock_00.png ..
clock_63.png, 16x16 each) and bakes them into src/clock_frames.h as a
PROGMEM RGB565 lookup table the ESP32 firmware blits directly.

These are Mojang's own game assets, pulled here only as a personal/hobby
reference for driving a personal display device - not redistributed as a
standalone asset pack. Don't push the generated header to a public repo
if that matters to you; see README.md's Asset provenance section.

Usage:
    pip install pillow requests
    python3 tools/gen_clock_frames.py
"""
import sys
from pathlib import Path

try:
    import requests
    from PIL import Image
except ImportError:
    sys.exit("Needs: pip install pillow requests")

MC_VERSION = "1.20.1"
BASE_URL = (
    f"https://raw.githubusercontent.com/InventivetalentDev/minecraft-assets/"
    f"{MC_VERSION}/assets/minecraft/textures/item/clock_{{:02d}}.png"
)
FRAME_COUNT = 64
OUT_PATH = Path(__file__).parent.parent / "src" / "clock_frames.h"


def rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def main() -> None:
    frames = []
    for i in range(FRAME_COUNT):
        url = BASE_URL.format(i)
        resp = requests.get(url, timeout=10)
        resp.raise_for_status()
        tmp = Path(f"/tmp/clock_{i:02d}.png")
        tmp.write_bytes(resp.content)
        img = Image.open(tmp).convert("RGBA")
        if img.size != (16, 16):
            sys.exit(f"frame {i}: expected 16x16, got {img.size}")

        pixels = []
        for y in range(16):
            for x in range(16):
                r, g, b, a = img.getpixel((x, y))
                # Fully transparent pixels (corners outside the coin) become
                # black - the round GC9A01 face doesn't need real alpha
                # blending since the whole screen is the clock face.
                pixels.append(rgb565(r, g, b) if a > 0 else 0x0000)
        frames.append(pixels)
        print(f"converted frame {i:02d}")

    with OUT_PATH.open("w") as f:
        f.write("#pragma once\n")
        f.write("#include <Arduino.h>\n\n")
        f.write(
            "// Vanilla Minecraft clock item textures (clock_00..clock_63,\n"
            "// 16x16 RGB565), baked in by tools/gen_clock_frames.py.\n"
            "// See README.md's Asset provenance section before sharing this\n"
            "// repo publicly.\n"
        )
        f.write(f"static const uint16_t CLOCK_FRAME_COUNT = {FRAME_COUNT};\n")
        f.write("static const uint16_t clockFrames[64][256] PROGMEM = {\n")
        for i, pixels in enumerate(frames):
            hex_vals = ", ".join(f"0x{p:04X}" for p in pixels)
            f.write(f"  {{ {hex_vals} }},\n")
        f.write("};\n")

    print(f"wrote {OUT_PATH}")


if __name__ == "__main__":
    main()
