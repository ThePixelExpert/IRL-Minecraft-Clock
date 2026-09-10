# mc-clock-esp32

ESP32 + 1.28" round GC9A01 TFT (240x240) displaying live Minecraft server
time as an isolated recreation of the vanilla clock item's animated part
only. This is designed to sit inside a 3D-printed shell that reproduces
the item's gold coin casing - the screen only needs to show what actually
moves: a day/night sky with the sun or moon, extrapolated from the real
game texture's tiny sky window to fill the whole round screen. See
"Isolated animation" below for how the geometry was derived, and "Asset
provenance" before sharing this repo publicly.

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
the top showing a sliver of a hidden rotating day/night wheel - most of
the item is static gold casing. If you're 3D-printing that gold casing as
a physical shell around this screen, the screen only needs to draw the
part that actually moves.

Pulling and inspecting the raw `clock_00.png`..`clock_63.png` frames
(see Asset provenance) showed the window's content is a straight
blue(day)/black(night) split that rotates over the day, with the sun
riding the day side and the moon riding the night side, 180 deg apart -
consistent with a single hidden disc where the sun and moon sit at
opposite poles. Since the vanilla texture only ever reveals a small
sliver of that disc through the narrow window, showing the *whole* disc
requires extrapolating past what any single frame actually shows -
`drawClockFace()` in `src/main.cpp` reconstructs the full circle: a hard
day/night terminator line through the center (perpendicular to the
sun/moon axis) rotating once per Minecraft day, sun and moon discs at
the two poles. Colors (sky blue, night black, sun yellow, moon gray/blue
plus its highlight) are sampled directly from real pixels in
`clock_00.png` and `clock_32.png`, not guessed.

**The rotation angle itself is extracted from the real frames too, not a
linear formula.** `tools/gen_clock_frames.py` classifies every pixel in
each of the 64 frames (sun/moon/day-blue/night-black vs. the gold shell,
including the shell's own subtle per-frame shimmer, which had to be
told apart from real sky colors by hue shape rather than "does it change
between frames" - it does, and that's not the same thing as being part of
the animation) and takes the sun's (or moon's) pixel centroid angle
relative to the window's center. That gives 64 real angles - but a 16x16
sprite only has a handful of sun/moon pixels to centroid (as few as 1-2
near sunrise/sunset), so the raw extraction is noisy and briefly dips
backwards frame to frame. The script clips that to monotonic and blends
it 50/50 with a perfectly even 360/64-degree progression before baking
it into `src/clock_angles.h` - real extracted motion data, smoothed just
enough to read as fluid instead of jittery on a screen 15x bigger than
the source sprite.

An earlier version of this firmware instead tried isolating the raw
per-frame *pixels* (masking out the gold shell, flood-filling the gaps
with the nearest surviving sky color) and blitting that bitmap directly.
It looked wrong: a fixed dark "shadow" pixel band under the sun/moon
disc - present in the source texture in every frame, not really part of
day/night at all - flood-filled into a big blocky rectangle when
extended out to fill the whole screen. Extracting just the angle and
re-rendering procedurally avoids that while still being driven by real
per-frame game data for the motion.

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
