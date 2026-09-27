# home-firmware

Прошивки ESP32 для системы [Home](https://github.com/ananalog/home) на ESP-IDF v5.4.
Проект — [`home/doc/05-firmware-core.md`](https://github.com/ananalog/home/blob/main/doc/05-firmware-core.md),
первое устройство — [`home/doc/06-co2-egg.md`](https://github.com/ananalog/home/blob/main/doc/06-co2-egg.md).

## Структура

```
external/home-protocol/     сабмодуль протокола (C-кодек → компонент home_proto)
components/home_proto/      обёртка ESP-IDF над external/home-protocol/c
components/home_core/       ядро: Wi-Fi (DHCP/статика), поиск сервера (UDP), TCP-связь, BLE-настройка,
                            точки, OTA с откатом, заводской сброс, кнопка, пересылка логов
components/ssd1306/         драйвер OLED 72x40 + шрифты (tools/gen_fonts.py)
components/acd1200/         драйвер датчика CO2 ASAIR ACD1200 (I2C)
devices/co2-egg/            датчик CO2: ESP32-C3 «Egg» + ACD1200
devices/board-probe/        проверка новой платы: flash, I2C-скан, экран, светодиод, кнопка
partitions/4mb.csv          factory + ota_0 + ota_1 по 1.25 МБ
scripts/                    fw-build.sh, fw-flash-usb.sh, fw-monitor.sh
tools/                      manifest.py, check_size.py, gen_fonts.py
tests/host/                 тесты логики без ESP-IDF
```

## Сборка и прошивка

Нужен Docker (образ `espressif/idf:v5.4.2`) или установленный ESP-IDF 5.4.

```
git clone --recursive https://github.com/ananalog/home-firmware && cd home-firmware
scripts/fw-build.sh co2-egg                  # → dist/co2-egg/<версия>/
scripts/fw-flash-usb.sh co2-egg              # первая прошивка по USB: всё стирается, образ → factory
scripts/fw-monitor.sh                        # лог с платы
```

Обновления дальше — по воздуху: `homectl fw upload dist/co2-egg/<версия>/co2-egg-<версия>.bin && homectl fw flash <устройство>`.

Версия берётся из тега `co2-egg/vX.Y.Z` на HEAD, иначе `version.txt` + коммит.

## Новая плата

```
scripts/fw-build.sh board-probe && scripts/fw-flash-usb.sh board-probe && scripts/fw-monitor.sh
```
В логе: объём flash, найденные I2C-устройства на парах пинов, OLED; на экране — «PROBE OK».

## Новое устройство

1. `devices/<имя>/` по образцу `co2-egg`: `CMakeLists.txt`, `sdkconfig.defaults`, `version.txt`, `main/`.
2. В `main.c`: таблица точек `home_point_t`, колбэки `on_set`/`on_invoke`, `HOME_IMAGE_DESC(модель, маска ревизий)`,
   `home_start(&device)`; показания — `home_report(id, value)`.
3. Модель добавить в `home-protocol/schema.yaml` (enum `Model`).

Сервер, Mini App, CLI и Android строят интерфейс по описанию точек — их менять не нужно.
