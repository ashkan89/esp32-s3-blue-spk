#include "battery.h"

#if BATTERY_ENABLED

namespace {

BatteryStatus status;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

bool configured;
float smoothed;       // the EMA of the cell voltage, 0 before the first sample
uint32_t nextSample;

// Live configuration, defaults from hw_config.h until management overrides it.
bool enabled = true;
float divider = BATTERY_DIVIDER_DEFAULT;
float calibration = BATTERY_CALIBRATION_DEFAULT;
float fullVolts = BATTERY_FULL_V_DEFAULT;
float emptyVolts = BATTERY_EMPTY_V_DEFAULT;
uint8_t cells = 1;
uint8_t lowPct = BATTERY_LOW_PCT_DEFAULT;
uint8_t critPct = BATTERY_CRITICAL_PCT_DEFAULT;

/*
 * The discharge curve, per cell.
 *
 * A linear map from empty to full is the obvious thing and it is wrong in the
 * way that matters: a Li-ion cell spends most of its charge between 3.9 V and
 * 3.6 V, so a straight line shows 50% for an hour and then falls off a cliff.
 * These are the knees of a moderately loaded 18650 discharge, and between them
 * the value is interpolated linearly -- fourteen segments is more than enough
 * resolution for an indicator, and each one is a number you can check against a
 * datasheet curve rather than a polynomial nobody can audit.
 *
 * The table is fixed. The configurable end points are applied *afterwards*, by
 * rescaling: the curve is evaluated at the cell voltage, at `full` and at
 * `empty`, and the result is where the first sits between the other two. That
 * matters for the case it exists for -- a pack deliberately charged to 4.10 V
 * for longevity should read 100% when it is as full as it gets, and 99% just
 * below, rather than jumping from 100% to the raw curve's 78%.
 */
struct CurvePoint {
  float volts;
  uint16_t milli;  // per mille, so the rescale below has room to divide
};
constexpr CurvePoint CURVE[] = {
    {4.15f, 1000}, {4.05f, 920}, {3.97f, 850}, {3.91f, 770}, {3.85f, 680},
    {3.80f, 590},  {3.75f, 500}, {3.71f, 410}, {3.68f, 330}, {3.64f, 250},
    {3.59f, 170},  {3.52f, 100}, {3.45f, 50},  {3.35f, 10},
};
constexpr size_t CURVE_LEN = sizeof(CURVE) / sizeof(CURVE[0]);

/// Where the curve reaches zero. Below this a Li-ion cell is not nearly empty,
/// it is empty, and the knee spacing has nothing left to say about it. Only
/// reachable when `empty` is configured lower than the usual 3.30.
constexpr float CURVE_FLOOR_V = 3.00f;

/// The raw curve, in per mille, monotonically increasing in `v`.
uint16_t curveMilli(float v) {
  if (v >= CURVE[0].volts) return 1000;
  for (size_t i = 1; i < CURVE_LEN; i++) {
    if (v >= CURVE[i].volts) {
      const float span = CURVE[i - 1].volts - CURVE[i].volts;
      const float part = v - CURVE[i].volts;
      const int32_t lo = CURVE[i].milli, hi = CURVE[i - 1].milli;
      return (uint16_t)(lo + (hi - lo) * (span > 0 ? part / span : 0.0f) + 0.5f);
    }
  }
  if (v <= CURVE_FLOOR_V) return 0;
  const float last = CURVE[CURVE_LEN - 1].volts;
  const float part = (v - CURVE_FLOOR_V) / (last - CURVE_FLOOR_V);
  return (uint16_t)(CURVE[CURVE_LEN - 1].milli * part + 0.5f);
}

/// `cellVolts`, `fullVolts` and `emptyVolts` are all per cell.
uint8_t percentFor(float cellVolts) {
  if (cellVolts >= fullVolts) return 100;
  if (cellVolts <= emptyVolts) return 0;

  const int32_t here = curveMilli(cellVolts);
  const int32_t top = curveMilli(fullVolts);
  const int32_t bottom = curveMilli(emptyVolts);
  if (top <= bottom) {
    // The end points are close enough together that the curve cannot tell them
    // apart -- a pack configured with a 50 mV window, say. Nothing useful is
    // left to interpolate, so fall back to a straight line between them, which
    // at that spacing is the same answer to within a percent anyway.
    const float span = fullVolts - emptyVolts;
    return (uint8_t)((cellVolts - emptyVolts) / span * 100.0f + 0.5f);
  }
  const int32_t range = top - bottom;
  const int32_t out = ((here - bottom) * 100 + range / 2) / range;
  return (uint8_t)(out < 0 ? 0 : out > 100 ? 100 : out);
}

/// Median of BATTERY_OVERSAMPLE conversions. Insertion sort on nine elements is
/// faster than anything cleverer and needs no allocation.
uint16_t readMillivolts() {
#if PIN_BATTERY_SENSE >= 0
  /*
   * One throw-away conversion first, and it is not superstition.
   *
   * The divider is 100k over 100k, so the ADC looks into a 50 kOhm Thevenin
   * source. The ESP32's SAR front end is a switched sample-and-hold: it
   * connects a small capacitor to the pin for a fixed acquisition window, and
   * the charge that capacitor needs has to come through those 50 kOhm. Espressif
   * specify the recommended source impedance as an order of magnitude lower
   * than that, and the symptom of exceeding it is precisely this -- the first
   * conversion after the input multiplexer has been elsewhere reads low,
   * because the cap started at whatever the previously selected channel left on
   * it and did not finish charging. Back-to-back conversions on the same
   * channel are fine, because the cap is already close.
   *
   * So the first one is discarded and the median is taken over the rest, which
   * costs about 30 microseconds twice a second and removes a systematic
   * negative offset that no amount of calibration trim can distinguish from a
   * genuinely lower cell voltage. The hardware answer -- a 100 nF from the pin
   * to ground, which turns the 50 kOhm into a reservoir the S/H can draw from
   * instantly -- is documented in the README and is still worth fitting.
   */
  (void)analogReadMilliVolts(PIN_BATTERY_SENSE);

  uint16_t samples[BATTERY_OVERSAMPLE];
  for (uint8_t i = 0; i < BATTERY_OVERSAMPLE; i++) {
    // analogReadMilliVolts applies the chip's factory ADC calibration, which is
    // worth about 40 mV of accuracy over converting the raw count by hand.
    samples[i] = (uint16_t)analogReadMilliVolts(PIN_BATTERY_SENSE);
  }
  for (uint8_t i = 1; i < BATTERY_OVERSAMPLE; i++) {
    const uint16_t v = samples[i];
    int8_t j = (int8_t)i - 1;
    while (j >= 0 && samples[j] > v) {
      samples[j + 1] = samples[j];
      j--;
    }
    samples[j + 1] = v;
  }
  return samples[BATTERY_OVERSAMPLE / 2];
#else
  return 0;
#endif
}

bool readPin(int pin) {
  if (pin < 0) return false;
  const int level = digitalRead(pin);
  return BATTERY_STAT_ACTIVE_LOW ? level == LOW : level == HIGH;
}

void sample() {
  const uint16_t mv = readMillivolts();
  const float atPin = mv / 1000.0f;
  const float raw = atPin * divider * calibration;

  // An upper bound as well as a lower one. A sense pin with no divider on it
  // is a floating high-impedance input, and the readings it invents are as
  // likely to be above a charged pack as below an empty one; a number outside
  // the window a real cell can occupy is not a measurement.
  // Both bounds scale with the cell count, because `raw` is pack volts. With
  // only the ceiling scaled, a 2S pack reading 1.1 V per cell -- a broken
  // divider, or a pack that is genuinely gone -- passed as a real, critically
  // flat cell and flashed the low-battery pattern about it.
  const bool present = raw >= BATTERY_MIN_PLAUSIBLE_V * cells &&
                       raw <= BATTERY_MAX_PLAUSIBLE_V * cells;
  if (!present) {
    // No cell, or no divider. Do not smooth towards zero from a real reading --
    // that would show a battery draining away when what happened is a wire came
    // off. Drop straight to "not present" and say so.
    smoothed = 0.0f;
  } else if (smoothed <= 0.0f) {
    smoothed = raw;  // first reading: adopt it rather than fading up from zero
  } else {
    smoothed += (raw - smoothed) * BATTERY_SMOOTHING;
  }

  const float volts = present ? smoothed : 0.0f;
  const float perCell = present ? volts / cells : 0.0f;
  const uint8_t pct = present ? percentFor(perCell) : 0;

  const bool charging = readPin(PIN_BATTERY_CHARGING);
  const bool done = readPin(PIN_BATTERY_FULL);
  const bool havePins = PIN_BATTERY_CHARGING >= 0 || PIN_BATTERY_FULL >= 0;

  const bool low = present && pct <= lowPct && !charging;
  const bool critical = present && pct <= critPct && !charging;

  BatteryState state;
  if (!present) state = BAT_UNKNOWN;
  else if (charging) state = BAT_CHARGING;
  else if (done && pct >= 95) state = BAT_FULL;
  else if (critical) state = BAT_CRITICAL;
  else if (low) state = BAT_LOW;
  else if (havePins) state = BAT_DISCHARGING;
  // Without the charger pins there is no way to know whether the cell is
  // resting, being drained or being topped up. "Discharging" is still the
  // honest label for a battery-powered speaker that is switched on, so it is
  // used -- but haveChargePins is reported alongside so the dashboard can say
  // where the number came from.
  else state = BAT_DISCHARGING;

  portENTER_CRITICAL(&mux);
  status.enabled = enabled && PIN_BATTERY_SENSE >= 0;
  status.present = present;
  status.state = state;
  status.volts = volts;
  status.cellVolts = perCell;
  status.percent = pct;
  status.low = low;
  status.critical = critical;
  status.charging = charging;
  status.chargeDone = done;
  status.haveChargePins = havePins;
  status.millivoltsAtPin = mv;
  status.sampleAt = millis();
  status.samples++;
  status.divider = divider;
  status.calibration = calibration;
  status.fullVolts = fullVolts;
  status.emptyVolts = emptyVolts;
  status.cells = cells;
  status.lowPercent = lowPct;
  status.criticalPercent = critPct;
  portEXIT_CRITICAL(&mux);
}

}  // namespace

bool battery_begin() {
  status = BatteryStatus{};
  status.divider = divider;
  status.calibration = calibration;
  status.fullVolts = fullVolts;
  status.emptyVolts = emptyVolts;
  status.cells = cells;
  status.lowPercent = lowPct;
  status.criticalPercent = critPct;

  if (PIN_BATTERY_SENSE < 0) {
    Serial.println("[bat] no sense pin configured; the gauge cannot run");
    return false;
  }

#if PIN_BATTERY_SENSE >= 0
  // 11 dB attenuation gives a usable span of roughly 0.15-2.45 V at the pin,
  // which is where a 2:1 divider on a single cell lands. 12 bits is the ESP32's
  // maximum and costs nothing.
  analogSetPinAttenuation(PIN_BATTERY_SENSE, ADC_11db);
  analogReadResolution(12);
#endif
#if PIN_BATTERY_CHARGING >= 0
  pinMode(PIN_BATTERY_CHARGING, INPUT);
#endif
#if PIN_BATTERY_FULL >= 0
  pinMode(PIN_BATTERY_FULL, INPUT);
#endif

  /*
    * `configured` means "the ADC is set up", not "the gauge is switched on".
    *
    * They used to be the same flag, and that made the setting one-way: a gauge
    * that was off at boot returned from here before this line, so battery_loop()
    * declined to sample forever and turning it on from the dashboard changed
    * nothing until the next restart.
    */
  configured = true;
  if (!enabled) {
    Serial.printf("[bat] gauge is off in settings; sense pin %d configured and "
                  "ready. Enable it under Settings > Battery.\n",
                  PIN_BATTERY_SENSE);
    return false;
  }
  sample();
  nextSample = millis() + BATTERY_SAMPLE_MS;

  BatteryStatus s;
  battery_snapshot(&s);
  if (s.present) {
    Serial.printf("[bat] gauge on pin %d: %.2f V, %u%% (%s)%s\n",
                  PIN_BATTERY_SENSE, s.volts, (unsigned)s.percent,
                  battery_state_name(s.state),
                  s.haveChargePins ? "" : ", charger pins not wired");
  } else {
    Serial.printf("[bat] gauge on pin %d reads %u mV -- below %.2f V at the "
                  "cell, so no battery is assumed. Check the divider.\n",
                  PIN_BATTERY_SENSE, (unsigned)s.millivoltsAtPin,
                  BATTERY_MIN_PLAUSIBLE_V);
  }
  return s.present;
}

void battery_loop() {
  if (!configured || !enabled) return;
  const uint32_t now = millis();
  if ((int32_t)(now - nextSample) < 0) return;
  nextSample = now + BATTERY_SAMPLE_MS;
  sample();
}

void battery_snapshot(BatteryStatus *out) {
  if (out == nullptr) return;
  portENTER_CRITICAL(&mux);
  *out = status;
  portEXIT_CRITICAL(&mux);
}

bool battery_present() {
  portENTER_CRITICAL(&mux);
  const bool present = status.enabled && status.present;
  portEXIT_CRITICAL(&mux);
  return present;
}

uint8_t battery_percent() {
  portENTER_CRITICAL(&mux);
  const uint8_t pct = status.percent;
  portEXIT_CRITICAL(&mux);
  return pct;
}

BatteryState battery_state() {
  portENTER_CRITICAL(&mux);
  const BatteryState state = status.state;
  portEXIT_CRITICAL(&mux);
  return state;
}

bool battery_critical() {
  portENTER_CRITICAL(&mux);
  // `enabled` is part of the test, not a separate check somewhere else. This
  // one decides whether the status LED flashes, and a gauge the user has just
  // switched off must stop it -- the reading fields are cleared below when that
  // happens, but relying on that alone would leave the LED at the mercy of one
  // ordering in battery_configure().
  const bool crit = status.enabled && status.present && status.critical;
  portEXIT_CRITICAL(&mux);
  return crit;
}

void battery_configure(bool on, float div, float calib, float full, float empty,
                       uint8_t cellCount, uint8_t low, uint8_t crit) {
  enabled = on;
  // Clamped, not validated: these arrive from a web form and a gauge that
  // switches itself off because a field was blank is worse than one that pins
  // the value to something sane and keeps reporting.
  divider = (div >= 1.0f && div <= 20.0f) ? div : BATTERY_DIVIDER_DEFAULT;
  calibration = (calib >= 0.5f && calib <= 2.0f) ? calib
                                                 : BATTERY_CALIBRATION_DEFAULT;
  cells = (cellCount >= 1 && cellCount <= 4) ? cellCount : 1;
  // Per cell, both of them, because that is what the curve consumes. Scaling
  // one of the pair by `cells` and not the other is what used to make a 2S pack
  // read 100% at every voltage: the clamp treated them as pack volts while the
  // stored defaults were per-cell, so `full` came out at 2.10 V per cell and
  // every real cell was above it.
  fullVolts = (full >= 3.4f && full <= 4.5f) ? full : BATTERY_FULL_V_DEFAULT;
  emptyVolts = (empty >= 2.5f && empty < fullVolts - 0.05f)
                   ? empty
                   : (BATTERY_EMPTY_V_DEFAULT < fullVolts - 0.05f
                          ? BATTERY_EMPTY_V_DEFAULT
                          : fullVolts - 0.5f);
  critPct = crit <= 50 ? crit : BATTERY_CRITICAL_PCT_DEFAULT;
  lowPct = (low <= 90 && low > critPct) ? low : BATTERY_LOW_PCT_DEFAULT;
  if (lowPct <= critPct) lowPct = (uint8_t)min(90, critPct + 5);

  portENTER_CRITICAL(&mux);
  status.enabled = enabled && PIN_BATTERY_SENSE >= 0;
  if (!status.enabled) {
    // Leave nothing behind that reads as a live measurement. A stale `critical`
    // here is what kept the status LED flashing after the gauge was switched
    // off, and a stale voltage put "bat 3.4V critical" on the OLED stats line.
    status.present = false;
    status.state = BAT_UNKNOWN;
    status.percent = 0;
    status.volts = status.cellVolts = 0.0f;
    status.low = status.critical = false;
    status.charging = status.chargeDone = false;
  }
  status.divider = divider;
  status.calibration = calibration;
  status.fullVolts = fullVolts;
  status.emptyVolts = emptyVolts;
  status.cells = cells;
  status.lowPercent = lowPct;
  status.criticalPercent = critPct;
  portEXIT_CRITICAL(&mux);

  // The trim or the divider may have moved, so the smoothed value is now about
  // a different quantity. Start it again from the next reading rather than
  // walking it across.
  smoothed = 0.0f;
  /*
   * Ask battery_loop() for a reading; do not take one here.
   *
   * This function runs on the web server's task, and sample() writes `status`
   * and `smoothed` and drives the ADC -- all of which belong to the Arduino loop
   * task, as the header says. Calling it here made two tasks converters and two
   * writers of one struct. Clearing the deadline instead means the next
   * battery_loop() samples immediately, which is within ten milliseconds and on
   * the right task.
   */
  nextSample = millis();
}

float battery_calibration_for(float actualVolts) {
  // Against the pack, because that is what a meter is across. The window is the
  // one a real pack of this many cells can occupy; a target outside it is a
  // wrong divider ratio rather than a tolerance to trim out, and trimming would
  // hide the actual problem.
  if (actualVolts < BATTERY_MIN_PLAUSIBLE_V * cells ||
      actualVolts > BATTERY_MAX_PLAUSIBLE_V * cells) {
    return 0.0f;
  }
  portENTER_CRITICAL(&mux);
  const uint16_t mv = status.millivoltsAtPin;
  portEXIT_CRITICAL(&mux);
  if (mv < 100) return 0.0f;  // nothing on the pin: there is nothing to trim
  const float uncalibrated = (mv / 1000.0f) * divider;
  if (uncalibrated <= 0.05f) return 0.0f;
  const float trim = actualVolts / uncalibrated;
  return (trim >= 0.5f && trim <= 2.0f) ? trim : 0.0f;
}

const char *battery_state_name(BatteryState state) {
  switch (state) {
    case BAT_DISCHARGING: return "discharging";
    case BAT_CHARGING: return "charging";
    case BAT_FULL: return "full";
    case BAT_LOW: return "low";
    case BAT_CRITICAL: return "critical";
    default: return "unknown";
  }
}

bool battery_command(const char *line) {
  // The keyword has to end the word, not merely start it. A plain prefix test
  // claimed every line beginning with those letters -- "battery" was handled
  // here as "bat" with the argument "tery", printed an unknown-argument
  // complaint, and returned true, so no later handler ever saw it. Requiring a
  // space or the end of the line keeps each console verb to itself.
  if (strncmp(line, "bat", 3) != 0) return false;
  if (line[3] != 0 && line[3] != ' ') return false;
  const char *arg = line + 3;
  while (*arg == ' ') arg++;

  if (strncmp(arg, "calib ", 6) == 0) {
    const float want = atof(arg + 6);
    const float trim = battery_calibration_for(want);
    if (trim <= 0.0f) {
      Serial.println("[bat] cannot calibrate: no plausible reading on the pin, "
                     "or the target is more than 2x out");
      return true;
    }
    battery_configure(enabled, divider, trim, fullVolts, emptyVolts, cells,
                      lowPct, critPct);
    Serial.printf("[bat] trim %.4f applied (not saved -- use the dashboard to "
                  "store it)\n", trim);
    return true;
  }

  BatteryStatus s;
  battery_snapshot(&s);
  if (!s.enabled) {
    Serial.println("[bat] gauge is off: no sense pin, or disabled in settings");
    return true;
  }
  if (!s.present) {
    Serial.printf("[bat] no battery: pin reads %u mV (needs >= %.0f mV for a "
                  "%.1f V cell through a %.2f:1 divider)\n",
                  (unsigned)s.millivoltsAtPin,
                  BATTERY_MIN_PLAUSIBLE_V / s.divider * 1000.0f,
                  BATTERY_MIN_PLAUSIBLE_V, s.divider);
    return true;
  }
  Serial.printf("[bat] %u%% | %.3f V", (unsigned)s.percent, s.volts);
  if (s.cells > 1) Serial.printf(" (%.3f V/cell x%u)", s.cellVolts, s.cells);
  Serial.printf(" | %s | pin %u mV | divider %.2f trim %.4f | full %.2f/cell "
                "empty %.2f/cell | low %u%% crit %u%%",
                battery_state_name(s.state), (unsigned)s.millivoltsAtPin,
                s.divider, s.calibration, s.fullVolts, s.emptyVolts,
                (unsigned)s.lowPercent, (unsigned)s.criticalPercent);
  if (!s.haveChargePins) {
    Serial.print(" | charger pins not wired: charging cannot be detected");
  }
  Serial.println();
  return true;
}

#endif  // BATTERY_ENABLED
