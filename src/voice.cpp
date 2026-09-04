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

const uint8_t QUEUE_LEN = 4;

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

VoiceConfig config;
bool configured;

volatile uint8_t queue[QUEUE_LEN];
volatile uint8_t queueHead, queueTail;

/*
 * The queue has three producers -- the Arduino loop task, the radio decoder
 * task and the web server -- and the clip is claimed by whichever of two
 * consumers asks first. Both of those are short critical sections rather than
 * mutexes: a spinlock held for four instructions is safe on an audio task in a
 * way that waiting on a mutex never is.
 */
portMUX_TYPE queueMux = portMUX_INITIALIZER_UNLOCKED;

Decoder dec;
volatile uint8_t owner;
volatile bool playing;
uint32_t lastMixMs;
uint32_t outRate = 44100;

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
  if (v > 32767) return 32767;
  if (v < -32768) return -32768;
  return (int16_t)v;
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
  dec.step = (uint32_t)(((uint64_t)VOICE_CLIP_RATE << 16) / (outRate ? outRate : 44100));
  // Prime the interpolator so the first output frame sits between two real
  // samples rather than between zero and the first one, which would be a step.
  nextSourceSample(&dec.prev);
  if (!nextSourceSample(&dec.next)) dec.next = dec.prev;
  dec.phase = 0;
  return true;
}

/// Pops the next queued clip and loads it. False when the queue is empty.
bool startNext() {
  while (queueHead != queueTail) {
    const uint8_t index = queue[queueHead];
    queueHead = (uint8_t)((queueHead + 1) % QUEUE_LEN);
    if (loadClip(index)) {
      playing = true;
      return true;
    }
  }
  playing = false;
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
  const uint8_t volume = configured ? config.volume : 66;
  return ((int32_t)volume * DUCK_UNITY) / 100;
}

/// Where the music is pulled down to while a clip plays, as a 0..4096
/// multiplier. duck=0 leaves the music alone, duck=100 mutes it outright.
inline int32_t duckTarget() {
  const uint8_t duck = configured ? config.duck : 75;
  return DUCK_UNITY - ((int32_t)duck * DUCK_UNITY) / 100;
}

bool categoryAllowed(VoiceCategory category) {
  if (!configured) voice_defaults(&config);
  if (!config.enabled) return false;
  return (config.categories & (uint8_t)category) != 0;
}

/// Frees the clip for the other path when the one that claimed it has gone
/// quiet. Called from both renderers, so a stream that stops mid-announcement
/// is picked up by loop() rather than leaving the clip stranded.
void releaseIfStale() {
  if (owner == OWNER_MIX && (uint32_t)(millis() - lastMixMs) > MIX_STALE_MS) {
    owner = OWNER_NONE;
  }
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
  config = cfg;
  if (config.volume > 100) config.volume = 100;
  if (config.duck > 100) config.duck = 100;
  config.categories &= (uint8_t)VOICE_CAT_ALL;
  configured = true;
  if (!config.enabled) voice_silence();
}

void voice_get(VoiceConfig *out) {
  if (!out) return;
  if (!configured) voice_defaults(&config);
  *out = config;
}

bool voice_say_index(uint8_t clip, VoiceCategory category) {
  if (clip >= VOICE_CLIP_COUNT) return false;
  if (!categoryAllowed(category)) return false;
  bool queued = false;
  taskENTER_CRITICAL(&queueMux);
  const uint8_t next = (uint8_t)((queueTail + 1) % QUEUE_LEN);
  // A full queue drops the newest rather than the oldest. Announcements are
  // events in time: the four already waiting describe what happened, and a
  // fifth that pushed one of them out would tell a story that did not occur.
  if (next != queueHead) {
    queue[queueTail] = clip;
    queueTail = next;
    queued = true;
  }
  taskEXIT_CRITICAL(&queueMux);
  return queued;
}

bool voice_say(VoiceClipId clip, VoiceCategory category) {
  return voice_say_index((uint8_t)clip, category);
}

void voice_silence() {
  queueHead = queueTail;
  resetDecoder();
  playing = false;
  owner = OWNER_NONE;
}

bool voice_busy() { return playing || queueHead != queueTail; }

void voice_set_sample_rate(uint32_t hz) {
  if (hz < 8000 || hz > 96000 || hz == outRate) return;
  outRate = hz;
  // Retune the running clip rather than dropping it: the rate changes when a
  // stream starts, which is exactly when an announcement is most likely to be
  // in flight.
  if (dec.data) dec.step = (uint32_t)(((uint64_t)VOICE_CLIP_RATE << 16) / outRate);
}

size_t voice_render(int16_t *interleaved, size_t frames) {
  if (!interleaved || frames == 0) return 0;
  releaseIfStale();
  // Claim it or leave it alone: testing and then setting without this is the
  // window in which both renderers decode alternate chunks of one clip.
  bool mine = false;
  taskENTER_CRITICAL(&queueMux);
  if (owner == OWNER_NONE) {
    owner = OWNER_LOOP;
    mine = true;
  } else if (owner == OWNER_LOOP) {
    mine = true;
  }
  taskEXIT_CRITICAL(&queueMux);
  if (!mine) return 0;

  if (!playing && !startNext()) {
    owner = OWNER_NONE;
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
        owner = OWNER_NONE;
        break;
      }
    }
  }
  return produced;
}

bool voice_mix(int16_t *interleaved, size_t frames) {
  if (!interleaved || frames == 0) return false;

  const bool queued = playing || queueHead != queueTail;
  // Nothing to say and the music already back at full level: the common case,
  // and it has to cost nothing because this runs on every buffer of every
  // stream.
  if (!queued && duckLevel >= DUCK_UNITY) return false;

  releaseIfStale();
  bool mine = false;
  taskENTER_CRITICAL(&queueMux);
  if (queued && owner == OWNER_NONE) owner = OWNER_MIX;
  mine = owner == OWNER_MIX;
  taskEXIT_CRITICAL(&queueMux);
  if (mine) lastMixMs = millis();
  if (mine && !playing && !startNext()) {
    owner = OWNER_NONE;
  }

  const bool speaking = mine && playing;
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
        owner = OWNER_NONE;
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

  if (!configured) voice_defaults(&config);

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
  // The console asks for it by name, so it is said whatever the category mask
  // has to say about it -- somebody typing "say battery_low" wants to hear it.
  const uint8_t saved = config.categories;
  const bool savedEnabled = config.enabled;
  config.categories = VOICE_CAT_ALL;
  config.enabled = true;
  const bool ok = voice_say_index(index, VOICE_CAT_SYSTEM);
  config.categories = saved;
  config.enabled = savedEnabled;
  LOGF("[voice] %s \"%s\"\n", ok ? "queued" : "dropped",
                VOICE_CLIPS[index].text);
  return true;
}
