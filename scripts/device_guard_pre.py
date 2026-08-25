"""Install early Device Guard workarounds needed by the ESP32 platform."""

import os
import subprocess
import sys

Import("env")


original_run = subprocess.run
original_clone = type(env).Clone


def find_esptool_python():
    """Return the PlatformIO-core Python that can import tool-esptoolpy."""
    package_dir = env.PioPlatform().get_package_dir("tool-esptoolpy")
    if package_dir:
        core_dir = os.path.dirname(os.path.dirname(package_dir))
        for relative in (("penv", "Scripts", "python.exe"),
                         ("penv", "bin", "python")):
            candidate = os.path.join(core_dir, *relative)
            if os.path.isfile(candidate):
                return candidate
    return sys.executable


esptool_python = find_esptool_python()


def run_with_python_esptool(args, *positional, **kwargs):
    """Route the platform's direct esptool subprocess calls through Python."""
    if (
        isinstance(args, (list, tuple))
        and args
        and os.path.basename(str(args[0])).lower() == "esptool.exe"
    ):
        args = [esptool_python, "-m", "esptool", *args[1:]]
    return original_run(args, *positional, **kwargs)


def clone_with_compatible_archiver(self, *args, **kwargs):
    cloned = original_clone(self, *args, **kwargs)
    cloned.Replace(
        AR="xtensa-esp32-elf-ar",
        RANLIB="xtensa-esp32-elf-ar",
        RANLIBFLAGS=["s"],
    )
    return cloned


subprocess.run = run_with_python_esptool
env.AddMethod(clone_with_compatible_archiver, "Clone")
