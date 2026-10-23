#pragma once
#include <stddef.h>
#include <stdint.h>

#include "trigger_fsm.h"

struct LogEntry {
  uint32_t seq;
  uint32_t bootNo;
  uint32_t uptimeMs;
  uint8_t type;  // EventType
  uint8_t from;  // State
  uint8_t to;    // State
  uint8_t pad;
  char text[20];
};

// Кільцевий журнал подій у NVS (переживає перезавантаження, з CRC). Записи накопичуються в RAM,
// у флеш скидаються з затримкою і ніколи не під час ARMING/READY (запис блокує цикл на ~десятки мс).
class EventLog : public IEventSink {
 public:
  static constexpr size_t CAPACITY = 32;

  bool begin(uint32_t bootNo);
  void event(EventType type, State from, State to, const char* text, uint32_t now) override;
  void flush(uint32_t now, bool allowBlocking);

  size_t size() const { return count_; }
  const LogEntry& newest(size_t i) const;  // i = 0 -> найновіший

 private:
  bool save();

  LogEntry e_[CAPACITY] = {};
  uint16_t head_ = 0;   // наступна позиція запису
  uint16_t count_ = 0;
  uint32_t nextSeq_ = 1;
  uint32_t bootNo_ = 0;
  bool dirty_ = false;
  bool urgent_ = false;
  uint32_t lastFlushMs_ = 0;
};

const char* eventTypeName(uint8_t t);
