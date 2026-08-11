#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int gpio_num;
    uint32_t debounce_ms;
    bool raw_prev;          // останній "сирий" зчитаний рівень
    bool stable_state;      // підтверджений (антидребезговий) стан
    int64_t last_change_us; // час останньої зміни сирого рівня
    bool press_event;       // одноразовий прапорець нового натискання
} button_t;

void button_init(button_t *btn, int gpio_num, uint32_t debounce_ms);

// Викликати кожен тік головного циклу. Неблокуюча, без переривань —
// це той самий підхід "опитування + антидребезг", що й на вашому скріні,
// лише замість millis() використовує esp_timer_get_time().
void button_update(button_t *btn);

// Повертає true рівно один раз — на момент підтвердженого натискання.
bool button_consume_press_event(button_t *btn);