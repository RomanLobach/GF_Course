// Append-only log storage on the ESP32's own internal flash - no external chip,
// nothing wired up on SPI for storage.
//
// Uses LittleFS (bundled with the Arduino ESP32 core) mounted on the internal
// flash's "spiffs" partition (see the default PlatformIO/ESP32 partition
// table - no partitions.csv override needed).
//
// Writes go through a small RAM ring buffer drained in loopTask() so a flash
// write never stalls packet handling on the radio/protocol core. loopTask() is
// told when the protocol is inside a burst window and then defers all flash I/O;
// flush() runs at most once per second.
#pragma once

#include <Arduino.h>
#include <FS.h>
#include "Config.h"

// Heartbeats/control packets are never logged - only session
// events, MEAS bench packets (the main data) and TEST per-burst summaries.
enum class LogRecordType : uint8_t {
  SessionEvent = 0, // see LogEventKind
  Bench = 1,        // one MEAS bench packet (Base: sent, Rover: received/lost)
  TestSummary = 2,  // one TEST burst summary
  Post = 3,         // power-on self-test result, once per boot (eventKind = Post mask)
};

enum class LogEventKind : uint8_t {
  SyncSuccess = 0, // entered SYNC (handshake #1, or a new session after MEAS)
  SyncFail = 1,    // session lost (3 missed heartbeats / peer restarted the handshake)
  ManualStop = 2,  // "Синхронізація [завершити]" (either side)
  MeasureDone = 3, // full 6-profile MEAS pass finished - delimits its bench records
  MeasStopped = 4, // MEAS stopped from the menu or by lost slots
};

#pragma pack(push, 1)
struct LogRecord {
  uint16_t sessionId;
  uint16_t seq;           // running record index (not the on-air seq_num)
  uint8_t direction;      // Bench only: 0 = TX, 1 = RX
  uint8_t type;           // LogRecordType
  uint8_t configIndex;    // 0 for session events, 1..6 for bench packets
  uint8_t eventKind;      // SessionEvent: LogEventKind; TestSummary: packets sent in the burst; Post: mask
  uint32_t sessionTimeMs; // relative to the local session timer
  uint16_t size;          // Bench only: packet size in bytes
  int16_t rssi;           // dBm, or INT16_MIN if not applicable
  int16_t snrTenths;      // SNR*10 in dB, or INT16_MIN if not applicable
  uint8_t deviceStatus;   // Protocol::DeviceState value at time of record
  uint8_t received;       // Bench: 1 = received/sent, 0 = lost; TestSummary: packets received (0xFF = unknown)
  // TX only (Base side): measured on-air duration from startTransmit() to the
  // DIO0 TX-done interrupt - actual measured airtime, not the Airtime.h estimate
  // (TestSummary: average over the burst). 0 if not applicable.
  uint32_t txTimeUs;
};
#pragma pack(pop)

class FlashLog {
public:
  static constexpr int16_t NO_RSSI = INT16_MIN;
  static constexpr int16_t NO_SNR = INT16_MIN;

  bool begin();

  // Enqueues a record (non-blocking); actual flash write happens in loopTask().
  void logEvent(const LogRecord &rec);

  // Call every main-loop iteration: drains pending records to flash. With
  // allowFlashIo = false (protocol inside a burst window) nothing touches flash.
  void loopTask(bool allowFlashIo);

  // Erases all stored logs (menu "Стерти логи" / browser). Never touches the session id.
  bool eraseAll();

  uint32_t recordCount() const { return nextSeq_; }
  uint8_t fillPercent() const;
  bool canLogTest() const { return fillPercent() < Config::LOG_TEST_FILL_LIMIT_PERCENT; }
  bool hasRoomForMeasRun() const;

  // Streaming CSV export, one line at a time - used by WifiOffload's HTTP handler.
  bool beginExport();
  bool nextCsvLine(String &outLine);
  void endExport();

  static const char *csvHeader() {
    return "session_id,seq,direction,type,config,session_time,size_bytes,rssi_dbm,snr_db,status,received,event,tx_time_ms,burst_sent,burst_recv\n";
  }

private:
  static constexpr auto LOG_PATH = "/log.bin";
  static constexpr size_t RING_CAPACITY = 32;

  File logFile_;
  File exportFile_;

  LogRecord ring_[RING_CAPACITY]{};
  volatile size_t ringHead_ = 0; // next write slot
  volatile size_t ringTail_ = 0; // next slot to flush
  uint32_t nextSeq_ = 0;
  uint32_t lastFlushMs_ = 0;
  bool dirty_ = false;
};
