/*
 * soft_clock.h -- wall-clock time on a device that has no clock.
 *
 * A Bluetooth speaker has no hardware RTC. The management dashboard can set
 * the clock from a browser, while the sources below keep it useful even when
 * the speaker has no network. The time comes from whichever is available:
 *
 *   1. DS3231 module on the same two I2C wires as the display. On by default
 *      (-DUSE_DS3231=1 in platformio.ini); set it to 0 for a board with no RTC
 *      fitted, though leaving it on costs only a probe of 0x68 at boot.
 *
 *      The only option that survives a power cut properly. ~1 EUR, keeps time
 *      for years on its coin cell, and it is the same SDA/SCL pair the OLED
 *      already uses -- two extra wires, no extra GPIO:
 *
 *        VCC -> 3.3V      GND -> GND
 *        SDA -> GPIO21    SCL -> GPIO22     (with the OLED, in parallel)
 *
 *      Both modules carry their own pull-ups, which in parallel is fine on
 *      short leads. The RTC answers on 0x68, the panel on 0x3C, so nothing
 *      collides. Leave SQW and 32K unconnected -- neither is used.
 *
 *      A word on the coin cell: the common ZS-042 board wires a charging
 *      circuit intended for a rechargeable LIR2032. Fitted with a CR2032, as
 *      most of them ship, that circuit trickles current into a non-rechargeable
 *      cell. Either fit a LIR2032 or lift the charge resistor -- and either way
 *      the timekeeping is the same.
 *
 *   2. SNTP, automatically, whenever the speaker is on Wi-Fi in management
 *      mode. Nothing to configure: management_loop() calls
 *      soft_clock_network_begin() the moment the station has an address, the
 *      answer is adopted from soft_clock_tick(), and the IDF's SNTP client
 *      keeps re-syncing on its own for as long as the network is up.
 *
 *      Bluetooth mode has no Wi-Fi at all -- one antenna, one radio -- so the
 *      clock there runs from whatever the last sync left in NVS and the RTC.
 *
 *      The legacy boot-time sync (-DUSE_NTP=1 with -DWIFI_SSID/-DWIFI_PASS)
 *      still exists for builds with MANAGEMENT_ENABLED=0.
 *
 *   3. Set from the web dashboard, or typed in over the serial monitor:
 *      time 2026-08-18 14:30:00. Also time 14:30 and date 2026-08-18.
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
  CLOCK_SRC_NTP,        ///< network (SNTP)
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

// ------------------------------------------------------------- time zone ----
/*
 * Everything above deals in *local* time, and the system clock holds UTC. The
 * bridge between the two is a fixed offset -- there is no DST rule engine on
 * this device, and inventing one that goes stale is worse than an offset the
 * owner sets once.
 *
 * The offset starts at CLOCK_TZ_OFFSET_MIN and is then learned from the
 * browser: the dashboard's "Sync browser time" button sends its UTC offset
 * along with the time, so the network sync below lands on the same wall clock
 * the owner just confirmed rather than on UTC.
 */

/// Minutes east of UTC. Negative is west: -300 is US Eastern, 330 is IST.
int32_t soft_clock_utc_offset_min();

/// Persists a new offset and re-derives local time from it immediately.
/// Accepts -840..840 (UTC-14 to UTC+14); anything else is ignored.
void soft_clock_set_utc_offset_min(int32_t minutes);

// ------------------------------------------------------------ presentation --
/*
 * How the time is written down, rather than what it is. Kept here, next to the
 * clock itself, because the OLED and the dashboard both draw it and neither is
 * a sensible owner of the preference. Seeded from CLOCK_24H and then whatever
 * the dashboard's Clock card last stored.
 */

/// True for 24-hour time, false for 12-hour with AM/PM.
bool soft_clock_use_24h();
void soft_clock_set_use_24h(bool on);

// --------------------------------------------------------- network sync -----

/// Whether SNTP may correct the clock while the speaker is on Wi-Fi. On by
/// default; turning it off is for an owner who has set the time by hand and
/// wants it left alone. soft_clock_network_begin() honours this, so the caller
/// does not have to.
bool soft_clock_auto_sync();
void soft_clock_set_auto_sync(bool on);

/// Starts (or restarts) SNTP. Call once the station has an address; it is
/// cheap and idempotent, so calling it again after a reconnect is fine. Does
/// nothing while soft_clock_auto_sync() is false.
void soft_clock_network_begin();

/// Stops SNTP. Call when the network goes away, so the client is not polling
/// an interface that cannot answer.
void soft_clock_network_end();

/// True once an SNTP answer has been adopted this boot.
bool soft_clock_network_synced();
