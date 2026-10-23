#pragma once
#include <Arduino.h>

#include "event_log.h"
#include "trigger_fsm.h"

// Веб-інтерфейс: плата підключається як клієнт до відомої WiFi-мережі (без блокування старту:
// фізичні кнопки працюють і без мережі), сторінка з LittleFS, статус JSON, команди, журнал.
class WebUi {
 public:
  bool begin(TriggerFsm* fsm, EventLog* log);
  void handle();
  String ip() const;  // "no WiFi", якщо не підключено

 private:
  bool mdnsStarted_ = false;
  bool wasConnected_ = false;
};
