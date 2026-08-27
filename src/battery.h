/*
 * battery.h -- the cell voltage, as a percentage you can believe.
 *
 * Reading a battery with an ADC is easy and getting a number worth showing is
 * not, so it is worth being clear about which of the two this is.
 *
 * What it does well: resting voltage. The divider is read nine times, the
 * median is taken (the ESP32's SAR ADC produces the occasional wild sample and
 * a mean carries it through), and the result is smoothed. That gives a voltage
 * good to a few tens of millivolts once the trim is set, which is the accuracy
 * limit of the resistors rather than of the converter.
 *
 * What no voltage gauge does well: percentage under load. A Li-ion cell's
 * terminal voltage sags with current, so a speaker that starts playing looks
 * like it lost 10% and gets it back when the track ends. That is the physics,
 * not a bug -- a coulomb counter is the fix and this board does not have one.
 * Two things keep it presentable: the curve below is the loaded-discharge shape
 * rather than the open-circuit one, and the smoothing is slow enough that the
 * number does not jump around. Treat it as "roughly how full", which is what a
 * battery indicator is for.
 *
 * Charge state comes from the charger, not from the voltage. A TP4056 brings out
 * CHRG and STDBY and those two pins say exactly what is happening; without them
 * this reports "unknown" rather than inferring, because a cell resting at 4.15 V
 * and a cell being topped up at 4.15 V are indistinguishable from the voltage
 * alone and guessing would mean the indicator lies at the one moment anybody is
 * watching it.
 *
 * Threading. Sampling happens on the Arduino loop task in battery_loop() and
 * nothing else writes. Readers take a copy; the fields are small enough that a
 * torn read would have to catch a single float mid-store, and the snapshot is
 * assembled behind a critical section so it cannot.
 */

#pragma once

#include <Arduino.h>

#include "hw_config.h"

enum BatteryState : uint8_t {
  BAT_UNKNOWN = 0,   ///< no sense pin, or a reading too low to be a cell
  BAT_DISCHARGING,
  BAT_CHARGING,      ///< the charger says so (needs PIN_BATTERY_CHARGING)
  BAT_FULL,          ///< charge terminated (needs PIN_BATTERY_FULL)
  BAT_LOW,           ///< at or below the low threshold, and not charging
  BAT_CRITICAL,      ///< at or below the critical threshold, and not charging
};

struct BatteryStatus {
  bool enabled;   ///< the gauge is switched on in settings and has a pin
  bool present;   ///< a plausible cell voltage is being read
  BatteryState state;
  float volts;        ///< at the cell, after the divider and the trim
  float cellVolts;    ///< per cell, for a multi-cell pack (volts / cells)
  uint8_t percent;    ///< 0..100 from the curve
  bool low;
  bool critical;
  bool charging;      ///< raw CHRG pin, false when it is not wired
  bool chargeDone;    ///< raw STDBY pin, false when it is not wired
  bool haveChargePins;///< whether either of the two above means anything
  uint16_t millivoltsAtPin;  ///< what the ADC actually measured, for trimming
  uint32_t sampleAt;  ///< millis() of the last sample
  uint32_t samples;   ///< how many have been taken, so "warming up" is visible

  // The configuration in force, echoed so the dashboard's fields can be filled
  // from the device rather than from its own idea of the defaults.
  float divider;
  float calibration;
  float fullVolts;   ///< per cell, not per pack
  float emptyVolts;  ///< per cell, not per pack
  uint8_t cells;
  uint8_t lowPercent;
  uint8_t criticalPercent;
};

#if BATTERY_ENABLED

/// Configures the ADC and takes the first reading, so the first snapshot after
/// this is already meaningful. Returns false when the gauge is disabled or the
/// sense pin is -1, in which case every reader sees `enabled = false` and the
/// dashboard says the battery is not wired.
bool battery_begin();

/// Samples on a timer. Arduino loop task only; cheap enough to call every loop.
void battery_loop();

/// A copy of everything above.
void battery_snapshot(BatteryStatus *out);

/// True when a cell is being read. Everything else is decoration around this.
/// False when the gauge is switched off, whatever the last reading was -- a
/// disabled gauge must not keep the status LED flashing.
bool battery_present();

/// Just the percentage and the state, for the LED and the OLED, which run every
/// frame and need one value each.
uint8_t battery_percent();
BatteryState battery_state();

/// True while the cell is below the critical threshold. The status LED uses this
/// and it deliberately does not include "charging": a cell at 4% on a charger is
/// being fixed, and an indicator that keeps shouting about it is noise.
bool battery_critical();

/*
 * Applies stored configuration. Called by management once at start-up and again
 * whenever the settings are saved, because these are the numbers that turn a
 * voltage into a percentage and they belong to the pack, not to the firmware.
 *
 *   divider      cell volts per volt at the pin: 2.0 for 100k/100k
 *   calibration  multiplicative trim, 1.0 for none
 *   fullVolts    a charged cell, PER CELL      (4.20, or 4.10 for longevity)
 *   emptyVolts   where the curve reaches 0%,
 *                PER CELL                     (3.30)
 *   cells        series cells. The divider reports pack volts; the curve works
 *                per cell, so this is what converts between them. It is also
 *                why full/empty are per-cell: they feed the curve, not the
 *                divider, and a 2S pack is two of the same cell.
 *   low, crit    warning thresholds in percent
 *
 * Out-of-range values are clamped rather than rejected: this is fed from a web
 * form, and a silently sane gauge beats one that switches itself off because a
 * field was left blank.
 */
void battery_configure(bool enabled, float divider, float calibration,
                       float fullVolts, float emptyVolts, uint8_t cells,
                       uint8_t low, uint8_t crit);

/// The trim that would make the present reading equal `actualVolts`, for the
/// dashboard's "calibrate from a meter reading" button. Returns 0 when there is
/// nothing to calibrate against (no reading, or an implausible target), and the
/// caller should refuse rather than store it.
float battery_calibration_for(float actualVolts);

const char *battery_state_name(BatteryState state);

/// Serial console: "bat", "bat calib <volts>", "bat raw".
bool battery_command(const char *line);

#else

inline bool battery_begin() { return false; }
inline void battery_loop() {}
inline void battery_snapshot(BatteryStatus *out) { if (out) *out = BatteryStatus{}; }
inline bool battery_present() { return false; }
inline uint8_t battery_percent() { return 0; }
inline BatteryState battery_state() { return BAT_UNKNOWN; }
inline bool battery_critical() { return false; }
inline void battery_configure(bool, float, float, float, float, uint8_t,
                              uint8_t, uint8_t) {}
inline float battery_calibration_for(float) { return 0.0f; }
inline const char *battery_state_name(BatteryState) { return "unknown"; }
inline bool battery_command(const char *) { return false; }

#endif
