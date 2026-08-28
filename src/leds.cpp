#include "leds.h"

#if LEDS_ENABLED

#include <Arduino.h>
#include <driver/rmt_tx.h>
#include <math.h>
#include <string.h>

#include "audio_probe.h"

// ------------------------------------------------------------------ strip ----
/*
 * The wire protocol, straight onto the RMT peripheral.
 *
 * A WS2812B has no clock line: a bit is a pulse whose *width* says whether it
 * is a one or a zero, 1.25 us apart, and a frame is 24 of those per pixel with
 * no gaps. Bit-banging that means disabling interrupts for the length of the
 * frame, which on a device with an audio path is not a trade worth making --
 * and a bit-banger that is preempted anyway writes visible garbage.
 *
 * RMT is the hardware answer: the symbols are handed over once and clocked out
 * by the peripheral while the CPU does something else.
 *
 * This talks to the IDF's rmt_tx driver rather than to Adafruit_NeoPixel or to
 * the Arduino core's rmtInit()/rmtWrite() wrapper, and both of those choices
 * are about IRAM rather than taste. IRAM is the binding constraint on this
 * chip -- the Bluetooth controller alone holds 32 KB of the 128 KB, and it is
 * the one memory that cannot be traded for any other. The NeoPixel library
 * puts its transmit path there and would not link at all. The Arduino wrapper
 * implements receive in the same translation unit as transmit, so linking it
 * drags rmt_rx.c in too: 523 bytes of IRAM for a direction this speaker has no
 * use for. Going one layer down costs about thirty lines and buys that back.
 *
 * At a 10 MHz tick each unit is 0.1 us, so a bit is:
 *
 *   0 bit   high 0.3 us, low 0.9 us      datasheet: 0.4 +/- 0.15, 0.85 +/- 0.15
 *   1 bit   high 0.9 us, low 0.3 us      datasheet: 0.8 +/- 0.15, 0.45 +/- 0.15
 *
 * which is the timing the IDF's own led_strip driver uses. The latch is a low
 * line for 50 us or more; at 60 fps the gap between frames is 16 ms, so it
 * happens on its own and there is nothing to wait for.
 */
static const uint32_t RMT_TICK_HZ = 10000000;  // 0.1 us per tick
static const uint16_t T0H = 3, T0L = 9, T1H = 9, T1L = 3;

/// 24 symbols per pixel, one per bit.
static const size_t SYMBOL_COUNT = (size_t)LED_COUNT * 24;
static rmt_symbol_word_t symbols[SYMBOL_COUNT];

/// RMT memory reserved for the channel, in symbols. Must be a multiple of the
/// 64-symbol hardware block. Three blocks is 192, comfortably more than the 168
/// a seven-pixel frame needs, so the whole frame is loaded once and clocked out
/// with no refill interrupt at all -- which is the point on a board whose other
/// interrupts are carrying audio. A longer strip still works; the driver simply
/// goes back to refilling the block as it drains.
static const size_t RMT_MEM_SYMBOLS = 192;

static rmt_channel_handle_t rmt_channel;
static rmt_encoder_handle_t rmt_encoder;
static bool g_present;

// ----------------------------------------------------------------- config ----
/*
 * Written by the web task, read by the render task. A spinlock rather than a
 * mutex: the critical section is a twelve-byte copy, and taking a mutex on the
 * render task once a frame to protect that would cost more than the copy.
 */
static portMUX_TYPE cfg_mux = portMUX_INITIALIZER_UNLOCKED;
static LedConfig cfg = {
    true,          LED_DEFAULT_EFFECT,     LED_DEFAULT_BRIGHTNESS,
    LED_DEFAULT_SPEED, LED_DEFAULT_REACTIVITY, LED_DEFAULT_COLOR,
    LED_DEFAULT_COLOR2};

// ------------------------------------------------------------------ colour ---
static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
static inline uint8_t red_of(uint32_t c) { return (uint8_t)(c >> 16); }
static inline uint8_t grn_of(uint32_t c) { return (uint8_t)(c >> 8); }
static inline uint8_t blu_of(uint32_t c) { return (uint8_t)c; }

/// Scales a colour by 0..1. Plain multiplication, not gamma: the gamma curve is
/// applied once at the very end, in commit(), so it is never applied twice to a
/// colour that has been through two effects.
static uint32_t dim(uint32_t c, float f) {
  if (f <= 0.0f) return 0;
  if (f > 1.0f) f = 1.0f;
  return rgb((uint8_t)(red_of(c) * f), (uint8_t)(grn_of(c) * f),
             (uint8_t)(blu_of(c) * f));
}

static uint32_t mix(uint32_t a, uint32_t b, float f) {
  if (f <= 0.0f) return a;
  if (f >= 1.0f) return b;
  return rgb((uint8_t)(red_of(a) + (red_of(b) - red_of(a)) * f),
             (uint8_t)(grn_of(a) + (grn_of(b) - grn_of(a)) * f),
             (uint8_t)(blu_of(a) + (blu_of(b) - blu_of(a)) * f));
}

/// Hue is 0..65535 around the wheel, saturation and value 0..255. Integer
/// throughout: this is called up to LED_COUNT times a frame and the ESP32's FPU
/// is single precision, so there is nothing to gain from floats here.
static uint32_t hsv(uint16_t h, uint8_t s, uint8_t v) {
  if (s == 0) return rgb(v, v, v);
  const uint8_t region = (uint8_t)(h / 10923u);  // 65536/6, so 0..5
  const uint8_t f = (uint8_t)(((h - (uint32_t)region * 10923u) * 6u) >> 8);
  const uint8_t p = (uint8_t)((v * (255 - s)) / 255);
  const uint8_t q = (uint8_t)((v * (255 - ((s * f) / 255))) / 255);
  const uint8_t t = (uint8_t)((v * (255 - ((s * (255 - f)) / 255))) / 255);
  switch (region) {
    case 0: return rgb(v, t, p);
    case 1: return rgb(q, v, p);
    case 2: return rgb(p, v, t);
    case 3: return rgb(p, q, v);
    case 4: return rgb(t, p, v);
    default: return rgb(v, p, q);
  }
}

/// The hue of a picked colour, so an effect that generates its own colours can
/// still be steered by the colour picker -- a blue fire, a green comet.
static uint16_t hue_of(uint32_t c) {
  const int r = red_of(c), g = grn_of(c), b = blu_of(c);
  const int hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
  const int lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
  const int span = hi - lo;
  if (span == 0) return 0;  // grey has no hue; red is as good an answer as any
  float h;
  if (hi == r) h = (float)(g - b) / span;
  else if (hi == g) h = 2.0f + (float)(b - r) / span;
  else h = 4.0f + (float)(r - g) / span;
  h /= 6.0f;
  if (h < 0.0f) h += 1.0f;
  return (uint16_t)(h * 65535.0f);
}

/*
 * Gamma.
 *
 * A WS2812 is linear in duty cycle and the eye is not, so a fade written as a
 * straight ramp spends most of its travel looking bright and then falls off a
 * cliff at the end. Correcting on the way out is what makes the breathing
 * effect breathe rather than blink. 2.6 is the usual compromise for these
 * parts -- 2.2 leaves the bottom end too bright, 3.0 crushes it.
 */
static uint8_t gamma_lut[256];

static void build_gamma() {
  for (int i = 0; i < 256; i++) {
    gamma_lut[i] = (uint8_t)(powf((float)i / 255.0f, 2.6f) * 255.0f + 0.5f);
  }
}

// ------------------------------------------------------------------- ring ----
/*
 * Ring geometry. On the 7-bit ring one pixel is in the middle and the rest are
 * around the rim, which is worth knowing about: a comet should chase round the
 * rim rather than jumping through the centre, and a VU meter reads much better
 * with a core that glows and a rim that fills.
 *
 * With LED_CENTRE_INDEX at -1 the centre simply does not exist, rim_count()
 * becomes the whole strip and every effect degrades into its linear form.
 */
static const bool HAS_CENTRE = (LED_CENTRE_INDEX >= 0) && (LED_CENTRE_INDEX < LED_COUNT);

static inline uint16_t rim_count() { return HAS_CENTRE ? LED_COUNT - 1 : LED_COUNT; }

/// The k-th rim pixel, skipping the centre.
static inline uint16_t rim_pixel(uint16_t k) {
  if (!HAS_CENTRE) return k;
  return k < (uint16_t)LED_CENTRE_INDEX ? k : k + 1;
}

// ------------------------------------------------------------------ frame ----
static uint32_t buf[LED_COUNT];

static inline void put(uint16_t i, uint32_t c) {
  if (i < LED_COUNT) buf[i] = c;
}
static void fill(uint32_t c) {
  for (uint16_t i = 0; i < LED_COUNT; i++) buf[i] = c;
}
static inline void put_rim(uint16_t k, uint32_t c) { put(rim_pixel(k), c); }
static inline void put_centre(uint32_t c) {
  if (HAS_CENTRE) put((uint16_t)LED_CENTRE_INDEX, c);
}

// ------------------------------------------------------------------ audio ----
/*
 * The analysis is tuned for a 32-bar graph, where twitchiness reads as detail.
 * A light with the same twitchiness reads as a fault, so everything is smoothed
 * again here, with asymmetric rise and fall times -- fast up, slow down, which
 * is how a VU meter and an eye both behave.
 */
static float a_loud;   ///< 0..1, overall level
static float a_bass;   ///< 0..1, bottom four bands
static float a_kick;   ///< 0..1, beat envelope: snaps to 1, decays
static bool a_prev_beat;
static uint16_t a_beat_hue;  ///< advanced on every beat, so beats differ
static uint32_t a_heard_ms;

/// One-pole approach with separate attack and release, frame-rate independent.
static float approach(float cur, float target, float dt, float rise_s, float fall_s) {
  const float tau = target > cur ? rise_s : fall_s;
  float k = dt / tau;
  if (k > 1.0f) k = 1.0f;
  return cur + (target - cur) * k;
}

static float band_avg(const AudioVis &v, uint8_t lo, uint8_t hi) {
  uint32_t sum = 0;
  for (uint8_t b = lo; b <= hi && b < VIS_BANDS; b++) sum += v.bands[b];
  return (float)sum / (float)((hi - lo + 1) * 255);
}

static void update_audio(const AudioVis &v, float dt, uint32_t now) {
  const uint8_t vu = v.vu_l > v.vu_r ? v.vu_l : v.vu_r;
  a_loud = approach(a_loud, vu / 255.0f, dt, 0.05f, 0.28f);
  a_bass = approach(a_bass, band_avg(v, 0, 3), dt, 0.03f, 0.16f);

  // The beat flag is true for exactly one analysis frame, and the lighting task
  // may well run several frames per analysis -- so it is latched on the edge
  // rather than sampled, or a beat lands on some frames and not others.
  if (v.beat && !a_prev_beat) {
    a_kick = 1.0f;
    a_beat_hue += 9000;
  }
  a_prev_beat = v.beat;

  a_kick -= dt / 0.20f;
  if (a_kick < 0.0f) a_kick = 0.0f;

  if (v.active) a_heard_ms = now;
}

static bool hearing(uint32_t now) {
  return a_heard_ms != 0 && (now - a_heard_ms) < LED_AUDIO_IDLE_MS;
}

// ----------------------------------------------------------------- effects ---
/*
 * Phase accumulators rather than a frame counter, so the effects run at the
 * same visual speed whatever the frame rate -- and keep running at the same
 * speed when a frame is late because the Bluetooth stack wanted the core.
 */
static float phase;   ///< 0..1, the main rotation
static float phase2;  ///< 0..1, a second, slower one for the effects with two

/// Cycles per second for a given speed setting. Squared, so the bottom half of
/// the slider is where the slow, calm settings live and the useful range is not
/// crammed into the first ten pixels of travel.
static float rate_of(uint8_t speed) {
  const float t = speed / 255.0f;
  return 0.06f + t * t * 2.4f;
}

static float wrap01(float x) { return x - floorf(x); }

/// Smooth 0..1 triangle from a phase, via a cosine.
static float swell(float p) { return 0.5f - 0.5f * cosf(6.2831853f * p); }

// --- twinkle and fire keep a little state of their own ----------------------
static float tw_level[LED_COUNT];
static bool tw_second[LED_COUNT];  ///< this spark is the secondary colour
static uint8_t fire_heat[LED_COUNT];

/// Energy of the frequency slice belonging to rim pixel k, 0..1. The bands are
/// already log-spaced and tilted by the probe, so a plain slice is enough --
/// low pitches land on the first pixel, high ones on the last.
static float slice_energy(const AudioVis &v, uint16_t k, uint16_t n) {
  const uint8_t lo = (uint8_t)((uint32_t)k * VIS_BANDS / n);
  uint8_t hi = (uint8_t)((uint32_t)(k + 1) * VIS_BANDS / n);
  if (hi <= lo) hi = lo + 1;
  uint8_t best = 0;
  for (uint8_t b = lo; b < hi && b < VIS_BANDS; b++) {
    if (v.bands[b] > best) best = v.bands[b];
  }
  return best / 255.0f;
}

/// What the reactive effects show when there is nothing to react to: a slow,
/// dim breath of the primary colour. Better than going dark, which reads as a
/// broken ring, and better than freezing on the last frame.
static void render_idle_reactive(const LedConfig &c) {
  const float v = 0.05f + 0.10f * swell(phase2);
  fill(dim(c.color, v));
}

static void render(const LedConfig &c, const AudioVis &vis, float dt, uint32_t now) {
  const bool live = hearing(now);
  const uint16_t n = rim_count();
  const uint16_t base_hue = hue_of(c.color);

  switch (c.effect) {
    case LED_FX_OFF:
      fill(0);
      break;

    case LED_FX_SOLID:
      fill(c.color);
      break;

    case LED_FX_BREATHE:
      // Never all the way to black: a breath that extinguishes looks like a
      // fault, and the gamma curve already makes the bottom end feel deep.
      fill(dim(c.color, 0.06f + 0.94f * swell(phase)));
      break;

    case LED_FX_RAINBOW:
      for (uint16_t i = 0; i < LED_COUNT; i++) {
        put(i, hsv((uint16_t)(wrap01(phase + (float)i / LED_COUNT) * 65535.0f), 255, 255));
      }
      break;

    case LED_FX_COLOR_CYCLE:
      fill(hsv((uint16_t)(phase * 65535.0f), 255, 255));
      break;

    case LED_FX_STROBE: {
      // With reactivity up and music playing this fires on the beat instead of
      // on a timer, which is the difference between a strobe and a strobe that
      // is in time with the track.
      bool on;
      if (live && c.reactivity > 5) {
        on = a_kick > 0.55f;
      } else {
        on = wrap01(phase * 2.0f) < 0.18f;
      }
      fill(on ? c.color : 0);
      break;
    }

    case LED_FX_COMET: {
      const float head = phase * n;
      for (uint16_t k = 0; k < n; k++) {
        float d = (float)k - head;
        while (d < 0.0f) d += n;              // the tail wraps round the rim
        const float v = powf(0.42f, d);       // one pixel back is 42% as bright
        put_rim(k, dim(c.color, v));
      }
      put_centre(dim(c.color2, 0.12f));
      break;
    }

    case LED_FX_CHASE: {
      const uint8_t step = (uint8_t)((uint32_t)(phase * 3.0f) % 3);
      for (uint16_t k = 0; k < n; k++) {
        put_rim(k, (k % 3 == step) ? c.color : dim(c.color2, 0.10f));
      }
      put_centre(dim(c.color2, 0.10f));
      break;
    }

    case LED_FX_TWINKLE: {
      const float r = rate_of(c.speed);
      for (uint16_t i = 0; i < LED_COUNT; i++) {
        tw_level[i] -= dt * (0.4f + r * 0.5f);
        if (tw_level[i] <= 0.0f) {
          tw_level[i] = 0.0f;
          // Chance per second scales with speed, so "faster" means "busier".
          if (random(1000) < (long)(dt * r * 900.0f)) {
            tw_level[i] = 1.0f;
            tw_second[i] = random(2) == 0;
          }
        }
        put(i, dim(tw_second[i] ? c.color2 : c.color, tw_level[i] * tw_level[i]));
      }
      break;
    }

    case LED_FX_FIRE: {
      /*
       * Cool, diffuse, spark -- the classic three steps, wrapped round the ring
       * instead of running up a strip. Bass feeds the spark rate when the music
       * sync is on, so the fire flares on the kick drum.
       */
      const float r = rate_of(c.speed);
      const uint8_t cool = (uint8_t)(dt * (55.0f + r * 45.0f) * 255.0f / 60.0f);
      for (uint16_t i = 0; i < LED_COUNT; i++) {
        const uint8_t sub = (uint8_t)random(cool + 1);
        fire_heat[i] = fire_heat[i] > sub ? (uint8_t)(fire_heat[i] - sub) : 0;
      }
      uint8_t blended[LED_COUNT];
      for (uint16_t i = 0; i < LED_COUNT; i++) {
        const uint16_t a = (i + LED_COUNT - 1) % LED_COUNT, b = (i + 1) % LED_COUNT;
        blended[i] = (uint8_t)(((uint16_t)fire_heat[a] + fire_heat[i] * 2 + fire_heat[b]) / 4);
      }
      memcpy(fire_heat, blended, sizeof(fire_heat));

      float spark = 0.30f + 0.45f * (r / 2.5f);
      if (live) spark += (c.reactivity / 100.0f) * a_bass * 1.6f;
      if (random(1000) < (long)(dt * spark * 1000.0f * LED_COUNT)) {
        const uint16_t i = (uint16_t)random(LED_COUNT);
        const uint16_t add = 160 + (uint16_t)random(96);
        fire_heat[i] = (uint8_t)(fire_heat[i] + add > 255 ? 255 : fire_heat[i] + add);
      }

      for (uint16_t i = 0; i < LED_COUNT; i++) {
        const float t = fire_heat[i] / 255.0f;
        // Black to the picked hue over the bottom half, then the hue washing
        // out towards white over the top -- which is what makes it read as heat
        // rather than as a colour fade.
        put(i, t < 0.5f ? hsv(base_hue, 255, (uint8_t)(t * 2.0f * 255.0f))
                        : hsv(base_hue, (uint8_t)(255 - (t - 0.5f) * 2.0f * 200.0f), 255));
      }
      break;
    }

    case LED_FX_GRADIENT:
      for (uint16_t i = 0; i < LED_COUNT; i++) {
        put(i, mix(c.color, c.color2, swell(wrap01(phase + (float)i / LED_COUNT))));
      }
      break;

    case LED_FX_VU: {
      if (!live) { render_idle_reactive(c); break; }
      /*
       * The rim fills with the level and the centre glows with it. The last
       * pixel is fractional rather than snapping on, which at seven pixels is
       * the difference between a meter and a bar graph.
       */
      const float filled = a_loud * n;
      for (uint16_t k = 0; k < n; k++) {
        const uint32_t tint = mix(c.color, c.color2, n > 1 ? (float)k / (n - 1) : 0.0f);
        const float f = filled - k;
        put_rim(k, dim(tint, f >= 1.0f ? 1.0f : (f > 0.0f ? f : 0.0f)));
      }
      put_centre(dim(c.color, 0.15f + 0.85f * a_bass));
      break;
    }

    case LED_FX_SPECTRUM: {
      if (!live) { render_idle_reactive(c); break; }
      // Pitch to hue, bass red through treble violet -- the mapping every
      // spectrum display has used since they were made of neon.
      for (uint16_t k = 0; k < n; k++) {
        const float e = slice_energy(vis, k, n);
        const uint16_t h = (uint16_t)((float)k / n * 48000.0f);
        put_rim(k, hsv(h, 255, (uint8_t)(e * e * 255.0f)));
      }
      put_centre(dim(c.color, a_loud));
      break;
    }

    case LED_FX_BEAT: {
      if (!live) { render_idle_reactive(c); break; }
      // Squared, so the flash has a hard front edge and a soft tail rather than
      // a symmetrical fade, which is what makes it read as a hit.
      const float v = a_kick * a_kick;
      fill(mix(dim(c.color, 0.05f), hsv((uint16_t)(base_hue + a_beat_hue), 255, 255), v));
      break;
    }

    case LED_FX_MUSIC:
    default: {
      if (!live) { render_idle_reactive(c); break; }
      /*
       * The full sync, and the mode the whole feature is really for.
       *
       *   the rim      one frequency slice per pixel, so the ring is a circular
       *                spectrum analyser
       *   the hue      the picked colour, spread by a third of the wheel across
       *                the rim and rotated a step on every beat -- so the
       *                palette is yours but it still moves with the track
       *   the centre   bass, straight: it thumps
       *   every beat   a wash towards white across the whole ring, on top
       *
       * Reactivity mixes between a steady lit ring and all of the above, so the
       * slider goes from "a lamp that pulses" to "the ring is the music".
       */
      const float react = c.reactivity / 100.0f;
      const uint16_t spread = 22000;  // a third of the wheel across the rim

      for (uint16_t k = 0; k < n; k++) {
        const float e = slice_energy(vis, k, n);
        const uint16_t h = (uint16_t)(base_hue + a_beat_hue +
                                      (uint16_t)((float)k / n * spread));
        // The floor keeps the ring alive between transients; react decides how
        // far below full brightness the quiet parts are allowed to fall.
        const float v = (1.0f - react) * 0.55f + react * e;
        const uint32_t lit = hsv(h, 255, (uint8_t)(v * 255.0f));
        put_rim(k, mix(lit, rgb(255, 255, 255), a_kick * react * 0.55f));
      }

      const float centre = (1.0f - react) * 0.35f + react * a_bass;
      put_centre(mix(hsv((uint16_t)(base_hue + a_beat_hue), 255,
                         (uint8_t)(centre * 255.0f)),
                     rgb(255, 255, 255), a_kick * react * 0.7f));
      break;
    }
  }
}

// ------------------------------------------------------------------ commit ---
/*
 * The one place brightness, the reactive gain, the hard ceiling and gamma are
 * applied -- all after the effect, so an effect never has to think about any of
 * them and two effects can never apply them twice.
 */
static void commit(const LedConfig &c, uint32_t now) {
  float user = c.brightness / 255.0f;

  // The global music reaction, on top of every effect including the ones that
  // know nothing about audio. The reactive effects opt out: they have already
  // spent the audio on something more interesting than overall brightness, and
  // dimming them again would just make them quieter as well as darker.
  const bool own_audio = c.effect == LED_FX_VU || c.effect == LED_FX_SPECTRUM ||
                         c.effect == LED_FX_BEAT || c.effect == LED_FX_MUSIC;
  if (!own_audio && c.reactivity > 0 && hearing(now)) {
    const float react = c.reactivity / 100.0f;
    const float env = 0.35f + 0.65f * a_loud;
    float gain = 1.0f - react * (1.0f - env) + react * 0.40f * a_kick;
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;
    user *= gain;
  }

  const float ceiling = (float)LED_BRIGHTNESS_MAX / 255.0f;
  const float scale = user * ceiling;

  rmt_symbol_word_t *out = symbols;
  for (uint16_t i = 0; i < LED_COUNT; i++) {
    const uint32_t c8 = dim(buf[i], scale);
    const uint8_t r = gamma_lut[red_of(c8)];
    const uint8_t g = gamma_lut[grn_of(c8)];
    const uint8_t b = gamma_lut[blu_of(c8)];
#if LED_STRIP_GRB
    const uint8_t channel[3] = {g, r, b};
#else
    const uint8_t channel[3] = {r, g, b};
#endif
    for (uint8_t c = 0; c < 3; c++) {
      for (int8_t bit = 7; bit >= 0; bit--) {  // most significant bit first
        const bool one = channel[c] & (1u << bit);
        out->level0 = 1;
        out->duration0 = one ? T1H : T0H;
        out->level1 = 0;
        out->duration1 = one ? T1L : T0L;
        out++;
      }
    }
  }
  // rmt_transmit() queues and returns; the wait is the 210 us the frame takes
  // to clock out, on a task that has nothing else to do. The timeout is a
  // backstop against a transfer that never completes -- dropping a frame of
  // light is not worth wedging a task over.
  rmt_transmit_config_t tx = {};
  tx.loop_count = 0;
  if (rmt_transmit(rmt_channel, rmt_encoder, symbols,
                   SYMBOL_COUNT * sizeof(symbols[0]), &tx) == ESP_OK) {
    rmt_tx_wait_all_done(rmt_channel, 50);
  }
}

// -------------------------------------------------------------------- task ---
static void leds_task(void *) {
  const TickType_t period = pdMS_TO_TICKS(1000 / LED_FPS);
  TickType_t next = xTaskGetTickCount() + period;
  uint32_t last_ms = millis();

  for (;;) {
    const uint32_t now = millis();
    float dt = (now - last_ms) * 0.001f;
    last_ms = now;
    if (dt <= 0.0f) dt = 0.001f;
    if (dt > 0.25f) dt = 0.25f;  // after a stall, do not jump the animation

    LedConfig live;
    portENTER_CRITICAL(&cfg_mux);
    live = cfg;
    portEXIT_CRITICAL(&cfg_mux);

    AudioVis vis;
    audio_probe_frame(&vis, 1000 / LED_FPS);
    update_audio(vis, dt, now);

    const float r = rate_of(live.speed);
    phase = wrap01(phase + dt * r);
    phase2 = wrap01(phase2 + dt * r * 0.23f);

    if (!live.enabled) {
      fill(0);
    } else {
      render(live, vis, dt, now);
    }
    commit(live, now);

    // Same guard as the UI task: an overrun resets the schedule instead of
    // spinning at priority 1 on core 0 and starving the idle task.
    const TickType_t t = xTaskGetTickCount();
    if ((int32_t)(next - t) <= 0) {
      next = t + period;
      vTaskDelay(1);
    } else {
      vTaskDelay(next - t);
      next += period;
    }
  }
}

// ------------------------------------------------------------------ public ---
bool leds_begin() {
  if (PIN_LEDS < 0) {
    Serial.println("[leds] disabled (PIN_LEDS is -1)");
    return false;
  }
  build_gamma();

  rmt_tx_channel_config_t channel = {};
  channel.gpio_num = (gpio_num_t)PIN_LEDS;
  channel.clk_src = RMT_CLK_SRC_DEFAULT;
  channel.resolution_hz = RMT_TICK_HZ;
  channel.mem_block_symbols = RMT_MEM_SYMBOLS;
  channel.trans_queue_depth = 1;  // one frame in flight; the task waits for it
  esp_err_t err = rmt_new_tx_channel(&channel, &rmt_channel);
  if (err != ESP_OK) {
    Serial.printf("[leds] no RMT channel for GPIO%d (%s) -- lighting is off\n",
                  (int)PIN_LEDS, esp_err_to_name(err));
    return false;
  }

  // The copy encoder hands our symbols to the hardware unchanged, which is what
  // we want: the bit timing is already baked into them in commit().
  rmt_copy_encoder_config_t encoder = {};
  err = rmt_new_copy_encoder(&encoder, &rmt_encoder);
  if (err == ESP_OK) err = rmt_enable(rmt_channel);
  if (err != ESP_OK) {
    Serial.printf("[leds] RMT would not start (%s) -- lighting is off\n",
                  esp_err_to_name(err));
    rmt_del_channel(rmt_channel);
    rmt_channel = nullptr;
    return false;
  }
  g_present = true;

  // Blank, so the ring is dark from here rather than showing whatever the
  // pixels came up holding.
  LedConfig off = {};
  fill(0);
  commit(off, millis());

  Serial.printf("[leds] %u WS2812 on GPIO%d, %s, cap %u/255, %u fps\n",
                (unsigned)LED_COUNT, (int)PIN_LEDS,
                HAS_CENTRE ? "ring with centre" : "strip",
                (unsigned)LED_BRIGHTNESS_MAX, (unsigned)LED_FPS);
  return true;
}

void leds_start() {
  if (!g_present) return;
  // Core 0, priority 1: the same berth as the display task, and below both the
  // Bluetooth controller and the audio path, so a late frame of light is the
  // only thing a busy moment can cost. The stack carries a whole AudioVis copy
  // (a little over 200 bytes) on top of the effect locals.
  xTaskCreatePinnedToCore(leds_task, "leds", 4096, nullptr, 1, nullptr, 0);
}

bool leds_present() { return g_present; }

void leds_configure(const LedConfig &in) {
  LedConfig v = in;
  if (v.effect >= LED_FX_COUNT) v.effect = LED_FX_MUSIC;
  if (v.reactivity > 100) v.reactivity = 100;
  v.color &= 0xFFFFFFu;
  v.color2 &= 0xFFFFFFu;
  portENTER_CRITICAL(&cfg_mux);
  cfg = v;
  portEXIT_CRITICAL(&cfg_mux);
}

void leds_get(LedConfig *out) {
  portENTER_CRITICAL(&cfg_mux);
  *out = cfg;
  portEXIT_CRITICAL(&cfg_mux);
}

static const char *const FX_NAMES[LED_FX_COUNT] = {
    "Off",     "Solid",   "Breathing", "Rainbow",  "Colour cycle",
    "Strobe",  "Comet",   "Chase",     "Twinkle",  "Fire",
    "Gradient", "VU meter", "Spectrum", "Beat flash", "Music sync"};

static const char *const FX_HINTS[LED_FX_COUNT] = {
    "Dark. The ring stays configured and comes back where you left it.",
    "The picked colour, steady.",
    "The picked colour, rising and falling.",
    "The full spectrum wrapped around the ring, rotating.",
    "The whole ring on one hue, drifting through the wheel.",
    "Hard flashes. With reactivity up it fires on the beat instead of a timer.",
    "A bright head chasing round the rim, trailing a tail.",
    "Theatre chase: every third pixel marching round.",
    "Random sparks rising and fading, in both picked colours.",
    "Flicker in the hue you picked. Bass feeds the flare.",
    "A smooth sweep between your two colours, rotating.",
    "The rim fills with the level, the centre thumps with the bass.",
    "One frequency band per pixel. Bass red, treble violet.",
    "Dark between beats, a burst of colour on each one.",
    "The rim is a circular spectrum, the centre is the bass, the hue steps on "
    "every beat."};

const char *leds_effect_name(uint8_t effect) {
  return effect < LED_FX_COUNT ? FX_NAMES[effect] : "?";
}

const char *leds_effect_hint(uint8_t effect) {
  return effect < LED_FX_COUNT ? FX_HINTS[effect] : "";
}

bool leds_hearing_audio() { return hearing(millis()); }

// ----------------------------------------------------------------- console ---
static void print_leds_status() {
  LedConfig c;
  leds_get(&c);
  Serial.printf("[leds] %s | %s | bright %u | speed %u | react %u%% | "
                "#%06lX / #%06lX | %s\n",
                c.enabled ? "on" : "off", leds_effect_name(c.effect),
                (unsigned)c.brightness, (unsigned)c.speed,
                (unsigned)c.reactivity, (unsigned long)c.color,
                (unsigned long)c.color2,
                leds_hearing_audio() ? "hearing audio" : "no audio");
}

bool leds_command(const char *line) {
  if (strncmp(line, "leds", 4) != 0) return false;
  const char *arg = line + 4;
  while (*arg == ' ') arg++;

  LedConfig c;
  leds_get(&c);
  unsigned value = 0;
  unsigned long colour = 0;

  if (*arg == 0) {
    print_leds_status();
    Serial.println(F("  leds on|off              switch the ring on or off\n"
                     "  leds fx <0-14>           choose an effect\n"
                     "  leds list                list the effects\n"
                     "  leds color RRGGBB        primary colour, hex\n"
                     "  leds color2 RRGGBB       secondary colour, hex\n"
                     "  leds bright 0..255       brightness\n"
                     "  leds speed 0..255        effect speed\n"
                     "  leds react 0..100        how much the music shows"));
    return true;
  }
  if (strcmp(arg, "on") == 0 || strcmp(arg, "off") == 0) {
    c.enabled = arg[1] == 'n';
  } else if (strcmp(arg, "list") == 0) {
    for (uint8_t i = 0; i < LED_FX_COUNT; i++) {
      Serial.printf("  %2u  %-13s %s\n", i, FX_NAMES[i], FX_HINTS[i]);
    }
    return true;
  } else if (sscanf(arg, "fx %u", &value) == 1) {
    if (value >= LED_FX_COUNT) {
      Serial.printf("[leds] effect must be 0..%u\n", LED_FX_COUNT - 1);
      return true;
    }
    c.effect = (uint8_t)value;
  } else if (sscanf(arg, "color2 %lx", &colour) == 1) {
    c.color2 = (uint32_t)colour & 0xFFFFFFu;
  } else if (sscanf(arg, "color %lx", &colour) == 1) {
    c.color = (uint32_t)colour & 0xFFFFFFu;
  } else if (sscanf(arg, "bright %u", &value) == 1) {
    c.brightness = (uint8_t)(value > 255 ? 255 : value);
  } else if (sscanf(arg, "speed %u", &value) == 1) {
    c.speed = (uint8_t)(value > 255 ? 255 : value);
  } else if (sscanf(arg, "react %u", &value) == 1) {
    c.reactivity = (uint8_t)(value > 100 ? 100 : value);
  } else {
    Serial.printf("[leds] unknown: %s (try 'leds')\n", arg);
    return true;
  }

  leds_configure(c);
  print_leds_status();
  return true;
}

#endif  // LEDS_ENABLED
