// Wi-Fi STA connection + small HTTP API for the PC viewer:
//   GET  /info           role, firmware version, session id, record count, log fill %, POST
//                        mask + battery mV
//   GET  /logs.csv       log export
//   GET  /version        build + runtime identity (JSON, same facts as the `version` command)
//   GET  /syslog         system log, oldest first (text, same as `log all`)
//   POST /cmd            body = one console command line, reply = its text output
//   POST /erase-logs     erase logs (session id untouched)
//   POST /reset-session  erase logs AND reset the session id to 0000
// All responses carry Access-Control-Allow-Origin: * and the POSTs are CORS "simple"
// requests (no custom headers, no body), so the browser never sends a preflight.
// Networks: the ones saved in NVS (WifiStore, `wifi add`) first, then the built-in secrets.h
// list. The system log names them only by number, never by SSID.
// Only reachable from IDLE (the menu gates Wi-Fi on it); runs on the protocol core,
// MenuController pauses/resumes RadioManager around its lifetime.
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "FlashLog.h"
#include "SessionState.h"
#include "Console.h"
#include "WifiStore.h"
#include "ConfigStore.h"

class WifiOffload {
public:
  enum class Phase : uint8_t { Idle, Connecting, Connected, Failed };

  WifiOffload(FlashLog &log, SessionState &session, Console &console, const ConfigStore &config);

  void start(); // begin trying the saved networks, then Config::WIFI_CREDENTIALS, in order
  void loopTask(); // call every main-loop iteration while phase() != Idle
  void stop();
  // Any network to try at all (saved or built-in)?
  static bool hasNetworks();

  Phase phase() const { return phase_; }
  String statusText() const;

private:
  FlashLog &log_;
  SessionState &session_;
  Console &console_;
  const ConfigStore &config_;
  WebServer server_{80};
  Phase phase_ = Phase::Idle;
  static constexpr size_t MAX_CANDIDATES = Config::WIFI_STORED_MAX + Config::WIFI_CREDENTIAL_COUNT;
  WifiStore::Network candidates_[MAX_CANDIDATES]{};
  size_t candidateCount_ = 0;
  size_t savedCount_ = 0; // candidates_[0..savedCount_) came from NVS
  size_t credentialIndex_ = 0;
  uint32_t attemptStartMs_ = 0;
  bool serverStarted_ = false;

  void tryNextCredential();
  void handleLogsCsv();
  void handleInfo();
  void handleEraseLogs();
  void handleResetSession();
  void handleVersion();
  void handleSyslog();
  void handleCmd();
  void runCommand(char *line); // streams the command output as the response body
};
