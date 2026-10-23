#include "indicators.h"

#include <Arduino.h>
#include <U8g2lib.h>

#include "config.h"

namespace {
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, cfg::PIN_OLED_SCL, cfg::PIN_OLED_SDA);
constexpr uint32_t DISPLAY_PERIOD_MS = 200;
}  // namespace

void StatusLed::write(int pin, bool on) { digitalWrite(pin, on ? LOW : HIGH); }

void StatusLed::begin() {
  // GPIO0 і GPIO2 — strapping піни: спершу вимикаємо LED (HIGH), лише потім робимо їх виходами
  digitalWrite(cfg::PIN_LED_RED, HIGH);
  digitalWrite(cfg::PIN_LED_YELLOW, HIGH);
  pinMode(cfg::PIN_LED_RED, OUTPUT);
  pinMode(cfg::PIN_LED_YELLOW, OUTPUT);
}

void StatusLed::update(State s, bool explosionOutput, bool testMode, bool fireIgnored, uint32_t now) {
  write(cfg::PIN_LED_RED, explosionOutput);  // єдине місце, де вмикається червоний

  bool yellow = false;
  switch (s) {
    case State::ARMING: yellow = (now / 500) % 2 == 0; break;
    case State::READY: yellow = !fireIgnored || now % 400 >= 100; break;
    case State::FAULT: yellow = (now / 100) % 2 == 0; break;
    case State::SAFE: yellow = testMode && now % 1000 < 80; break;
    case State::EXPLOSION: {  // сюди з вимкненим виходом потрапляємо лише в тест-режимі
      const uint32_t ph = now % 1000;
      yellow = testMode && (ph < 100 || (ph >= 200 && ph < 300));
      break;
    }
    default: break;
  }
  write(cfg::PIN_LED_YELLOW, yellow);
}

bool StatusDisplay::begin() {
  if (!oled.begin()) return false;
  oled.setBusClock(400000);  // 100 кГц блокує цикл ~100 мс на кожне перемальовування екрана
  return true;
}

void StatusDisplay::update(const Snapshot& s, const char* ip, uint32_t now) {
  if (now - lastMs_ < DISPLAY_PERIOD_MS) return;
  lastMs_ = now;

  char line[28];
  oled.clearBuffer();
  oled.setFont(u8g2_font_logisoso16_tr);
  oled.drawStr(0, 18, s.state == State::EXPLOSION ? "EXPLOSION" : stateName(s.state));

  oled.setFont(u8g2_font_6x10_tf);
  switch (s.state) {
    case State::ARMING: snprintf(line, sizeof(line), "Ready in %lus", (unsigned long)((s.armRemainingMs + 999) / 1000)); break;
    case State::READY:
      if (s.fireIgnored) snprintf(line, sizeof(line), "FIRE pre-arm: ignored");
      else if (s.fireConfirmedMs) snprintf(line, sizeof(line), "FIRE confirm %lums", (unsigned long)s.fireConfirmedMs);
      else snprintf(line, sizeof(line), "Armed, waiting");
      break;
    case State::EXPLOSION:
      snprintf(line, sizeof(line), s.testMode ? "TEST: output OFF" : "SIMULATION, Stop=reset");
      break;
    case State::FAULT: snprintf(line, sizeof(line), "%.20s", s.fault); break;
    default: snprintf(line, sizeof(line), s.testMode ? "TEST MODE, disarmed" : "Disarmed"); break;
  }
  oled.drawStr(0, 32, line);

  if (s.duty >= 0) snprintf(line, sizeof(line), "SNS:%s d%d%%", sensorStatusName(s.sensor), s.duty);
  else snprintf(line, sizeof(line), "SNS:%s", sensorStatusName(s.sensor));
  oled.drawStr(0, 44, line);

  oled.setFont(u8g2_font_5x8_tf);  // 25 символів: "5V:5.34V 192.168.100.123"
  snprintf(line, sizeof(line), "5V:%lu.%02luV%s %s", (unsigned long)(s.vbusMv / 1000), (unsigned long)((s.vbusMv % 1000) / 10),
           s.vbusOk ? "" : "!", ip);
  oled.drawStr(0, 63, line);
  oled.sendBuffer();
}
