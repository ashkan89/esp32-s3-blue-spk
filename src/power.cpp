#include "power.h"

#include <Arduino.h>
#include <WiFi.h>

#include "battery.h"
#include "leds.h"
#include "status_led.h"
#include "ui.h"

namespace {

PowerMode g_mode = POWER_MODE_OFF;
uint8_t g_threshold = 20;
bool g_active;
bool g_blind;
char g_reason[96] = "Power saving is off.";

/*
 * Hysteresis, in points of charge.
 *
 * The gauge is a voltage reading off a curve, and voltage moves when the load
 * does -- which is exactly what saving changes. So a pack sitting on the
 * threshold switches the lights off, sags less, reads a point higher, switches
 * them back on, and oscillates. Coming out needs to be visibly better than
 * going in, not merely different.
 */
constexpr uint8_t EXIT_MARGIN_PCT = 5;

/*
 * How often the policy is worth re-asking.
 *
 * power_tick() is called from the Arduino loop, which goes round thousands of
 * times a second, and every pass would otherwise take a battery snapshot behind
 * a critical section and format a sentence nobody reads. The gauge samples far
 * slower than this anyway, so half a second is already more often than the
 * input can change.
 */
constexpr uint32_t EVAL_EVERY_MS = 500;
uint32_t g_lastEval;

/// Transmit power while saving. -- 11 dBm rather than the default 19.5 keeps a
/// dashboard usable across a room and costs roughly a third of the radio's
/// transmit current. Anything lower starts dropping the far end of a house.
constexpr wifi_power_t SAVE_TX_POWER = WIFI_POWER_11dBm;

void say(const char *text) { strlcpy(g_reason, text, sizeof(g_reason)); }

/// Applies the four effects. Called only when the answer actually changes, so
/// the Wi-Fi calls -- the only ones here that are not a flag write -- do not run
/// on every loop.
void apply(bool on) {
  leds_set_power_save(on);
  ui_set_power_save(on);
  status_led_mute(on);

  /*
   * Only with a radio to talk to. This runs from loadSettings() as well as from
   * the loop, and in POWER_MODE_ON that is before WiFi.begin() -- and in
   * Bluetooth-only mode it is before nothing, because the station is never
   * started at all. Both calls would be errors logged into the boot output for
   * no benefit.
   *
   * In access-point mode the radio has to stay awake for its clients and the
   * IDF ignores the sleep request; asking anyway is harmless and means there is
   * no deferred state to unwind when a station comes up later.
   */
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.setSleep(on);
    // Back to the ESP32 default rather than to a remembered value: nothing else
    // in this firmware sets transmit power, so the default is what it was.
    WiFi.setTxPower(on ? SAVE_TX_POWER : WIFI_POWER_19_5dBm);
  }

  Serial.printf("[power] saving %s\n", on ? "on" : "off");
}

/// The policy, with no side effects, so the reason and the answer are decided
/// in one place and cannot disagree with each other.
bool decide() {
  g_blind = false;

  if (g_mode == POWER_MODE_OFF) {
    say("Power saving is off. Every setting is the one you chose.");
    return false;
  }
  if (g_mode == POWER_MODE_ON) {
    say("Power saving is on, on mains as well as on battery.");
    return true;
  }

  BatteryStatus b;
  battery_snapshot(&b);

  if (!b.enabled) {
    g_blind = true;
    say("Automatic needs the battery gauge, which is switched off in Settings.");
    return false;
  }
  if (!b.present) {
    g_blind = true;
    say("Automatic is waiting for a pack: the gauge is on but reading no "
        "plausible cell voltage.");
    return false;
  }

  // Charging wins outright. A low pack with a charger on it is a pack that is
  // getting better, and there is nothing left to stretch.
  if (b.charging || b.chargeDone) {
    say(b.chargeDone ? "Charged, so nothing is being saved."
                     : "Charging, so nothing is being saved whatever the "
                       "percentage says.");
    return false;
  }

  const uint8_t exit_at =
      (uint8_t)(g_threshold + EXIT_MARGIN_PCT > 100 ? 100
                                                    : g_threshold + EXIT_MARGIN_PCT);
  const bool on = g_active ? b.percent < exit_at : b.percent <= g_threshold;

  // Three sentences rather than two, because of the hysteresis: between the
  // threshold and the exit point saving is still on while the percentage is
  // above the number the owner set, and "at or below the threshold" would be a
  // plain lie about the only figure on the card.
  char text[96];
  if (!on) {
    snprintf(text, sizeof(text), "On battery at %u%%, above the %u%% threshold.",
             (unsigned)b.percent, (unsigned)g_threshold);
  } else if (b.percent <= g_threshold) {
    snprintf(text, sizeof(text),
             "On battery at %u%%, at or below the %u%% threshold.",
             (unsigned)b.percent, (unsigned)g_threshold);
  } else {
    snprintf(text, sizeof(text),
             "On battery at %u%%, and saving until %u%% to avoid flapping.",
             (unsigned)b.percent, (unsigned)exit_at);
  }
  say(text);
  return on;
}

/// Decide and act. Separate from power_tick() so power_configure() can bypass
/// the throttle: a setting the owner just changed should take effect now, not
/// within half a second.
void evaluate() {
  const bool want = decide();
  if (want == g_active) return;
  g_active = want;
  apply(want);
}

}  // namespace

void power_configure(PowerMode mode, uint8_t threshold) {
  g_mode = mode > POWER_MODE_AUTO ? POWER_MODE_OFF : mode;
  g_threshold = threshold > 100 ? 100 : threshold;
  g_lastEval = millis();
  evaluate();
}

void power_tick() {
  const uint32_t now = millis();
  if (now - g_lastEval < EVAL_EVERY_MS) return;
  g_lastEval = now;
  evaluate();
}

bool power_saving() { return g_active; }

const char *power_reason() { return g_reason; }

bool power_auto_blind() { return g_blind; }
