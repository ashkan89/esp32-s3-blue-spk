/*
 * ui.h -- the 128x32 OLED user interface.
 *
 * Runs on its own FreeRTOS task, pinned to core 0, at priority 1. That matters:
 * pushing one 512-byte frame over I2C blocks for 7-12 ms, and doing that in
 * loop() would fight with the melody playback that also lives there. On its own
 * low-priority task it simply gets preempted by anything that matters, and the
 * animation degrades instead of the audio.
 *
 * It never touches the audio path. Everything it draws comes from two read-only
 * sources: a lock-free snapshot of player_state.h, and the analyser in
 * audio_probe.h. There is no path from the display back into the stream.
 *
 * Screens rotate on their own and are also reachable from the BOOT button and
 * the serial console:
 *
 *   Now playing   title, artist, live progress bar, mini spectrum
 *   Spectrum      32-band analyser, three styles (bars / mirrored / matrix)
 *   VU            stereo RMS meters with peak dots and a dB scale
 *   Scope         trigger-aligned waveform
 *   Waterfall     scrolling spectrogram, dithered
 *   Clock         seven-segment time, date, seconds sweep
 *   Info          device name, phone, volume, uptime, heap, clock source
 *
 * plus two that take over when they apply: a pairing screen with an animated
 * beacon while no phone is connected, and a bouncing screensaver after five
 * minutes of nothing at all.
 */

#pragma once

#include <stdint.h>

// For the blanking defaults and bounds below, which are owner-tunable and so
// live with the rest of the UI's build-time settings.
#include "ui_config.h"

/// Brings up I2C and the panel. Returns false if nothing answers on the bus, in
/// which case the whole UI stays switched off and the speaker works as before.
bool ui_begin();

/// Spawns the render task. No-op if ui_begin() failed.
void ui_start();

/// True if a panel was found.
bool ui_present();

/// Resets the idle timers, undims, and lights the panel again if blanking had
/// switched it off. Call on anything the user did.
void ui_wake();

/*
 * When the panel switches itself off.
 *
 * Distinct from the dim and the screensaver: those keep the display lit and
 * only move the lit pixels around, which is a burn-in measure. This turns the
 * display off at the controller, which is the only thing that stops the panel
 * ageing at all -- and the only one that gets back the ~15 mA it draws, which
 * on a battery is the reason to want it.
 *
 * The two timed modes differ in what counts as a reason to stay on:
 *
 *   UI_BLANK_IDLE     runs off the same idle timer the screensaver uses, which
 *                     audio keeps warm -- so a speaker that is playing keeps
 *                     its display no matter how long the track is.
 *   UI_BLANK_ALWAYS   runs off the last thing the *owner* did: a button press,
 *                     a dashboard action, a serial command. Playback does not
 *                     hold it open, so the panel goes dark mid-album.
 *
 * A system overlay -- an update, a restart, the factory-reset countdown --
 * suspends both. Those are the moments somebody is watching the panel for a
 * reason, and blanking through one would look like a crash.
 */
enum UiBlankMode : uint8_t {
  UI_BLANK_NEVER = 0,  ///< the panel stays on
  UI_BLANK_IDLE,       ///< off once nothing is playing and nobody has touched it
  UI_BLANK_ALWAYS,     ///< off on a timer, whatever is playing
};

/// Applies a blanking policy and wakes the panel so the change is visible.
/// `after_seconds` is clamped to UI_BLANK_AFTER_S_MIN..MAX and ignored for
/// UI_BLANK_NEVER. Safe from any task.
void ui_set_blank(UiBlankMode mode, uint16_t after_seconds);

/// True while blanking has the display switched off, so the dashboard can say
/// so rather than leaving a dark panel looking like a fault.
bool ui_blanked();

/// Holds the panel off for power saving, over the top of whatever blanking mode
/// is set. Lifted the moment saving ends; it writes no setting. See power.h.
void ui_set_power_save(bool on);

/// Powers the panel down and parks the render task. For standby: without it the
/// task goes on running an FFT thirty times a second to draw nothing. There is
/// no resume -- standby ends in a restart.
void ui_suspend();

/// The two countdowns, in milliseconds, for the dashboard to show. `idle` is
/// what UI_BLANK_IDLE watches -- reset by audio as well as by the owner --
/// and `untouched` is what UI_BLANK_ALWAYS watches. Reported because "it never
/// blanks" is otherwise a guess about which of them is being held open.
uint32_t ui_idle_ms();
uint32_t ui_untouched_ms();

enum UiSystemStatus : uint8_t {
  UI_STATUS_NETWORK = 0,
  UI_STATUS_UPDATE,
  UI_STATUS_SUCCESS,
  UI_STATUS_ERROR,
  UI_STATUS_RESTART,
  /*
   * The two moments the speaker stops and starts being one, which get their own
   * full-screen artwork rather than the bordered panel the others share.
   *
   * That is not decoration for its own sake. The panel is the shape this
   * firmware uses for "something is happening, wait" -- an update, a reconnect,
   * a countdown -- and going dark and silent immediately afterwards is the one
   * case where that reads wrong: a speaker that shows "please wait" and then
   * never comes back is indistinguishable from one that crashed. A farewell
   * that is visibly a farewell says the shutdown was deliberate, which is the
   * whole information content of the screen.
   *
   * GOODBYE is shown by power_sleep_now() before standby; WELCOME by setup()
   * on the boot that follows one, so waking is acknowledged rather than looking
   * like an unexplained reboot.
   */
  UI_STATUS_GOODBYE,
  UI_STATUS_WELCOME,
};

/// Temporarily takes over the OLED for an important system operation. Progress
/// is 0..100, or -1 for a status without a progress bar. duration_ms=0 keeps it
/// visible until replaced/cleared. Safe to call from any task.
void ui_show_system_status(UiSystemStatus kind, const char *title,
                           const char *detail, int16_t progress,
                           uint32_t duration_ms = 0);

/// Serial console: "next", "screen <n>", "auto", "bright <0-255>", "ui".
/// Returns false if the line was not one of those.
bool ui_command(const char *line);

/// True once, when the BOOT button offer to change radio mode has been
/// confirmed with a second press. Polled from loop().
bool ui_take_mode_switch_request();

/// True once, when the BOOT button has been held through the whole factory
/// reset countdown. Polled from loop(); the caller does the wiping and the
/// reboot, because GPIO0 is also the download-mode strap and restarting while
/// it is still held leaves the chip in the serial bootloader.
bool ui_take_factory_reset_request();
