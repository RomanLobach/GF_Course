#include "bitbang_spi.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"

#define BIT_DELAY_US  2

static inline void digital_write(int gpio, int level)
{
    gpio_set_level(gpio, level);
}

static inline int digital_read(int gpio)
{
    return gpio_get_level(gpio);
}

void bitbang_spi_init(bitbang_spi_t *spi, int sck_gpio, int mosi_gpio, int miso_gpio, int cs_gpio)
{
    spi->sck_gpio = sck_gpio;
    spi->mosi_gpio = mosi_gpio;
    spi->miso_gpio = miso_gpio;
    spi->cs_gpio = cs_gpio;

    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << sck_gpio) | (1ULL << mosi_gpio) | (1ULL << cs_gpio),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out_conf);

    gpio_config_t in_conf = {
        .pin_bit_mask = 1ULL << miso_gpio,
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&in_conf);

    digital_write(spi->sck_gpio, 0);
    digital_write(spi->cs_gpio, 1);
}

void bitbang_spi_select(bitbang_spi_t *spi)
{
    digital_write(spi->cs_gpio, 0);
}

void bitbang_spi_deselect(bitbang_spi_t *spi)
{
    digital_write(spi->cs_gpio, 1);
}

uint8_t bitbang_spi_transfer_byte(bitbang_spi_t *spi, uint8_t out_byte)
{
    uint8_t in_byte = 0;

    for (int bit = 7; bit >= 0; bit--) {
        // 1. Виставляємо біт на MOSI, ПОКИ SCK ще низький (setup time)
        digital_write(spi->mosi_gpio, (out_byte >> bit) & 0x01);
        esp_rom_delay_us(BIT_DELAY_US);

        // 2. Формуємо фронт такту: SCK LOW -> HIGH.
        //    Саме на цьому фронті приймач зчитує MOSI (Mode 0).
        digital_write(spi->sck_gpio, 1);

        // 3. Ми теж зчитуємо MISO на цьому самому фронті — так і має
        //    поводитись хост (master) у SPI Mode 0.
        int bit_in = digital_read(spi->miso_gpio);
        in_byte = (in_byte << 1) | (bit_in & 0x01);

        esp_rom_delay_us(BIT_DELAY_US);

        // 4. Завершуємо такт: SCK повертається в LOW
        digital_write(spi->sck_gpio, 0);
        esp_rom_delay_us(BIT_DELAY_US);
    }

    return in_byte;
}