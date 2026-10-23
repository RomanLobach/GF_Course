#include "web_ui.h"

#include <ESPmDNS.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config.h"
#include "wifi_secrets.h"

namespace {
WebServer server(80);
TriggerFsm* gFsm = nullptr;
EventLog* gLog = nullptr;

void sendJson(int code, const String& body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

void handleRoot() {
  File f = LittleFS.open("/index.html", "r");
  if (!f) {
    server.send(500, "text/plain", "index.html not found");
    return;
  }
  server.streamFile(f, "text/html; charset=utf-8");
  f.close();
}

void handleStatus() {
  const Snapshot s = gFsm->snapshot(millis());
  String j = "{\"state\":\"";
  j += stateName(s.state);
  j += "\",\"armRemainingMs\":" + String(s.armRemainingMs);
  j += ",\"fireConfirmMs\":" + String(s.fireConfirmedMs);
  j += ",\"sensor\":\"";
  j += sensorStatusName(s.sensor);
  j += "\",\"duty\":" + String(s.duty);
  j += ",\"vbusMv\":" + String(s.vbusMv);
  j += ",\"vbusOk\":" + String(s.vbusOk ? "true" : "false");
  j += ",\"fault\":\"";
  j += s.fault;
  j += "\",\"test\":" + String(s.testMode ? "true" : "false");
  j += ",\"fireIgnored\":" + String(s.fireIgnored ? "true" : "false") + "}";
  sendJson(200, j);
}

void appendEscaped(String& j, const char* t) {
  for (; *t; t++) {
    if (*t == '"' || *t == '\\') j += '\\';
    j += *t;
  }
}

void handleLog() {
  String j = "[";
  for (size_t i = 0; i < gLog->size(); i++) {
    const LogEntry& e = gLog->newest(i);
    if (i) j += ',';
    j += "{\"seq\":" + String(e.seq) + ",\"boot\":" + String(e.bootNo) + ",\"t\":" + String(e.uptimeMs);
    j += ",\"type\":\"" + String(eventTypeName(e.type)) + "\",\"from\":\"" + stateName((State)e.from);
    j += "\",\"to\":\"" + String(stateName((State)e.to)) + "\",\"text\":\"";
    appendEscaped(j, e.text);
    j += "\"}";
  }
  j += "]";
  sendJson(200, j);
}

void handleCmd() {
  const String c = server.arg("cmd");
  Cmd cmd;
  if (c == "start") cmd = Cmd::START;
  else if (c == "stop") cmd = Cmd::STOP;
  else if (c == "explosion") cmd = Cmd::EXPLOSION;
  else if (c == "test") cmd = Cmd::TEST;
  else if (c == "wdttest") cmd = Cmd::WDT_TEST;
  else {
    sendJson(400, "{\"ok\":false,\"msg\":\"unknown command\"}");
    return;
  }
  const CmdResult r = gFsm->command(cmd, Source::WEB, millis());
  sendJson(r.accepted ? 200 : 409, String("{\"ok\":") + (r.accepted ? "true" : "false") + ",\"confirm\":" +
                                       (r.needsConfirm ? "true" : "false") + ",\"msg\":\"" + r.message + "\"}");
}
}  // namespace

bool WebUi::begin(TriggerFsm* fsm, EventLog* log) {
  gFsm = fsm;
  gLog = log;
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(cfg::HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);  // не чекаємо підключення: автомат і кнопки не залежать від мережі
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/cmd", HTTP_POST, handleCmd);
  server.on("/api/log", HTTP_GET, handleLog);
  server.onNotFound([] { server.send(404, "text/plain", "not found"); });
  server.begin();
  return true;
}

void WebUi::handle() {
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected != wasConnected_) {
    wasConnected_ = connected;
    if (connected) Serial.printf("[wifi] connected to %s, ip %s\n", WIFI_SSID, WiFi.localIP().toString().c_str());
    else Serial.println("[wifi] disconnected, reconnecting");
    mdnsStarted_ = false;
  }
  if (connected && !mdnsStarted_) {
    mdnsStarted_ = MDNS.begin(cfg::HOSTNAME);
    if (mdnsStarted_) MDNS.addService("http", "tcp", 80);
  }
  server.handleClient();
}

String WebUi::ip() const { return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("no WiFi"); }
