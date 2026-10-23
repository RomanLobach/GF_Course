#include "pwm_channel.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_timer.h>

#include "config.h"

// Вимірює період і тривалість високого рівня за фронтами; публікує кадр після кожного періоду.
void IRAM_ATTR PwmChannel::onEdge(void* arg) {
  IsrState* s = static_cast<IsrState*>(arg);
  const uint32_t t = (uint32_t)esp_timer_get_time();
  if (gpio_get_level((gpio_num_t)s->pin)) {
    if (s->haveHigh) {
      portENTER_CRITICAL_ISR(&s->mux);
      s->periodUs = t - s->riseUs;
      s->highUs = s->pendingHighUs;
      s->frames = s->frames + 1;
      portEXIT_CRITICAL_ISR(&s->mux);
    }
    s->riseUs = t;
    s->haveHigh = false;
  } else if (s->riseUs != 0) {
    s->pendingHighUs = t - s->riseUs;
    s->haveHigh = true;
  }
}

PwmChannel::Cls PwmChannel::classify(uint32_t periodUs, uint32_t highUs) const {
  using namespace pwmproto;
  const uint32_t lo = PERIOD_US * (100 - PERIOD_TOL_PCT) / 100;
  const uint32_t hi = PERIOD_US * (100 + PERIOD_TOL_PCT) / 100;
  if (periodUs < lo || periodUs > hi || highUs > periodUs) return Cls::INVALID;
  const int duty = (int)(highUs * 100UL / periodUs);
  if (abs(duty - (int)normalDuty_) <= DUTY_TOL_PCT) return Cls::NORMAL;
  if (abs(duty - (int)fireDuty_) <= DUTY_TOL_PCT) return Cls::FIRE;
  return Cls::INVALID;
}

void PwmChannel::setDetail(const char* text) { snprintf(detail_, sizeof(detail_), "%s: %s", name_, text); }

void PwmChannel::begin(uint32_t now) {
  isr_.pin = pin_;
  pinMode(pin_, INPUT);
  lastFrameMs_ = now;
  attachInterruptArg(digitalPinToInterrupt(pin_), onEdge, &isr_, CHANGE);
}

void PwmChannel::update(uint32_t now) {
  IsrState* st = &isr_;
  uint32_t frames, highUs, periodUs;
  portENTER_CRITICAL(&st->mux);
  frames = st->frames;
  highUs = st->highUs;
  periodUs = st->periodUs;
  portEXIT_CRITICAL(&st->mux);

  if (frames != lastFrames_) {
    lastFrames_ = frames;
    lastFrameMs_ = now;
    const Cls c = classify(periodUs, highUs);
    duty_ = periodUs ? (int)(highUs * 100UL / periodUs) : -1;
    if (c != candidate_) {
      candidate_ = c;
      candidateSince_ = now;
    }
  }

  if (now - lastFrameMs_ > cfg::SENSOR_TIMEOUT_MS) {
    // Немає сигналу: обрив/КЗ/мертвий датчик -> FAULT, ніколи не FIRE.
    status_ = SensorStatus::FAULT;
    setDetail(digitalRead(pin_) ? "stuck high" : "no signal");
    duty_ = -1;
    candidate_ = Cls::INVALID;
    candidateSince_ = now;
    return;
  }

  // Статус змінюється лише коли новий клас тримається SENSOR_DEBOUNCE_MS (глітч не змінює стан).
  if (candidate_ != Cls::NONE && now - candidateSince_ >= cfg::SENSOR_DEBOUNCE_MS) {
    switch (candidate_) {
      case Cls::NORMAL: status_ = SensorStatus::NORMAL; setDetail("ok"); break;
      case Cls::FIRE: status_ = SensorStatus::FIRE; setDetail("fire"); break;
      default: status_ = SensorStatus::FAULT; setDetail("invalid PWM"); break;
    }
  }
}
