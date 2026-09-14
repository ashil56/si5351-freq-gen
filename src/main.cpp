#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Preferences.h>
#include <Si5351.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

// ============================================================
//  Si5351 Generator + Keypad + Presets + AutoSave + FactoryReset
// ============================================================

// --- Пины энкодера ---
#define PIN_ENC_CLK   35
#define PIN_ENC_DT    27
#define PIN_ENC_SW     0

// --- Пины I2C ---
#define PIN_SDA       21
#define PIN_SCL       22

// --- Пины тачскрина ---
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

// --- Объекты ---
TFT_eSPI tft = TFT_eSPI();
Si5351   si5351;
SPIClass touchscreenSPI = SPIClass(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
Preferences prefs;

// --- Экраны ---
enum Screen { SCR_MAIN, SCR_KEYPAD, SCR_PRESETS };
Screen screen = SCR_MAIN;

// --- Энкодер ---
uint8_t encLast = 0;
int     encAccum = 0;

// --- Кнопка ---
bool btnWasPressed = false;
unsigned long btnPressTime = 0;
bool longPressHandled = false;
#define LONG_PRESS_MS        2000
#define FACTORY_WARN_MS      5000
#define FACTORY_RESET_MS    10000

bool factoryWarningShown = false;
bool factoryResetFired   = false;

// --- Тач ---
bool touchWasPressed = false;
unsigned long lastTouchTime = 0;
#define TOUCH_DEBOUNCE_MS 400

unsigned long touchStartTime = 0;
int8_t        touchSlot = -1;
bool          touchLongFired = false;
#define TOUCH_LONG_MS 700

// --- Частоты и состояние выходов ---
long freq[3] = { 10000000, 14000000, 21000000 };
bool outputOn[3] = { true, true, true };
uint8_t activeCh = 0;
long step = -100000;

// --- Отрисовка ---
bool needRedraw = true;

// --- Автосохранение ---
bool stateDirty = false;
unsigned long stateDirtyTime = 0;
#define AUTOSAVE_DELAY_MS 2000

// --- Клавиатура ---
String keyInput = "";
uint8_t keyUnit = 2;
uint8_t keyChannel = 0;

// --- Пресеты ---
#define NUM_PRESETS 8
bool presetValid[NUM_PRESETS];
long presetFreq[NUM_PRESETS][3];
bool presetOn[NUM_PRESETS][3];

// --- Таблица энкодера ---
static const int8_t encTable[16] = {
  0, +1, -1,  0,
  -1,  0,  0, +1,
  +1,  0,  0, -1,
   0, -1, +1,  0
};

// --- Геометрия ---
#define HEADER_H      40
#define KEYPAD_TOP    40
#define KEYPAD_ROW_H  50
#define KEYPAD_COL_W  80
#define CELL_W        80
#define CELL_H        90
#define CELLS_TOP     40

// --- Прототипы ---
void drawHeader();
void drawAll();
void drawStep();
void drawKeypad();
void drawPresetsScreen();
void drawPresetCell(int n, int x, int y);
void showSplash(bool restored);
void showFactoryWarning();
void doFactoryReset();

void setSi5351Freq(long freqHz, uint8_t clk);
void setOutputEnabled(uint8_t ch, bool on);
void toggleOutput(uint8_t ch);
void cycleStep();
void selectChannel(uint8_t ch);
void markDirty();

void openKeypad(uint8_t ch);
void closeKeypad(bool apply);
void openPresets();
void closePresets();
void savePreset(uint8_t n);
void loadPreset(uint8_t n);
void loadAllPresets();

void loadLastState();
void saveLastState();

void handleKeypadTouch(int16_t x, int16_t y);
void handleMainTouch(int16_t x, int16_t y);
void applyKeyInput();

// ===================== Автосохранение =====================
void markDirty() {
  stateDirty = true;
  stateDirtyTime = millis();
}

void loadLastState(bool &wasEmpty) {
  prefs.begin("laststate", true);
  wasEmpty = !prefs.isKey("f0");
  for (int i = 0; i < 3; i++) {
    char key[8];
    sprintf(key, "f%d", i);
    freq[i] = prefs.getLong(key, freq[i]);
    sprintf(key, "o%d", i);
    outputOn[i] = prefs.getBool(key, true);
  }
  activeCh = prefs.getUChar("active", 0);
  step     = prefs.getLong("step", -100000);
  prefs.end();

  if (activeCh > 2) activeCh = 0;
  Serial.print("State loaded, wasEmpty="); Serial.println(wasEmpty);
}

void saveLastState() {
  prefs.begin("laststate", false);
  for (int i = 0; i < 3; i++) {
    char key[8];
    sprintf(key, "f%d", i); prefs.putLong(key, freq[i]);
    sprintf(key, "o%d", i); prefs.putBool(key, outputOn[i]);
  }
  prefs.putUChar("active", activeCh);
  prefs.putLong("step", step);
  prefs.end();

  stateDirty = false;
  Serial.println("State saved");
}

// ===================== Factory Reset =====================
void doFactoryReset() {
  Serial.println("FACTORY RESET");

  // Красный экран с предупреждением
  tft.fillScreen(TFT_RED);
  tft.setTextColor(TFT_WHITE, TFT_RED);
  tft.setTextSize(3);
  tft.setCursor(50, 80);
  tft.println("FACTORY");
  tft.setCursor(50, 115);
  tft.println("RESET");

  tft.setTextSize(2);
  tft.setCursor(60, 165);
  tft.print("Erasing NVS...");

  // Очищаем оба раздела NVS
  prefs.begin("laststate", false);
  prefs.clear();
  prefs.end();

  prefs.begin("presets", false);
  prefs.clear();
  prefs.end();

  delay(500);
  tft.setCursor(70, 195);
  tft.print("Rebooting...");
  delay(700);

  ESP.restart();
}

void showFactoryWarning() {
  // Плашка поверх экрана
  tft.fillRect(30, 100, 260, 70, TFT_MAROON);
  tft.drawRect(30, 100, 260, 70, TFT_WHITE);
  tft.drawRect(31, 101, 258, 68, TFT_WHITE);

  tft.setTextColor(TFT_WHITE, TFT_MAROON);
  tft.setTextSize(2);
  tft.setCursor(50, 115);
  tft.print("FACTORY RESET");
  tft.setTextSize(1);
  tft.setCursor(60, 145);
  tft.print("in 5s - release to cancel");
}

// ===================== Splash screen =====================
void showSplash(bool restored) {
  tft.fillScreen(TFT_BLACK);

  // Заголовок
  tft.setTextSize(3);
  if (restored) {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setCursor(30, 25);
    tft.println("Restored");
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setCursor(30, 58);
    tft.println("from NVS");
  } else {
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.setCursor(30, 25);
    tft.println("Defaults");
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setCursor(30, 58);
    tft.println("loaded");
  }

  // Три частоты
  const uint16_t cols[3] = { TFT_GREEN, TFT_CYAN, TFT_YELLOW };
  tft.setTextSize(2);
  for (int i = 0; i < 3; i++) {
    uint16_t c = outputOn[i] ? cols[i] : TFT_DARKGREY;
    tft.setTextColor(c, TFT_BLACK);
    tft.setCursor(30, 105 + i * 27);
    char buf[32];
    sprintf(buf, "CLK%d: %9.4f MHz", i, freq[i] / 1.0e6);
    tft.print(buf);
  }

  // Подсказка внизу
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(20, 220);
  tft.print("Hold encoder 10s = factory reset");

  delay(1500);
}

// ===================== Si5351 =====================
void setSi5351Freq(long freqHz, uint8_t clk) {
  uint64_t freqCenti = (uint64_t)freqHz * 100ULL;
  si5351.set_freq(freqCenti, (enum si5351_clock)clk);
  si5351.drive_strength((enum si5351_clock)clk, SI5351_DRIVE_8MA);
  si5351.output_enable((enum si5351_clock)clk, outputOn[clk] ? 1 : 0);
}

void setOutputEnabled(uint8_t ch, bool on) {
  if (ch > 2) return;
  outputOn[ch] = on;
  si5351.output_enable((enum si5351_clock)ch, on ? 1 : 0);
  Serial.print("CLK"); Serial.print(ch);
  Serial.println(on ? " ON" : " OFF");
}

void toggleOutput(uint8_t ch) {
  setOutputEnabled(ch, !outputOn[ch]);
  needRedraw = true;
  markDirty();
}

// ===================== Канал / шаг =====================
void selectChannel(uint8_t ch) {
  if (ch > 2 || ch == activeCh) return;
  activeCh = ch;
  needRedraw = true;
  markDirty();
  Serial.print("Active: CLK"); Serial.println(activeCh);
}

void cycleStep() {
  if      (step == -100)        step = -1000;
  else if (step == -1000)       step = -100000;
  else if (step == -100000)     step = -1000000;
  else if (step == -1000000)    step = -10000000;
  else                          step = -100;
  drawStep();
  markDirty();
}

// ===================== Отрисовка главного экрана =====================
void drawHeader() {
  tft.fillRect(0, 0, 320, HEADER_H, TFT_BLACK);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(5, 6);
  tft.print("Si5351");

  tft.fillRect(255, 4, 60, 30, TFT_DARKGREEN);
  tft.drawRect(255, 4, 60, 30, TFT_LIGHTGREY);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.setTextSize(2);
  tft.setCursor(263, 10);
  tft.print("PRST");
}

void drawAll() {
  const uint16_t colorId[3] = { TFT_GREEN, TFT_CYAN, TFT_YELLOW };

  for (uint8_t i = 0; i < 3; i++) {
    float fMHz = freq[i] / 1.0e6;
    char buf[32];
    int y = 50 + i * 45;

    tft.fillRect(0, y, 320, 42, TFT_BLACK);

    uint16_t labelColor = outputOn[i] ? colorId[i] : TFT_DARKGREY;
    uint8_t  labelSize  = (i == activeCh) ? 3 : 2;
    const char* prefix  = (i == activeCh) ? ">" : " ";

    tft.setTextColor(labelColor, TFT_BLACK);
    tft.setTextSize(labelSize);
    tft.setCursor(10, y + 2);
    sprintf(buf, "%sCLK%d", prefix, i);
    tft.print(buf);

    uint16_t valueColor;
    if (i == activeCh) valueColor = outputOn[i] ? colorId[i] : TFT_DARKGREY;
    else                valueColor = outputOn[i] ? TFT_LIGHTGREY : TFT_DARKGREY;

    tft.setTextColor(valueColor, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(110, y + 8);
    sprintf(buf, "%.4f MHz", fMHz);
    tft.print(buf);
  }
}

void drawStep() {
  long stepAbs = -step;
  char buf[32];
  if (stepAbs < 1000)          sprintf(buf, "%ld Hz", stepAbs);
  else if (stepAbs < 1000000)  sprintf(buf, "%ld kHz", stepAbs / 1000);
  else                          sprintf(buf, "%ld MHz", stepAbs / 1000000);

  tft.fillRect(0, 195, 320, 45, TFT_BLACK);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 205);
  tft.print("Step: ");
  tft.print(buf);

  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setCursor(10, 228);
  tft.print("(tap here to change)");
}

// ===================== Клавиатура =====================
void drawKeypad() {
  tft.fillScreen(TFT_BLACK);

  tft.fillRect(0, 0, 320, HEADER_H, TFT_NAVY);
  tft.drawFastHLine(0, HEADER_H - 1, 320, TFT_LIGHTGREY);

  tft.setTextColor(TFT_CYAN, TFT_NAVY);
  tft.setTextSize(2);
  tft.setCursor(5, 12);
  tft.print("CLK"); tft.print(keyChannel);

  String display = keyInput.length() > 0 ? keyInput : "0";
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setTextSize(3);
  tft.setCursor(90, 8);
  tft.print(display);

  const char* labels[4][4] = {
    { "7",  "8", "9", "DEL" },
    { "4",  "5", "6", "Hz"  },
    { "1",  "2", "3", "kHz" },
    { "C",  "0", ".", "MHz" }
  };

  for (int row = 0; row < 4; row++) {
    for (int col = 0; col < 4; col++) {
      int x = col * KEYPAD_COL_W;
      int y = KEYPAD_TOP + row * KEYPAD_ROW_H;

      uint16_t bg = TFT_NAVY, fg = TFT_WHITE;
      if (row == 0 && col == 3)       { bg = TFT_MAROON; }
      else if (row == 3 && col == 0)  { bg = TFT_ORANGE; fg = TFT_BLACK; }
      else if (col == 3)               { bg = TFT_DARKGREEN; }

      tft.fillRect(x + 2, y + 2, KEYPAD_COL_W - 4, KEYPAD_ROW_H - 4, bg);
      tft.drawRect(x, y, KEYPAD_COL_W, KEYPAD_ROW_H, TFT_LIGHTGREY);

      String label = labels[row][col];
      uint8_t tsz = (label.length() == 1) ? 4 : 2;
      tft.setTextColor(fg, bg);
      tft.setTextSize(tsz);
      int textW = label.length() * 6 * tsz;
      int textH = 8 * tsz;
      tft.setCursor(x + (KEYPAD_COL_W - textW) / 2,
                    y + (KEYPAD_ROW_H - textH) / 2);
      tft.print(label);
    }
  }
}

// ===================== Экран пресетов =====================
void drawPresetsScreen() {
  tft.fillScreen(TFT_BLACK);

  tft.fillRect(0, 0, 320, HEADER_H, TFT_NAVY);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.print("Presets");

  tft.fillRect(245, 5, 70, 30, TFT_MAROON);
  tft.drawRect(245, 5, 70, 30, TFT_LIGHTGREY);
  tft.setTextColor(TFT_WHITE, TFT_MAROON);
  tft.setTextSize(2);
  tft.setCursor(255, 10);
  tft.print("BACK");

  for (int i = 0; i < NUM_PRESETS; i++) {
    int col = i % 4;
    int row = i / 4;
    drawPresetCell(i, col * CELL_W, CELLS_TOP + row * CELL_H);
  }

  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(5, 232);
  tft.print("Tap=load | Long press=save | Back=exit");
}

void drawPresetCell(int n, int x, int y) {
  uint16_t bg = presetValid[n] ? TFT_NAVY : TFT_BLACK;

  tft.fillRect(x + 2, y + 2, CELL_W - 4, CELL_H - 4, bg);
  tft.drawRect(x, y, CELL_W, CELL_H, TFT_LIGHTGREY);

  tft.setTextColor(presetValid[n] ? TFT_WHITE : TFT_DARKGREY, bg);
  tft.setTextSize(3);
  tft.setCursor(x + 5, y + 3);
  char num[4];
  sprintf(num, "%d", n + 1);
  tft.print(num);

  if (presetValid[n]) {
    tft.setTextColor(TFT_DARKGREY, bg);
    tft.setTextSize(1);
    tft.setCursor(x + 55, y + 10);
    tft.print("MHz");

    const uint16_t cols[3] = { TFT_GREEN, TFT_CYAN, TFT_YELLOW };
    for (int i = 0; i < 3; i++) {
      uint16_t c = presetOn[n][i] ? cols[i] : TFT_DARKGREY;
      tft.setTextColor(c, bg);
      tft.setTextSize(1);
      tft.setCursor(x + 5, y + 40 + i * 13);
      char buf[16];
      sprintf(buf, "%.3f", presetFreq[n][i] / 1.0e6);
      tft.print(buf);
    }
  } else {
    tft.setTextColor(TFT_DARKGREY, bg);
    tft.setTextSize(1);
    tft.setCursor(x + 5, y + 55);
    tft.print("empty");
    tft.setCursor(x + 5, y + 68);
    tft.print("(long=save)");
  }
}

// ===================== Пресеты =====================
void loadAllPresets() {
  prefs.begin("presets", true);
  for (int n = 0; n < NUM_PRESETS; n++) {
    char key[8];
    sprintf(key, "p%dv", n);
    presetValid[n] = prefs.getBool(key, false);
    for (int i = 0; i < 3; i++) {
      sprintf(key, "p%df%d", n, i);
      presetFreq[n][i] = prefs.getLong(key, 10000000);
      sprintf(key, "p%do%d", n, i);
      presetOn[n][i] = prefs.getBool(key, true);
    }
  }
  prefs.end();
}

void savePreset(uint8_t n) {
  if (n >= NUM_PRESETS) return;

  prefs.begin("presets", false);
  char key[8];
  sprintf(key, "p%dv", n); prefs.putBool(key, true);
  for (int i = 0; i < 3; i++) {
    sprintf(key, "p%df%d", n, i); prefs.putLong(key, freq[i]);
    sprintf(key, "p%do%d", n, i); prefs.putBool(key, outputOn[i]);
  }
  prefs.end();

  presetValid[n] = true;
  for (int i = 0; i < 3; i++) {
    presetFreq[n][i] = freq[i];
    presetOn[n][i] = outputOn[i];
  }

  int col = n % 4;
  int row = n / 4;
  drawPresetCell(n, col * CELL_W, CELLS_TOP + row * CELL_H);
  Serial.print("Saved preset "); Serial.println(n + 1);
}

void loadPreset(uint8_t n) {
  if (n >= NUM_PRESETS || !presetValid[n]) return;

  for (int i = 0; i < 3; i++) {
    freq[i] = presetFreq[n][i];
    outputOn[i] = presetOn[n][i];
    setSi5351Freq(freq[i], i);
  }
  markDirty();
  Serial.print("Loaded preset "); Serial.println(n + 1);
}

void openPresets() {
  screen = SCR_PRESETS;
  drawPresetsScreen();
}

void closePresets() {
  screen = SCR_MAIN;
  tft.fillScreen(TFT_BLACK);
  drawHeader();
  drawAll();
  drawStep();
}

// ===================== Клавиатура =====================
void openKeypad(uint8_t ch) {
  keyChannel = ch;
  activeCh = ch;
  needRedraw = true;
  keyUnit = 2;

  long f = freq[ch];
  char buf[16];
  if (f % 1000000 == 0)         sprintf(buf, "%ld", f / 1000000);
  else if (f % 1000 == 0)        sprintf(buf, "%ld.%03ld", f / 1000000, (f % 1000000) / 1000);
  else                            sprintf(buf, "%ld.%06ld", f / 1000000, f % 1000000);
  keyInput = String(buf);

  screen = SCR_KEYPAD;
  drawKeypad();
}

void closeKeypad(bool apply) {
  if (apply && keyInput.length() > 0) applyKeyInput();
  keyInput = "";

  screen = SCR_MAIN;
  tft.fillScreen(TFT_BLACK);
  drawHeader();
  drawAll();
  drawStep();
}

void applyKeyInput() {
  if (keyInput.length() == 0) return;

  long long integerPart = 0, decimalPart = 0;
  int decimalDigits = 0;
  bool afterDot = false;

  for (size_t i = 0; i < keyInput.length(); i++) {
    char c = keyInput.charAt(i);
    if (c == '.') { afterDot = true; continue; }
    if (c < '0' || c > '9') continue;

    if (!afterDot) {
      integerPart = integerPart * 10 + (c - '0');
      if (integerPart > 999999999LL) integerPart = 999999999LL;
    } else {
      if (decimalDigits < 6) {
        decimalPart = decimalPart * 10 + (c - '0');
        decimalDigits++;
      }
    }
  }

  long long multiplier = (keyUnit == 0) ? 1LL :
                         (keyUnit == 1) ? 1000LL : 1000000LL;
  long long result = integerPart * multiplier;

  long long decMult = 1;
  for (int i = 0; i < decimalDigits; i++) decMult *= 10;
  if (decMult > 0) result += (decimalPart * multiplier) / decMult;

  if (result < 10000LL)       result = 10000LL;
  if (result > 220000000LL)   result = 220000000LL;

  freq[keyChannel] = (long)result;
  setSi5351Freq((long)result, keyChannel);
  markDirty();
}

// ===================== Обработка касаний =====================
void handleKeypadTouch(int16_t x, int16_t y) {
  if (y < KEYPAD_TOP) return;
  int col = x / KEYPAD_COL_W;
  int row = (y - KEYPAD_TOP) / KEYPAD_ROW_H;
  if (col < 0 || col > 3 || row < 0 || row > 3) return;

  if (row == 0 && col == 3) {
    if (keyInput.length() > 0) keyInput.remove(keyInput.length() - 1);
    drawKeypad(); return;
  }
  if (row == 3 && col == 0) { keyInput = ""; drawKeypad(); return; }
  if (row == 1 && col == 3) { keyUnit = 0; closeKeypad(true); return; }
  if (row == 2 && col == 3) { keyUnit = 1; closeKeypad(true); return; }
  if (row == 3 && col == 3) { keyUnit = 2; closeKeypad(true); return; }

  String digit;
  if      (row == 0 && col < 3) digit = String(col + 7);
  else if (row == 1 && col < 3) digit = String(col + 4);
  else if (row == 2 && col < 3) digit = String(col + 1);
  else if (row == 3 && col == 1) digit = "0";
  else if (row == 3 && col == 2) digit = ".";

  if (digit.length() > 0 && keyInput.length() < 9) {
    if (!(digit == "." && keyInput.indexOf('.') >= 0)) keyInput += digit;
  }
  drawKeypad();
}

void handleMainTouch(int16_t x, int16_t y) {
  if (y < 40 && x >= 250) { openPresets(); return; }

  if (y >= 45 && y < 92) {
    if (x < 90) toggleOutput(0);
    else        openKeypad(0);
  }
  else if (y >= 92 && y < 138) {
    if (x < 90) toggleOutput(1);
    else        openKeypad(1);
  }
  else if (y >= 138 && y < 185) {
    if (x < 90) toggleOutput(2);
    else        openKeypad(2);
  }
  else if (y >= 190 && y < 240) {
    cycleStep();
  }
}

// ===================== SETUP =====================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== BOOT ===");

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  tft.init();
  tft.setRotation(1);
  tft.invertDisplay(false);
  delay(80);
  tft.fillScreen(TFT_BLACK);

  touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchscreenSPI);
  ts.setRotation(1);

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);

  pinMode(PIN_ENC_CLK, INPUT);
  pinMode(PIN_ENC_DT, INPUT);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);

  // --- Загрузка состояния ---
  bool nvsEmpty = false;
  loadLastState(nvsEmpty);
  loadAllPresets();

  // --- Si5351 ---
  si5351.init(SI5351_CRYSTAL_LOAD_8PF, 25000000, 0);
  for (uint8_t i = 0; i < 3; i++) setSi5351Freq(freq[i], i);

  // --- Сплэш-экран 1.5 сек ---
  showSplash(!nvsEmpty);

  encLast = (digitalRead(PIN_ENC_CLK) << 1) | digitalRead(PIN_ENC_DT);

  // --- Главный экран ---
  tft.fillScreen(TFT_BLACK);
  drawHeader();
  drawAll();
  drawStep();

  Serial.println("=== READY ===");
}

// ===================== LOOP =====================
void loop() {
  // ============ ЭНКОДЕР ============
  if (screen == SCR_MAIN) {
    uint8_t encCur = (digitalRead(PIN_ENC_CLK) << 1) | digitalRead(PIN_ENC_DT);
    if (encCur != encLast) {
      uint8_t transition = (encLast << 2) | encCur;
      int8_t dir = encTable[transition];
      encAccum += dir;
      encLast = encCur;

      if (abs(encAccum) >= 4) {
        int clicks = encAccum / 4;
        encAccum = encAccum % 4;

        freq[activeCh] += (long)clicks * step;
        if (freq[activeCh] < 10000)     freq[activeCh] = 10000;
        if (freq[activeCh] > 220000000) freq[activeCh] = 220000000;

        setSi5351Freq(freq[activeCh], activeCh);
        needRedraw = true;
        markDirty();
      }
    }
  } else {
    encLast = (digitalRead(PIN_ENC_CLK) << 1) | digitalRead(PIN_ENC_DT);
  }

  // ============ КНОПКА ЭНКОДЕРА (с factory reset) ============
  bool btnNow = (digitalRead(PIN_ENC_SW) == LOW);

  // Фронт нажатия
  if (btnNow && !btnWasPressed) {
    btnPressTime = millis();
    longPressHandled = false;
    factoryWarningShown = false;
    factoryResetFired = false;
  }

  // Обработка удержания
  if (btnNow && !factoryResetFired) {
    unsigned long held = millis() - btnPressTime;

    // 2 сек — обычное долгое нажатие
    if (!longPressHandled && held >= LONG_PRESS_MS) {
      longPressHandled = true;
      if (screen == SCR_KEYPAD) {
        closeKeypad(false);
      } else if (screen == SCR_PRESETS) {
        closePresets();
      } else {
        activeCh = (activeCh + 1) % 3;
        needRedraw = true;
        markDirty();
      }
    }

    // 5 сек — предупреждение
    if (!factoryWarningShown && held >= FACTORY_WARN_MS) {
      factoryWarningShown = true;
      showFactoryWarning();
    }

    // 10 сек — factory reset
    if (held >= FACTORY_RESET_MS) {
      factoryResetFired = true;
      doFactoryReset();   // не возвращается — уходит в ESP.restart()
    }
  }

  // Отпускание
  if (!btnNow && btnWasPressed) {
    if (!longPressHandled) {
      delay(50);
      if (digitalRead(PIN_ENC_SW) == HIGH) {
        if (screen == SCR_KEYPAD)       closeKeypad(false);
        else if (screen == SCR_PRESETS) closePresets();
        else                             cycleStep();
      }
    }
    // Если было предупреждение, но reset не сработал — восстановить экран
    if (factoryWarningShown && !factoryResetFired) {
      if (screen == SCR_MAIN) {
        drawHeader();
        drawAll();
        drawStep();
      } else if (screen == SCR_PRESETS) {
        drawPresetsScreen();
      } else if (screen == SCR_KEYPAD) {
        drawKeypad();
      }
    }
  }
  btnWasPressed = btnNow;

  // ============ ТАЧСКРИН ============
  bool touched = ts.touched();
  int16_t tx = 0, ty = 0;
  if (touched) {
    TS_Point p = ts.getPoint();
    tx = map(p.x, 200, 3745, 5, 315);
    ty = map(p.y, 373, 3831, 5, 235);
    tx = constrain(tx, 0, 319);
    ty = constrain(ty, 0, 239);
  }

  if (screen == SCR_PRESETS) {
    if (touched) {
      if (!touchWasPressed) {
        touchWasPressed = true;
        touchStartTime = millis();
        touchLongFired = false;
        touchSlot = -1;

        if (ty < 40 && tx >= 240) {
          touchSlot = -2;
        } else if (ty >= CELLS_TOP && ty < CELLS_TOP + 2 * CELL_H) {
          int col = tx / CELL_W;
          int row = (ty - CELLS_TOP) / CELL_H;
          if (col >= 0 && col < 4 && row >= 0 && row < 2) {
            touchSlot = row * 4 + col;
          }
        }
      } else {
        if (!touchLongFired && touchSlot >= 0 &&
            (millis() - touchStartTime > TOUCH_LONG_MS)) {
          touchLongFired = true;
          savePreset(touchSlot);
        }
      }
    } else {
      if (touchWasPressed) {
        if (!touchLongFired) {
          if (touchSlot >= 0) {
            loadPreset(touchSlot);
            closePresets();
          } else if (touchSlot == -2) {
            closePresets();
          }
        }
        touchWasPressed = false;
        touchSlot = -1;
      }
    }
  } else {
    if (touched) {
      if (!touchWasPressed && (millis() - lastTouchTime > TOUCH_DEBOUNCE_MS)) {
        lastTouchTime = millis();
        touchWasPressed = true;

        if (screen == SCR_KEYPAD)     handleKeypadTouch(tx, ty);
        else                           handleMainTouch(tx, ty);
      }
    } else {
      touchWasPressed = false;
    }
  }

  // ============ АВТОСОХРАНЕНИЕ ============
  if (stateDirty && (millis() - stateDirtyTime > AUTOSAVE_DELAY_MS)) {
    saveLastState();
  }

  // ============ ПЕРЕРИСОВКА ============
  if (needRedraw && screen == SCR_MAIN) {
    needRedraw = false;
    drawAll();
  }

  delay(5);
}