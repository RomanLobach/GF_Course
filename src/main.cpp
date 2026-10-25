#include <Arduino.h>
#include "config.h"
#include "mav_link.h"
#include "storage.h"
#include "web.h"

namespace {
uint32_t lastRowMs = 0;
uint32_t lastStatusMs = 0;
uint32_t lastOpenTryMs = 0;
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n== ESP32-C6 MAVLink companion ==");

    if (!storage::begin()) Serial.println("[fs] LittleFS: помилка монтування");
    else Serial.printf("[fs] вільно %u з %u байт\n", (unsigned)storage::freeBytes(), (unsigned)storage::totalBytes());

    web::begin();
    mav_link::begin();
    Serial.printf("[mav] UART1: RX=GPIO%d TX=GPIO%d, %d бод\n", MAV_RX_PIN, MAV_TX_PIN, MAV_BAUD);
}

void loop() {
    web::handle();

    const uint32_t now = millis();
    const LinkState link = mav_link::state();

    if (link == LinkState::Connected) {
        // нове підключення => новий CSV-файл
        if (!storage::isOpen() && now - lastOpenTryMs >= 2000) {
            lastOpenTryMs = now;
            if (storage::openNewLog()) Serial.printf("[fs] новий файл %s\n", storage::currentName().c_str());
            else Serial.println("[fs] не вдалося створити файл");
        }
        if (storage::isOpen() && now - lastRowMs >= LOG_PERIOD_MS) {
            // пишемо лише коли потоки вже прийшли, щоб у CSV не було порожніх рядків
            const Telemetry t = mav_link::snapshot();
            const bool dataReady = t.pos_ms != 0 && t.sys_ms != 0 &&
                                   now - t.pos_ms < DATA_STALE_MS && now - t.sys_ms < DATA_STALE_MS;
            if (dataReady) {
                lastRowMs = now;
                if (!storage::writeRow(t)) Serial.println("[fs] помилка запису");
            }
        }
    } else if (storage::isOpen()) {
        storage::closeLog();
        Serial.println("[fs] файл закрито (зв'язок втрачено)");
    }

    if (now - lastStatusMs >= 2000) {
        lastStatusMs = now;
        const Telemetry t = mav_link::snapshot();
        Serial.printf("[%lu] link=%s mode=%s armed=%d batt=%.2fV alt=%.1fm fix=%u sats=%u file=%s\n",
                      (unsigned long)now, linkStateName(link), flightModeName(t), t.armed() ? 1 : 0,
                      t.batt_mv == 0xFFFF ? 0.0f : t.batt_mv / 1000.0f, t.alt_rel_mm / 1000.0f,
                      t.fix_type, t.sats == 255 ? 0 : t.sats, storage::currentName().c_str());
    }

    delay(2);
}
