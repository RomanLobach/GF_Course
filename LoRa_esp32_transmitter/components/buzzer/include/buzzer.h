#pragma once
#include <stdint.h>
#include <stdbool.h>

// Ініціалізація піна баззера (GPIO0 згідно зі схемою)
void buzzer_init(int gpio_num);

// Запустити короткий "пік" тривалістю duration_ms.
// Функція НЕблокуюча — миттєво повертає керування.
void buzzer_beep(uint32_t duration_ms);

// Викликати кожен тік головного циклу — вимикає баззер,
// коли час "піку" вичерпано. Теж неблокуюча.
void buzzer_update(void);