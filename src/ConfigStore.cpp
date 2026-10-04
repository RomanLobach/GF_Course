#include "ConfigStore.h"

#include <Preferences.h>
#include <nvs.h>
#include <esp_rom_crc.h>
#include "SysLog.h"

namespace {

constexpr uint16_t MAGIC = 0xC0F1;
constexpr const char *SLOT_KEYS[2] = {"a", "b"};
constexpr char SLOT_NAMES[2] = {'A', 'B'};

#pragma pack(push, 1)
struct BlobHeader {
  uint16_t magic;
  uint16_t version;    // payload schema
  uint32_t generation; // +1 on every save; the higher one of the two slots is current
  uint16_t payloadLen;
};
// Schema 1. Never change it in place: a new schema gets its own struct + decode case.
struct PayloadV1 {
  uint16_t sessionId;
  uint8_t logLevel;
  char otaUrl[Config::OTA_URL_MAX + 1];
};
// Schema 2 (1.0.0): + Wi-Fi connect timeout and setup-portal idle timeout, both formerly
// fixed in the firmware. A schema-1 blob migrates with the old built-in values.
struct PayloadV2 {
  uint16_t sessionId;
  uint8_t logLevel;
  char otaUrl[Config::OTA_URL_MAX + 1];
  uint8_t wifiTimeoutS;
  uint8_t portalTimeoutMin;
};
#pragma pack(pop)
static_assert(sizeof(BlobHeader) == 10, "config blob header layout");
static_assert(sizeof(PayloadV1) == 132, "config schema 1 layout");
static_assert(sizeof(PayloadV2) == 134, "config schema 2 layout");
constexpr size_t CRC_LEN = 4;
constexpr size_t BLOB_MAX = 256; // largest blob any schema may use
static_assert(sizeof(BlobHeader) + sizeof(PayloadV2) + CRC_LEN <= BLOB_MAX, "config blob too large");

enum class ParamType : uint8_t { UInt, Text };
struct ParamDef {
  const char *key;
  ParamType type;
  uint32_t min; // UInt: value range; Text: length range
  uint32_t max;
  bool readOnly;
  const char *help;
};
// The bounds table: set() and load both validate against it.
constexpr ParamDef PARAMS[] = {
  {"session_id", ParamType::UInt, 0, 9999, true, "last session id (reset only from the viewer, with the logs)"},
  {"log_level", ParamType::UInt, 0, 3, false, "system log level at boot: 0 error, 1 warn, 2 info, 3 debug"},
  {"ota_url", ParamType::Text, 10, Config::OTA_URL_MAX, false, "update manifest URL (http:// or https://)"},
  {"wifi_timeout_s", ParamType::UInt, 3, 30, false, "seconds to wait for each Wi-Fi network before the next one"},
  {"portal_timeout_min", ParamType::UInt, 1, 60, false, "Wi-Fi setup portal closes after this many idle minutes"},
};
constexpr size_t PARAM_COUNT = sizeof(PARAMS) / sizeof(PARAMS[0]);
enum ParamIndex : size_t { P_SESSION_ID, P_LOG_LEVEL, P_OTA_URL, P_WIFI_TIMEOUT, P_PORTAL_TIMEOUT };

int findParam(const char *key) {
  for (size_t i = 0; i < PARAM_COUNT; i++) {
    if (strcasecmp(key, PARAMS[i].key) == 0) return static_cast<int>(i);
  }
  return -1;
}

uint32_t crc32(const uint8_t *data, const size_t len) {
  return esp_rom_crc32_le(0, data, len);
}

bool validUrl(const char *s) {
  const size_t len = strnlen(s, Config::OTA_URL_MAX + 1);
  const ParamDef &p = PARAMS[P_OTA_URL];
  if (len < p.min || len > p.max) return false;
  if (strncmp(s, "http://", 7) != 0 && strncmp(s, "https://", 8) != 0) return false;
  for (size_t i = 0; i < len; i++) {
    if (s[i] <= ' ' || s[i] > '~') return false;
  }
  return true;
}

bool parseUInt(const char *s, uint32_t &out) {
  if (!s || !*s) return false;
  char *end = nullptr;
  const unsigned long v = strtoul(s, &end, 10);
  if (*end != '\0' || s[0] == '-') return false;
  out = static_cast<uint32_t>(v);
  return true;
}

// Every stored schema decodes into the current in-RAM DeviceConfig here; a new schema adds
// a case and fills the fields the older payload doesn't have with defaults.
bool decodePayload(const uint16_t version, const uint8_t *p, const size_t len, DeviceConfig &out) {
  switch (version) {
    case 1: {
      if (len != sizeof(PayloadV1)) return false;
      PayloadV1 v1{};
      memcpy(&v1, p, sizeof(v1));
      out.sessionId = v1.sessionId;
      out.logLevel = v1.logLevel;
      memcpy(out.otaUrl, v1.otaUrl, sizeof(out.otaUrl));
      out.otaUrl[Config::OTA_URL_MAX] = '\0';
      // Fields schema 1 didn't have keep the defaults (= the values 0.4.0 had built in).
      return true;
    }
    case 2: {
      if (len != sizeof(PayloadV2)) return false;
      PayloadV2 v2{};
      memcpy(&v2, p, sizeof(v2));
      out.sessionId = v2.sessionId;
      out.logLevel = v2.logLevel;
      memcpy(out.otaUrl, v2.otaUrl, sizeof(out.otaUrl));
      out.otaUrl[Config::OTA_URL_MAX] = '\0';
      out.wifiTimeoutS = v2.wifiTimeoutS;
      out.portalTimeoutMin = v2.portalTimeoutMin;
      return true;
    }
    default:
      return false;
  }
}

size_t encodePayload(const DeviceConfig &cfg, uint8_t *p) {
  PayloadV2 v2{};
  v2.sessionId = cfg.sessionId;
  v2.logLevel = cfg.logLevel;
  memcpy(v2.otaUrl, cfg.otaUrl, sizeof(v2.otaUrl));
  v2.wifiTimeoutS = cfg.wifiTimeoutS;
  v2.portalTimeoutMin = cfg.portalTimeoutMin;
  memcpy(p, &v2, sizeof(v2));
  return sizeof(v2);
}

} // namespace

// ============================================================================
// Load / save
// ============================================================================

void ConfigStore::setDefaults(DeviceConfig &cfg) {
  cfg.sessionId = 0;
  cfg.logLevel = static_cast<uint8_t>(LogLevel::Info); // -D APP_DEBUG_SERIAL overrides it at boot
  strlcpy(cfg.otaUrl, Config::OTA_DEFAULT_MANIFEST_URL, sizeof(cfg.otaUrl));
  cfg.wifiTimeoutS = Config::WIFI_CONNECT_TIMEOUT_DEFAULT_S;
  cfg.portalTimeoutMin = Config::PORTAL_IDLE_TIMEOUT_DEFAULT_MIN;
}

bool ConfigStore::sanitize(DeviceConfig &cfg) {
  DeviceConfig def;
  setDefaults(def);
  bool fixed = false;
  if (cfg.sessionId > PARAMS[P_SESSION_ID].max) {
    cfg.sessionId = def.sessionId;
    fixed = true;
  }
  if (cfg.logLevel > PARAMS[P_LOG_LEVEL].max) {
    cfg.logLevel = def.logLevel;
    fixed = true;
  }
  if (cfg.wifiTimeoutS < PARAMS[P_WIFI_TIMEOUT].min || cfg.wifiTimeoutS > PARAMS[P_WIFI_TIMEOUT].max) {
    cfg.wifiTimeoutS = def.wifiTimeoutS;
    fixed = true;
  }
  if (cfg.portalTimeoutMin < PARAMS[P_PORTAL_TIMEOUT].min || cfg.portalTimeoutMin > PARAMS[P_PORTAL_TIMEOUT].max) {
    cfg.portalTimeoutMin = def.portalTimeoutMin;
    fixed = true;
  }
  if (!validUrl(cfg.otaUrl)) {
    strlcpy(cfg.otaUrl, def.otaUrl, sizeof(cfg.otaUrl));
    fixed = true;
  }
  return fixed;
}

bool ConfigStore::readSlot(const uint8_t slot, DeviceConfig &out) {
  SlotInfo &info = slots_[slot];
  info = SlotInfo{};

  Preferences prefs;
  if (!prefs.begin(Config::CFG_NVS_NAMESPACE, true)) return false;
  if (!prefs.isKey(SLOT_KEYS[slot])) {
    prefs.end();
    return false;
  }
  uint8_t buf[BLOB_MAX];
  const size_t len = prefs.getBytesLength(SLOT_KEYS[slot]);
  const bool read = len >= sizeof(BlobHeader) + CRC_LEN && len <= BLOB_MAX &&
                    prefs.getBytes(SLOT_KEYS[slot], buf, len) == len;
  prefs.end();

  info.state = SlotState::Bad;
  if (!read) return false;
  BlobHeader h{};
  memcpy(&h, buf, sizeof(h));
  if (h.magic != MAGIC || sizeof(h) + h.payloadLen + CRC_LEN != len) return false;
  uint32_t storedCrc = 0;
  memcpy(&storedCrc, buf + len - CRC_LEN, CRC_LEN);
  if (crc32(buf, len - CRC_LEN) != storedCrc) return false;

  // Intact blob: its generation counts even if we can't read its schema.
  info.version = h.version;
  info.generation = h.generation;
  if (!anyGeneration_ || static_cast<int32_t>(h.generation - maxGeneration_) > 0) maxGeneration_ = h.generation;
  anyGeneration_ = true;

  if (h.version > SCHEMA_VERSION) {
    info.state = SlotState::Newer;
    return false;
  }
  DeviceConfig decoded;
  setDefaults(decoded);
  if (!decodePayload(h.version, buf + sizeof(h), h.payloadLen, decoded)) return false;
  info.state = SlotState::Ok;
  out = decoded;
  return true;
}

uint8_t ConfigStore::targetSlot() const {
  if (activeSlot_ >= 0) return static_cast<uint8_t>(1 - activeSlot_);
  for (uint8_t i = 0; i < 2; i++) {
    if (slots_[i].state == SlotState::Absent || slots_[i].state == SlotState::Bad) return i;
  }
  // Both hold a newer schema (we were rolled back): sacrifice the older of the two.
  return static_cast<int32_t>(slots_[0].generation - slots_[1].generation) <= 0 ? 0 : 1;
}

size_t ConfigStore::buildBlob(uint8_t *buf, const uint32_t generation) const {
  const size_t payloadLen = encodePayload(cfg_, buf + sizeof(BlobHeader));
  const BlobHeader h{MAGIC, SCHEMA_VERSION, generation, static_cast<uint16_t>(payloadLen)};
  memcpy(buf, &h, sizeof(h));
  const size_t len = sizeof(h) + payloadLen + CRC_LEN;
  const uint32_t crc = crc32(buf, len - CRC_LEN);
  memcpy(buf + len - CRC_LEN, &crc, CRC_LEN);
  return len;
}

bool ConfigStore::save() {
  uint8_t buf[BLOB_MAX];
  const uint32_t generation = anyGeneration_ ? maxGeneration_ + 1 : 1;
  const size_t len = buildBlob(buf, generation);

  const uint8_t slot = targetSlot();
  Preferences prefs;
  bool ok = prefs.begin(Config::CFG_NVS_NAMESPACE, false);
  if (ok) {
    uint8_t back[BLOB_MAX];
    ok = prefs.putBytes(SLOT_KEYS[slot], buf, len) == len && prefs.getBytes(SLOT_KEYS[slot], back, len) == len &&
         memcmp(buf, back, len) == 0;
    prefs.end();
  }
  if (!ok) {
    // The active slot is untouched, so the previous config still loads on the next boot.
    slots_[slot].state = SlotState::Bad;
    SLOG_E("cfg", "save to slot %c failed", SLOT_NAMES[slot]);
    return false;
  }
  slots_[slot].state = SlotState::Ok;
  slots_[slot].version = SCHEMA_VERSION;
  slots_[slot].generation = generation;
  activeSlot_ = static_cast<int8_t>(slot);
  maxGeneration_ = generation;
  anyGeneration_ = true;
  SLOG_D("cfg", "saved slot %c gen %lu", SLOT_NAMES[slot], static_cast<unsigned long>(generation));
  return true;
}

bool ConfigStore::migrateLegacy() {
  // Raw NVS API: Preferences logs an error for a namespace that doesn't exist, which is the
  // normal case on every board that never ran the pre-config firmware.
  nvs_handle_t h;
  if (nvs_open(Config::LEGACY_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
  uint16_t id = 0;
  const bool found = nvs_get_u16(h, Config::LEGACY_NVS_KEY_SESSION_ID, &id) == ESP_OK;
  nvs_close(h);
  if (!found) return false;

  cfg_.sessionId = id <= PARAMS[P_SESSION_ID].max ? id : 0;
  if (!save()) return true; // keep the legacy key, retry on the next boot
  if (nvs_open(Config::LEGACY_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
    nvs_erase_key(h, Config::LEGACY_NVS_KEY_SESSION_ID);
    nvs_commit(h);
    nvs_close(h);
  }
  SLOG_I("cfg", "migrated schema 0 -> %u, session id %u", SCHEMA_VERSION, cfg_.sessionId);
  return true;
}

bool ConfigStore::begin() {
  setDefaults(cfg_);

  // Read-write once, so the namespace exists and later read-only opens never fail.
  Preferences prefs;
  if (!prefs.begin(Config::CFG_NVS_NAMESPACE, false)) {
    loadResult_ = LoadResult::NvsError;
    SLOG_E("cfg", "NVS unavailable, defaults (not persisted)");
    return false;
  }
  prefs.end();

  DeviceConfig candidates[2];
  int8_t best = -1;
  for (uint8_t i = 0; i < 2; i++) {
    if (!readSlot(i, candidates[i])) continue;
    if (best < 0 || static_cast<int32_t>(slots_[i].generation - slots_[best].generation) > 0) best = static_cast<int8_t>(i);
  }

  const bool anyBad = slots_[0].state == SlotState::Bad || slots_[1].state == SlotState::Bad;
  const bool allAbsent = slots_[0].state == SlotState::Absent && slots_[1].state == SlotState::Absent;

  if (best >= 0) {
    cfg_ = candidates[best];
    activeSlot_ = best;
    const bool fixed = sanitize(cfg_);
    if (fixed) SLOG_W("cfg", "out-of-range value reset to default");
    if (slots_[best].version < SCHEMA_VERSION) {
      loadResult_ = LoadResult::Migrated;
      SLOG_I("cfg", "migrated schema %u -> %u", slots_[best].version, SCHEMA_VERSION);
      save();
    } else {
      loadResult_ = slots_[1 - best].state == SlotState::Bad ? LoadResult::Recovered : LoadResult::Loaded;
      if (fixed) save();
    }
  } else if (allAbsent) {
    loadResult_ = migrateLegacy() ? LoadResult::Migrated : LoadResult::Fresh;
    if (loadResult_ == LoadResult::Fresh) save();
  } else if (!anyBad) {
    // Only a newer schema is stored (rolled back): don't touch it, the newer firmware will
    // pick it up again. Defaults until something here gets saved.
    loadResult_ = LoadResult::NewerSchema;
  } else {
    loadResult_ = LoadResult::Corrupt;
    save();
  }

  const char slotName = activeSlot_ >= 0 ? SLOT_NAMES[activeSlot_] : '-';
  const auto gen = static_cast<unsigned long>(activeSlot_ >= 0 ? slots_[activeSlot_].generation : 0);
  if (loadResult_ == LoadResult::Corrupt) {
    SLOG_E("cfg", "both slots damaged, defaults (slot %c)", slotName);
  } else if (loadResult_ == LoadResult::Recovered || loadResult_ == LoadResult::NewerSchema) {
    SLOG_W("cfg", "%s, slot %c gen %lu", loadResultName(loadResult_), slotName, gen);
  } else {
    SLOG_I("cfg", "%s, slot %c gen %lu", loadResultName(loadResult_), slotName, gen);
  }
  return true;
}

// ============================================================================
// Access
// ============================================================================

bool ConfigStore::setSessionId(const uint16_t id) {
  if (id == cfg_.sessionId) return true;
  cfg_.sessionId = id;
  return save();
}

ConfigStore::SetResult ConfigStore::set(const char *key, const char *value) {
  const int idx = findParam(key);
  if (idx < 0) return SetResult::UnknownKey;
  const ParamDef &p = PARAMS[idx];
  if (p.readOnly) return SetResult::ReadOnly;

  const DeviceConfig before = cfg_;
  switch (static_cast<ParamIndex>(idx)) {
    case P_LOG_LEVEL: {
      uint32_t v = 0;
      LogLevel lvl;
      if (parseUInt(value, v) && v >= p.min && v <= p.max) {
        cfg_.logLevel = static_cast<uint8_t>(v);
      } else if (SysLog::parseLevel(value, lvl)) {
        cfg_.logLevel = static_cast<uint8_t>(lvl);
      } else {
        return SetResult::Invalid;
      }
      break;
    }
    case P_OTA_URL:
      if (!validUrl(value)) return SetResult::Invalid;
      strlcpy(cfg_.otaUrl, value, sizeof(cfg_.otaUrl));
      break;
    case P_WIFI_TIMEOUT:
    case P_PORTAL_TIMEOUT: {
      uint32_t v = 0;
      if (!parseUInt(value, v) || v < p.min || v > p.max) return SetResult::Invalid;
      (idx == P_WIFI_TIMEOUT ? cfg_.wifiTimeoutS : cfg_.portalTimeoutMin) = static_cast<uint8_t>(v);
      break;
    }
    case P_SESSION_ID:
      return SetResult::ReadOnly;
  }
  if (memcmp(&before, &cfg_, sizeof(cfg_)) == 0) return SetResult::Ok;
  if (!save()) {
    cfg_ = before;
    return SetResult::SaveFailed;
  }
  SLOG_I("cfg", "%s changed", p.key);
  return SetResult::Ok;
}

bool ConfigStore::stressWrite(const uint32_t n) {
  const DeviceConfig before = cfg_;
  snprintf(cfg_.otaUrl, sizeof(cfg_.otaUrl), "http://stress.test/%lu", static_cast<unsigned long>(n));
  if (save()) return true;
  cfg_ = before;
  return false;
}

bool ConfigStore::tearTest() {
  // The new value goes into a full blob with the next generation, but only its first half
  // reaches NVS - the length no longer matches the header, so the loader rejects the slot.
  const DeviceConfig before = cfg_;
  strlcpy(cfg_.otaUrl, "http://torn.write/", sizeof(cfg_.otaUrl));
  uint8_t buf[BLOB_MAX];
  const size_t len = buildBlob(buf, anyGeneration_ ? maxGeneration_ + 1 : 1);
  cfg_ = before;

  const uint8_t slot = targetSlot();
  Preferences prefs;
  if (!prefs.begin(Config::CFG_NVS_NAMESPACE, false)) return false;
  const bool ok = prefs.putBytes(SLOT_KEYS[slot], buf, len / 2) == len / 2;
  prefs.end();
  slots_[slot].state = SlotState::Bad;
  SLOG_W("cfg", "tear-test: %u of %u B into slot %c", static_cast<unsigned>(len / 2), static_cast<unsigned>(len),
         SLOT_NAMES[slot]);
  return ok;
}

bool ConfigStore::resetToDefaults() {
  const uint16_t sessionId = cfg_.sessionId;
  setDefaults(cfg_);
  cfg_.sessionId = sessionId;
  SLOG_I("cfg", "reset to defaults");
  return save();
}

bool ConfigStore::print(Print &out, const char *key) const {
  DeviceConfig def;
  setDefaults(def);
  bool any = false;
  for (size_t i = 0; i < PARAM_COUNT; i++) {
    const ParamDef &p = PARAMS[i];
    if (key && strcasecmp(key, p.key) != 0) continue;
    any = true;
    switch (static_cast<ParamIndex>(i)) {
      case P_SESSION_ID: out.printf("%-18s = %04u", p.key, cfg_.sessionId); break;
      case P_LOG_LEVEL:
        out.printf("%-18s = %u (%s)", p.key, cfg_.logLevel, SysLog::levelName(static_cast<LogLevel>(cfg_.logLevel)));
        break;
      case P_OTA_URL: out.printf("%-18s = %s", p.key, cfg_.otaUrl); break;
      case P_WIFI_TIMEOUT: out.printf("%-18s = %u", p.key, cfg_.wifiTimeoutS); break;
      case P_PORTAL_TIMEOUT: out.printf("%-18s = %u", p.key, cfg_.portalTimeoutMin); break;
    }
    if (p.type == ParamType::UInt) {
      out.printf("\n             range %lu..%lu", static_cast<unsigned long>(p.min), static_cast<unsigned long>(p.max));
    } else {
      out.printf("\n             length %lu..%lu", static_cast<unsigned long>(p.min), static_cast<unsigned long>(p.max));
    }
    if (p.readOnly) {
      out.print(", read-only");
    } else if (i == P_LOG_LEVEL) {
      out.printf(", default %u", def.logLevel);
    } else if (i == P_WIFI_TIMEOUT) {
      out.printf(", default %u", def.wifiTimeoutS);
    } else if (i == P_PORTAL_TIMEOUT) {
      out.printf(", default %u", def.portalTimeoutMin);
    } else {
      out.print(", default: GitHub hw6-latest");
    }
    out.printf("\n             %s\n", p.help);
  }
  return any;
}

void ConfigStore::printStatus(Print &out) const {
  static const char *const STATE_NAMES[] = {"absent", "damaged", "newer schema", "ok"};
  out.printf("config schema v%u, load: %s", SCHEMA_VERSION, loadResultName(loadResult_));
  if (activeSlot_ >= 0) out.printf(", active slot %c", SLOT_NAMES[activeSlot_]);
  out.println();
  for (uint8_t i = 0; i < 2; i++) {
    const SlotInfo &s = slots_[i];
    out.printf("  slot %c: %s", SLOT_NAMES[i], STATE_NAMES[static_cast<uint8_t>(s.state)]);
    if (s.state == SlotState::Ok || s.state == SlotState::Newer) {
      out.printf(", schema v%u, gen %lu", s.version, static_cast<unsigned long>(s.generation));
    }
    out.println();
  }
}

const char *ConfigStore::loadResultName(const LoadResult r) {
  switch (r) {
    case LoadResult::Loaded: return "loaded";
    case LoadResult::Recovered: return "recovered from the other slot";
    case LoadResult::Migrated: return "migrated";
    case LoadResult::Fresh: return "fresh defaults";
    case LoadResult::NewerSchema: return "newer schema only, defaults";
    case LoadResult::Corrupt: return "corrupt, defaults";
    case LoadResult::NvsError: return "NVS error";
  }
  return "?";
}
