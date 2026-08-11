#include <string.h>
#include "sx1276.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sx1276";

// ---- Регістри SX1276 (Semtech datasheet) ----
#define REG_FIFO                   0x00
#define REG_OP_MODE                0x01
#define REG_FRF_MSB                 0x06
#define REG_FRF_MID                 0x07
#define REG_FRF_LSB                 0x08
#define REG_PA_CONFIG               0x09
#define REG_FIFO_ADDR_PTR           0x0D
#define REG_FIFO_TX_BASE_ADDR       0x0E
#define REG_FIFO_RX_BASE_ADDR       0x0F
#define REG_FIFO_RX_CURRENT_ADDR    0x10
#define REG_IRQ_FLAGS               0x12
#define REG_RX_NB_BYTES             0x13
#define REG_PAYLOAD_LENGTH          0x22
#define REG_MODEM_CONFIG_1          0x1D
#define REG_MODEM_CONFIG_2          0x1E
#define REG_MODEM_CONFIG_3          0x26
#define REG_PREAMBLE_MSB            0x20
#define REG_PREAMBLE_LSB            0x21
#define REG_SYNC_WORD               0x39
#define REG_VERSION                 0x42

#define MODE_LONG_RANGE_MODE  0x80
#define MODE_SLEEP            0x00
#define MODE_STDBY             0x01
#define MODE_TX                 0x03
#define MODE_RX_CONTINUOUS       0x05

#define IRQ_TX_DONE_MASK              0x08
#define IRQ_RX_DONE_MASK              0x40
#define IRQ_PAYLOAD_CRC_ERROR_MASK    0x20

#define PA_BOOST  0x80

// Приватний sync word — щоб не заважати іншим LoRa-мережам поруч.
#define LORA_SYNC_WORD  0xAB

static esp_err_t write_reg(sx1276_t *radio, uint8_t addr, uint8_t value)
{
    bitbang_spi_select(&radio->spi);
    bitbang_spi_transfer_byte(&radio->spi, addr | 0x80); // біт7=1 -> запис
    bitbang_spi_transfer_byte(&radio->spi, value);
    bitbang_spi_deselect(&radio->spi);
    return ESP_OK;
}

static uint8_t read_reg(sx1276_t *radio, uint8_t addr)
{
    bitbang_spi_select(&radio->spi);
    bitbang_spi_transfer_byte(&radio->spi, addr & 0x7F); // біт7=0 -> читання
    uint8_t value = bitbang_spi_transfer_byte(&radio->spi, 0x00); // фіктивний байт, щоб отримати відповідь
    bitbang_spi_deselect(&radio->spi);
    return value;
}

static void reset_chip(int rst_gpio)
{
    gpio_set_level(rst_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(rst_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

esp_err_t sx1276_init(sx1276_t *radio,
                       int sck_gpio, int miso_gpio, int mosi_gpio,
                       int cs_gpio, int rst_gpio,
                       uint32_t frequency_hz)
{
    radio->rst_gpio = rst_gpio;

    gpio_config_t rst_conf = {
        .pin_bit_mask = 1ULL << rst_gpio,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst_conf);

    bitbang_spi_init(&radio->spi, sck_gpio, mosi_gpio, miso_gpio, cs_gpio);

    reset_chip(rst_gpio);

    uint8_t version = read_reg(radio, REG_VERSION);
    if (version != 0x12) {
        ESP_LOGE(TAG, "Unexpected SX1276 version: 0x%02X (очікувалось 0x12) — перевірте SPI/RST", version);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "SX1276 detected (bit-bang SPI), version register: 0x%02X", version);

    write_reg(radio, REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_SLEEP);
    vTaskDelay(pdMS_TO_TICKS(10));

    uint64_t frf = ((uint64_t)frequency_hz << 19) / 32000000ULL;
    write_reg(radio, REG_FRF_MSB, (uint8_t)(frf >> 16));
    write_reg(radio, REG_FRF_MID, (uint8_t)(frf >> 8));
    write_reg(radio, REG_FRF_LSB, (uint8_t)(frf >> 0));

    write_reg(radio, REG_FIFO_TX_BASE_ADDR, 0x00);
    write_reg(radio, REG_FIFO_RX_BASE_ADDR, 0x00);

    write_reg(radio, REG_MODEM_CONFIG_1, 0x72); // BW=125kHz, CR=4/5, явний заголовок
    write_reg(radio, REG_MODEM_CONFIG_2, 0x74); // SF7, CRC увімкнено
    write_reg(radio, REG_MODEM_CONFIG_3, 0x04); // AGC увімкнено

    write_reg(radio, REG_PREAMBLE_MSB, 0x00);
    write_reg(radio, REG_PREAMBLE_LSB, 0x08);

    write_reg(radio, REG_SYNC_WORD, LORA_SYNC_WORD);

    write_reg(radio, REG_PA_CONFIG, PA_BOOST | 0x0F); // ~17 дБм

    write_reg(radio, REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_STDBY);
    vTaskDelay(pdMS_TO_TICKS(10));

    return ESP_OK;
}

esp_err_t sx1276_send(sx1276_t *radio, const uint8_t *data, uint8_t len, uint32_t timeout_ms)
{
    write_reg(radio, REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_STDBY);

    write_reg(radio, REG_FIFO_ADDR_PTR, 0x00);
    for (uint8_t i = 0; i < len; i++) {
        write_reg(radio, REG_FIFO, data[i]);
    }
    write_reg(radio, REG_PAYLOAD_LENGTH, len);

    write_reg(radio, REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_TX);

    int64_t start = esp_timer_get_time();
    while (1) {
        uint8_t irq = read_reg(radio, REG_IRQ_FLAGS);
        if (irq & IRQ_TX_DONE_MASK) {
            write_reg(radio, REG_IRQ_FLAGS, 0xFF);
            write_reg(radio, REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_STDBY);
            return ESP_OK;
        }
        if ((esp_timer_get_time() - start) > (int64_t)timeout_ms * 1000) {
            ESP_LOGW(TAG, "TX timeout");
            write_reg(radio, REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_STDBY);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

esp_err_t sx1276_start_receive(sx1276_t *radio)
{
    write_reg(radio, REG_FIFO_RX_BASE_ADDR, 0x00);
    write_reg(radio, REG_FIFO_ADDR_PTR, 0x00);
    write_reg(radio, REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_RX_CONTINUOUS);
    return ESP_OK;
}

bool sx1276_receive(sx1276_t *radio, uint8_t *out_data, uint8_t max_len, uint8_t *out_len)
{
    uint8_t irq = read_reg(radio, REG_IRQ_FLAGS);
    if (!(irq & IRQ_RX_DONE_MASK)) {
        return false;
    }

    write_reg(radio, REG_IRQ_FLAGS, 0xFF);

    if (irq & IRQ_PAYLOAD_CRC_ERROR_MASK) {
        return false; // пошкоджений пакет — ігноруємо
    }

    uint8_t len = read_reg(radio, REG_RX_NB_BYTES);
    if (len > max_len) len = max_len;

    uint8_t addr = read_reg(radio, REG_FIFO_RX_CURRENT_ADDR);
    write_reg(radio, REG_FIFO_ADDR_PTR, addr);

    for (uint8_t i = 0; i < len; i++) {
        out_data[i] = read_reg(radio, REG_FIFO);
    }

    *out_len = len;
    return true;
}