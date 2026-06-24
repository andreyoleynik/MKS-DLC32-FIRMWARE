# MKS-DLC32-FIRMWARE — CNC-форк

Прошивка контроллера **MKS DLC32 v2.1** (ESP32) для ЧПУ-станка со шпинделем.
Форк ветки `MKS-DLC32-NEW-CNC` от [IDLEVEL/MKS-DLC32-FIRMWARE](https://github.com/IDLEVEL/MKS-DLC32-FIRMWARE),
которая, в свою очередь, основана на брошенной официальной прошивке
[makerbase-mks/MKS-DLC32-FIRMWARE](https://github.com/makerbase-mks/MKS-DLC32-FIRMWARE)
(движок Grbl_ESP32 v1.1).

Зачем форк: оригинал makerbase заброшен (последний коммит — январь 2023),
исходники свежее V2.1 не публикуются. IDLEVEL добавил CNC-функции (OTA, выносной
пульт, переписанный Telnet), но его ветка не собиралась на Linux и тащила железо,
которого нет на стоковой плате. Этот форк это чинит и причёсывает под обычную DLC32 v2.1.

Подробный технический разбор и план доработок — в [`docs/FORK_NOTES.md`](docs/FORK_NOTES.md).

## Целевое железо

- Плата: **MKS DLC32 v2.1** — ESP32-WROOM-32U (8 МБ flash, 520 КБ RAM), степперы через
  I2S-сдвиговые регистры. Полная спека — [`docs/BOARD_SPEC.md`](docs/BOARD_SPEC.md).
- Экран: штатный тач **MKS TS35 (3.5")** — рисуется через LVGL.
- Назначение: **ЧПУ-фрезер** (3 оси XYZ, шпиндель с PWM).

Карта пинов под эту плату задаётся в `Firmware/Grbl_Esp32/src/Machines/i2s_out_xyz_mks_dlc32.h`
(флаги `USE_BOARD_V2_0` + `USR_Z_MOTOR` включены):

| Функция | Пин |
|---|---|
| Шпиндель/лазер PWM | GPIO32 |
| Probe | GPIO22 |
| Концевики X / Y / Z | GPIO36 / GPIO35 / GPIO34 |
| Степперы X/Y/Z | I2S (BCK16 / WS17 / DATA21) |
| TFT TS35 (SPI) | SCK18, MISO19, MOSI23, CS25, RS33, RST27, EN5, TouchCS26 |

Все 3 оси полноценные (движение/джоггинг/probe/обнуление по Z работают).
Авто-хоминг по Z выключен в дефолтах (`$22=0`) — включается через настройки или
`DEFAULT_HOMING_ENABLE 1` + `DEFAULT_HOMING_CYCLE_1 (bit(Z_AXIS))`.

## Прошивка готовыми бинарниками (без PlatformIO)

В [`release/`](release/) лежит всё для прошивки по USB: бинарники + скрипты
`flash_usb.sh` (Linux/macOS) и `flash_usb.bat` (Windows). Нужен только Python
с esptool (`pip install esptool`). Настройки Wi-Fi/GRBL при прошивке сохраняются.
Подробная инструкция: [`release/README.md`](release/README.md).

## Сборка

Нужен [PlatformIO Core](https://platformio.org/) (или расширение PlatformIO в VS Code).

```bash
cd Firmware
pio run -e mks_dlc32_v2_1            # сборка (по USB)
pio run -e mks_dlc32_v2_1 -t upload  # прошивка по USB (укажите порт в platformio.ini)
```

Артефакт: `Firmware/.pio/build/mks_dlc32_v2_1/firmware.bin`.

Окружения (`Firmware/platformio.ini` + `Firmware/ini/mks_dlc32.ini`):

| env | Назначение |
|---|---|
| `mks_dlc32_v2_1` | Основное, прошивка по USB. Партиция `esp32_8MiB.csv` (2× OTA-слот по 3 МБ) |
| `mks_dlc32_v2_1_ota` | OTA по воздуху (espota на 192.168.4.1:3232). Та же партиция 8 МБ |

Обе сборки используют одинаковую таблицу разделов `esp32_8MiB.csv` и дают идентичный
бинарник, поэтому прошитая по USB плата сразу готова к обновлению по воздуху (OTA).

## Тесты

Чистую логику (без зависимостей от железа) покрываем нативными unit-тестами:

```bash
cd Firmware
pio test -e native
```

Тесты лежат в `Firmware/test/`. Сейчас покрыт классификатор политики SD↔USB
(`line_safe_during_sd_job`, см. `test/test_sd_job_policy/`).

## Внешний пульт (опция, по умолчанию выключен)

IDLEVEL добавил поддержку выносного пульта на ёмкостных кнопках **MPR121** (по UART) и
беспроводного джойстика. На стоковой DLC32 v2.1 этого железа нет, поэтому поддержка
вынесена в компайл-тайм переключатель и **по умолчанию выключена** — так не тянется
библиотека `Adafruit_MPR121`, не занимаются GPIO 4/0 (IIC) и экономится flash.

Включить пульт — раскомментировать одну строку в `Firmware/Grbl_Esp32/src/Config.h`:

```c
#define ENABLE_EXTERNAL_BOARD   /* enable support for external board */
```

Прошивка джойстика/Bluetooth-моста лежит в `Joystick/` и `GRBL_Bluetooth/` (отдельные проекты).

## Статус

Всё ниже **проверено на реальной плате MKS DLC32 v2.1** (ESP32-D0WD, flash 8 МБ) —
протокол испытаний в [`docs/CODE_REVIEW.md`](docs/CODE_REVIEW.md).

- ✅ Собирается на Linux/Mac/Windows (исправлен Windows-only путь инклуда).
- ✅ Пульт IDLEVEL вынесен в переключатель, по умолчанию off.
- ✅ OTA по воздуху работает: партиция под реальные 8 МБ (`esp32_8MiB.csv`,
  два app-слота по 3 МБ), `pio run -e mks_dlc32_v2_1_ota -t upload --upload-port <IP>`.
- ✅ Wi-Fi: явный авто-реконнект STA при обрыве (раз в 15 с), mDNS в AP и STA
  (`http://mks-dlc32.local`). Сеть должна быть WPA2 2.4 ГГц (WPA3-only ESP32 не умеет).
- ✅ SD ↔ USB/сеть: во время печати с карты со второго интерфейса проходят realtime
  и read-only запросы (`?`, `$$`, `$G`…), движение отклоняется `error:120`
  **без обрыва печати**.
- ✅ WebUI: канал WebUI↔ядро жив (вкладка настроек не висит, #25), большие ответы
  не теряются; имя устройства по умолчанию `mks-dlc32`, меняется в WebUI.
- ✅ Telnet :23, HTTP-загрузка файлов на SD (`/upload`). Карта — только FAT32
  (exFAT не монтируется — карты >32 ГБ переформатировать). ⚠️ Во время выполнения
  задания с SD загрузка нового файла **намеренно блокируется** (карта занята
  чтением G-code) — заливать до запуска; после завершения работает снова.
- ✅ Удалённое управление через центральный сервер: плата сама подключается к нему
  по TCP и предъявляет токен и имя станка (`$Remote/Server`, `$Remote/Token`,
  `$Remote/Name`, хранятся в NVS) — инструкция в
  [`docs/REMOTE_CONTROL.md`](docs/REMOTE_CONTROL.md).
- 🔜 Фикс инициализации языка экрана (`=` вместо `==` в `mc_language_init`) — косметика.
- 🔜 По умолчанию аутентификация выключена — смените пароль AP в WebUI.

Дорожная карта и список багов оригинала — в [`docs/FORK_NOTES.md`](docs/FORK_NOTES.md).

## Лицензия

GPLv3 (наследуется от Grbl / Grbl_ESP32). См. [`LICENSE`](LICENSE).
