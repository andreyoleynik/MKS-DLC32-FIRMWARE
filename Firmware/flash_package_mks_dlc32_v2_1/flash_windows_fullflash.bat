@echo off
setlocal

if "%~1"=="" (
  echo Usage: flash_windows_fullflash.bat COMx
  echo Example: flash_windows_fullflash.bat COM5
  exit /b 1
)

set PORT=%~1

py -m esptool --chip esp32 --port %PORT% --baud 460800 --before default_reset --after hard_reset write_flash -z --flash_mode dout --flash_freq 80m --flash_size 8MB 0x0 fullflash_8MB.bin

if errorlevel 1 (
  echo Flash failed
  exit /b 1
)

echo Flash complete
endlocal
