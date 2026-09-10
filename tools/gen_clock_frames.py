#!/usr/bin/env python3
"""
Downloads the 64 vanilla Minecraft clock item textures (clock_00.png ..
clock_63.png, 16x16 each) and edits out the static gold coin shell,
leaving only the real sky/sun/moon window pixels - everything else
(coin, transparent corners) becomes black. No fill/extension step yet;
that's a later pass once this first edit is confirmed on real hardware.
Bakes the resulting 64 frames into src/clock_frames.h as an RGB565
PROGMEM table the ESP32 firmware blits directly (nearest-neighbor scaled
16px -> 240px).

Classifying "is this pixel part of the window" by whether it differs
across the 64 frames is wrong - the gold shell has its own subtle
per-frame shimmer/highlight shading unrelated to the sky animation, so
that test lets some gold pixels leak in as fake "window" content.
Classifying per-pixel-per-frame by color family instead (gold has a
distinct r > g > b warm signature the sky/sun/moon colors never do)
avoids that.

These are still Mojang's own game assets (edited, not redrawn) - see
README.md's Asset provenance section before sharing this repo publicly.

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
SIZE = 16
OUT_PATH = Path(__file__).parent.parent / "src" / "clock_frames.h"


def rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def fetch_frame(i: int) -> Image.Image:
    url = BASE_URL.format(i)
    resp = requests.get(url, timeout=10)
    resp.raise_for_status()
    tmp = Path(f"/tmp/clock_{i:02d}.png")
    tmp.write_bytes(resp.content)
    img = Image.open(tmp).convert("RGBA")
    if img.size != (SIZE, SIZE):
        sys.exit(f"frame {i}: expected {SIZE}x{SIZE}, got {img.size}")
    return img


def is_window_pixel(r: int, g: int, b: int, a: int) -> bool:
    """True for real sky/sun/moon colors, False for gold coin shell or
    transparency. Thresholds verified against actual sampled pixels from
    clock_00/08/32.png."""
    if a == 0:
        return False
    if r > g and g > b and (r - b) > 60:
        return False  # gold shell (incl. its per-frame shimmer/highlight)
    return True


def cut_out_shell(frame: Image.Image):
    """Just the removal step: real window pixels kept as-is, everything
    else (coin shell, transparent corners) becomes black. No fill."""
    grid = [[(0, 0, 0)] * SIZE for _ in range(SIZE)]
    for y in range(SIZE):
        for x in range(SIZE):
            r, g, b, a = frame.getpixel((x, y))
            if is_window_pixel(r, g, b, a):
                grid[y][x] = (r, g, b)
    return grid


def main() -> None:
    print("fetching 64 frames...")
    frames = [fetch_frame(i) for i in range(FRAME_COUNT)]

    print("cutting the gold coin shell out of each frame...")
    all_pixels = []
    for i, frame in enumerate(frames):
        grid = cut_out_shell(frame)
        pixels = [rgb565(*grid[y][x]) for y in range(SIZE) for x in range(SIZE)]
        all_pixels.append(pixels)

    with OUT_PATH.open("w") as f:
        f.write("#pragma once\n")
        f.write("#include <Arduino.h>\n\n")
        f.write(
            "// Real vanilla clock animation with the gold coin shell cut\n"
            "// out (tools/gen_clock_frames.py) - everywhere the shell used\n"
            "// to be is black, no fill/extension yet. Meant for a screen\n"
            "// mounted behind a 3D-printed reproduction of the coin shell.\n"
            "// See README.md's Asset provenance section before sharing this\n"
            "// repo publicly.\n"
        )
        f.write(f"static const uint16_t CLOCK_FRAME_COUNT = {FRAME_COUNT};\n")
        f.write(f"static const uint16_t clockFrames[{FRAME_COUNT}][{SIZE*SIZE}] PROGMEM = {{\n")
        for pixels in all_pixels:
            hex_vals = ", ".join(f"0x{p:04X}" for p in pixels)
            f.write(f"  {{ {hex_vals} }},\n")
        f.write("};\n")

    print(f"wrote {OUT_PATH}")


if __name__ == "__main__":
    main()
