#define USER_SETUP_LOADED

// Драйвер
#define ST7789_DRIVER

// Разрешение
#define TFT_WIDTH  240
#define TFT_HEIGHT 320

// Пины (CYD v3)
#define TFT_MISO  12
#define TFT_MOSI  13
#define TFT_SCLK  14
#define TFT_CS    15
#define TFT_DC     2
#define TFT_RST   -1

// Подсветка
#define TFT_BL    21
#define TFT_BACKLIGHT_ON HIGH

// Цветовой порядок
#define TFT_RGB_ORDER TFT_BGR

// Инверсия — начни с OFF, если не поможет — попробуй ON
#define TFT_INVERSION_OFF

// Шрифты
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

// SPI
#define SPI_FREQUENCY        27000000
#define SPI_READ_FREQUENCY    6000000
#define SPI_TOUCH_FREQUENCY  2500000

// --- Тачскрин XPT2046 (CYD) ---
#define TOUCH_CS   33
#define TOUCH_MOSI 13
#define TOUCH_MISO 12
#define TOUCH_CLK   14
#define TOUCH_IRQ   24