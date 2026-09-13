# HW5 — FreeRTOS tasks + Queue на LoRa32

Практика з FreeRTOS: розносимо обробку кнопки, радіо і GPS по окремих task'ах
і передаємо події між ними через Queue. Плата — LilyGO TTGO T3 LoRa32 v1.6.1
(ESP32 + SX1276 + SSD1306 OLED), два пристрої: передавач і приймач.

## Задача

1. Button Task визначає одинарне/подвійне натискання кнопки і кладе результат
   у Queue.
2. Головний `loop()` забирає результат з Queue і вирішує, який пакет
   відправити по радіо (`BUTTON SINGLE` / `BUTTON DOUBLE`).
3. Radio Task відправляє пакет по LoRa і логує затримку від постановки в чергу
   до відправки (бонусне завдання ⭐).
4. Додатково: передавач читає дату/час з GPS-модуля (SEQURE M10-25Q),
   показує на дисплеї і раз на секунду шле це саме по LoRa. Приймач приймає
   і показує дату/час на своєму дисплеї.

## Архітектура

Два build environment в одному `platformio.ini` (`env:transmitter` і
`env:receiver`), роль вибирається через `-D DEVICE_ROLE_*` — код спільний
(`src/main.cpp`), розгалужений по `#ifdef`.

**Передавач:**
```
Button (GPIO39) → Button Task → buttonQueue → loop() → radioQueue → Radio Task → LoRa
GPS (UART, TinyGPSPlus)       → GPS Task ───────────────→ radioQueue ┘
```
- Button Task (пріоритет 3) — дебаунс + одинарний/подвійний клік
- Radio Task (пріоритет 2) — єдиний власник LoRa, шле все з `radioQueue`
- GPS Task (пріоритет 1) — читає NMEA, оновлює дисплей, раз/сек кладе
  `"DT <дата> <час>"` у ту саму `radioQueue`

**Приймач:**
```
LoRa → Radio Rx Task → displayQueue → Display Task → OLED
```

## Пінаут

| Призначення      | Пін      |
|-------------------|----------|
| Кнопка             | GPIO39 (input-only, без внутрішньої підтяжки) |
| LoRa SCK/MISO/MOSI/SS/RST/DIO0 | 5 / 19 / 27 / 18 / 23 / 26 |
| OLED SDA/SCL       | 21 / 22 (адреса 0x3C) |
| GPS RX/TX (UART2)  | 15 / 14 (SEQURE M10-25Q, 115200 бод) |

## Результати

Фінальні логи знято одночасно з обох плат: [`logs/transmitter_final.log`](logs/transmitter_final.log),
[`logs/receiver_final.log`](logs/receiver_final.log).

- Одинарний клік → `BUTTON SINGLE`, подвійний → `BUTTON DOUBLE`, приймач
  отримав обидва без спотворень (RSSI −23…−26 дБм).
- GPS дата/час передаються раз/секунду і приймаються коректно (sats: 3).
- Затримка "чергу → відправлено по радіо": ~57 мс для GPS-пакетів (кнопка
  чекає в черзі до `loop()`, тому дорожче — до ~400 мс, бо `loop()` перевіряє
  Queue раз на 50 мс).

## Збірка і прошивка

```sh
pio run -e transmitter -t upload   # передавач
pio run -e receiver -t upload      # приймач
pio device monitor -e transmitter  # serial-лог (тільки в інтерактивному терміналі)
```
