#pragma once

#include <stdint.h>

enum RuntimeEvent : uint8_t {
  RUNTIME_EVENT_UNKNOWN = 0,
  RUNTIME_EVENT_BOOT,
  RUNTIME_EVENT_MODE_READY,
  RUNTIME_EVENT_MODE_SWITCH,
  RUNTIME_EVENT_BT_CONNECTED,
  RUNTIME_EVENT_BT_DISCONNECTED,
  RUNTIME_EVENT_RADIO_CONNECTING,
  RUNTIME_EVENT_RADIO_PLAYING,
  RUNTIME_EVENT_RADIO_RETRY,
  RUNTIME_EVENT_RADIO_STOPPED,
  RUNTIME_EVENT_OTA,
  RUNTIME_EVENT_ALARM,
  RUNTIME_EVENT_STANDBY,
};

struct RuntimeEventStatus {
  RuntimeEvent current;
  uint32_t currentAtMs;
  RuntimeEvent previousBoot;
  uint32_t previousBootAtMs;
};

/* Call once near the beginning of setup, before recording this boot's events. */
void runtime_events_begin();
void runtime_event_note(RuntimeEvent event);
void runtime_event_snapshot(RuntimeEventStatus *out);
const char *runtime_event_name(RuntimeEvent event);
