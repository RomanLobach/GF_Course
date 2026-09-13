# HW7 — FreeRTOS на ESP32

Плата: TTGO T3 v1.6.1 (ESP32 + SX1276 LoRa), `framework = arduino`, PlatformIO.
Кнопка — GPIO39, LED — GPIO15

Три частини зібрані як окремі PlatformIO environments (`build_src_filter` вибирає один
`.cpp` з `setup()/loop()` на build):

| Environment | Файл | Задача |
|---|---|---|
| `ttgo-lora32-v1` (default) | `src/main.cpp` | Частина 1 |
| `deadlock` | `src/mutex_deadlock.cpp` | Частина 2 |
| `lora-async` | `src/lora_async.cpp` | Бонус |

```bash
pio run -e <env> -t upload -t monitor
```

## Частина 1 — Queue + два ядра

`buttonTask` (ядро 0) з дебаунсом (30 мс) читає GPIO39, розрізняє одинарний/подвійний
клік (вікно 300 мс) і зсуває індекс масиву інтервалів `{250, 500, 1000, 2000} мс` вперед
(одинарний) або назад (подвійний) по колу. Нове значення передається через
`xQueueOverwrite` у чергу-mailbox довжиною 1 — без спільної глобальної змінної.
`ledTask` (ядро 1) читає чергу через `xQueuePeek` і блимає GPIO15 з отриманим інтервалом.

**Результат:** `logs/part1_serial.log` — 4 одинарних кліки (250→500→1000→2000→250) і
4 подвійних (250→2000→1000→500→250), поведінка відповідає умові.

## Частина 2 — навмисний deadlock + watchdog

`taskA` (ядро 0) бере `mutexA` → `mutexB`; `taskB` (ядро 1) бере `mutexB` → `mutexA`
(circular wait, обидва mutex не рекурсивні). Обидві задачі зареєстровані в Task Watchdog
Timer (`esp_task_wdt_init(5s, panic=true)` + `esp_task_wdt_add`), тож коли блокуються
назавжди на `xSemaphoreTake(..., portMAX_DELAY)`, вони перестають скидати watchdog.

**Результат:** `logs/part2_deadlock_serial.log` — `task_wdt: Task watchdog got triggered`
для `taskA`/`taskB`, `abort()`, перезавантаження; далі цикл повторюється нескінченно
(очікувано — сценарій навмисно ламає систему).

## Бонус — асинхронна LoRa-передача

`ledTask` (ядро 1) блимає GPIO15 кожні 100 мс незалежно від радіо. `loraTask` (ядро 0)
через `RadioLib` (SX1276) передає
32-байтні пакети з SF11 асинхронно: `radio.startTransmit()` + завершення через
переривання на DIO0 (`setDio0Action`), без блокуючого `transmit()`.

**Результат:** `logs/part3_lora_serial.log` — `radio.begin()` без помилок, стабільний
вивід `LoRa: packet sent` кожну секунду. Стабільність 100-мс блимання LED під час TX
серійним логом не перевірялась (щоб не спотворювати таймінг зайвим `Serial.print` у
LED-задачі) — підтверджується візуально/відео.
