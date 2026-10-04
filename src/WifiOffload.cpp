#include "WifiOffload.h"
#include "Config.h"
#include "SysLog.h"
#include "Version.h"
#include "Post.h"

namespace {
// Print adapter that streams into a chunked HTTP response in small blocks.
class HttpPrint : public Print {
public:
  explicit HttpPrint(WebServer &server) : server_(server) {}
  ~HttpPrint() override { flush(); }

  size_t write(const uint8_t c) override {
    buf_[len_++] = static_cast<char>(c);
    if (len_ == sizeof(buf_) - 1) flush();
    return 1;
  }
  void flush() override {
    if (len_ == 0) return;
    buf_[len_] = '\0';
    server_.sendContent(buf_, len_);
    len_ = 0;
  }

private:
  WebServer &server_;
  char buf_[256]{};
  size_t len_ = 0;
};
} // namespace

WifiOffload::WifiOffload(FlashLog &log, SessionState &session, Console &console)
  : log_(log), session_(session), console_(console) {}

void WifiOffload::tryNextCredential() {
  const WifiStore::Network &net = candidates_[credentialIndex_];
  WiFi.begin(net.ssid, net.password);
  attemptStartMs_ = millis();
  // No SSID in the log: /syslog is readable by anyone on the network.
  SLOG_I("wifi", "connecting to network #%u (%s)", static_cast<unsigned>(credentialIndex_ + 1),
         credentialIndex_ < savedCount_ ? "saved" : "built-in");
}

void WifiOffload::start() {
  phase_ = Phase::Connecting;
  credentialIndex_ = 0;
  serverStarted_ = false;

  candidateCount_ = 0;
  for (size_t i = 0; i < Config::WIFI_STORED_MAX; i++) {
    if (WifiStore::read(i, candidates_[candidateCount_])) candidateCount_++;
  }
  savedCount_ = candidateCount_;
  for (const auto &cred : Config::WIFI_CREDENTIALS) {
    if (cred.ssid[0] == '\0') continue; // placeholder entry of a build without secrets.h
    bool duplicate = false;
    for (size_t i = 0; i < savedCount_; i++) duplicate |= strcmp(candidates_[i].ssid, cred.ssid) == 0;
    if (duplicate) continue;
    WifiStore::Network &net = candidates_[candidateCount_++];
    strlcpy(net.ssid, cred.ssid, sizeof(net.ssid));
    strlcpy(net.password, cred.password, sizeof(net.password));
  }

  WiFiClass::mode(WIFI_STA);
  if (candidateCount_ == 0) {
    SLOG_W("wifi", "no networks configured (wifi add)");
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
        SLOG_I("wifi", "connected, ip %s", WiFi.localIP().toString().c_str());
        if (!serverStarted_) {
          server_.on("/logs.csv", HTTP_GET, [this] { handleLogsCsv(); });
          server_.on("/info", HTTP_GET, [this] { handleInfo(); });
          server_.on("/erase-logs", HTTP_POST, [this] { handleEraseLogs(); });
          server_.on("/reset-session", HTTP_POST, [this] { handleResetSession(); });
          server_.on("/version", HTTP_GET, [this] { handleVersion(); });
          server_.on("/syslog", HTTP_GET, [this] { handleSyslog(); });
          server_.on("/cmd", HTTP_POST, [this] { handleCmd(); });
          server_.begin();
          serverStarted_ = true;
        }
      } else if (millis() - attemptStartMs_ >= Config::WIFI_CONNECT_TIMEOUT_MS) {
        credentialIndex_++;
        if (credentialIndex_ >= candidateCount_) {
          SLOG_W("wifi", "no network reachable");
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
  memset(candidates_, 0, sizeof(candidates_)); // don't keep passwords in RAM longer than needed
  candidateCount_ = 0;
  phase_ = Phase::Idle;
}

String WifiOffload::statusText() const {
  switch (phase_) {
    case Phase::Idle:
      return "";
    case Phase::Connecting:
      return String("Підключення: ") + candidates_[credentialIndex_].ssid;
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
  char body[288];
  const Post::Result &post = Post::result();
  snprintf(body, sizeof(body),
           "{\"role\":\"%s\",\"version\":\"%s\",\"device\":\"%s\",\"sessionId\":%u,\"records\":%u,\"fillPercent\":%u,"
           "\"post\":%u,\"battMv\":%u}",
           Version::roleName(), Version::FIRMWARE, Version::deviceId(), static_cast<unsigned>(session_.sessionId()),
           static_cast<unsigned>(log_.recordCount()), static_cast<unsigned>(log_.fillPercent()),
           static_cast<unsigned>(post.mask), static_cast<unsigned>(post.batteryMv));
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

void WifiOffload::handleVersion() {
  char body[384];
  Version::toJson(body, sizeof(body));
  server_.sendHeader("Access-Control-Allow-Origin", "*");
  server_.send(200, "application/json", body);
}

void WifiOffload::handleSyslog() {
  char line[] = "log all";
  runCommand(line);
}

void WifiOffload::handleCmd() {
  // CORS-"simple" POST: the viewer sends the command line as a text/plain body.
  char line[Console::LINE_LEN];
  server_.arg("plain").toCharArray(line, sizeof(line));
  SLOG_I("http", "cmd: %s", line);
  runCommand(line);
}

void WifiOffload::runCommand(char *line) {
  server_.sendHeader("Access-Control-Allow-Origin", "*");
  server_.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server_.send(200, "text/plain; charset=utf-8", "");
  {
    HttpPrint out(server_);
    Console::Context ctx{out, false};
    console_.execute(line, ctx);
  }
  server_.sendContent(""); // terminate chunked response
}
