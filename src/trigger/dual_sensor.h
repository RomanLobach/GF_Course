#pragma once
#include "sensor_iface.h"

// Двоканальний датчик: статус NORMAL/FIRE видається ЛИШЕ коли обидва канали збігаються.
// Будь-яка помилка каналу або тривала розбіжність -> FAULT (ніколи FIRE).
class DualSensor : public ISensor {
 public:
  DualSensor(ISensor* a, ISensor* b) : a_(a), b_(b) {}
  void begin(uint32_t now) override;
  void update(uint32_t now) override;
  SensorStatus status() const override { return status_; }
  const char* detail() const override { return detail_; }
  int dutyPercent() const override { return a_->dutyPercent(); }

 private:
  ISensor* a_;
  ISensor* b_;
  SensorStatus status_ = SensorStatus::UNKNOWN;
  const char* detail_ = "waiting";
  uint32_t mismatchSince_ = 0;
};
