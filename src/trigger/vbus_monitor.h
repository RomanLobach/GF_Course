#pragma once
#include <stdint.h>

#include "power_iface.h"

// Моніторинг напруги живлення (шина 5 В через зовнішній дільник на GPIO35).
class VbusMonitor : public IVoltageMonitor {
 public:
  void begin(uint32_t now);
  void update(uint32_t now);
  uint32_t millivolts() const override { return mv_; }
  bool ok() const override { return ok_; }

 private:
  void sample();
  uint32_t mv_ = 0;
  bool ok_ = false;  // доки не виміряно — вважаємо поганою
  uint8_t badCount_ = 0;
  uint32_t lastMs_ = 0;
};
