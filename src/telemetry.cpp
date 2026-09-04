#include "app_config.h"
#include "telemetry.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <string.h>

#include "audio_probe.h"
#include "battery.h"
#include "power.h"
#include "stability_policy.h"

namespace {

TelemetrySample *ring;
uint16_t ringHead;   ///< where the next sample goes
uint16_t ringCount;  ///< how many are valid, up to TELEMETRY_SAMPLES

uint32_t lastSampleMs;

/*
 * Uptime that survives the millis() wrap.
 *
 * millis() rolls over after 49.7 days. A speaker that lives on a shelf plugged
 * into the wall reaches that, and when it does an uptime computed as millis()
 * / 1000 goes back to zero and every duration derived from it goes briefly
 * negative. Accumulating the difference each pass costs one subtraction and is
 * correct across any number of wraps.
 */
uint32_t uptimeSeconds;
uint32_t lastUptimeMs;

Preferences prefs;
bool prefsOk;
uint32_t runtimeBase;   ///< runtime accumulated before this boot
uint32_t bootCount;
uint32_t lastRuntimeWriteMs;
const uint32_t RUNTIME_WRITE_MS = 600000;  // ten minutes, as the clock does

void serviceUptime() {
  const uint32_t now = millis();
  const uint32_t elapsed = now - lastUptimeMs;  // correct across the wrap
  if (elapsed >= 1000) {
    uptimeSeconds += elapsed / 1000;
    lastUptimeMs += (elapsed / 1000) * 1000;
  }
}

void persistRuntime() {
  if (!prefsOk) return;
  prefs.putULong("runtime", runtimeBase + uptimeSeconds);
}

/*
 * Reads the die temperature.
 *
 * temperatureRead() is available on every ESP32 the Arduino core supports, but
 * on the original silicon it is the undocumented ROM routine, which returns a
 * value in Fahrenheit on a coarse ladder and can answer 53.33 C for minutes on
 * end because that is the step it landed on. Two things are done about that:
 * the reading is passed through unchanged (inventing precision would be worse),
 * and anything outside a range the part could physically be in is reported as
 * "no reading" rather than as a number.
 */
int16_t readTemperature() {
  const float celsius = temperatureRead();
  if (isnan(celsius) || celsius < -40.0f || celsius > 125.0f) return INT16_MIN;
  return (int16_t)(celsius * 10.0f + (celsius >= 0 ? 0.5f : -0.5f));
}

void fill(TelemetrySample *out) {
  memset(out, 0, sizeof(*out));

  BatteryStatus bat;
  battery_snapshot(&bat);
  if (bat.present) {
    out->millivolts = (uint16_t)(bat.volts * 1000.0f + 0.5f);
    out->percent = bat.percent;
    if (bat.charging) out->flags |= TELEMETRY_CHARGING;
  }

  out->deciCelsius = readTemperature();
  out->heapKb = (uint16_t)(ESP.getFreeHeap() / 1024);
  if (WiFi.status() == WL_CONNECTED) {
    const int rssi = WiFi.RSSI();
    out->rssi = (int8_t)(rssi < -128 ? -128 : (rssi > 0 ? 0 : rssi));
  }
  // "Something was playing" is what makes a battery graph readable: a curve
  // that suddenly steepens means nothing until you can see that the music
  // started there.
  if (audio_probe_last_active() &&
      (uint32_t)(millis() - audio_probe_last_active()) < 5000) {
    out->flags |= TELEMETRY_AUDIO;
  }
  if (power_saving()) out->flags |= TELEMETRY_SAVING;
  out->valid = true;
}

}  // namespace

bool telemetry_begin() {
  lastUptimeMs = millis();
  ring = (TelemetrySample *)calloc(TELEMETRY_SAMPLES, sizeof(TelemetrySample));
  if (!ring) {
    LOGLN("[telemetry] no room for the history ring; the dashboard "
                   "graphs will be empty this boot");
    return false;
  }

  prefsOk = prefs.begin("telemetry", false);
  if (prefsOk) {
    runtimeBase = prefs.getULong("runtime", 0);
    bootCount = prefs.getULong("boots", 0) + 1;
    prefs.putULong("boots", bootCount);
  }

  fill(&ring[0]);
  ringHead = 1;
  ringCount = 1;
  lastSampleMs = millis();
  return true;
}

void telemetry_loop() {
  serviceUptime();

  const uint32_t now = millis();
  if (prefsOk && now - lastRuntimeWriteMs >= RUNTIME_WRITE_MS) {
    lastRuntimeWriteMs = now;
    persistRuntime();
  }

  if (!ring) return;
  if (now - lastSampleMs < (uint32_t)TELEMETRY_INTERVAL_S * 1000u) return;
  lastSampleMs = now;

  fill(&ring[ringHead]);
  ringHead = (uint16_t)stability_ring_advance(ringHead, 1, TELEMETRY_SAMPLES);
  if (ringCount < TELEMETRY_SAMPLES) ringCount++;
}

uint16_t telemetry_count() { return ring ? ringCount : 0; }

bool telemetry_at(uint16_t index, TelemetrySample *out) {
  if (!out || !ring || index >= ringCount) return false;
  // The oldest sample sits `ringCount` places behind the head, wrapping.
  const uint16_t start = (uint16_t)stability_ring_oldest(
      ringHead, ringCount, TELEMETRY_SAMPLES);
  *out = ring[(start + index) % TELEMETRY_SAMPLES];
  return true;
}

void telemetry_now(TelemetrySample *out) {
  if (!out) return;
  fill(out);
}

uint32_t telemetry_span_seconds() {
  return (uint32_t)ringCount * TELEMETRY_INTERVAL_S;
}

uint32_t telemetry_uptime_seconds() {
  serviceUptime();
  return uptimeSeconds;
}

uint32_t telemetry_runtime_seconds() {
  return runtimeBase + telemetry_uptime_seconds();
}

uint32_t telemetry_boot_count() { return bootCount; }

bool telemetry_command(const char *line) {
  if (!line || strcmp(line, "graph") != 0) return false;

  TelemetrySample now;
  telemetry_now(&now);
  LOGF("[telemetry] uptime %lu h %lu m | total runtime %lu h over %lu boots\n",
                (unsigned long)(telemetry_uptime_seconds() / 3600),
                (unsigned long)((telemetry_uptime_seconds() % 3600) / 60),
                (unsigned long)(telemetry_runtime_seconds() / 3600),
                (unsigned long)telemetry_boot_count());
  LOGF("[telemetry] now: ");
  if (now.millivolts) LOGF("%u.%03u V %u%% ", now.millivolts / 1000,
                                    now.millivolts % 1000, (unsigned)now.percent);
  else LOGP("no battery ");
  if (now.deciCelsius != INT16_MIN) LOGF("| %.1f C ", now.deciCelsius / 10.0f);
  LOGF("| heap %u kB", (unsigned)now.heapKb);
  if (now.rssi) LOGF(" | rssi %d dBm", (int)now.rssi);
  LOGLN();

  if (!ring || !ringCount) {
    LOGLN("[telemetry] no history yet");
    return true;
  }

  /*
   * A sparkline, because forty numbers in a column tell you nothing and a shape
   * tells you everything. Each column is one sample; the scale is printed
   * underneath so the shape can be read as values.
   */
  static const char *const BLOCKS = " .:-=+*#%@";
  struct Series {
    const char *name;
    long lo, hi;
    bool any;
  };
  Series series[3] = {{"battery mV", 0, 0, false},
                      {"temp 0.1C", 0, 0, false},
                      {"heap kB", 0, 0, false}};

  const uint16_t width = ringCount < 60 ? ringCount : 60;
  const uint16_t start = (uint16_t)((ringHead + TELEMETRY_SAMPLES - width) % TELEMETRY_SAMPLES);

  for (uint8_t s = 0; s < 3; s++) {
    for (uint16_t i = 0; i < width; i++) {
      const TelemetrySample &sample = ring[(start + i) % TELEMETRY_SAMPLES];
      if (!sample.valid) continue;
      long value;
      if (s == 0) {
        if (!sample.millivolts) continue;
        value = sample.millivolts;
      } else if (s == 1) {
        if (sample.deciCelsius == INT16_MIN) continue;
        value = sample.deciCelsius;
      } else {
        value = sample.heapKb;
      }
      if (!series[s].any) {
        series[s].lo = series[s].hi = value;
        series[s].any = true;
      } else {
        if (value < series[s].lo) series[s].lo = value;
        if (value > series[s].hi) series[s].hi = value;
      }
    }
  }

  for (uint8_t s = 0; s < 3; s++) {
    if (!series[s].any) continue;
    LOGF("  %-11s ", series[s].name);
    const long span = series[s].hi - series[s].lo;
    for (uint16_t i = 0; i < width; i++) {
      const TelemetrySample &sample = ring[(start + i) % TELEMETRY_SAMPLES];
      long value;
      bool have = sample.valid;
      if (s == 0) {
        have = have && sample.millivolts;
        value = sample.millivolts;
      } else if (s == 1) {
        have = have && sample.deciCelsius != INT16_MIN;
        value = sample.deciCelsius;
      } else {
        value = sample.heapKb;
      }
      if (!have) {
        LOGP(' ');
        continue;
      }
      const int level = span > 0 ? (int)((value - series[s].lo) * 9 / span) : 4;
      LOGP(BLOCKS[level + 1 > 9 ? 9 : level + 1]);
    }
    LOGF("  %ld..%ld\n", series[s].lo, series[s].hi);
  }
  LOGF("  %u samples, %lu s apart, oldest on the left\n",
                (unsigned)width, (unsigned long)TELEMETRY_INTERVAL_S);
  return true;
}
