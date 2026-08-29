#pragma once

#include <Arduino.h>

/*
 * The single on-board LED, used as a real status indicator.
 *
 * Everything is expressed as a 16-slot pattern clocked at 125 ms, so one full
 * cycle is two seconds and every state is distinguishable across the room
 * without counting milliseconds. Patterns are chosen so that "more light" means
 * "more settled": solid when a phone is streaming, a lone flash when the
 * speaker is idle and waiting, restless blinking when the speaker needs
 * something from you.
 *
 * Nothing here blocks or allocates. status_led_tick() is called from loop() and
 * from inside the melody player, so the indicator keeps running through the
 * half second the chimes own the CPU.
 */

enum StatusLedState : uint8_t {
  LED_BOOT = 0,        // solid: powering up
  LED_SETUP_AP,        // triple blink: setup Wi-Fi is open, come and configure
  LED_WIFI_CONNECTING, // even 500 ms blink: joining the saved network
  LED_IDLE,            // one short flash every 2 s: up and waiting
  LED_BT_CONNECTED,    // near solid with a wink: phone attached, not playing
  LED_BT_STREAMING,    // solid: audio is flowing
  LED_UPDATING,        // fast strobe: writing flash, do not remove power
  LED_FAULT,           // urgent double-double blink: something failed
  LED_NO_MEDIA,        // two slow winks: the source has nothing to play from
  LED_BATTERY_LOW,     // long-short heartbeat: charge it
  LED_STATE_COUNT
};

// pin is driven with pinMode(OUTPUT). Set active_high false for boards that
// wire the LED between 3V3 and the pin.
void status_led_begin(uint8_t pin, bool active_high);

// Sets the resting pattern. Cheap enough to call every loop.
void status_led_state(StatusLedState state);
StatusLedState status_led_state();

// Holds the indicator dark whatever pattern is set, and lets it resume where it
// was on release. Power saving uses it; nothing else should, because a status
// LED that is off is a speaker with no status on it.
void status_led_mute(bool on);
bool status_led_muted();

// Plays `pulses` fast blinks over the top of the resting pattern and then
// returns to it. Use for events worth noticing: a phone connecting, a track
// change, a setting saved. Safe to call from any task.
void status_led_blip(uint8_t pulses);

// Advances the pattern. Call as often as convenient; it is millis()-driven and
// does nothing between slots.
void status_led_tick();
