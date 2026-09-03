/*
 * voice.h -- spoken announcements.
 *
 * What it says and where the words come from. Everything this can say was
 * spoken into scripts/voice_phrases.txt by a person editing a text file, read
 * out by the Windows speech engine, and embedded in the firmware as IMA ADPCM
 * by scripts/make_voice_clips.py. There is no synthesiser on the chip and no
 * network call at announcement time: a clip either exists in flash or it is not
 * said. That is the trade this design makes, and it buys the two properties
 * that matter for the things worth announcing -- "battery critically low" works
 * with no network, in every radio mode, and it sounds like a person.
 *
 * The cost is the obvious one: the speaker cannot read out a name it has never
 * been given. "Connected" it can say; "connected to Ashkan's iPhone" it can say
 * only if somebody added that line to the phrase file and rebuilt. The
 * dashboard leans into that rather than hiding it -- it lists every clip in the
 * firmware and lets a Bluetooth address be pointed at one, so the announcement
 * for a known phone is a mapping rather than a synthesis.
 *
 * How a clip reaches the speaker. Two paths, because there are two situations:
 *
 *   nothing playing   loop() notices a queued clip, and, exactly as it does for
 *                     the melodies in main.cpp, renders it into a buffer and
 *                     writes that to I2S itself. This is voice_render().
 *   music playing     the announcement is mixed into the stream on the audio
 *                     task, and the music is ducked underneath it. This is
 *                     voice_mix(), called from LoudVolumeControl for A2DP and
 *                     from the radio player for network audio.
 *
 * They are the same decoder and the same queue; which one drains it is decided
 * by an owner flag, so a clip can never be pulled from both at once and played
 * twice at half speed. Ducking ramps over about 60 ms in each direction -- fast
 * enough to get out of the way of the first syllable, slow enough not to sound
 * like a fault.
 *
 * What it costs. ADPCM is a dozen integer operations per sample and the
 * resampler is two multiplies; at 44.1 kHz stereo that is well under 1% of a
 * core, and only while something is actually being said. The queue is four
 * deep and statically allocated. Nothing here allocates, blocks or logs from an
 * audio path.
 *
 * In Bluetooth mode there is no dashboard to configure any of this, so the
 * settings are read from NVS at boot like everything else and the announcements
 * work exactly as they do in the Wi-Fi modes. That is deliberate: the battery
 * warning is most useful in the mode that has no other way to tell you.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "voice_clips.h"

/*
 * What may speak.
 *
 * A mask rather than one switch because the announcements differ enormously in
 * how welcome they are. "Battery critically low" is worth interrupting music
 * for; "device connected" said every time a phone wanders back into range is
 * the kind of thing that gets a feature switched off entirely. Splitting them
 * means the owner turns off the annoying half and keeps the useful half.
 */
enum VoiceCategory : uint8_t {
  VOICE_CAT_SYSTEM = 1 << 0,      ///< boot, shutdown, radio mode
  VOICE_CAT_CONNECTION = 1 << 1,  ///< Bluetooth and Wi-Fi coming and going
  VOICE_CAT_BATTERY = 1 << 2,     ///< low, critical, charging, full
  VOICE_CAT_RADIO = 1 << 3,       ///< internet radio connecting and failing
  VOICE_CAT_ALARM = 1 << 4,       ///< the alarm, snooze, the sleep timer
  VOICE_CAT_ALL = 0x1F,
};

struct VoiceConfig {
  bool enabled;      ///< the master switch
  uint8_t volume;    ///< 0..100, relative to whatever the source is doing
  uint8_t categories;  ///< a bitwise or of VoiceCategory
  uint8_t duck;      ///< 0..100, how far music is pulled down under a clip
};

/// The factory shape: on, categories on except the chatty connection one,
/// two thirds volume, music ducked to a quarter.
void voice_defaults(VoiceConfig *out);

void voice_configure(const VoiceConfig &cfg);
void voice_get(VoiceConfig *out);

/*
 * Queues a clip.
 *
 * Safe from any task, including a Bluetooth callback: it writes one slot in a
 * ring buffer and returns. Nothing is decoded here.
 *
 * `category` is checked against the configured mask, so callers do not have to
 * -- "say the battery warning" is one call whether or not battery announcements
 * are switched on. Returns false when the clip was dropped, which is either
 * because its category is off, because the queue is full, or because voice is
 * off altogether; no caller has ever needed to care, but the console does.
 */
bool voice_say(VoiceClipId clip, VoiceCategory category);

/// As above, taking the clip as a plain index -- for the device-name mapping,
/// where the clip is whatever the owner pointed a Bluetooth address at and is
/// not known at compile time. Out-of-range indices are dropped.
bool voice_say_index(uint8_t clip, VoiceCategory category);

/// Drops anything queued and stops what is being said. For the moment the owner
/// switches announcements off, and for standby.
void voice_silence();

/// True when there is something queued or in progress -- the flag loop() and
/// the melody code both consult before taking the DAC.
bool voice_busy();

/// Tells the decoder what rate the buffers it is asked to fill are at. Cheap
/// and idempotent. Safe from the audio task.
void voice_set_sample_rate(uint32_t hz);

/*
 * Renders the current announcement into an empty stereo buffer.
 *
 * For the case where nothing else is using the DAC: loop() calls this and
 * writes the result to I2S, the same shape as the melodies. Returns the number
 * of frames written, which is 0 when there is nothing to say -- or when the
 * mixing path has already claimed the clip, which is what stops the two from
 * both playing it.
 *
 * `frames` is sample pairs; the buffer must hold 2 * frames int16s.
 */
size_t voice_render(int16_t *interleaved, size_t frames);

/*
 * Mixes the current announcement into a buffer that already has music in it,
 * ducking the music underneath.
 *
 * Called from the audio task, on every buffer, whether or not anything is being
 * said: with nothing queued and the duck ramp at rest it returns after one
 * comparison. Returns true while it is doing something, which the radio player
 * uses to keep its own metadata display honest.
 */
bool voice_mix(int16_t *interleaved, size_t frames);

/// The clip table, for the dashboard's picker. `index` is what voice_say_index()
/// takes and what the device mapping stores.
uint8_t voice_clip_count();
const char *voice_clip_id(uint8_t index);
const char *voice_clip_text(uint8_t index);

/// Looks a clip up by the id in the phrase file. Returns VOICE_CLIP_COUNT when
/// there is no such clip, which is what a settings restore from a firmware with
/// a different phrase list produces and must survive.
uint8_t voice_clip_by_id(const char *id);

/// Serial console: "say", "say <id>", "say off|on". Returns false if the line
/// was something else.
bool voice_command(const char *line);
