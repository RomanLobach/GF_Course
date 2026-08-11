#include "buzzer.h"
#include "driver/gpio.h"
#include "esp_timer.h"

// Цей баззер активний по LOW: LOW = звук, HIGH = тиша
#define BUZZER_IDLE_LEVEL  1
#define BUZZER_ON_LEVEL    0

static int s_gpio = -1;
static bool s_active = false;
static int64_t s_off_time_us = 0;

void buzzer_init(int gpio_num)
{
    s_gpio = gpio_num;
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << gpio_num,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);
    gpio_set_level(s_gpio, BUZZER_IDLE_LEVEL);
}

void buzzer_beep(uint32_t duration_ms)
{
    if (s_gpio < 0) return;
    gpio_set_level(s_gpio, BUZZER_ON_LEVEL);
    s_active = true;
    s_off_time_us = esp_timer_get_time() + (int64_t)duration_ms * 1000;
}

void buzzer_update(void)
{
    if (s_active && esp_timer_get_time() >= s_off_time_us) {
        gpio_set_level(s_gpio, BUZZER_IDLE_LEVEL);
        s_active = false;
    }
}