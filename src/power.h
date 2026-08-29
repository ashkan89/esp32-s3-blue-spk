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
