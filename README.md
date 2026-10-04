# ttgo-lora-bench — прошивка, готова до поля

Прошивка ESP32 для пари LilyGo TTGO T3 v1.6.1 (ESP32 + SX1276 + OLED + енкодер): Base передає,
Rover приймає. Одна прошивка на обидві плати, роль задається при збірці. Як пара вимірює LoRa/FSK
і що намірялось, описано в [doc/radio_bench.md](doc/radio_bench.md). Тут — як прошивку довели до
стану, коли пристрій можна віддати іншій людині й винести в поле.

## Завдання

Допрацювати мініпроєкт за 6 кроками:

1. **Version і Log** — команда `version` з hash збірки, який підставляється автоматично;
   кільцевий лог з рівнями.
2. **Config** — `config get/set/reset`, межі значень, зберігання в NVS, `cfg_version`, хоча б одна
   міграція між версіями.
3. **POST** — самотест щонайменше 4 блоків; результат — бітова маска в лозі й телеметрії.
4. **Protocol Spec** — 7 розділів, пакет у HEX із розбором.
5. **Польовий чекліст** — 5 перевірок «що → як → критерій».
6. **Зламати систему** — вимкнути живлення посеред запису конфігу; пристрій має стартувати зі
   старим коректним значенням.

## Результат

| Крок | Рішення | Підтвердження |
|---|---|---|
| 1. Version + Log | версія з git-тегу, hash і дата коміту — параметрами збірки; системний лог: RAM-кільце + кільцевий файл, рівні E/W/I/D | [`version` на 1.0.0](doc/logs/base-ota-1.0.0.txt#L66-L76), [`log`](doc/logs/base-ota-1.0.0.txt#L108-L133), [скрін 0.1.0](doc/screenshots/v0.1.0-rover-version.png) |
| 2. Config | блок у NVS у двох слотах (A/B) з CRC32, схема v2, таблиця меж, дві міграції | [get/set/reset і межі](doc/logs/rover-config-0.2.0.txt#L28-L119), [міграція 0→1](doc/logs/base-first-boot-0.2.0.txt#L67-L68), [міграція 1→2](doc/logs/base-ota-1.0.0.txt#L61-L62) |
| 3. POST | 8 бітів: живлення, NVS, радіо, OLED, flash, конфіг, причина перезапуску, примусовий збій | [POST = 0x00](doc/logs/base-ota-1.0.0.txt#L77-L86), [збій 0x80](doc/logs/rover-post-fail-0.2.0.txt#L11-L26), [телеметрія `/info`](doc/screenshots/v0.2.0-viewer-post-telemetry.png) |
| 4. Protocol Spec | службовий (4 Б) і тестовий (64 Б) пакети | [doc/protocol_spec.md](doc/protocol_spec.md) |
| 5. Чекліст | 13 перевірок з вимірюваними критеріями | [doc/field_checklist.md](doc/field_checklist.md) |
| 6. Злам | обрив USB під безперервним записом конфігу + штучно розірваний запис | [doc/power_cut_experiment.md](doc/power_cut_experiment.md) |

Понад завдання: оновлення по Wi-Fi з GitHub Releases з автоматичним відкатом
([doc/ota_update.md](doc/ota_update.md)) і налаштування Wi-Fi з телефона, без кабелю.

## Етапи

Проєкт живе в загальному репозиторії курсу
[RomanLobach/GF_Course](https://github.com/RomanLobach/GF_Course): кожне домашнє завдання там —
окрема orphan-гілка, ця — [`hw6`](https://github.com/RomanLobach/GF_Course/tree/hw6). Коміти йдуть
прямо в `hw6`. Теги в git спільні на весь репозиторій, тому релізи цієї домашки позначені тегами з
префіксом гілки, `hw6-vX.Y.Z`: так вони не перетинаються з релізами інших завдань, а `git describe
--match 'hw6-v*'` бачить лише свої. З тієї ж причини пристрої беруть оновлення не з
`releases/latest`, який спільний для всіх гілок, а з окремого ковзного релізу `hw6-latest`.

Кожен етап — реліз `hw6-vX.Y.Z` ([Releases](https://github.com/RomanLobach/GF_Course/releases),
подробиці — [CHANGELOG.md](CHANGELOG.md)). Починаючи з 0.3.0, нові версії ставляться лише
по Wi-Fi.

### 0.1.0 — версія, лог, консоль, CI (2026-09-29)

- [scripts/version.py](scripts/version.py) перед збіркою викликає `git describe --match 'hw6-v*'`
  і передає `FW_VERSION`, `FW_GIT_HASH`, `FW_BUILD_DATE` (дата коміту, щоб збірка була
  відтворюваною) і `FW_DIRTY` як `-D`. Між релізами версія виглядає як `X.Y.Z-N-gHASH`, з
  незакоміченими змінами — `-dirty`.
- `SysLog`: рівні ERROR/WARN/INFO/DEBUG, RAM-кільце на 64 записи і кільцевий файл на 256 записів у
  LittleFS. Лог переживає перезапуск, тож видно, що було перед збоєм. У flash пише лише поза
  вікном прийому пакетів.
- Консоль на Serial і ті самі команди через `POST /cmd` по Wi-Fi.
- GitHub Actions: збірка обох ролей на кожен push; на тег — реліз з образами й `manifest.json`.

### 0.2.0 — конфіг, Wi-Fi у NVS, самотест (2026-10-04)

- `ConfigStore`: заголовок (magic, `cfg_version`, generation, довжина) + payload + CRC32. Запис
  іде в неактивний слот і перечитується; при старті береться цілий слот з більшою generation.
  Межі перевіряються і при `config set`, і при читанні.
- Міграція 0→1: ID сесії, який стара прошивка тримала в окремому ключі NVS, переноситься в конфіг,
  а старий ключ видаляється.
- Wi-Fi-мережі — в окремому просторі NVS. Релізні образи не містять паролів.
- POST перед запуском інтерфейсу. Маска → системний лог, запис `POST` у CSV-лог, поле `post` у
  `GET /info`, попап на екрані.

### 0.3.0 — оновлення по Wi-Fi і відкат (2026-10-04)

- `manifest.json` по HTTPS → образ своєї ролі → потоковий запис у неактивний розділ з перевіркою
  SHA-256. Будь-який збій скасовує запис, і поточна прошивка лишається.
- Нова прошивка підтверджується лише після POST = 0 і збігу ролі, інакше завантажувач повертає
  попередню. Перевірено на образі з примусовим збоєм самотесту.
- Меню «Сервіс».

### 0.4.0 — налаштування Wi-Fi без кабелю (2026-10-04, поставлено по Wi-Fi)

- «Сервіс → Налаштувати Wi-Fi»: плата піднімає точку доступу `ttgo-lora-bench-XXXXXX` (WPA2,
  новий пароль щоразу, показується на екрані), сторінка на 192.168.4.1: мережі поруч, додавання
  й видалення збережених.
- «Сервіс → Версія».

### 1.0.0 — конфіг v2 і злам (2026-10-04, поставлено по Wi-Fi)

- Схема v2: `wifi_timeout_s` і `portal_timeout_min`. Міграція 1→2 відбулась під час оновлення
  0.4.0 → 1.0.0 на обох платах; конфіг v1 лишився в другому слоті на випадок відкату.
- `config tear-test` — штучно розірваний запис; повторено обрив живлення на релізі.

## Як перевірити

Консоль: `pio device monitor -e base` (або `-e rover`), далі:

```
version                       # версія, hash, роль, розділ, причина перезапуску
log 20                        # останні записи системного логу
post                          # маска самотесту по бітах
config show                   # схема, стан слотів, усі параметри з межами
config set wifi_timeout_s 31  # -> error: value out of range
config set wifi_timeout_s 12
config reset
config tear-test              # розірваний запис + перезапуск -> load: recovered
ota check                     # що пропонує hw6-latest
```

| Консоль | Що робить |
|---|---|
| `version` | збірка й пристрій |
| `log [n\|all]`, `log level error\|warn\|info\|debug`, `log clear`, `log stats` | системний лог |
| `post` | самотест |
| `config [show]`, `config get <ключ>`, `config set <ключ> <значення>`, `config reset` | конфіг |
| `config stress [n]`, `config tear-test` | експерименти з обривом (лише Serial) |
| `wifi list`, `wifi add <ssid> [пароль]`, `wifi del <n>` | мережі (add/del — лише Serial) |
| `ota [status]`, `ota check`, `ota update [--force]`, `ota cancel` | оновлення |
| `reboot` | перезапуск |

Параметри конфігу: `log_level` (0–3), `ota_url` (адреса маніфесту), `wifi_timeout_s` (3–30 с),
`portal_timeout_min` (1–60 хв); `session_id` — лише для читання.

HTTP у режимі «Передати по Wi-Fi»: `GET /info` (роль, версія, POST, напруга, заповнення логу),
`GET /version`, `GET /syslog`, `GET /logs.csv`, `POST /cmd`, `POST /erase-logs`,
`POST /reset-session`.

## Як працює прошивка

| Задача | Ядро | Що робить |
|---|---|---|
| `loop()` | 1 | протокол, радіо, меню, логи, конфіг, Wi-Fi, консоль — без блокувань |
| `inputTaskFunc` | 0 | опитування енкодера кожну 1 мс |
| `displayTaskFunc` | 0 | рендер OLED (нижчий пріоритет за енкодер) |

Задачі обмінюються лише через черги FreeRTOS. Мережеві операції, що блокують (TLS під час
оновлення), виконуються в окремій задачі.

| Модуль | Роль |
|---|---|
| [Version](src/Version.h) | дані збірки, `version`, `GET /version` |
| [SysLog](src/SysLog.h) | системний лог: рівні, RAM-кільце, кільцевий файл |
| [Console](src/Console.h) · [SystemCommands](src/SystemCommands.cpp) | команди Serial / HTTP |
| [ConfigStore](src/ConfigStore.h) | конфіг у NVS: A/B, CRC32, межі, міграції |
| [Post](src/Post.h) | самотест і маска |
| [Ota](src/Ota.h) | оновлення з GitHub Releases, відкат |
| [WifiStore](src/WifiStore.h) · [WifiPortal](src/WifiPortal.h) · [WifiOffload](src/WifiOffload.h) | мережі в NVS, точка доступу для налаштування, HTTP API |
| [SessionState](src/SessionState.h) · [RadioManager](src/RadioManager.h) · [FlashLog](src/FlashLog.h) | радіопротокол і лог вимірювань — див. [doc/radio_bench.md](doc/radio_bench.md) |
| [Menu](src/Menu.h) · [Display](src/Display.h) · [Encoder](src/Encoder.h) | інтерфейс |
| [Config.h](include/Config.h) | піни, частоти, профілі, таймінги, константи |

## Збірка, прошивка, релізи

```
pio run -e base -t upload      # Base
pio run -e rover -t upload     # Rover
pio device monitor -e base     # консоль
```

Wi-Fi на кожній платі: `IDLE` → «Сервіс → Налаштувати Wi-Fi», підключитися до
`ttgo-lora-bench-XXXXXX` з паролем з екрана, на сторінці вибрати мережу й ввести пароль, натиснути
енкодер. Для локальних збірок можна ще `include/secrets.h` (шаблон —
[include/secrets.example.h](include/secrets.example.h)).

Реліз: розділ у [CHANGELOG.md](CHANGELOG.md) → тег `hw6-vX.Y.Z` → push. CI збирає обидві ролі з
тегу, публікує `ttgo-lora-bench-<роль>-X.Y.Z.bin` і `manifest.json` (розмір, SHA-256) і оновлює
`hw6-latest`, звідки пристрої беруть оновлення («Сервіс → Оновити прошивку» або `ota update`).

## Структура

```
src/, include/        прошивка
doc/                  специфікація, чекліст, експерименти, логи з плат, переглядач логів
scripts/              версія збірки, маніфест релізу, локальний сервер оновлень, запис консолі
radioanalysis/        waterfall сигналів із RTL-SDR
.github/workflows/    збірка і релізи
```

Детальніше — [doc/README.md](doc/README.md), [scripts/README.md](scripts/README.md).

## Ліцензія

[MIT](LICENSE)
