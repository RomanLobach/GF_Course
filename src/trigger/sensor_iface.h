#pragma once
#include <stdint.h>

// Абстракція датчика: автомат нічого не знає про PWM, лише про статус.
enum class SensorStatus : uint8_t { UNKNOWN, NORMAL, FIRE, FAULT };

inline const char* sensorStatusName(SensorStatus s) {
  switch (s) {
    case SensorStatus::NORMAL: return "NORMAL";
    case SensorStatus::FIRE: return "FIRE";
    case SensorStatus::FAULT: return "FAULT";
    default: return "UNKNOWN";
  }
}

class ISensor {
 public:
  virtual ~ISensor() = default;
  virtual void begin(uint32_t now) = 0;
  virtual void update(uint32_t now) = 0;
  virtual SensorStatus status() const = 0;
  virtual const char* detail() const = 0;  // причина FAULT / пояснення
  virtual int dutyPercent() const = 0;     // -1, якщо невідомо
};
