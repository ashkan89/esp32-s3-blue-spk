#include "app_config.h"
#include "memory_pressure.h"

#include <Arduino.h>

namespace {

uint32_t levelWord = MEMORY_NORMAL;
uint32_t freeWord;
uint32_t largestWord;
uint32_t minimumWord;
uint32_t sinceWord;
uint32_t transitionWord;
uint32_t lastSampleAt;

void store(uint32_t *word, uint32_t value) {
  __atomic_store_n(word, value, __ATOMIC_RELEASE);
}

uint32_t load(const uint32_t *word) {
  return __atomic_load_n(word, __ATOMIC_ACQUIRE);
}

}  // namespace

const char *memory_pressure_name(MemoryPressureLevel level) {
  switch (level) {
    case MEMORY_CONSTRAINED: return "constrained";
    case MEMORY_CRITICAL: return "critical";
    default: return "normal";
  }
}

void memory_pressure_begin() {
  const uint32_t now = millis();
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t largest = ESP.getMaxAllocHeap();
  const MemoryPressureLevel level =
      stability_memory_next(MEMORY_NORMAL, freeHeap, largest);
  store(&freeWord, freeHeap);
  store(&largestWord, largest);
  store(&minimumWord, freeHeap);
  store(&levelWord, level);
  store(&sinceWord, now);
  store(&transitionWord, 0);
  lastSampleAt = now;
}

void memory_pressure_tick() {
  const uint32_t now = millis();
  if (!stability_elapsed(now, lastSampleAt, 500)) return;
  lastSampleAt = now;

  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t largest = ESP.getMaxAllocHeap();
  const MemoryPressureLevel before = memory_pressure_level();
  const MemoryPressureLevel after =
      stability_memory_next(before, freeHeap, largest);
  store(&freeWord, freeHeap);
  store(&largestWord, largest);
  const uint32_t oldMinimum = load(&minimumWord);
  if (!oldMinimum || freeHeap < oldMinimum) store(&minimumWord, freeHeap);
  if (after != before) {
    store(&levelWord, after);
    store(&sinceWord, now);
    __atomic_add_fetch(&transitionWord, 1U, __ATOMIC_RELEASE);
    LOGF("[memory] pressure %s -> %s (%u free, %u largest)\n",
         memory_pressure_name(before), memory_pressure_name(after),
         (unsigned)freeHeap, (unsigned)largest);
  }
}

MemoryPressureLevel memory_pressure_level() {
  return (MemoryPressureLevel)load(&levelWord);
}

void memory_pressure_snapshot(MemoryPressureStatus *out) {
  if (!out) return;
  out->level = memory_pressure_level();
  out->freeHeap = load(&freeWord);
  out->largestBlock = load(&largestWord);
  out->minimumFree = load(&minimumWord);
  out->sinceMs = load(&sinceWord);
  out->transitions = load(&transitionWord);
}

uint16_t memory_pressure_optional_interval(uint16_t normalMs) {
  return stability_optional_interval(normalMs, memory_pressure_level());
}

size_t memory_pressure_json_limit() {
  return stability_json_limit(memory_pressure_level());
}

bool memory_pressure_accept_json(size_t bytes) {
  return bytes <= memory_pressure_json_limit();
}
