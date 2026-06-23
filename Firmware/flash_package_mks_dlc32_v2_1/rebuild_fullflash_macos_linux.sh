#!/usr/bin/env bash
set -euo pipefail

# Usage:
#   ./rebuild_fullflash_macos_linux.sh [platformio_env]
# Example:
#   ./rebuild_fullflash_macos_linux.sh mks_dlc32_v2_1

ENV_NAME="${1:-mks_dlc32_v2_1}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${FW_ROOT}/.pio/build/${ENV_NAME}"

PIO_BIN="${HOME}/.platformio/penv/bin/platformio"
PY_BIN="${HOME}/.platformio/penv/bin/python"
ESPTOOL_PY="${HOME}/.platformio/packages/tool-esptoolpy/esptool.py"

if [[ ! -x "${PIO_BIN}" ]]; then
  echo "PlatformIO not found at: ${PIO_BIN}"
  exit 1
fi
if [[ ! -x "${PY_BIN}" ]]; then
  echo "Python (PlatformIO venv) not found at: ${PY_BIN}"
  exit 1
fi
if [[ ! -f "${ESPTOOL_PY}" ]]; then
  echo "esptool.py not found at: ${ESPTOOL_PY}"
  exit 1
fi

"${PIO_BIN}" run --environment "${ENV_NAME}" --project-dir "${FW_ROOT}"

cp "${BUILD_DIR}/firmware.bin" "${SCRIPT_DIR}/firmware.bin"
cp "${BUILD_DIR}/partitions.bin" "${SCRIPT_DIR}/partitions.bin"

"${PY_BIN}" "${ESPTOOL_PY}" --chip esp32 merge_bin \
  --output "${SCRIPT_DIR}/fullflash_8MB.bin" \
  --flash_mode dout \
  --flash_freq 80m \
  --flash_size keep \
  0x1000 "${SCRIPT_DIR}/bootloader.bin" \
  0x8000 "${SCRIPT_DIR}/partitions.bin" \
  0xE000 "${SCRIPT_DIR}/boot_app0.bin" \
  0x10000 "${SCRIPT_DIR}/firmware.bin"

# Keep a zip alongside the raw image for easier sharing on Windows.
(cd "${SCRIPT_DIR}" && zip -q -9 -r "fullflash_8MB.bin.zip" "fullflash_8MB.bin")

echo "Rebuilt: ${SCRIPT_DIR}/fullflash_8MB.bin"
echo "Updated: ${SCRIPT_DIR}/firmware.bin, ${SCRIPT_DIR}/partitions.bin"
