# Скрипти

| Скрипт | Коли запускається | Що робить |
|---|---|---|
| [version.py](version.py) | сам, перед кожною збіркою (`extra_scripts = pre:` у `platformio.ini`) | бере версію з `git describe --match 'hw6-v*'` і передає `FW_VERSION`, `FW_GIT_HASH`, `FW_BUILD_DATE`, `FW_DIRTY` як `-D` |
| [make_manifest.py](make_manifest.py) | у CI на тег `hw6-v*` | перейменовує образи в `ttgo-lora-bench-<роль>-X.Y.Z.bin`, пише `manifest.json` (URL, розмір, SHA-256) |
| [ota_server.py](ota_server.py) | вручну, для розробки | локальний HTTP-сервер оновлень з образів `.pio/build`; на платі `config set ota_url http://<ПК>:8000/manifest.json`. Підставляє окремий образ для демо відкату |
| [console_session.py](console_session.py) | вручну | надсилає команди в консоль плати й одразу пише вивід у файл (так записано логи в `doc/logs/`) |

Python 3 з `pyserial` (є в середовищі PlatformIO: `~/.platformio/penv/bin/python`). Подробиці й
приклади — у docstring кожного скрипта.
