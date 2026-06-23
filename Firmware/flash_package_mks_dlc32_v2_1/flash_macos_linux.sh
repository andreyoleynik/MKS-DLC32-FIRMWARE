#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-}"
if [[ -z "$PORT" ]]; then
  echo "Usage: $0 <serial-port>"
  echo "Example: $0 /dev/ttyUSB0"
  exit 1
fi

python3 -m esptool \
  --chip esp32 \
  --port "$PORT" \
  --baud 460800 \
  --before default_reset \
  --after hard_reset \
  write_flash -z \
  --flash_mode dout \
  --flash_freq 80m \
  --flash_size 8MB \
  0x1000 bootloader.bin \
  0x8000 partitions.bin \
  0xE000 boot_app0.bin \
  0x10000 firmware.bin

echo "Flash complete"
