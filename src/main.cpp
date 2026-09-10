#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <math.h>

#include "secrets.h"
#include "dial.h"

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

static const uint16_t HALF_HEIGHT = SCREEN_SIZE / 2;

TFT_eSPI tft = TFT_eSPI();
// One 240x240x16bpp sprite (115200 bytes) doesn't reliably fit: the ESP32's
// classic (non-PSRAM) DRAM heap has plenty of *free* memory but is split
// into fragmented regions, so the largest single allocatable block can be
// well under total free heap (measured ~110KB block with ~298KB free on
// this board). Splitting the double-buffer into top/bottom halves (57600
// bytes each) keeps every allocation comfortably under that ceiling while
// still avoiding full-frame tearing.
TFT_eSprite frameTop = TFT_eSprite(&tft);
TFT_eSprite frameBottom = TFT_eSprite(&tft);
bool spriteReady = false;  // draw calls fall back straight to tft when false

// Which surface (and locally-adjusted y) a given screen row belongs to.
TFT_eSPI* surfaceFor(uint16_t y) {
  if (!spriteReady) return &tft;
  return (y < HALF_HEIGHT) ? (TFT_eSPI*)&frameTop : (TFT_eSPI*)&frameBottom;
}
uint16_t localY(uint16_t y) {
  return (!spriteReady || y < HALF_HEIGHT) ? y : (uint16_t)(y - HALF_HEIGHT);
}

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
// coin/case part of the vanilla clock item, so the firmware only needs
// to render the part that actually animates - the physical shell is
// the "coin shell", the whole screen is the "window". dial.h (built by
// tools/gen_dial_asset.py from the REAL pre-1.5 misc/dial.png) is that
// same rotating dial texture the original clock item sampled, before
// Minecraft 1.5 replaced it with a pre-rendered 64-frame animation. We
// use the original approach instead of the modern one specifically
// because it rotates continuously rather than stepping through 64 fixed
// positions - see
// https://minecraft.wiki/w/Procedural_animated_texture_generation/Clocks
//
// Algorithm (ported from that page's setup_clock_sprite pseudocode):
// for each output pixel, take its position centered on the screen as a
// (u, v) pair in [-0.5, 0.5], rotate that coordinate by -dial_angle, and
// sample dial.h at the rotated position (wrapping around its edges).
// The item-mask/fuchsia-multiply step in the original isn't needed here
// since every screen pixel is "window" - there's no static coin shell
// to draw around it, that's the real 3D-printed part.

enum class ClockStatus { LIVE, OFFLINE, DEMO };

// Redraw throttling: dial_angle is now continuous, so "did anything
// change" can't be an equality check like the old discrete frame index
// was. At real MC speed (20 real minutes/day = ~0.3 deg/sec) a redraw
// every loop tick would be imperceptibly finer than this threshold and
// just re-triggers the exact SPI bottleneck the old frame-skip logic
// was written to avoid. Redraw once the dial has actually turned far
// enough to look different.
static const float MIN_REDRAW_ANGLE_STEP = 0.5f * DEG_TO_RAD;
float lastDrawnAngle = -1000.0f; // sentinel guarantees the first call always draws
ClockStatus lastDrawnStatus = static_cast<ClockStatus>(-1);

float angularDelta(float a, float b) {
  float d = fmodf(a - b + PI, 2.0f * PI);
  if (d < 0) d += 2.0f * PI;
  return fabsf(d - PI);
}

// The real clock item's window only ever reveals a small sliver of a much
// larger rotating dial - showing the dial's *entire* extent stretched
// across the whole screen (zoom 1.0) looks like a spinning wheel, not a
// window. DIAL_ZOOM shrinks the sampled region to 1/DIAL_ZOOM of the
// dial's extent, magnifying that smaller slice to fill the screen instead
// - 2.0 means only half the dial's width/height is ever visible at once.
// Nearest-neighbor indexing below (int truncation, no interpolation) is
// unchanged, so raising DIAL_ZOOM makes each dial texel cover *more*
// screen pixels - blockier, not smoother, matching the wiki algorithm's
// own direct pixel lookup (no bilinear filtering in the original either).
static const float DIAL_ZOOM = 2.0f;

void renderDial(float dialAngle) {
  float rx = sinf(-dialAngle);
  float ry = cosf(-dialAngle);
  for (uint16_t y = 0; y < SCREEN_SIZE; y++) {
    float v = (y / (float)(SCREEN_SIZE - 1) - 0.5f) / DIAL_ZOOM;
    TFT_eSPI* surface = surfaceFor(y);
    uint16_t ly = localY(y);
    for (uint16_t x = 0; x < SCREEN_SIZE; x++) {
      float u = -(x / (float)(SCREEN_SIZE - 1) - 0.5f) / DIAL_ZOOM;
      int32_t dx = (int32_t)((u * ry + v * rx + 0.5f) * DIAL_WIDTH)  % DIAL_WIDTH;
      int32_t dy = (int32_t)((v * ry - u * rx + 0.5f) * DIAL_HEIGHT) % DIAL_HEIGHT;
      if (dx < 0) dx += DIAL_WIDTH;
      if (dy < 0) dy += DIAL_HEIGHT;
      uint16_t color = pgm_read_word(&dial[dy * DIAL_WIDTH + dx]);
      surface->drawPixel(x, ly, color);
    }
  }
}

void drawClockFace(uint32_t ticks, ClockStatus status) {
  float dialAngle = (ticks / (float)MC_TICKS_PER_DAY) * 2.0f * PI;

  if (angularDelta(dialAngle, lastDrawnAngle) < MIN_REDRAW_ANGLE_STEP && status == lastDrawnStatus) {
    return; // nothing a viewer would actually see has changed - skip the SPI push
  }
  lastDrawnAngle = dialAngle;
  lastDrawnStatus = status;

  renderDial(dialAngle);

  uint32_t totalMinutes = (uint32_t)((ticks / (float)MC_TICKS_PER_DAY) * 24.0f * 60.0f);
  totalMinutes = (totalMinutes + 6 * 60) % (24 * 60); // MC day starts at 06:00 clock-equivalent
  uint32_t hh = totalMinutes / 60;
  uint32_t mm = totalMinutes % 60;
  char buf[8];
  snprintf(buf, sizeof(buf), "%02lu:%02lu", hh, mm);

  // The extended-fill area near the bottom edge is always a flat sky
  // color (blue or black; the disc never reaches down this far), so
  // plain white text reads fine here regardless of time of day. Neither
  // this text nor the status dot/label below ever crosses the
  // top/bottom sprite split (y=HALF_HEIGHT), so each just draws to
  // whichever single surface owns its y.
  const uint16_t TEXT_Y = SCREEN_SIZE - 20;
  TFT_eSPI* textSurface = surfaceFor(TEXT_Y);
  textSurface->setTextDatum(MC_DATUM);
  textSurface->setTextColor(TFT_WHITE);
  textSurface->drawString(buf, CENTER, localY(TEXT_Y), 4);

  const uint16_t DOT_Y = 20;
  TFT_eSPI* dotSurface = surfaceFor(DOT_Y);
  uint16_t dotColor = TFT_RED;
  if (status == ClockStatus::LIVE) dotColor = TFT_GREEN;
  else if (status == ClockStatus::DEMO) dotColor = TFT_ORANGE;
  dotSurface->fillCircle(SCREEN_SIZE - 20, localY(DOT_Y), 5, dotColor);

  if (status == ClockStatus::DEMO) {
    dotSurface->setTextColor(TFT_ORANGE);
    dotSurface->drawString("DEMO", CENTER, localY(DOT_Y), 2);
  } else if (status == ClockStatus::OFFLINE) {
    dotSurface->setTextColor(TFT_RED);
    dotSurface->drawString("OFFLINE", CENTER, localY(DOT_Y), 2);
  }

  // pushSprite is only meaningful when we're actually drawing into the
  // off-screen sprites; if allocation failed, every draw call above
  // already landed straight on the real screen via tft.
  if (spriteReady) {
    frameTop.pushSprite(0, 0);
    frameBottom.pushSprite(0, HALF_HEIGHT);
  }
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

  frameTop.setColorDepth(16);
  frameBottom.setColorDepth(16);
  Serial.printf("Heap before sprites: free=%u largestBlock=%u\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  void* topBuf = frameTop.createSprite(SCREEN_SIZE, HALF_HEIGHT);
  void* bottomBuf = topBuf ? frameBottom.createSprite(SCREEN_SIZE, HALF_HEIGHT) : nullptr;
  if (topBuf != nullptr && bottomBuf != nullptr) {
    spriteReady = true;
    Serial.println("Sprites allocated OK, drawing double-buffered (split top/bottom)");
  } else {
    // Either half failed - most likely still heap fragmentation, just
    // less of it than the old single full-screen sprite needed. Fall
    // back to drawing straight to the panel; every drawClockFace() call
    // below still works, it just isn't double-buffered (may flicker
    // slightly). Free whichever half did succeed so it isn't leaked.
    if (topBuf) frameTop.deleteSprite();
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
