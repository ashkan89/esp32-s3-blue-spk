/*
 * power.h -- one switch for everything on this board that costs current.
 *
 * The parts of a speaker that draw power and are not the speaker: seven WS2812
 * pixels at up to 260 mA, an OLED panel at ~15 mA, an indicator LED, and a Wi-Fi
 * radio that by default never sleeps between beacons. Each of those already has
 * its own setting, and setting five of them by hand every time the battery gets
 * low is not a feature. This is the one switch.
 *
 * What it does *not* touch is the audio path. Dropping the CPU clock is the
 * obvious next saving and it is deliberately not here: SBC decode with the
 * Bluetooth stack running has little headroom at 240 MHz and none at 160, and a
 * power mode whose symptom is crackle is not one anybody would leave on. The
 * same reasoning that keeps CORE_DEBUG_LEVEL at 1 applies -- see the note in
 * platformio.ini. Everything below is peripheral current, and none of it can
 * reach a sample.
 *
 * ---------------------------------------------------------------------------
 * The three modes
 * ---------------------------------------------------------------------------
 *   POWER_MODE_OFF    nothing is saved; every setting is the one you chose
 *   POWER_MODE_ON     saving, always, mains or battery
 *   POWER_MODE_AUTO   saving while the pack is at or below a threshold you set
 *                     *and* nothing is charging it
 *
 * Charging wins outright in AUTO, whatever the percentage says. A pack at 8%
 * with a charger on it is a pack that is getting better, and dimming the lights
 * to protect it makes the speaker worse for no reason -- the whole point of the
 * mode is to stretch the time until a charger arrives, and one has.
 *
 * AUTO needs a gauge. With the battery switched off in settings, no sense pin
 * compiled in, or no plausible cell voltage on it, there is nothing to read and
 * the mode reports itself inactive rather than guessing -- power_reason() says
 * which, so a card that will not engage explains itself.
 */

#pragma once

#include <stdint.h>

enum PowerMode : uint8_t {
  POWER_MODE_OFF = 0,  ///< never save
  POWER_MODE_ON,       ///< always save
  POWER_MODE_AUTO,     ///< save on battery, at or below the threshold
};

/// Applies a policy. `threshold` is the battery percentage AUTO engages at or
/// below, 0-100; it is ignored by the other two modes. Re-evaluates immediately,
/// so the effects are visible before this returns. Safe from any task.
void power_configure(PowerMode mode, uint8_t threshold);

/// Re-reads the gauge and applies the policy. Call from the Arduino loop; it
/// does nothing most of the time and only touches anything when the answer
/// changes.
void power_tick();

/// Whether saving is in force right now.
bool power_saving();

/// One line for the dashboard saying why it is or is not, which is the whole
/// difference between a mode that looks broken and one that explains itself.
const char *power_reason();

/// True while AUTO has nothing to work with: no gauge, or no pack on it. The
/// card greys the threshold out and says so rather than counting down against
/// a percentage that does not exist.
bool power_auto_blind();

// ------------------------------------------------------------------ sleep ---
/*
 * Standby, which is a different thing from saving.
 *
 * Saving leaves a working speaker that costs less to run. Standby stops being a
 * speaker: everything external goes dark, both radios come down, the core drops
 * to 10 MHz, and the board sits there watching one GPIO until somebody presses
 * BOOT -- at which point it restarts, because coming back means bringing the
 * radios, the audio path and the DFPlayer up from nothing, and setup() is what
 * does that.
 *
 * ---------------------------------------------------------------------------
 * Why this is not esp_deep_sleep_start()
 * ---------------------------------------------------------------------------
 * Because it does not fit. Deep sleep would be ~10 uA against the ~15 mA this
 * manages, and it was the first thing tried -- but esp_sleep's entry path has
 * to run with the flash cache off, so it lives in IRAM, and it wants about
 * 1.8 KB of it. This firmware has 653 bytes of IRAM left: the Bluetooth
 * controller blob alone holds 33 KB there, and the same ceiling is why the
 * WS2812 driver is forty lines of RMT rather than a library and why the PSRAM
 * cache workaround is switched off in platformio.ini. Linking deep sleep in
 * overflows iram0_0_seg by 1012 bytes and the image will not build.
 *
 * So this is what fits, and the difference is real: standby is roughly 15 mA
 * where deep sleep would be effectively nothing, which on a 2000 mAh pack is
 * about five days rather than about forever. Against the ~260 mA the ring alone
 * can draw it is still worth having. A build with Bluetooth compiled out has
 * the IRAM for the real thing; that is the way in if the microamps matter more
 * than A2DP does.
 *
 * ---------------------------------------------------------------------------
 * What has to be shut down by hand
 * ---------------------------------------------------------------------------
 * The external parts, because nothing about a quiet ESP32 reaches them:
 *
 *   the ring     WS2812s latch: they hold the last colour they were sent
 *                forever, with no clock and no data. A ring left mid-rainbow
 *                would stay mid-rainbow, lit, drawing its full current, on a
 *                board that is otherwise asleep.
 *   the panel    the SSD1306 keeps displaying whatever is in its buffer, and
 *                has to be told to power down.
 *   the DFPlayer decodes its own card and does not care what the ESP32 is
 *                doing. Left alone it would go on playing to an empty room.
 *
 * The DAC needs nothing: with the I2S clocks stopped the PCM5102A idles.
 *
 * Waking is the BOOT button, which is already on the board and already the
 * speaker's only control. It has to be held for a moment rather than merely
 * seen low, so a speaker in a bag that brushes it stays asleep.
 *
 * And then the release has to be waited for before the restart. The ESP32
 * re-samples its strapping pins on a *system* reset, which is what esp_restart()
 * performs -- it is not only power-on that latches them. GPIO0 is the
 * download-mode strap, so restarting while the wake button is still down puts
 * the chip in the ROM serial bootloader instead of running the firmware, and
 * from the outside that is indistinguishable from a speaker that died in
 * standby. The wake sequence is therefore "held long enough, then let go", the
 * same rule the factory-reset hold in main.cpp follows for the same reason.
 */

enum SleepMode : uint8_t {
  SLEEP_MODE_OFF = 0,    ///< never sleep on its own
  SLEEP_MODE_IDLE,       ///< sleep after the timeout, whatever the power mode
  SLEEP_MODE_SAVING,     ///< sleep after the timeout, but only while saving
};

/// Applies the sleep policy. `after_seconds` is the idle time before the
/// speaker puts itself away, clamped to POWER_SLEEP_AFTER_S_MIN..MAX.
void power_configure_sleep(SleepMode mode, uint16_t after_seconds);

/// Defers sleep. Called from ui_wake(), so everything already treated as the
/// owner doing something -- the button, the dashboard, the serial console, an
/// update writing progress -- pushes the timer out without a second list of
/// call sites to keep in step.
void power_note_activity();

/// Milliseconds since the speaker last did anything, by the same definition the
/// display's idle blanking uses: audio through the analyser, the DFPlayer
/// playing, or the owner. For the dashboard's countdown.
uint32_t power_idle_ms();

/// Shuts everything down in order and waits for the button. Does not return:
/// it ends in a restart. Refuses, and says so, when no wake button is compiled
/// in -- a speaker that cannot be woken is not one to put to sleep.
bool power_sleep_now();

/// False when PIN_UI_BUTTON is -1, in which case there is nothing that could
/// wake the board and the card says so rather than offering a switch that would
/// strand it.
bool power_sleep_possible();

/// True if this boot is a wake from standby rather than a power-on or an
/// ordinary reboot. Read once; the answer is consumed.
bool power_woke_from_sleep();

static const uint16_t POWER_SLEEP_AFTER_S_MIN = 60;
static const uint16_t POWER_SLEEP_AFTER_S_MAX = 43200;  // 12 hours
static const uint16_t POWER_SLEEP_AFTER_S_DEFAULT = 1800;

/*
 * What saving does, so the dashboard can list it without repeating itself and
 * the two owners below can be found from here:
 *
 *   the ring     off -- by a long way the largest load on the board, and the
 *                one whose absence is most obviously a choice rather than a
 *                fault (leds.cpp honours it in the same place as resting)
 *   the panel    off -- the same ~15 mA the blanking modes get back, taken
 *                unconditionally rather than after a timeout (ui.cpp)
 *   the Wi-Fi    modem sleep on and transmit power reduced, which costs some
 *                latency on the dashboard and nothing else
 *   the LED      the indicator is left dark between states
 *
 * All four come back the moment saving ends, at whatever they were set to
 * before: saving never writes a setting, it only overrides one.
 */
