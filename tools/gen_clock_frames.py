#!/usr/bin/env python3
"""
Downloads the 64 vanilla Minecraft clock item textures (clock_00.png ..
clock_63.png, 16x16 each), edits out the static gold coin shell so only
the sky/sun/moon window remains, then fills every removed pixel with the
color of the nearest surviving sky/sun/moon pixel (nearest-neighbor -
same blocky/pixelated style as the source art, not smoothed) so there's
no rigid edge where the coin shell used to be. Bakes the resulting 64
frames into src/clock_frames.h as an RGB565 PROGMEM table the ESP32
firmware blits directly (nearest-neighbor scaled 16px -> 240px).

Two things the naive version of this got wrong, both fixed here:

1. Classifying "is this pixel part of the window" by whether it differs
   across the 64 frames is wrong - the gold shell has its own subtle
   per-frame shimmer/highlight shading unrelated to the sky animation,
   so that test let some gold pixels leak in as fake "window" seeds.
   Fixed by classifying per-pixel-per-frame by color family instead
   (gold has a distinct r > g > b warm signature the sky/sun/moon
   colors never do).

2. Rows 7-8 (right under where the sun/moon sits) have a handful of
   pixels that are black even in the full-daytime frame and blue even
   in the full-nighttime frame - a fixed shadow/shading detail on the
   coin surface, not real day/night. Using those as fill seeds paints
   a big wrong-colored block for half the image once flood-filled
   outward. Fixed by excluding blue/black (but not sun/moon) pixels in
   those two rows from seeding the fill - they still get filled in
   themselves, just from the real sky pixels above them instead of
   contributing their own misleading color.

These are still Mojang's own game assets (isolated/extended, not
redrawn) - see README.md's Asset provenance section before sharing this
repo publicly.

Usage:
    pip install pillow requests
    python3 tools/gen_clock_frames.py
"""
import sys
from collections import deque
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
# Empirically identified (see module docstring, point 2): rows where a
# fixed shadow detail can masquerade as day/night sky and must not be
# allowed to seed the fill, even though it's still a real texture pixel.
SHADOW_ROWS = {7, 8}
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


def classify(r: int, g: int, b: int, a: int):
    """Sky/sun/moon color family, or None for gold coin shell/transparent.
    Thresholds verified against actual sampled pixels from clock_00/08/32."""
    if a == 0:
        return None
    if r > g and g > b and (r - b) > 60:
        return None  # gold shell (incl. its per-frame shimmer/highlight)
    if r > 180 and g > 180 and b < 160 and abs(r - g) < 30:
        return "sun"
    if b > r + 40 and b > g + 40:
        return "blue"
    if max(r, g, b) < 60:
        return "black"
    if abs(r - g) < 20 and abs(g - b) < 40 and 60 <= r <= 160:
        return "moon"
    return None


def inpaint_frame(frame: Image.Image):
    """Multi-source BFS flood fill from the real, edited-in window pixels
    outward - deliberately blocky/pixelated (nearest surviving pixel,
    same look as the source sprite), not smoothed. Shadow-row blue/black
    pixels are real texture data and kept in the output, just not used to
    seed the fill outward (see module docstring)."""
    grid = [[None] * SIZE for _ in range(SIZE)]
    q = deque()
    for y in range(SIZE):
        for x in range(SIZE):
            r, g, b, a = frame.getpixel((x, y))
            c = classify(r, g, b, a)
            if c is None:
                continue
            if c in ("blue", "black") and y in SHADOW_ROWS:
                continue  # misleading shadow pixel - leave blank, let the fill overwrite it
            grid[y][x] = (r, g, b)
            q.append((x, y))

    while q:
        x, y = q.popleft()
        color = grid[y][x]
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < SIZE and 0 <= ny < SIZE and grid[ny][nx] is None:
                grid[ny][nx] = color
                q.append((nx, ny))

    return grid


def main() -> None:
    print("fetching 64 frames...")
    frames = [fetch_frame(i) for i in range(FRAME_COUNT)]

    print("editing out the coin shell + filling gaps for each frame...")
    all_pixels = []
    for i, frame in enumerate(frames):
        grid = inpaint_frame(frame)
        pixels = [rgb565(*grid[y][x]) for y in range(SIZE) for x in range(SIZE)]
        all_pixels.append(pixels)

    with OUT_PATH.open("w") as f:
        f.write("#pragma once\n")
        f.write("#include <Arduino.h>\n\n")
        f.write(
            "// Real vanilla clock animation with the gold coin shell edited\n"
            "// out and the resulting gaps filled with the nearest surviving\n"
            "// sky/sun/moon pixel (tools/gen_clock_frames.py) - deliberately\n"
            "// blocky/pixelated like the source sprite, not smoothed. Meant\n"
            "// for a screen mounted behind a 3D-printed reproduction of the\n"
            "// coin shell. See README.md's Asset provenance section before\n"
            "// sharing this repo publicly.\n"
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
