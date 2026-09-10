#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <math.h>

#include "secrets.h"
#include "clock_angles.h"

static const uint16_t SCREEN_SIZE = 240;
static const uint16_t CENTER = SCREEN_SIZE / 2;
static const uint32_t POLL_INTERVAL_MS = 3000;
static const uint32_t MC_TICKS_PER_DAY = 24000;
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;

// Demo mode: runs a fast simulated day/night cycle so the dial, colours and
// electronics can be tested on the bench with no WiFi, bridge, or Minecraft
// server at all. Kicks in automatically whenever we've never successfully
// pulled a real tick count; drops out the moment a real reading arrives.
static const uint32_t DEMO_CYCLE_SECONDS = 60; // one full simulated MC day per this many real seconds
static const uint32_t DEMO_TICKS_PER_LOOP = (MC_TICKS_PER_DAY * 50) / (DEMO_CYCLE_SECONDS * 1000);

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite frame = TFT_eSprite(&tft);
TFT_eSPI* canvas = &tft;   // points at frame once the sprite buffer allocates
bool spriteReady = false;  // draw calls silently no-op on an un-created sprite

uint32_t lastPollMs = 0;
uint32_t currentTicks = 0;      // 0..23999, last known-good value from bridge (or simulated in demo mode)
bool haveValidTime = false;     // true if the most recent poll succeeded
bool everHadValidTime = false;  // true once we've received at least one real reading
bool wifiConnected = false;

// ---- networking -------------------------------------------------------

// Non-blocking (bounded) WiFi connect: gives up after WIFI_CONNECT_TIMEOUT_MS
// so a bench test with no WiFi in range still boots straight into demo mode
// instead of hanging in setup() forever.
bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connecting to WiFi %s", WIFI_SSID);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println("\nWiFi connect timed out, continuing in demo mode");
      return false;
    }
    delay(300);
    Serial.print(".");
  }
  Serial.printf("\nWiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
  return true;
}

bool fetchTicks(uint32_t &outTicks) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(2000);
  if (!http.begin(BRIDGE_URL)) return false;

  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err && doc["ok"] == true) {
      outTicks = doc["ticks"].as<uint32_t>() % MC_TICKS_PER_DAY;
      ok = true;
    }
  } else {
    Serial.printf("Bridge HTTP GET failed, code=%d\n", code);
  }
  http.end();
  return ok;
}

// ---- drawing -------------------------------------------------------
//
// This screen sits behind a 3D-printed gold shell that reproduces the
// coin/case part of the vanilla clock item - so the firmware only needs
// to draw the part of the item that actually animates: the little sky
// window with the sun/moon. In the real 16x16 texture that's a small
// lens-shaped cutout showing a sliver of a hidden rotating day/night
// wheel (colors sampled directly from clock_00.png/clock_32.png via
// tools/gen_clock_frames.py - see README's "Isolated animation" section
// for how that was reverse-engineered from the raw frames). Since the
// screen is the *entire* visible circle now (no coin around it), that
// hidden wheel is reconstructed here in full instead of glimpsed through
// a narrow slot: a straight day/night terminator through the center,
// rotating once per Minecraft day, with the sun riding the day pole and
// the moon riding the night pole 180 deg opposite it.

static const uint16_t SKY_DAY     = 0x3A95; // rgb(58,83,172)   - clock_00.png day sky
static const uint16_t SKY_NIGHT   = 0x18A2; // rgb(24,22,22)    - clock_32.png night sky
static const uint16_t SUN_COLOR   = 0xFFE0; // rgb(255,255,0)   - clock_00.png sun disc
static const uint16_t MOON_BASE   = 0x52AD; // rgb(86,86,109)   - clock_32.png moon disc
static const uint16_t MOON_HILITE = 0x6B71; // rgb(108,108,137) - clock_32.png moon highlight

static const int16_t POLE_DISTANCE = 50; // how far the sun/moon centers sit from screen center
static const int16_t DISC_RADIUS   = 38; // sun/moon disc size (max reach 88px keeps clear of the HUD text/dot at +-100px)

enum class ClockStatus { LIVE, OFFLINE, DEMO };

// Redraw caching: quantize rotation to the same 64 steps the vanilla
// texture used (24000 ticks / 64 = 375 ticks per step) so we redraw only
// when the picture would actually visibly change, not on every ~50ms
// loop tick. That's what actually fixes sluggish/torn refresh - the SPI
// bus was being asked to push full 240x240 screens far more often than
// the animation was actually advancing.
uint8_t lastDrawnStep = 255;
ClockStatus lastDrawnStatus = static_cast<ClockStatus>(-1);

// Step 0 = solar noon (sun straight up, symmetric day), step 32 =
// midnight (moon straight up, symmetric night) - same phase convention
// derived from the real texture frames (tick 0 is sunrise).
uint8_t animationStepForTicks(uint32_t ticks) {
  uint32_t shifted = (ticks + 18000UL) % MC_TICKS_PER_DAY;
  return (uint8_t)((shifted * CLOCK_ANGLE_STEPS) / MC_TICKS_PER_DAY) % CLOCK_ANGLE_STEPS;
}

void drawClockFace(uint32_t ticks, ClockStatus status) {
  uint8_t step = animationStepForTicks(ticks);

  if (step == lastDrawnStep && status == lastDrawnStatus) {
    return; // nothing a viewer would actually see has changed - skip the SPI push
  }
  lastDrawnStep = step;
  lastDrawnStatus = status;

  // Sun pole angle: looked up from the REAL per-frame angle extracted
  // from clock_00.png..clock_63.png (tools/gen_clock_frames.py), not a
  // pure linear formula - captures whatever actual (slightly uneven)
  // motion the vanilla animation has, smoothed just enough to stay fluid
  // rather than reproducing the raw extraction's pixel-centroid jitter.
  float sunAngleRad = radians(clockAngleTenthsDeg[step] / 10.0f);
  float sunDirX = cosf(sunAngleRad);
  float sunDirY = sinf(sunAngleRad);

  // Per-pixel day/night split: a pixel is on the sun's half of the circle
  // if its direction from center has a positive dot product with the sun
  // direction - a hard-edged line through the center, perpendicular to
  // the sun/moon axis, rotating together with them.
  for (int16_t y = 0; y < SCREEN_SIZE; y++) {
    int16_t dy = y - CENTER;
    for (int16_t x = 0; x < SCREEN_SIZE; x++) {
      int16_t dx = x - CENTER;
      float dot = dx * sunDirX + dy * sunDirY;
      canvas->drawPixel(x, y, dot >= 0 ? SKY_DAY : SKY_NIGHT);
    }
  }

  int16_t sunX = CENTER + (int16_t)(sunDirX * POLE_DISTANCE);
  int16_t sunY = CENTER + (int16_t)(sunDirY * POLE_DISTANCE);
  int16_t moonX = CENTER - (int16_t)(sunDirX * POLE_DISTANCE);
  int16_t moonY = CENTER - (int16_t)(sunDirY * POLE_DISTANCE);

  canvas->fillCircle(sunX, sunY, DISC_RADIUS, SUN_COLOR);

  canvas->fillCircle(moonX, moonY, DISC_RADIUS, MOON_BASE);
  // Small offset highlight so the moon doesn't read as a flat gray dot -
  // matches the lighter patch visible in the real clock_32.png moon.
  canvas->fillCircle(moonX - DISC_RADIUS / 3, moonY - DISC_RADIUS / 3, DISC_RADIUS / 2, MOON_HILITE);

  uint32_t totalMinutes = (uint32_t)((ticks / (float)MC_TICKS_PER_DAY) * 24.0f * 60.0f);
  totalMinutes = (totalMinutes + 6 * 60) % (24 * 60); // MC day starts at 06:00 clock-equivalent
  uint32_t hh = totalMinutes / 60;
  uint32_t mm = totalMinutes % 60;
  char buf[8];
  snprintf(buf, sizeof(buf), "%02lu:%02lu", hh, mm);

  canvas->setTextDatum(MC_DATUM);
  canvas->setTextColor(TFT_WHITE);
  canvas->drawString(buf, CENTER, SCREEN_SIZE - 20, 4);

  uint16_t dotColor = TFT_RED;
  if (status == ClockStatus::LIVE) dotColor = TFT_GREEN;
  else if (status == ClockStatus::DEMO) dotColor = TFT_ORANGE;
  canvas->fillCircle(SCREEN_SIZE - 20, 20, 5, dotColor);

  if (status == ClockStatus::DEMO) {
    canvas->setTextColor(TFT_ORANGE);
    canvas->drawString("DEMO", CENTER, 20, 2);
  } else if (status == ClockStatus::OFFLINE) {
    canvas->setTextColor(TFT_RED);
    canvas->drawString("OFFLINE", CENTER, 20, 2);
  }

  // pushSprite is only meaningful when we're actually drawing into the
  // off-screen sprite; if allocation failed, canvas points straight at
  // tft and every draw call above already landed on the real screen.
  if (spriteReady) frame.pushSprite(0, 0);
}

// ---- main -------------------------------------------------------

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  // Immediate on-screen feedback, drawn straight to tft (no sprite involved
  // yet) so you see *something* right away instead of a black screen during
  // the up-to-15s WiFi connect attempt below.
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Connecting...", CENTER, CENTER, 4);

  frame.setColorDepth(16);
  void* spriteBuf = frame.createSprite(SCREEN_SIZE, SCREEN_SIZE);
  if (spriteBuf != nullptr) {
    canvas = &frame;
    spriteReady = true;
    Serial.println("Sprite allocated OK, drawing double-buffered");
  } else {
    // Out of heap for a 240x240x16bpp (115200 byte) buffer - fall back to
    // drawing straight to the panel. Every drawClockFace() call below
    // still works, it just isn't double-buffered (may flicker slightly).
    Serial.println("WARNING: sprite allocation FAILED, drawing directly to TFT");
  }

  wifiConnected = connectWiFi();

  if (wifiConnected && fetchTicks(currentTicks)) {
    haveValidTime = true;
    everHadValidTime = true;
  }

  ClockStatus status = everHadValidTime ? ClockStatus::LIVE : ClockStatus::DEMO;
  drawClockFace(currentTicks, status);
  lastPollMs = millis();
}

void loop() {
  uint32_t now = millis();

  // No WiFi at all (bench test): don't hammer a dead radio, just run demo.
  if (!wifiConnected && WiFi.status() != WL_CONNECTED) {
    currentTicks = (currentTicks + DEMO_TICKS_PER_LOOP) % MC_TICKS_PER_DAY;
    drawClockFace(currentTicks, ClockStatus::DEMO);
    delay(50);
    return;
  }
  wifiConnected = true;

  if (now - lastPollMs >= POLL_INTERVAL_MS) {
    lastPollMs = now;
    uint32_t ticks;
    ClockStatus status;

    if (fetchTicks(ticks)) {
      currentTicks = ticks;
      haveValidTime = true;
      everHadValidTime = true;
      status = ClockStatus::LIVE;
    } else {
      haveValidTime = false;
      if (everHadValidTime) {
        // We know real MC time; keep advancing it between polls so the
        // dial doesn't freeze. Real MC time runs ~1 tick per 50ms.
        currentTicks = (currentTicks + (POLL_INTERVAL_MS / 50)) % MC_TICKS_PER_DAY;
        status = ClockStatus::OFFLINE;
      } else {
        // Never reached the bridge/server (e.g. bridge not running yet,
        // wrong URL, RCON not enabled) - fall back to the fast demo cycle
        // instead of sitting frozen at tick 0.
        currentTicks = (currentTicks + DEMO_TICKS_PER_LOOP * (POLL_INTERVAL_MS / 50)) % MC_TICKS_PER_DAY;
        status = ClockStatus::DEMO;
      }
    }
    drawClockFace(currentTicks, status);
  }

  delay(50);
}
