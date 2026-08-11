#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#define SSD1306_WIDTH  128
#define SSD1306_HEIGHT 64
#define SSD1306_PAGES  (SSD1306_HEIGHT / 8)

typedef struct {
    i2c_master_dev_handle_t dev;
    uint8_t framebuffer[SSD1306_WIDTH * SSD1306_PAGES];
} ssd1306_t;

esp_err_t ssd1306_init(ssd1306_t *disp, i2c_master_bus_handle_t bus_handle, uint8_t i2c_addr);
void ssd1306_clear(ssd1306_t *disp);

// page: 0..7 (кожна сторінка = 8 пікселів висоти); col_px: 0..127
void ssd1306_draw_string(ssd1306_t *disp, uint8_t page, uint8_t col_px, const char *str);

esp_err_t ssd1306_flush(ssd1306_t *disp);