#pragma once

#include <stddef.h>
#include <stdint.h>

#include "stability_policy.h"

struct MemoryPressureStatus {
  MemoryPressureLevel level;
  uint32_t freeHeap;
  uint32_t largestBlock;
  uint32_t minimumFree;
  uint32_t sinceMs;
  uint32_t transitions;
};

void memory_pressure_begin();
void memory_pressure_tick();
MemoryPressureLevel memory_pressure_level();
const char *memory_pressure_name(MemoryPressureLevel level);
void memory_pressure_snapshot(MemoryPressureStatus *out);

/* Optional visual analysis backs off under pressure. Audio rendering itself is
 * never skipped, and nothing owned by a running task is reclaimed. */
uint16_t memory_pressure_optional_interval(uint16_t normalMs);

/* JSON is already buffered by WebServer, but refusing it before ArduinoJson
 * builds a second tree prevents the larger and more fragmented peak. */
bool memory_pressure_accept_json(size_t bytes);
size_t memory_pressure_json_limit();
