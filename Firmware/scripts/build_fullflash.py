from pathlib import Path
import shutil
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
        "firmware": build_dir / "firmware.bin",
        "spiffs": build_dir / "spiffs.bin",
    }

    missing = [str(path) for path in required.values() if not path.exists()]
    if missing:
        print("Missing required files:")
        for path in missing:
            print(f"  {path}")
        return 1

    esptool = Path.home() / ".platformio" / "packages" / "tool-esptoolpy" / "esptool.py"
    python_bin = Path.home() / ".platformio" / "penv" / "bin" / "python"

    if not esptool.exists():
        print(f"esptool not found: {esptool}")
        return 1
    if not python_bin.exists():
        print(f"python not found: {python_bin}")
        return 1

    output = build_dir / "fullflash.bin"
    fullflash_10000 = build_dir / "fullflash_0x10000_0x310000.bin"

    cmd = [
        str(python_bin),
        str(esptool),
        "--chip",
        "esp32",
        "merge_bin",
        "--output",
        str(output),
        "--flash_mode",
        "dout",
        "--flash_freq",
        "80m",
        "--flash_size",
        "8MB",
        "0x1000",
        str(required["bootloader"]),
        "0x8000",
        str(required["partitions"]),
        "0xE000",
        str(required["boot_app0"]),
        "0x10000",
        str(required["firmware"]),
        "0x310000",
        str(required["firmware"]),
        "0x610000",
        str(required["spiffs"]),
    ]

    subprocess.run(cmd, check=True)
    shutil.copy2(output, fullflash_10000)
    print(f"Created {output}")
    print(f"Created {fullflash_10000}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
