/*
 * audio_eq.h -- a five-band equaliser that sits in the sample path, wherever
 * the samples happen to come from.
 *
 * Why this is here at all. Two of the three audio sources on this speaker have
 * no tone control of their own: A2DP hands over finished PCM, and the internet
 * radio decoder does the same. The DFPlayer is the exception -- it has six
 * hardware presets and answers command 0x07 -- so in that mode the dashboard
 * drives the module's own equaliser instead and this one stays out of the way
 * (see audio_eq_hw_preset()). Everywhere else, if the owner wants more bass,
 * something on this chip has to make it.
 *
 * What it is. Five biquad sections per channel, at 60 Hz, 250 Hz, 1 kHz, 4 kHz
 * and 12 kHz. The two ends are shelves and the three in the middle are peaking
 * sections, which is the arrangement a physical five-band tone stack uses and
 * for the same reason: a peaking filter at 60 Hz leaves the bottom octave --
 * where a small speaker has nothing anyway -- alone, while a shelf lifts the
 * whole of it, which is what "more bass" means to the person turning the knob.
 *
 * Where it runs. audio_eq_process() is called from whichever task owns the
 * samples: the Bluetooth task for A2DP (from inside LoudVolumeControl, so the
 * analyser and the DAC both see the equalised stream), and the radio task for
 * network audio. Both are audio paths where blocking is not allowed, so:
 *
 *   - it never allocates, never locks, and never calls into anything that does;
 *   - coefficients are recomputed on the *caller's* task -- the web handler --
 *     into a shadow set, and the audio task adopts them at a block boundary by
 *     flipping one index. A torn read is impossible because nothing is ever
 *     written to the set the audio task is reading;
 *   - with every gain at zero and no preamp it returns immediately, so an owner
 *     who never opens the Sound page pays one comparison per buffer.
 *
 * Arithmetic. Single-precision float, because this chip has an FPU and a biquad
 * is five multiplies and four adds. Five bands, two channels, 44100 frames a
 * second is about 4.4 million floating-point operations a second, which is
 * roughly 2% of one core -- measured against the ~15% the SBC decoder itself
 * costs. The transposed direct form II is used because it is the arrangement
 * whose state variables stay small when the coefficients are extreme, which
 * matters here: +12 dB at 60 Hz is a very high-Q section by the standards of
 * anything that has to run in 24 bits of mantissa.
 *
 * Headroom. Boosting a band pushes samples past full scale, and a full-scale
 * stream has none to give. Two things guard that: an automatic preamp, which
 * pulls the whole signal down by however much the largest boost adds, and the
 * same quadratic soft knee main.cpp uses on the volume path, so whatever still
 * arrives over the ceiling is rounded off rather than clipped flat. The
 * automatic preamp can be switched off by an owner who would rather have the
 * loudness and hear the limiter work.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/// How many bands the tone stack has. Fixed: the centre frequencies are chosen
/// together, and the dashboard draws exactly this many sliders.
static const uint8_t EQ_BANDS = 5;

/// Band centre frequencies, in Hz, for the dashboard's labels.
extern const uint16_t EQ_BAND_HZ[EQ_BANDS];

/// The gain range of one band, in dB. Symmetric, and deliberately not larger:
/// past about 12 dB the section's Q makes it ring audibly on percussion.
static const int8_t EQ_GAIN_MIN = -12;
static const int8_t EQ_GAIN_MAX = 12;

/// The manual preamp range, in dB. Asymmetric because its job is mostly to give
/// headroom back, not to take more.
static const int8_t EQ_PREAMP_MIN = -12;
static const int8_t EQ_PREAMP_MAX = 6;

/*
 * The named curves.
 *
 * These are presets in the ordinary sense -- each one writes a set of band
 * gains and then behaves exactly as if the owner had moved the sliders there --
 * with one addition: each also carries a DFPlayer hardware preset to use in the
 * mode where the module does the work instead. That mapping is the only reason
 * the two ideas live in one enum rather than two.
 *
 *   MUSIC       a gentle smile: a little lift at each end, a shallow dip in the
 *               upper mid where a small cabinet is already shouting.
 *   VOICE       the opposite shape. Bass and top cut, presence lifted at 1 kHz
 *               and 4 kHz, which is where consonants live. For podcasts, and
 *               for anything where the point is the words.
 *   BASS        a large low shelf and a small top lift, with everything between
 *               them left alone so the bass arrives without the mid muddying.
 *   NIGHT       the quiet-listening curve. Deep bass pulled down hard -- it is
 *               what carries through a wall -- and the mid brought up, so
 *               speech stays intelligible at a volume that will not wake
 *               anybody. This is a loudness curve run backwards, on purpose.
 */
enum EqPreset : uint8_t {
  EQ_PRESET_FLAT = 0,
  EQ_PRESET_MUSIC,
  EQ_PRESET_VOICE,
  EQ_PRESET_BASS,
  EQ_PRESET_NIGHT,
  EQ_PRESET_CUSTOM,  ///< the sliders were moved; not one of the curves above
  EQ_PRESET_COUNT
};

/// The stored shape of the tone stack. This is what NVS holds and what the
/// dashboard edits; the coefficients are derived from it and are not stored.
struct EqConfig {
  bool enabled;           ///< false bypasses the whole thing, sliders and all
  uint8_t preset;         ///< an EqPreset; EQ_PRESET_CUSTOM once a slider moves
  int8_t gain[EQ_BANDS];  ///< dB per band, EQ_GAIN_MIN..EQ_GAIN_MAX
  int8_t preamp;          ///< dB applied to everything, EQ_PREAMP_MIN..MAX
  bool autoPreamp;        ///< subtract the largest boost automatically
};

/// The factory shape: on, flat, automatic preamp armed.
void audio_eq_defaults(EqConfig *out);

/// Fills `out` with the gains a named preset stands for. EQ_PRESET_CUSTOM
/// leaves `out` untouched and returns false, which is what makes "pick Custom"
/// mean "keep what is on screen".
bool audio_eq_preset_gains(uint8_t preset, int8_t *out);

/// The preset's name, for the dashboard and the console.
const char *audio_eq_preset_name(uint8_t preset);

/// The DFPlayer hardware equaliser this preset maps to (0 normal, 1 pop,
/// 2 rock, 3 jazz, 4 classic, 5 bass). Used in DFPlayer mode, where the module
/// has its own tone control and this one is not in the sample path at all.
uint8_t audio_eq_hw_preset(uint8_t preset);

/// Installs a configuration. Safe from any task: the coefficients are computed
/// here, on the caller, into the set the audio task is *not* reading, and the
/// swap is a single store. Clamps everything it is given.
void audio_eq_configure(const EqConfig &cfg);

/// The live configuration, as stored.
void audio_eq_get(EqConfig *out);

/// Tells the equaliser what rate the samples arriving at audio_eq_process() are
/// at, so the coefficients can be rebuilt for it. Cheap and idempotent -- it
/// returns immediately when the rate has not changed -- so the A2DP and radio
/// paths can both call it whenever their source tells them the rate, which for
/// a stream that switches from 44.1 to 48 kHz mid-session is the only warning
/// there is. Safe from the audio task.
void audio_eq_set_sample_rate(uint32_t hz);

/*
 * Filters one buffer of interleaved 16-bit stereo, in place.
 *
 * `frames` is sample *pairs*, so the buffer is 2 * frames int16s long. Returns
 * immediately when the equaliser is bypassed or flat, which is the common case
 * and the reason this is safe to call unconditionally from the audio path.
 *
 * Audio task only, and one task at a time: the filter state is per-instance and
 * carrying it across a task switch would be a discontinuity in the output, not
 * merely a race. The two callers are in mutually exclusive radio modes, so this
 * costs nothing to guarantee.
 */
void audio_eq_process(int16_t *interleaved, size_t frames);

/// True when audio_eq_process() would do something -- enabled, and not flat.
/// The dashboard shows this as "active", so a bypassed-because-flat equaliser
/// does not read as broken.
bool audio_eq_active();

/// The gain reduction the automatic preamp is currently applying, in dB, as a
/// negative number. Reported so the dashboard can explain where the loudness
/// went when a big boost is dialled in.
float audio_eq_headroom_db();

/// Serial console: "eq", "eq off", "eq flat|music|voice|bass|night",
/// "eq <band 1-5> <dB>". Returns false if the line was something else.
bool audio_eq_command(const char *line);
