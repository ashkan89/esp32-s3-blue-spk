/*
 * leds.h -- the WS2812 ring: fifteen effects, a colour picker, and a music
 * sync that runs off the same FFT the spectrum screen draws.
 *
 * Wiring, the current budget and the level-shifting caveat are all in
 * hw_config.h under "WS2812 ring"; this file is the behaviour.
 *
 * ---------------------------------------------------------------------------
 * Threading
 * ---------------------------------------------------------------------------
 * One task, core 0, priority 1 -- the same berth as the display, and for the
 * same reason: it must never be in a position to delay a sample. Writing seven
 * pixels costs 210 us of RMT time and almost no CPU, so the task spends its
 * life asleep between frames.
 *
 * It never touches the audio path. Everything it reacts to comes from
 * audio_probe_frame(), which is a read-only copy of an analysis the display
 * task may well have done already. There is no path from the lighting back
 * into the stream.
 *
 * Configuration arrives from the web task, which is a different task again, so
 * the live settings live behind a spinlock. Both sides only ever copy the whole
 * struct in or out -- it is a couple of dozen bytes and the critical section is
 * a memcpy, which is cheaper than reasoning about which fields may be seen
 * half-updated.
 *
 * ---------------------------------------------------------------------------
 * How the music sync works
 * ---------------------------------------------------------------------------
 * audio_probe.h hands over 32 log-spaced bands, stereo VU levels and a beat
 * flag, already smoothed and auto-gained for display. That is most of the work,
 * and reusing it is why the lighting reacts identically to Bluetooth, to a
 * network stream and to the start-up chimes -- all three feed the same probe.
 *
 * The exception is DFPlayer mode. That module decodes its own card and hands
 * out analog audio that never passes through this chip, so there is nothing to
 * analyse and the reactive effects sit at their resting brightness. The
 * dashboard says so rather than leaving you to work it out.
 *
 * Three signals are pulled out of the analysis and smoothed again, more gently,
 * because what reads well as a 32-bar graph is twitchy as a light:
 *
 *   loudness   drives the global reactive dimming that rides on top of *every*
 *              effect, so even a plain rainbow breathes with the track
 *   bass       drives the centre pixel and the fire/pulse effects
 *   beat       the transient flag, used as a trigger rather than a level: it
 *              fires the strobe, kicks the hue on, and flashes the ring
 *
 * `reactivity` is how much of any of that is allowed to show, 0 to 100. At 0
 * the effects run exactly as they would in silence, which is what you want for
 * a lamp; at 100 a quiet passage is genuinely dim.
 */

#pragma once

#include <stdint.h>

#include "hw_config.h"

/*
 * The effects, in the order the dashboard lists them.
 *
 * The first six are the ones that do not care about the audio, the next four
 * are decorative but still ride the global reactive dimming, and the last four
 * are driven by the music outright. LED_FX_OFF is a real effect rather than a
 * separate flag so that "off" survives a reboot like any other choice.
 */
enum LedEffect : uint8_t {
  LED_FX_OFF = 0,      ///< dark, but the driver is still running
  LED_FX_SOLID,        ///< the picked colour, steady
  LED_FX_BREATHE,      ///< the picked colour, sine fade
  LED_FX_RAINBOW,      ///< hue spread around the ring, rotating
  LED_FX_COLOR_CYCLE,  ///< whole ring one hue, cycling slowly
  LED_FX_STROBE,       ///< hard flashes; fires on the beat when reactive
  LED_FX_COMET,        ///< a head with a decaying tail, chasing round
  LED_FX_CHASE,        ///< theatre chase, primary over secondary
  LED_FX_TWINKLE,      ///< random pixels rising and falling
  LED_FX_FIRE,         ///< flicker, in the hue of the picked colour
  LED_FX_GRADIENT,     ///< primary to secondary and back, rotating
  LED_FX_VU,           ///< the ring fills with the level
  LED_FX_SPECTRUM,     ///< one frequency band per pixel, hue by pitch
  LED_FX_BEAT,         ///< dark between beats, a burst on each one
  LED_FX_MUSIC,        ///< the full sync: bass centre, spectrum rim, beat hue
  LED_FX_COUNT
};

/// Everything the owner can change. Persisted in NVS by management.cpp, applied
/// live by leds_configure(). Colours are 0x00RRGGBB, as the dashboard's colour
/// input hands them over.
struct LedConfig {
  bool enabled;
  uint8_t effect;      ///< a LedEffect
  uint8_t brightness;  ///< 0..255, before LED_BRIGHTNESS_MAX caps it
  uint8_t speed;       ///< 0..255, how fast the effect runs
  uint8_t reactivity;  ///< 0..100, how much the music is allowed to show
  uint32_t color;      ///< primary
  uint32_t color2;     ///< secondary: the other end of the gradients

  /*
   * Resting. With idleOff set, the ring goes dark once nothing has been heard
   * and nothing has been changed for idleAfterS seconds, and comes straight
   * back on the first note or the first touch of the dashboard.
   *
   * Deliberately *not* the same thing as `enabled`, which is the owner saying
   * the ring should be off and survives a reboot as that. This is the ring
   * resting between uses, and `enabled` stays true throughout -- so the page
   * still shows the effect and the colours that will come back.
   *
   * DFPlayer mode hears nothing (that module's audio never passes through this
   * chip), so there the timer only ever runs from the last dashboard change.
   */
  bool idleOff;
  uint16_t idleAfterS;
};

#if LEDS_ENABLED

/// Claims the pin and blanks the ring. False if the lighting is switched off at
/// compile time or PIN_LEDS is -1, in which case nothing else here does
/// anything and the speaker behaves as it did without a ring.
bool leds_begin();

/// Spawns the render task. No-op if leds_begin() returned false.
void leds_start();

/// True if the driver is live.
bool leds_present();

/// Applies a whole configuration. Safe from any task; takes effect on the next
/// frame, which is at most 17 ms away.
void leds_configure(const LedConfig &cfg);

/// Copies the live configuration out. Safe from any task.
void leds_get(LedConfig *out);

/// Display name of an effect, for the dashboard and the console. Returns "?"
/// for an out-of-range index rather than reading off the end of the table.
const char *leds_effect_name(uint8_t effect);

/// One-line description of what an effect does, for the dashboard.
const char *leds_effect_hint(uint8_t effect);

/// True when the audio probe has heard something recently enough for the
/// reactive effects to be doing anything. The dashboard reports it, because
/// "the music mode looks frozen" is otherwise a mystery in DFPlayer mode.
bool leds_hearing_audio();

/// True while idleOff has the ring resting. Reported for the same reason: a
/// dark ring that the page says is on and rainbow is otherwise a fault.
bool leds_resting();

/// Serial console: "leds", "leds on|off", "leds fx <0-14>", "leds color RRGGBB",
/// "leds color2 RRGGBB", "leds bright <0-255>", "leds speed <0-255>",
/// "leds react <0-100>". Returns false if the line was not one of those.
bool leds_command(const char *line);

#else
inline bool leds_begin() { return false; }
inline void leds_start() {}
inline bool leds_present() { return false; }
inline void leds_configure(const LedConfig &) {}
inline void leds_get(LedConfig *out) { *out = LedConfig{}; }
inline const char *leds_effect_name(uint8_t) { return "?"; }
inline const char *leds_effect_hint(uint8_t) { return ""; }
inline bool leds_hearing_audio() { return false; }
inline bool leds_resting() { return false; }
inline bool leds_command(const char *) { return false; }
#endif
