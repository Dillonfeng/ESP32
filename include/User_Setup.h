//                            USER DEFINED SETTINGS
//   Set driver type, fonts to be loaded, pins used and SPI control method etc
//
//   See the User_Setup_Select.h file if you wish to be able to define multiple
//   setups and then easily select which setup file is used by the compiler.
//
//   If this file is edited correctly then all the library example sketches should
//   run without the need to make any more changes for a particular hardware setup!

// ##################################################################################
//
// Section 0. Call up the right driver file and any options for it
//
// ##################################################################################

// ESP32-S3: use FSPI port 2 (fixes REG_SPI_BASE returning 0 for FSPI=1)
#define USE_FSPI_PORT

// Define ST7796 driver for 4.0" 480x320 SPI TFT display
#define ST7796_DRIVER

// Display dimensions (ST7796 native portrait 320x480, landscape via MV in rotation)
#define TFT_WIDTH  320
#define TFT_HEIGHT 480

// Color order: BGR (memory confirmed - screen needs BGR color order)
#define TFT_RGB_ORDER TFT_BGR

// Only define one driver, the other ones must be commented out
//#define ILI9341_DRIVER
//#define ST7735_DRIVER
//#define ILI9163_DRIVER
//#define S6D02A1_DRIVER
//#define RPI_ILI9486_DRIVER // 20MHz maximum SPI
//#define M5STACK
//#define TFT_DRIVER 1
//#define ST7789_DRIVER
//#define R61581_DRIVER
//#define RM68140
//#define SSD1351_DRIVER
//#define SSD1963_480_DRIVER
//#define SSD1963_800_DRIVER
//#define SSD1963_800ALT_DRIVER
//#define ILI9488_DRIVER

// For ST7789 and ST7796S displays with no CS pin (or CS tied to ground)
// #define TFT_SDA_READ   // Display has a bidirectional SDA pin (no MISO)

// ##################################################################################
//
// Section 1. Define the pins that are used to interface with the display here
//
// ##################################################################################

// ESP32-S3 引脚定义 - 4.0" ST7796S SPI TFT
#define TFT_CS    10  // Chip select control pin
#define TFT_DC     9  // Data Command control pin
#define TFT_RST   14  // Reset pin (could connect to Arduino RESET pin)
#define TFT_BL    21  // LED back-light

// SPI 引脚
#define TFT_MISO  13
#define TFT_MOSI  11
#define TFT_SCLK  12

#define TFT_BACKLIGHT_ON HIGH  // HIGH or LOW are options

// ##################################################################################
//
// Section 2. Write only
//
// ##################################################################################

//  #define SPI_FREQUENCY  20000000
//  #define SPI_READ_FREQUENCY  20000000
#define SPI_FREQUENCY   40000000   // 40MHz (ST7796 支持; 20MHz 时翻页/指针整面闪)
#define SPI_READ_FREQUENCY  20000000

// ##################################################################################
//
// Section 3. Define the fonts that are to be used here
//
// ##################################################################################

// Comment out the following lines to remove unwanted fonts from FLASH
#define LOAD_GLCD   // Font 1. Original Adafruit 8 pixel font needs ~1820 bytes in FLASH
#define LOAD_FONT2  // Font 2. Small 16 pixel high font, needs ~3534 bytes in FLASH, 96 characters
#define LOAD_FONT4  // Font 4. Medium 26 pixel high font, needs ~5848 bytes in FLASH, 96 characters
#define LOAD_FONT6  // Font 6. Large 48 pixel font, needs ~2666 bytes in FLASH, only characters 1234567890:-.apm
#define LOAD_FONT7  // Font 7. 7 segment 48 pixel font, needs ~2438 bytes in FLASH, only characters 1234567890:.
#define LOAD_FONT8  // Font 8. Large 75 pixel font needs ~3256 bytes in FLASH, only digits, e.g. clock
//#define LOAD_FONT8N // Font 8. Alternative to Font 8 above, slightly narrower, so 3 digits fit a 160 pixel TFT
#define LOAD_GFXFF  // FreeFonts. Include access to the 48 Adafruit_GFX free fonts FF1 to FF48 and custom fonts

// ##################################################################################
//
// Section 4. Other options
//
// ##################################################################################

// #define SPI_TOUCH_FREQUENCY  2500000
// BUILDV5-TOUCHSPEED: 2.5→8MHz。XPT2046 12-bit ADC 支持最高 ~10MHz SPI；
// getTouch() 耗时从 ~92ms 降到 ~30ms → main loop 帧率 10-20fps → 25-30fps，
// 配合高频泵(每帧10次 audio.loop) → HLS 下载 ≥150KB/s ≥ 播放消耗 110KB/s。
#define SPI_TOUCH_FREQUENCY  8000000

// Comment out the following to disable touch support
#define SUPPORT_XPT2046

// XPT2046 触摸引脚
#define TOUCH_CS  1   // GPIO20被USB D+占用,改用GPIO1
#define TOUCH_IRQ 38  // GPIO19被USB D-占用,改用GPIO38

// ##################################################################################
//
// Section 5. Touch screen calibration
//
// ##################################################################################

// Calibration values
#define XPT2046_X_MIN  300
#define XPT2046_X_MAX  3900
#define XPT2046_Y_MIN  200
#define XPT2046_Y_MAX  3700

#define XPT2046_INV_X  1
#define XPT2046_INV_Y  1

// #define XPT2046_ROTATION 0
