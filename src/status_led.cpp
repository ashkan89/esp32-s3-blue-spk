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
volatile uint8_t blipSlots;  // remaining half-cycles of the attention blink

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
  if (slots > blipSlots) blipSlots = slots;
}

void status_led_tick() {
  if (ledPin == 0xFF) return;

  const uint32_t now = millis();

  if (blipSlots) {
    if (now - lastSlotAt < BLIP_MS) return;
    lastSlotAt = now;
    const uint8_t left = (uint8_t)(blipSlots - 1);
    blipSlots = left;
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
