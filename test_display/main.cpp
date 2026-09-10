// Bare-bones display test: no WiFi, no bridge, no clock logic. Just proves
// the GC9A01 is wired correctly and TFT_eSPI can talk to it. Flash with:
//   pio run -e display_test -t upload -t monitor
//
// What you should see, cycling every ~1.5s: solid RED, then GREEN, then
// BLUE full-screen fills, then a white circle outline with "HELLO" text.
// If the screen stays blank through all of this, it's wiring/power/backlight,
// not the clock firmware's fault - see README.md's Troubleshooting section.
#include <Arduino.h>
#include <TFT_eSPI.h>

TFT_eSPI tft = TFT_eSPI();

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("display_test: starting tft.init()");

  tft.init();
  tft.setRotation(0);

  Serial.println("display_test: init done, entering fill-color loop");
}

void loop() {
  Serial.println("display_test: RED");
  tft.fillScreen(TFT_RED);
  delay(1500);

  Serial.println("display_test: GREEN");
  tft.fillScreen(TFT_GREEN);
  delay(1500);

  Serial.println("display_test: BLUE");
  tft.fillScreen(TFT_BLUE);
  delay(1500);

  Serial.println("display_test: circle + text");
  tft.fillScreen(TFT_BLACK);
  tft.drawCircle(120, 120, 100, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("HELLO", 120, 120, 4);
  delay(1500);
}
