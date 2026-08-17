#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include "img_none.h"
#include "img_0.h"
#include "img_1.h"
#include "img_2.h"
#include "img_3.h"
#include "img_4.h"
#include "img_5.h"
#include "img_6.h"
#include "img_7.h"
#include "img_8.h"
#include "img_9.h"

// =====================================================
// ESP32-C3 SUPER MINI
// ST7789 170x320 + TENSTAR EC11 + DS3231
//
// NO RTClib
// NO TJpg_Decoder
// NO extra image libraries
//
// Images are stored as 64-color, 6-bit indexed RGB565.
// Each 170x320 image is ~41 KB instead of ~109 KB raw.
// =====================================================

#define TFT_CS    10
#define TFT_DC     7
#define TFT_RST    1
#define TFT_SCLK   4
#define TFT_MOSI   6

#define ENC_S1     2
#define ENC_S2     3
#define ENC_KEY    5

#define RTC_SDA    8
#define RTC_SCL    9
#define DS3231_ADDR 0x68

#define W 170
#define H 320

Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// One scanline in RAM.
uint16_t lineBuffer[W];

enum Mode {
  RUN_MODE,
  SET_HOURS,
  SET_MINUTES
};

Mode mode = RUN_MODE;

int setHours = 12;
int setMinutes = 0;


unsigned long lastFrame = 0;
unsigned long lastKeyTime = 0;
bool keyPressed = false;

// Display timing state
const unsigned long DIGIT_TIME = 550;
const unsigned long HOUR_MINUTE_PAUSE = 700;
const unsigned long MINUTE_HOUR_PAUSE = 1300;

uint8_t displayStep = 0;
unsigned long lastDisplayStep = 0;
bool displayStarted = false;

uint8_t settingStep = 0;
unsigned long lastSettingStep = 0;


// -----------------------------------------------------
// Image descriptor
// -----------------------------------------------------

struct ImageRef {
  const uint16_t* palette;
  const uint8_t* data;
  uint32_t size;
};

ImageRef getImage(uint8_t n) {
  switch(n) {
    case 0: return {img_0_pal, img_0_idx, img_0_idx_size};
    case 1: return {img_1_pal, img_1_idx, img_1_idx_size};
    case 2: return {img_2_pal, img_2_idx, img_2_idx_size};
    case 3: return {img_3_pal, img_3_idx, img_3_idx_size};
    case 4: return {img_4_pal, img_4_idx, img_4_idx_size};
    case 5: return {img_5_pal, img_5_idx, img_5_idx_size};
    case 6: return {img_6_pal, img_6_idx, img_6_idx_size};
    case 7: return {img_7_pal, img_7_idx, img_7_idx_size};
    case 8: return {img_8_pal, img_8_idx, img_8_idx_size};
    case 9: return {img_9_pal, img_9_idx, img_9_idx_size};
    default: return {img_none_pal, img_none_idx, img_none_idx_size};
  }
}

// -----------------------------------------------------
// Draw packed 6-bit image
// -----------------------------------------------------

void showImage(uint8_t n) {
  ImageRef img = getImage(n);

  uint32_t bytePos = 0;
  uint32_t bitBuffer = 0;
  uint8_t bits = 0;

  int x = 0;
  int y = 0;

  while(y < H) {
    while(bits < 6 && bytePos < img.size) {
      bitBuffer |= ((uint32_t)pgm_read_byte(img.data + bytePos++)) << bits;
      bits += 8;
    }

    uint8_t idx = bitBuffer & 0x3F;
    bitBuffer >>= 6;
    bits -= 6;

    lineBuffer[x++] = pgm_read_word(img.palette + idx);

    if(x >= W) {
      tft.drawRGBBitmap(0, y, lineBuffer, W, 1);
      x = 0;
      y++;
    }
  }
}

// -----------------------------------------------------
// DS3231 direct I2C
// -----------------------------------------------------

uint8_t bcdToDec(uint8_t v) {
  return ((v >> 4) * 10) + (v & 0x0F);
}

uint8_t decToBcd(uint8_t v) {
  return ((v / 10) << 4) | (v % 10);
}

bool ds3231Present() {
  Wire.beginTransmission(DS3231_ADDR);
  return Wire.endTransmission() == 0;
}

void ds3231ReadTime(uint8_t &h, uint8_t &m, uint8_t &s) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x00);
  Wire.endTransmission(false);
  Wire.requestFrom(DS3231_ADDR, (uint8_t)3);

  if(Wire.available() >= 3) {
    s = bcdToDec(Wire.read() & 0x7F);
    m = bcdToDec(Wire.read() & 0x7F);
    h = bcdToDec(Wire.read() & 0x3F);
  }
}

void ds3231SetTime(uint8_t h, uint8_t m) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x00);
  Wire.write(0x00);
  Wire.write(decToBcd(m));
  Wire.write(decToBcd(h));
  Wire.endTransmission();
}

// -----------------------------------------------------
// Encoder
// Your tested EC11:
// CW  = 11 -> 01
// CCW = 11 -> 10
// -----------------------------------------------------

uint8_t previousState = 0;

uint8_t readEncoderState() {
  int s1 = analogRead(ENC_S1);
  int s2 = analogRead(ENC_S2);

  uint8_t a = (s1 < 3000) ? 0 : 1;
  uint8_t b = (s2 < 1800) ? 0 : 1;

  return (a << 1) | b;
}

void changeValue(int delta) {
  if(mode == SET_HOURS) {
    setHours += delta;
    if(setHours > 23) setHours = 0;
    if(setHours < 0)  setHours = 23;
  } else if(mode == SET_MINUTES) {
    setMinutes += delta;
    if(setMinutes > 59) setMinutes = 0;
    if(setMinutes < 0)  setMinutes = 59;
  }
}

void readEncoder() {
  uint8_t state = readEncoderState();

  if(state != previousState) {
    if(previousState == 3 && state == 1)
      changeValue(+1);
    else if(previousState == 3 && state == 2)
      changeValue(-1);

    previousState = state;
  }
}

// -----------------------------------------------------
// Button
// -----------------------------------------------------

void enterHourSetup()
{
  uint8_t h, m, s;
  ds3231ReadTime(h, m, s);

  setHours = h;
  setMinutes = m;

  mode = SET_HOURS;
  settingStep = 0;
  lastSettingStep = millis();
  lastFrame = millis();
}

void enterMinuteSetup()
{
  mode = SET_MINUTES;
  settingStep = 0;
  lastSettingStep = millis();
  lastFrame = millis();
}

void saveAndRun()
{
  ds3231SetTime(setHours, setMinutes);

  mode = RUN_MODE;
  displayStep = 0;
  displayStarted = false;
  lastDisplayStep = millis();
  lastFrame = millis();
}

void readButton() {
  int key = digitalRead(ENC_KEY);

  if(key == LOW && !keyPressed) {
    if(millis() - lastKeyTime > 250) {
      lastKeyTime = millis();
      keyPressed = true;

      if(mode == RUN_MODE)
        enterHourSetup();
      else if(mode == SET_HOURS)
        enterMinuteSetup();
      else
        saveAndRun();
    }
  }

  if(key == HIGH)
    keyPressed = false;
}

// -----------------------------------------------------
// Normal mode
//
// Example 15:31:
// 1 -> 5 -> BLANK 1.4s -> 3 -> 1 -> BLANK 2.6s -> repeat
//
// The long pauses belong to the blank lamp, so the last
// minute digit stays on screen only for DIGIT_TIME.
// -----------------------------------------------------

unsigned long durationForStep(uint8_t step)
{
  if (step == 2) return HOUR_MINUTE_PAUSE;
  if (step == 5) return MINUTE_HOUR_PAUSE;
  return DIGIT_TIME;
}

void displayRunMode()
{
  uint8_t h, m, s;
  ds3231ReadTime(h, m, s);

  if (!displayStarted)
  {
    displayStep = 0;
    showImage(h / 10);
    lastDisplayStep = millis();
    displayStarted = true;
    return;
  }

  if (millis() - lastDisplayStep < durationForStep(displayStep))
    return;

  lastDisplayStep = millis();

  displayStep++;
  if (displayStep >= 6)
    displayStep = 0;

  switch (displayStep)
  {
    case 0: showImage(h / 10); break;
    case 1: showImage(h % 10); break;
    case 2: showImage(10); break;
    case 3: showImage(m / 10); break;
    case 4: showImage(m % 10); break;
    case 5: showImage(10); break;
  }
}

// -----------------------------------------------------
// Setting mode
//
// SET HOURS 15:
// 1 -> 5 -> BLANK -> repeat
//
// SET MINUTES 31:
// 3 -> 1 -> BLANK -> repeat
// -----------------------------------------------------

void displaySetMode()
{
  unsigned long interval = (settingStep == 2) ? 900 : 550;

  if (millis() - lastSettingStep < interval)
    return;

  lastSettingStep = millis();

  if (mode == SET_HOURS)
  {
    if (settingStep == 0) showImage(setHours / 10);
    else if (settingStep == 1) showImage(setHours % 10);
    else showImage(10);
  }
  else
  {
    if (settingStep == 0) showImage(setMinutes / 10);
    else if (settingStep == 1) showImage(setMinutes % 10);
    else showImage(10);
  }

  settingStep++;
  if (settingStep >= 3)
    settingStep = 0;
}

// -----------------------------------------------------
// Setup
// -----------------------------------------------------

void setup() {
  pinMode(ENC_S1, INPUT);
  pinMode(ENC_S2, INPUT);
  pinMode(ENC_KEY, INPUT_PULLUP);

  analogReadResolution(12);

  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);

  tft.init(170, 320);
  tft.setRotation(0);

  Wire.begin(RTC_SDA, RTC_SCL);

  previousState = readEncoderState();

  if(!ds3231Present()) {
    showImage(10);
    while(true) delay(100);
  }

  showImage(10);
  lastFrame = millis();
}

// -----------------------------------------------------
// Loop
// -----------------------------------------------------

void loop() {
  readEncoder();
  readButton();

  if(mode == RUN_MODE)
    displayRunMode();
  else
    displaySetMode();

  delay(1);
}
