#include "app_config.h"
#include "power.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_wifi.h>

#include "audio_probe.h"
#include "battery.h"
#include "df_player.h"
#include "leds.h"
#include "runtime_events.h"
#include "status_led.h"
#include "ui.h"
#include "ui_config.h"

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

// --- sleep ------------------------------------------------------------------
SleepMode g_sleepMode = SLEEP_MODE_OFF;
uint32_t g_sleepAfterMs = (uint32_t)POWER_SLEEP_AFTER_S_DEFAULT * 1000UL;
uint32_t g_activeAtWord;
bool g_sleeping;  // the shutdown is running; do not start it twice

/// The same grace the display's idle blanking uses, and for the same reason: a
/// fade or the gap between two tracks is not the speaker being finished with.
constexpr uint32_t SLEEP_AUDIO_GRACE_MS = UI_AUDIO_GRACE_MS;

/// How long the panel says what is about to happen before it happens. Long
/// enough to read, short enough not to feel like a fault.
constexpr uint32_t SLEEP_NOTICE_MS = 2500;

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

  LOGF("[power] saving %s\n", on ? "on" : "off");
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

/// Is the speaker doing anything? The same three inputs the display's idle
/// blanking uses, deliberately: two different answers to "is this thing in use"
/// is one more than a speaker needs.
bool busy(uint32_t now) {
  const uint32_t heard = audio_probe_last_active();
  if (heard != 0 && (now - heard) < SLEEP_AUDIO_GRACE_MS) return true;
  return df_player_active();
}

void serviceSleep(uint32_t now) {
  if (g_sleepMode == SLEEP_MODE_OFF || g_sleeping) return;
  if (!power_sleep_possible()) return;
  if (g_sleepMode == SLEEP_MODE_SAVING && !g_active) {
    // Tied to saving and saving is not on: the countdown has not started, so it
    // should not be part-way through when it does.
    __atomic_store_n(&g_activeAtWord, now, __ATOMIC_RELEASE);
    return;
  }
  if (busy(now)) {
    __atomic_store_n(&g_activeAtWord, now, __ATOMIC_RELEASE);
    return;
  }
  if (now - __atomic_load_n(&g_activeAtWord, __ATOMIC_ACQUIRE) <
      g_sleepAfterMs) {
    return;
  }
  power_sleep_now();
}

}  // namespace

/*
 * Survives the restart that ends standby. RTC slow memory is not touched by a
 * software reset and NOINIT keeps the startup code from zeroing it, so this is
 * the one thing that can tell "woke from standby" from "somebody rebooted it":
 * both are ESP_RST_SW as far as the reset reason is concerned.
 */
RTC_NOINIT_ATTR uint32_t g_wokeMagic;
constexpr uint32_t WOKE_MAGIC = 0x5A11EEB1;

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
  serviceSleep(now);
}

bool power_saving() { return g_active; }

const char *power_reason() { return g_reason; }

bool power_auto_blind() { return g_blind; }

// ------------------------------------------------------------------ sleep ---
void power_configure_sleep(SleepMode mode, uint16_t after_seconds) {
  g_sleepMode = mode > SLEEP_MODE_SAVING ? SLEEP_MODE_OFF : mode;
  if (after_seconds < POWER_SLEEP_AFTER_S_MIN) after_seconds = POWER_SLEEP_AFTER_S_MIN;
  if (after_seconds > POWER_SLEEP_AFTER_S_MAX) after_seconds = POWER_SLEEP_AFTER_S_MAX;
  g_sleepAfterMs = (uint32_t)after_seconds * 1000UL;
  // A policy that has just been chosen starts its countdown now rather than
  // part-way through, which for a long-running speaker would otherwise mean
  // "sleep immediately".
  __atomic_store_n(&g_activeAtWord, millis(), __ATOMIC_RELEASE);
}

void power_note_activity() {
  __atomic_store_n(&g_activeAtWord, millis(), __ATOMIC_RELEASE);
}

uint32_t power_idle_ms() {
  return millis() - __atomic_load_n(&g_activeAtWord, __ATOMIC_ACQUIRE);
}

bool power_sleep_possible() { return PIN_UI_BUTTON >= 0; }

bool power_woke_from_sleep() {
  static bool checked, woke;
  if (!checked) {
    checked = true;
    woke = esp_reset_reason() == ESP_RST_SW && g_wokeMagic == WOKE_MAGIC;
    g_wokeMagic = 0;  // read once: the next boot is not this one
  }
  return woke;
}

bool power_sleep_now() {
  if (!power_sleep_possible()) {
    LOGLN("[power] no wake button compiled in; refusing to stand by");
    return false;
  }
  if (g_sleeping) return true;
  g_sleeping = true;
  runtime_event_note(RUNTIME_EVENT_STANDBY);

  LOGF("[power] standby; wake on GPIO%d\n", (int)PIN_UI_BUTTON);

  /*
   * Say so first, and on the panel rather than only on a serial port nobody is
   * watching. A speaker that goes dark and silent with no warning is
   * indistinguishable from one that has crashed.
   */
  ui_show_system_status(UI_STATUS_GOODBYE, "Goodbye",
                        "Press BOOT to wake", -1, 0);
  delay(SLEEP_NOTICE_MS);

  /*
   * The DFPlayer first: it is the one that is audible, and its command has
   * furthest to travel -- two frames at the driver's own pacing on a 9600 baud
   * link. Stop before standby, because a sleeping module ignores everything
   * including stop, so the order is not interchangeable.
   */
  df_player_stop();
  df_player_standby();
  delay(300);

  /*
   * Each on the task that owns the hardware. Two writers on one I2C bus or one
   * RMT channel is how a shutdown becomes a crash -- so both of these only ask,
   * and the owning task acts.
   *
   * The ring is then waited for rather than guessed at. WS2812s latch: if the
   * radios come down and the core drops to 10 MHz before the dark frame has
   * actually been clocked out, the pixels stay lit at whatever the last effect
   * left them, drawing their full current, on a board that is supposed to be
   * asleep. leds_suspended() says when that frame has gone; the bound is there
   * so a wedged render task cannot stop the speaker going to standby at all.
   */
  leds_suspend();
  ui_suspend();
  status_led_mute(true);
  constexpr uint32_t BLANK_WAIT_MS = 400;
  const uint32_t blankDeadline = millis() + BLANK_WAIT_MS;
  while (!leds_suspended() && (int32_t)(millis() - blankDeadline) < 0) {
    delay(10);
  }
  if (!leds_suspended()) {
    LOGLN("[power] the ring did not confirm its dark frame; going to "
                   "standby anyway");
  }
  delay(150);  // the panel's own power-down, on the UI task

  /*
   * The radios, which on this chip are most of the current. Both are guarded on
   * their own status rather than on which mode we think we are in: Bluetooth is
   * never started in Wi-Fi only mode, and tearing down a controller that was
   * never brought up is a panic, not a no-op.
   */
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
  if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) {
    esp_bluedroid_disable();
  }
  if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED) {
    esp_bluedroid_deinit();
  }
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) {
    esp_bt_controller_disable();
  }
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED) {
    esp_bt_controller_deinit();
  }

  /*
   * And the core itself. Ten megahertz is the lowest the PLL-free path will do
   * and is legal only because both radios are down -- the Wi-Fi and Bluetooth
   * MACs need 80 MHz to keep time with the air. Nothing is left that needs to
   * be fast: one GPIO read every fifty milliseconds.
   */
  setCpuFrequencyMhz(10);

  LOGFLUSH();

  /*
   * The wait. Deliberately a restart rather than a resume: coming back means
   * the radios, the audio path and the DFPlayer all have to be brought up from
   * nothing, and that is precisely what setup() does. Waking into a half-torn-
   * down speaker to save four seconds is not a trade worth making.
   *
   * The press has to be a deliberate one. A speaker in a bag that brushes the
   * button should stay asleep, so the button is required to be down for
   * WAKE_HOLD_MS rather than merely observed low once.
   */
  pinMode(PIN_UI_BUTTON, INPUT_PULLUP);
  constexpr uint32_t WAKE_HOLD_MS = 400;
  uint32_t downSince = 0;
  for (;;) {
    if (digitalRead(PIN_UI_BUTTON) == LOW) {
      if (downSince == 0) downSince = millis();
      if (millis() - downSince >= WAKE_HOLD_MS) break;
    } else {
      downSince = 0;
    }
    delay(50);
  }

  setCpuFrequencyMhz(240);  // so the restart runs at the speed it expects
  g_wokeMagic = WOKE_MAGIC;
  LOGLN("[power] waking");

  /*
   * Wait for the button to come back up before resetting.
   *
   * PIN_UI_BUTTON is GPIO0, the download-mode strap, and the ESP32 re-samples
   * its strapping pins on the system reset esp_restart() performs -- not only at
   * power-on. Restarting with the button still down therefore lands in the ROM
   * serial bootloader rather than in setup(), and the speaker looks dead until
   * it is power-cycled. Waking requires a 400 ms hold, so at this point the
   * button is by construction still pressed; without this loop the failure is
   * not a race but the normal case.
   *
   * Bounded, because a shorted or stuck button must not strand the board here
   * with both radios already down. Past the deadline the reset goes ahead: a
   * chip in download mode is recoverable and an unresponsive one is not.
   */
  constexpr uint32_t RELEASE_WAIT_MS = 10000;
  const uint32_t releaseDeadline = millis() + RELEASE_WAIT_MS;
  while (digitalRead(PIN_UI_BUTTON) == LOW &&
         (int32_t)(millis() - releaseDeadline) < 0) {
    delay(10);
  }
  if (digitalRead(PIN_UI_BUTTON) == LOW) {
    LOGLN("[power] BOOT still held after 10 s; restarting anyway. If "
                   "the speaker comes back silent, the chip is in download mode "
                   "-- release the button and reset it.");
  }
  delay(60);  // contact bounce on the release, well clear of the reset
  LOGFLUSH();
  ESP.restart();
  return true;  // unreachable; keeps the signature honest
}
