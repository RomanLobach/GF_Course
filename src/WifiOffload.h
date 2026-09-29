// Wi-Fi STA connection + small HTTP API for the PC viewer:
//   GET  /info           role, firmware version, session id, record count, log fill %
//   GET  /logs.csv       log export
//   GET  /version        build + runtime identity (JSON, same facts as the `version` command)
//   GET  /syslog         system log, oldest first (text, same as `log all`)
//   POST /cmd            body = one console command line, reply = its text output
//   POST /erase-logs     erase logs (session id untouched)
//   POST /reset-session  erase logs AND reset the session id to 0000
// All responses carry Access-Control-Allow-Origin: * and the POSTs are CORS "simple"
// requests (no custom headers, no body), so the browser never sends a preflight.
// Only reachable from IDLE (the menu gates Wi-Fi on it); runs on the protocol core,
// MenuController pauses/resumes RadioManager around its lifetime.
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "FlashLog.h"
#include "SessionState.h"
#include "Console.h"

class WifiOffload {
public:
  enum class Phase : uint8_t { Idle, Connecting, Connected, Failed };

  WifiOffload(FlashLog &log, SessionState &session, Console &console);

  void start(); // begin trying Config::WIFI_CREDENTIALS in order
  void loopTask(); // call every main-loop iteration while phase() != Idle
  void stop();

  Phase phase() const { return phase_; }
  String statusText() const;

private:
  FlashLog &log_;
  SessionState &session_;
  Console &console_;
  WebServer server_{80};
  Phase phase_ = Phase::Idle;
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
