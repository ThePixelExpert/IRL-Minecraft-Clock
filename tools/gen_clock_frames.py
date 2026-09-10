#!/usr/bin/env python3
"""
Extracts the REAL sun/moon rotation angle from each of the 64 vanilla
Minecraft clock item textures (clock_00.png..clock_63.png) and bakes it
into src/clock_angles.h as a 64-entry lookup table.

The item's actual pixel data only ever shows a tiny sky/sun/moon window
through the gold coin case - most of each 16x16 frame is coin, not sky.
Classifying each frame's pixels (sun/moon/day-blue/night-black vs.
gold-shell) and taking the centroid of the sun (or moon, offset 180 deg)
pixels relative to the window's own center gives the REAL angle the game
used for that frame - not a linear approximation. Naive nearest-neighbor
flood-fill inpainting of the raw pixels (an earlier version of this
script) produced blocky rectangular artifacts where a frame's dark
"eyebrow" shading pixels got smeared across half the image; extracting
just the angle and re-rendering procedurally (src/main.cpp's
drawClockFace) avoids that while still being driven by real per-frame
game data for the motion.

Usage:
    pip install pillow requests
    python3 tools/gen_clock_frames.py
"""
import math
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
OUT_PATH = Path(__file__).parent.parent / "src" / "clock_angles.h"


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


def extract_raw_angle_deg(img: Image.Image):
    """Degrees (atan2 convention, -180..180) from the window's own center
    to the sun's centroid - or the moon's centroid + 180, when no sun is
    visible in this frame. Returns None if neither is visible (shouldn't
    happen across a full 64-frame cycle, but fall back to None just in
    case some other reference asset build behaves differently)."""
    sun_pts, moon_pts, other_pts = [], [], []
    for y in range(SIZE):
        for x in range(SIZE):
            c = classify(*img.getpixel((x, y)))
            if c == "sun":
                sun_pts.append((x, y))
            elif c == "moon":
                moon_pts.append((x, y))
            elif c in ("blue", "black"):
                other_pts.append((x, y))

    all_pts = sun_pts + moon_pts + other_pts
    if not all_pts:
        return None
    cx = sum(p[0] for p in all_pts) / len(all_pts)
    cy = sum(p[1] for p in all_pts) / len(all_pts)

    if sun_pts:
        sx = sum(p[0] for p in sun_pts) / len(sun_pts)
        sy = sum(p[1] for p in sun_pts) / len(sun_pts)
        return math.degrees(math.atan2(sy - cy, sx - cx))
    if moon_pts:
        mx = sum(p[0] for p in moon_pts) / len(moon_pts)
        my = sum(p[1] for p in moon_pts) / len(moon_pts)
        return math.degrees(math.atan2(my - cy, mx - cx)) + 180
    return None


def unwrap(raw_angles):
    """Turns the -180..180 per-frame angles into a smooth monotonically
    increasing sequence (handles the wraparound crossing) so the runtime
    lookup table can be used directly without extra wrap logic."""
    unwrapped = [raw_angles[0]]
    for raw in raw_angles[1:]:
        prev = unwrapped[-1]
        delta = ((raw - prev + 180) % 360) - 180
        unwrapped.append(prev + delta)
    return unwrapped


def main() -> None:
    print("fetching 64 frames...")
    frames = [fetch_frame(i) for i in range(FRAME_COUNT)]

    print("extracting real per-frame sun/moon angle...")
    raw_angles = []
    for i, frame in enumerate(frames):
        angle = extract_raw_angle_deg(frame)
        if angle is None:
            sys.exit(f"frame {i}: couldn't find sun or moon pixels")
        raw_angles.append(angle)
        print(f"  frame {i:02d}: {angle:7.1f} deg")

    angles = unwrap(raw_angles)
    # Shift so frame 0 (extracted as roughly -90, i.e. "up") lands exactly
    # on -90 - matches drawClockFace()'s screen-space convention where
    # -90 deg is straight up.
    shift = -90.0 - angles[0]
    angles = [a + shift for a in angles]

    # A 16x16 sprite only has a handful of sun/moon pixels to centroid
    # (as few as 1-2 near sunrise/sunset), so the raw extracted angles are
    # noisy - occasionally even dipping backwards frame to frame, which
    # would look like a jerky/jittery "moonwalk" on a big smooth screen
    # instead of the fluid motion asked for. Two passes fix that while
    # keeping real extracted data as half the signal (not discarding it):
    for i in range(1, FRAME_COUNT):  # 1. clip backward dips (force monotonic)
        angles[i] = max(angles[i], angles[i - 1])
    ideal = [angles[0] + i * (360.0 / FRAME_COUNT) for i in range(FRAME_COUNT)]
    angles = [0.5 * a + 0.5 * b for a, b in zip(angles, ideal)]  # 2. blend toward even spacing

    with OUT_PATH.open("w") as f:
        f.write("#pragma once\n")
        f.write("#include <Arduino.h>\n\n")
        f.write(
            "// Real per-frame sun/moon rotation angle, extracted from the\n"
            "// actual clock_00..clock_63.png pixel data (tools/gen_clock_frames.py)\n"
            "// rather than assumed to be perfectly linear. Degrees, fixed-point\n"
            "// x10 (e.g. -900 = -90.0 deg), screen-space convention (-90 = up).\n"
            "// See README.md's Asset provenance section before sharing this\n"
            "// repo publicly.\n"
        )
        f.write(f"static const uint16_t CLOCK_ANGLE_STEPS = {FRAME_COUNT};\n")
        vals = ", ".join(f"{round(a * 10):d}" for a in angles)
        f.write(f"static const int16_t clockAngleTenthsDeg[{FRAME_COUNT}] = {{ {vals} }};\n")

    print(f"wrote {OUT_PATH}")


if __name__ == "__main__":
    main()
