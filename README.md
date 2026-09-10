# mc-clock-esp32

ESP32 + 1.28" round GC9A01 TFT (240x240) displaying live Minecraft server
time, animated by continuously rotating the real pre-1.5 clock dial
texture behind the screen - the same technique the original Minecraft
clock item used before Java Edition 1.5 replaced it with a pre-rendered
64-frame animation. This is designed to sit inside a 3D-printed shell
that reproduces the item's gold coin casing, so the screen only needs to
show what's visible through the case's window. See "Dial rendering"
below for how it works, and "Asset provenance" before doing anything
with the generated asset beyond personal use on your own device.

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
- Generate `src/dial.h` (see "Dial rendering" below) — required, not
  committed to this repo.
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

## Dial rendering (for the 3D-printed shell build)

The real clock item is a gold coin with a small lens-shaped window near
the top showing a sliver of a rotating sky/sun/moon dial - most of the
item is static gold casing. Since this screen sits behind a 3D-printed
reproduction of that casing, the firmware only needs to render what's
visible through the window - i.e. the whole screen is dial content, no
casing to draw.

**This is a required local generation step, same as `src/secrets.h`.**
`tools/gen_dial_asset.py` downloads the official Minecraft 1.4.7 client
jar straight from Mojang's version manifest and extracts `misc/dial.png`
- the actual dial texture the original (pre-1.5) clock item sampled,
before Java Edition 1.5 replaced that system with the pre-rendered
64-frame animation seen in modern versions. `renderDial()` in
`src/main.cpp` is a C++ port of that original system's rotation math
([reference](https://minecraft.wiki/w/Procedural_animated_texture_generation/Clocks)):
for every screen pixel, rotate its centered (u, v) coordinate by
`-dial_angle` and sample the dial texture at the rotated position,
wrapping at its edges. `dial_angle = (ticks / 24000.0) * 2*PI`, i.e. one
full continuous rotation per Minecraft day - not 64 discrete steps, so
the sun/moon actually glide instead of jumping every 375 ticks.

Run the generator once before building:

```
pip install pillow requests
python3 tools/gen_dial_asset.py
```

## Asset provenance

`src/dial.h` (generated by `tools/gen_dial_asset.py`) is Mojang's own
`misc/dial.png` game asset, sourced directly from an official Mojang
client jar. This is fine for a personal display device you built for
yourself, same as running a resource/texture pack - but it's still
Mojang's copyrighted artwork, not something this repo has any license to
redistribute as a standalone asset. **`src/dial.h` is gitignored and was
never committed to this repo** - you must run the generator yourself to
produce it locally before building the firmware.

## Refresh rate

The GC9A01 has no hardware refresh limit issue here — the actual bottleneck
is SPI bandwidth: pushing a full 240x240x16bpp frame is 115200 bytes, which
takes tens of ms even at 20-40MHz. The firmware avoids fighting that limit
by only re-rendering and pushing a new frame once the dial has rotated
past a minimum angle threshold (`MIN_REDRAW_ANGLE_STEP`, 0.5° by default)
or connection status changes — not on every ~50ms loop tick — so it
doesn't try to push more screens per second than the picture is actually
changing. If you still want snappier visuals: try raising `SPI_FREQUENCY`
back up in `src/User_Setup_GC9A01.h` once your wiring is confirmed solid
(see Troubleshooting), or shorten `DEMO_CYCLE_SECONDS` if demo mode still
feels choppy - a faster demo cycle rotates the dial faster, so pushes
happen more often by design there.

## Notes

- Minecraft day cycle is 24000 ticks; 0/24000 = sunrise, 6000 = noon,
  12000 = sunset, 18000 = midnight.
- Dial angle = `(ticks / 24000.0) * 2*PI`, sampled continuously against
  the real dial texture (see "Dial rendering") rather than drawn from
  scratch - sun/moon placement and sky shading come from that texture.
- If your MC server doesn't have RCON enabled: set `enable-rcon=true`,
  `rcon.password=...`, `rcon.port=25575` in `server.properties` and restart.
  For this homelab's server (`itzg/minecraft-server` in the `minecraft` LXC,
  `192.168.68.90`), that means adding `ENABLE_RCON=true`, `RCON_PASSWORD=...`,
  `RCON_PORT=25575` to its container env and recreating the container —
  `server.properties` gets regenerated from env vars on that image, editing
  it directly won't survive a restart.
