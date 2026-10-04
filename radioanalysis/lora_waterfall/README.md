# Waterfall сигналів LoRa/FSK

Запис ефіру з RTL-SDR Blog V4 і побудова waterfall (спектрограм) для кожного радіопрофілю пари,
поки плати працюють у режимі «Тест частот». Результати й висновки — у
[doc/radio_bench.md](../../doc/radio_bench.md#прихований-рівень--waterfall).

| Файл | Що робить |
|---|---|
| [capture_waterfall.py](capture_waterfall.py) | один профіль: запис `rtl_sdr`, пошук пакета за тривалістю, огляд усього запису й деталь одного пакета зі STFT під модуляцію |
| [run_all.py](run_all.py) | покроковий прохід по профілях 1–6 і службовому каналу `S` |
| [captures/20260928/](captures/20260928/) | знімки: `*_overview.png`, `*_detail.png`, `*_meta.json` (налаштування й виміряні значення) |

```
cd radioanalysis/lora_waterfall
~/radioconda/bin/python run_all.py --open          # усі профілі, gain 0 дБ
~/radioconda/bin/python capture_waterfall.py --profile 3 --gain 20
```

Потрібні `rtl_sdr` і Python з numpy/scipy/matplotlib (наприклад, radioconda). Таблиця `PROFILES`
у `capture_waterfall.py` повторює `BENCH_CONFIGS` з [include/Config.h](../../include/Config.h).
Біля SDR навіть ~8 дБ підсилення перевантажує АЦП, тож за замовчуванням gain 0 (скрипт попереджає
про кліпінг). Сирі IQ-записи (`*.cu8`) у репозиторій не потрапляють.

Для профілів 1 і 2 є по два знімки; у звіті використано пізніші (`213730`, `213843`).
