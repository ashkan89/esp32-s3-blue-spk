/*
 * soft_clock.h -- wall-clock time on a device that has no clock.
 *
 * A Bluetooth speaker has no RTC and, deliberately, no network: Wi-Fi and
 * Bluetooth share one 2.4 GHz radio, and running both while A2DP is streaming is
 * exactly the kind of thing that makes the audio stutter. So the time comes from
 * whichever of these is available, in order of preference:
 *
 *   1. DS3231 module on the same two I2C wires as the display  (-DUSE_DS3231=1)
 *      The only option that survives a power cut properly. ~1 EUR, keeps time
 *      for years on its coin cell, and it is the same SDA/SCL pair the OLED
 *      already uses -- two extra wires, no extra GPIO.
 *
 *   2. One NTP sync at boot, before the radio starts    (-DUSE_NTP=1 plus
 *      -DWIFI_SSID=\"..\" -DWIFI_PASS=\"..\")
 *      Wi-Fi is brought up, the time is fetched, and Wi-Fi is switched off again
 *      before a2dp_sink.start() is ever called, so the two radios never overlap.
 *      Costs about four seconds of boot time.
 *
 *   3. Typed in over the serial monitor:  time 2026-08-18 14:30:00
 *      Also  time 14:30  to set just the clock, and  date 2026-08-18.
 *
 *   4. The build timestamp, as a last resort, so a fresh flash shows roughly
 *      the right time instead of 1 Jan 1970 -- it drifts, and it is wrong by
 *      however long the firmware has been sitting on disk, but it never looks
 *      broken.
 *
 * Whatever the source, the current time is written to NVS every ten minutes.
 * After a reset the ESP32 keeps counting in its own RTC anyway; after a real
 * power cut, NVS gets you back to within ten minutes rather than back to 1970.
 */

#pragma once

#include <stdint.h>
#include <time.h>

/// Where the current time came from. Shown on the info screen, because "is the
/// clock actually right?" is otherwise unanswerable.
enum ClockSource : uint8_t {
  CLOCK_SRC_BUILD = 0,  ///< build timestamp -- approximate
  CLOCK_SRC_NVS,        ///< restored from flash after a power cut
  CLOCK_SRC_SERIAL,     ///< set by hand
  CLOCK_SRC_RTC,        ///< DS3231
  CLOCK_SRC_NTP,        ///< network, once, at boot
};

/// Seeds the clock. Call after Wire has been set up (the DS3231 path needs it)
/// and before a2dp_sink.start() (the NTP path needs the radio to itself).
void soft_clock_begin();

/// Local time now. Never fails: falls back to the build-time seed.
void soft_clock_now(struct tm *out);

/// True once the time came from something better than the build timestamp.
bool soft_clock_trusted();

ClockSource soft_clock_source();

/// One-line label for the source, for the info screen.
const char *soft_clock_source_name();

/// Sets local time from a broken-down time, and pushes it to the DS3231 and NVS
/// so it survives the next power cut.
void soft_clock_set(const struct tm &t, ClockSource source);

/// Housekeeping: the periodic NVS write. Call from loop(); it does nothing most
/// of the time.
void soft_clock_tick();

/// Handles the "time ..." and "date ..." serial commands. Returns false if the
/// line was not one of them, so the caller can try its own commands.
bool soft_clock_command(const char *line);
