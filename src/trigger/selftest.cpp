#include "selftest.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Wire.h>

#include "config.h"
#include "trigger_fsm.h"

namespace {
void report(const char* name, bool ok, const char* extra = "") {
  Serial.printf("[selftest] %-12s %s %s\n", name, ok ? "OK  " : "FAIL", extra);
}
}  // namespace

SelfTestResult runSelfTest() {
  SelfTestResult r{CF_NONE, false, 0};

  const uint64_t id = ESP.getEfuseMac();
  char idStr[24];
  snprintf(idStr, sizeof(idStr), "%04X%08X", (uint16_t)(id >> 32), (uint32_t)id);
  const bool idOk = id != 0 && id != 0xFFFFFFFFFFFFULL;
  report("chip id", idOk, idStr);
  if (!idOk) r.critical |= CF_CHIP_ID;

  const bool fsOk = LittleFS.begin(false) && LittleFS.exists("/index.html");
  report("filesystem", fsOk, fsOk ? "" : "(mount failed or /index.html missing; run uploadfs)");
  if (!fsOk) r.critical |= CF_FILESYSTEM;

  // NVS: лічильник завантажень з перевіркою зворотного читання
  Preferences prefs;
  bool prefsOk = prefs.begin("fire", false);
  if (prefsOk) {
    const uint32_t boots = prefs.getUInt("boots", 0) + 1;
    prefsOk = prefs.putUInt("boots", boots) > 0 && prefs.getUInt("boots", 0) == boots;
    char b[16];
    snprintf(b, sizeof(b), "boot #%lu", (unsigned long)boots);
    if (prefs.isKey("token")) prefs.remove("token");  // залишок старої авторизації
    report("settings", prefsOk, b);
    r.bootNo = boots;
    prefs.end();
  } else {
    report("settings", false);
  }
  if (!prefsOk) r.critical |= CF_SETTINGS;

  // Кнопки при старті мають бути відпущені (HIGH), інакше — залипання/обрив лінії
  pinMode(cfg::PIN_BTN_GREEN, INPUT);
  pinMode(cfg::PIN_BTN_YELLOW, INPUT_PULLUP);
  delay(5);
  const bool btnOk = digitalRead(cfg::PIN_BTN_GREEN) == HIGH && digitalRead(cfg::PIN_BTN_YELLOW) == HIGH;
  report("buttons", btnOk, btnOk ? "" : "(pressed or stuck at boot)");
  if (!btnOk) r.critical |= CF_BUTTON_STUCK;

  Wire.begin(cfg::PIN_OLED_SDA, cfg::PIN_OLED_SCL);
  Wire.beginTransmission(cfg::OLED_ADDR);
  r.displayOk = Wire.endTransmission() == 0;
  report("oled", r.displayOk);

  return r;
}
