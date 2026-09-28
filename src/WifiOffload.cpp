#include "WifiOffload.h"
#include "Config.h"

WifiOffload::WifiOffload(FlashLog &log, SessionState &session) : log_(log), session_(session) {}

void WifiOffload::tryNextCredential() {
  const Config::WifiCredential &cred = Config::WIFI_CREDENTIALS[credentialIndex_];
  WiFi.begin(cred.ssid, cred.password);
  attemptStartMs_ = millis();
}

void WifiOffload::start() {
  phase_ = Phase::Connecting;
  credentialIndex_ = 0;
  serverStarted_ = false;
  WiFiClass::mode(WIFI_STA);
  if (Config::WIFI_CREDENTIAL_COUNT == 0) {
    phase_ = Phase::Failed;
    return;
  }
  tryNextCredential();
}

void WifiOffload::loopTask() {
  switch (phase_) {
    case Phase::Idle:
      return;

    case Phase::Connecting:
      if (WiFiClass::status() == WL_CONNECTED) {
        phase_ = Phase::Connected;
        if (!serverStarted_) {
          server_.on("/logs.csv", HTTP_GET, [this] { handleLogsCsv(); });
          server_.on("/info", HTTP_GET, [this] { handleInfo(); });
          server_.on("/erase-logs", HTTP_POST, [this] { handleEraseLogs(); });
          server_.on("/reset-session", HTTP_POST, [this] { handleResetSession(); });
          server_.begin();
          serverStarted_ = true;
        }
      } else if (millis() - attemptStartMs_ >= Config::WIFI_CONNECT_TIMEOUT_MS) {
        credentialIndex_++;
        if (credentialIndex_ >= Config::WIFI_CREDENTIAL_COUNT) {
          phase_ = Phase::Failed;
        } else {
          tryNextCredential();
        }
      }
      return;

    case Phase::Connected:
      server_.handleClient();
      return;

    case Phase::Failed:
      ;
  }
}

void WifiOffload::stop() {
  if (serverStarted_) {
    server_.close();
    serverStarted_ = false;
  }
  WiFi.disconnect(true);
  WiFiClass::mode(WIFI_OFF);
  phase_ = Phase::Idle;
}

String WifiOffload::statusText() const {
  switch (phase_) {
    case Phase::Idle:
      return "";
    case Phase::Connecting:
      return String("Підключення: ") + Config::WIFI_CREDENTIALS[credentialIndex_].ssid;
    case Phase::Connected:
      return String("IP: ") + WiFi.localIP().toString() + "\nНатисніть, щоб вийти";
    case Phase::Failed:
      return "Wi-Fi недоступний";
  }
  return "";
}

void WifiOffload::handleLogsCsv() {
  server_.sendHeader("Access-Control-Allow-Origin", "*");
  server_.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server_.send(200, "text/csv", "");
  server_.sendContent(FlashLog::csvHeader());

  if (log_.beginExport()) {
    String line;
    while (log_.nextCsvLine(line)) {
      line += "\n";
      server_.sendContent(line);
    }
    log_.endExport();
  }
  server_.sendContent(""); // terminate chunked response
}

void WifiOffload::handleInfo() {
  char body[160];
  snprintf(body, sizeof(body), "{\"role\":\"%s\",\"sessionId\":%u,\"records\":%u,\"fillPercent\":%u}",
           Config::DEVICE_ROLE == Config::Role::Base ? "base" : "rover", static_cast<unsigned>(session_.sessionId()),
           static_cast<unsigned>(log_.recordCount()), static_cast<unsigned>(log_.fillPercent()));
  server_.sendHeader("Access-Control-Allow-Origin", "*");
  server_.send(200, "application/json", body);
}

void WifiOffload::handleEraseLogs() {
  const bool ok = log_.eraseAll();
  server_.sendHeader("Access-Control-Allow-Origin", "*");
  server_.send(ok ? 200 : 500, "text/plain", ok ? "ok" : "erase failed");
}

void WifiOffload::handleResetSession() {
  // The id is only ever reset together with the logs, otherwise old
  // and new sessions with the same number would get mixed up in the viewer.
  const bool ok = log_.eraseAll();
  if (ok) session_.resetSessionId();
  server_.sendHeader("Access-Control-Allow-Origin", "*");
  server_.send(ok ? 200 : 500, "text/plain", ok ? "ok" : "erase failed");
}
