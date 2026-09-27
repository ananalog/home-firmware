# home-firmware

Прошивки ESP32 для системы [Home](https://github.com/ananalog/home) на ESP-IDF v5.
Проект — [`home/doc/05-firmware-core.md`](https://github.com/ananalog/home/blob/main/doc/05-firmware-core.md),
первое устройство — [`home/doc/06-co2-egg.md`](https://github.com/ananalog/home/blob/main/doc/06-co2-egg.md).

```
components/home_core/     общее ядро: Wi-Fi, BLE, связь с сервером, OTA, откат, сброс
external/home-protocol/   сабмодуль протокола (C-кодек подключается как компонент home_proto)
components/drivers/       acd1200, ssd1306, ws2812, button
devices/co2-egg/          датчик CO2: ESP32-C3 0.42" OLED + ASAIR ACD1200
scripts/                  fw-build.sh, fw-flash-usb.sh, fw-monitor.sh
```
