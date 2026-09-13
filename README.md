# HW4 — Crash / Backtrace / Partition Table

Плата: **LilyGO TTGO T3 v1.6.1** (ESP32 + SX1276 LoRa), PlatformIO, Arduino framework.

Повний текст завдання: [task.md](task.md).

## Що зроблено

- `src/main.cpp` — меню через Serial (115200 бод), команди `1`–`4`:
  - `1` — crash: цілочисельне ділення на нуль
  - `2` — crash: запис за недійсною адресою (`0x1`)
  - `3` — бонус: перелік partition table через `esp_partition_find/get`
  - `4` — бонус: сирий hex-дамп самої partition table через `spi_flash_read()`
- `platformio.ini` — додано `build_type = debug`, `monitor_speed = 115200`,
  `monitor_filters = esp32_exception_decoder, time` для автоматичного декодування
  backtrace прямо в `pio device monitor`.
- Прошивку залито на реальну плату, обидва crash спровоковано наживо, логи збережено:
  - [`logs/raw_serial_log.txt`](logs/raw_serial_log.txt) — сирий вивід Serial Monitor
    (обидва Guru Meditation Error з регістрами й backtrace, вивід partition table, hex-дамп)
  - [`logs/decoded_backtrace.txt`](logs/decoded_backtrace.txt) — розшифрований backtrace

## Прийняті рішення

- **Обидва типи crash в одній прошивці**, вибір командою через Serial (`1`/`2`), а не
  окремі білди — так можна продемонструвати два різні сценарії без перепрошивання
  й без `#ifdef`-перемикачів.
- **Ділення на нуль** реалізоване через `volatile int` змінні — інакше компілятор
  міг би обчислити результат ділення на етапі компіляції (undefined behavior) і
  забрати crash оптимізацією.
- **Декодування backtrace через вбудований `esp32_exception_decoder`** (monitor filter
  PlatformIO), а не через окреме розширення VS Code (`dankeboy36/esp-exception-decoder`
  чи `me-no-dev/EspExceptionDecoder`) — цей фільтр використовує той самий механізм
  (`addr2line` по `firmware.elf`), що й обидва рекомендовані розширення. Для перевірки
  результат додатково підтверджено вручну через
  `xtensa-esp32-elf-addr2line -pfiaC -e firmware.elf <адреси>`. Ну і поки використовую 
  Clion, тому таке рішення. 
- **Бонусні пункти виконано обидва**: і читання partition table через офіційний
  `esp_partition` API, і сирий побайтовий дамп самого розділу таблиці розділів
  (`spi_flash_read` на офсеті `0x8000`, дефолтний `CONFIG_PARTITION_TABLE_OFFSET`).

## Результати

### Crash #1 — ділення на нуль

```
Guru Meditation Error: Core 1 panic'ed (IntegerDivideByZero). Exception was unhandled.
EXCCAUSE: 0x00000006
Backtrace: 0x400d15f5:0x3ffb2240 0x400d16ad:0x3ffb2270 0x400d33dd:0x3ffb2290
```

| Причина | Файл | Рядок | Функція |
|---|---|---|---|
| `IntegerDivideByZero` | `src/main.cpp` | 28 | `crashDivideByZero()` |

### Crash #2 — некоректний доступ до пам'яті

```
Guru Meditation Error: Core 1 panic'ed (LoadProhibited). Exception was unhandled.
EXCCAUSE: 0x0000001c   EXCVADDR: 0x00000001
Backtrace: 0x400d1627:0x3ffb2250 0x400d16b4:0x3ffb2270 0x400d33dd:0x3ffb2290
```

| Причина | Файл | Рядок | Функція |
|---|---|---|---|
| `LoadProhibited` (запис за адресою `0x1`) | `src/main.cpp` | 36 | `crashBadPointer()` |

Повний розбір обох backtrace — у [`logs/decoded_backtrace.txt`](logs/decoded_backtrace.txt).

### Бонус — Partition Table

Реальна таблиця розділів плати (команда `3`, `esp_partition` API):

```
name             type subtype address    size
nvs              0x01 0x02    0x00009000 20480 bytes
otadata          0x01 0x00    0x0000e000 8192 bytes
app0             0x00 0x10    0x00010000 1310720 bytes
app1             0x00 0x11    0x00150000 1310720 bytes
spiffs           0x01 0x82    0x00290000 1441792 bytes
coredump         0x01 0x03    0x003f0000 65536 bytes
```

Той самий розділ підтверджено сирим hex-дампом (команда `4`, `spi_flash_read`
на офсеті `0x8000`) — кожен запис починається з magic-байтів `aa 50`, повний вивід
у [`logs/raw_serial_log.txt`](logs/raw_serial_log.txt).

## Як відтворити

```bash
pio run                                   # збірка
pio run -t upload                         # прошивка
pio device monitor                        # відкрити Serial Monitor
                                           # (backtrace декодується автоматично)
```

У Serial Monitor надіслати `1`, `2`, `3` або `4` для відповідної дії з меню.
