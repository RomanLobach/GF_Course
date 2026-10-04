#include "WifiStore.h"

#include <nvs.h>

namespace WifiStore {
namespace {

// Raw NVS API: the namespace doesn't exist until the first `wifi add`, and Preferences
// would log an error for that on every Wi-Fi start.
void keyFor(char *buf, const char prefix, const size_t slot) {
  buf[0] = prefix;
  buf[1] = static_cast<char>('0' + slot);
  buf[2] = '\0';
}

bool validSsid(const char *s) {
  const size_t len = strnlen(s, SSID_MAX + 1);
  return len >= 1 && len <= SSID_MAX;
}

bool validPassword(const char *s) {
  // Empty = open network; WPA2 needs 8..63 characters.
  const size_t len = strnlen(s, PASSWORD_MAX + 1);
  return len == 0 || (len >= 8 && len <= PASSWORD_MAX);
}

} // namespace

bool read(const size_t slot, Network &out) {
  if (slot >= Config::WIFI_STORED_MAX) return false;
  nvs_handle_t h;
  if (nvs_open(Config::WIFI_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
  char key[3];
  keyFor(key, 's', slot);
  size_t len = sizeof(out.ssid);
  bool ok = nvs_get_str(h, key, out.ssid, &len) == ESP_OK && out.ssid[0] != '\0';
  if (ok) {
    keyFor(key, 'p', slot);
    len = sizeof(out.password);
    if (nvs_get_str(h, key, out.password, &len) != ESP_OK) out.password[0] = '\0';
  }
  nvs_close(h);
  return ok;
}

size_t count() {
  size_t n = 0;
  Network net{};
  for (size_t i = 0; i < Config::WIFI_STORED_MAX; i++) {
    if (read(i, net)) n++;
  }
  return n;
}

AddResult add(const char *ssid, const char *password, size_t &slotOut) {
  if (!validSsid(ssid) || !validPassword(password)) return AddResult::Invalid;

  int target = -1;
  int firstFree = -1;
  Network net{};
  for (size_t i = 0; i < Config::WIFI_STORED_MAX; i++) {
    if (!read(i, net)) {
      if (firstFree < 0) firstFree = static_cast<int>(i);
    } else if (strcmp(net.ssid, ssid) == 0) {
      target = static_cast<int>(i);
      break;
    }
  }
  const bool update = target >= 0;
  if (!update) target = firstFree;
  if (target < 0) return AddResult::Full;

  nvs_handle_t h;
  if (nvs_open(Config::WIFI_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return AddResult::NvsError;
  char key[3];
  keyFor(key, 'p', static_cast<size_t>(target));
  bool ok = nvs_set_str(h, key, password) == ESP_OK;
  keyFor(key, 's', static_cast<size_t>(target));
  ok = ok && nvs_set_str(h, key, ssid) == ESP_OK; // ssid last: it marks the slot as used
  ok = ok && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  if (!ok) return AddResult::NvsError;
  slotOut = static_cast<size_t>(target);
  return update ? AddResult::Updated : AddResult::Added;
}

bool remove(const size_t slot) {
  if (slot >= Config::WIFI_STORED_MAX) return false;
  nvs_handle_t h;
  if (nvs_open(Config::WIFI_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
  char key[3];
  keyFor(key, 's', slot);
  const esp_err_t err = nvs_erase_key(h, key); // ssid first: the slot reads as empty from here on
  keyFor(key, 'p', slot);
  nvs_erase_key(h, key);
  const bool ok = err == ESP_OK && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}

} // namespace WifiStore
