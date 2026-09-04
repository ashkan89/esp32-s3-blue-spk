/*
 * -O2 for this file, against the -Os the rest of the firmware is built with.
 *
 * Measured, not assumed: building the whole image at -O2 costs 148 kB of extra
 * code. On a chip that executes from flash through a 32 kB instruction cache
 * that is a bad trade -- almost all of the growth is cold code, and it evicts
 * hot code from the cache. Applied to one file it costs a couple of kilobytes.
 *
 * This file earns it. The ADPCM decode, the resampler and the ducking mixer all
 * run once per sample, on an audio task, underneath music that is already
 * playing.
 */
#pragma GCC optimize("O2")

#include "app_config.h"
#include "voice.h"

#include <Arduino.h>
#include <string.h>

#include "stability_policy.h"

namespace {

/*
 * The IMA ADPCM reference tables. The same two the encoder in
 * scripts/make_voice_clips.py uses, and they have to stay identical: this
 * decoder reproduces the encoder's arithmetic exactly, which is what makes the
 * format lossless with respect to itself.
 */
const int16_t STEP_TABLE[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,
    21,    23,    25,    28,    31,    34,    37,    41,    45,    50,    55,
    60,    66,    73,    80,    88,    97,    107,   118,   130,   143,   157,
    173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,
    494,   544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,
    1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,  3660,
    4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};

const int8_t INDEX_TABLE[16] = {-1, -1, -1, -1, 2, 4, 6, 8,
                                -1, -1, -1, -1, 2, 4, 6, 8};

/*
 * Who is draining the queue.
 *
 * The two render paths run on different tasks and neither can see the other, so
 * the first one to touch a clip claims it and the other leaves it alone until
 * it is finished. Without this a clip that starts while music is playing and
 * carries on after the music stops would be pulled by both, and each would get
 * every other chunk of it.
 */
enum Owner : uint8_t {
  OWNER_NONE = 0,
  OWNER_MIX,   ///< the audio task, mixing under music
  OWNER_LOOP,  ///< the Arduino task, with the DAC to itself
};

/// How long the mixing path may go unheard before the loop path assumes the
/// stream has stopped and takes over. Two or three buffers' worth: long enough
/// that a scheduling hiccup does not hand the clip over mid-word, short enough
/// that a stream ending mid-announcement is picked up before the gap is audible.
const uint32_t MIX_STALE_MS = 350;

// One slot distinguishes full from empty, so five array entries provide the
// advertised four pending announcements.
const uint8_t QUEUE_LEN = 5;

struct Decoder {
  const uint8_t *data;
  uint32_t samples;     ///< total, at VOICE_CLIP_RATE
  uint32_t consumed;    ///< how many have been decoded
  int32_t predictor;
  int8_t index;

  // Linear resampling from VOICE_CLIP_RATE to the output rate. `phase` is a
  // 16.16 fixed-point position between `prev` and `next`.
  int16_t prev, next;
  uint32_t phase;
  uint32_t step;  ///< 16.16 increment: how much of a source sample per output one
};

/* Four bytes published atomically: enabled, volume, category mask, duck and a
 * configured bit. The audio path never observes a half-written configuration. */
uint32_t configWord;

uint8_t queue[QUEUE_LEN];
uint8_t queueHead, queueTail;
uint32_t queuedCount;
uint32_t queueHighWater;

/*
 * The queue has three producers -- the Arduino loop task, the radio decoder
 * task and the web server -- and the clip is claimed by whichever of two
 * consumers asks first. Both of those are short critical sections rather than
 * mutexes: a spinlock held for four instructions is safe on an audio task in a
 * way that waiting on a mutex never is.
 */
portMUX_TYPE queueMux = portMUX_INITIALIZER_UNLOCKED;

Decoder dec;
uint8_t owner;
uint32_t playingWord;
bool cancelRequested;
uint32_t lastMixMs;
uint32_t outRateWord = 44100;
uint32_t appliedOutRate = 44100;

/*
 * The ducking ramp, as a 0..4096 multiplier on the music.
 *
 * A ramp rather than a switch: 60 ms is long enough that the level change reads
 * as somebody turning the music down and short enough to be out of the way
 * before the first syllable. The two rates differ on purpose -- coming back up
 * slower than going down is what every broadcast ducker does, because a fast
 * recovery lands on the tail of the announcement and sounds like a stutter.
 */
const int32_t DUCK_UNITY = 4096;
int32_t duckLevel = DUCK_UNITY;
const int32_t DUCK_ATTACK = 70;   // per frame, ~60 ms from unity to a quarter
const int32_t DUCK_RELEASE = 24;  // per frame, ~170 ms back

inline int16_t clamp16(int32_t v) {
  return stability_pcm_saturate(v);
}

uint32_t packConfig(const VoiceConfig &cfg, bool ready = true) {
  return (cfg.enabled ? 1U : 0U) | ((uint32_t)cfg.volume << 1) |
         ((uint32_t)cfg.categories << 8) | ((uint32_t)cfg.duck << 16) |
         (ready ? 0x80000000U : 0U);
}

bool unpackConfig(uint32_t word, VoiceConfig *out) {
  if (!out || !(word & 0x80000000U)) return false;
  out->enabled = (word & 1U) != 0;
  out->volume = (uint8_t)((word >> 1) & 0x7FU);
  out->categories = (uint8_t)((word >> 8) & 0xFFU);
  out->duck = (uint8_t)((word >> 16) & 0x7FU);
  return true;
}

VoiceConfig currentConfig() {
  VoiceConfig cfg;
  const uint32_t word = __atomic_load_n(&configWord, __ATOMIC_ACQUIRE);
  if (!unpackConfig(word, &cfg)) {
    cfg.enabled = true;
    cfg.volume = 70;
    cfg.categories = (uint8_t)(VOICE_CAT_SYSTEM | VOICE_CAT_BATTERY |
                               VOICE_CAT_RADIO | VOICE_CAT_ALARM);
    cfg.duck = 75;
  }
  return cfg;
}

bool isPlaying() {
  return __atomic_load_n(&playingWord, __ATOMIC_ACQUIRE) != 0;
}

void setPlaying(bool value) {
  __atomic_store_n(&playingWord, value ? 1U : 0U, __ATOMIC_RELEASE);
}

uint32_t pendingClips() {
  return __atomic_load_n(&queuedCount, __ATOMIC_ACQUIRE);
}

void resetDecoder() {
  dec.data = nullptr;
  dec.samples = 0;
  dec.consumed = 0;
  dec.predictor = 0;
  dec.index = 0;
  dec.prev = 0;
  dec.next = 0;
  dec.phase = 0;
}

/// One ADPCM nibble -> one sample. Exactly the encoder's arithmetic, run
/// backwards; see scripts/make_voice_clips.py for the forward half.
inline int16_t decodeNibble(uint8_t code) {
  const int32_t step = STEP_TABLE[dec.index];
  int32_t delta = step >> 3;
  if (code & 4) delta += step;
  if (code & 2) delta += step >> 1;
  if (code & 1) delta += step >> 2;

  dec.predictor = (code & 8) ? dec.predictor - delta : dec.predictor + delta;
  if (dec.predictor > 32767) dec.predictor = 32767;
  if (dec.predictor < -32768) dec.predictor = -32768;

  int8_t next = (int8_t)(dec.index + INDEX_TABLE[code]);
  if (next < 0) next = 0;
  if (next > 88) next = 88;
  dec.index = next;
  return (int16_t)dec.predictor;
}

/// The next source sample, or false at the end of the clip. Two samples live in
/// each byte, low nibble first -- the order the encoder packs them in.
bool nextSourceSample(int16_t *out) {
  if (!dec.data || dec.consumed >= dec.samples) return false;
  const uint8_t byte = pgm_read_byte(dec.data + (dec.consumed >> 1));
  const uint8_t code = (dec.consumed & 1) ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 0x0F);
  dec.consumed++;
  *out = decodeNibble(code);
  return true;
}

/// Starts the clip at `index`, or returns false if there is no such clip.
bool loadClip(uint8_t index) {
  if (index >= VOICE_CLIP_COUNT) return false;
  const VoiceClipDef &clip = VOICE_CLIPS[index];
  resetDecoder();
  dec.data = clip.data;
  dec.samples = clip.samples;
  appliedOutRate = __atomic_load_n(&outRateWord, __ATOMIC_ACQUIRE);
  dec.step = (uint32_t)(((uint64_t)VOICE_CLIP_RATE << 16) /
                        (appliedOutRate ? appliedOutRate : 44100));
  // Prime the interpolator so the first output frame sits between two real
  // samples rather than between zero and the first one, which would be a step.
  nextSourceSample(&dec.prev);
  if (!nextSourceSample(&dec.next)) dec.next = dec.prev;
  dec.phase = 0;
  return true;
}

/// Pops the next queued clip and loads it. False when the queue is empty.
bool startNext() {
  for (;;) {
    uint8_t index = VOICE_CLIP_COUNT;
    taskENTER_CRITICAL(&queueMux);
    if (queueHead != queueTail) {
      index = queue[queueHead];
      queueHead = (uint8_t)((queueHead + 1) % QUEUE_LEN);
      __atomic_sub_fetch(&queuedCount, 1U, __ATOMIC_RELEASE);
    }
    taskEXIT_CRITICAL(&queueMux);
    if (index >= VOICE_CLIP_COUNT) break;
    if (loadClip(index)) {
      setPlaying(true);
      return true;
    }
  }
  setPlaying(false);
  return false;
}

/*
 * One mono output sample from the clip, resampled to the output rate.
 *
 * Linear interpolation. On speech at 16 kHz played out at 44.1 kHz the images
 * this leaves sit above 8 kHz at about -30 dB, which through a small driver is
 * inaudible -- and a polyphase filter that did better would cost more than the
 * rest of this file put together for a result nobody could hear.
 */
bool nextOutputSample(int16_t *out) {
  if (!dec.data) return false;
  while (dec.phase >= 0x10000) {
    dec.phase -= 0x10000;
    dec.prev = dec.next;
    if (!nextSourceSample(&dec.next)) {
      dec.data = nullptr;  // the clip is finished; drain the last interpolation
      *out = dec.prev;
      return false;
    }
  }
  const int32_t frac = (int32_t)(dec.phase >> 4);  // 12 bits, cheap to multiply
  *out = (int16_t)(((int32_t)dec.prev * (4096 - frac) + (int32_t)dec.next * frac) >> 12);
  dec.phase += dec.step;
  return true;
}

/// The announcement's own level, as a 0..4096 multiplier.
inline int32_t clipGain() {
  const uint8_t volume = currentConfig().volume;
  return ((int32_t)volume * DUCK_UNITY) / 100;
}

/// Where the music is pulled down to while a clip plays, as a 0..4096
/// multiplier. duck=0 leaves the music alone, duck=100 mutes it outright.
inline int32_t duckTarget() {
  const uint8_t duck = currentConfig().duck;
  return DUCK_UNITY - ((int32_t)duck * DUCK_UNITY) / 100;
}

bool categoryAllowed(VoiceCategory category) {
  const VoiceConfig cfg = currentConfig();
  if (!cfg.enabled) return false;
  return (cfg.categories & (uint8_t)category) != 0;
}

/* Claims the decoder without ever handing it to two tasks. The only takeover
 * is from a mixing path that has missed several complete DMA buffers. */
bool claimDecoder(Owner wanted) {
  bool mine;
  taskENTER_CRITICAL(&queueMux);
  if (owner == OWNER_MIX && wanted == OWNER_LOOP &&
      (uint32_t)(millis() - lastMixMs) > MIX_STALE_MS) {
    owner = OWNER_NONE;
  }
  if (owner == OWNER_NONE) owner = wanted;
  mine = owner == wanted;
  if (mine && wanted == OWNER_MIX) lastMixMs = millis();
  taskEXIT_CRITICAL(&queueMux);
  return mine;
}

void releaseDecoder(Owner mine) {
  taskENTER_CRITICAL(&queueMux);
  if (owner == mine) owner = OWNER_NONE;
  taskEXIT_CRITICAL(&queueMux);
}

/* Only the task that owns the decoder may honour cancellation. Control tasks
 * clear the queue and set the request, but never touch ADPCM/resampler state. */
void serviceCancel(Owner mine) {
  bool cancel = false;
  taskENTER_CRITICAL(&queueMux);
  if (owner == mine && cancelRequested) {
    cancelRequested = false;
    cancel = true;
  }
  taskEXIT_CRITICAL(&queueMux);
  if (cancel) {
    resetDecoder();
    setPlaying(false);
  }
}

void adoptSampleRate() {
  const uint32_t rate = __atomic_load_n(&outRateWord, __ATOMIC_ACQUIRE);
  if (rate == appliedOutRate) return;
  appliedOutRate = rate;
  if (dec.data) {
    dec.step = (uint32_t)(((uint64_t)VOICE_CLIP_RATE << 16) / appliedOutRate);
  }
}

bool enqueueClip(uint8_t clip) {
  bool queued = false;
  taskENTER_CRITICAL(&queueMux);
  const uint8_t next = (uint8_t)((queueTail + 1) % QUEUE_LEN);
  // A full queue drops the newest rather than the oldest. Announcements are
  // events in time: the four already waiting describe what happened, and a
  // fifth that pushed one of them out would tell a story that did not occur.
  if (next != queueHead) {
    queue[queueTail] = clip;
    queueTail = next;
    __atomic_add_fetch(&queuedCount, 1U, __ATOMIC_RELEASE);
    const uint32_t depth = pendingClips();
    uint32_t previous =
        __atomic_load_n(&queueHighWater, __ATOMIC_RELAXED);
    while (depth > previous &&
           !__atomic_compare_exchange_n(&queueHighWater, &previous, depth,
                                        false, __ATOMIC_RELEASE,
                                        __ATOMIC_RELAXED)) {
    }
    queued = true;
  }
  taskEXIT_CRITICAL(&queueMux);
  return queued;
}

}  // namespace

void voice_defaults(VoiceConfig *out) {
  if (!out) return;
  out->enabled = true;
  out->volume = 70;
  // Connection announcements are off by default. A phone that drifts in and out
  // of range at the edge of the house would otherwise have the speaker talking
  // to an empty room all afternoon, and that is the single most common reason
  // somebody turns a feature like this off and never turns it back on.
  out->categories = (uint8_t)(VOICE_CAT_SYSTEM | VOICE_CAT_BATTERY |
                              VOICE_CAT_RADIO | VOICE_CAT_ALARM);
  out->duck = 75;
}

void voice_configure(const VoiceConfig &cfg) {
  VoiceConfig next = cfg;
  if (next.volume > 100) next.volume = 100;
  if (next.duck > 100) next.duck = 100;
  next.categories &= (uint8_t)VOICE_CAT_ALL;
  __atomic_store_n(&configWord, packConfig(next), __ATOMIC_RELEASE);
  if (!next.enabled) voice_silence();
}

void voice_get(VoiceConfig *out) {
  if (!out) return;
  const uint32_t word = __atomic_load_n(&configWord, __ATOMIC_ACQUIRE);
  if (!unpackConfig(word, out)) voice_defaults(out);
}

bool voice_say_index(uint8_t clip, VoiceCategory category) {
  if (clip >= VOICE_CLIP_COUNT) return false;
  if (!categoryAllowed(category)) return false;
  return enqueueClip(clip);
}

bool voice_say(VoiceClipId clip, VoiceCategory category) {
  return voice_say_index((uint8_t)clip, category);
}

void voice_silence() {
  taskENTER_CRITICAL(&queueMux);
  queueHead = queueTail;
  __atomic_store_n(&queuedCount, 0U, __ATOMIC_RELEASE);
  cancelRequested = true;
  taskEXIT_CRITICAL(&queueMux);
}

bool voice_busy() { return isPlaying() || pendingClips() != 0; }

uint8_t voice_queue_depth() { return (uint8_t)pendingClips(); }

uint8_t voice_queue_high_water() {
  return (uint8_t)__atomic_load_n(&queueHighWater, __ATOMIC_ACQUIRE);
}

void voice_set_sample_rate(uint32_t hz) {
  if (hz < 8000 || hz > 96000) return;
  // The setter can run on loop() while A2DP owns the decoder. Publish only;
  // whichever audio path owns the next buffer retunes its own state.
  __atomic_store_n(&outRateWord, hz, __ATOMIC_RELEASE);
}

size_t voice_render(int16_t *interleaved, size_t frames) {
  if (!interleaved || frames == 0) return 0;
  // Claim it or leave it alone: testing and then setting without this is the
  // window in which both renderers decode alternate chunks of one clip.
  if (!claimDecoder(OWNER_LOOP)) return 0;
  serviceCancel(OWNER_LOOP);
  adoptSampleRate();

  if (!isPlaying() && !startNext()) {
    releaseDecoder(OWNER_LOOP);
    return 0;
  }

  const int32_t gain = clipGain();
  size_t produced = 0;
  while (produced < frames) {
    int16_t sample = 0;
    const bool more = nextOutputSample(&sample);
    const int16_t value = clamp16(((int32_t)sample * gain) >> 12);
    interleaved[2 * produced] = value;
    interleaved[2 * produced + 1] = value;
    produced++;
    if (!more) {
      // Straight on to the next queued clip, so a two-part announcement plays
      // as one utterance instead of with a buffer-length hole in it.
      if (!startNext()) {
        releaseDecoder(OWNER_LOOP);
        break;
      }
    }
  }
  return produced;
}

bool voice_mix(int16_t *interleaved, size_t frames) {
  if (!interleaved || frames == 0) return false;

  const bool queued = isPlaying() || pendingClips() != 0;
  // Nothing to say and the music already back at full level: the common case,
  // and it has to cost nothing because this runs on every buffer of every
  // stream.
  if (!queued && duckLevel >= DUCK_UNITY) return false;

  const bool mine = queued && claimDecoder(OWNER_MIX);
  if (mine) serviceCancel(OWNER_MIX);
  if (mine) adoptSampleRate();
  if (mine && !isPlaying() && !startNext()) {
    releaseDecoder(OWNER_MIX);
  }

  const bool speaking = mine && isPlaying();
  const int32_t target = speaking ? duckTarget() : DUCK_UNITY;
  const int32_t gain = clipGain();

  for (size_t f = 0; f < frames; f++) {
    if (duckLevel < target) {
      duckLevel += DUCK_RELEASE;
      if (duckLevel > target) duckLevel = target;
    } else if (duckLevel > target) {
      duckLevel -= DUCK_ATTACK;
      if (duckLevel < target) duckLevel = target;
    }

    int32_t left = ((int32_t)interleaved[2 * f] * duckLevel) >> 12;
    int32_t right = ((int32_t)interleaved[2 * f + 1] * duckLevel) >> 12;

    if (speaking) {
      int16_t sample = 0;
      const bool more = nextOutputSample(&sample);
      const int32_t voiced = ((int32_t)sample * gain) >> 12;
      left += voiced;
      right += voiced;
      if (!more && !startNext()) {
        releaseDecoder(OWNER_MIX);
        // The duck ramp is left where it is; the loop above walks it back to
        // unity over the following frames, which is the release.
        for (size_t rest = f + 1; rest < frames; rest++) {
          if (duckLevel < DUCK_UNITY) {
            duckLevel += DUCK_RELEASE;
            if (duckLevel > DUCK_UNITY) duckLevel = DUCK_UNITY;
          }
          interleaved[2 * rest] =
              clamp16(((int32_t)interleaved[2 * rest] * duckLevel) >> 12);
          interleaved[2 * rest + 1] =
              clamp16(((int32_t)interleaved[2 * rest + 1] * duckLevel) >> 12);
        }
        interleaved[2 * f] = clamp16(left);
        interleaved[2 * f + 1] = clamp16(right);
        return true;
      }
    }

    interleaved[2 * f] = clamp16(left);
    interleaved[2 * f + 1] = clamp16(right);
  }
  return speaking;
}

uint8_t voice_clip_count() { return VOICE_CLIP_COUNT; }

const char *voice_clip_id(uint8_t index) {
  return index < VOICE_CLIP_COUNT ? VOICE_CLIPS[index].id : "";
}

const char *voice_clip_text(uint8_t index) {
  return index < VOICE_CLIP_COUNT ? VOICE_CLIPS[index].text : "";
}

uint8_t voice_clip_by_id(const char *id) {
  if (!id || !id[0]) return VOICE_CLIP_COUNT;
  for (uint8_t i = 0; i < VOICE_CLIP_COUNT; i++) {
    if (strcmp(VOICE_CLIPS[i].id, id) == 0) return i;
  }
  return VOICE_CLIP_COUNT;
}

bool voice_command(const char *line) {
  if (!line || strncmp(line, "say", 3) != 0) return false;
  const char *rest = line + 3;
  while (*rest == ' ') rest++;

  VoiceConfig config;
  voice_get(&config);

  if (*rest == '\0') {
    LOGF("[voice] %s, volume %u%%, duck %u%%, %u clips |",
                  config.enabled ? "on" : "off", (unsigned)config.volume,
                  (unsigned)config.duck, (unsigned)VOICE_CLIP_COUNT);
    LOGF(" system %s connection %s battery %s radio %s alarm %s\n",
                  config.categories & VOICE_CAT_SYSTEM ? "on" : "off",
                  config.categories & VOICE_CAT_CONNECTION ? "on" : "off",
                  config.categories & VOICE_CAT_BATTERY ? "on" : "off",
                  config.categories & VOICE_CAT_RADIO ? "on" : "off",
                  config.categories & VOICE_CAT_ALARM ? "on" : "off");
    LOGP("[voice] clips:");
    for (uint8_t i = 0; i < VOICE_CLIP_COUNT; i++) {
      LOGF(" %s", VOICE_CLIPS[i].id);
    }
    LOGLN();
    return true;
  }
  if (strcmp(rest, "on") == 0 || strcmp(rest, "off") == 0) {
    VoiceConfig next = config;
    next.enabled = rest[1] == 'n';
    voice_configure(next);
    LOGF("[voice] announcements %s\n", next.enabled ? "on" : "off");
    return true;
  }
  if (strcmp(rest, "stop") == 0) {
    voice_silence();
    LOGLN("[voice] silenced");
    return true;
  }

  const uint8_t index = voice_clip_by_id(rest);
  if (index >= VOICE_CLIP_COUNT) {
    LOGF("[voice] no clip called \"%s\"; type 'say' for the list\n", rest);
    return true;
  }
  // The console asks for it by name, so it bypasses the category mask without
  // temporarily rewriting the configuration under the audio task.
  const bool ok = enqueueClip(index);
  LOGF("[voice] %s \"%s\"\n", ok ? "queued" : "dropped",
                VOICE_CLIPS[index].text);
  return true;
}
