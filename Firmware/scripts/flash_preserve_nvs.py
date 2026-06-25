"""
Flash mks_dlc32_v2_1 preserving the NVS partition (user settings).

Writes each region individually via esptool write_flash, intentionally
skipping NVS (0x9000–0xDFFF) so stored $-settings survive the update.

Flashed regions:
  0x1000   bootloader.bin
  0x8000   partitions.bin
  0xE000   boot_app0.bin       (OTA selector — reset to boot from app0)
  0x10000  firmware.bin        (app0 slot)
  0x310000 firmware.bin        (app1 slot — duplicate for robustness)
  0x610000 spiffs.bin          (web UI filesystem)

Skipped:
  0x9000–0xDFFF  NVS           (user $-settings live here → preserved)
  0x7F0000       coredump      (not touched)
"""

from pathlib import Path
import subprocess
import sys


def main() -> int:
    env_name = sys.argv[1] if len(sys.argv) > 1 else "mks_dlc32_v2_1"
    project_dir = Path(__file__).resolve().parent.parent
    build_dir = project_dir / ".pio" / "build" / env_name

    required = {
        "bootloader": build_dir / "bootloader.bin",
        "partitions": build_dir / "partitions.bin",
        "boot_app0": build_dir / "boot_app0.bin",
        "firmware":  build_dir / "firmware.bin",
        "spiffs":    build_dir / "spiffs.bin",
    }

    missing = [str(p) for p in required.values() if not p.exists()]
    if missing:
        print("Missing required files:")
        for p in missing:
            print(f"  {p}")
        return 1

    esptool = Path.home() / ".platformio" / "packages" / "tool-esptoolpy" / "esptool.py"
    python_bin = Path.home() / ".platformio" / "penv" / "bin" / "python"
    port = "/dev/cu.usbserial-210"

    flash_regions = [
        ("0x1000",   required["bootloader"]),
        ("0x8000",   required["partitions"]),
        ("0xE000",   required["boot_app0"]),
        ("0x10000",  required["firmware"]),
        ("0x310000", required["firmware"]),
        ("0x610000", required["spiffs"]),
    ]

    print("NVS at 0x9000–0xDFFF will be PRESERVED (user settings kept)")
    print()

    addr_file_args = []
    for addr, path in flash_regions:
        addr_file_args += [addr, str(path)]

    cmd = [
        str(python_bin),
        str(esptool),
        "--chip", "esp32",
        "--port", port,
        "--baud", "460800",
        "--before", "default_reset",
        "--after", "hard_reset",
        "write_flash",
        "--flash_mode", "dout",
        "--flash_freq", "80m",
        "--flash_size", "8MB",
        *addr_file_args,
    ]

    print("Flashing (NVS preserved):")
    for addr, path in flash_regions:
        print(f"  {addr}  {path.name}")
    print()

    result = subprocess.run(cmd)
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
