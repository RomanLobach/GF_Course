#include "WifiPortal.h"
#include <WiFi.h>
#include "HttpPrint.h"
#include "SysLog.h"
#include "Version.h"
#include "WifiStore.h"

namespace {
constexpr uint8_t DNS_PORT = 53;

// SSIDs come from the air and from NVS: escape everything that matters in HTML text/attributes.
void printEscaped(Print &out, const char *s) {
  for (; *s; s++) {
    switch (*s) {
      case '&': out.print(F("&amp;")); break;
      case '<': out.print(F("&lt;")); break;
      case '>': out.print(F("&gt;")); break;
      case '"': out.print(F("&quot;")); break;
      case '\'': out.print(F("&#39;")); break;
      default: out.write(static_cast<uint8_t>(*s));
    }
  }
}

const char PAGE_HEAD[] PROGMEM =
  "<!doctype html><html lang=uk><head><meta charset=utf-8>"
  "<meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>ttgo-lora-bench Wi-Fi</title><style>"
  "body{font:16px system-ui,sans-serif;margin:0 auto;padding:16px;max-width:480px}"
  "h1{font-size:20px}h2{font-size:17px;margin-top:24px}"
  "label{display:block;padding:6px 0}input[type=text],input[type=password]{width:100%;"
  "box-sizing:border-box;padding:8px;font-size:16px}"
  "button{padding:8px 16px;font-size:16px;margin-top:8px}.m{padding:8px;background:#eef}"
  ".d{color:#666;font-size:14px}form.i{display:inline}"
  "</style>";
} // namespace

void WifiPortal::start() {
  if (active_) return;
  WiFiClass::mode(WIFI_AP_STA); // STA side only for scanning

  // Fresh password per opening; no 0/O/1/l/i so it reads unambiguously off the OLED.
  static const char ALPHABET[] = "abcdefghjkmnpqrstuvwxyz23456789";
  for (size_t i = 0; i < Config::PORTAL_PASSWORD_LEN; i++) {
    password_[i] = ALPHABET[esp_random() % (sizeof(ALPHABET) - 1)];
  }
  password_[Config::PORTAL_PASSWORD_LEN] = '\0';
  snprintf(apName_, sizeof(apName_), "%s%s", Config::PORTAL_AP_PREFIX, Version::deviceId());

  if (!WiFi.softAP(apName_, password_)) {
    SLOG_E("portal", "soft AP start failed");
    WiFiClass::mode(WIFI_OFF);
    return;
  }
  dns_.setErrorReplyCode(DNSReplyCode::NoError);
  dns_.start(DNS_PORT, "*", WiFi.softAPIP());

  if (!routesAdded_) {
    server_.on("/", HTTP_GET, [this] { handleRoot(); });
    server_.on("/scan", HTTP_GET, [this] { handleScan(); });
    server_.on("/save", HTTP_POST, [this] { handleSave(); });
    server_.on("/del", HTTP_POST, [this] { handleDelete(); });
    server_.onNotFound([this] { handleNotFound(); });
    routesAdded_ = true;
  }
  server_.begin();
  WiFi.scanNetworks(true); // async; the page shows the result when it's in

  active_ = true;
  timedOut_ = false;
  savedThisSession_ = 0;
  message_[0] = '\0';
  lastRequestMs_ = millis();
  SLOG_I("portal", "open, ap %s", apName_);
}

void WifiPortal::stop() {
  if (!active_) return;
  server_.close();
  dns_.stop();
  WiFi.scanDelete();
  WiFi.softAPdisconnect(true);
  WiFiClass::mode(WIFI_OFF);
  memset(password_, 0, sizeof(password_));
  active_ = false;
  SLOG_I("portal", "closed, %u network(s) saved", static_cast<unsigned>(savedThisSession_));
}

void WifiPortal::loopTask() {
  if (!active_) return;
  dns_.processNextRequest();
  server_.handleClient();
  if (millis() - lastRequestMs_ >= config_.get().portalTimeoutMin * 60000UL) {
    SLOG_I("portal", "idle timeout");
    stop();
    timedOut_ = true;
  }
}

bool WifiPortal::popTimedOut() {
  const bool t = timedOut_;
  timedOut_ = false;
  return t;
}

void WifiPortal::screenText(char *buf, const size_t len) const {
  const char *last = savedThisSession_ ? "Збережено. Натисніть - вихід" : "Натисніть, щоб вийти";
  snprintf(buf, len, "%s\nПароль: %s\n192.168.4.1\n%s", apName_, password_, last);
}

void WifiPortal::redirectHome() {
  server_.sendHeader("Location", "http://192.168.4.1/");
  server_.send(303, "text/plain", "");
}

void WifiPortal::handleNotFound() {
  lastRequestMs_ = millis();
  redirectHome(); // captive-portal probes of phones/laptops land on the page
}

void WifiPortal::handleScan() {
  lastRequestMs_ = millis();
  if (WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
  }
  redirectHome();
}

void WifiPortal::handleRoot() {
  lastRequestMs_ = millis();
  const int16_t scan = WiFi.scanComplete();

  server_.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server_.send(200, "text/html; charset=utf-8", "");
  {
    HttpPrint out(server_);
    out.print(FPSTR(PAGE_HEAD));
    if (scan == WIFI_SCAN_RUNNING) out.print(F("<meta http-equiv=refresh content=3>"));
    out.print(F("</head><body><h1>Wi-Fi для "));
    printEscaped(out, apName_);
    out.print(F("</h1>"));
    if (message_[0]) {
      out.print(F("<p class=m>"));
      out.print(message_);
      out.print(F("</p>"));
      message_[0] = '\0';
    }

    out.print(F("<h2>Додати мережу</h2><form method=post action=/save>"));
    if (scan == WIFI_SCAN_RUNNING) {
      out.print(F("<p class=d>Пошук мереж…</p>"));
    } else if (scan > 0) {
      // Strongest first, hidden and repeated SSIDs (several APs of one network) skipped.
      int16_t order[Config::PORTAL_SCAN_MAX];
      size_t shown = 0;
      bool used[64] = {};
      const int16_t total = scan < 64 ? scan : 64;
      while (shown < Config::PORTAL_SCAN_MAX) {
        int16_t best = -1;
        for (int16_t i = 0; i < total; i++) {
          if (used[i] || WiFi.SSID(i).isEmpty()) continue;
          if (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best)) best = i;
        }
        if (best < 0) break;
        used[best] = true;
        bool dup = false;
        for (size_t k = 0; k < shown; k++) dup |= WiFi.SSID(order[k]) == WiFi.SSID(best);
        if (!dup) order[shown++] = best;
      }
      for (size_t k = 0; k < shown; k++) {
        const int16_t i = order[k];
        const String ssid = WiFi.SSID(i);
        out.print(F("<label><input type=radio name=pick value=\""));
        printEscaped(out, ssid.c_str());
        out.print(F("\"> "));
        printEscaped(out, ssid.c_str());
        out.printf(" <span class=d>%d dBm%s</span></label>", WiFi.RSSI(i),
                   WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? ", відкрита" : "");
      }
    } else {
      out.print(F("<p class=d>Мереж не знайдено.</p>"));
    }
    out.print(F("<p><a href=/scan>Шукати ще раз</a></p>"
                "<label>Або назва вручну<input type=text name=ssid maxlength=32 autocapitalize=off></label>"
                "<label>Пароль (порожній - відкрита мережа)<input type=password name=pass maxlength=63></label>"
                "<button>Зберегти</button></form>"));

    out.printf("<h2>Збережені мережі (до %u)</h2>", static_cast<unsigned>(Config::WIFI_STORED_MAX));
    bool any = false;
    WifiStore::Network net{};
    for (size_t i = 0; i < Config::WIFI_STORED_MAX; i++) {
      if (!WifiStore::read(i, net)) continue;
      any = true;
      out.printf("<p>%u. ", static_cast<unsigned>(i + 1));
      printEscaped(out, net.ssid);
      out.printf(" <form class=i method=post action=/del><input type=hidden name=n value=%u>"
                 "<button>Видалити</button></form></p>",
                 static_cast<unsigned>(i + 1));
    }
    memset(&net, 0, sizeof(net));
    if (!any) out.print(F("<p class=d>Немає.</p>"));
    out.print(F("<p class=d>Пристрій пробує збережені мережі по черзі. Закінчивши, натисніть кнопку "
                "енкодера.</p></body></html>"));
  }
  server_.sendContent("");
}

void WifiPortal::handleSave() {
  lastRequestMs_ = millis();
  String ssid = server_.arg("ssid");
  ssid.trim();
  if (ssid.isEmpty()) ssid = server_.arg("pick");
  String pass = server_.arg("pass");

  size_t slot = 0;
  switch (WifiStore::add(ssid.c_str(), pass.c_str(), slot)) {
    case WifiStore::AddResult::Added:
    case WifiStore::AddResult::Updated:
      snprintf(message_, sizeof(message_), "Збережено як мережу %u.", static_cast<unsigned>(slot + 1));
      SLOG_I("portal", "network saved to slot %u", static_cast<unsigned>(slot + 1));
      savedThisSession_++;
      break;
    case WifiStore::AddResult::Full:
      snprintf(message_, sizeof(message_), "Усі %u місця зайняті - спершу видаліть мережу.",
               static_cast<unsigned>(Config::WIFI_STORED_MAX));
      break;
    case WifiStore::AddResult::Invalid:
      snprintf(message_, sizeof(message_), "Потрібна назва (до 32) і пароль 8-63 символи або порожній.");
      break;
    case WifiStore::AddResult::NvsError:
      snprintf(message_, sizeof(message_), "Помилка запису в пам'ять.");
      SLOG_E("portal", "nvs write failed");
      break;
  }
  pass.clear(); // the String buffer is freed; don't keep the password around longer than needed
  redirectHome();
}

void WifiPortal::handleDelete() {
  lastRequestMs_ = millis();
  const long n = server_.arg("n").toInt();
  if (n >= 1 && n <= static_cast<long>(Config::WIFI_STORED_MAX) && WifiStore::remove(static_cast<size_t>(n - 1))) {
    snprintf(message_, sizeof(message_), "Мережу %ld видалено.", n);
    SLOG_I("portal", "network %ld removed", n);
  } else {
    snprintf(message_, sizeof(message_), "Не вдалося видалити.");
  }
  redirectHome();
}
