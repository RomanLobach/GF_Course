# Документація

| Файл | Що всередині |
|---|---|
| [protocol_spec.md](protocol_spec.md) | формат службового й тестового пакетів, типи повідомлень, ACK/retry, реальні пакети в HEX |
| [field_checklist.md](field_checklist.md) | перевірки перед виходом у поле: що → як → критерій |
| [power_cut_experiment.md](power_cut_experiment.md) | обрив живлення під час запису конфігу і штучно розірваний запис |
| [ota_update.md](ota_update.md) | оновлення по Wi-Fi і відкат: як влаштовано, перевірки на платах |
| [radio_bench.md](radio_bench.md) | радіовимірювальна пара LoRa/FSK: підхід, профілі, результати, waterfall |
| [log_viewer.html](log_viewer.html) | переглядач логів обох плат (відкрити в браузері, ввести IP плат) |
| [lora-log-export.csv](lora-log-export.csv) | експорт логів вимірювань з переглядача, з нього взято результати |

## Логи з плат

Вивід консолі, записаний з реальних плат. `> команда` — що введено, `### [...]` — що відбувалось
між командами.

| Лог | Версія | Що показує |
|---|---|---|
| [base-first-boot-0.2.0.txt](logs/base-first-boot-0.2.0.txt) | 0.2.0 (до тегу) | перший старт: `version`, `post`, `config show`, міграція конфігу 0→1 |
| [rover-config-0.2.0.txt](logs/rover-config-0.2.0.txt) | 0.2.0 (до тегу) | `config get/set/reset`, відмова поза межами, значення після `reboot` |
| [rover-post-fail-0.2.0.txt](logs/rover-post-fail-0.2.0.txt) | 0.2.0 (до тегу) | збірка з примусовим збоєм самотесту: маска `0x80` |
| [rover-power-cut-0.2.0-a.txt](logs/rover-power-cut-0.2.0-a.txt), [-b](logs/rover-power-cut-0.2.0-b.txt) | 0.2.0 (до тегу) | обрив живлення під `config stress` |
| [rover-ota-local-0.3.0.txt](logs/rover-ota-local-0.3.0.txt) | 0.3.0 (до тегу) | оновлення з локального сервера, відкат, обрив завантаження, скасування з меню |
| [base-ota-1.0.0.txt](logs/base-ota-1.0.0.txt), [rover-ota-1.0.0.txt](logs/rover-ota-1.0.0.txt) | 0.4.0 → 1.0.0 | оновлення з GitHub Releases, міграція конфігу 1→2, `version`, `post`, `log` |
| [rover-tear-test-1.0.0.txt](logs/rover-tear-test-1.0.0.txt) | 1.0.0 | межі значень, `config tear-test` → старт зі старим значенням |
| [rover-power-cut-1.0.0.txt](logs/rover-power-cut-1.0.0.txt) | 1.0.0 | обрив живлення під `config stress` |
| [base-meas.txt](logs/base-meas.txt), [rover-meas.txt](logs/rover-meas.txt) | 0.2.0 (до тегу) | прохід вимірювання з боку кожної плати |
| [base-packets-hex.txt](logs/base-packets-hex.txt), [rover-packets-hex.txt](logs/rover-packets-hex.txt) | 0.2.0 (до тегу) | службові пакети в HEX (джерело прикладів для специфікації) |

## Скріншоти

| Файл | Що показує |
|---|---|
| [v0.1.0-rover-version.png](screenshots/v0.1.0-rover-version.png) | `version` на релізі 0.1.0 |
| [v0.1.0-rover-syslog.png](screenshots/v0.1.0-rover-syslog.png) | системний лог через кілька перезапусків |
| [v0.1.0-dev-rover-console.png](screenshots/v0.1.0-dev-rover-console.png) | консоль збірки перед 0.1.0 |
| [v0.1.0-dev-base-http-version.png](screenshots/v0.1.0-dev-base-http-version.png), [v0.1.0-dev-base-http-syslog.png](screenshots/v0.1.0-dev-base-http-syslog.png) | `GET /version`, `GET /syslog` |
| [v0.2.0-viewer-post-telemetry.png](screenshots/v0.2.0-viewer-post-telemetry.png) | результат самотесту в телеметрії (переглядач, `/info`) |
