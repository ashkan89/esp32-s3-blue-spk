#!/usr/bin/env python3
"""Compile and run the production stability-policy primitives on the host."""

import os
import glob
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "src")

UNIT = r'''
#include "stability_policy.h"
#include <stdint.h>

static_assert(stability_memory_next(MEMORY_NORMAL, 120000, 60000) == MEMORY_NORMAL);
static_assert(stability_memory_next(MEMORY_NORMAL, 89999, 60000) == MEMORY_CONSTRAINED);
static_assert(stability_memory_next(MEMORY_NORMAL, 120000, 44999) == MEMORY_CONSTRAINED);
static_assert(stability_memory_next(MEMORY_NORMAL, 69999, 60000) == MEMORY_CRITICAL);
static_assert(stability_memory_next(MEMORY_CRITICAL, 81000, 50000) == MEMORY_CRITICAL);
static_assert(stability_memory_next(MEMORY_CRITICAL, 82000, 36000) == MEMORY_CONSTRAINED);
static_assert(stability_memory_next(MEMORY_CONSTRAINED, 104999, 60000) == MEMORY_CONSTRAINED);
static_assert(stability_memory_next(MEMORY_CONSTRAINED, 105000, 52000) == MEMORY_NORMAL);
static_assert(stability_optional_interval(16, MEMORY_NORMAL) == 16);
static_assert(stability_optional_interval(16, MEMORY_CONSTRAINED) == 32);
static_assert(stability_optional_interval(16, MEMORY_CRITICAL) == 64);
static_assert(stability_optional_interval(20000, MEMORY_CRITICAL) == UINT16_MAX);
static_assert(stability_json_limit(MEMORY_NORMAL) == 12288);
static_assert(stability_json_limit(MEMORY_CONSTRAINED) == 8192);
static_assert(stability_json_limit(MEMORY_CRITICAL) == 4096);

static_assert(!stability_elapsed(0x00000005U, 0xFFFFFFF0U, 22U));
static_assert(stability_elapsed(0x00000006U, 0xFFFFFFF0U, 22U));
static_assert(stability_remaining(0xFFFFFFF0U, 0x00000010U) == 32U);
static_assert(stability_remaining(100U, 100U) == 0U);
static_assert(stability_remaining(101U, 100U) == 0U);

// One-shot: a future time is today; the same minute has passed and is tomorrow.
static_assert(stability_alarm_seconds_until(true, 0, 7, 0, 1, 6, 59, 30) == 30U);
static_assert(stability_alarm_seconds_until(true, 0, 7, 0, 1, 7, 0, 0) == 86400U);
// Weekly: Monday 07:00 from Sunday, and the just-missed Monday is next week.
static_assert(stability_alarm_seconds_until(true, 1U << 1, 7, 0, 0, 8, 0, 0) == 82800U);
static_assert(stability_alarm_seconds_until(true, 1U << 1, 7, 0, 1, 7, 0, 1) == 604799U);
static_assert(stability_alarm_seconds_until(false, 0x7f, 7, 0, 1, 6, 0, 0) == 0U);

static_assert(stability_reconnect_next(2000) == 4000);
static_assert(stability_reconnect_next(32000) == 60000);
static_assert(stability_reconnect_next(60000) == 60000);
static_assert(stability_reconnect_jitter(60000, 0) == 45000);
static_assert(stability_reconnect_jitter(60000, 30000) == 75000);

static_assert(stability_pcm_volume(32767, 127) == 32767);
static_assert(stability_pcm_volume(-32768, 127) == -32768);
static_assert(stability_pcm_volume(12345, 0) == 0);
static_assert(stability_pcm_volume(20000, 64) == 10000);
static_assert(stability_pcm_saturate(40000) == 32767);
static_assert(stability_pcm_saturate(-40000) == -32768);
static_assert(stability_pcm_saturate(1234) == 1234);
static_assert(stability_eq_frequency(12000, 48000) == 12000);
static_assert(stability_eq_frequency(12000, 16000) == 7200);
static_assert(stability_eq_frequency(4000, 8000) == 3600);

static_assert(stability_ring_advance(7, 5, 10) == 2);
static_assert(stability_ring_advance(9, 1, 10) == 0);
static_assert(stability_ring_advance(4, 99, 0) == 0);
static_assert(stability_ring_oldest(3, 3, 10) == 0);
static_assert(stability_ring_oldest(2, 10, 10) == 2);
static_assert(stability_ring_oldest(2, 99, 10) == 2);

static_assert(stability_mode_next(0, 3) == 1);
static_assert(stability_mode_next(1, 3) == 2);
static_assert(stability_mode_next(2, 3) == 0);
static_assert(stability_mode_next(2, 0) == 0);
constexpr StabilityModeBootDecision mode_wifi =
    stability_mode_boot(0, 3, 0, 2, 2);
static_assert(mode_wifi.mode == 0 && mode_wifi.nextStrikes == 0 &&
              !mode_wifi.strikePending && !mode_wifi.fellBack);
constexpr StabilityModeBootDecision mode_first =
    stability_mode_boot(1, 3, 0, 0, 2);
static_assert(mode_first.mode == 1 && mode_first.nextStrikes == 1 &&
              mode_first.strikePending && !mode_first.fellBack);
constexpr StabilityModeBootDecision mode_second =
    stability_mode_boot(2, 3, 0, 1, 2);
static_assert(mode_second.mode == 2 && mode_second.nextStrikes == 2 &&
              mode_second.strikePending && !mode_second.fellBack);
constexpr StabilityModeBootDecision mode_fallback =
    stability_mode_boot(2, 3, 0, 2, 2);
static_assert(mode_fallback.mode == 0 && mode_fallback.nextStrikes == 0 &&
              !mode_fallback.strikePending && mode_fallback.fellBack);
constexpr StabilityModeBootDecision mode_corrupt =
    stability_mode_boot(99, 3, 0, 1, 2);
static_assert(mode_corrupt.mode == 0 && mode_corrupt.nextStrikes == 0 &&
              !mode_corrupt.strikePending && !mode_corrupt.fellBack);

static_assert(stability_url_http("http://station.example/live"));
static_assert(stability_url_http("HTTPS://station.example/live"));
static_assert(!stability_url_http("ftp://station.example/live"));
static_assert(!stability_url_http("station.example/live"));
static_assert(!stability_url_http(nullptr));
static_assert(stability_url_https("Https://station.example/live"));
static_assert(!stability_url_https("http://station.example/live"));
static_assert(stability_utf8_sequence_length("A") == 1);
static_assert(stability_utf8_sequence_length("\xC3\xA9") == 2);
static_assert(stability_utf8_sequence_length("\xE2\x82\xAC") == 3);
static_assert(stability_utf8_sequence_length("\xF0\x9F\x98\x80") == 4);
static_assert(stability_utf8_sequence_length("\xF0") == 1);
static_assert(stability_utf8_sequence_length("\xE2\x82") == 1);
static_assert(stability_utf8_sequence_length("\xC0\x80") == 1);
static_assert(stability_utf8_sequence_length("\xED\xA0\x80") == 1);
static_assert(stability_utf8_sequence_length("\xF4\x90\x80\x80") == 1);
static_assert(stability_utf8_sequence_length(nullptr) == 0);
static_assert(stability_bounded_copy_length(200, 64) == 63);
static_assert(stability_bounded_copy_length(12, 64) == 12);
static_assert(stability_bounded_copy_length(12, 0) == 0);
'''


def find_compiler():
    candidates = []
    for root in ("C:/p/packages", os.path.expanduser("~/.platformio/packages")):
        candidates += glob.glob(os.path.join(
            root, "toolchain-xtensa*", "bin", "xtensa-esp32-elf-g++*"))
        candidates += glob.glob(os.path.join(
            root, "toolchain-xtensa*", "bin", "xtensa-esp-elf-g++*"))
    return candidates[0] if candidates else None


def main():
    compiler = find_compiler()
    if not compiler:
        print("no C++ compiler found", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory() as tmp:
        source = os.path.join(tmp, "stability_policy_test.cpp")
        with open(source, "w", encoding="utf-8") as handle:
            handle.write(UNIT)
        build = subprocess.run(
            [compiler, "-fsyntax-only", "-std=gnu++17", "-Wall", "-Wextra",
             "-Werror", "-I", SRC, source], capture_output=True, text=True
        )
        if build.returncode:
            print(build.stderr, file=sys.stderr)
            return build.returncode
    print("all compile-time memory, timer, history, mode, URL, metadata, backoff, and PCM tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
