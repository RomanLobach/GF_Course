#pragma once
#include <stdint.h>
#include <pwm_protocol.h>

#ifndef ARM_DELAY_MS
#define ARM_DELAY_MS 5000UL  // для бойового режиму: -DARM_DELAY_MS=300000UL (env trigger_prod)
#endif

// Коефіцієнт зовнішнього дільника на лінії вимірювання напруги (Uвх = Uadc * ratio).
#ifndef VBUS_DIVIDER_RATIO
#define VBUS_DIVIDER_RATIO 2.0f
#endif

static_assert(ARM_DELAY_MS >= 1000UL && ARM_DELAY_MS <= 600000UL, "ARM_DELAY_MS out of range");

namespace cfg {
constexpr int PIN_BTN_GREEN = 34;   // зняти ARM (Stop), активний LOW
constexpr int PIN_BTN_YELLOW = 14;  // поставити ARM (Start), активний LOW
constexpr int PIN_LED_RED = 0;      // спрацювання (EXPLOSION), активний LOW: 3V3 -> R -> LED -> GPIO0
constexpr int PIN_LED_YELLOW = 2;   // ARMING/READY/FAULT, активний LOW: 3V3 -> R -> LED -> GPIO2
constexpr int PIN_PWM_IN = 13;      // канал A
constexpr int PIN_PWM_IN_B = 15;    // канал B (інверсний); GPIO15 — strapping, але лише глушить лог ROM
constexpr int PIN_VBUS_ADC = 35;
constexpr int PIN_OLED_SDA = 21;
constexpr int PIN_OLED_SCL = 22;
constexpr uint8_t OLED_ADDR = 0x3C;

constexpr uint32_t ARM_DELAY = ARM_DELAY_MS;
constexpr uint32_t BTN_DEBOUNCE_MS = 30;

constexpr uint32_t SENSOR_TIMEOUT_MS = 250;   // немає фронтів -> FAULT
constexpr uint32_t SENSOR_DEBOUNCE_MS = 50;   // стан має триматись, щоб змінити статус
// Спрацювання миттєве: захист від хибних тривог робить фільтр датчика (валідний PWM-кадр +
// SENSOR_DEBOUNCE_MS безперервно), тож додаткова витримка в автоматі 0. Можна збільшити за потреби.
constexpr uint32_t CHANNEL_MISMATCH_MS = 100;  // канали розходяться довше -> FAULT
constexpr uint32_t FIRE_CONFIRM_MS = 0;
constexpr uint32_t READY_SETTLE_MS = 500;     // після входу в READY спрацювання заборонене
constexpr uint32_t EXPLOSION_CONFIRM_WINDOW_MS = 5000;  // вікно друго кроку ручного підриву

constexpr uint32_t VBUS_MIN_MV = 4000;
constexpr uint32_t VBUS_MAX_MV = 5600;
constexpr uint32_t VBUS_SAMPLE_MS = 200;
constexpr uint8_t VBUS_FAULT_SAMPLES = 3;     // поспіль, щоб оголосити помилку

constexpr uint32_t WDT_TIMEOUT_S = 5;        // task WDT
constexpr uint32_t RTC_WDT_TIMEOUT_MS = 8000; // другий, незалежний (RTC) watchdog — страховка від збою першого
constexpr const char* HOSTNAME = "trigger";  // http://rigger.local (mDNS)

}  // namespace cfg
