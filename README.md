# HW11 — GPS-модуль на Raspberry Pi

Окремий процес на Raspberry Pi 5 для роботи з GPS-модулем SEQURE M10-25Q (u-blox M10), який раніше був підключений до польотного контролера.

Демонстрація: [demo.mp4](demo.mp4)

## 1. Raspberry Pi OS

- Raspberry Pi OS (Debian 13 «trixie», 64-bit), записана через Raspberry Pi Imager.
- Hostname `rasp`, користувач `roman`, доступ по SSH за ключем (`ssh roman@rasp.local`).

## 2. Підключення GPS через UART

| GPS     | Raspberry Pi 5           |
|---------|--------------------------|
| TX      | GPIO15 (RXD0, pin 10)    |
| RX      | GPIO14 (TXD0, pin 8)     |
| VCC     | 5V (pin 2)               |
| GND     | GND (pin 6)              |

Налаштування UART0 на GPIO14/15:

- `/boot/firmware/config.txt`:
  ```
  enable_uart=1
  dtparam=uart0=on
  ```
- з `/boot/firmware/cmdline.txt` прибрано `console=serial0,115200`, щоб ядро не використовувало порт як консоль.

Після перезавантаження порт доступний як `/dev/ttyAMA0`. Швидкість модуля — 115200 бод.

## 3. Програма на Python

Скрипт: [gps_reader.py](gps_reader.py), залежність — `pyserial` (`python3-serial`).

- **Ініціалізація UART** — відкриває `/dev/ttyAMA0`, 115200 8N1.
- **Налаштування GPS-модуля** — надсилає UBX `CFG-VALSET` (у RAM модуля): частота 1 Гц, з NMEA увімкнені лише GGA і RMC. Очікує `ACK` від модуля.
- **Постійне зчитування** — виділяє NMEA-речення з потоку (модуль також шле бінарні UBX-пакети), перевіряє контрольну суму, парсить RMC/GGA і раз на секунду виводить:
  ```
  2026-10-20 18:07:56 UTC | 50.450100, 30.523400 | alt 179.6 m | 0.0 km/h | sats 12 | hdop 0.59 | FIX
  ```
- Якщо модуль замовк на 3 с (напр. після скидання) — UART відкривається заново й модуль переналаштовується.
- Лог пишеться в stdout (journald) і в `gps.log`.

## 4. Автозапуск

systemd-сервіс: [gps-reader.service](gps-reader.service)

```bash
sudo cp gps-reader.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now gps-reader
```

- запуск при старті системи (`WantedBy=multi-user.target`);
- від користувача `roman`, група `dialout` (доступ до UART);
- `Restart=always` — перезапуск через 3 с, якщо процес завершився.

## 5. Перевірка після перезавантаження

```bash
sudo reboot
# після старту:
systemctl is-enabled gps-reader   # enabled
systemctl is-active gps-reader    # active
journalctl -u gps-reader -b -f    # дані з GPS щосекунди
```

Після перезавантаження процес стартує самостійно (новий PID), у журналі видно відкриття UART, підтвердження налаштування модуля та безперервне зчитування GPS-даних:

```
INFO GPS reader starting
INFO UART opened: /dev/ttyAMA0 @ 115200 8N1
INFO GPS configured: 1 Hz, NMEA GGA+RMC only (RAM layer)
INFO 2026-10-20 18:07:54 UTC | ...
```
