#include "Display.h"
#include <Wire.h>

namespace {
const char *stateCode(const DisplaySnapshot &snap) {
  if (snap.wifi) return "WIFI";
  switch (snap.state) {
    case Protocol::DeviceState::Idle: return "IDLE";
    case Protocol::DeviceState::Searching:
    case Protocol::DeviceState::SeenYou: return "SRCH"; // both search sub-steps show as SRCH
    case Protocol::DeviceState::Synced: return "SYNC";
    case Protocol::DeviceState::Measuring: return "MEAS";
    case Protocol::DeviceState::Testing: return "TEST";
  }
  return "????";
}

// Profile label, e.g. "SF6/125" or "FSK15k".
void formatProfileLabel(const uint8_t profile, char *out, const size_t outLen) {
  if (profile < 1 || profile > Config::BENCH_CONFIG_COUNT) {
    snprintf(out, outLen, "-");
    return;
  }
  const Config::BenchConfig &cfg = Config::BENCH_CONFIGS[profile - 1];
  if (cfg.modulation == Config::Modulation::LoRaMod) {
    snprintf(out, outLen, "SF%u/%u", static_cast<unsigned>(cfg.sf), static_cast<unsigned>(cfg.bwKhz));
  } else {
    snprintf(out, outLen, "FSK%uk", static_cast<unsigned>(cfg.bitrate / 1000));
  }
}

const char *const MAIN_LABELS[MenuItems::MAIN_COUNT] = {
  "Синхронізація", "Вимірювання", "Тест частот", "Сервіс", "Вийти",
};
const char *const SERVICE_LABELS[MenuItems::SERVICE_COUNT] = {
  "Передати по Wi-Fi", "Оновити прошивку", "Стерти логи", "Назад",
};

const char *suffixText(const MenuItems::Suffix s) {
  switch (s) {
    case MenuItems::Suffix::Start: return " [почати]";
    case MenuItems::Suffix::Finish: return " [завершити]";
    case MenuItems::Suffix::Stop: return " [зупинити]";
    case MenuItems::Suffix::Unavailable: return " [X]";
    case MenuItems::Suffix::Submenu: return " >";
    case MenuItems::Suffix::None: return "";
  }
  return "";
}

// Blinking at 2 Hz, driven by the display task's own clock (it renders at least every 100 ms).
bool blinkOn() { return (millis() / 250) % 2 == 0; }
} // namespace

bool Display::begin() {
  Wire.begin(Config::PIN_OLED_SDA, Config::PIN_OLED_SCL, Config::OLED_I2C_CLOCK_HZ);
  oled_.setI2CAddress(Config::OLED_I2C_ADDR << 1); // u8g2 wants the 8-bit address
  if (!oled_.begin()) return false;

  // u8g2_font_5x7_t_cyrillic covers ASCII 32-128 plus Cyrillic U+0400-U+052F
  // (includes the Ukrainian-specific letters і/ї/є/ґ used in the menu text)
  // at roughly the same cell size as the old Adafruit_GFX default font, so
  // the row-height layout below (rowH = 9 etc.) still fits six menu rows.
  oled_.setFont(u8g2_font_5x7_t_cyrillic);
  oled_.setFontPosTop(); // y = top of glyph, matching the old GFX cursor convention
  oled_.setFontMode(1);  // transparent: only glyph strokes are drawn
  oled_.enableUTF8Print(); // without this, print() treats UTF-8 Cyrillic as raw bytes
  oled_.clearBuffer();
  oled_.sendBuffer();
  return true;
}

void Display::drawLeft(const int16_t x, const int16_t y, const char *text) {
  oled_.setCursor(x, y);
  oled_.print(text);
}

void Display::drawCentered(const int16_t xStart, const int16_t xEnd, const int16_t y, const char *text) {
  const uint16_t w = oled_.getUTF8Width(text);
  const auto x = static_cast<int16_t>(xStart + (xEnd - xStart - static_cast<int16_t>(w)) / 2);
  oled_.setCursor(x, y);
  oled_.print(text);
}

void Display::drawRight(const int16_t xEnd, const int16_t y, const char *text) {
  const uint16_t w = oled_.getUTF8Width(text);
  oled_.setCursor(xEnd - static_cast<int16_t>(w), y);
  oled_.print(text);
}

void Display::renderStatusBar(const DisplaySnapshot &snap) {
  const char *code = stateCode(snap);
  drawLeft(0, 3, code);
  if (snap.pending && blinkOn()) {
    const int16_t x = static_cast<int16_t>(oled_.getUTF8Width(code) + 2);
    oled_.drawBox(x, 7, 2, 2);
  }

  char idBuf[8];
  snprintf(idBuf, sizeof(idBuf), "%04u", static_cast<unsigned>(snap.sessionId % 10000));
  const uint32_t sec = snap.sessionElapsedMs / 1000;
  unsigned minutes = sec / 60;
  if (minutes > 99) minutes = 99;
  char timeBuf[8];
  snprintf(timeBuf, sizeof(timeBuf), "%02u:%02u", minutes, static_cast<unsigned>(sec % 60));

  constexpr int16_t seg = Config::OLED_WIDTH / 3;
  drawCentered(seg, seg * 2, 3, idBuf);
  drawRight(Config::OLED_WIDTH, 3, timeBuf);
}

void Display::renderMain(const DisplaySnapshot &snap) {
  renderStatusBar(snap);
  constexpr int16_t l = 1;
  constexpr int16_t r = Config::OLED_WIDTH - 1;
  char buf[40];
  char label[14];

  if (snap.state == Protocol::DeviceState::Measuring) {
    formatProfileLabel(snap.runProfile, label, sizeof(label));
    snprintf(buf, sizeof(buf), "Профіль %u/6 %s", static_cast<unsigned>(snap.runProfile), label);
    drawCentered(l, r, 20, buf);
    if (snap.isBase) {
      snprintf(buf, sizeof(buf), "Надіслано %u/%u", static_cast<unsigned>(snap.burstTx), static_cast<unsigned>(snap.burstSize));
    } else {
      snprintf(buf, sizeof(buf), "Прийнято %u/%u", static_cast<unsigned>(snap.burstRx), static_cast<unsigned>(snap.burstSize));
    }
    drawCentered(l, r, 32, buf);
    if (!snap.isBase && snap.runHasSignal) {
      snprintf(buf, sizeof(buf), "RSSI %d SNR %.1f", static_cast<int>(lroundf(snap.runRssi)), snap.runSnr);
      drawCentered(l, r, 44, buf);
    }
    return;
  }

  if (snap.state == Protocol::DeviceState::Testing) {
    // While a switch is pending, show the profile the operator picked (with the blinking dot).
    const uint8_t shown = snap.runDesiredProfile != 0 ? snap.runDesiredProfile : snap.runProfile;
    formatProfileLabel(shown, label, sizeof(label));
    snprintf(buf, sizeof(buf), "Профіль: %s", label);
    drawCentered(l, r, 18, buf);
    if (snap.testPaused) {
      drawCentered(l, r, 30, "Пауза: ліміт ефіру");
    } else if (snap.lastRecv == 0xFF) {
      drawCentered(l, r, 30, "Прийнято: -");
    } else {
      const unsigned sent = snap.lastSent != 0 ? snap.lastSent : snap.burstSize;
      snprintf(buf, sizeof(buf), "Прийнято %u/%u", static_cast<unsigned>(snap.lastRecv), sent);
      drawCentered(l, r, 30, buf);
    }
    if (!snap.isBase && snap.runHasSignal) {
      snprintf(buf, sizeof(buf), "RSSI %d SNR %.1f", static_cast<int>(lroundf(snap.runRssi)), snap.runSnr);
      drawCentered(l, r, 42, buf);
    }
    if (snap.logFull) drawCentered(l, r, 54, "лог повний");
    return;
  }

  snprintf(buf, sizeof(buf), "RSSI: %ddBm", static_cast<int>(lroundf(snap.rssi)));
  drawCentered(l, r, 24, buf);
  snprintf(buf, sizeof(buf), "SNR: %.1fdB", snap.snr);
  drawCentered(l, r, 36, buf);
}

void Display::renderMenu(const DisplaySnapshot &snap) {
  constexpr int rowH = 9;
  const int totalH = rowH * snap.menuCount;
  const int y0 = (Config::OLED_HEIGHT - totalH) / 2;
  const bool main = snap.menuPage == MenuItems::Page::Main;

  for (uint8_t i = 0; i < snap.menuCount; i++) {
    const int y = y0 + i * rowH;
    String label(main ? MAIN_LABELS[i] : SERVICE_LABELS[i]);
    label += suffixText(snap.menuSuffix[i]);

    oled_.setDrawColor(1);
    if (i == snap.menuIndex) {
      oled_.drawBox(0, y - 1, Config::OLED_WIDTH, rowH);
      oled_.setDrawColor(0);
    }
    oled_.setCursor(1, y);
    oled_.print(label);
  }
  oled_.setDrawColor(1);
}

void Display::renderConfirm(const DisplaySnapshot &snap) {
  drawCentered(0, Config::OLED_WIDTH, 18, "Дійсно стерти?");

  for (int i = 0; i < 2; i++) {
    const char *labels[2] = {"Ні", "Так"};
    constexpr int16_t xs[2] = {40, 76};
    const uint16_t w = oled_.getUTF8Width(labels[i]);
    if (i == snap.confirmIndex) {
      oled_.setDrawColor(1);
      oled_.drawBox(xs[i] - 2, 34, w + 4, 10);
      oled_.setDrawColor(0);
    } else {
      oled_.setDrawColor(1);
    }
    oled_.setCursor(xs[i], 36);
    oled_.print(labels[i]);
  }
  oled_.setDrawColor(1);
}

void Display::renderPopup(const char *text) {
  // Word-wrap by rendered pixel width (UTF-8 Cyrillic is 2 bytes/glyph, so byte counts lie).
  constexpr int maxLines = 4;
  constexpr int16_t maxWidth = Config::OLED_WIDTH - 4;
  String lines[maxLines];
  int lineCount = 0;
  String current;
  const String s(text);
  int pos = 0;
  while (pos < static_cast<int>(s.length()) && lineCount < maxLines) {
    int end = s.indexOf(' ', pos);
    if (end < 0) end = s.length();
    const String word = s.substring(pos, end);
    const String candidate = current.length() ? current + " " + word : word;
    if (current.length() && oled_.getUTF8Width(candidate.c_str()) > maxWidth) {
      lines[lineCount++] = current;
      current = word;
    } else {
      current = candidate;
    }
    pos = end + 1;
  }
  if (current.length() && lineCount < maxLines) lines[lineCount++] = current;

  const int totalH = lineCount * 10;
  const int y0 = (Config::OLED_HEIGHT - totalH) / 2;
  for (int i = 0; i < lineCount; i++) {
    drawCentered(2, Config::OLED_WIDTH - 2, static_cast<int16_t>(y0 + i * 10), lines[i].c_str());
  }
}

void Display::renderWifi(const DisplaySnapshot &snap) {
  renderStatusBar(snap);
  const String s(snap.text);
  const int nl = s.indexOf('\n');
  if (nl < 0) {
    drawCentered(2, Config::OLED_WIDTH - 2, 28, snap.text);
  } else {
    drawCentered(2, Config::OLED_WIDTH - 2, 24, s.substring(0, nl).c_str());
    drawCentered(2, Config::OLED_WIDTH - 2, 38, s.substring(nl + 1).c_str());
  }
}

void Display::renderOta(const DisplaySnapshot &snap) {
  renderWifi(snap); // same two text lines
  if (snap.otaPercent < 0) return;
  constexpr int x = 14, y = 50, w = Config::OLED_WIDTH - 2 * x, h = 6;
  oled_.drawFrame(x, y, w, h);
  oled_.drawBox(x + 1, y + 1, (w - 2) * snap.otaPercent / 100, h - 2);
}

void Display::render(const DisplaySnapshot &snap) {
  oled_.clearBuffer();
  switch (snap.screen) {
    case AppScreen::Main: renderMain(snap); break;
    case AppScreen::Menu: renderMenu(snap); break;
    case AppScreen::ConfirmDelete: renderConfirm(snap); break;
    case AppScreen::Popup: renderPopup(snap.text); break;
    case AppScreen::WifiScreen: renderWifi(snap); break;
    case AppScreen::OtaScreen: renderOta(snap); break;
  }
  oled_.sendBuffer();
}
