#include "audio_probe.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <math.h>
#include <string.h>

#include "app_config.h"

// ------------------------------------------------------------------- ring ----
// 1024 decimated samples is 46 ms at 22.05 kHz: enough for the FFT window plus
// slack for the oscilloscope trigger to hunt for a zero crossing.
static const uint16_t RING_SIZE = 1024;  // power of two, masked not divided
static const uint16_t RING_MASK = RING_SIZE - 1;

static int16_t ring[RING_SIZE];
static uint32_t ring_written;  // monotonic count, wraps harmlessly after days

/*
 * Whether the feed is still running.
 *
 * The ring is not cleared when playback stops -- nothing calls it, and the
 * writer is a hot path that should not be made to. So the newest FFT_SIZE
 * samples stay exactly as the last track left them, and analysing them again
 * reports that same music, at that same level, for as long as the speaker is
 * powered. `active` therefore never went false once anything had ever played,
 * which is what kept the panel awake and the ring lit.
 *
 * The fix costs nothing on the writer's side: analyse_locked() watches the
 * write counter itself. A counter that has not moved for PROBE_STALE_MS means
 * no new audio, and the window is treated as the silence it is.
 */
static uint32_t ring_head_seen;
static uint32_t ring_head_at;

/// A2DP packets land every few milliseconds and the analysis runs at most every
/// 16, so a quarter of a second of no new samples is a stopped stream and not a
/// stalled one. Short enough that the display and the ring settle promptly,
/// long enough to ride out a network stream refilling its buffer.
static const uint32_t PROBE_STALE_MS = 250;

// Decimation state: the first sample of each pair waits here for its partner.
static int32_t pair_hold;
static bool pair_pending;

// Per-channel accumulators, drained by analyse(). Guarded by a spinlock rather
// than a mutex: the critical section is a handful of instructions, and this one
// is entered from the Bluetooth task where blocking is not acceptable.
static portMUX_TYPE meter_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t meter_sq_l, meter_sq_r;  // sum of (sample >> 7)^2
static uint32_t meter_n;
static uint16_t meter_pk_l, meter_pk_r;

// ------------------------------------------------------------------- fft -----
static float fft_re[FFT_SIZE];
static float fft_im[FFT_SIZE];
static float fft_win[FFT_SIZE];        // Hann
static float tw_re[FFT_SIZE / 2];      // twiddles
static float tw_im[FFT_SIZE / 2];
static uint16_t bit_rev[FFT_SIZE];
// uint16_t, not uint8_t: a bin index only fits in a byte while FFT_SIZE stays
// at or below 512, and that is not a limit worth hiding here.
static uint16_t band_lo[VIS_BANDS];    // first bin of each band
static uint16_t band_hi[VIS_BANDS];    // last bin, inclusive
static float band_tilt[VIS_BANDS];     // dB added to compensate spectral tilt

// --------------------------------------------------------------- analysis ----
static AudioVis vis;

// Serialises the analysis itself, so the display and lighting tasks share one
// FFT rather than racing to redo it. Created in audio_probe_init(); see the
// service block at the bottom of this file for how it is used.
static SemaphoreHandle_t analyse_gate;
static float band_val[VIS_BANDS];   // 0..1, smoothed bar height
static float peak_val[VIS_BANDS];   // 0..1, peak-hold cap
static uint16_t peak_hang[VIS_BANDS];
static float agc_db = -12.0f;
static float bass_avg;
static uint32_t last_beat_ms;
static uint32_t last_active_ms;
static float vu_l_f, vu_r_f, pk_l_f, pk_r_f;

/// Noise floor. Below this a band is drawn as nothing at all, which matters
/// because the SBC decoder never outputs exact silence.
static const float FLOOR_DB = -78.0f;

/// How far above the floor the loudest band has to reach for the frame to count
/// as audio. Applied to the *untilted* peak, so it is a plain dBFS threshold:
/// FLOOR_DB + 8 is -70 dBFS, which is far below anything anybody listens to and
/// above the dither a source puts on a silent stream. Raise it if a paused
/// source still reads as playing on your hardware; every consumer of `active`
/// -- the idle timers, the visualiser screens, the beat detector -- follows it.
static const float ACTIVE_ABOVE_FLOOR_DB = 8.0f;

/// The last untilted peak, in dBFS, purely so the dashboard can show what the
/// analyser is actually hearing. "It says nothing is playing and it is" is not
/// a thing anybody should have to guess at twice.
static uint32_t last_peak_word;

static void publish_peak_db(float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  __atomic_store_n(&last_peak_word, bits, __ATOMIC_RELEASE);
}
/// The auto-gain ceiling is clamped here so that near-silence does not get
/// amplified into a full-height display of dither noise.
static const float AGC_MIN_DB = -42.0f;
/// Extra dB given to the top band relative to the bottom one. Music loses
/// roughly this much energy from bass to treble, so without it every spectrum
/// display is a wedge that slopes down to the right.
static const float TILT_TOP_DB = 11.0f;

void audio_probe_init() {
  analyse_gate = xSemaphoreCreateMutex();
  if (!analyse_gate) {
    LOGLN("[audio] no memory for analyser lock; spectrum is disabled");
  }
  publish_peak_db(FLOOR_DB);

  // Hann window: the cheapest window that keeps a single tone from smearing
  // across half the display.
  for (uint16_t i = 0; i < FFT_SIZE; i++) {
    fft_win[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (FFT_SIZE - 1));
  }

  for (uint16_t k = 0; k < FFT_SIZE / 2; k++) {
    const float a = -2.0f * (float)M_PI * k / FFT_SIZE;
    tw_re[k] = cosf(a);
    tw_im[k] = sinf(a);
  }

  // Bit-reversal permutation, so the transform below can run in place.
  uint8_t bits = 0;
  while ((1u << bits) < FFT_SIZE) bits++;
  for (uint16_t i = 0; i < FFT_SIZE; i++) {
    uint16_t r = 0;
    for (uint8_t b = 0; b < bits; b++) {
      if (i & (1u << b)) r |= 1u << (bits - 1 - b);
    }
    bit_rev[i] = r;
  }

  /*
   * Log-spaced band edges over bins 1..N/2-1. At 22.05 kHz with FFT_SIZE 256
   * that is 86 Hz to 10.9 kHz, which lines up well with what a 32-bar display
   * can usefully distinguish.
   *
   * Linear spacing would put 28 of the 32 bars above 2 kHz, where music has
   * almost no energy, and cram the entire bass range into the first bar.
   */
  const uint16_t top_bin = FFT_SIZE / 2 - 1;
  uint16_t prev_hi = 0;
  for (uint8_t b = 0; b < VIS_BANDS; b++) {
    const float f0 = (float)b / VIS_BANDS;
    const float f1 = (float)(b + 1) / VIS_BANDS;
    uint16_t lo = (uint16_t)(1.0f * powf((float)top_bin, f0) + 0.5f);
    uint16_t hi = (uint16_t)(1.0f * powf((float)top_bin, f1) + 0.5f);
    if (lo <= prev_hi) lo = prev_hi + 1;  // no bin counted twice
    if (lo > top_bin) lo = top_bin;
    if (hi < lo) hi = lo;
    if (hi > top_bin) hi = top_bin;
    band_lo[b] = lo;
    band_hi[b] = hi;
    prev_hi = hi;
    band_tilt[b] = TILT_TOP_DB * (float)b / (VIS_BANDS - 1);
  }

  memset(&vis, 0, sizeof(vis));
}

// ------------------------------------------------------------------- feed ----
void audio_probe_feed(const Frame *frames, uint16_t count) {
  if (frames == nullptr || count == 0) return;

  uint32_t sq_l = 0, sq_r = 0;
  uint16_t pk_l = 0, pk_r = 0;
  uint32_t head = __atomic_load_n(&ring_written, __ATOMIC_ACQUIRE);

  for (uint16_t i = 0; i < count; i++) {
    const int32_t l = frames[i].channel1;
    const int32_t r = frames[i].channel2;

    // Meters. Squaring 16-bit samples overflows a uint32 accumulator after a
    // few thousand frames, so shift down by 7 first: the result is still a
    // faithful ratio, and the dB conversion in analyse() undoes the scaling.
    const int32_t ls = l >> 7, rs = r >> 7;
    sq_l += (uint32_t)(ls * ls);
    sq_r += (uint32_t)(rs * rs);
    const uint16_t al = (uint16_t)(l < 0 ? -l : l);
    const uint16_t ar = (uint16_t)(r < 0 ? -r : r);
    if (al > pk_l) pk_l = al;
    if (ar > pk_r) pk_r = ar;

    // Mono downmix, then decimate 2:1 by averaging the pair. The average is
    // also a (very gentle) low-pass, which is what keeps content above 11 kHz
    // from folding back down into the visible part of the spectrum.
    const int32_t mono = (l + r) >> 1;
    if (!pair_pending) {
      pair_hold = mono;
      pair_pending = true;
    } else {
      ring[head & RING_MASK] = (int16_t)((pair_hold + mono) >> 1);
      head++;
      pair_pending = false;
    }
  }

  // Publish the new samples only after they are all stored, so a reader can
  // never be pointed at a slot that has not been written yet.
  __atomic_store_n(&ring_written, head, __ATOMIC_RELEASE);

  portENTER_CRITICAL(&meter_mux);
  meter_sq_l += sq_l;
  meter_sq_r += sq_r;
  meter_n += count;
  if (pk_l > meter_pk_l) meter_pk_l = pk_l;
  if (pk_r > meter_pk_r) meter_pk_r = pk_r;
  portEXIT_CRITICAL(&meter_mux);
}

// -------------------------------------------------------------------- fft ----
/// In-place iterative radix-2 Cooley-Tukey. fft_re/fft_im hold the input in
/// bit-reversed order on entry and the spectrum in natural order on exit.
static void fft_run() {
  for (uint16_t len = 2; len <= FFT_SIZE; len <<= 1) {
    const uint16_t half = len >> 1;
    const uint16_t step = FFT_SIZE / len;
    for (uint16_t i = 0; i < FFT_SIZE; i += len) {
      for (uint16_t j = 0; j < half; j++) {
        const uint16_t k = j * step;
        const float wr = tw_re[k], wi = tw_im[k];
        const uint16_t a = i + j, b = a + half;
        const float xr = fft_re[b] * wr - fft_im[b] * wi;
        const float xi = fft_re[b] * wi + fft_im[b] * wr;
        fft_re[b] = fft_re[a] - xr;
        fft_im[b] = fft_im[a] - xi;
        fft_re[a] += xr;
        fft_im[a] += xi;
      }
    }
  }
}

/// Amplitude ratio to dBFS, with the noise floor as the lower clamp.
static inline float to_db(float amplitude_0_1) {
  if (amplitude_0_1 <= 1e-7f) return FLOOR_DB;
  const float db = 20.0f * log10f(amplitude_0_1);
  return db < FLOOR_DB ? FLOOR_DB : db;
}

// -------------------------------------------------------- oscilloscope -------
/*
 * Fills vis.wave from the ring.
 *
 * The trace is trigger-aligned: instead of drawing whatever samples happen to
 * be newest, it looks backwards for a rising zero crossing and starts there.
 * Without that, a steady tone slides sideways a random amount every frame and
 * looks like noise; with it, the waveform sits still the way a scope does.
 */
static void fill_wave(uint32_t head, float norm) {
  const uint16_t span = VIS_WAVE_POINTS;
  if (head < span * 2) {
    memset(vis.wave, 0, sizeof(vis.wave));
    return;
  }

  // Search window: the span before the newest span, so that whatever trigger we
  // find still has a full trace after it.
  uint32_t start = head - span * 2;
  for (uint16_t i = 1; i < span; i++) {
    const int16_t a = ring[(start + i - 1) & RING_MASK];
    const int16_t b = ring[(start + i) & RING_MASK];
    if (a <= 0 && b > 0) {  // rising edge through zero
      start += i;
      break;
    }
  }

  for (uint16_t i = 0; i < span; i++) {
    float v = ring[(start + i) & RING_MASK] * norm;
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    vis.wave[i] = (int8_t)(v * 127.0f);
  }
}

// ---------------------------------------------------------------- analyse ----
static void analyse_locked(uint32_t dt_ms) {
  const uint32_t now = millis();
  if (dt_ms == 0) dt_ms = 1;
  if (dt_ms > 250) dt_ms = 250;  // after a long stall, do not snap everything
  const float dt = dt_ms * 0.001f;

  // --- drain the meters ---------------------------------------------------
  uint32_t sq_l, sq_r, n;
  uint16_t pk_l, pk_r;
  portENTER_CRITICAL(&meter_mux);
  sq_l = meter_sq_l;
  sq_r = meter_sq_r;
  n = meter_n;
  pk_l = meter_pk_l;
  pk_r = meter_pk_r;
  meter_sq_l = meter_sq_r = meter_n = 0;
  meter_pk_l = meter_pk_r = 0;
  portEXIT_CRITICAL(&meter_mux);

  // --- copy the newest FFT_SIZE samples out of the ring -------------------
  // Stale samples are silence: see the note on ring_head_seen above.
  const uint32_t head = __atomic_load_n(&ring_written, __ATOMIC_ACQUIRE);
  if (head != ring_head_seen) {
    ring_head_seen = head;
    ring_head_at = now;
  }
  const bool fed = ring_head_at != 0 && (now - ring_head_at) < PROBE_STALE_MS;
  const bool have_samples = fed && head >= FFT_SIZE;

  float window_peak = 1.0f;
  if (have_samples) {
    const uint32_t base = head - FFT_SIZE;
    for (uint16_t i = 0; i < FFT_SIZE; i++) {
      const int16_t s = ring[(base + i) & RING_MASK];
      const uint16_t a = (uint16_t)(s < 0 ? -s : s);
      if (a > window_peak) window_peak = a;
      // Read straight into bit-reversed position, so fft_run() can work in
      // place without a separate shuffle pass.
      const uint16_t d = bit_rev[i];
      fft_re[d] = s * fft_win[i];
      fft_im[d] = 0.0f;
    }
    fft_run();
  } else {
    memset(fft_re, 0, sizeof(fft_re));
    memset(fft_im, 0, sizeof(fft_im));
  }

  // --- bins to bands ------------------------------------------------------
  // A Hann-windowed FFT of a full-scale sine puts amplitude N/4 in its bin, so
  // that is the divisor that makes "1.0" mean digital full scale.
  const float bin_scale = 1.0f / (32768.0f * FFT_SIZE * 0.25f);

  float raw_db[VIS_BANDS];
  float frame_max_db = FLOOR_DB;
  /*
   * The same peak with the display tilt left off, and the only thing `active`
   * is allowed to look at.
   *
   * band_tilt is cosmetic: it lifts the high bands so a spectrum of real music
   * fills the display evenly instead of sloping off to the right. But it is
   * added *after* to_db() has already clamped at FLOOR_DB, so digital silence
   * does not come out at the floor -- it comes out at the floor plus the tilt,
   * which at the top band is FLOOR_DB + 11. Tested against FLOOR_DB + 8, that
   * is louder than the threshold, so silence read as audio: `active` was true
   * from boot, in every mode, whether or not anything was connected.
   *
   * Everything downstream believed it. The idle timers never counted, so the
   * panel never blanked and the ring never rested; the visualiser screens were
   * always eligible. The tilt stays where it is for the bars, and the test now
   * reads the untilted peak.
   */
  float frame_peak_db = FLOOR_DB;
  float bass_energy = 0.0f;

  for (uint8_t b = 0; b < VIS_BANDS; b++) {
    float best = 0.0f;
    for (uint16_t k = band_lo[b]; k <= band_hi[b]; k++) {
      const float m = fft_re[k] * fft_re[k] + fft_im[k] * fft_im[k];
      if (m > best) best = m;  // peak, not mean: peaks look like music
    }
    const float flat_db = to_db(sqrtf(best) * bin_scale);
    const float db = flat_db + band_tilt[b];
    raw_db[b] = db;
    if (db > frame_max_db) frame_max_db = db;
    if (flat_db > frame_peak_db) frame_peak_db = flat_db;
    if (b < 4) bass_energy += sqrtf(best) * bin_scale;
  }

  publish_peak_db(frame_peak_db);
  vis.active = frame_peak_db > FLOOR_DB + ACTIVE_ABOVE_FLOOR_DB;
  if (vis.active) {
    __atomic_store_n(&last_active_ms, now, __ATOMIC_RELEASE);
  }

  /*
   * Auto-gain.
   *
   * The phone attenuates in software before we ever see the samples, so at a
   * comfortable listening volume the stream can easily sit 20 dB below full
   * scale -- and a fixed 0 dBFS ceiling would then show two-pixel stubs. The
   * ceiling instead tracks the loudest band: it jumps up instantly so a
   * transient is never clipped off the top, and releases downward over
   * VIS_AGC_RELEASE_S so a quiet passage slowly gets the full height back.
   */
  if (frame_max_db > agc_db) {
    agc_db = frame_max_db;
  } else {
    const float k = dt / VIS_AGC_RELEASE_S;
    agc_db += (frame_max_db - agc_db) * (k > 1.0f ? 1.0f : k);
  }
  if (agc_db < AGC_MIN_DB) agc_db = AGC_MIN_DB;
  if (agc_db > 6.0f) agc_db = 6.0f;
  vis.agc_db = agc_db;

  const float fall = VIS_FALL_PER_S * dt;
  const float peak_fall = VIS_PEAK_FALL_PER_S * dt;
  const float span_db = VIS_RANGE_DB;
  uint8_t loudest = 0;

  for (uint8_t b = 0; b < VIS_BANDS; b++) {
    float target = (raw_db[b] - (agc_db - span_db)) / span_db;
    if (target < 0.0f) target = 0.0f;
    if (target > 1.0f) target = 1.0f;

    // Instant attack, timed decay. This asymmetry is the entire reason a
    // hardware analyser feels alive and a naive lerp feels like syrup.
    if (target >= band_val[b]) {
      band_val[b] = target;
    } else {
      band_val[b] -= fall;
      if (band_val[b] < target) band_val[b] = target;
    }

    if (band_val[b] >= peak_val[b]) {
      peak_val[b] = band_val[b];
      peak_hang[b] = VIS_PEAK_HANG_MS;
    } else if (peak_hang[b] > dt_ms) {
      peak_hang[b] -= dt_ms;
    } else {
      peak_hang[b] = 0;
      peak_val[b] -= peak_fall;
      if (peak_val[b] < band_val[b]) peak_val[b] = band_val[b];
    }

    vis.bands[b] = (uint8_t)(band_val[b] * 255.0f);
    vis.peaks[b] = (uint8_t)(peak_val[b] * 255.0f);
    if (vis.bands[b] > loudest) loudest = vis.bands[b];
  }
  vis.level = loudest;

  // --- beat -----------------------------------------------------------------
  /*
   * Cheapest beat detector that actually works on real music: compare the
   * energy of the bottom four bands against its own running average. A kick
   * drum is a short burst several times louder than the local average, so a
   * fixed ratio plus a refractory period catches it without needing tempo
   * tracking. Used only for accents in the UI, so a missed beat costs nothing.
   */
  const float bass_k = dt / 0.5f;
  bass_avg += (bass_energy - bass_avg) * (bass_k > 1.0f ? 1.0f : bass_k);
  vis.beat = false;
  if (vis.active && bass_energy > bass_avg * 1.35f && bass_energy > 0.004f &&
      now - last_beat_ms > 170) {
    vis.beat = true;
    last_beat_ms = now;
  }

  // --- VU -------------------------------------------------------------------
  // RMS of the >>7 accumulators back to a 0..1 ratio: sqrt(sum/n) * 128/32768.
  auto rms_ratio = [](uint32_t sq, uint32_t count) -> float {
    if (count == 0) return 0.0f;
    return sqrtf((float)sq / (float)count) * (128.0f / 32768.0f);
  };
  const float vu_range = 48.0f;  // dB shown across the meter
  auto db_to_unit = [&](float ratio) -> float {
    const float db = to_db(ratio);
    float u = (db + vu_range) / vu_range;
    if (u < 0.0f) u = 0.0f;
    if (u > 1.0f) u = 1.0f;
    return u;
  };

  const float tl = db_to_unit(rms_ratio(sq_l, n));
  const float tr = db_to_unit(rms_ratio(sq_r, n));

  // Meters get a short integration time on the way up (so a snare registers)
  // and a slow fall, which is the ballistics of a real VU movement.
  auto approach = [](float cur, float target, float rise, float fall) -> float {
    float k = target > cur ? rise : fall;
    if (k > 1.0f) k = 1.0f;
    return cur + (target - cur) * k;
  };
  const float vu_rise = dt / 0.05f, vu_fall = dt / 0.35f;
  vu_l_f = approach(vu_l_f, tl, vu_rise, vu_fall);
  vu_r_f = approach(vu_r_f, tr, vu_rise, vu_fall);

  const float hl = db_to_unit(pk_l / 32768.0f);
  const float hr = db_to_unit(pk_r / 32768.0f);
  pk_l_f = hl > pk_l_f ? hl : pk_l_f - peak_fall;
  pk_r_f = hr > pk_r_f ? hr : pk_r_f - peak_fall;
  if (pk_l_f < vu_l_f) pk_l_f = vu_l_f;
  if (pk_r_f < vu_r_f) pk_r_f = vu_r_f;

  vis.vu_l = (uint8_t)(vu_l_f * 255.0f);
  vis.vu_r = (uint8_t)(vu_r_f * 255.0f);
  vis.peak_l = (uint8_t)(pk_l_f * 255.0f);
  vis.peak_r = (uint8_t)(pk_r_f * 255.0f);

  // --- waveform -------------------------------------------------------------
  // Normalise to the window peak so a quiet passage still fills the trace, with
  // a floor so that silence stays a flat line instead of amplified dither.
  fill_wave(head, have_samples ? 1.0f / (window_peak < 900.0f ? 900.0f
                                                             : window_peak)
                               : 0.0f);
}

// ---------------------------------------------------------------- service ----
/*
 * One analysis, any number of watchers. See the note at the top of the header.
 *
 * `analyse_gate` serialises the FFT itself and is only ever *tried*, never
 * waited on. A short cross-core critical section protects the published frame;
 * it is used only by the two low-priority visual tasks, never by PCM input.
 */
static uint32_t analysed_at;      // millis() of the last real analysis
static AudioVis published;
static portMUX_TYPE publish_mux = portMUX_INITIALIZER_UNLOCKED;

static void publish() {
  portENTER_CRITICAL(&publish_mux);
  memcpy(&published, &vis, sizeof(published));
  portEXIT_CRITICAL(&publish_mux);
}

void audio_probe_frame(AudioVis *out, uint16_t min_interval_ms) {
  if (!out) return;
  if (analyse_gate && xSemaphoreTake(analyse_gate, 0) == pdTRUE) {
    const uint32_t now = millis();
    const uint32_t age = now - analysed_at;
    if (age >= min_interval_ms) {
      analysed_at = now;
      analyse_locked(age);
      publish();
    }
    xSemaphoreGive(analyse_gate);
  }

  portENTER_CRITICAL(&publish_mux);
  memcpy(out, &published, sizeof(*out));
  portEXIT_CRITICAL(&publish_mux);
}

uint32_t audio_probe_last_active() {
  return __atomic_load_n(&last_active_ms, __ATOMIC_ACQUIRE);
}

float audio_probe_peak_db() {
  const uint32_t bits = __atomic_load_n(&last_peak_word, __ATOMIC_ACQUIRE);
  float value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}
