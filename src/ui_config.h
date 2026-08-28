/*
 * ui_config.h -- every knob for the OLED user interface in one place.
 *
 * The display is a 0.91" 128x32 I2C OLED (SSD1306, "UNIVISION" panel -- the
 * generic 4-pin blue/white module sold as "0.91 inch OLED"). It is driven with
 * U8g2 in full-buffer mode, which costs 512 bytes of RAM and lets us animate.
 *
 * Wiring (module -> ESP32):
 *   VCC -> 3.3V      (NOT 5V; these modules have no regulator)
 *   GND -> GND
 *   SDA -> GPIO21
 *   SCL -> GPIO22
 *
 * This is the canonical ESP32 I2C pair, which is what every module's silkscreen
 * and every tutorial assumes. It does cost the I2S data line its usual pin:
 * PIN_I2S_DOUT in main.cpp sits on GPIO23 rather than GPIO22 for exactly this
 * reason. Two signals cannot share one pin, so if you ever move I2C back off
 * 21/22, move the DAC's DIN back with it.
 *
 * The DS3231 RTC module, if fitted, hangs off these same two wires -- I2C is a
 * bus, and the RTC answers on 0x68 while the panel answers on 0x3C. See
 * soft_clock.h.
 */

#pragma once

#include <stdint.h>

// ------------------------------------------------------------------ panel ----

/// Set to 0 to switch the UI off: no I2C probe, no render task, no display.
/// (The U8g2 code is still linked in -- this is a runtime switch, not a way to
/// shrink the binary.) Leaving it at 1 is safe with no display attached: the
/// I2C address is probed at boot and the whole UI is skipped if nothing answers.
#ifndef UI_ENABLED
#define UI_ENABLED 1
#endif

static const int PIN_OLED_SDA = 21;
static const int PIN_OLED_SCL = 22;

/// 0x3C on almost every module; 0x3D on a few. Both are probed, in this order.
static const uint8_t OLED_ADDR_PRIMARY = 0x3C;
static const uint8_t OLED_ADDR_ALTERNATE = 0x3D;

/// I2C bit rate. One full 128x32 frame is 512 bytes, so a frame costs roughly
/// 512 * 9 / bus_hz seconds of blocking transfer: ~11.5 ms at 400 kHz, ~6.6 ms
/// at 700 kHz. 400 kHz is the SSD1306's rated maximum and is reliable even on
/// long dupont leads; most modules also run happily at 700 kHz-1 MHz if the
/// wires are short, which buys back CPU and lets UI_FPS go higher.
static const uint32_t OLED_BUS_HZ = 400000;

/// Frames per second the UI task aims for. Render is 1-5 ms, so the real
/// ceiling is set by the I2C transfer above: 30 fps is comfortable at 400 kHz,
/// 45-60 fps needs 700 kHz or more.
static const uint8_t UI_FPS = 30;

/// Contrast (SSD1306 "contrast" is really brightness), 1..255. Three levels the
/// mode button cycles through; the display also dims to DIM after a while with
/// nothing happening, because these panels do burn in.
static const uint8_t UI_BRIGHT_LOW = 40;
static const uint8_t UI_BRIGHT_MID = 120;
static const uint8_t UI_BRIGHT_HIGH = 255;
static const uint8_t UI_BRIGHT_DIM = 12;

/// Rotate the panel 180 degrees. Handy when the module ends up upside down in
/// an enclosure -- costs nothing, it is a controller register.
#ifndef UI_FLIP_180
#define UI_FLIP_180 0
#endif

// ----------------------------------------------------------------- timing ----

/// How long each screen is shown before the carousel slides to the next one.
static const uint32_t UI_SCREEN_DWELL_MS = 9000;

/// Transition length. Below ~120 ms it reads as a jump cut, above ~350 ms it
/// starts to feel slow.
static const uint16_t UI_TRANSITION_MS = 220;

/// A volume change takes over the whole screen for this long, then the carousel
/// resumes where it was.
static const uint16_t UI_VOLUME_POPUP_MS = 1400;

/// Connect / disconnect / new-track toasts.
static const uint16_t UI_TOAST_MS = 2200;

/// Idle handling. After UI_DIM_AFTER_MS with no music, no button and no
/// Bluetooth event the panel drops to UI_BRIGHT_DIM; after UI_SLEEP_AFTER_MS it
/// switches to the bouncing screensaver, which keeps the lit pixels moving.
/// Any event (connect, track, volume, button) wakes it instantly.
static const uint32_t UI_DIM_AFTER_MS = 90000;
static const uint32_t UI_SLEEP_AFTER_MS = 300000;

// ----------------------------------------------------------------- button ----

/// GPIO0 is the BOOT button that is already on the dev board -- no wiring, no
/// extra part. It only means anything to the bootloader while RESET is held, so
/// using it at runtime is free. Set to -1 to disable.
///
///   short press (< 600 ms)   next screen
///   long press  (> 600 ms)   pause / resume the carousel on this screen
///   hold        (> 2500 ms)  cycle brightness: low -> mid -> high
static const int PIN_UI_BUTTON = 0;

/// One button, escalating by how long you hold it. Each tier is far enough from
/// the next to be told apart by feel:
///
///   release < 600 ms      next screen (or confirm a pending mode switch)
///   release < 1.5 s       pin the current screen / release it
///   held 1.5 s            brightness: low -> mid -> high
///   held 3 s              offer to switch radio mode; release, then one short
///                         press confirms it
///   held 6 s              factory reset countdown, 5 s, release cancels
static const uint16_t UI_BTN_LONG_MS = 600;
static const uint16_t UI_BTN_HOLD_MS = 1500;
static const uint16_t UI_BTN_MODE_MS = 3000;

/// How long the "press again to confirm" offer stays on screen after you let
/// go. Long enough to read it, short enough that a stray press later does not
/// tip the speaker into the other mode.
static const uint16_t UI_BTN_MODE_CONFIRM_MS = 8000;

/// The factory reset countdown is deliberately long and loud: it throws away
/// Wi-Fi credentials, the dashboard password and every Bluetooth bond.
static const uint16_t UI_BTN_RESET_ARM_MS = 6000;
static const uint16_t UI_BTN_RESET_COUNT_MS = 5000;

// -------------------------------------------------------------- analyser ----

/// FFT size, in samples of the decimated (22.05 kHz) mono signal. 256 gives
/// 86 Hz per bin over 0-11 kHz, which is the useful range for a 32-bar display
/// and costs ~5 KB of RAM and a few hundred microseconds per frame. Must be a
/// power of two; 512 doubles both the resolution and the cost.
static const uint16_t FFT_SIZE = 256;

/// Bars in the spectrum displays. 32 bars x (3 px bar + 1 px gap) = 128 px, so
/// they fill the panel exactly. Changing this changes the bar geometry too.
static const uint8_t VIS_BANDS = 32;

/// Dynamic range of the bars, in dB below the auto-gain ceiling. 42 dB shows
/// quiet detail; 30 dB is punchier and calmer.
static const float VIS_RANGE_DB = 42.0f;

/// How fast a bar falls back, in fractions of full height per second. Bars rise
/// instantly (that is what makes an analyser feel responsive) and fall at this
/// rate; the peak caps fall at the slower rate after hanging for PEAK_HANG_MS.
static const float VIS_FALL_PER_S = 2.2f;
static const float VIS_PEAK_FALL_PER_S = 0.55f;
static const uint16_t VIS_PEAK_HANG_MS = 420;

/// The auto-gain that keeps the bars using the full height whatever the phone's
/// volume slider is doing. It tracks the loudest recent band, rises with the
/// music instantly and releases over this many seconds.
static const float VIS_AGC_RELEASE_S = 3.0f;

// ------------------------------------------------------------------ clock ----

/// There is no built-in RTC, so the clock is a software one. It is seeded at
/// boot from the build timestamp (so it is never wildly wrong), and can be set
/// exactly from the dashboard or the three sources below -- see soft_clock.h:
///
///   1. a DS3231 module on the same two I2C wires as this display, GPIO21/22 --
///      on by default (-DUSE_DS3231=1), and the only source that survives a
///      power cut
///   2. network time, automatically, whenever the speaker is on Wi-Fi in
///      management mode -- nothing to configure
///   3. over the serial monitor:  time 2026-08-18 14:30:00
///
/// Whatever the source, the time is written to NVS every 10 minutes, so a
/// reboot or a power cut comes back within minutes rather than back to 1970.

/// 24-hour clock (1) or 12-hour with AM/PM (0).
#ifndef CLOCK_24H
#define CLOCK_24H 1
#endif

/// Minutes east of UTC, used to turn network (UTC) time into local time.
/// 0 = UTC, -300 = US Eastern (EST), 330 = IST.
///
/// This is only the starting value. The dashboard's "Sync browser time" button
/// sends the browser's own offset, and that is what gets stored and used from
/// then on -- so the default only has to be right for a speaker that is never
/// opened in a browser.
static const int32_t CLOCK_TZ_OFFSET_MIN = 0;
