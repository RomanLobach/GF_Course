#include "Post.h"

#include <Wire.h>
#include <esp_system.h>
#include "Config.h"
#include "SysLog.h"
#include "Version.h"

namespace Post {
namespace {

constexpr int16_t SX1276_VERSION = 0x12;
constexpr int BATTERY_SAMPLES = 16;

const char *const NAMES[BIT_COUNT] = {"power", "nvs", "radio", "oled", "storage", "config", "reset", "forced"};
const char *const LABELS[BIT_COUNT] = {"живлення", "NVS", "радіо", "OLED", "flash", "конфіг", "перезапуск", "тест"};

Result g_result;

uint16_t readBatteryMv() {
  uint32_t sum = 0;
  for (int i = 0; i < BATTERY_SAMPLES; i++) sum += analogReadMilliVolts(Config::PIN_BATTERY_ADC);
  return static_cast<uint16_t>(sum / BATTERY_SAMPLES * Config::BATTERY_DIVIDER);
}

bool oledAcks() {
  Wire.begin(Config::PIN_OLED_SDA, Config::PIN_OLED_SCL, Config::OLED_I2C_CLOCK_HZ);
  // Right after a fast reset the SSD1306 sometimes isn't answering yet (seen as a false "oled"
  // failure on ~1 boot in 4) - give it a few tries. setup() only, before the protocol runs.
  bool ack = false;
  for (uint32_t i = 0; i < Config::POST_OLED_PROBE_TRIES && !ack; i++) {
    if (i > 0) {
      delay(Config::POST_OLED_PROBE_GAP_MS);
      SLOG_D("post", "oled probe retry %u", static_cast<unsigned>(i));
    }
    Wire.beginTransmission(Config::OLED_I2C_ADDR);
    ack = Wire.endTransmission() == 0;
  }
  // Release the bus: Display::begin() installs the I2C driver again from the display task,
  // so its interrupt lands on the UI core, not on the protocol core that runs this.
  Wire.end();
  return ack;
}

bool badResetReason(const esp_reset_reason_t r) {
  return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_BROWNOUT;
}

} // namespace

void run(const Inputs &in) {
  Result r;
  r.ran = true;

  const uint16_t mv = readBatteryMv();
  r.batteryMv = mv >= Config::POST_BATT_ABSENT_MV ? mv : 0;
  if (r.batteryMv != 0 && r.batteryMv < Config::POST_BATT_MIN_MV) r.mask |= Power;

  if (!in.nvsOk) r.mask |= Nvs;

  r.radioChipVersion = in.radioChipVersion;
  if (!in.radioInit || in.radioChipVersion != SX1276_VERSION) r.mask |= Radio;

  if (!oledAcks()) r.mask |= Oled;
  if (!in.storageOk) r.mask |= Storage;
  if (!in.configHealthy) r.mask |= ConfigBad;

  const esp_reset_reason_t reason = esp_reset_reason();
  if (badResetReason(reason)) r.mask |= ResetReason;

#if defined(FW_FORCE_POST_FAIL)
  r.mask |= Forced;
#endif

  g_result = r;

  for (uint8_t bit = 0; bit < BIT_COUNT; bit++) {
    if (r.mask & (1u << bit)) SLOG_E("post", "FAIL %s", NAMES[bit]);
  }
  if (reason == ESP_RST_WDT) SLOG_I("post", "reset by RTC wdt (normal after flashing)");
  SLOG_I("post", "%s mask 0x%02X, batt %u mV, sx127x 0x%02X", r.mask ? "FAILED" : "ok", r.mask, r.batteryMv,
         static_cast<unsigned>(r.radioChipVersion & 0xFF));
}

const Result &result() {
  return g_result;
}

const char *bitName(const uint8_t bit) {
  return bit < BIT_COUNT ? NAMES[bit] : "?";
}

const char *bitLabel(const uint8_t bit) {
  return bit < BIT_COUNT ? LABELS[bit] : "?";
}

void describe(char *buf, const size_t len) {
  if (g_result.mask == 0) {
    snprintf(buf, len, "Самотест OK");
    return;
  }
  size_t n = static_cast<size_t>(snprintf(buf, len, "Самотест: збій"));
  bool first = true;
  for (uint8_t bit = 0; bit < BIT_COUNT && n < len; bit++) {
    if (!(g_result.mask & (1u << bit))) continue;
    n += static_cast<size_t>(snprintf(buf + n, len - n, "%s %s", first ? "" : ",", LABELS[bit]));
    first = false;
  }
}

void print(Print &out) {
  const Result &r = g_result;
  out.printf("POST %s, mask 0x%02X (reset: %s)\n", r.mask ? "FAILED" : "ok", r.mask, Version::resetReasonName());
  for (uint8_t bit = 0; bit < BIT_COUNT; bit++) {
    out.printf("  %u %-8s %s", bit, NAMES[bit], (r.mask & (1u << bit)) ? "FAIL" : "ok");
    if (bit == 0) {
      if (r.batteryMv == 0) {
        out.print("  (no battery)");
      } else {
        out.printf("  (%u mV, min %lu)", r.batteryMv, static_cast<unsigned long>(Config::POST_BATT_MIN_MV));
      }
    }
    if (bit == 2) out.printf("  (version 0x%02X)", static_cast<unsigned>(r.radioChipVersion & 0xFF));
    out.println();
  }
}

} // namespace Post
