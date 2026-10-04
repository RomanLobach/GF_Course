# Експеримент: вимкнення живлення під час запису конфігу

**Мета.** Переконатися, що після обриву живлення посеред збереження конфігу плата стартує з
останнім *коректним* значенням, а не з частково записаним чи пошкодженим.

## Як влаштовано збереження

Конфіг — один блоб у NVS, у двох слотах (`ttgo_cfg/a`, `ttgo_cfg/b`), див.
[src/ConfigStore.h](../src/ConfigStore.h):

```
| magic 0xC0F1 | cfg_version | generation | payload len | payload | CRC32 |
```

- Запис іде **завжди в неактивний слот** з `generation + 1`, потім читається назад і
  порівнюється. Активний слот під час запису не чіпається.
- При старті береться валідний слот (magic + довжина + CRC32) з **найбільшою generation**.
  Зламаний слот ігнорується, і плата стартує з другого.
- Додатково NVS сам по собі пише запис атомарно (новий запис → позначка валідності → стирання
  старого), тож обрив на будь-якому кроці лишає хоча б одну цілу копію.

## Методика

1. Плата Rover без акумулятора, живлення лише від USB.
2. У консолі `config stress 5000` — безперервні збереження: кожна ітерація пише
   `ota_url = http://stress.test/N`. Перед записом плата друкує `write N ->`, після успішного
   запису — `ok slot X gen G`.
3. Посеред потоку висмикнути USB-кабель.
4. Через ~5 с вставити кабель; плата стартує сама. `log`, `post`, `config show`.
5. `config reset` повертає `ota_url` до дефолту.

Скрипт [scripts/console_session.py](../scripts/console_session.py) записує вивід консолі у файл
одразу, тож останній рядок перед обривом зберігається.

## Результати на релізі 1.0.0 (2026-10-04, Rover `93F5FC`)

### Обрив живлення ([logs/rover-power-cut-1.0.0.txt](logs/rover-power-cut-1.0.0.txt))

Stress стартував із generation 1766. Останні рядки, що дійшли до ПК:

```
write 3154 -> ok slot B gen 4920
write 3155 -> ok slot A gen 4921
### [port lost: USB висмикнуто]
```

Після ввімкнення:

```
#380 b52 [     0.472] I cfg  : loaded, slot A gen 4925
#381 b52 [     0.535] I post : ok mask 0x00, batt 4276 mV, sx127x 0x12

config schema v2, load: loaded, active slot A
  slot A: ok, schema v2, gen 4925
  slot B: ok, schema v2, gen 4924
ota_url            = http://stress.test/3159
```

1766 + 3159 = 4925: плата завантажила цілий запис №3159, generation і значення узгоджені.
Записи 3156–3159 плата зробила вже після того, як зник USB-зв'язок із ПК, але ще до втрати
живлення, тому в консоль вони не потрапили. Обидва слоти цілі, POST = 0x00.

### Розірваний запис ([logs/rover-tear-test-1.0.0.txt](logs/rover-tear-test-1.0.0.txt))

Влучити висмикуванням точно в момент запису NVS важко, тож для гарантованого випадку є
`config tear-test`. Команда пише в неактивний слот новий блок (`ota_url = http://torn.write/`,
generation + 1), але лише його першу половину, як при обриві посеред запису, і перезапускає
плату. Перед тестом `wifi_timeout_s` змінено на 12, щоб було видно, яке значення завантажилось.

```
> config tear-test
W cfg  : tear-test: 74 of 148 B into slot B
...
W cfg  : recovered from the other slot, slot A gen 1765
I post : ok mask 0x00

config schema v2, load: recovered from the other slot, active slot A
  slot A: ok, schema v2, gen 1765
  slot B: damaged
ota_url            = https://github.com/RomanLobach/GF_Course/releases/download/hw6-latest/manifest.json
wifi_timeout_s     = 12
```

Плата відкинула пошкоджений слот B і стартувала зі старими значеннями зі слота A. Наступне
збереження (`config set wifi_timeout_s 8`) пішло в слот B і відновило його.

## Результати на 0.2.0 (до тегу, збірка 0.1.0-1-g0d3db36c, Rover `93F5FC`)

### Спроба 2 — з повним журналом ([logs/rover-power-cut-0.2.0-b.txt](logs/rover-power-cut-0.2.0-b.txt))

Останні рядки перед обривом:

```
write 973 -> ok slot A gen 1753
write 974 -> ok slot B gen 1754
write 975 ->
### [port lost]
```

Живлення зникло **під час запису №975** (у слот A, generation 1755): `write` надруковано, `ok` — ні.

Після ввімкнення:

```
#88 b17 [     0.579] I boot : git 0d3db36c+dirty, reset power-on, app0
#89 b17 [     0.587] I cfg  : loaded, slot B gen 1754
#90 b17 [     0.659] I post : ok mask 0x00, batt 4144 mV, sx127x 0x12

ota_url    = http://stress.test/974
```

Плата завантажила **запис №974** (слот B, generation 1754) — останній підтверджений. Перерваний
№975 не прочитано, конфіг не пошкоджено, POST = 0x00, біт 5 (config) = ok.
(Generation 1755 у слоті A, видима пізніше в `config show`, — це вже наступне, штатне збереження
session id після автоматичного хендшейку.)

### Спроба 1 ([logs/rover-power-cut-0.2.0-a.txt](logs/rover-power-cut-0.2.0-a.txt))

Журнал рядків `write` не зберігся (скрипт тоді писав файл лише наприкінці). Stress стартував із
generation 40; після ввімкнення — `loaded, slot A gen 779`, `ota_url = http://stress.test/739`:
40 + 739 = 779, тобто цілий запис №739 з узгодженими generation і значенням. POST = 0x00.

## Висновок

У всіх трьох обривах живлення і в тесті з розірваним записом плата стартувала зі старим коректним значенням (останнім повністю записаним), без
частково записаного конфігу й без помилок самотесту. Очікуваний результат підтверджено.
