// System (diagnostic) log with levels - separate from FlashLog, which holds the benchmark
// data for the CSV export.
//
// - Levels ERROR < WARN < INFO < DEBUG; records above the current level are dropped at the
//   call site (cheap).
// - Every record goes into a fixed RAM ring (no heap) and is echoed to Serial only if the
//   UART TX buffer has room for the whole line - a full buffer drops the echo (counted),
//   it never blocks the caller.
// - loopTask(allowFlashIo) persists the ring into a fixed-size circular file on LittleFS
//   (/syslog.bin, FILE_CAPACITY slots), so the log survives a reboot - the "why did it
//   restart in the field" trail. Slot = seq % FILE_CAPACITY, every slot carries its own seq,
//   so no header has to be kept consistent on power loss: boot rescans the slots.
// - Protocol core only (loop()/setup()): the input and display tasks never log (the Serial
//   echo takes the UART lock).
// - begin() must run first in setup(): records logged before it go to Serial only.
#pragma once

#include <Arduino.h>

enum class LogLevel : uint8_t { Error = 0, Warn = 1, Info = 2, Debug = 3 };

namespace SysLog {

constexpr size_t RAM_CAPACITY = 64;   // records kept in RAM (4 KB)
constexpr size_t FILE_CAPACITY = 256; // records kept on flash (16 KB, preallocated)
constexpr size_t TAG_LEN = 5;
constexpr size_t TEXT_LEN = 48;

#pragma pack(push, 1)
struct Record {
  uint32_t seq;          // 1, 2, ... across reboots; 0 = empty slot
  uint32_t uptimeMs;     // millis() when logged
  uint16_t bootId;       // increments on every boot (derived from the file, no NVS)
  uint8_t level;         // LogLevel
  char tag[TAG_LEN];     // module, NUL-padded (not necessarily NUL-terminated)
  char text[TEXT_LEN];   // NUL-terminated, truncated to fit
};
#pragma pack(pop)
static_assert(sizeof(Record) == 64, "SysLog::Record is a 64-byte on-flash slot");

bool begin(); // mounts LittleFS, opens/creates the file, recovers seq and boot id
void loopTask(bool allowFlashIo);
// Write everything still in RAM to the file right now (before an intentional reboot).
void sync();

void log(LogLevel level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void setLevel(LogLevel level);
LogLevel level();

bool clear(); // wipes RAM + file; seq keeps counting so old/new never mix

// Reading back, oldest to newest: seq in [firstSeq(), nextSeq()). read() fails for a seq that
// was overwritten or never persisted.
uint32_t firstSeq();
uint32_t nextSeq();
bool read(uint32_t seq, Record &out);
void format(const Record &rec, char *buf, size_t len); // "#12 b3 [  12.345] I sess : text"

uint16_t bootId();
uint32_t droppedEcho();  // Serial echoes skipped because the TX buffer was full
uint32_t droppedFlash(); // records lost because the RAM ring overflowed before a flash write

const char *levelName(LogLevel level);
bool parseLevel(const char *s, LogLevel &out); // "error"/"e", "warn"/"w", "info"/"i", "debug"/"d"

} // namespace SysLog

#define SLOG_E(tag, ...) SysLog::log(LogLevel::Error, tag, __VA_ARGS__)
#define SLOG_W(tag, ...) SysLog::log(LogLevel::Warn, tag, __VA_ARGS__)
#define SLOG_I(tag, ...) SysLog::log(LogLevel::Info, tag, __VA_ARGS__)
#define SLOG_D(tag, ...) SysLog::log(LogLevel::Debug, tag, __VA_ARGS__)
