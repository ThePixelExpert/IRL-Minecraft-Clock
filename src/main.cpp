#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <math.h>

#include "secrets.h"
#include "clock_frames.h"

static const uint16_t SCREEN_SIZE = 240;
static const uint16_t CENTER = SCREEN_SIZE / 2;
static const uint16_t SRC_SIZE = 16;   // edited clock texture is 16x16

// The real sky/sun/moon content only ever occupies this bounding box
// within the 16x16 texture (rows 2-8, cols 3-12 - verified by checking
// every frame; everything outside it is coin shell, now black). Crop to
// just that box and stretch it to fill the whole screen instead of
// upscaling the full 16x16 canvas uniformly, which would leave the real
// content as a small patch surrounded by a black margin.
static const uint16_t CROP_ROW_START = 2;
static const uint16_t CROP_ROW_END   = 9;  // exclusive
static const uint16_t CROP_COL_START = 3;
static const uint16_t CROP_COL_END   = 13; // exclusive
static const uint16_t CROP_H = CROP_ROW_END - CROP_ROW_START;
static const uint16_t CROP_W = CROP_COL_END - CROP_COL_START;
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
// to draw the part of the item that actually animates. clock_frames.h
// (built by tools/gen_clock_frames.py) is the REAL clock_00.png..
// clock_63.png pixel data with the gold coin shell edited out and the
// resulting gaps filled in with the nearest surviving sky/sun/moon pixel
// - deliberately blocky/pixelated like the source sprite, not smoothed,
// so there's no rigid edge where the coin shell used to be. This just
// blits that real edited bitmap, nearest-neighbor scaled 16px -> 240px.

enum class ClockStatus { LIVE, OFFLINE, DEMO };

// Redraw caching: the texture only has 64 visual states (one every 375
// ticks), so there's no reason to re-blit and re-push a full 240x240
// frame on every ~50ms loop tick just because currentTicks incremented
// by a handful. Skipping unchanged frames is what actually fixes
// sluggish/torn refresh - the SPI bus was being asked to push complete
// screens far more often than the picture was actually changing.
uint8_t lastDrawnFrame = 255;
ClockStatus lastDrawnStatus = static_cast<ClockStatus>(-1);

// Frame 0 in the vanilla set is solar noon (sun centered, symmetric day
// sky); frame 32 is midnight (full moon sky); the set advances one frame
// per 375 ticks (24000 ticks / 64 frames). Shift by 18000 ticks so our
// tick-0-is-sunrise convention lands on the correct vanilla frame.
uint8_t frameIndexForTicks(uint32_t ticks) {
  uint32_t shifted = (ticks + 18000UL) % MC_TICKS_PER_DAY;
  return (uint8_t)((shifted * CLOCK_FRAME_COUNT) / MC_TICKS_PER_DAY) % CLOCK_FRAME_COUNT;
}

void blitClockFrame(uint8_t frameIndex) {
  for (uint16_t y = 0; y < SCREEN_SIZE; y++) {
    uint16_t srcY = CROP_ROW_START + (uint16_t)(((uint32_t)y * CROP_H) / SCREEN_SIZE);
    const uint16_t srcRowBase = srcY * SRC_SIZE;
    for (uint16_t x = 0; x < SCREEN_SIZE; x++) {
      uint16_t srcX = CROP_COL_START + (uint16_t)(((uint32_t)x * CROP_W) / SCREEN_SIZE);
      uint16_t color = pgm_read_word(&clockFrames[frameIndex][srcRowBase + srcX]);
      canvas->drawPixel(x, y, color);
    }
  }
}

void drawClockFace(uint32_t ticks, ClockStatus status) {
  uint8_t frameIndex = frameIndexForTicks(ticks);

  if (frameIndex == lastDrawnFrame && status == lastDrawnStatus) {
    return; // nothing a viewer would actually see has changed - skip the SPI push
  }
  lastDrawnFrame = frameIndex;
  lastDrawnStatus = status;

  blitClockFrame(frameIndex);

  uint32_t totalMinutes = (uint32_t)((ticks / (float)MC_TICKS_PER_DAY) * 24.0f * 60.0f);
  totalMinutes = (totalMinutes + 6 * 60) % (24 * 60); // MC day starts at 06:00 clock-equivalent
  uint32_t hh = totalMinutes / 60;
  uint32_t mm = totalMinutes % 60;
  char buf[8];
  snprintf(buf, sizeof(buf), "%02lu:%02lu", hh, mm);

  // The extended-fill area near the bottom edge is always a flat sky
  // color (blue or black; the disc never reaches down this far), so
  // plain white text reads fine here regardless of time of day.
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
