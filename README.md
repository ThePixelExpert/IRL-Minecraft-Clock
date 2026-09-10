# mc-clock-esp32

ESP32 + 1.28" round GC9A01 TFT (240x240) displaying live Minecraft server
time as an animated Minecraft-style clock (rotating sun/moon dial, sky color
shifts with time of day).

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

## Notes

- Minecraft day cycle is 24000 ticks; 0/24000 = sunrise, 6000 = noon,
  12000 = sunset, 18000 = midnight.
- Dial angle = `(ticks / 24000.0) * 360`, sun and moon are drawn 180°
  apart on that dial, sky color interpolates day/dusk/night/dawn bands.
- If your MC server doesn't have RCON enabled: set `enable-rcon=true`,
  `rcon.password=...`, `rcon.port=25575` in `server.properties` and restart.
