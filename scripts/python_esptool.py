"""Use Device Guard-compatible entry points for the ESP32 build tools.

Some managed Windows installations block the generated ``esptool.exe`` shim
with Device Guard, even though the Python interpreter and esptool package are
allowed. PlatformIO uses OBJCOPY for bootloader generation, ERASETOOL for the
application image and erase operations, and UPLOADER for serial flashing, so
all three commands need replacing.

The toolchain's GCC-specific archive launchers can be blocked by the same
policy. GNU ar itself is allowed and provides both archive and index modes, so
use it directly for AR and as ``ar s`` for the ranlib step.
"""

import os

Import("env")


package_dir = env.PioPlatform().get_package_dir("tool-esptoolpy")
python = env.subst("$PYTHONEXE")
if package_dir:
    core_dir = os.path.dirname(os.path.dirname(package_dir))
    for relative in (("penv", "Scripts", "python.exe"),
                     ("penv", "bin", "python")):
        candidate = os.path.join(core_dir, *relative)
        if os.path.isfile(candidate):
            python = candidate
            break
python_esptool = f'"{python}" -m esptool'

env.Replace(
    OBJCOPY=python_esptool,
    ERASETOOL=python_esptool,
    AR="xtensa-esp32-elf-ar",
    RANLIB="xtensa-esp32-elf-ar",
    RANLIBFLAGS=["s"],
)

if env.subst("$UPLOAD_PROTOCOL") == "esptool":
    env.Replace(UPLOADER=python_esptool)
