// Persistent device config in NVS: one versioned blob, kept in two slots so a write torn by
// a power cut never loses the last good copy.
//
// Slot layout (NVS namespace Config::CFG_NVS_NAMESPACE, keys "a"/"b"):
//   BlobHeader (magic, schema version, generation, payload length) | payload | CRC32
// - Load: the valid slot (magic + length + CRC) with the highest generation wins. An older
//   schema is migrated forward and saved; a newer one (left behind by a rollback) is neither
//   read nor overwritten - defaults are used until something is saved.
// - No usable slot at all: the pre-config firmware's `lora_session/last_id` is migrated
//   (schema 0 -> 1) and removed afterwards; otherwise defaults.
// - Save: always into the slot that is NOT the active one, generation + 1, read back and
//   compared. Until it succeeds the previous slot stays the active one.
// - Values are range-checked against PARAMS[] on set() and on load (bad value -> default).
//
// Protocol core only. Writes happen only when a value changes (session id: once per session).
#pragma once

#include <Arduino.h>
#include "Config.h"

struct DeviceConfig {
  uint16_t sessionId = 0;                    // last used session id (service field)
  uint8_t logLevel = 2;                      // SysLog level at boot, 0 error .. 3 debug
  char otaUrl[Config::OTA_URL_MAX + 1] = {}; // update manifest URL
};

class ConfigStore {
public:
  static constexpr uint16_t SCHEMA_VERSION = 1;

  enum class LoadResult : uint8_t {
    Loaded,      // newest slot valid
    Recovered,   // one slot damaged, the other one used
    Migrated,    // older schema (or the legacy session-id key) converted and saved
    Fresh,       // nothing stored yet - defaults
    NewerSchema, // only a config from newer firmware - defaults, slots left intact
    Corrupt,     // slots present but none valid - defaults
    NvsError,    // NVS could not be opened - defaults, nothing persists
  };

  enum class SetResult : uint8_t { Ok, UnknownKey, ReadOnly, Invalid, SaveFailed };

  bool begin(); // false only if NVS itself is unusable

  const DeviceConfig &get() const { return cfg_; }
  bool setSessionId(uint16_t id);

  // Text interface (console): key/value per PARAMS[].
  SetResult set(const char *key, const char *value);
  bool resetToDefaults(); // everything except the session id
  bool print(Print &out, const char *key) const; // key == nullptr: all; false = unknown key
  void printStatus(Print &out) const;

  // Power-cut experiment: store ota_url = "http://stress.test/<n>" and save, without a SysLog
  // line per write (a long run would push the whole history out of the ring).
  bool stressWrite(uint32_t n);
  char activeSlotName() const { return activeSlot_ >= 0 ? (activeSlot_ == 0 ? 'A' : 'B') : '-'; }
  uint32_t generation() const { return maxGeneration_; }

  LoadResult loadResult() const { return loadResult_; }
  static const char *loadResultName(LoadResult r);
  // For POST: the stored config was damaged beyond recovery (or NVS is gone).
  bool healthy() const { return loadResult_ != LoadResult::Corrupt && loadResult_ != LoadResult::NvsError; }

private:
  enum class SlotState : uint8_t { Absent, Bad, Newer, Ok };
  struct SlotInfo {
    SlotState state = SlotState::Absent;
    uint16_t version = 0;
    uint32_t generation = 0;
  };

  DeviceConfig cfg_;
  LoadResult loadResult_ = LoadResult::Fresh;
  SlotInfo slots_[2];
  int8_t activeSlot_ = -1; // -1: nothing valid stored
  uint32_t maxGeneration_ = 0;
  bool anyGeneration_ = false;

  static void setDefaults(DeviceConfig &cfg);
  static bool sanitize(DeviceConfig &cfg); // true if something had to be reset
  bool readSlot(uint8_t slot, DeviceConfig &out);
  bool save();
  uint8_t targetSlot() const;
  bool migrateLegacy();
};
