/*
 * hw_config.h -- the pins and knobs for the two hardware blocks that are not
 * part of the audio path: the DFPlayer Mini and the battery gauge.
 *
 * Kept out of main.cpp for the same reason ui_config.h is: these are wiring
 * decisions, they get changed per board, and hunting for them inside a 1000
 * line file is how a rewire turns into a debugging session. Everything here is
 * overridable from build_flags, so a variant board needs no source edit:
 *
 *     build_flags = -DPIN_DF_BUSY=34 -DPIN_BATTERY_SENSE=-1
 *
 * ---------------------------------------------------------------------------
 * DFPlayer Mini (MP3-TF-16P, YX5200-24SS) -> ESP32
 * ---------------------------------------------------------------------------
 * The module is a complete MP3 player: it holds the microSD card, decodes the
 * file itself, and hands out *analog* stereo on DAC_R / DAC_L. It is not an I2S
 * source, so it does not and cannot feed the PCM5102A -- see the note under
 * "Audio wiring" below, which is the one part of this that needs thinking about
 * rather than just connecting.
 *
 *   Module pin        ESP32              Notes
 *   ----------------  -----------------  --------------------------------------
 *   1  VCC            5V (VIN)           4.0-5.0 V. 3.3 V works but is quiet
 *                                        and browns out on card access.
 *   7,10 GND          GND                both, and share ground with the DAC
 *   2  RX             GPIO17 via 1k      1k series resistor is not optional in
 *                                        practice: without it the module picks
 *                                        up switching noise and answers frames
 *                                        that were never sent.
 *   3  TX             GPIO16             3.3 V logic, connect directly
 *   4  DAC_R          output stage R     line level, ~1 Vrms
 *   5  DAC_L          output stage L
 *   6  SPK2           not connected      the on-board 3 W amp is unused
 *   8  SPK1           not connected
 *   9  IO1            GPIO32             module's own "previous" button input
 *   11 IO2            GPIO33             module's own "next" button input
 *   12 ADKEY1         GPIO14             ADC key bank 1 (track 1 when grounded)
 *   13 ADKEY2         GPIO27             ADC key bank 2 (track 11 when grounded)
 *   14 USB+  (D+)     USB socket D+      see "USB" below
 *   15 USB-  (D-)     USB socket D-
 *   16 BUSY           GPIO35             LOW while a file is playing
 *
 * IO1/IO2/ADKEY1/ADKEY2 are *inputs on the module*, held high by its own
 * pull-ups, and acted on when pulled to ground. The firmware drives them
 * open-drain -- INPUT (high impedance) at rest, OUTPUT LOW for the length of a
 * press -- so the module's buttons still work if you also wire real ones, and
 * nothing fights over the line. See df_player_pulse().
 *
 * Audio wiring. DAC_L/DAC_R are analog line outputs; the PCM5102A's input is
 * digital I2S. Nothing can turn one into the other, so the DFPlayer joins the
 * signal chain *after* the PCM5102A, at the output jack:
 *
 *     DFPlayer DAC_L --||-- 10k --+
 *                     10uF        |
 *     PCM5102A  LOUT ---- 10k ----+---- jack tip (left)
 *
 * and the same for the right channel. The series resistors make it a passive
 * summing node, and the 10 uF blocks the DFPlayer's DC bias (its outputs sit at
 * about half its supply, which would otherwise push DC into whatever is
 * downstream). This is safe precisely because the radio modes are mutually
 * exclusive: DFPlayer mode never starts the A2DP sink or the network player, so
 * only one of the two sources is ever producing anything. The other one
 * contributes its output impedance and nothing else.
 *
 * USB. Pins 14/15 are the module's USB data lines and they do two different
 * jobs depending on what is on the other end:
 *
 *   a USB flash drive   the module is the host and plays from the stick. Wire a
 *                       USB-A socket: D+ to pin 14, D- to pin 15, and 5 V/GND
 *                       from the same supply as VCC. Select it from the
 *                       dashboard (source "USB") and it appears as a second
 *                       library alongside the card.
 *   a computer          the module becomes a card reader and the microSD shows
 *                       up as a mass storage volume, which is how the card gets
 *                       loaded without taking it out. Wire a USB-B/micro-B
 *                       socket the same way. The module tells us over the
 *                       serial link when this happens (0x3A/0x3B with device 4)
 *                       and the dashboard says so; the card belongs to the
 *                       computer while it is plugged in, so playback from the
 *                       card stops until it is unplugged.
 *
 * One socket can do both if it is an OTG-style connector, but two sockets wired
 * in parallel is simpler and is what the wiring above assumes. Do not plug both
 * in at once.
 *
 * ---------------------------------------------------------------------------
 * Battery gauge
 * ---------------------------------------------------------------------------
 * A single 18650 / LiPo cell through a divider into ADC1, plus the two status
 * pins a TP4056 charger board brings out.
 *
 *   BAT+ --- 100k ---+--- 100k --- GND
 *                    |
 *                 GPIO34 (ADC1_CH6)
 *
 * ADC1 specifically: ADC2 shares hardware with the Wi-Fi radio and reads
 * garbage whenever the driver is up, which in this firmware is nearly always.
 * That leaves GPIO32-39, and 34/35/36/39 are input-only, which is exactly what
 * a sense pin wants. 100k/100k halves 4.2 V to 2.1 V, inside the ~2.45 V the
 * 11 dB attenuator can read.
 *
 * The charger's CHRG and STDBY pins are open-drain and pull low; GPIO34-39 have
 * no internal pull-ups, so each needs a 10k to 3V3 of its own. They are
 * optional and default to off -- without them the voltage and the percentage
 * are still right, the firmware just cannot tell charging from resting and says
 * so instead of guessing.
 */

#pragma once

#include <stdint.h>

// =========================================================== DFPlayer Mini ===

/// Compile the DFPlayer driver in at all. 0 removes the mode, the API and the
/// dashboard page; the rest of the firmware is unchanged.
#ifndef DFPLAYER_ENABLED
#define DFPLAYER_ENABLED 1
#endif

/// UART2. Any two free pins work -- the ESP32's UARTs are on the GPIO matrix --
/// but 16/17 are the historical UART2 pair and are free on this board.
#ifndef PIN_DF_TX
#define PIN_DF_TX 17  // ESP32 out -> module RX (through 1k)
#endif
#ifndef PIN_DF_RX
#define PIN_DF_RX 16  // ESP32 in  <- module TX
#endif

/// 9600 8N1 is the only rate the YX5200 speaks. Listed because it looks like a
/// choice and is not.
static const uint32_t DF_BAUD = 9600;

/// BUSY, LOW while a file plays. Set -1 if it is not wired: the driver then
/// relies on the module's status query alone, which is a second slower to
/// notice a track ending and cannot see the gap between two tracks at all.
#ifndef PIN_DF_BUSY
#define PIN_DF_BUSY 35
#endif

/// The module's own button and ADC-key inputs, driven open-drain. -1 disables
/// one without affecting the others.
#ifndef PIN_DF_IO1
#define PIN_DF_IO1 32
#endif
#ifndef PIN_DF_IO2
#define PIN_DF_IO2 33
#endif
#ifndef PIN_DF_ADKEY1
#define PIN_DF_ADKEY1 14
#endif
#ifndef PIN_DF_ADKEY2
#define PIN_DF_ADKEY2 27
#endif

/// An LED that follows what the DFPlayer is doing, separate from the firmware's
/// own status LED on GPIO2 (which has the radio and the update to talk about).
/// -1 if you have not fitted one. The dashboard can force it on, off, or leave
/// it mirroring BUSY.
#ifndef PIN_DF_LED
#define PIN_DF_LED 4
#endif
#ifndef DF_LED_ACTIVE_HIGH
#define DF_LED_ACTIVE_HIGH 1
#endif

/// Optional VBUS sense on the DFPlayer's USB socket, through a divider, so a
/// computer being plugged in is visible even before the module reports it.
/// Off by default: an input-only pin with nothing on it floats, and a floating
/// pin invents events. -1 unless you wired it.
#ifndef PIN_DF_USB_DETECT
#define PIN_DF_USB_DETECT -1
#endif

/// How long a pulse on IO1/IO2 lasts. The module reads a short press as
/// previous/next and a long one as volume down/up, and the boundary in its
/// firmware is around a second.
static const uint16_t DF_PRESS_SHORT_MS = 150;
static const uint16_t DF_PRESS_LONG_MS = 1400;

/// ADKEY pulses only have one meaning, so one length.
static const uint16_t DF_ADKEY_PRESS_MS = 200;

/// Minimum gap between two frames on the wire. The YX5200 drops commands that
/// arrive while it is still acting on the last one, and answers 0x40/0x01
/// ("module busy") when it notices. 40 ms is comfortable; below ~25 ms the
/// module starts missing volume steps.
static const uint16_t DF_COMMAND_GAP_MS = 40;

/// How often the driver asks the module what it is doing. Every poll is two
/// frames on a 9600 baud link, so this is not free -- but it is the only way to
/// see a change made with the physical buttons.
static const uint16_t DF_POLL_MS = 900;

/// A reset takes the module about 1.5 s to come back from, and it ignores
/// everything in between. Nothing is sent until this has elapsed.
static const uint16_t DF_RESET_SETTLE_MS = 2000;

/// Give up on the module if it has not answered a single frame in this long.
/// Reported as "offline" rather than retried forever: the usual cause is TX and
/// RX swapped, and no amount of waiting fixes that.
static const uint32_t DF_ONLINE_TIMEOUT_MS = 6000;

/// Volume range of the module. Fixed by its firmware; the dashboard and the
/// OLED work in 0..127 like the other sources and the driver converts.
static const uint8_t DF_VOLUME_MAX = 30;

/// Where the volume starts before anything is stored, on the module's scale.
static const uint8_t DF_VOLUME_DEFAULT = 20;

// ============================================================ Battery gauge ===

/// Compile the gauge in at all.
#ifndef BATTERY_ENABLED
#define BATTERY_ENABLED 1
#endif

/// ADC1 pin the divider feeds. -1 disables the gauge outright, and the dashboard
/// then says the battery is not wired rather than showing 0%.
///
/// The pin is configured here but the *gauge* is off until it is switched on
/// under Settings > Battery, and that default is deliberate rather than
/// cautious. A sense pin with no divider on it floats, a floating input invents
/// readings, and one that happens to settle inside a cell's voltage window
/// would have the speaker insisting on a critically flat battery -- flashing the
/// status LED about it, in every mode, on a board with no battery in it. So the
/// firmware never assumes a pack is fitted: fit one, tick the box, and it reads
/// from then on. Nothing has to be rebuilt.
#ifndef PIN_BATTERY_SENSE
#define PIN_BATTERY_SENSE 34
#endif

/// TP4056 status pins, both active low and both needing their own 10k pull-up
/// to 3V3. CHRG is low while charging; STDBY is low once the charge terminated.
/// Without them the state is reported as unknown -- see the header note.
#ifndef PIN_BATTERY_CHARGING
#define PIN_BATTERY_CHARGING -1
#endif
#ifndef PIN_BATTERY_FULL
#define PIN_BATTERY_FULL -1
#endif
#ifndef BATTERY_STAT_ACTIVE_LOW
#define BATTERY_STAT_ACTIVE_LOW 1
#endif

/// Divider ratio: cell volts per volt at the pin. 2.0 for the 100k/100k above.
/// Stored in NVS and adjustable from the dashboard, so this is only the value a
/// factory-fresh speaker starts with.
static const float BATTERY_DIVIDER_DEFAULT = 2.0f;

/// Multiplicative trim on top of the divider, for when the resistors are 5%
/// parts and the reading is 60 mV out. 1.0 = no correction. The dashboard can
/// compute it for you from a meter reading.
static const float BATTERY_CALIBRATION_DEFAULT = 1.0f;

/// The cell's own limits, used for the percentage curve. Defaults are a single
/// Li-ion/LiPo cell; a 2S pack is two of everything and a divider of 4.
static const float BATTERY_FULL_V_DEFAULT = 4.20f;
static const float BATTERY_EMPTY_V_DEFAULT = 3.30f;

/// Percentages at which the speaker starts saying something about it: a warning
/// on the display and the dashboard at `low`, an urgent LED pattern at
/// `critical`. Nothing is switched off at either -- see README.
static const uint8_t BATTERY_LOW_PCT_DEFAULT = 20;
static const uint8_t BATTERY_CRITICAL_PCT_DEFAULT = 7;

/// Sample period. The cell moves in minutes, not milliseconds, so this is set
/// by how fast the display should react to a charger being plugged in.
static const uint16_t BATTERY_SAMPLE_MS = 500;

/// Samples per reading, median-filtered. The ESP32's SAR ADC is noisy enough
/// that a single conversion wanders 30-40 mV, which is a whole percent.
static const uint8_t BATTERY_OVERSAMPLE = 9;

/// Smoothing on the filtered result, 0..1 per sample. Low enough that the
/// current spike from a card access does not move the percentage.
static const float BATTERY_SMOOTHING = 0.12f;

/// The window a real pack can occupy, per cell. A reading below the floor is not
/// a discharged cell, it is no cell at all (or no divider); one above the
/// ceiling is not a cell either. Both bounds matter: a sense pin with nothing on
/// it is a floating high-impedance input, and the numbers it invents are as
/// likely to land above a charged pack as below an empty one.
static const float BATTERY_MIN_PLAUSIBLE_V = 2.20f;
static const float BATTERY_MAX_PLAUSIBLE_V = 4.45f;
