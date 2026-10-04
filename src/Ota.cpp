#include "Ota.h"

#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include <nvs.h>
#include <cstring>

#include "Config.h"
#include "OtaRoots.h"
#include "SysLog.h"
#include "Version.h"

// Keep a freshly OTA-installed image "pending verify" until confirmBoot() decides (the core's
// weak default marks every image valid at start-up).
extern "C" bool verifyRollbackLater() { return true; }

namespace {

enum class Stage : uint8_t { Manifest, Download, Finished };
enum class Outcome : uint8_t { Installed, Available, UpToDate, Failed, Cancelled };
enum class Fail : uint8_t { None, Wifi, Network, Manifest, NoImage, Size, Flash, Hash, Cancelled };

// Worker <-> protocol core. The worker writes everything except `cancel`; the protocol core
// reads the result fields only after `stage == Finished`.
struct Shared {
  char url[Config::OTA_URL_MAX + 1];
  char role[8];
  bool install;
  bool force;
  volatile Stage stage;
  volatile uint32_t done;
  volatile uint32_t total;
  volatile bool cancel;
  Outcome outcome;
  Fail fail;
  char detail[48];  // English, for the console / system log
  char version[24]; // version offered by the manifest
};
Shared g_job{};
uint8_t g_buf[Config::OTA_CHUNK_BYTES];

struct Semver {
  unsigned major = 0, minor = 0, patch = 0;
};
Semver parseSemver(const char *s) {
  Semver v;
  sscanf(s, "%u.%u.%u", &v.major, &v.minor, &v.patch);
  return v;
}
int compare(const Semver &a, const Semver &b) {
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  return 0;
}

// Minimal lookup in the flat manifest we generate ourselves (scripts/make_manifest.py):
// finds "key" in [from, end) and copies its string or number value.
const char *findKey(const char *from, const char *end, const char *key) {
  char quoted[24];
  snprintf(quoted, sizeof(quoted), "\"%s\"", key);
  const size_t qlen = strlen(quoted);
  for (const char *p = from; p + qlen <= end; p++) {
    if (memcmp(p, quoted, qlen) != 0) continue;
    p += qlen;
    while (p < end && (*p == ' ' || *p == ':' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    return p < end ? p : nullptr;
  }
  return nullptr;
}
bool jsonString(const char *from, const char *end, const char *key, char *out, size_t len) {
  const char *p = findKey(from, end, key);
  if (!p || *p != '"') return false;
  p++;
  size_t n = 0;
  while (p < end && *p != '"' && n + 1 < len) out[n++] = *p++;
  out[n] = '\0';
  return p < end && *p == '"';
}
bool jsonNumber(const char *from, const char *end, const char *key, uint32_t &out) {
  const char *p = findKey(from, end, key);
  if (!p || *p < '0' || *p > '9') return false;
  out = strtoul(p, nullptr, 10);
  return true;
}

void setFail(const Fail f, const char *fmt, ...) {
  g_job.fail = f;
  g_job.outcome = f == Fail::Cancelled ? Outcome::Cancelled : Outcome::Failed;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_job.detail, sizeof(g_job.detail), fmt, ap);
  va_end(ap);
}

bool beginGet(HTTPClient &http, WiFiClient &plain, WiFiClientSecure &tls, const char *url) {
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); // GitHub -> release-assets host
  http.setTimeout(Config::OTA_HTTP_TIMEOUT_MS);
  http.setConnectTimeout(Config::OTA_HTTP_TIMEOUT_MS);
  if (strncmp(url, "https://", 8) == 0) {
    tls.setCACert(OtaRoots::PEM);
    return http.begin(tls, url);
  }
  return http.begin(plain, url);
}

struct ImageInfo {
  char url[256];
  uint32_t size;
  char sha256[65];
};

bool fetchManifest(ImageInfo &img) {
  WiFiClient plain;
  WiFiClientSecure tls;
  HTTPClient http;
  if (!beginGet(http, plain, tls, g_job.url)) {
    setFail(Fail::Manifest, "bad manifest url");
    return false;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    setFail(Fail::Network, "manifest: http %d", code);
    http.end();
    return false;
  }
  const int len = http.getSize();
  if (len > static_cast<int>(sizeof(g_buf)) - 1) {
    setFail(Fail::Manifest, "manifest too large (%d B)", len);
    http.end();
    return false;
  }
  // Read into the shared chunk buffer: no heap String for the body.
  WiFiClient *s = http.getStreamPtr();
  size_t n = 0;
  const uint32_t startMs = millis();
  while ((len < 0 || n < static_cast<size_t>(len)) && n < sizeof(g_buf) - 1 &&
         millis() - startMs < Config::OTA_HTTP_TIMEOUT_MS) {
    const int avail = s->available();
    if (avail > 0) {
      n += s->readBytes(g_buf + n, std::min(static_cast<size_t>(avail), sizeof(g_buf) - 1 - n));
    } else if (!s->connected()) {
      break;
    } else {
      vTaskDelay(1);
    }
  }
  http.end();
  g_buf[n] = '\0';
  const char *body = reinterpret_cast<const char *>(g_buf);
  const char *end = body + n;

  char project[24] = {};
  if (!jsonString(body, end, "project", project, sizeof(project)) || strcmp(project, Version::PROJECT) != 0) {
    setFail(Fail::Manifest, "manifest: wrong project '%s'", project);
    return false;
  }
  if (!jsonString(body, end, "version", g_job.version, sizeof(g_job.version))) {
    setFail(Fail::Manifest, "manifest: no version");
    return false;
  }
  const char *images = findKey(body, end, "images");
  const char *entry = images ? findKey(images, end, g_job.role) : nullptr;
  const char *entryEnd = entry ? static_cast<const char *>(memchr(entry, '}', end - entry)) : nullptr;
  if (!entryEnd || !jsonString(entry, entryEnd, "url", img.url, sizeof(img.url)) ||
      !jsonNumber(entry, entryEnd, "size", img.size) ||
      !jsonString(entry, entryEnd, "sha256", img.sha256, sizeof(img.sha256)) || strlen(img.sha256) != 64) {
    setFail(Fail::NoImage, "manifest: no '%s' image", g_job.role);
    return false;
  }
  return true;
}

bool download(const ImageInfo &img) {
  WiFiClient plain;
  WiFiClientSecure tls;
  HTTPClient http;
  if (!beginGet(http, plain, tls, img.url)) {
    setFail(Fail::Manifest, "bad image url");
    return false;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    setFail(Fail::Network, "image: http %d", code);
    http.end();
    return false;
  }
  const int len = http.getSize();
  if (len >= 0 && static_cast<uint32_t>(len) != img.size) {
    setFail(Fail::Size, "image: %d B, manifest %u B", len, static_cast<unsigned>(img.size));
    http.end();
    return false;
  }
  if (!Update.begin(img.size, U_FLASH)) {
    setFail(Fail::Flash, "update begin: %s", Update.errorString());
    http.end();
    return false;
  }
  g_job.total = img.size;
  g_job.stage = Stage::Download;

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts_ret(&sha, 0);

  WiFiClient *s = http.getStreamPtr();
  uint32_t lastDataMs = millis();
  bool ok = true;
  while (g_job.done < img.size) {
    if (g_job.cancel) {
      setFail(Fail::Cancelled, "cancelled");
      ok = false;
      break;
    }
    const int avail = s->available();
    if (avail > 0) {
      const size_t want = std::min({static_cast<size_t>(avail), sizeof(g_buf),
                                    static_cast<size_t>(img.size - g_job.done)});
      const size_t got = s->readBytes(g_buf, want);
      mbedtls_sha256_update_ret(&sha, g_buf, got);
      if (Update.write(g_buf, got) != got) {
        setFail(Fail::Flash, "write: %s", Update.errorString());
        ok = false;
        break;
      }
      g_job.done += got;
      lastDataMs = millis();
    } else if (!s->connected()) {
      setFail(Fail::Network, "connection lost at %u B", static_cast<unsigned>(g_job.done));
      ok = false;
      break;
    } else if (millis() - lastDataMs >= Config::OTA_HTTP_TIMEOUT_MS) {
      setFail(Fail::Network, "no data for %u s", static_cast<unsigned>(Config::OTA_HTTP_TIMEOUT_MS / 1000));
      ok = false;
      break;
    } else {
      vTaskDelay(1);
    }
  }
  http.end();

  uint8_t digest[32];
  mbedtls_sha256_finish_ret(&sha, digest);
  mbedtls_sha256_free(&sha);
  if (ok) {
    char hex[65];
    for (size_t i = 0; i < sizeof(digest); i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    if (strcasecmp(hex, img.sha256) != 0) {
      setFail(Fail::Hash, "sha256 mismatch");
      ok = false;
    }
  }
  if (!ok) {
    Update.abort(); // the running image and the boot partition stay as they were
    return false;
  }
  if (!Update.end()) {
    setFail(Fail::Flash, "update end: %s", Update.errorString());
    return false;
  }
  g_job.outcome = Outcome::Installed;
  return true;
}

void worker(void *) {
  ImageInfo img{};
  if (fetchManifest(img)) {
    const int cmp = compare(parseSemver(g_job.version), parseSemver(Version::FIRMWARE));
    if (!g_job.install) {
      g_job.outcome = cmp > 0 ? Outcome::Available : Outcome::UpToDate;
      g_job.total = img.size;
    } else if (cmp <= 0 && !g_job.force) {
      g_job.outcome = Outcome::UpToDate;
    } else {
      download(img);
    }
  }
  g_job.stage = Stage::Finished; // last write: the protocol core reads the result after this
  vTaskDelete(nullptr);
}

// ---- NVS: role pinned at the first boot, last reported rollback ----

uint8_t roleCode() { return Config::DEVICE_ROLE == Config::Role::Base ? 1 : 2; }

} // namespace

Ota::Ota(WifiOffload &wifi, RadioManager &radio, SessionState &session, ConfigStore &config)
  : wifi_(wifi), radio_(radio), session_(session), config_(config) {}

void Ota::confirmBoot(const bool postOk) {
  nvs_handle_t h;
  const bool nvsOk = nvs_open(Config::DEV_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK;
  uint8_t storedRole = 0;
  if (nvsOk && nvs_get_u8(h, "role", &storedRole) != ESP_OK) {
    storedRole = roleCode(); // first boot of this board: pin the role it was flashed with
    nvs_set_u8(h, "role", storedRole);
    nvs_commit(h);
  }
  const bool roleOk = !nvsOk || storedRole == roleCode();

  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
    if (postOk && roleOk) {
      esp_ota_mark_app_valid_cancel_rollback();
      if (nvsOk) nvs_erase_key(h, "rb_label");
      SLOG_I("ota", "new image confirmed in %s", running->label);
    } else {
      SLOG_E("ota", "new image rejected (%s), rolling back", postOk ? "role mismatch" : "POST failed");
      SysLog::sync();
      if (nvsOk) nvs_close(h);
      esp_ota_mark_app_invalid_rollback_and_reboot(); // returns only if there is nothing to go back to
      SLOG_E("ota", "no previous image to roll back to");
      return;
    }
  } else if (!roleOk) {
    SLOG_W("ota", "role differs from the first boot (%s now)", Version::roleName());
  }

  // Report a rollback once, on the first boot of the image we fell back to.
  const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
  if (invalid && nvsOk) {
    char seen[8] = {};
    size_t len = sizeof(seen);
    if (nvs_get_str(h, "rb_label", seen, &len) != ESP_OK || strcmp(seen, invalid->label) != 0) {
      SLOG_W("ota", "update in %s rejected, rolled back", invalid->label);
      nvs_set_str(h, "rb_label", invalid->label);
      nvs_commit(h);
    }
  }
  if (nvsOk) nvs_close(h);
}

const char *Ota::start(const bool install, const bool force) {
  if (active()) return "an update is already running";
  if (session_.state() != Protocol::DeviceState::Idle) return "only in IDLE - finish the sync first";
  const char *url = config_.get().otaUrl;
  if (url[0] == '\0') return "ota_url is empty";

  memset(&g_job, 0, sizeof(g_job));
  strlcpy(g_job.url, url, sizeof(g_job.url));
  strlcpy(g_job.role, Version::roleName(), sizeof(g_job.role));
  g_job.install = install;
  g_job.force = force;
  install_ = install;
  force_ = force;
  resultPending_ = false;

  pausedRadio_ = !radio_.isPaused();
  if (pausedRadio_) radio_.pause();
  startedWifi_ = false;
  if (wifi_.phase() == WifiOffload::Phase::Idle || wifi_.phase() == WifiOffload::Phase::Failed) {
    wifi_.stop();
    wifi_.start();
    startedWifi_ = true;
  }
  SLOG_I("ota", "%s%s started", install ? "update" : "check", force ? " (force)" : "");
  phase_ = Phase::WaitWifi;
  phaseStartMs_ = millis();
  return nullptr;
}

void Ota::startWorker() {
  g_job.stage = Stage::Manifest;
  phase_ = Phase::Manifest;
  if (xTaskCreatePinnedToCore(worker, "ota", Config::OTA_TASK_STACK_BYTES, nullptr, Config::OTA_TASK_PRIORITY,
                              nullptr, Config::OTA_TASK_CORE) != pdPASS) {
    setFail(Fail::Network, "no memory for the worker task");
    g_job.stage = Stage::Finished;
  }
}

void Ota::cancel() {
  if (phase_ == Phase::WaitWifi) {
    setFail(Fail::Cancelled, "cancelled");
    finish();
  } else if (phase_ == Phase::Manifest || phase_ == Phase::Download) {
    g_job.cancel = true; // the worker stops at the next chunk (a manifest fetch runs to its end)
  }
}

void Ota::finish() {
  if (startedWifi_) wifi_.stop();
  if (pausedRadio_) radio_.resume();
  startedWifi_ = pausedRadio_ = false;
  phase_ = Phase::Done;
  resultPending_ = true;
}

void Ota::loopTask() {
  switch (phase_) {
    case Phase::Idle:
    case Phase::Done:
      return;

    case Phase::WaitWifi:
      if (wifi_.phase() == WifiOffload::Phase::Connected) {
        startWorker();
      } else if (wifi_.phase() == WifiOffload::Phase::Failed) {
        setFail(Fail::Wifi, "no Wi-Fi network reachable");
        SLOG_W("ota", "%s", g_job.detail);
        finish();
      }
      return;

    case Phase::Manifest:
    case Phase::Download:
      if (g_job.stage == Stage::Download && phase_ == Phase::Manifest) {
        phase_ = Phase::Download;
        SLOG_I("ota", "downloading %s, %u B", g_job.version, static_cast<unsigned>(g_job.total));
      }
      if (g_job.stage != Stage::Finished) return;
      switch (g_job.outcome) {
        case Outcome::Installed:
          SLOG_I("ota", "installed %s, rebooting", g_job.version);
          phase_ = Phase::Rebooting;
          rebootAtMs_ = millis() + Config::OTA_REBOOT_DELAY_MS;
          resultPending_ = true;
          return;
        case Outcome::Available:
          SLOG_I("ota", "available %s (running %s)", g_job.version, Version::FIRMWARE);
          break;
        case Outcome::UpToDate:
          SLOG_I("ota", "up to date (manifest %s)", g_job.version);
          break;
        case Outcome::Failed:
          SLOG_E("ota", "%s", g_job.detail);
          break;
        case Outcome::Cancelled:
          SLOG_W("ota", "cancelled");
          break;
      }
      finish();
      return;

    case Phase::Rebooting:
      if (static_cast<int32_t>(millis() - rebootAtMs_) >= 0) {
        SysLog::sync();
        ESP.restart();
      }
      return;
  }
}

int8_t Ota::percent() const {
  if (phase_ == Phase::Rebooting) return 100;
  if (phase_ != Phase::Download || g_job.total == 0) return -1;
  return static_cast<int8_t>(static_cast<uint64_t>(g_job.done) * 100 / g_job.total);
}

namespace {
const char *failText(const Fail f) {
  switch (f) {
    case Fail::Wifi: return "немає Wi-Fi";
    case Fail::Network: return "немає зв'язку з сервером";
    case Fail::Manifest: return "хибний маніфест";
    case Fail::NoImage: return "немає образу для ролі";
    case Fail::Size: return "не збігся розмір";
    case Fail::Flash: return "помилка запису";
    case Fail::Hash: return "не збігся SHA-256";
    case Fail::Cancelled: return "скасовано";
    case Fail::None: break;
  }
  return "";
}
} // namespace

void Ota::screenText(char *buf, const size_t len) const {
  switch (phase_) {
    case Phase::WaitWifi:
      snprintf(buf, len, "Оновлення: Wi-Fi…\nНатисніть, щоб скасувати");
      return;
    case Phase::Manifest:
      snprintf(buf, len, "Перевірка версії…\nНатисніть, щоб скасувати");
      return;
    case Phase::Download:
      snprintf(buf, len, "Завантаження %s\nНатисніть, щоб скасувати", g_job.version);
      return;
    case Phase::Rebooting:
      snprintf(buf, len, "Встановлено %s\nПерезавантаження…", g_job.version);
      return;
    case Phase::Done:
      switch (g_job.outcome) {
        case Outcome::Available:
          snprintf(buf, len, "Доступна версія %s", g_job.version);
          return;
        case Outcome::UpToDate:
          snprintf(buf, len, "Встановлено останню версію (%s)", Version::FIRMWARE);
          return;
        case Outcome::Cancelled:
          snprintf(buf, len, "Оновлення скасовано");
          return;
        case Outcome::Failed:
          snprintf(buf, len, "Оновлення не вдалося: %s", failText(g_job.fail));
          return;
        case Outcome::Installed:
          break;
      }
      break;
    case Phase::Idle:
      break;
  }
  if (len) buf[0] = '\0';
}

bool Ota::popResult(char *buf, const size_t len) {
  if (!resultPending_ || phase_ != Phase::Done) return false;
  resultPending_ = false;
  screenText(buf, len);
  return true;
}

void Ota::printStatus(Print &out) const {
  out.printf("running:  %s (%s)\n", Version::FIRMWARE, Version::runningPartition());
  out.printf("manifest: %s\n", config_.get().otaUrl);
  const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
  if (invalid) out.printf("rejected: %s (rolled back)\n", invalid->label);
  switch (phase_) {
    case Phase::Idle:
      out.println(F("state:    idle"));
      return;
    case Phase::WaitWifi:
      out.println(F("state:    connecting Wi-Fi"));
      return;
    case Phase::Manifest:
      out.println(F("state:    reading manifest"));
      return;
    case Phase::Download:
      out.printf("state:    downloading %s, %u / %u B (%d%%)\n", g_job.version, static_cast<unsigned>(g_job.done),
                 static_cast<unsigned>(g_job.total), percent());
      return;
    case Phase::Rebooting:
      out.printf("state:    installed %s, rebooting\n", g_job.version);
      return;
    case Phase::Done:
      break;
  }
  switch (g_job.outcome) {
    case Outcome::Available:
      out.printf("last:     %s available (%u B) - `ota update` installs it\n", g_job.version,
                 static_cast<unsigned>(g_job.total));
      return;
    case Outcome::UpToDate:
      out.printf("last:     up to date (manifest %s)%s\n", g_job.version, install_ ? " - `ota update --force` reinstalls" : "");
      return;
    case Outcome::Failed:
    case Outcome::Cancelled:
      out.printf("last:     failed - %s\n", g_job.detail);
      return;
    case Outcome::Installed:
      return;
  }
}

void Ota::cmdOta(void *self, const int argc, char **argv, Console::Context &ctx) {
  auto *ota = static_cast<Ota *>(self);
  Print &out = ctx.out;
  const char *sub = argc >= 2 ? argv[1] : "status";
  if (strcasecmp(sub, "status") == 0) {
    ota->printStatus(out);
    return;
  }
  if (strcasecmp(sub, "cancel") == 0) {
    ota->cancel();
    out.println(F("cancel requested"));
    return;
  }
  const bool check = strcasecmp(sub, "check") == 0;
  if (!check && strcasecmp(sub, "update") != 0) {
    out.println(F("usage: ota [status] | check | update [--force] | cancel"));
    return;
  }
  const bool force = argc >= 3 && strcmp(argv[2], "--force") == 0;
  const char *err = ota->start(!check, force);
  if (err) {
    out.printf("error: %s\n", err);
    return;
  }
  out.println(F("started - progress: `ota status` / `log`"));
}

void Ota::registerCommands(Console &console) {
  console.add("ota", "ota [status] | check | update [--force] | cancel", cmdOta, this);
}
