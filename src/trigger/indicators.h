#pragma once
#include <stdint.h>

#include "trigger_fsm.h"

// Червоний LED (GPIO0, активний LOW): світиться ТІЛЬКИ коли активний вихід спрацювання
// (DISCHARGE поза тест-режимом).
// Жовтий LED (GPIO2, активний LOW): ARMING - повільне миготіння, READY - світить постійно,
// FAULT - швидке миготіння, SAFE - вимкнений. READY з ігнорованим сигналом (FIRE був до
// постановки) - світить із короткими «смиканнями» (100 мс вимкнено раз на 400 мс). Тест-режим: SAFE - короткий імпульс раз на секунду,
// DISCHARGE (вихід вимкнений) - подвійний спалах раз на секунду.
class StatusLed {
 public:
  void begin();
  void update(State s, bool explosionOutput, bool testMode, bool fireIgnored, uint32_t now);

 private:
  static void write(int pin, bool on);
};

class StatusDisplay {
 public:
  bool begin();
  void update(const Snapshot& s, const char* ip, uint32_t now);

 private:
  uint32_t lastMs_ = 0;
};
