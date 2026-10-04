// Wi-Fi networks saved in NVS (namespace Config::WIFI_NVS_NAMESPACE), up to
// Config::WIFI_STORED_MAX, in fixed slots 1..N (keys s0/p0 ...). Release images carry no
// secrets.h, so this is where the field units' networks live; WifiOffload tries these first,
// then the built-in secrets.h list.
//
// Kept apart from ConfigStore on purpose: passwords never show up in `config` output.
// Adding/removing is Serial-only (see SystemCommands); nothing here logs an SSID.
#pragma once

#include <Arduino.h>
#include "Config.h"

namespace WifiStore {

constexpr size_t SSID_MAX = 32;
constexpr size_t PASSWORD_MAX = 63;

struct Network {
  char ssid[SSID_MAX + 1];
  char password[PASSWORD_MAX + 1];
};

enum class AddResult : uint8_t { Added, Updated, Full, Invalid, NvsError };

// Slot i (0-based) into `out`; false if the slot is empty.
bool read(size_t slot, Network &out);
// Same SSID already stored -> its password is replaced. slotOut = 0-based slot used.
AddResult add(const char *ssid, const char *password, size_t &slotOut);
bool remove(size_t slot);
size_t count();

} // namespace WifiStore
