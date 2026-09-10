#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <math.h>

#include "secrets.h"

static const uint16_t SCREEN_SIZE = 240;
static const uint16_t CENTER = SCREEN_SIZE / 2;
static const uint16_t DIAL_RADIUS = 108;
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
// Recreates the vanilla clock item's animation: a gold pocket-watch case
// with a cream face and a single black hand that sweeps one full turn per
// Minecraft day (24000 ticks). Colours/geometry are an original
// from-scratch recreation, not extracted game assets.

static const uint16_t GOLD_LIGHT = 0xFE60; // tft.color565(255, 204, 0)-ish, precomputed for speed
static const uint16_t GOLD_DARK  = 0x9AC0; // deeper gold for the bezel's outer edge/shadow
static const uint16_t FACE_CREAM = 0xF7BC; // warm off-white dial face
static const uint16_t HAND_BLACK = 0x0000;

enum class ClockStatus { LIVE, OFFLINE, DEMO };

void drawClockFace(uint32_t ticks, ClockStatus status) {
  canvas->fillScreen(TFT_BLACK); // fillScreen == fillSprite when canvas is the sprite; works on both

  // Gold case: two concentric rings (outer = darker gold shadow edge, inner
  // = brighter gold face of the bezel), then the cream dial on top.
  canvas->fillSmoothCircle(CENTER, CENTER, DIAL_RADIUS + 16, GOLD_DARK);
  canvas->fillSmoothCircle(CENTER, CENTER, DIAL_RADIUS + 11, GOLD_LIGHT);
  canvas->fillSmoothCircle(CENTER, CENTER, DIAL_RADIUS + 2, GOLD_DARK);
  canvas->fillSmoothCircle(CENTER, CENTER, DIAL_RADIUS, FACE_CREAM);

  // 8 tick marks around the rim, like the notches on the real item texture.
  for (int i = 0; i < 8; i++) {
    float a = radians(i * 45.0f);
    int x1 = CENTER + cosf(a) * (DIAL_RADIUS - 6);
    int y1 = CENTER + sinf(a) * (DIAL_RADIUS - 6);
    int x2 = CENTER + cosf(a) * (DIAL_RADIUS - 14);
    int y2 = CENTER + sinf(a) * (DIAL_RADIUS - 14);
    canvas->drawWideLine(x1, y1, x2, y2, 3, GOLD_DARK);
  }

  // The hand: tick 0 (sunrise) points straight up, sweeping clockwise
  // through a full 360 deg over 24000 ticks, same period as the real item.
  float angleRad = radians((ticks / (float)MC_TICKS_PER_DAY) * 360.0f - 90.0f);
  int tipX = CENTER + cosf(angleRad) * (DIAL_RADIUS - 18);
  int tipY = CENTER + sinf(angleRad) * (DIAL_RADIUS - 18);

  // Taper the hand into a thin triangle instead of a uniform-width line,
  // closer to the wedge-shaped hand on the actual texture.
  float perpRad = angleRad + PI / 2.0f;
  int baseHalfWidth = 5;
  int bx1 = CENTER + cosf(perpRad) * baseHalfWidth;
  int by1 = CENTER + sinf(perpRad) * baseHalfWidth;
  int bx2 = CENTER - cosf(perpRad) * baseHalfWidth;
  int by2 = CENTER - sinf(perpRad) * baseHalfWidth;
  canvas->fillTriangle(bx1, by1, bx2, by2, tipX, tipY, HAND_BLACK);

  // Center hub
  canvas->fillSmoothCircle(CENTER, CENTER, 6, HAND_BLACK);

  // HUD text: HH:MM in-game time + connection dot
  uint32_t totalMinutes = (uint32_t)((ticks / (float)MC_TICKS_PER_DAY) * 24.0f * 60.0f);
  // Minecraft day starts at 06:00 real-clock-equivalent at tick 0
  totalMinutes = (totalMinutes + 6 * 60) % (24 * 60);
  uint32_t hh = totalMinutes / 60;
  uint32_t mm = totalMinutes % 60;

  char buf[8];
  snprintf(buf, sizeof(buf), "%02lu:%02lu", hh, mm);

  canvas->setTextDatum(MC_DATUM);
  canvas->setTextColor(HAND_BLACK, FACE_CREAM);
  canvas->drawString(buf, CENTER, CENTER + DIAL_RADIUS - 30, 4);

  uint16_t dotColor = TFT_RED;
  if (status == ClockStatus::LIVE) dotColor = TFT_DARKGREEN;
  else if (status == ClockStatus::DEMO) dotColor = TFT_ORANGE;
  canvas->fillCircle(CENTER, CENTER + DIAL_RADIUS - 4, 4, dotColor);

  if (status == ClockStatus::DEMO) {
    canvas->setTextColor(TFT_ORANGE, FACE_CREAM);
    canvas->drawString("DEMO", CENTER, CENTER - DIAL_RADIUS + 22, 2);
  } else if (status == ClockStatus::OFFLINE) {
    canvas->setTextColor(TFT_RED, FACE_CREAM);
    canvas->drawString("OFFLINE", CENTER, CENTER - DIAL_RADIUS + 22, 2);
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
