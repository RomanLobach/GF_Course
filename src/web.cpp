#include "web.h"
#include "config.h"
#include "mav_link.h"
#include "storage.h"
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>

namespace {

WebServer server(80);

const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="uk"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP MAVLink</title>
<style>
:root{--bg:#14171c;--card:#1d2229;--fg:#e7ebf0;--mut:#8b96a5;--ok:#3ecf8e;--bad:#ef5b5b;--warn:#f2b84b;--acc:#4da3ff}
@media (prefers-color-scheme:light){:root{--bg:#f4f6f8;--card:#fff;--fg:#1a2028;--mut:#667180}}
*{box-sizing:border-box}
body{margin:0;padding:16px;background:var(--bg);color:var(--fg);font:16px/1.4 system-ui,sans-serif;max-width:760px;margin-inline:auto}
h1{font-size:20px;margin:0 0 12px}
h2{font-size:15px;color:var(--mut);font-weight:600;margin:0 0 8px;text-transform:uppercase;letter-spacing:.04em}
.card{background:var(--card);border-radius:10px;padding:14px;margin-bottom:14px}
.badge{display:inline-block;padding:2px 10px;border-radius:99px;font-weight:600;font-size:14px}
.CONNECTED{background:var(--ok);color:#06210f}.LOST{background:var(--bad);color:#fff}.WAITING{background:var(--warn);color:#2a1d00}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:10px}
.k{color:var(--mut);font-size:13px}.v{font-size:18px;font-variant-numeric:tabular-nums}
table{width:100%;border-collapse:collapse}
td,th{padding:8px 4px;border-bottom:1px solid rgba(128,128,128,.25);text-align:left}
th{color:var(--mut);font-weight:600;font-size:13px}
a{color:var(--acc);text-decoration:none}
button{background:none;border:1px solid var(--bad);color:var(--bad);border-radius:6px;padding:3px 10px;cursor:pointer}
.mut{color:var(--mut);font-size:13px}
</style></head><body>
<h1>ESP32-C6 &middot; MAVLink телеметрія</h1>

<div class="card">
  <h2>Зв'язок з польотником</h2>
  <span id="link" class="badge WAITING">...</span>
  <div class="grid" style="margin-top:12px">
    <div><div class="k">Режим</div><div class="v" id="mode">—</div></div>
    <div><div class="k">Armed</div><div class="v" id="armed">—</div></div>
    <div><div class="k">Батарея</div><div class="v" id="batt">—</div></div>
    <div><div class="k">Висота (відн.)</div><div class="v" id="alt">—</div></div>
    <div><div class="k">GPS</div><div class="v" id="gps">—</div></div>
    <div><div class="k">Координати</div><div class="v" id="pos">—</div></div>
    <div><div class="k">Швидкість</div><div class="v" id="gs">—</div></div>
    <div><div class="k">Крен / тангаж / курс</div><div class="v" id="att">—</div></div>
  </div>
</div>

<div class="card">
  <h2>Збережені файли</h2>
  <table><thead><tr><th>Файл</th><th>Розмір</th><th></th></tr></thead><tbody id="files"></tbody></table>
  <p class="mut" id="space"></p>
</div>

<script>
const $=id=>document.getElementById(id);
const f=(v,d=1,u='')=>v===undefined?'—':(+v).toFixed(d)+u;
const FIX=['No GPS','No Fix','2D Fix','3D Fix','DGPS','RTK Float','RTK Fixed'];

async function telemetry(){
  try{
    const t=await (await fetch('/api/telemetry')).json();
    $('link').textContent=t.link; $('link').className='badge '+t.link;
    $('mode').textContent=t.mode??'—';
    $('armed').textContent=t.armed===undefined?'—':(t.armed?'так':'ні');
    $('batt').textContent=t.batt_v===undefined?'—':f(t.batt_v,2,' В')+(t.batt_pct!==undefined?' ('+t.batt_pct+'%)':'');
    $('alt').textContent=f(t.alt_rel,1,' м');
    $('gps').textContent=t.fix===undefined?'—':(FIX[t.fix]??t.fix)+(t.sats!==undefined?', '+t.sats+' sat':'')+(t.hdop!==undefined?', HDOP '+f(t.hdop,1):'');
    $('pos').textContent=t.lat===undefined?'—':f(t.lat,5)+', '+f(t.lon,5);
    $('gs').textContent=f(t.gs,1,' м/с');
    $('att').textContent=t.roll===undefined?'—':f(t.roll,0)+'° / '+f(t.pitch,0)+'° / '+f(t.yaw,0)+'°';
  }catch(e){ $('link').textContent='ESP недоступний'; $('link').className='badge LOST'; }
}

const size=n=>n<1024?n+' Б':(n/1024).toFixed(1)+' КБ';
async function files(){
  try{
    const r=await (await fetch('/api/files')).json();
    const rows=r.files.slice().reverse().map(x=>
      '<tr><td><a href="/download?f='+x.name+'">'+x.name+'</a>'+(x.current?' <span class="mut">(пишеться)</span>':'')+'</td>'+
      '<td>'+size(x.size)+'</td><td>'+(x.current?'':'<button onclick="del(\''+x.name+'\')">видалити</button>')+'</td></tr>');
    $('files').innerHTML=rows.join('')||'<tr><td colspan="3" class="mut">файлів ще немає</td></tr>';
    $('space').textContent='Вільно '+size(r.free)+' з '+size(r.total);
  }catch(e){}
}
async function del(n){
  if(!confirm('Видалити '+n+'?'))return;
  await fetch('/delete?f='+encodeURIComponent(n),{method:'POST'});
  files();
}
telemetry(); files();
setInterval(telemetry,1000); setInterval(files,5000);
</script>
</body></html>)HTML";

void sendFiles() {
    String j = "{\"files\":[";
    auto list = storage::list();
    String cur = storage::currentName();
    for (size_t i = 0; i < list.size(); i++) {
        if (i) j += ",";
        j += "{\"name\":\"" + list[i].name + "\",\"size\":" + String((unsigned)list[i].size) +
             ",\"current\":" + (list[i].name == cur ? "true" : "false") + "}";
    }
    j += "],\"free\":" + String((unsigned)storage::freeBytes()) +
         ",\"total\":" + String((unsigned)storage::totalBytes()) + "}";
    server.send(200, "application/json", j);
}

void sendDownload() {
    String name = server.arg("f");
    if (!storage::isSafeName(name)) {
        server.send(400, "text/plain", "bad file name");
        return;
    }
    storage::flushNow();  // щоб поточний файл віддавався з усіма рядками
    File f = LittleFS.open("/" + name, "r");
    if (!f) {
        server.send(404, "text/plain", "not found");
        return;
    }
    server.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
    server.streamFile(f, "text/csv");
    f.close();
}

void doDelete() {
    String name = server.arg("f");
    if (storage::remove(name)) server.send(200, "text/plain", "ok");
    else server.send(400, "text/plain", "cannot delete");
}

}  // namespace

namespace web {

void begin() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("[web] AP \"%s\", http://%s/\n", AP_SSID, WiFi.softAPIP().toString().c_str());

    server.on("/", HTTP_GET, [] { server.send_P(200, "text/html; charset=utf-8", PAGE); });
    server.on("/api/telemetry", HTTP_GET, [] {
        server.send(200, "application/json",
                    telemetryToJson(mav_link::snapshot(), mav_link::state(), millis()));
    });
    server.on("/api/files", HTTP_GET, sendFiles);
    server.on("/download", HTTP_GET, sendDownload);
    server.on("/delete", HTTP_POST, doDelete);
    server.onNotFound([] { server.send(404, "text/plain", "not found"); });
    server.begin();
}

void handle() {
    server.handleClient();
}

}  // namespace web
