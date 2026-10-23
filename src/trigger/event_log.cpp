#include "event_log.h"

#include <Arduino.h>
#include <Preferences.h>

namespace {
constexpr uint32_t MAGIC = 0x464C4731;  // 'FLG1'
constexpr uint32_t FLUSH_INTERVAL_MS = 2000;

struct Blob {
  uint32_t magic;
  uint32_t crc;  // CRC32 від nextSeq до кінця
  uint32_t nextSeq;
  uint16_t head;
  uint16_t count;
  LogEntry e[EventLog::CAPACITY];
};

uint32_t crc32(const uint8_t* d, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  while (n--) {
    c ^= *d++;
    for (int i = 0; i < 8; i++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

uint32_t blobCrc(const Blob& b) {
  return crc32(reinterpret_cast<const uint8_t*>(&b.nextSeq), sizeof(Blob) - offsetof(Blob, nextSeq));
}
}  // namespace

const char* eventTypeName(uint8_t t) {
  switch ((EventType)t) {
    case EventType::BOOT: return "BOOT";
    case EventType::TRANSITION: return "TRANSITION";
    case EventType::CMD_REJECT: return "CMD_REJECT";
    case EventType::AUTH_FAIL: return "AUTH_FAIL";
    case EventType::TEST_MODE: return "TEST_MODE";
    case EventType::FIRE_IGNORED: return "FIRE_IGNORED";
  }
  return "?";
}

bool EventLog::begin(uint32_t bootNo) {
  bootNo_ = bootNo;
  Preferences prefs;
  if (!prefs.begin("fire", true)) return false;
  Blob b;
  const bool read = prefs.getBytes("log", &b, sizeof(b)) == sizeof(b);
  prefs.end();
  if (read && b.magic == MAGIC && b.crc == blobCrc(b) && b.count <= CAPACITY && b.head < CAPACITY) {
    memcpy(e_, b.e, sizeof(e_));
    head_ = b.head;
    count_ = b.count;
    nextSeq_ = b.nextSeq;
    return true;
  }
  Serial.println("[log] no valid log in NVS, starting empty");
  return read;  // false, якщо запису ще не було
}

void EventLog::event(EventType type, State from, State to, const char* text, uint32_t now) {
  LogEntry& en = e_[head_];
  en.seq = nextSeq_++;
  en.bootNo = bootNo_;
  en.uptimeMs = now;
  en.type = (uint8_t)type;
  en.from = (uint8_t)from;
  en.to = (uint8_t)to;
  en.pad = 0;
  strncpy(en.text, text ? text : "", sizeof(en.text) - 1);
  en.text[sizeof(en.text) - 1] = '\0';
  head_ = (head_ + 1) % CAPACITY;
  if (count_ < CAPACITY) count_++;
  dirty_ = true;
  if (type == EventType::BOOT || to == State::FAULT || to == State::EXPLOSION || type == EventType::FIRE_IGNORED) urgent_ = true;
}

const LogEntry& EventLog::newest(size_t i) const { return e_[(head_ + CAPACITY - 1 - i) % CAPACITY]; }

bool EventLog::save() {
  Blob b{};
  b.magic = MAGIC;
  b.nextSeq = nextSeq_;
  b.head = head_;
  b.count = count_;
  memcpy(b.e, e_, sizeof(e_));
  b.crc = blobCrc(b);
  Preferences prefs;
  if (!prefs.begin("fire", false)) return false;
  const bool ok = prefs.putBytes("log", &b, sizeof(b)) == sizeof(b);
  prefs.end();
  return ok;
}

void EventLog::flush(uint32_t now, bool allowBlocking) {
  if (!dirty_ || !allowBlocking) return;
  if (!urgent_ && now - lastFlushMs_ < FLUSH_INTERVAL_MS) return;
  if (save()) {
    dirty_ = false;
    urgent_ = false;
  }
  lastFlushMs_ = now;  // при помилці записуємо знову не частіше ніж раз на інтервал
}
