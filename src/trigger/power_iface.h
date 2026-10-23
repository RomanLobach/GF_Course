#pragma once
#include <stdint.h>

// Абстракція моніторингу живлення: автомат знає лише "в нормі / ні".
class IVoltageMonitor {
 public:
  virtual ~IVoltageMonitor() = default;
  virtual uint32_t millivolts() const = 0;
  virtual bool ok() const = 0;
};
