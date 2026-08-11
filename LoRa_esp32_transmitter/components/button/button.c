#include "button.h"
#include "driver/gpio.h"
#include "esp_timer.h"

void button_init(button_t *btn, int gpio_num, uint32_t debounce_ms)
{
    btn->gpio_num = gpio_num;
    btn->debounce_ms = debounce_ms;

    // Зовнішній pull-up до 3.3V вже є на схемі — внутрішній не потрібен
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << gpio_num,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };
    gpio_config(&io_conf);

    bool level = gpio_get_level(gpio_num);
    btn->raw_prev = level;
    btn->stable_state = level;
    btn->last_change_us = esp_timer_get_time();
    btn->press_event = false;
}

void button_update(button_t *btn)
{
    bool raw = gpio_get_level(btn->gpio_num);
    int64_t now = esp_timer_get_time();

    if (raw != btn->raw_prev) {
        btn->last_change_us = now;
        btn->raw_prev = raw;
    }

    if ((now - btn->last_change_us) > (int64_t)btn->debounce_ms * 1000) {
        if (raw != btn->stable_state) {
            bool was_released = btn->stable_state; // true = HIGH (не натиснута)
            btn->stable_state = raw;

            // Кнопка підтягнута до 3.3V -> натискання замикає на GND (LOW)
            if (was_released && !btn->stable_state) {
                btn->press_event = true;
            }
        }
    }
}

bool button_consume_press_event(button_t *btn)
{
    if (btn->press_event) {
        btn->press_event = false;
        return true;
    }
    return false;
}