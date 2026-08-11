#pragma once
#include <stdint.h>

typedef struct {
    int sck_gpio;
    int mosi_gpio;
    int miso_gpio;
    int cs_gpio;
} bitbang_spi_t;

void bitbang_spi_init(bitbang_spi_t *spi, int sck_gpio, int mosi_gpio, int miso_gpio, int cs_gpio);

void bitbang_spi_select(bitbang_spi_t *spi);   // LOW
void bitbang_spi_deselect(bitbang_spi_t *spi); // HIGH

uint8_t bitbang_spi_transfer_byte(bitbang_spi_t *spi, uint8_t out_byte);