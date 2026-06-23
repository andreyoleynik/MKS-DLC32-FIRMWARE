@echo off
setlocal

if "%~1"=="" (
  echo Usage: flash_windows.bat COMx
  echo Example: flash_windows.bat COM5
  exit /b 1
)

set PORT=%~1

py -m esptool --chip esp32 --port %PORT% --baud 460800 --before default_reset --after hard_reset write_flash -z --flash_mode dout --flash_freq 80m --flash_size 8MB 0x1000 bootloader.bin 0x8000 partitions.bin 0xE000 boot_app0.bin 0x10000 firmware.bin

if errorlevel 1 (
  echo Flash failed
  exit /b 1
)

echo Flash complete
endlocal
