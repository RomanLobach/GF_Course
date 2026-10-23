#include "dual_sensor.h"

#include "config.h"

void DualSensor::begin(uint32_t now) {
  a_->begin(now);
  b_->begin(now);
}

void DualSensor::update(uint32_t now) {
  a_->update(now);
  b_->update(now);
  const SensorStatus sa = a_->status();
  const SensorStatus sb = b_->status();

  if (sa == SensorStatus::FAULT || sb == SensorStatus::FAULT) {
    status_ = SensorStatus::FAULT;
    detail_ = sa == SensorStatus::FAULT ? a_->detail() : b_->detail();
    mismatchSince_ = 0;
    return;
  }
  if (sa == SensorStatus::UNKNOWN || sb == SensorStatus::UNKNOWN) {
    status_ = SensorStatus::UNKNOWN;
    detail_ = "waiting";
    return;
  }
  if (sa == sb) {
    status_ = sa;
    detail_ = sa == SensorStatus::FIRE ? "fire signal" : "ok";
    mismatchSince_ = 0;
    return;
  }

  // Канали розходяться. Короткий розбіг (різна затримка фільтрів) переживаємо, не змінюючи статус
  // (NORMAL лишається NORMAL, FIRE не з'являється); тривала розбіжність -> FAULT.
  if (mismatchSince_ == 0) mismatchSince_ = now;
  if (now - mismatchSince_ >= cfg::CHANNEL_MISMATCH_MS) {
    status_ = SensorStatus::FAULT;
    detail_ = "channels disagree";
  }
}
