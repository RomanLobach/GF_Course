// Renders the 128x64 monochrome OLED screens - main screen / menu / confirm
// dialog / popup / Wi-Fi.
// Runs entirely inside the UI task (its own core); never touched from the
// radio/protocol core.
//
// Uses U8g2 (not Adafruit_GFX/Adafruit_SSD1306): all menu/popup text is
// Ukrainian (UTF-8 Cyrillic), and Adafruit_GFX's built-in font only covers
// ASCII/CP437 - U8g2's Cyrillic bitmap fonts plus its UTF-8-aware print()
// render it correctly.
#pragma once

#include <U8g2lib.h>
#include "Config.h"
#include "UiTypes.h"

class Display {
public:
  bool begin();
  void render(const DisplaySnapshot &snap);

private:
  U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled_{U8G2_R0, U8X8_PIN_NONE};

  void renderStatusBar(const DisplaySnapshot &snap);
  void renderMain(const DisplaySnapshot &snap);
  void renderMenu(const DisplaySnapshot &snap);
  void renderConfirm(const DisplaySnapshot &snap);
  void renderPopup(const char *text);
  void renderWifi(const DisplaySnapshot &snap);

  void drawLeft(int16_t x, int16_t y, const char *text);
  void drawCentered(int16_t xStart, int16_t xEnd, int16_t y, const char *text);
  void drawRight(int16_t xEnd, int16_t y, const char *text);
};
