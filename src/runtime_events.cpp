#include "runtime_events.h"

#include <Arduino.h>
#include <esp_attr.h>

namespace {

const uint32_t EVENT_MAGIC = 0x45564E54U;  // "EVNT"

struct PersistedEvent {
  uint32_t magic;
  uint32_t event;
  uint32_t atMs;
  uint32_t check;
};

RTC_NOINIT_ATTR PersistedEvent persisted;
portMUX_TYPE eventMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t currentWord;
uint32_t currentAtWord;
uint32_t previousWord;
uint32_t previousAtWord;

uint32_t checksum(uint32_t event, uint32_t atMs) {
  return EVENT_MAGIC ^ event ^ atMs ^ 0xA5C35A7EU;
}

}  // namespace

const char *runtime_event_name(RuntimeEvent event) {
  switch (event) {
    case RUNTIME_EVENT_BOOT: return "boot";
    case RUNTIME_EVENT_MODE_READY: return "mode ready";
    case RUNTIME_EVENT_MODE_SWITCH: return "mode switch";
    case RUNTIME_EVENT_BT_CONNECTED: return "Bluetooth connected";
    case RUNTIME_EVENT_BT_DISCONNECTED: return "Bluetooth disconnected";
    case RUNTIME_EVENT_RADIO_CONNECTING: return "radio connecting";
    case RUNTIME_EVENT_RADIO_PLAYING: return "radio playing";
    case RUNTIME_EVENT_RADIO_RETRY: return "radio retry";
    case RUNTIME_EVENT_RADIO_STOPPED: return "radio stopped";
    case RUNTIME_EVENT_OTA: return "firmware update";
    case RUNTIME_EVENT_ALARM: return "alarm active";
    case RUNTIME_EVENT_STANDBY: return "standby";
    default: return "unknown";
  }
}

void runtime_events_begin() {
  RuntimeEvent previous = RUNTIME_EVENT_UNKNOWN;
  uint32_t previousAt = 0;
  if (persisted.magic == EVENT_MAGIC &&
      persisted.event <= RUNTIME_EVENT_STANDBY &&
      persisted.check == checksum(persisted.event, persisted.atMs)) {
    previous = (RuntimeEvent)persisted.event;
    previousAt = persisted.atMs;
  }
  __atomic_store_n(&previousWord, (uint32_t)previous, __ATOMIC_RELEASE);
  __atomic_store_n(&previousAtWord, previousAt, __ATOMIC_RELEASE);
  runtime_event_note(RUNTIME_EVENT_BOOT);
}

void runtime_event_note(RuntimeEvent event) {
  if (event <= RUNTIME_EVENT_UNKNOWN || event > RUNTIME_EVENT_STANDBY) return;
  const uint32_t atMs = millis();
  portENTER_CRITICAL(&eventMux);
  __atomic_store_n(&currentAtWord, atMs, __ATOMIC_RELAXED);
  __atomic_store_n(&currentWord, (uint32_t)event, __ATOMIC_RELEASE);

  // Invalidate first and commit the magic last. A reset at any intermediate
  // instruction therefore produces an invalid breadcrumb rather than mixing
  // fields from two events that happen to have the same XOR checksum.
  persisted.magic = 0;
  __sync_synchronize();
  persisted.event = (uint32_t)event;
  persisted.atMs = atMs;
  persisted.check = checksum((uint32_t)event, atMs);
  __sync_synchronize();
  persisted.magic = EVENT_MAGIC;
  portEXIT_CRITICAL(&eventMux);
}

void runtime_event_snapshot(RuntimeEventStatus *out) {
  if (!out) return;
  portENTER_CRITICAL(&eventMux);
  out->current = (RuntimeEvent)__atomic_load_n(&currentWord, __ATOMIC_ACQUIRE);
  out->currentAtMs = __atomic_load_n(&currentAtWord, __ATOMIC_RELAXED);
  portEXIT_CRITICAL(&eventMux);
  out->previousBoot =
      (RuntimeEvent)__atomic_load_n(&previousWord, __ATOMIC_ACQUIRE);
  out->previousBootAtMs =
      __atomic_load_n(&previousAtWord, __ATOMIC_ACQUIRE);
}
