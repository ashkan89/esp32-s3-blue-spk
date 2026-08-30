#!/usr/bin/env python3
"""
Host-side test for src/pin_check.h.

pin_check.h is pure preprocessor and static_assert -- it includes only
hw_config.h and ui_config.h, which in turn need nothing but <stdint.h>. So the
whole pin map can be checked without a board, without linking, and in under a
second, by asking the cross-compiler to parse a two-line translation unit with
various -D overrides and looking at whether it succeeded.

That is worth having because the assertions are the kind of code that quietly
stops asserting: a macro typo turns `PIN_OK_OUTPUT(x)` into something that is
true for everything, the build still passes, and nobody finds out until a pin
map has already been shipped wrong. Each case below states a pin configuration
and whether it must be accepted or rejected.

Run:  python scripts/test_pin_check.py
Exit: 0 if every case behaved, 1 otherwise (and the failures are printed).
"""

import glob
import os
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "src")


def find_compiler():
    """The xtensa g++ PlatformIO already installed, wherever it landed."""
    candidates = []
    for root in ("C:/p/packages", os.path.expanduser("~/.platformio/packages")):
        candidates += glob.glob(
            os.path.join(root, "toolchain-xtensa*", "bin", "xtensa-esp32-elf-g++*")
        )
        candidates += glob.glob(
            os.path.join(root, "toolchain-xtensa*", "bin", "xtensa-esp-elf-g++*")
        )
    for name in ("g++", "clang++"):
        from shutil import which

        found = which(name)
        if found:
            candidates.append(found)
    return candidates[0] if candidates else None


CASES = [
    # (name, extra -D flags, must_compile)
    ("stock pin map", [], True),
    ("I2S data on a flash pin", ["-DPIN_MAP_I2S_DOUT=7"], False),
    ("I2S clock on a flash pin", ["-DPIN_MAP_I2S_BCLK=11"], False),
    ("I2S data on an input-only pin", ["-DPIN_MAP_I2S_DOUT=34"], False),
    ("WS2812 on an input-only pin", ["-DPIN_LEDS=39"], False),
    ("WS2812 on a flash pin", ["-DPIN_LEDS=9"], False),
    ("WS2812 on the I2C clock", ["-DPIN_LEDS=22"], False),
    ("WS2812 on the status LED", ["-DPIN_LEDS=2"], False),
    ("I2S data on the I2C clock", ["-DPIN_MAP_I2S_DOUT=22"], False),
    ("battery sense on ADC2", ["-DPIN_BATTERY_SENSE=25"], False),
    ("battery sense on ADC1", ["-DPIN_BATTERY_SENSE=36"], True),
    ("battery gauge switched off", ["-DPIN_BATTERY_SENSE=-1"], True),
    ("DFPlayer IO1 on an input-only pin", ["-DPIN_DF_IO1=35"], False),
    ("DFPlayer TX equals RX", ["-DPIN_DF_TX=16"], False),
    ("DFPlayer BUSY on the battery pin", ["-DPIN_DF_BUSY=34"], False),
    ("DFPlayer BUSY unwired", ["-DPIN_DF_BUSY=-1"], True),
    ("DFPlayer indicator LED unwired", ["-DPIN_DF_LED=-1"], True),
    ("a pin that does not exist on a WROOM", ["-DPIN_LEDS=29"], False),
    ("LED centre index past the end", ["-DLED_CENTRE_INDEX=7"], False),
    ("plain strip, no centre pixel", ["-DLED_CENTRE_INDEX=-1"], True),
    ("optional hardware all compiled out",
     ["-DLEDS_ENABLED=0", "-DDFPLAYER_ENABLED=0", "-DBATTERY_ENABLED=0"], True),
]


def main():
    compiler = find_compiler()
    if compiler is None:
        print("no C++ compiler found; skipping", file=sys.stderr)
        return 0

    with tempfile.TemporaryDirectory() as tmp:
        unit = os.path.join(tmp, "pin_check_test.cpp")
        with open(unit, "w") as handle:
            handle.write('#include "pin_check.h"\n')

        failures = []
        for name, flags, must_compile in CASES:
            command = [
                compiler, "-fsyntax-only", "-std=gnu++17",
                # The soft advice in pin_check.h is #warning, which must not be
                # allowed to look like a failure -- these cases are about the
                # hard assertions.
                "-Wno-cpp", "-I", SRC,
            ] + flags + [unit]
            result = subprocess.run(command, capture_output=True, text=True)
            compiled = result.returncode == 0
            if compiled != must_compile:
                failures.append((name, flags, must_compile, result.stderr.strip()))
                print(f"FAIL  {name}")
            else:
                print(f"ok    {name}")

    if failures:
        print(f"\n{len(failures)} of {len(CASES)} cases behaved wrongly:\n")
        for name, flags, must_compile, err in failures:
            want = "compile" if must_compile else "be rejected"
            print(f"  {name} ({' '.join(flags) or 'no overrides'}) should {want}")
            for line in err.splitlines():
                if "static assertion" in line or "error:" in line:
                    print(f"      {line.strip()}")
        return 1

    print(f"\nall {len(CASES)} pin-map cases behaved as specified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
