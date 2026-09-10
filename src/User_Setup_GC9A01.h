// TFT_eSPI display profile for a 1.28" round GC9A01 240x240 SPI panel.
// Injected via platformio.ini build_flags (-include), overrides the
// library's default User_Setup.h without editing library files.

#define GC9A01_DRIVER

#define TFT_WIDTH  240
#define TFT_HEIGHT 240

#define TFT_MISO -1
#define TFT_MOSI 23
#define TFT_SCLK 18
#define TFT_CS    5
#define TFT_DC    2
#define TFT_RST   4
#define TFT_BL   -1   // backlight tied straight to 3.3V; set a GPIO here to PWM-dim it

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

#define SPI_FREQUENCY       40000000
#define SPI_READ_FREQUENCY  20000000
