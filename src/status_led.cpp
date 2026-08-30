#include "status_led.h"

namespace {

// One bit per 125 ms slot, most significant bit first, so the literal reads
// left to right as the eye sees it over two seconds.
const uint16_t PATTERNS[LED_STATE_COUNT] = {
    0b1111111111111111,  // LED_BOOT
    0b1010100000000000,  // LED_SETUP_AP
    0b1111000011110000,  // LED_WIFI_CONNECTING
    0b1000000000000000,  // LED_IDLE
    0b1111111011111110,  // LED_BT_CONNECTED
    0b1111111111111111,  // LED_BT_STREAMING
    0b1010101010101010,  // LED_UPDATING
    0b1010001010000000,  // LED_FAULT
    0b1100110000000000,  // LED_NO_MEDIA
    0b1111100010000000,  // LED_BATTERY_LOW
};

constexpr uint32_t SLOT_MS = 125;
constexpr uint32_t BLIP_MS = 55;

uint8_t ledPin = 0xFF;
bool ledActiveHigh = true;
volatile uint8_t baseState = LED_BOOT;
volatile bool muted;

/*
 * The blip counter, and the one thing here that needs a lock.
 *
 * Everything else in this file is a single-byte store from one side and a load
 * from the other, which on this chip is atomic in the only sense that matters:
 * a reader sees the old value or the new one. `blipSlots` is different because
 * both sides read-modify-write it -- status_led_blip() raises it from the
 * Bluetooth and Wi-Fi callback tasks, status_led_tick() decrements it from the
 * Arduino loop -- and a decrement that lands between the other side's load and
 * store is simply lost. The visible cost is small (a blip one flash short, or
 * one that never ends because the decrement to zero was the one dropped) but
 * the second of those leaves the indicator stuck, and the fix is four
 * instructions inside a spinlock rather than an argument about how unlikely it
 * is.
 */
portMUX_TYPE blip_mux = portMUX_INITIALIZER_UNLOCKED;
uint8_t blipSlots;  // remaining half-cycles of the attention blink

uint32_t lastSlotAt;
uint8_t slot;
int8_t lastLevel = -1;

void drive(bool on) {
  const int8_t level = on ? 1 : 0;
  if (level == lastLevel) return;
  lastLevel = level;
  digitalWrite(ledPin, (on == ledActiveHigh) ? HIGH : LOW);
}

}  // namespace

void status_led_begin(uint8_t pin, bool active_high) {
  ledPin = pin;
  ledActiveHigh = active_high;
  lastLevel = -1;
  pinMode(ledPin, OUTPUT);
  drive(false);
  lastSlotAt = millis();
}

void status_led_state(StatusLedState state) {
  if (state >= LED_STATE_COUNT) return;
  baseState = (uint8_t)state;
}

StatusLedState status_led_state() { return (StatusLedState)baseState; }

void status_led_blip(uint8_t pulses) {
  if (!pulses) return;
  if (pulses > 6) pulses = 6;
  // Two half-cycles per pulse: on, off.
  const uint8_t slots = (uint8_t)(pulses * 2);
  portENTER_CRITICAL(&blip_mux);
  if (slots > blipSlots) blipSlots = slots;
  portEXIT_CRITICAL(&blip_mux);
}

void status_led_mute(bool on) {
  if (on == muted) return;
  muted = on;
  // Coming back, the next slot redraws from the pattern; going away, the pin is
  // taken low here rather than waiting up to 125 ms for a slot that says so.
  if (on && ledPin != 0xFF) drive(false);
}

bool status_led_muted() { return muted; }

void status_led_tick() {
  if (ledPin == 0xFF) return;
  if (muted) {
    drive(false);
    return;
  }

  const uint32_t now = millis();

  // Read and decrement in one critical section, so a blip raised from a radio
  // callback between the two cannot be overwritten by the store.
  portENTER_CRITICAL(&blip_mux);
  const uint8_t pending = blipSlots;
  const bool due = pending != 0 && (now - lastSlotAt) >= BLIP_MS;
  if (due) blipSlots = (uint8_t)(pending - 1);
  portEXIT_CRITICAL(&blip_mux);

  if (pending) {
    if (!due) return;
    lastSlotAt = now;
    const uint8_t left = (uint8_t)(pending - 1);
    // An odd count left means "on", so a blip always ends dark and the resting
    // pattern picks up from a known state.
    drive((left & 1) != 0);
    if (!left) slot = 0;
    return;
  }

  if (now - lastSlotAt < SLOT_MS) return;
  // Catch up rather than drift if a melody or an OTA write stole the CPU.
  const uint32_t missed = (now - lastSlotAt) / SLOT_MS;
  lastSlotAt += missed * SLOT_MS;
  slot = (uint8_t)((slot + missed) & 0x0F);

  const uint16_t pattern = PATTERNS[baseState];
  drive((pattern >> (15 - slot)) & 1);
}
