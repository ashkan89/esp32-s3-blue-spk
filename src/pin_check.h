/*
 * pin_check.h -- the pin map in one place, checked by the compiler.
 *
 * Every GPIO this firmware uses is declared in one of three files: main.cpp for
 * the I2S pins and the status LED, ui_config.h for I2C and the button, and
 * hw_config.h for the DFPlayer, the battery gauge and the WS2812 ring. That
 * split is deliberate -- each list sits next to the wiring notes it belongs to
 * -- but it means nothing was ever looking at all of them at once, and the two
 * mistakes a rewire actually produces are exactly the ones you only see from
 * there: two peripherals given the same pin, and a pin that cannot do the job
 * asked of it.
 *
 * So this header includes all three and asserts the things a classic ESP32
 * cares about. It contains no code and costs no flash; a violation is a build
 * error naming the pin, which is the cheapest possible moment to find out.
 *
 * ---------------------------------------------------------------------------
 * The rules, and why each one is here
 * ---------------------------------------------------------------------------
 *
 *   GPIO 6-11    wired to the module's SPI flash on every WROOM-32/32D/32E.
 *                Driving one does not misbehave subtly: the chip stops
 *                executing, because that is where the code is being fetched
 *                from. There is no configuration that makes these usable.
 *
 *   GPIO 20,24   not bonded out on the ESP32-D0WD die inside a WROOM module.
 *   GPIO 28-31   Configuring them succeeds and nothing ever happens.
 *
 *   GPIO 34-39   input only. No output driver and no internal pull-up or
 *                pull-down at all -- pinMode(OUTPUT) is silently ignored, so a
 *                signal assigned here simply never appears on the wire. Fine
 *                for the battery divider and the DFPlayer's BUSY output;
 *                useless for anything this firmware has to drive.
 *
 *   GPIO 0,2     strapping pins, sampled on every system reset (not only at
 *   GPIO 5,12,15 power-on -- esp_restart() re-latches them too). This build
 *                uses GPIO0 for the BOOT button and GPIO2 for the on-board LED,
 *                which is what a devkit already wires and is safe because
 *                neither is driven low while a reset is being taken; see the
 *                release waits in main.cpp's factory reset and in
 *                power_sleep_now(). GPIO12 is the one to stay away from: it
 *                selects the flash voltage at boot, and holding it high on a
 *                3.3 V module can stop the board booting at all.
 *
 *   GPIO 1,3     UART0, the programming and console port. Using either ends the
 *                serial log and, on GPIO3, can hold the chip in reset from the
 *                auto-reset circuit on a devkit.
 *
 * The strapping and UART0 rules are *warnings* rather than errors, because a
 * variant board may genuinely have them free and this firmware has no way to
 * know. Everything above them is an error: there is no board on which those are
 * a working choice.
 *
 * Nothing here changes a pin. If an assertion fires, the fix is in the file
 * that declares the pin -- or a -D override in platformio.ini, which every one
 * of them honours.
 */

#pragma once

#include "hw_config.h"
#include "ui_config.h"

/*
 * The pins that used to live in main.cpp, because a .cpp is not somewhere the
 * rest of the build can look.
 *
 * This header is now their single definition: main.cpp reads PIN_MAP_I2S_* for
 * the I2S driver, status_led gets PIN_STATUS_LED through main.cpp, and
 * diagnostics.cpp prints all of them. Everything is still overridable from
 * build_flags exactly as before, and because there is only one copy there is
 * nothing for a second copy to drift away from -- which was the point of
 * gathering the map here in the first place.
 *
 *   BCK -> GPIO26   LCK -> GPIO25   DIN -> GPIO23
 *
 * GPIO23 for the data line rather than the GPIO22 most I2S examples use: the
 * OLED and the DS3231 own the canonical I2C pair on 21/22. The static_assert
 * further down is what now enforces that, rather than a comment.
 */
#ifndef PIN_MAP_I2S_BCLK
#define PIN_MAP_I2S_BCLK 26
#endif
#ifndef PIN_MAP_I2S_LRCK
#define PIN_MAP_I2S_LRCK 25
#endif
#ifndef PIN_MAP_I2S_DOUT
#define PIN_MAP_I2S_DOUT 23
#endif

/// The on-board LED of most WROOM-32D devkits, and the only indicator the board
/// has. Override either from build_flags for a variant that wires it the other
/// way round or brings it out elsewhere.
#ifndef PIN_STATUS_LED
#define PIN_STATUS_LED 2
#endif
#ifndef STATUS_LED_ACTIVE_HIGH
#define STATUS_LED_ACTIVE_HIGH 1
#endif

// =========================================================== the predicates ==
// Written as macros rather than constexpr functions so they can be used from
// #if for the warnings as well as from static_assert for the errors.

/// True for a pin number that exists at all on an ESP32-D0WD in a WROOM module.
#define PIN_EXISTS(p) \
  ((p) >= 0 && (p) <= 39 && !((p) >= 28 && (p) <= 31) && (p) != 20 && (p) != 24)

/// True for the six pins the module's SPI flash occupies.
#define PIN_IS_FLASH(p) ((p) >= 6 && (p) <= 11)

/// True for the input-only pins, which have no output driver.
#define PIN_IS_INPUT_ONLY(p) ((p) >= 34 && (p) <= 39)

/// True for ADC1 channels -- the only ADC usable while Wi-Fi is up.
#define PIN_IS_ADC1(p) (((p) >= 32 && (p) <= 39))

/// A pin that is switched off with -1 passes everything.
#define PIN_OFF(p) ((p) < 0)

/// The two shapes every check below takes.
#define PIN_OK_ANY(p) (PIN_OFF(p) || (PIN_EXISTS(p) && !PIN_IS_FLASH(p)))
#define PIN_OK_OUTPUT(p) (PIN_OK_ANY(p) && !PIN_IS_INPUT_ONLY(p))

/// Two pins collide when they are both in use and equal. -1 never collides.
#define PINS_DISTINCT(a, b) (PIN_OFF(a) || PIN_OFF(b) || (a) != (b))

// ================================================================= existence ==

static_assert(PIN_OK_OUTPUT(PIN_MAP_I2S_BCLK),
              "I2S BCLK is on a flash, input-only or non-existent pin");
static_assert(PIN_OK_OUTPUT(PIN_MAP_I2S_LRCK),
              "I2S LRCK is on a flash, input-only or non-existent pin");
static_assert(PIN_OK_OUTPUT(PIN_MAP_I2S_DOUT),
              "I2S DOUT is on a flash, input-only or non-existent pin");

static_assert(PIN_OK_OUTPUT(PIN_OLED_SDA),
              "I2C SDA must be able to drive; 34-39 are input only");
static_assert(PIN_OK_OUTPUT(PIN_OLED_SCL),
              "I2C SCL must be able to drive; 34-39 are input only");

static_assert(PIN_OK_OUTPUT(PIN_STATUS_LED),
              "the status LED is on a flash, input-only or non-existent pin");
static_assert(PIN_OK_ANY(PIN_UI_BUTTON),
              "the UI button is on a flash or non-existent pin");

#if LEDS_ENABLED
static_assert(PIN_OK_OUTPUT(PIN_LEDS),
              "the WS2812 data line must be able to drive; 34-39 are input only "
              "and 6-11 belong to the flash");
static_assert(LED_COUNT > 0, "LED_COUNT must be at least 1");
static_assert(LED_BRIGHTNESS_MAX >= 1 && LED_BRIGHTNESS_MAX <= 255,
              "LED_BRIGHTNESS_MAX is a 1..255 ceiling, not a percentage");
static_assert(LED_CENTRE_INDEX < LED_COUNT,
              "LED_CENTRE_INDEX points past the end of the strip");
#endif

#if DFPLAYER_ENABLED
static_assert(PIN_OK_OUTPUT(PIN_DF_TX),
              "the DFPlayer TX line must be able to drive; 34-39 are input only");
static_assert(PIN_OK_ANY(PIN_DF_RX), "the DFPlayer RX line is on an unusable pin");
static_assert(PIN_OK_ANY(PIN_DF_BUSY),
              "the DFPlayer BUSY line is on an unusable pin");
static_assert(PIN_OK_ANY(PIN_DF_USB_DETECT),
              "the DFPlayer USB-detect line is on an unusable pin");
static_assert(PIN_OK_OUTPUT(PIN_DF_LED),
              "the DFPlayer indicator LED must be able to drive");
/*
 * IO1/IO2/ADKEY1/ADKEY2 are driven open-drain -- INPUT at rest, OUTPUT LOW for
 * the length of a press -- so they need a real output driver. An input-only pin
 * here fails silently and in the most confusing possible way: the code runs,
 * the log says the press was sent, and the module never sees a thing.
 */
static_assert(PIN_OK_OUTPUT(PIN_DF_IO1),
              "DFPlayer IO1 is driven low to press it; 34-39 cannot drive");
static_assert(PIN_OK_OUTPUT(PIN_DF_IO2),
              "DFPlayer IO2 is driven low to press it; 34-39 cannot drive");
static_assert(PIN_OK_OUTPUT(PIN_DF_ADKEY1),
              "DFPlayer ADKEY1 is driven low to press it; 34-39 cannot drive");
static_assert(PIN_OK_OUTPUT(PIN_DF_ADKEY2),
              "DFPlayer ADKEY2 is driven low to press it; 34-39 cannot drive");
#endif

#if BATTERY_ENABLED
static_assert(PIN_OK_ANY(PIN_BATTERY_SENSE),
              "the battery sense pin is on a flash or non-existent pin");
/*
 * ADC1, not ADC2, and this is not a preference.
 *
 * ADC2 shares its hardware with the Wi-Fi radio. adc2_get_raw() returns
 * ESP_ERR_TIMEOUT for as long as the driver holds the lock, which in four of
 * this firmware's five radio modes is essentially always -- and the Arduino
 * wrapper does not report that, it returns 0. A gauge on ADC2 would read a flat
 * battery whenever Wi-Fi was up and a correct one whenever it was not.
 */
static_assert(PIN_OFF(PIN_BATTERY_SENSE) || PIN_IS_ADC1(PIN_BATTERY_SENSE),
              "the battery sense pin must be on ADC1 (GPIO32-39): ADC2 stops "
              "reading whenever the Wi-Fi driver is up");
static_assert(PIN_OK_ANY(PIN_BATTERY_CHARGING),
              "the TP4056 CHRG pin is on a flash or non-existent pin");
static_assert(PIN_OK_ANY(PIN_BATTERY_FULL),
              "the TP4056 STDBY pin is on a flash or non-existent pin");
static_assert(BATTERY_MIN_PLAUSIBLE_V < BATTERY_MAX_PLAUSIBLE_V,
              "the plausible-cell window is inside out");
static_assert(BATTERY_EMPTY_V_DEFAULT < BATTERY_FULL_V_DEFAULT,
              "the default empty voltage is above the default full voltage");
#endif

// ================================================================ collisions ==
/*
 * Every pair that could plausibly be typed the same by accident. Two signals
 * cannot share a pin: the second peripheral to be configured wins the GPIO
 * matrix and the first one goes quiet, with nothing logged anywhere.
 *
 * The list is written out rather than generated because a static_assert has to
 * name what went wrong to be worth having, and "PIN_I2S_DOUT and PIN_OLED_SCL
 * are the same pin" is a sentence somebody can act on.
 */
#define PIN_CLASH_MSG(a, b) "GPIO conflict: " a " and " b " are the same pin"

static_assert(PINS_DISTINCT(PIN_MAP_I2S_BCLK, PIN_MAP_I2S_LRCK),
              PIN_CLASH_MSG("I2S BCLK", "I2S LRCK"));
static_assert(PINS_DISTINCT(PIN_MAP_I2S_BCLK, PIN_MAP_I2S_DOUT),
              PIN_CLASH_MSG("I2S BCLK", "I2S DOUT"));
static_assert(PINS_DISTINCT(PIN_MAP_I2S_LRCK, PIN_MAP_I2S_DOUT),
              PIN_CLASH_MSG("I2S LRCK", "I2S DOUT"));
static_assert(PINS_DISTINCT(PIN_OLED_SDA, PIN_OLED_SCL),
              PIN_CLASH_MSG("I2C SDA", "I2C SCL"));

// I2S against I2C -- the one that actually happened, and the reason
// PIN_I2S_DOUT is GPIO23 rather than the GPIO22 every I2S example uses.
static_assert(PINS_DISTINCT(PIN_MAP_I2S_DOUT, PIN_OLED_SDA),
              PIN_CLASH_MSG("I2S DOUT", "I2C SDA"));
static_assert(PINS_DISTINCT(PIN_MAP_I2S_DOUT, PIN_OLED_SCL),
              PIN_CLASH_MSG("I2S DOUT", "I2C SCL"));
static_assert(PINS_DISTINCT(PIN_MAP_I2S_BCLK, PIN_OLED_SDA),
              PIN_CLASH_MSG("I2S BCLK", "I2C SDA"));
static_assert(PINS_DISTINCT(PIN_MAP_I2S_BCLK, PIN_OLED_SCL),
              PIN_CLASH_MSG("I2S BCLK", "I2C SCL"));
static_assert(PINS_DISTINCT(PIN_MAP_I2S_LRCK, PIN_OLED_SDA),
              PIN_CLASH_MSG("I2S LRCK", "I2C SDA"));
static_assert(PINS_DISTINCT(PIN_MAP_I2S_LRCK, PIN_OLED_SCL),
              PIN_CLASH_MSG("I2S LRCK", "I2C SCL"));

// The two pins the board itself provides.
static_assert(PINS_DISTINCT(PIN_STATUS_LED, PIN_UI_BUTTON),
              PIN_CLASH_MSG("the status LED", "the UI button"));
static_assert(PINS_DISTINCT(PIN_STATUS_LED, PIN_MAP_I2S_BCLK),
              PIN_CLASH_MSG("the status LED", "I2S BCLK"));
static_assert(PINS_DISTINCT(PIN_STATUS_LED, PIN_MAP_I2S_LRCK),
              PIN_CLASH_MSG("the status LED", "I2S LRCK"));
static_assert(PINS_DISTINCT(PIN_STATUS_LED, PIN_MAP_I2S_DOUT),
              PIN_CLASH_MSG("the status LED", "I2S DOUT"));
static_assert(PINS_DISTINCT(PIN_STATUS_LED, PIN_OLED_SDA),
              PIN_CLASH_MSG("the status LED", "I2C SDA"));
static_assert(PINS_DISTINCT(PIN_STATUS_LED, PIN_OLED_SCL),
              PIN_CLASH_MSG("the status LED", "I2C SCL"));
static_assert(PINS_DISTINCT(PIN_UI_BUTTON, PIN_OLED_SDA),
              PIN_CLASH_MSG("the UI button", "I2C SDA"));
static_assert(PINS_DISTINCT(PIN_UI_BUTTON, PIN_OLED_SCL),
              PIN_CLASH_MSG("the UI button", "I2C SCL"));

#if LEDS_ENABLED
static_assert(PINS_DISTINCT(PIN_LEDS, PIN_MAP_I2S_BCLK),
              PIN_CLASH_MSG("the WS2812 data line", "I2S BCLK"));
static_assert(PINS_DISTINCT(PIN_LEDS, PIN_MAP_I2S_LRCK),
              PIN_CLASH_MSG("the WS2812 data line", "I2S LRCK"));
static_assert(PINS_DISTINCT(PIN_LEDS, PIN_MAP_I2S_DOUT),
              PIN_CLASH_MSG("the WS2812 data line", "I2S DOUT"));
static_assert(PINS_DISTINCT(PIN_LEDS, PIN_OLED_SDA),
              PIN_CLASH_MSG("the WS2812 data line", "I2C SDA"));
static_assert(PINS_DISTINCT(PIN_LEDS, PIN_OLED_SCL),
              PIN_CLASH_MSG("the WS2812 data line", "I2C SCL"));
static_assert(PINS_DISTINCT(PIN_LEDS, PIN_STATUS_LED),
              PIN_CLASH_MSG("the WS2812 data line", "the status LED"));
static_assert(PINS_DISTINCT(PIN_LEDS, PIN_UI_BUTTON),
              PIN_CLASH_MSG("the WS2812 data line", "the UI button"));
#endif

#if DFPLAYER_ENABLED
static_assert(PINS_DISTINCT(PIN_DF_TX, PIN_DF_RX),
              PIN_CLASH_MSG("DFPlayer TX", "DFPlayer RX"));
static_assert(PINS_DISTINCT(PIN_DF_TX, PIN_MAP_I2S_DOUT),
              PIN_CLASH_MSG("DFPlayer TX", "I2S DOUT"));
static_assert(PINS_DISTINCT(PIN_DF_RX, PIN_MAP_I2S_DOUT),
              PIN_CLASH_MSG("DFPlayer RX", "I2S DOUT"));
static_assert(PINS_DISTINCT(PIN_DF_TX, PIN_OLED_SDA),
              PIN_CLASH_MSG("DFPlayer TX", "I2C SDA"));
static_assert(PINS_DISTINCT(PIN_DF_RX, PIN_OLED_SCL),
              PIN_CLASH_MSG("DFPlayer RX", "I2C SCL"));
static_assert(PINS_DISTINCT(PIN_DF_BUSY, PIN_DF_IO1),
              PIN_CLASH_MSG("DFPlayer BUSY", "DFPlayer IO1"));
static_assert(PINS_DISTINCT(PIN_DF_BUSY, PIN_DF_IO2),
              PIN_CLASH_MSG("DFPlayer BUSY", "DFPlayer IO2"));
static_assert(PINS_DISTINCT(PIN_DF_IO1, PIN_DF_IO2),
              PIN_CLASH_MSG("DFPlayer IO1", "DFPlayer IO2"));
static_assert(PINS_DISTINCT(PIN_DF_ADKEY1, PIN_DF_ADKEY2),
              PIN_CLASH_MSG("DFPlayer ADKEY1", "DFPlayer ADKEY2"));
static_assert(PINS_DISTINCT(PIN_DF_IO1, PIN_DF_ADKEY1),
              PIN_CLASH_MSG("DFPlayer IO1", "DFPlayer ADKEY1"));
static_assert(PINS_DISTINCT(PIN_DF_IO1, PIN_DF_ADKEY2),
              PIN_CLASH_MSG("DFPlayer IO1", "DFPlayer ADKEY2"));
static_assert(PINS_DISTINCT(PIN_DF_IO2, PIN_DF_ADKEY1),
              PIN_CLASH_MSG("DFPlayer IO2", "DFPlayer ADKEY1"));
static_assert(PINS_DISTINCT(PIN_DF_IO2, PIN_DF_ADKEY2),
              PIN_CLASH_MSG("DFPlayer IO2", "DFPlayer ADKEY2"));
static_assert(PINS_DISTINCT(PIN_DF_LED, PIN_STATUS_LED),
              PIN_CLASH_MSG("the DFPlayer indicator LED", "the status LED"));
#if LEDS_ENABLED
static_assert(PINS_DISTINCT(PIN_DF_TX, PIN_LEDS),
              PIN_CLASH_MSG("DFPlayer TX", "the WS2812 data line"));
static_assert(PINS_DISTINCT(PIN_DF_RX, PIN_LEDS),
              PIN_CLASH_MSG("DFPlayer RX", "the WS2812 data line"));
static_assert(PINS_DISTINCT(PIN_DF_LED, PIN_LEDS),
              PIN_CLASH_MSG("the DFPlayer indicator LED", "the WS2812 data line"));
static_assert(PINS_DISTINCT(PIN_DF_IO1, PIN_LEDS),
              PIN_CLASH_MSG("DFPlayer IO1", "the WS2812 data line"));
static_assert(PINS_DISTINCT(PIN_DF_IO2, PIN_LEDS),
              PIN_CLASH_MSG("DFPlayer IO2", "the WS2812 data line"));
static_assert(PINS_DISTINCT(PIN_DF_ADKEY1, PIN_LEDS),
              PIN_CLASH_MSG("DFPlayer ADKEY1", "the WS2812 data line"));
static_assert(PINS_DISTINCT(PIN_DF_ADKEY2, PIN_LEDS),
              PIN_CLASH_MSG("DFPlayer ADKEY2", "the WS2812 data line"));
#endif
#endif

#if DFPLAYER_ENABLED && BATTERY_ENABLED
static_assert(PINS_DISTINCT(PIN_BATTERY_SENSE, PIN_DF_BUSY),
              PIN_CLASH_MSG("the battery sense pin", "DFPlayer BUSY"));
static_assert(PINS_DISTINCT(PIN_BATTERY_SENSE, PIN_DF_IO1),
              PIN_CLASH_MSG("the battery sense pin", "DFPlayer IO1"));
static_assert(PINS_DISTINCT(PIN_BATTERY_SENSE, PIN_DF_IO2),
              PIN_CLASH_MSG("the battery sense pin", "DFPlayer IO2"));
static_assert(PINS_DISTINCT(PIN_BATTERY_SENSE, PIN_DF_USB_DETECT),
              PIN_CLASH_MSG("the battery sense pin", "the DFPlayer USB detect"));
static_assert(PINS_DISTINCT(PIN_BATTERY_CHARGING, PIN_DF_BUSY),
              PIN_CLASH_MSG("the TP4056 CHRG pin", "DFPlayer BUSY"));
static_assert(PINS_DISTINCT(PIN_BATTERY_FULL, PIN_DF_BUSY),
              PIN_CLASH_MSG("the TP4056 STDBY pin", "DFPlayer BUSY"));
#endif

#if BATTERY_ENABLED
static_assert(PINS_DISTINCT(PIN_BATTERY_SENSE, PIN_BATTERY_CHARGING),
              PIN_CLASH_MSG("the battery sense pin", "the TP4056 CHRG pin"));
static_assert(PINS_DISTINCT(PIN_BATTERY_SENSE, PIN_BATTERY_FULL),
              PIN_CLASH_MSG("the battery sense pin", "the TP4056 STDBY pin"));
static_assert(PINS_DISTINCT(PIN_BATTERY_CHARGING, PIN_BATTERY_FULL),
              PIN_CLASH_MSG("the TP4056 CHRG pin", "the TP4056 STDBY pin"));
#endif

// ============================================================== soft advice ==
/*
 * Warnings, not errors. Each of these is a pin that *works*, on a board where
 * you know what else is attached to it -- which this firmware does not. They
 * are worth one line at build time and nothing more.
 *
 * GPIO12 is the exception that is nearly an error: MTDI selects the flash
 * regulator voltage at reset, and a module whose flash runs at 3.3 V will not
 * boot at all if it is held high. It is never used by default here.
 */
#define PIN_IS_STRAP(p) ((p) == 0 || (p) == 2 || (p) == 5 || (p) == 12 || (p) == 15)
#define PIN_IS_UART0(p) ((p) == 1 || (p) == 3)

#if PIN_IS_UART0(PIN_MAP_I2S_BCLK) || PIN_IS_UART0(PIN_MAP_I2S_LRCK) || \
    PIN_IS_UART0(PIN_MAP_I2S_DOUT) || PIN_IS_UART0(PIN_OLED_SDA) ||     \
    PIN_IS_UART0(PIN_OLED_SCL) || PIN_IS_UART0(PIN_STATUS_LED)
#warning "A pin on UART0 (GPIO1/GPIO3) is in use: the serial console and the \
upload path share those. Expect no log, and on GPIO3 a board that can be held \
in reset by its own auto-reset circuit."
#endif

#if LEDS_ENABLED && PIN_IS_UART0(PIN_LEDS)
#warning "PIN_LEDS is on UART0 (GPIO1/GPIO3); the serial console will not work."
#endif

#if PIN_MAP_I2S_BCLK == 12 || PIN_MAP_I2S_LRCK == 12 || \
    PIN_MAP_I2S_DOUT == 12 || PIN_OLED_SDA == 12 || PIN_OLED_SCL == 12
#warning "GPIO12 (MTDI) selects the flash voltage at reset. Anything that pulls \
it high while the board comes out of reset can stop a 3.3 V module booting."
#endif

#if LEDS_ENABLED && PIN_LEDS == 12
#warning "PIN_LEDS is GPIO12 (MTDI), the flash-voltage strap. A WS2812 line \
idles low so this usually boots, but the first pixel and the bootloader are now \
sharing a decision. Move it if the board becomes unreliable at power-on."
#endif

#if DFPLAYER_ENABLED && (PIN_DF_TX == 12 || PIN_DF_IO1 == 12 ||    \
                         PIN_DF_IO2 == 12 || PIN_DF_ADKEY1 == 12 || \
                         PIN_DF_ADKEY2 == 12 || PIN_DF_LED == 12)
#warning "A DFPlayer line is on GPIO12 (MTDI), the flash-voltage strap. The \
module's pull-ups hold it high at reset, which can stop a 3.3 V module booting."
#endif

/*
 * GPIO2 is checked rather than warned about, because it is the one strapping
 * pin this firmware deliberately uses: it is the on-board LED of every WROOM
 * devkit, and it has to be low or floating at reset, which an LED to ground is.
 * Wiring it the other way round -- STATUS_LED_ACTIVE_HIGH 0, LED to 3V3 -- puts
 * a pull-up on a strapping pin and, together with GPIO0 low, selects the ROM
 * download mode instead of booting.
 */
#if PIN_STATUS_LED == 2 && !STATUS_LED_ACTIVE_HIGH
#warning "PIN_STATUS_LED is GPIO2 with STATUS_LED_ACTIVE_HIGH 0, which implies \
a pull-up on a strapping pin. With BOOT also held, the chip enters the ROM \
serial bootloader instead of running the firmware."
#endif
