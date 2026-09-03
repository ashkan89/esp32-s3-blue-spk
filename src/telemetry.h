/*
 * telemetry.h -- a few hours of history, so the dashboard can draw a line
 * instead of a number.
 *
 * Every reading this firmware has -- battery voltage, charge, chip temperature,
 * free heap, Wi-Fi signal -- was already available as "what is it right now",
 * and for most of them that is the useless half of the question. A battery at
 * 3.8 V is fine or nearly flat depending entirely on which way it is moving. A
 * heap at 90 kB is healthy unless it was 140 kB an hour ago, in which case
 * something is leaking and the speaker will fall over tonight. Free heap in
 * particular is the number that turns a mysterious reboot into a diagnosis, and
 * it is only ever diagnostic as a slope.
 *
 * What it costs. One sample every thirty seconds into a fixed ring of 240,
 * which is two hours and about 2.4 kB of RAM that is allocated once at boot and
 * never grows. Taking a sample is four reads of values other modules are
 * already keeping, so there is nothing to switch off in a release build. The
 * ring is in RAM and not in flash on purpose: this is for looking at a running
 * speaker, and writing a sample to NVS every thirty seconds would wear the
 * chip out inside a year for a history nobody reads after a reboot.
 *
 * On the temperature reading, which deserves a warning. This is the ESP32's
 * internal sensor, which measures the *die*, not the room -- on a chip that has
 * just transmitted, it reads ten to twenty degrees above ambient, and on the
 * original ESP32 silicon it is an undocumented ROM function with a coarse step
 * and a per-chip offset nobody calibrated. It is genuinely useful for what it
 * is good at: spotting a speaker that is cooking in the sun, or a charge
 * current that is heating the board, both of which show up as a trend. It is
 * not a thermometer, the dashboard says so next to the graph, and no decision
 * in this firmware is made from it.
 */

#pragma once

#include <stdint.h>

/// How many samples the ring holds.
static const uint16_t TELEMETRY_SAMPLES = 240;

/// Seconds between samples. Thirty gives two hours of history, which is the
/// span over which a battery discharge curve and a heap leak both become
/// visible; a finer interval buys detail nobody is looking for and a coarser
/// one loses the shape of a charge cycle.
static const uint16_t TELEMETRY_INTERVAL_S = 30;

/*
 * One sample.
 *
 * Ten bytes, and every field is packed to the smallest type that holds it
 * without losing anything the graph could show: millivolts because that is the
 * gauge's own resolution, tenths of a degree because the sensor's step is
 * coarser than that anyway, and kilobytes of heap because a graph pixel is
 * worth about 400 bytes.
 *
 * `valid` is not padding. A ring that has only been running ten minutes is
 * mostly empty, and a dashboard that plots the empty part as zero volts draws a
 * cliff that is not there.
 */
struct TelemetrySample {
  uint16_t millivolts;   ///< battery terminal voltage, or 0 with no gauge
  uint8_t percent;       ///< battery charge, 0..100
  int16_t deciCelsius;   ///< die temperature in tenths, see the warning above
  uint16_t heapKb;       ///< free heap
  int8_t rssi;           ///< Wi-Fi signal in dBm, or 0 when not associated
  uint8_t flags;         ///< a bitwise or of TelemetryFlag
  bool valid;
};

enum TelemetryFlag : uint8_t {
  TELEMETRY_CHARGING = 1 << 0,
  TELEMETRY_AUDIO = 1 << 1,      ///< something was playing when this was taken
  TELEMETRY_SAVING = 1 << 2,     ///< power saving was engaged
};

/// Allocates the ring and takes the first sample. Returns false if the memory
/// was not there, in which case everything below is a no-op and the dashboard
/// simply shows no graphs.
bool telemetry_begin();

/// Takes a sample when one is due. Arduino loop task only; it does nothing on
/// all but one pass in several thousand.
void telemetry_loop();

/*
 * The history, oldest first, read one sample at a time and without a copy.
 *
 * Handing out 240 samples at once would need somewhere to put them, and the one
 * caller -- the web handler that serialises them -- had that somewhere as a
 * 2.9 kB static buffer, resident for the life of the firmware so that one
 * endpoint could format a document. Both the ring and the handler run on the
 * Arduino loop task, so there is nothing to copy for: `index` counts from the
 * oldest sample, and false means there is no sample there.
 */
uint16_t telemetry_count();
bool telemetry_at(uint16_t index, TelemetrySample *out);

/// The most recent sample, taken now rather than read from the ring, so a
/// dashboard that polls faster than the sample interval still shows live
/// numbers.
void telemetry_now(TelemetrySample *out);

/// Seconds of history the ring currently holds.
uint32_t telemetry_span_seconds();

/// How long the speaker has been up, in seconds. Kept here rather than left to
/// millis() at the call site because it survives the 49-day wrap, which a
/// device that is left plugged in will eventually reach.
uint32_t telemetry_uptime_seconds();

/*
 * Total runtime across every boot, in seconds, and how many boots there have
 * been.
 *
 * Uptime answers "how long since the last restart", which is the question you
 * ask when something has gone wrong. This answers "how much has this thing
 * actually been used", which is the one the graphs are for -- and it is the
 * only number here that has to survive a power cut, so it is the only one
 * written to flash. Once every ten minutes, which is the same cadence the clock
 * already persists at and for the same reason.
 */
uint32_t telemetry_runtime_seconds();
uint32_t telemetry_boot_count();

/// Serial console: "graph". Returns false if the line was something else.
bool telemetry_command(const char *line);
