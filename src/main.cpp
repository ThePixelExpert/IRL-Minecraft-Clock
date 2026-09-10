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
static const uint16_t BODY_RADIUS = 18;   // sun/moon disc size
static const uint32_t POLL_INTERVAL_MS = 3000;
static const uint32_t MC_TICKS_PER_DAY = 24000;

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite frame = TFT_eSprite(&tft);

uint32_t lastPollMs = 0;
uint32_t currentTicks = 0;      // 0..23999, last known-good value from bridge
bool haveValidTime = false;
float displayAngleDeg = 0;      // smoothed rotation actually drawn, eases toward target

// ---- colour helpers -------------------------------------------------------

uint16_t lerpColor(uint16_t c1, uint16_t c2, float t) {
  t = constrain(t, 0.0f, 1.0f);
  uint8_t r1 = (c1 >> 11) & 0x1F, g1 = (c1 >> 5) & 0x3F, b1 = c1 & 0x1F;
  uint8_t r2 = (c2 >> 11) & 0x1F, g2 = (c2 >> 5) & 0x3F, b2 = c2 & 0x1F;
  uint8_t r = r1 + (r2 - r1) * t;
  uint8_t g = g1 + (g2 - g1) * t;
  uint8_t b = b1 + (b2 - b1) * t;
  return (r << 11) | (g << 5) | b;
}

// Sky colour across the Minecraft day cycle: dawn -> day -> dusk -> night -> dawn
uint16_t skyColorForTicks(uint32_t ticks) {
  const uint16_t NIGHT  = tft.color565(10, 10, 35);
  const uint16_t DAWN   = tft.color565(255, 150, 90);
  const uint16_t DAY    = tft.color565(100, 180, 255);
  const uint16_t DUSK   = tft.color565(255, 110, 60);

  // Minecraft ticks: 0 = sunrise, 6000 = noon, 12000 = sunset, 18000 = midnight
  if (ticks < 1000) return lerpColor(DAWN, DAY, ticks / 1000.0f);
  if (ticks < 11000) return DAY;
  if (ticks < 13000) return lerpColor(DAY, DUSK, (ticks - 11000) / 2000.0f);
  if (ticks < 14000) return lerpColor(DUSK, NIGHT, (ticks - 13000) / 1000.0f);
  if (ticks < 22000) return NIGHT;
  if (ticks < 23000) return lerpColor(NIGHT, DAWN, (ticks - 22000) / 1000.0f);
  return DAWN;
}

// ---- networking -------------------------------------------------------

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connecting to WiFi %s", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.printf("\nWiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
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

// Draws a simple sun disc with rays radiating outward at (x, y).
void drawSun(int x, int y, uint16_t color) {
  frame.fillCircle(x, y, BODY_RADIUS, color);
  for (int i = 0; i < 8; i++) {
    float a = i * (PI / 4.0f);
    int x1 = x + cosf(a) * (BODY_RADIUS + 3);
    int y1 = y + sinf(a) * (BODY_RADIUS + 3);
    int x2 = x + cosf(a) * (BODY_RADIUS + 8);
    int y2 = y + sinf(a) * (BODY_RADIUS + 8);
    frame.drawLine(x1, y1, x2, y2, color);
  }
}

// Draws a crescent-ish moon: light disc with a shadow circle offset to
// bite a chunk out of it, same trick real MC clock textures use.
void drawMoon(int x, int y, uint16_t color, uint16_t shadowColor) {
  frame.fillCircle(x, y, BODY_RADIUS, color);
  frame.fillCircle(x + BODY_RADIUS / 2, y - BODY_RADIUS / 3, BODY_RADIUS - 2, shadowColor);
}

void drawClockFace(uint32_t ticks, bool connected) {
  uint16_t sky = skyColorForTicks(ticks);
  frame.fillSprite(sky);

  // Outer bezel
  frame.drawSmoothCircle(CENTER, CENTER, DIAL_RADIUS + 14, TFT_DARKGREY, sky);
  frame.drawSmoothCircle(CENTER, CENTER, DIAL_RADIUS + 10, TFT_BLACK, sky);

  // Rotation: 0 ticks = sunrise (sun at east/right), matches vanilla clock
  // orientation where the sun climbs from the horizon at tick 0.
  float angleDeg = (ticks / (float)MC_TICKS_PER_DAY) * 360.0f;
  float angleRad = radians(angleDeg - 90.0f); // -90 so tick 0 starts at top

  int sunX = CENTER + cosf(angleRad) * DIAL_RADIUS;
  int sunY = CENTER + sinf(angleRad) * DIAL_RADIUS;
  int moonX = CENTER + cosf(angleRad + PI) * DIAL_RADIUS;
  int moonY = CENTER + sinf(angleRad + PI) * DIAL_RADIUS;

  // Faint dial line connecting sun/moon, like the compass needle in the item
  frame.drawLine(sunX, sunY, moonX, moonY, TFT_DARKGREY);

  bool isDay = ticks < 12000 || ticks > 23500;
  // Draw whichever body is "behind" (closer to bottom / below horizon-ish)
  // first so the visually "up" one draws on top when they'd overlap.
  if (sunY > moonY) {
    drawMoon(moonX, moonY, TFT_WHITE, sky);
    drawSun(sunX, sunY, TFT_YELLOW);
  } else {
    drawSun(sunX, sunY, TFT_YELLOW);
    drawMoon(moonX, moonY, TFT_WHITE, sky);
  }

  // HUD text: HH:MM in-game time + connection dot
  uint32_t totalMinutes = (uint32_t)((ticks / (float)MC_TICKS_PER_DAY) * 24.0f * 60.0f);
  // Minecraft day starts at 06:00 real-clock-equivalent at tick 0
  totalMinutes = (totalMinutes + 6 * 60) % (24 * 60);
  uint32_t hh = totalMinutes / 60;
  uint32_t mm = totalMinutes % 60;

  char buf[8];
  snprintf(buf, sizeof(buf), "%02lu:%02lu", hh, mm);

  frame.setTextDatum(MC_DATUM);
  frame.setTextColor(TFT_WHITE, sky);
  frame.drawString(buf, CENTER, CENTER + DIAL_RADIUS - 30, 4);

  frame.fillCircle(CENTER, CENTER + DIAL_RADIUS - 4, 4, connected ? TFT_GREEN : TFT_RED);

  frame.pushSprite(0, 0);
}

// ---- main -------------------------------------------------------

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  frame.setColorDepth(16);
  frame.createSprite(SCREEN_SIZE, SCREEN_SIZE);

  connectWiFi();

  if (fetchTicks(currentTicks)) haveValidTime = true;
  drawClockFace(currentTicks, haveValidTime);
  lastPollMs = millis();
}

void loop() {
  uint32_t now = millis();

  if (now - lastPollMs >= POLL_INTERVAL_MS) {
    lastPollMs = now;
    uint32_t ticks;
    if (fetchTicks(ticks)) {
      currentTicks = ticks;
      haveValidTime = true;
    } else {
      haveValidTime = false;
      // keep advancing the last known time so the dial doesn't freeze
      // between polls: real MC time runs ~1 tick per real-world 50ms.
      currentTicks = (currentTicks + (POLL_INTERVAL_MS / 50)) % MC_TICKS_PER_DAY;
    }
    drawClockFace(currentTicks, haveValidTime);
  }

  delay(50);
}
