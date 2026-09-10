# mc-clock-esp32

ESP32 + 1.28" round GC9A01 TFT (240x240) displaying live Minecraft server
time as the real vanilla clock item animation, edited down to just its
moving part. This is designed to sit inside a 3D-printed shell that
reproduces the item's gold coin casing - the screen only needs to show
what actually animates. `tools/gen_clock_frames.py` takes the real
`clock_00.png`..`clock_63.png` game textures, edits out the gold coin
pixels, and fills the resulting gaps with the nearest surviving sky/sun/
moon pixel so there's no rigid edge where the coin shell used to be -
still the real, blocky/pixelated per-frame texture data, just extended
to fill the whole screen instead of a small window. See "Isolated
animation" below for the details, and "Asset provenance" before sharing
this repo publicly.

## Architecture

```
Minecraft server (RCON) <-- bridge/mc_time_bridge.py --> HTTP JSON --> ESP32 --> GC9A01
```

The ESP32 does not talk RCON directly (keeps the RCON password off the
device and off the network segment the ESP32 sits on). A small Python
bridge on your server/LAN polls the Minecraft server via RCON
(`time query daytime`) and exposes the tick count over plain HTTP. The
ESP32 polls that endpoint every few seconds and redraws the dial.

## Parts

- ESP32 dev board (any, this targets a generic 30-pin ESP32-WROOM)
- 1.28" round GC9A01 240x240 SPI TFT
- Wiring (adjust in `src/User_Setup_GC9A01.h` if you wire differently):

| GC9A01 | ESP32 |
|--------|-------|
| VCC    | 3.3V  |
| GND    | GND   |
| SCL/CLK| GPIO18|
| SDA/MOSI| GPIO23|
| RES    | GPIO4 |
| DC     | GPIO2 |
| CS     | GPIO5 |
| BLK    | 3.3V (or GPIO for PWM dimming) |

## Setup

### 1. Bridge (runs on your LAN, near the MC server)

```
cd bridge
cp config.example.json config.json   # fill in RCON host/port/password
python3 mc_time_bridge.py
```

(stdlib only — no pip installs needed, the RCON client is hand-rolled in `rcon.py`.)

Exposes `GET http://<bridge-host>:5005/time` -> `{"ticks": 6123, "day": 4}`.

### 2. ESP32 firmware (PlatformIO)

- Edit `src/secrets.h` (copy from `src/secrets.h.example`) with your WiFi
  SSID/password and the bridge URL.
- `pio run -t upload` (or open in PlatformIO IDE / VSCode extension).

Uses the `TFT_eSPI` library with a GC9A01 driver profile — the custom
`User_Setup_GC9A01.h` is wired in via `platformio.ini` build flags, no need
to hand-edit the library's own `User_Setup.h`.

## Demo / fallback mode

The firmware never just sits frozen when it can't reach the bridge:

- **No WiFi at all** (bench testing, no router in range): boots straight
  into a fast simulated day/night cycle — full 24000-tick MC day every
  `DEMO_CYCLE_SECONDS` (60s by default) — orange dot + "DEMO" label.
- **WiFi up but bridge/RCON unreachable, and never has been**: same fast
  demo cycle, so you can test the display/electronics before the bridge
  or Minecraft server is even running.
- **Bridge was reachable before but a poll fails** (network blip, server
  restart): keeps advancing the *last known real* time at real MC speed
  (1 tick / 50ms) instead of freezing or jumping to demo — red dot +
  "OFFLINE" label.
- **Bridge reachable**: normal green dot, live ticks.

Tune `DEMO_CYCLE_SECONDS` in `src/main.cpp` to speed up/slow down the
demo cycle.

## Troubleshooting: screen shows nothing

The ESP32's own LED just means the board has power/is running — it says
nothing about the display. If the screen stays blank after flashing:

1. **Flash the isolated display test first**, before debugging the full
   clock firmware:
   ```
   pio run -e display_test -t upload -t monitor
   ```
   This has zero WiFi/HTTP/JSON code — just `tft.init()` and color fills.
   If this also shows nothing, it's wiring/power/driver, not application
   logic.
2. **Check the backlight (BLK) is actually powered.** On many round GC9A01
   boards the backlight is a separate LED that needs its own supply — if
   it's not wired (or wired to a GPIO you never drove HIGH), the panel can
   be drawing correctly and you'd still see nothing.
3. **Double check every pin against `src/User_Setup_GC9A01.h`** — SCLK 18,
   MOSI 23, CS 5, DC 2, RST 4. DC and CS are the two most commonly swapped.
4. **Try `TFT_INVERSION_ON`** in `src/User_Setup_GC9A01.h` — already on by
   default in this repo since most GC9A01 clones need it; if you're
   getting a fully white/blank screen specifically, try commenting it out
   instead (some panels are the opposite).
5. **Lower `SPI_FREQUENCY`** (already defaulted to 20MHz here) — loose
   breadboard/dupont wiring often can't handle 40MHz+ and shows garbage or
   nothing.
6. **Check power**: logic is 3.3V — do not feed VCC 5V unless your specific
   board explicitly has a 5V-tolerant regulator on it (check the
   silkscreen/seller listing).
7. Watch the serial monitor (115200 baud) — both firmwares print what
   they're doing. If you see `display_test: RED` / `GREEN` / `BLUE`
   scrolling but the screen never changes, that confirms it's hardware
   (wiring/backlight/power), not code.

## Isolated animation (for the 3D-printed shell build)

The real clock item is a gold coin with a small lens-shaped window near
the top showing a sliver of the sky/sun/moon animation - most of the
item is static gold casing. If you're 3D-printing that gold casing as a
physical shell around this screen, the screen only needs to show what's
behind the window, extended out to fill the whole round screen instead
of a small cutout.

`tools/gen_clock_frames.py` builds this by literally editing the real
`clock_00.png`..`clock_63.png` textures, per frame:

1. **Classify every pixel** as sky-blue, night-black, sun, moon, or gold
   coin shell. This can't just be "does this pixel change between
   frames" (an earlier attempt did that) - the gold shell has its own
   subtle per-frame shimmer/highlight shading that has nothing to do
   with the sky animation, so that test let gold pixels leak in as fake
   "window" content. Classifying by color shape instead (gold has a
   distinct r > g > b warm signature the sky/sun/moon colors never do)
   fixed it.
2. **Delete the gold shell pixels** (and transparent corners), keeping
   only the real sky/sun/moon pixels exactly as they are.
3. **Fill every deleted pixel** with the color of the *nearest surviving*
   sky/sun/moon pixel (a plain nearest-neighbor flood fill - deliberately
   blocky/pixelated, matching the source sprite's own look, not smoothed
   into a gradient) so there's no rigid edge where the coin shell used to
   be.

One more real wrinkle: two rows right under where the sun/moon sits (row
7-8 of the 16x16 sprite) have pixels that are black even in the fully-lit
noon frame and blue even in the fully-dark midnight frame - a fixed
shading/shadow detail on the coin surface, not real day or night. Using
those as fill sources would flood-fill a big wrong-colored block across
half the screen once extended (this happened in an earlier version of
the script - noon came out half black). The fix: those specific pixels
are excluded from *seeding* the fill (step 3 above) - they're real
texture pixels and still present in the output, they just don't get to
dictate what a large chunk of empty space around them becomes.

The result (`src/clock_frames.h`, a 64-frame RGB565 table) is blitted
nearest-neighbor scaled 16px -> 240px by `blitClockFrame()` in
`src/main.cpp` - the real edited/extended texture data, pixelated like
the source sprite, not a from-scratch procedural recreation.

## Asset provenance

`src/clock_frames.h` is generated by `tools/gen_clock_frames.py`, which
downloads the 64 vanilla `clock_NN.png` item textures (Mojang's own game
assets) and bakes them into an RGB565 PROGMEM table. This is fine for a
personal display device you built for yourself, same as running a
resource/texture pack — but it's still Mojang's copyrighted artwork, not
something this repo has any license to redistribute as a standalone asset.
**Don't push this repo to a public remote with `clock_frames.h` included**
if that's a concern; regenerate it locally instead (`python3
tools/gen_clock_frames.py`, needs `pip install pillow requests`) and add
it to `.gitignore` if you want to keep the repo itself asset-free.

## Refresh rate

The GC9A01 has no hardware refresh limit issue here — the actual bottleneck
is SPI bandwidth: pushing a full 240x240x16bpp frame is 115200 bytes, which
takes tens of ms even at 20-40MHz. The firmware avoids fighting that limit
by only re-blitting and pushing a new frame when the visible clock frame
(1 of 64) or connection status actually changes — not on every ~50ms loop
tick — so it doesn't try to push more screens per second than the picture
is actually changing. If you still want snappier visuals: try raising
`SPI_FREQUENCY` back up in `src/User_Setup_GC9A01.h` once your wiring is
confirmed solid (see Troubleshooting), or shorten `DEMO_CYCLE_SECONDS` if
demo mode still feels choppy - a faster demo cycle changes frames faster,
so pushes happen more often by design there.

## Notes

- Minecraft day cycle is 24000 ticks; 0/24000 = sunrise, 6000 = noon,
  12000 = sunset, 18000 = midnight.
- Dial angle = `(ticks / 24000.0) * 360`, sun and moon are drawn 180°
  apart on that dial, sky color interpolates day/dusk/night/dawn bands.
- If your MC server doesn't have RCON enabled: set `enable-rcon=true`,
  `rcon.password=...`, `rcon.port=25575` in `server.properties` and restart.
  For this homelab's server (`itzg/minecraft-server` in the `minecraft` LXC,
  `192.168.68.90`), that means adding `ENABLE_RCON=true`, `RCON_PASSWORD=...`,
  `RCON_PORT=25575` to its container env and recreating the container —
  `server.properties` gets regenerated from env vars on that image, editing
  it directly won't survive a restart.
