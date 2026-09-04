/*
 * Small, hardware-independent policy primitives shared by firmware and host
 * regression tests. Keep this header free of Arduino/FreeRTOS dependencies.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

enum MemoryPressureLevel : uint8_t {
  MEMORY_NORMAL = 0,
  MEMORY_CONSTRAINED,
  MEMORY_CRITICAL,
};

/* These thresholds come from this image's measured large operations: the OTA
 * TLS path requires 80 kB free and a 45 kB block; a plain radio start requires
 * about 70 kB free and a 26 kB block. Recovery margins prevent oscillation. */
static const uint32_t MEMORY_CONSTRAINED_FREE = 90000;
static const uint32_t MEMORY_CONSTRAINED_BLOCK = 45000;
static const uint32_t MEMORY_CRITICAL_FREE = 70000;
static const uint32_t MEMORY_CRITICAL_BLOCK = 28000;
static const uint32_t MEMORY_RECOVER_FREE = 82000;
static const uint32_t MEMORY_RECOVER_BLOCK = 36000;
static const uint32_t MEMORY_NORMAL_FREE = 105000;
static const uint32_t MEMORY_NORMAL_BLOCK = 52000;

constexpr MemoryPressureLevel stability_memory_next(MemoryPressureLevel current,
                                                     uint32_t freeHeap,
                                                     uint32_t largestBlock) {
  const bool critical = freeHeap < MEMORY_CRITICAL_FREE ||
                        largestBlock < MEMORY_CRITICAL_BLOCK;
  if (critical) return MEMORY_CRITICAL;
  if (current == MEMORY_CRITICAL) {
    return freeHeap >= MEMORY_RECOVER_FREE &&
                   largestBlock >= MEMORY_RECOVER_BLOCK
               ? MEMORY_CONSTRAINED
               : MEMORY_CRITICAL;
  }
  const bool constrained = freeHeap < MEMORY_CONSTRAINED_FREE ||
                           largestBlock < MEMORY_CONSTRAINED_BLOCK;
  if (constrained) return MEMORY_CONSTRAINED;
  if (current == MEMORY_CONSTRAINED &&
      (freeHeap < MEMORY_NORMAL_FREE || largestBlock < MEMORY_NORMAL_BLOCK)) {
    return MEMORY_CONSTRAINED;
  }
  return MEMORY_NORMAL;
}

constexpr uint16_t stability_optional_interval(uint16_t normalMs,
                                               MemoryPressureLevel level) {
  const uint32_t multiplier = level == MEMORY_CRITICAL
                                  ? 4U
                              : level == MEMORY_CONSTRAINED ? 2U
                                                            : 1U;
  const uint32_t result = (uint32_t)normalMs * multiplier;
  return result > UINT16_MAX ? UINT16_MAX : (uint16_t)result;
}

constexpr size_t stability_json_limit(MemoryPressureLevel level) {
  return level == MEMORY_CRITICAL    ? 4096U
         : level == MEMORY_CONSTRAINED ? 8192U
                                       : 12288U;
}

/* Unsigned subtraction is rollover safe for intervals shorter than 2^31 ms. */
constexpr bool stability_elapsed(uint32_t now, uint32_t since,
                                 uint32_t interval) {
  return (uint32_t)(now - since) >= interval;
}

/* Remaining time to an absolute millis() deadline. Correct across rollover
 * while the deadline is less than 2^31 ms away (all firmware timers are). */
constexpr uint32_t stability_remaining(uint32_t now, uint32_t deadline) {
  return (int32_t)(deadline - now) > 0 ? deadline - now : 0U;
}

/* Seconds to the next alarm. Sunday is bit 0, matching tm_wday and AlarmDays;
 * days==0 is the one-shot form and means the next occurrence within 24 h. */
constexpr uint32_t stability_alarm_seconds_until(bool enabled, uint8_t days,
                                                  uint8_t hour, uint8_t minute,
                                                  uint8_t nowWday,
                                                  uint8_t nowHour,
                                                  uint8_t nowMinute,
                                                  uint8_t nowSecond) {
  if (!enabled) return 0;
  const int32_t nowSecs = (int32_t)nowHour * 3600 + nowMinute * 60 + nowSecond;
  const int32_t atSecs = (int32_t)hour * 3600 + minute * 60;
  if (days == 0) {
    int32_t delta = atSecs - nowSecs;
    if (delta <= 0) delta += 86400;
    return (uint32_t)delta;
  }
  for (uint8_t ahead = 0; ahead <= 7; ++ahead) {
    const uint8_t wday = (uint8_t)((nowWday + ahead) % 7);
    if (!(days & (1U << wday))) continue;
    const int32_t delta = atSecs - nowSecs + (int32_t)ahead * 86400;
    if (delta > 0) return (uint32_t)delta;
  }
  return 0;
}

constexpr uint32_t stability_reconnect_next(uint32_t current,
                                            uint32_t maximum = 60000) {
  return current >= maximum / 2U ? maximum : current * 2U;
}

constexpr uint32_t stability_reconnect_jitter(uint32_t base,
                                              uint32_t randomWord) {
  const uint32_t spread = base >= 4U ? base / 4U : 1U;
  const uint32_t range = spread * 2U + 1U;
  const int32_t offset = (int32_t)(randomWord % range) - (int32_t)spread;
  return (uint32_t)((int32_t)base + offset);
}

constexpr int16_t stability_pcm_saturate(int32_t value) {
  if (value > 32767) return 32767;
  if (value < -32768) return -32768;
  return (int16_t)value;
}

constexpr int16_t stability_pcm_volume(int16_t sample, uint8_t volume127) {
  const uint8_t gain = volume127 > 127 ? 127 : volume127;
  if (gain == 127) return sample;
  return (int16_t)(((int32_t)sample * gain) >> 7);
}

/* Keep every EQ corner below Nyquist. Low-rate speech/radio streams can be
 * 8 kHz, where the nominal 12 kHz shelf would otherwise alias into the band. */
constexpr uint32_t stability_eq_frequency(uint32_t requestedHz,
                                          uint32_t sampleRate) {
  const uint32_t highest = sampleRate * 45U / 100U;
  return requestedHz < highest ? requestedHz : highest;
}

constexpr size_t stability_ring_advance(size_t index, size_t amount,
                                        size_t capacity) {
  return capacity ? (index + amount) % capacity : 0;
}

/* Index of the oldest valid entry when `next` is the next write position. */
constexpr size_t stability_ring_oldest(size_t next, size_t count,
                                       size_t capacity) {
  return capacity
             ? (next + capacity - (count < capacity ? count : capacity)) %
                   capacity
             : 0;
}

constexpr uint8_t stability_mode_next(uint8_t current, uint8_t modeCount) {
  return modeCount ? (uint8_t)((current + 1U) % modeCount) : 0;
}

constexpr char stability_ascii_lower(char value) {
  return value >= 'A' && value <= 'Z' ? (char)(value + ('a' - 'A')) : value;
}

constexpr bool stability_ascii_prefix_ci(const char *text, const char *prefix) {
  if (!text || !prefix) return false;
  while (*prefix) {
    if (!*text || stability_ascii_lower(*text) !=
                      stability_ascii_lower(*prefix)) {
      return false;
    }
    ++text;
    ++prefix;
  }
  return true;
}

constexpr bool stability_url_http(const char *url) {
  return stability_ascii_prefix_ci(url, "http://") ||
         stability_ascii_prefix_ci(url, "https://");
}

constexpr bool stability_url_https(const char *url) {
  return stability_ascii_prefix_ci(url, "https://");
}

constexpr bool stability_utf8_continuation(char value) {
  return ((uint8_t)value & 0xC0U) == 0x80U;
}

/* Length of one valid UTF-8 sequence. Invalid, overlong, surrogate, out-of-range
 * or truncated input is one byte so a sanitizer makes progress without ever
 * stepping beyond the terminating NUL. */
constexpr uint8_t stability_utf8_sequence_length(const char *text) {
  if (!text || !*text) return 0;
  const uint8_t lead = (uint8_t)*text;
  if (lead < 0x80U) return 1;
  const uint8_t second = (uint8_t)text[1];
  if (lead >= 0xC2U && lead <= 0xDFU &&
      stability_utf8_continuation(text[1])) {
    return 2;
  }
  if ((lead & 0xF0U) == 0xE0U && stability_utf8_continuation(text[1]) &&
      stability_utf8_continuation(text[2]) &&
      !(lead == 0xE0U && second < 0xA0U) &&
      !(lead == 0xEDU && second >= 0xA0U)) {
    return 3;
  }
  if (lead >= 0xF0U && lead <= 0xF4U &&
      stability_utf8_continuation(text[1]) &&
      stability_utf8_continuation(text[2]) &&
      stability_utf8_continuation(text[3]) &&
      !(lead == 0xF0U && second < 0x90U) &&
      !(lead == 0xF4U && second > 0x8FU)) {
    return 4;
  }
  return 1;
}

constexpr size_t stability_bounded_copy_length(size_t inputLength,
                                               size_t destinationSize) {
  return destinationSize && inputLength >= destinationSize
             ? destinationSize - 1
             : destinationSize ? inputLength : 0;
}
