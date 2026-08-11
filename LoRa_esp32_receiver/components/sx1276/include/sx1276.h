#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "bitbang_spi.h"

typedef struct {
    bitbang_spi_t spi;
    int rst_gpio;
} sx1276_t;

esp_err_t sx1276_init(sx1276_t *radio,
                       int sck_gpio, int miso_gpio, int mosi_gpio,
                       int cs_gpio, int rst_gpio,
                       uint32_t frequency_hz);

// Блокуюча відправка пакету (до 255 байт) з таймаутом.
esp_err_t sx1276_send(sx1276_t *radio, const uint8_t *data, uint8_t len, uint32_t timeout_ms);

// Переводить модуль у безперервний режим прийому.
esp_err_t sx1276_start_receive(sx1276_t *radio);

// Неблокуюча перевірка: якщо пакет прийнято — заповнює out_data/out_len, повертає true.
bool sx1276_receive(sx1276_t *radio, uint8_t *out_data, uint8_t max_len, uint8_t *out_len);