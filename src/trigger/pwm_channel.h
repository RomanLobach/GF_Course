#pragma once
#include <Arduino.h>

#include "sensor_iface.h"

// Один PWM-канал датчика: статус із шпаруватості. Канал не знає про другий канал.
class PwmChannel : public ISensor {
 public:
  PwmChannel(int pin, uint8_t normalDutyPct, uint8_t fireDutyPct, const char* name)
      : pin_(pin), normalDuty_(normalDutyPct), fireDuty_(fireDutyPct), name_(name) {}
  void begin(uint32_t now) override;
  void update(uint32_t now) override;
  SensorStatus status() const override { return status_; }
  const char* detail() const override { return detail_; }
  int dutyPercent() const override { return duty_; }

 private:
  enum class Cls : uint8_t { NONE, NORMAL, FIRE, INVALID };
  // Дані переривання (вимірювання періоду/шпаруватості). Об'єкт каналу глобальний -> лежить у DRAM.
  struct IsrState {
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    volatile uint32_t frames = 0, highUs = 0, periodUs = 0;
    uint32_t riseUs = 0, pendingHighUs = 0;
    bool haveHigh = false;
    int pin = -1;
  };
  static void onEdge(void* arg);
  Cls classify(uint32_t periodUs, uint32_t highUs) const;
  void setDetail(const char* text);

  IsrState isr_;
  int pin_;
  uint8_t normalDuty_, fireDuty_;
  const char* name_;
  SensorStatus status_ = SensorStatus::UNKNOWN;
  char detail_[32] = "waiting";
  int duty_ = -1;
  uint32_t lastFrames_ = 0;
  uint32_t lastFrameMs_ = 0;
  Cls candidate_ = Cls::NONE;
  uint32_t candidateSince_ = 0;
};
