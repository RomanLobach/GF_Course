#pragma once
#include <Arduino.h>

// ---- UART до польотного контролера (SERIAL3 на FC) ----
#ifndef MAV_RX_PIN
#define MAV_RX_PIN 7   // сюди підключається TX3 польотника
#endif
#ifndef MAV_TX_PIN
#define MAV_TX_PIN 6   // звідси йде на RX3 польотника
#endif
#ifndef MAV_BAUD
#define MAV_BAUD 115200
#endif

// ---- MAVLink ----
constexpr uint8_t  OWN_SYSID            = 1;      // companion використовує sysid апарата
constexpr uint32_t HEARTBEAT_TIMEOUT_MS = 3000;   // стільки без heartbeat = зв'язок втрачено
constexpr uint32_t OWN_HEARTBEAT_MS     = 1000;
constexpr uint32_t REQUEST_FAST_MS      = 1000;   // перші запити потоків повторюємо швидко
constexpr uint32_t REQUEST_SLOW_MS      = 10000;  // далі раз на 10 с (на випадок перезапуску FC)
constexpr uint8_t  REQUEST_FAST_COUNT   = 3;

// ---- Логування ----
constexpr uint32_t LOG_PERIOD_MS        = 1000;   // один рядок CSV на секунду
constexpr uint8_t  FLUSH_EVERY_ROWS     = 5;      // скидати на флеш раз на 5 рядків
constexpr size_t   MIN_FREE_BYTES       = 100 * 1024; // менше за це видаляємо найстаріші файли
constexpr uint32_t DATA_STALE_MS        = 5000;   // дані старші за це вважаємо недійсними

// ---- Wi-Fi AP ----
constexpr const char* AP_SSID = "ESP-MAVLink";
constexpr const char* AP_PASS = "mavlink123";    // мінімум 8 символів
