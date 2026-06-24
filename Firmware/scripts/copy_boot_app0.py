from pathlib import Path
import shutil

Import("env")


def copy_boot_app0(source, target, env):
    framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    if not framework_dir:
        print("boot_app0 copy skipped: framework-arduinoespressif32 package not found")
        return

    src = Path(framework_dir) / "tools" / "partitions" / "boot_app0.bin"
    if not src.exists():
        print(f"boot_app0 copy skipped: {src} not found")
        return

    build_dir = Path(env.subst("$BUILD_DIR"))
    dst = build_dir / "boot_app0.bin"
    shutil.copy2(src, dst)
    print(f"Copied {src} -> {dst}")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_boot_app0)
