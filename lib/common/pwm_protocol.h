#pragma once
#include <stdint.h>

// Протокол сенсор -> ініціатор: інформація кодується шпаруватістю PWM.
// Будь-яке відхилення від очікуваної частоти/шпаруватості (обрив, КЗ, шум)
// трактується ініціатором як FAULT, а не як підрив.
namespace pwmproto {
constexpr uint32_t FREQ_HZ = 1000;
constexpr uint32_t PERIOD_US = 1000000UL / FREQ_HZ;
constexpr uint32_t PERIOD_TOL_PCT = 10;   // допуск на період, %
// Канал A: NORMAL=25%, FIRE=75%. Канал B (друга лінія) ІНВЕРСНИЙ: NORMAL=75%, FIRE=25%.
// Інверсія виявляє залипання, обрив однієї лінії та замикання ліній між собою.
constexpr uint8_t DUTY_NORMAL_PCT = 25;
constexpr uint8_t DUTY_FIRE_PCT = 75;
constexpr uint8_t DUTY_B_NORMAL_PCT = 100 - DUTY_NORMAL_PCT;
constexpr uint8_t DUTY_B_FIRE_PCT = 100 - DUTY_FIRE_PCT;
constexpr uint8_t DUTY_TOL_PCT = 8;       // допуск на шпаруватість, в.п.
}
