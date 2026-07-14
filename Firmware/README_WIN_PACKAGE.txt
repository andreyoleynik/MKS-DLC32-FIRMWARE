Оффлайн-пакет прошивки для Windows (MKS DLC32 v2.1)

Цель:
- Пользователь на Windows должен прошить устройство без установки Python и PlatformIO.

Состав пакета (обязательно):
- flash_preserve_nvs_full_win.bat (скрипт прошивки)
- flash_fs_only_win.bat (скрипт прошивки)
- firmware/bootloader.bin -> 0x1000
- firmware/partitions.bin -> 0x8000
- firmware/boot_app0.bin -> 0xE000
- firmware/firmware.bin -> 0x10000
- firmware/firmware.bin -> 0x310000
- firmware/spiffs.bin -> 0x610000
- tools/esptool.exe (утилита прошивки)

Важно:
- Скрипт полной прошивки сохраняет NVS (0x9000-0xDFFF), пользовательские настройки не стираются.
- Прошиваются оба OTA-слота, включая адрес 0x310000.

Как запускать у пользователя на Windows:
1) Подключить плату и определить COM-порт (например, COM5).
2) Полная прошивка с сохранением настроек:
   flash_preserve_nvs_full_win.bat COM5
3) Только прошивка файловой системы:
   flash_fs_only_win.bat COM5

Если COM-порт не указан, скрипты используют COM3.

Как собрать пакет на машине разработчика:
1) Сначала собрать прошивку и образ файловой системы:
   pio run -e mks_dlc32_v2_1
   pio run -t buildfs -e mks_dlc32_v2_1
2) Подготовить папку пакета:
   python scripts/prepare_windows_flash_package.py --esptool-exe C:\path\to\esptool.exe
3) Заархивировать папку flash_package_mks_dlc32_v2_1 и передать пользователю.

Чтобы не указывать параметр --esptool-exe:
- Положите Windows-файл esptool.exe в папку tools в корне проекта.
- Итоговый путь: tools/esptool.exe
- После этого можно запускать просто:
   python scripts/prepare_windows_flash_package.py
