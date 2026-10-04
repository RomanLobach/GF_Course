#include "SysLog.h"

#include <FS.h>
#include <LittleFS.h>
#include <stdarg.h>
#include "Config.h"

namespace SysLog {
namespace {

constexpr auto FILE_PATH = "/syslog.bin";
constexpr uint32_t FLUSH_INTERVAL_MS = 1000;
constexpr size_t LINE_LEN = 96;

Record g_ram[RAM_CAPACITY]{};
File g_file;
bool g_ready = false;
LogLevel g_level = static_cast<LogLevel>(Config::SYSLOG_DEFAULT_LEVEL);

uint32_t g_nextSeq = 1;    // seq of the next record to be logged
uint32_t g_flushedSeq = 1; // next seq to be written to the file
uint32_t g_fileOldest = 1; // oldest seq still valid in the file
uint32_t g_clearedBelow = 1;
uint16_t g_bootId = 0;
uint32_t g_droppedEcho = 0;
uint32_t g_droppedFlash = 0;
uint32_t g_lastFlushMs = 0;
bool g_dirty = false;

const char LEVEL_CHAR[] = {'E', 'W', 'I', 'D'};

void echo(const Record &rec) {
  char line[LINE_LEN];
  format(rec, line, sizeof(line));
  const size_t len = strlen(line);
  // +1 for the newline. Never wait for the UART: the protocol core has deadlines.
  if (Serial.availableForWrite() < static_cast<int>(len + 1)) {
    g_droppedEcho++;
    return;
  }
  Serial.write(reinterpret_cast<const uint8_t *>(line), len);
  Serial.write('\n');
}

bool createFile() {
  File f = LittleFS.open(FILE_PATH, "w");
  if (!f) return false;
  const Record empty{};
  for (size_t i = 0; i < FILE_CAPACITY; i++) {
    if (f.write(reinterpret_cast<const uint8_t *>(&empty), sizeof(empty)) != sizeof(empty)) {
      f.close();
      return false;
    }
  }
  f.close();
  return true;
}

bool readSlot(const size_t slot, Record &out) {
  if (!g_file.seek(slot * sizeof(Record))) return false;
  return g_file.read(reinterpret_cast<uint8_t *>(&out), sizeof(out)) == sizeof(out);
}

} // namespace

bool begin() {
  if (!LittleFS.begin(true)) return false;

  File probe = LittleFS.open(FILE_PATH, "r");
  const size_t size = probe ? probe.size() : 0;
  if (probe) probe.close();
  if (size != FILE_CAPACITY * sizeof(Record) && !createFile()) return false;

  g_file = LittleFS.open(FILE_PATH, "r+");
  if (!g_file) return false;

  // Recover the running counters from the slots themselves.
  uint32_t maxSeq = 0;
  uint32_t minSeq = UINT32_MAX;
  uint16_t lastBoot = 0;
  for (size_t slot = 0; slot < FILE_CAPACITY; slot++) {
    Record rec{};
    if (!readSlot(slot, rec) || rec.seq == 0) continue;
    if (rec.seq > maxSeq) {
      maxSeq = rec.seq;
      lastBoot = rec.bootId;
    }
    if (rec.seq < minSeq) minSeq = rec.seq;
  }

  g_nextSeq = maxSeq + 1;
  g_flushedSeq = g_nextSeq;
  g_fileOldest = maxSeq == 0 ? g_nextSeq : minSeq;
  g_clearedBelow = g_fileOldest;
  g_bootId = static_cast<uint16_t>(lastBoot + 1);
  g_ready = true;
  return true;
}

void log(const LogLevel level, const char *tag, const char *fmt, ...) {
  if (static_cast<uint8_t>(level) > static_cast<uint8_t>(g_level)) return;

  Record rec{};
  rec.uptimeMs = millis();
  rec.bootId = g_bootId;
  rec.level = static_cast<uint8_t>(level);
  strncpy(rec.tag, tag, TAG_LEN);
  va_list args;
  va_start(args, fmt);
  vsnprintf(rec.text, TEXT_LEN, fmt, args);
  va_end(args);

  if (g_ready) {
    rec.seq = g_nextSeq++;
    g_ram[rec.seq % RAM_CAPACITY] = rec;
  }
  echo(rec);
}

void loopTask(const bool allowFlashIo) {
  if (!g_ready || !allowFlashIo) return;

  if (g_nextSeq - g_flushedSeq > RAM_CAPACITY) {
    // Flash writes were held off longer than the ring lasts (a long burst window).
    g_droppedFlash += g_nextSeq - g_flushedSeq - RAM_CAPACITY;
    g_flushedSeq = g_nextSeq - RAM_CAPACITY;
  }
  while (g_flushedSeq < g_nextSeq) {
    const Record &rec = g_ram[g_flushedSeq % RAM_CAPACITY];
    if (!g_file.seek((rec.seq % FILE_CAPACITY) * sizeof(Record)) ||
        g_file.write(reinterpret_cast<const uint8_t *>(&rec), sizeof(rec)) != sizeof(rec)) {
      break; // retry on the next iteration
    }
    g_flushedSeq++;
    g_dirty = true;
  }
  if (g_flushedSeq > FILE_CAPACITY && g_flushedSeq - FILE_CAPACITY > g_fileOldest) {
    g_fileOldest = g_flushedSeq - FILE_CAPACITY;
  }
  if (g_dirty && millis() - g_lastFlushMs >= FLUSH_INTERVAL_MS) {
    g_file.flush();
    g_dirty = false;
    g_lastFlushMs = millis();
  }
}

void sync() {
  if (!g_ready) return;
  loopTask(true);
  if (g_dirty) {
    g_file.flush();
    g_dirty = false;
    g_lastFlushMs = millis();
  }
}

void setLevel(const LogLevel level) { g_level = level; }
LogLevel level() { return g_level; }

bool clear() {
  if (!g_ready) return false;
  g_file.close();
  const bool ok = createFile();
  g_file = LittleFS.open(FILE_PATH, "r+");
  g_flushedSeq = g_nextSeq;
  g_fileOldest = g_nextSeq;
  g_clearedBelow = g_nextSeq;
  g_dirty = false;
  return ok && g_file;
}

uint32_t firstSeq() {
  // Oldest of what the file and the RAM ring still hold; read() rejects any gap in between.
  const uint32_t ramOldest = g_nextSeq > RAM_CAPACITY ? g_nextSeq - RAM_CAPACITY : 1;
  const uint32_t first = g_fileOldest < ramOldest ? g_fileOldest : ramOldest;
  return first < g_clearedBelow ? g_clearedBelow : first;
}

uint32_t nextSeq() { return g_nextSeq; }

bool read(const uint32_t seq, Record &out) {
  if (seq < firstSeq() || seq >= g_nextSeq) return false;
  const Record &ram = g_ram[seq % RAM_CAPACITY];
  if (ram.seq == seq) {
    out = ram;
    return true;
  }
  if (seq >= g_flushedSeq) return false;
  return readSlot(seq % FILE_CAPACITY, out) && out.seq == seq;
}

void format(const Record &rec, char *buf, const size_t len) {
  char tag[TAG_LEN + 1] = {};
  memcpy(tag, rec.tag, TAG_LEN);
  const uint8_t lvl = rec.level < sizeof(LEVEL_CHAR) ? rec.level : 0;
  snprintf(buf, len, "#%lu b%u [%6lu.%03lu] %c %-5s: %s", static_cast<unsigned long>(rec.seq), rec.bootId,
           static_cast<unsigned long>(rec.uptimeMs / 1000), static_cast<unsigned long>(rec.uptimeMs % 1000),
           LEVEL_CHAR[lvl], tag, rec.text);
}

uint16_t bootId() { return g_bootId; }
uint32_t droppedEcho() { return g_droppedEcho; }
uint32_t droppedFlash() { return g_droppedFlash; }

const char *levelName(const LogLevel level) {
  switch (level) {
    case LogLevel::Error: return "error";
    case LogLevel::Warn:  return "warn";
    case LogLevel::Info:  return "info";
    case LogLevel::Debug: return "debug";
  }
  return "?";
}

bool parseLevel(const char *s, LogLevel &out) {
  static constexpr LogLevel ALL[] = {LogLevel::Error, LogLevel::Warn, LogLevel::Info, LogLevel::Debug};
  for (const LogLevel l : ALL) {
    const char *name = levelName(l);
    if (strcasecmp(s, name) == 0 || (s[0] != '\0' && s[1] == '\0' && tolower(s[0]) == name[0])) {
      out = l;
      return true;
    }
  }
  return false;
}

} // namespace SysLog
