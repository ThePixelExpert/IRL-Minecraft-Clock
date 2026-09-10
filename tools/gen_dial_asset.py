#!/usr/bin/env python3
"""
Generates src/dial.h from the REAL pre-1.5 "dial.png" texture - the
actual rotating sky/sun/moon dial asset Minecraft's old procedural clock
system sampled, before it was replaced by the pre-rendered 64-frame
system in Java Edition 1.5. See:
https://minecraft.wiki/w/Procedural_animated_texture_generation/Clocks

This downloads the official 1.4.7 client jar straight from Mojang's own
version manifest (piston-meta.mojang.com - the same endpoint the real
launcher uses, not a fan mirror) and pulls misc/dial.png out of it. Old
(pre-1.6) client jars store loose textures directly under top-level
folders like misc/, not the assets/minecraft/textures/... layout modern
versions use - if Mojang ever reshuffles historical jar contents, adjust
DIAL_PATH_IN_JAR below.

This asset is still Mojang's own copyrighted artwork. It is NOT
committed to this repo (src/dial.h is gitignored) - you must run this
script yourself to generate it locally. See README.md's Asset
provenance section before doing anything with the output beyond
personal use on your own device.

Usage:
    pip install pillow requests
    python3 tools/gen_dial_asset.py
"""
import io
import sys
import zipfile
from pathlib import Path

try:
    import requests
    from PIL import Image
except ImportError:
    sys.exit("Needs: pip install pillow requests")

MC_VERSION = "1.4.7"  # last release before the procedural dial was removed in 1.5
VERSION_MANIFEST_URL = "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"
DIAL_PATH_IN_JAR = "misc/dial.png"
OUT_PATH = Path(__file__).parent.parent / "src" / "dial.h"


def rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def get_client_jar_url(version: str) -> str:
    manifest = requests.get(VERSION_MANIFEST_URL, timeout=10).json()
    entry = next((v for v in manifest["versions"] if v["id"] == version), None)
    if entry is None:
        sys.exit(f"version {version} not found in Mojang's manifest")
    version_json = requests.get(entry["url"], timeout=10).json()
    return version_json["downloads"]["client"]["url"]


def fetch_dial_png() -> Image.Image:
    jar_url = get_client_jar_url(MC_VERSION)
    print(f"downloading official {MC_VERSION} client jar from {jar_url} ...")
    jar_bytes = requests.get(jar_url, timeout=60).content

    with zipfile.ZipFile(io.BytesIO(jar_bytes)) as jar:
        try:
            data = jar.read(DIAL_PATH_IN_JAR)
        except KeyError:
            candidates = [n for n in jar.namelist() if "dial" in n.lower()]
            sys.exit(
                f"{DIAL_PATH_IN_JAR} not found in the {MC_VERSION} jar. "
                f"Similarly-named entries found instead: {candidates or 'none'}. "
                "Update DIAL_PATH_IN_JAR to match."
            )
    return Image.open(io.BytesIO(data)).convert("RGBA")


def main() -> None:
    dial = fetch_dial_png()
    w, h = dial.size
    print(f"extracted dial.png: {w}x{h}")

    pixels = []
    for y in range(h):
        for x in range(w):
            r, g, b, a = dial.getpixel((x, y))
            pixels.append(rgb565(r, g, b) if a else 0x0000)

    with OUT_PATH.open("w") as f:
        f.write("#pragma once\n")
        f.write("#include <Arduino.h>\n\n")
        f.write(
            "// Real pre-1.5 Minecraft clock dial texture (misc/dial.png from\n"
            "// the official 1.4.7 client jar), generated locally by\n"
            "// tools/gen_dial_asset.py. NOT committed to git - see\n"
            "// README.md's Asset provenance section.\n"
        )
        f.write(f"static const uint16_t DIAL_WIDTH = {w};\n")
        f.write(f"static const uint16_t DIAL_HEIGHT = {h};\n")
        f.write(f"static const uint16_t dial[{w * h}] PROGMEM = {{\n")
        for row_start in range(0, len(pixels), w):
            row = pixels[row_start:row_start + w]
            f.write("  " + ", ".join(f"0x{p:04X}" for p in row) + ",\n")
        f.write("};\n")

    print(f"wrote {OUT_PATH}")


if __name__ == "__main__":
    main()
