#include "vbus_monitor.h"

#include <Arduino.h>

#include "config.h"

void VbusMonitor::begin(uint32_t now) {
  pinMode(cfg::PIN_VBUS_ADC, INPUT);
  analogSetPinAttenuation(cfg::PIN_VBUS_ADC, ADC_11db);
  lastMs_ = now;
  sample();
}

void VbusMonitor::update(uint32_t now) {
  if (now - lastMs_ < cfg::VBUS_SAMPLE_MS) return;
  lastMs_ = now;
  sample();
}

void VbusMonitor::sample() {
  uint32_t sum = 0;
  constexpr int N = 8;
  for (int i = 0; i < N; i++) sum += analogReadMilliVolts(cfg::PIN_VBUS_ADC);
  mv_ = (uint32_t)((sum / N) * VBUS_DIVIDER_RATIO);

  const bool inRange = mv_ >= cfg::VBUS_MIN_MV && mv_ <= cfg::VBUS_MAX_MV;
  if (inRange) {
    badCount_ = 0;
    ok_ = true;
  } else if (badCount_ < 255) {
    badCount_++;
    if (badCount_ >= cfg::VBUS_FAULT_SAMPLES || !ok_) ok_ = false;
  }
}
