#include <string.h>
#include "ssd1306.h"
#include "font_5x7.h"
#include "esp_log.h"

static const char *TAG = "ssd1306";

static esp_err_t write_cmd(ssd1306_t *disp, uint8_t cmd)
{
    uint8_t buf[2] = { 0x00, cmd }; // 0x00 = control byte для потоку команд
    return i2c_master_transmit(disp->dev, buf, sizeof(buf), 100);
}

esp_err_t ssd1306_init(ssd1306_t *disp, i2c_master_bus_handle_t bus_handle, uint8_t i2c_addr)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = i2c_addr,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus_handle, &dev_cfg, &disp->dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    // Стандартна ініціалізаційна послідовність SSD1306 128x64
    static const uint8_t init_cmds[] = {
        0xAE,       // display off
        0xD5, 0x80, // clock divide / oscillator freq
        0xA8, 0x3F, // multiplex ratio = 63 (128x64)
        0xD3, 0x00, // display offset = 0
        0x40,       // start line = 0
        0x8D, 0x14, // charge pump enable
        0x20, 0x02, // memory addressing mode = page addressing
        0xA1,       // segment remap
        0xC8,       // COM output scan direction remapped
        0xDA, 0x12, // COM pins hardware config
        0x81, 0xCF, // contrast
        0xD9, 0xF1, // pre-charge period
        0xDB, 0x40, // VCOMH deselect level
        0xA4,       // resume to RAM content display
        0xA6,       // normal (не інвертований) display
        0xAF,       // display on
    };

    for (size_t i = 0; i < sizeof(init_cmds); i++) {
        err = write_cmd(disp, init_cmds[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "init cmd 0x%02X failed: %s", init_cmds[i], esp_err_to_name(err));
            return err;
        }
    }

    ssd1306_clear(disp);
    return ssd1306_flush(disp);
}

void ssd1306_clear(ssd1306_t *disp)
{
    memset(disp->framebuffer, 0x00, sizeof(disp->framebuffer));
}

static void draw_char(ssd1306_t *disp, uint8_t page, uint8_t col_px, char c)
{
    if (page >= SSD1306_PAGES) return;
    const uint8_t *glyph = &font5x7[(uint8_t)c * 5];

    for (uint8_t i = 0; i < 5; i++) {
        uint8_t col = col_px + i;
        if (col >= SSD1306_WIDTH) return;
        disp->framebuffer[page * SSD1306_WIDTH + col] = glyph[i];
    }
}

void ssd1306_draw_string(ssd1306_t *disp, uint8_t page, uint8_t col_px, const char *str)
{
    uint8_t col = col_px;
    while (*str && col < SSD1306_WIDTH) {
        draw_char(disp, page, col, *str);
        col += 6; // 5px гліф + 1px проміжок
        str++;
    }
}

esp_err_t ssd1306_flush(ssd1306_t *disp)
{
    uint8_t chunk[1 + SSD1306_WIDTH];
    chunk[0] = 0x40; // control byte для потоку даних

    for (uint8_t page = 0; page < SSD1306_PAGES; page++) {
        esp_err_t err = write_cmd(disp, 0xB0 | page); // адреса сторінки
        if (err != ESP_OK) return err;
        err = write_cmd(disp, 0x00); // молодший ніббл колонки = 0
        if (err != ESP_OK) return err;
        err = write_cmd(disp, 0x10); // старший ніббл колонки = 0
        if (err != ESP_OK) return err;

        memcpy(&chunk[1], &disp->framebuffer[page * SSD1306_WIDTH], SSD1306_WIDTH);
        err = i2c_master_transmit(disp->dev, chunk, sizeof(chunk), 100);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}