// Template for include/secrets.h (git-ignored). Copy once and fill in:
//   cp include/secrets.example.h include/secrets.h
// Networks are tried in order during Wi-Fi log offload; 2.4 GHz only (ESP32).
#pragma once

#define WIFI_SECRETS { \
  { .ssid = "YOUR_SSID", .password = "YOUR_PASSWORD" }, \
}
