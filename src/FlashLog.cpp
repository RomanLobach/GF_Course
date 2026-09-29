#include "FlashLog.h"
#include <LittleFS.h>
#include "Config.h"
#include "Protocol.h"

namespace {
const char *deviceStatusName(uint8_t status) {
  switch (status) {
    case 0: return "IDLE";
    case 1: return "SRCH";
    case 2: return "SEEN";
    case 3: return "SYNC";
    case 4: return "MEAS";
    case 5: return "DONE"; // logs from older firmware only
    case 6: return "TEST";
    default: return "????";
  }
}

String configLabel(const uint8_t configIndex) {
  if (configIndex < 1 || configIndex > Config::BENCH_CONFIG_COUNT) return "";
  const Config::BenchConfig &cfg = Config::BENCH_CONFIGS[configIndex - 1];
  if (cfg.modulation == Config::Modulation::LoRaMod) {
    return "SF" + String(cfg.sf) + "/" + String(static_cast<int>(cfg.bwKhz));
  }
  return "FSK" + String(cfg.bitrate);
}

const char *eventKindName(uint8_t kind) {
  switch (static_cast<LogEventKind>(kind)) {
    case LogEventKind::SyncSuccess: return "SYNC_OK";
    case LogEventKind::SyncFail: return "SYNC_FAIL";
    case LogEventKind::ManualStop: return "MANUAL_STOP";
    case LogEventKind::MeasureDone: return "MEAS_DONE";
    case LogEventKind::MeasStopped: return "MEAS_STOP";
  }
  return "?";
}

String fmtSessionTime(const uint32_t ms) {
  const uint32_t totalSec = ms / 1000;
  const uint32_t h = totalSec / 3600;
  const uint32_t m = totalSec % 3600 / 60;
  const uint32_t s = totalSec % 60;
  char buf[16];
  snprintf(buf, sizeof(buf), "T+%02u:%02u:%02u", static_cast<unsigned>(h), static_cast<unsigned>(m), static_cast<unsigned>(s));
  return {buf};
}
} // namespace

bool FlashLog::begin() {
  if (!LittleFS.begin(true)) { // format on mount failure
    Serial.println(F("LittleFS mount failed"));
    return false;
  }

  File sizeCheck = LittleFS.open(LOG_PATH, "r");
  size_t fileSize = sizeCheck ? sizeCheck.size() : 0;
  if (sizeCheck) sizeCheck.close();

  if (fileSize % sizeof(LogRecord) != 0) {
    // LogRecord has no on-flash schema version, so a firmware update that
    // changes its packed size (e.g. adding a field) leaves old records at the
    // wrong stride - reading them with the new struct misaligns every field
    // after the boundary (session id, config, rssi, ...) into garbage. A
    // size mismatch is the only signal available that this happened, so
    // treat it as stale/incompatible and drop the file rather than export
    // corrupted data.
    Serial.println(F("log file size mismatches current LogRecord size - erasing stale log"));
    LittleFS.remove(LOG_PATH);
    fileSize = 0;
  }
  nextSeq_ = fileSize / sizeof(LogRecord);

  logFile_ = LittleFS.open(LOG_PATH, "a");
  if (!logFile_) {
    Serial.println(F("log file open failed"));
    return false;
  }
  return true;
}

void FlashLog::logEvent(const LogRecord &rec) {
  const size_t nextHead = (ringHead_ + 1) % RING_CAPACITY;
  if (nextHead == ringTail_) {
    // Ring full (flash write falling behind) - drop the oldest pending record
    // rather than block the caller.
    ringTail_ = (ringTail_ + 1) % RING_CAPACITY;
  }
  ring_[ringHead_] = rec;
  ringHead_ = nextHead;
}

void FlashLog::loopTask(const bool allowFlashIo) {
  if (!allowFlashIo) return;

  // Drain everything pending in one batch - each record is a small append into
  // LittleFS's cache; the expensive part is flush(), rate-limited below.
  while (ringTail_ != ringHead_) {
    LogRecord rec = ring_[ringTail_];
    rec.seq = static_cast<uint16_t>(nextSeq_++);
    logFile_.write(reinterpret_cast<const uint8_t *>(&rec), sizeof(rec));
    ringTail_ = (ringTail_ + 1) % RING_CAPACITY;
    dirty_ = true;
  }
  if (dirty_ && millis() - lastFlushMs_ >= 1000) {
    logFile_.flush();
    dirty_ = false;
    lastFlushMs_ = millis();
  }
}

uint8_t FlashLog::fillPercent() const {
  const size_t total = LittleFS.totalBytes();
  if (total == 0) return 100;
  return static_cast<uint8_t>(LittleFS.usedBytes() * 100 / total);
}

bool FlashLog::hasRoomForMeasRun() const {
  const size_t total = LittleFS.totalBytes();
  const size_t used = LittleFS.usedBytes();
  if (used >= total) return false;
  return (total - used) / sizeof(LogRecord) >= Config::LOG_MEAS_RESERVE_RECORDS;
}

bool FlashLog::eraseAll() {
  logFile_.close();
  LittleFS.remove(LOG_PATH);
  logFile_ = LittleFS.open(LOG_PATH, "a");
  ringHead_ = 0;
  ringTail_ = 0;
  nextSeq_ = 0;
  dirty_ = false;
  return logFile_;
}

bool FlashLog::beginExport() {
  exportFile_ = LittleFS.open(LOG_PATH, "r");
  return exportFile_;
}

bool FlashLog::nextCsvLine(String &outLine) {
  LogRecord rec{};
  const size_t n = exportFile_.read(reinterpret_cast<uint8_t *>(&rec), sizeof(rec));
  if (n != sizeof(rec)) return false;

  char sessionIdBuf[8];
  snprintf(sessionIdBuf, sizeof(sessionIdBuf), "%04u", static_cast<unsigned>(rec.sessionId % 10000));
  const auto type = static_cast<LogRecordType>(rec.type);
  const bool isEvent = type == LogRecordType::SessionEvent;
  const bool isTest = type == LogRecordType::TestSummary;
  const char *typeName = isEvent ? "EVT" : isTest ? "TEST" : "BENCH";

  outLine = String(sessionIdBuf) + "," +
            String(rec.seq) + "," +
            (isEvent ? "" : rec.direction == 0 ? "TX" : "RX") + "," +
            typeName + "," +
            (isEvent ? "" : configLabel(rec.configIndex)) + "," +
            fmtSessionTime(rec.sessionTimeMs) + "," +
            (isEvent ? "" : String(rec.size)) + "," +
            (rec.rssi == NO_RSSI ? "" : String(rec.rssi)) + "," +
            (rec.snrTenths == NO_SNR ? "" : String(static_cast<float>(rec.snrTenths) / 10.0f, 1)) + "," +
            deviceStatusName(rec.deviceStatus) + "," +
            (type == LogRecordType::Bench ? (rec.received ? "1" : "0") : "") + "," +
            (isEvent ? eventKindName(rec.eventKind) : "") + "," +
            (rec.txTimeUs == 0 ? "" : String(static_cast<float>(rec.txTimeUs) / 1000.0f, 2)) + "," +
            (isTest ? String(rec.eventKind) : "") + "," +
            (isTest && rec.received != 0xFF ? String(rec.received) : "");
  return true;
}

void FlashLog::endExport() {
  exportFile_.close();
}
