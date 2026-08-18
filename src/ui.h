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

/// Brings up I2C and the panel. Returns false if nothing answers on the bus, in
/// which case the whole UI stays switched off and the speaker works as before.
bool ui_begin();

/// Spawns the render task. No-op if ui_begin() failed.
void ui_start();

/// True if a panel was found.
bool ui_present();

/// Resets the idle timers and undims. Call on anything the user did.
void ui_wake();

/// Replaces "Ready to pair" on the idle screen, and suppresses the Bluetooth
/// screens. Used by the test-tone build, which has no radio at all.
void ui_set_headline(const char *text);

/// Serial console: "next", "screen <n>", "auto", "bright <0-255>", "ui".
/// Returns false if the line was not one of those.
bool ui_command(const char *line);
