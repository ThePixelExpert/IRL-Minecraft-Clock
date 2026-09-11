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
// Minecraft 1.5 replaced it with a pre-rendered 64-frame animation -
// see https://minecraft.wiki/w/Procedural_animated_texture_generation/Clocks
//
// renderDial() below ports that page's setup_clock_sprite pseudocode
// closely: same rx/ry rotation, same u/v centering, same rotated-lookup
// indexing into the dial texture, same dial_pix.rgb *= pix.r shading
// multiply at the end. The one real substitution is what stands in for
// "pix.r" (the source item texture's fuchsia-channel intensity, which
// marked - and shaded - the window in the real item texture): there's
// no real Mojang item texture in play here, since the physical 3D-printed
// shell is the actual coin casing, not a drawn one. itemMask() below is
// our own procedural falloff standing in for it - not derived from any
// Mojang asset - full brightness near the pivot, fading at the visible
// patch's edges, giving the same graduated-shading look the original's
// varying-intensity fuchsia values produced.
//
// The page also notes the real (pre-1.5) system had "230 visually
// distinct frames" - not infinitely smooth - and that the modern clock
// sprite's window shape would give 218 if procedurally regenerated.
// TOTAL_FRAMES below quantizes dial_angle to that same 218, rather than
// a continuous float, so the rotation visibly steps between distinct
// positions like the original did instead of gliding.

enum class ClockStatus { LIVE, OFFLINE, DEMO };

static const int TOTAL_FRAMES = 218;

int frameIndexFromTicks(uint32_t ticks) {
  return (int)(((uint64_t)ticks * TOTAL_FRAMES) / MC_TICKS_PER_DAY) % TOTAL_FRAMES;
}

int lastDrawnFrame = -1; // sentinel guarantees the first call always draws
ClockStatus lastDrawnStatus = static_cast<ClockStatus>(-1);

// The real clock item's window only ever reveals a small sliver of a much
// larger rotating dial, and day/night content sits on opposite sides of
// that dial (180 degrees apart) - so the rotation pivot can't be screen
// center, or the screen would show both at once (day at one edge, night
// at the other/middle). Instead the pivot sits at the bottom-center of
// the screen (a "horizon" the dial rotates against), so the screen only
// ever looks upward from that point. DIAL_ZOOM controls how far that
// upward reach extends into the dial as a fraction of one full rotation
// - it must stay above 2.0 (i.e. the reach must stay under a quarter
// turn / 0.25) so the diametrically-opposite content is never within
// reach. Nearest-neighbor indexing below (int truncation, no
// interpolation) is unchanged, so raising DIAL_ZOOM makes each dial
// texel cover *more* screen pixels - blockier, not smoother, matching
// the wiki algorithm's own direct pixel lookup (no bilinear filtering
// in the original either).
static const float DIAL_ZOOM = 2.5f;

// Reaches exactly the screen's farthest corner (top-left/top-right,
// since the pivot sits at the bottom): sqrt(0.5^2 + 1^2) / DIAL_ZOOM.
static const float MASK_RADIUS = 1.1180339887f / DIAL_ZOOM;

// Number of discrete shade bands the falloff snaps to (plus fully-dark
// below the lowest band) - a smooth analog gradient here fights the
// blocky pixel-art look everywhere else; pixel art shades in a handful
// of visible steps, not a continuous fade.
static const int MASK_LEVELS = 4;

// Stands in for pix.r in the original - see the comment block above.
float itemMask(float u, float v) {
  float r = sqrtf(u * u + v * v) / MASK_RADIUS;
  float mask = 1.0f - r;
  if (mask <= 0.0f) return 0.0f;
  if (mask >= 1.0f) return 1.0f;
  int level = (int)(mask * MASK_LEVELS); // 0..MASK_LEVELS-1
  return (level + 1) / (float)MASK_LEVELS;
}

// dial_pix.rgb *= pix.r, for a packed RGB565 pixel.
uint16_t scaleColor565(uint16_t color, float factor) {
  uint8_t r = (uint8_t)(((color >> 11) & 0x1F) * factor);
  uint8_t g = (uint8_t)(((color >> 5) & 0x3F) * factor);
  uint8_t b = (uint8_t)((color & 0x1F) * factor);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

void renderDial(float dialAngle) {
  float rx = sinf(-dialAngle);
  float ry = cosf(-dialAngle);
  for (uint16_t y = 0; y < SCREEN_SIZE; y++) {
    // v=0 at the bottom row (the pivot), going negative toward the top -
    // see the pivot placement note above.
    float v = ((y - (float)(SCREEN_SIZE - 1)) / (float)(SCREEN_SIZE - 1)) / DIAL_ZOOM;
    TFT_eSPI* surface = surfaceFor(y);
    uint16_t ly = localY(y);
    for (uint16_t x = 0; x < SCREEN_SIZE; x++) {
      float u = -(x / (float)(SCREEN_SIZE - 1) - 0.5f) / DIAL_ZOOM;
      float mask = itemMask(u, v);
      int32_t dx = (int32_t)((u * ry + v * rx + 0.5f) * DIAL_WIDTH)  % DIAL_WIDTH;
      int32_t dy = (int32_t)((v * ry - u * rx + 0.5f) * DIAL_HEIGHT) % DIAL_HEIGHT;
      if (dx < 0) dx += DIAL_WIDTH;
      if (dy < 0) dy += DIAL_HEIGHT;
      uint16_t dialPix = pgm_read_word(&dial[dy * DIAL_WIDTH + dx]);
      surface->drawPixel(x, ly, scaleColor565(dialPix, mask));
    }
  }
}

void drawClockFace(uint32_t ticks, ClockStatus status) {
  int frameIndex = frameIndexFromTicks(ticks);

  if (frameIndex == lastDrawnFrame && status == lastDrawnStatus) {
    return; // nothing a viewer would actually see has changed - skip the SPI push
  }
  lastDrawnFrame = frameIndex;
  lastDrawnStatus = status;

  float dialAngle = frameIndex * (2.0f * PI / TOTAL_FRAMES);
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
