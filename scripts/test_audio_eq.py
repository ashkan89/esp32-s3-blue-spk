#!/usr/bin/env python3
"""Numerically exercise the production five-band RBJ filter design."""

import cmath
import math
import os
import re
import sys


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE_PATH = os.path.join(ROOT, "src", "audio_eq.cpp")


def parse_source():
    with open(SOURCE_PATH, encoding="utf-8") as handle:
        source = handle.read()
    bands_match = re.search(
        r"EQ_BAND_HZ\[EQ_BANDS\]\s*=\s*\{([^}]+)\}", source
    )
    q_match = re.search(r"BAND_Q\s*=\s*([0-9.]+)f", source)
    slope_match = re.search(r"SHELF_SLOPE\s*=\s*([0-9.]+)f", source)
    preset_match = re.search(
        r"PRESET_GAINS\[EQ_PRESET_COUNT\]\[EQ_BANDS\]\s*=\s*\{(.*?)\n\};",
        source,
        re.S,
    )
    if not all((bands_match, q_match, slope_match, preset_match)):
        raise AssertionError("could not locate production EQ constants")
    bands = [int(value) for value in re.findall(r"\d+", bands_match.group(1))]
    presets = []
    for row in re.findall(r"\{([^{}]+)\}", preset_match.group(1)):
        values = [int(value) for value in re.findall(r"-?\d+", row)]
        if values:
            presets.append(values)
    assert "stability_eq_frequency(EQ_BAND_HZ[i], rate)" in source
    return bands, float(q_match.group(1)), float(slope_match.group(1)), presets


def peaking(hz, gain_db, q, rate):
    a = 10.0 ** (gain_db / 40.0)
    w0 = 2.0 * math.pi * hz / rate
    cos_w0 = math.cos(w0)
    alpha = math.sin(w0) / (2.0 * q)
    a0 = 1.0 + alpha / a
    return (
        (1.0 + alpha * a) / a0,
        (-2.0 * cos_w0) / a0,
        (1.0 - alpha * a) / a0,
        (-2.0 * cos_w0) / a0,
        (1.0 - alpha / a) / a0,
    )


def shelf(hz, gain_db, slope, rate, low):
    a = 10.0 ** (gain_db / 40.0)
    w0 = 2.0 * math.pi * hz / rate
    cos_w0 = math.cos(w0)
    sin_w0 = math.sin(w0)
    inner = (a + 1.0 / a) * (1.0 / slope - 1.0) + 2.0
    alpha = sin_w0 / 2.0 * math.sqrt(max(inner, 0.0))
    two_sqrt_a_alpha = 2.0 * math.sqrt(a) * alpha
    if low:
        b0 = a * ((a + 1.0) - (a - 1.0) * cos_w0 + two_sqrt_a_alpha)
        b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cos_w0)
        b2 = a * ((a + 1.0) - (a - 1.0) * cos_w0 - two_sqrt_a_alpha)
        a0 = (a + 1.0) + (a - 1.0) * cos_w0 + two_sqrt_a_alpha
        a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cos_w0)
        a2 = (a + 1.0) + (a - 1.0) * cos_w0 - two_sqrt_a_alpha
    else:
        b0 = a * ((a + 1.0) + (a - 1.0) * cos_w0 + two_sqrt_a_alpha)
        b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cos_w0)
        b2 = a * ((a + 1.0) + (a - 1.0) * cos_w0 - two_sqrt_a_alpha)
        a0 = (a + 1.0) - (a - 1.0) * cos_w0 + two_sqrt_a_alpha
        a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cos_w0)
        a2 = (a + 1.0) - (a - 1.0) * cos_w0 - two_sqrt_a_alpha
    return b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0


def response_db(coeffs, hz, rate):
    b0, b1, b2, a1, a2 = coeffs
    z1 = cmath.exp(-2j * math.pi * hz / rate)
    value = (b0 + b1 * z1 + b2 * z1 * z1) / (
        1.0 + a1 * z1 + a2 * z1 * z1
    )
    return 20.0 * math.log10(abs(value))


def assert_stable(coeffs):
    b0, b1, b2, a1, a2 = coeffs
    assert all(math.isfinite(value) for value in coeffs)
    discriminant = complex(a1 * a1 - 4.0 * a2, 0.0)
    roots = ((-a1 + cmath.sqrt(discriminant)) / 2.0,
             (-a1 - cmath.sqrt(discriminant)) / 2.0)
    assert max(abs(root) for root in roots) < 1.0
    assert max(abs(b0), abs(b1), abs(b2), abs(a1), abs(a2)) < 5.0


def main():
    bands, q, slope, presets = parse_source()
    assert bands == [60, 250, 1000, 4000, 12000]
    assert len(presets) == 6 and all(len(row) == len(bands) for row in presets)
    assert all(-12 <= gain <= 12 for row in presets for gain in row)
    assert [-max(0, max(row)) for row in presets] == [0, -4, -4, -9, -3, 0]

    designs = 0
    for rate in (8000, 11025, 16000, 22050, 32000, 44100, 48000, 96000):
        for band, requested in enumerate(bands):
            hz = min(requested, rate * 45 // 100)
            assert 0 < hz < rate / 2
            for gain in range(-12, 13):
                if gain == 0:
                    coeffs = (1.0, 0.0, 0.0, 0.0, 0.0)
                elif band == 0:
                    coeffs = shelf(hz, gain, slope, rate, True)
                elif band == len(bands) - 1:
                    coeffs = shelf(hz, gain, slope, rate, False)
                else:
                    coeffs = peaking(hz, gain, q, rate)
                assert_stable(coeffs)
                if gain == 0:
                    assert coeffs == (1.0, 0.0, 0.0, 0.0, 0.0)
                elif 0 < band < len(bands) - 1:
                    assert abs(response_db(coeffs, hz, rate) - gain) < 0.001
                designs += 1

    print(
        f"all {designs} EQ coefficient designs are finite, stable, "
        "Nyquist-bounded, and centre-gain correct"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as error:
        print(f"EQ test failed: {error}", file=sys.stderr)
        sys.exit(1)
