// Wi-Fi setup portal: the board raises its own WPA2 access point
// "ttgo-lora-bench-<deviceId>" with a fresh random password (shown on the OLED together with
// the address) and serves one page on http://192.168.4.1:
//   GET  /         networks nearby (async scan), a password form, saved networks (SSID only)
//   GET  /scan     restart the scan, back to /
//   POST /save     ssid + pass -> WifiStore::add
//   POST /del      n (1-based slot) -> WifiStore::remove
// Any other URL redirects to / and DNS answers every name with the AP address, so phones
// open the page by themselves (captive portal). No JS, the page is rendered on the board.
//
// This is how release units get their networks (release images carry no secrets.h); the
// Serial `wifi add/del` commands stay as a developer tool. Passwords never go to SysLog or
// back to the page, SSIDs never go to SysLog.
//
// Only from IDLE with the radio paused (MenuController). Everything runs on the protocol
// core from loopTask(), non-blocking; closes by itself after `portal_timeout_min` (config)
// without a request.
#pragma once

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>
#include "Config.h"
#include "ConfigStore.h"

class WifiPortal {
public:
  explicit WifiPortal(const ConfigStore &config) : config_(config) {}

  void start();
  void stop();
  void loopTask(); // every main-loop iteration; no-op while inactive

  bool active() const { return active_; }
  // Closed by the idle timeout since the last call? (consumed by the menu once)
  bool popTimedOut();
  // OLED text, up to 4 lines separated by '\n'.
  void screenText(char *buf, size_t len) const;

private:
  const ConfigStore &config_;
  WebServer server_{80};
  DNSServer dns_;
  bool active_ = false;
  bool routesAdded_ = false;
  bool timedOut_ = false;
  uint32_t lastRequestMs_ = 0;
  uint8_t savedThisSession_ = 0;
  char apName_[40] = {};
  char password_[Config::PORTAL_PASSWORD_LEN + 1] = {};
  char message_[128] = {}; // result line of the last save/delete, shown on the page once

  void handleRoot();
  void handleScan();
  void handleSave();
  void handleDelete();
  void handleNotFound();
  void redirectHome();
};
