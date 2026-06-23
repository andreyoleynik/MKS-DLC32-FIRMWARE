MKS DLC32 v2.1 manual flash package (no PlatformIO required)

Files in this folder:
- bootloader.bin
- partitions.bin
- boot_app0.bin
- firmware.bin
- fullflash_8MB.bin  (single-file combined image)
- flash_windows_fullflash.bat  (easy Windows script for non-programmers)

Target chip/config:
- Chip: ESP32
- Flash mode: dout
- Flash frequency: 80m
- Flash size: 8MB
- Partitions CSV used: esp32_8MiB.csv

Write addresses:
- 0x1000   bootloader.bin
- 0x8000   partitions.bin
- 0xE000   boot_app0.bin
- 0x10000  firmware.bin   (OTA slot app0)

Notes:
- boot_app0.bin is required for OTA partition layouts.
- If you need to flash app1 instead of app0, write firmware.bin at 0x310000.

esptool command (Linux/macOS):
python3 -m esptool --chip esp32 --port <PORT> --baud 460800 --before default_reset --after hard_reset write_flash -z --flash_mode dout --flash_freq 80m --flash_size 8MB 0x1000 bootloader.bin 0x8000 partitions.bin 0xE000 boot_app0.bin 0x10000 firmware.bin

esptool command (Windows):
py -m esptool --chip esp32 --port COMx --baud 460800 --before default_reset --after hard_reset write_flash -z --flash_mode dout --flash_freq 80m --flash_size 8MB 0x1000 bootloader.bin 0x8000 partitions.bin 0xE000 boot_app0.bin 0x10000 firmware.bin

Single-file flashing (recommended for non-PlatformIO PC):
- Linux/macOS:
  python3 -m esptool --chip esp32 --port <PORT> --baud 460800 --before default_reset --after hard_reset write_flash -z --flash_mode dout --flash_freq 80m --flash_size 8MB 0x0 fullflash_8MB.bin
- Windows:
  py -m esptool --chip esp32 --port COMx --baud 460800 --before default_reset --after hard_reset write_flash -z --flash_mode dout --flash_freq 80m --flash_size 8MB 0x0 fullflash_8MB.bin

Very easy Windows flow (no PlatformIO, one command):
1) Install Python for Windows and check "Add Python to PATH".
2) Open cmd in this folder.
3) Install tool once:
  py -m pip install esptool
4) Run script:
  flash_windows_fullflash.bat COM5
  (replace COM5 with actual port from Device Manager)

How to rebuild fullflash_8MB.bin (developer machine):
1) From Firmware root run:
  ./flash_package_mks_dlc32_v2_1/rebuild_fullflash_macos_linux.sh mks_dlc32_v2_1
2) Script will:
  - build firmware via PlatformIO,
  - copy fresh firmware.bin and partitions.bin into this folder,
  - recreate fullflash_8MB.bin and fullflash_8MB.bin.zip.

Optional filesystem (SPIFFS):
- Partition offset from esp32_8MiB.csv: 0x610000
- Build of spiffs.bin currently fails in this project due file:
  Grbl_Esp32/data/index.html.gz.bak_xyz_single_table
  (SPIFFS_write error)
- After removing/renaming that file and rebuilding FS image, flash with:
  ... write_flash ... 0x610000 spiffs.bin
