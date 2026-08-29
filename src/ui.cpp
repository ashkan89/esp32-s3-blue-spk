#include "ui.h"

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <WiFi.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "management.h"
#include "audio_probe.h"
#include "battery.h"
#include "df_player.h"
#include "player_state.h"
#include "power.h"
#include "soft_clock.h"
#include "status_led.h"
#include "ui_assets.h"
#include "ui_config.h"

// ------------------------------------------------------------------ panel ----
static const int W = 128;
static const int H = 32;
static const int PAGES = H / 8;
static const size_t FB_BYTES = (size_t)W * PAGES;

/*
 * Full-buffer ("F") constructor: the whole 512-byte frame is composed in RAM and
 * pushed in one go. The page-buffer variants would use less RAM but cannot do
 * transitions or read back what was drawn, and 512 bytes is not worth arguing
 * about next to the Bluetooth stack.
 *
 * Argument order is (rotation, reset, clock, data) -- clock before data, which
 * is the opposite of how everyone says "SDA/SCL", and a classic way to spend an
 * evening on a display that stays blank.
 */
#if UI_FLIP_180
static U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C u8g2(U8G2_R2, U8X8_PIN_NONE,
                                                   PIN_OLED_SCL, PIN_OLED_SDA);
#else
static U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE,
                                                   PIN_OLED_SCL, PIN_OLED_SDA);
#endif

static bool g_present;

/*
 * The battery glyph.
 *
 * Drawn rather than stored as a bitmap because the interesting part is the fill
 * level, and 14x7 pixels of outline plus a variable-width box is less code than
 * eleven bitmaps would be. Nine pixels of interior means the fill has ten
 * distinguishable states, which is as much as anybody reads off an icon.
 *
 * Charging is a diagonal stroke through the middle rather than an animation: the
 * screens redraw at 30 fps and a pulsing icon on a 128x32 panel next to a
 * spectrum analyser is one moving thing too many.
 */
static void draw_battery(int x, int y, const BatteryStatus &b) {
  const int bw = 12, bh = 7;
  u8g2.drawFrame(x, y, bw, bh);
  u8g2.drawBox(x + bw, y + 2, 2, 3);  // the terminal nub

  if (!b.present) {
    // Two diagonal strokes: "no cell", which is different from "empty cell" and
    // has to look different or a broken sense wire reads as a flat battery.
    u8g2.drawLine(x + 2, y + 1, x + bw - 3, y + bh - 2);
    u8g2.drawLine(x + 2, y + bh - 2, x + bw - 3, y + 1);
    return;
  }

  const int inner = bw - 4;  // 1 px frame plus 1 px air on each side
  int fill = (b.percent * inner + 50) / 100;
  // A cell that is present but nearly empty still shows one pixel: a completely
  // hollow icon is how the "no cell" case above reads.
  if (fill < 1 && b.percent > 0) fill = 1;
  if (fill > 0) u8g2.drawBox(x + 2, y + 2, fill, bh - 4);

  if (b.charging) {
    // A bolt through the icon, in the inverse of whatever is under it, so it is
    // legible over both the filled and the empty part.
    u8g2.setDrawColor(2);
    u8g2.drawLine(x + 7, y + 1, x + 4, y + 3);
    u8g2.drawLine(x + 4, y + 3, x + 7, y + 3);
    u8g2.drawLine(x + 7, y + 3, x + 4, y + bh - 2);
    u8g2.setDrawColor(1);
  }
}

// Fonts. "_tf" carries the full Latin-1 range, which is what drawUTF8 needs for
// accented track titles; "_tn" is digits only, which is all the clock wants.
#define FONT_TITLE u8g2_font_helvB08_tf
#define FONT_TEXT u8g2_font_5x7_tf
#define FONT_SMALL u8g2_font_4x6_tf
#define FONT_BIGNUM u8g2_font_logisoso16_tn

static inline uint8_t *fb() { return u8g2.getBufferPtr(); }

// ----------------------------------------------------------------- screens ---
enum UiScreen : uint8_t {
  SCR_NOW_PLAYING = 0,
  SCR_SPECTRUM,
  SCR_VU,
  SCR_SCOPE,
  SCR_WATERFALL,
  SCR_CLOCK,
  SCR_INFO,
  SCR_ROTATE_COUNT,  // everything above this is part of the carousel
  SCR_PAIRING,       // forced while no phone is connected
  SCR_SAVER,         // forced after UI_SLEEP_AFTER_MS of nothing
};

static UiScreen cur_screen = SCR_CLOCK;
static uint32_t screen_since;
static bool carousel_paused;
static uint8_t spectrum_style;  // 0 bars, 1 mirrored, 2 matrix

// Requests from the serial console, which arrives on the Arduino task. They are
// applied by the UI task at the top of a frame rather than acted on directly:
// two tasks drawing into one frame buffer would tear, and the transition needs
// the previous frame intact.
static volatile int8_t req_screen = -1;
static volatile bool req_next;

// --------------------------------------------------------------- overlays ----
enum ToastKind : uint8_t {
  TOAST_NONE = 0,
  TOAST_CONNECTED,
  TOAST_DISCONNECTED,
  TOAST_TRACK,
};

static ToastKind toast_kind;
static uint32_t toast_until;
static uint32_t popup_until;  // volume popup

struct SystemOverlay {
  UiSystemStatus kind;
  char title[36];
  char detail[80];
  int16_t progress;
  uint32_t until;
  bool active;
};

static SystemOverlay system_overlay;
static portMUX_TYPE system_overlay_mux = portMUX_INITIALIZER_UNLOCKED;

// ------------------------------------------------------------------ timers ---
static uint32_t last_frame_ms;
static uint32_t last_activity_ms;

/*
 * Two idle clocks, not one.
 *
 * last_activity_ms is the old one and audio keeps it warm, which is what the
 * dim and the screensaver want: a speaker that is playing is not idle. It is
 * therefore useless for a blanking mode whose whole point is to go dark during
 * playback, so the moments the *owner* did something get their own timestamp.
 */
static uint32_t last_input_ms;

static volatile bool power_save;
static volatile bool suspended;
static volatile uint8_t blank_mode = UI_BLANK_MODE_DEFAULT;
static volatile uint32_t blank_after_ms = UI_BLANK_AFTER_S_DEFAULT * 1000UL;
static bool panel_off;

/// One touch resets both clocks. Every caller wants both -- the owner pressing
/// a button is activity by any definition.
static void note_input(uint32_t now) {
  last_activity_ms = now;
  last_input_ms = now;
}
static uint32_t frame_counter;
static float fps_avg = UI_FPS;

// Edge detection against the previous snapshot.
static bool have_prev_state;
static bool prev_connected;
static uint32_t prev_track_seq;
static uint32_t prev_volume_seq;

// --------------------------------------------------------------- brightness --
static uint8_t bright_level = 1;  // 0 low, 1 mid, 2 high
static uint8_t bright_override;   // 0 = none, else the exact contrast value
static uint8_t bright_applied = 0xFF;

// ------------------------------------------------------------- transitions ---
enum TransKind : uint8_t { TRANS_SLIDE = 0, TRANS_WIPE, TRANS_DISSOLVE, TRANS_COUNT };

static uint8_t prev_fb[FB_BYTES];
static bool trans_active;
static uint32_t trans_start;
static uint8_t trans_kind;

// ------------------------------------------------------------------ button ---
static bool btn_down;
static uint32_t btn_since;
static bool btn_consumed;
/// Factory reset countdown. btn_reset_shown is the digit currently on screen,
/// 0xFF when the countdown is not running; btn_reset_request is handed to
/// loop() exactly once via ui_take_factory_reset_request().
static uint8_t btn_reset_shown = 0xFF;
static bool btn_reset_fired;
static bool btn_mode_offered;
static volatile bool btn_reset_request;
/// The radio-mode offer: non-zero while "press again to confirm" is on screen.
static uint32_t btn_mode_until;
static volatile bool btn_mode_request;

// ---------------------------------------------------------------- waterfall --
static uint8_t wf_fb[FB_BYTES];

// =========================================================== small helpers ===

static const uint8_t BAYER4[16] = {0,  8,  2,  10, 12, 4,  14, 6,
                                   3,  11, 1,  9,  15, 7,  13, 5};

/// Ordered dithering: turns an 8-bit intensity into an on/off pixel that reads
/// as a shade once the neighbours are drawn too. The only way to get grey out of
/// a one-bit panel.
static inline bool dither(int x, int y, uint8_t level) {
  return (level >> 4) > BAYER4[(y & 3) * 4 + (x & 3)];
}

static inline void wf_set(int x, int y) {
  wf_fb[(y >> 3) * W + x] |= (uint8_t)(1u << (y & 7));
}

/// Scales 0..255 to 0..max, rounding, so a band that is barely on still shows a
/// pixel rather than disappearing.
static inline int scale255(uint8_t v, int max) {
  return (int)(((uint32_t)v * max + 127) / 255);
}

static void fmt_clock_ms(uint32_t ms, char *out, size_t n) {
  uint32_t s = ms / 1000;
  const uint32_t h = s / 3600;
  s %= 3600;
  if (h > 0) {
    snprintf(out, n, "%u:%02u:%02u", (unsigned)h, (unsigned)(s / 60),
             (unsigned)(s % 60));
  } else {
    snprintf(out, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
  }
}

static void fmt_uptime(uint32_t ms, char *out, size_t n) {
  const uint32_t s = ms / 1000;
  if (s < 60) {
    snprintf(out, n, "%us", (unsigned)s);
  } else if (s < 3600) {
    snprintf(out, n, "%um", (unsigned)(s / 60));
  } else {
    snprintf(out, n, "%uh%02um", (unsigned)(s / 3600),
             (unsigned)((s % 3600) / 60));
  }
}

// ------------------------------------------------------------------ marquee ---
/*
 * Scrolling text. Long titles are the normal case on a 128 px panel, and cutting
 * them off with an ellipsis loses exactly the part you wanted.
 *
 * Each slot holds its own scroll position, and resets when the string changes
 * (detected by hash, so no copy of the previous text is kept). Text that fits
 * does not scroll at all -- constant motion for no reason is worse than static.
 */
enum MqSlot : uint8_t {
  MQ_TITLE = 0,
  MQ_ARTIST,
  MQ_THIRD,
  MQ_NAME,
  MQ_PEER,
  MQ_STATS,
  MQ_TOAST_A,
  MQ_TOAST_B,
  MQ_SYSTEM_A,
  MQ_SYSTEM_B,
  MQ_COUNT,
};

struct Marquee {
  float off;
  int16_t hold;
  uint32_t hash;
};
static Marquee mq[MQ_COUNT];

static const int16_t MQ_HOLD_MS = 1100;  ///< pause before and after each pass
static const int MQ_GAP = 20;            ///< blank between the two copies
static const float MQ_SPEED = 22.0f;     ///< px per second

static uint32_t str_hash(const char *s) {
  uint32_t h = 2166136261u;  // FNV-1a
  while (*s) {
    h ^= (uint8_t)*s++;
    h *= 16777619u;
  }
  return h;
}

static void draw_marquee(MqSlot slot, int x, int baseline, int w, const char *s,
                         uint32_t dt_ms, bool center_if_short = false) {
  if (s == nullptr || s[0] == 0) return;
  Marquee &m = mq[slot];
  const uint32_t h = str_hash(s);
  if (m.hash != h) {
    m.hash = h;
    m.off = 0.0f;
    m.hold = MQ_HOLD_MS;
  }

  const int tw = u8g2.getUTF8Width(s);
  u8g2.setClipWindow(x, 0, x + w, H);
  if (tw <= w) {
    u8g2.drawUTF8(center_if_short ? x + (w - tw) / 2 : x, baseline, s);
  } else {
    if (m.hold > 0) {
      m.hold -= (int16_t)dt_ms;
    } else {
      m.off += MQ_SPEED * dt_ms * 0.001f;
    }
    const int total = tw + MQ_GAP;
    if (m.off >= total) {
      m.off -= total;
      m.hold = MQ_HOLD_MS;
    }
    const int ox = x - (int)m.off;
    u8g2.drawUTF8(ox, baseline, s);
    u8g2.drawUTF8(ox + total, baseline, s);
  }
  u8g2.setMaxClipWindow();
}

// --------------------------------------------------------------------- bars ---
/*
 * Level bars are drawn as separate segments rather than one solid block. At this
 * size a solid bar is just a bright smear that is hard to read a value off;
 * segments give the eye something to count, and they look like the hardware this
 * is imitating.
 */
static void draw_seg_bar(int x, int y, int w, int h, float frac, int seg_w,
                         int gap) {
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  const int pitch = seg_w + gap;
  const int nseg = (w + gap) / pitch;
  const int lit = (int)(frac * nseg + 0.5f);
  for (int i = 0; i < nseg; i++) {
    if (i < lit) {
      u8g2.drawBox(x + i * pitch, y, seg_w, h);
    } else if ((i & 1) == 0) {
      // A dotted trail through the unlit part, so the full scale stays visible.
      u8g2.drawPixel(x + i * pitch + seg_w / 2, y + h / 2);
    }
  }
}

/// Seven-segment digit. Written out rather than using a font because a 24 px
/// numeric font costs 2 KB of flash, and because segments can be animated.
static const uint8_t SEG_DIGIT[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66,
                                      0x6D, 0x7D, 0x07, 0x7F, 0x6F};

static void draw_7seg(int x, int y, int w, int h, int t, uint8_t seg) {
  const int mid = y + (h - t) / 2;
  const int upper_h = mid - y - t;
  const int lower_h = (y + h - t) - (mid + t);
  if (seg & 0x01) u8g2.drawBox(x + t, y, w - 2 * t, t);              // a top
  if (seg & 0x40) u8g2.drawBox(x + t, mid, w - 2 * t, t);            // g middle
  if (seg & 0x08) u8g2.drawBox(x + t, y + h - t, w - 2 * t, t);      // d bottom
  if (seg & 0x20) u8g2.drawBox(x, y + t, t, upper_h);                // f
  if (seg & 0x02) u8g2.drawBox(x + w - t, y + t, t, upper_h);        // b
  if (seg & 0x10) u8g2.drawBox(x, mid + t, t, lower_h);              // e
  if (seg & 0x04) u8g2.drawBox(x + w - t, mid + t, t, lower_h);      // c
}

/// Erases a rectangle and outlines it -- the base of every overlay.
static void draw_panel(int x, int y, int w, int h) {
  u8g2.setDrawColor(0);
  u8g2.drawBox(x, y, w, h);
  u8g2.setDrawColor(1);
  u8g2.drawRFrame(x, y, w, h, 3);
}

/// Right-aligned text ending at x_right.
static void draw_right(int x_right, int baseline, const char *s) {
  u8g2.drawUTF8(x_right - u8g2.getUTF8Width(s), baseline, s);
}

// ================================================================= screens ===

/// The 6 px spectrum strip that shares the now-playing screen.
static void draw_mini_spectrum(const AudioVis &v, int y, int h) {
  for (uint8_t b = 0; b < VIS_BANDS; b++) {
    const int bh = scale255(v.bands[b], h);
    if (bh > 0) u8g2.drawBox(b * 4, y + h - bh, 3, bh);
  }
}

static void draw_now_playing(const PlayerInfo &s, const AudioVis &v,
                             uint32_t now, uint32_t dt) {
  // --- row 1: transport icon + title ---
  const uint8_t *icon = ICON_NOTE;
  if (s.playback == PS_PLAYING) icon = ICON_PLAY;
  else if (s.playback == PS_PAUSED) icon = ICON_PAUSE;
  else if (s.playback == PS_STOPPED && !s.streaming) icon = ICON_STOP;
  u8g2.drawXBMP(0, 0, 8, 8, icon);

  // A player that sends no AVRCP metadata leaves the title empty; the phone name
  // is the most useful thing to put there instead.
  u8g2.setFont(FONT_TITLE);
  const char *first = s.title[0] ? s.title : (s.peer[0] ? s.peer : "Connected");
  draw_marquee(MQ_TITLE, 10, 8, W - 10, first, dt);

  // --- row 2: artist, falling back to whatever else we know ---
  u8g2.setFont(FONT_TEXT);
  const char *second = s.artist[0] ? s.artist : s.album;
  if (second[0] == 0 && s.title[0]) second = s.peer;
  draw_marquee(MQ_ARTIST, 0, 17, W, second, dt);

  // --- row 3: live spectrum while the music plays, album name otherwise ---
  if (v.active) {
    draw_mini_spectrum(v, 19, 6);
  } else {
    u8g2.setFont(FONT_SMALL);
    char third[64];
    if (s.album[0] && s.artist[0]) {
      snprintf(third, sizeof(third), "%s", s.album);
    } else if (s.track_count > 0) {
      snprintf(third, sizeof(third), "track %u of %u", (unsigned)s.track_num,
               (unsigned)s.track_count);
    } else {
      snprintf(third, sizeof(third), "%s", s.genre);
    }
    draw_marquee(MQ_THIRD, 0, 24, W, third, dt);
  }

  // --- row 4: elapsed / progress / total ---
  u8g2.setFont(FONT_SMALL);
  const uint32_t pos = ps_position_ms(s, now);
  char elapsed[12], total[12];
  fmt_clock_ms(pos, elapsed, sizeof(elapsed));
  u8g2.drawUTF8(0, 31, elapsed);
  const int bar_x = u8g2.getUTF8Width(elapsed) + 4;

  if (s.track_ms > 0) {
    fmt_clock_ms(s.track_ms, total, sizeof(total));
    const int tw = u8g2.getUTF8Width(total);
    draw_right(W, 31, total);
    const int bar_w = (W - tw - 4) - bar_x;
    if (bar_w > 8) {
      const float frac = (float)pos / (float)s.track_ms;
      u8g2.drawHLine(bar_x, 29, bar_w);  // the track
      const int fill = (int)(frac * bar_w + 0.5f);
      if (fill > 0) u8g2.drawBox(bar_x, 28, fill, 2);
      // Playhead: three pixels tall, so the position is findable at a glance.
      const int px = bar_x + (fill > bar_w - 1 ? bar_w - 1 : fill);
      u8g2.drawVLine(px, 27, 4);
    }
  } else {
    /*
     * No duration from the phone. Rather than draw a progress bar that cannot
     * progress, run a barber pole: it says "playing, length unknown" without
     * implying a position.
     */
    const int bar_w = W - bar_x;
    const int phase = (int)(now / 60);
    for (int i = 0; i < bar_w; i++) {
      if (((i + phase) % 6) < 3) u8g2.drawVLine(bar_x + i, 28, 2);
    }
  }
}

static void draw_spectrum(const AudioVis &v, uint32_t now) {
  switch (spectrum_style) {
    case 1: {
      // Mirrored around the centre line: reads as a waveform envelope and uses
      // the full width without needing the full height per band.
      for (uint8_t b = 0; b < VIS_BANDS; b++) {
        const int x = b * 4;
        const int half = scale255(v.bands[b], 15);
        if (half > 0) {
          u8g2.drawBox(x, 16 - half, 3, half);
          u8g2.drawBox(x, 17, 3, half);
        } else {
          u8g2.drawPixel(x + 1, 16);
        }
        const int ph = scale255(v.peaks[b], 15);
        if (ph > half) {
          u8g2.drawHLine(x, 16 - ph, 3);
          u8g2.drawHLine(x, 17 + ph - 1, 3);
        }
      }
      break;
    }
    case 2: {
      // Matrix style: eight discrete blocks per column, like an LED analyser.
      for (uint8_t b = 0; b < VIS_BANDS; b++) {
        const int x = b * 4;
        const int lit = scale255(v.bands[b], 8);
        for (int r = 0; r < lit; r++) u8g2.drawBox(x, 29 - r * 4, 3, 3);
        const int pk = scale255(v.peaks[b], 8);
        if (pk > lit && pk > 0) u8g2.drawHLine(x, 29 - (pk - 1) * 4 + 1, 3);
      }
      break;
    }
    default: {
      // Classic bars from the bottom, with a floating peak cap.
      for (uint8_t b = 0; b < VIS_BANDS; b++) {
        const int x = b * 4;
        const int bh = scale255(v.bands[b], 30);
        if (bh > 0) u8g2.drawBox(x, H - bh, 3, bh);
        const int ph = scale255(v.peaks[b], 30);
        if (ph > bh + 1) u8g2.drawHLine(x, H - ph - 1, 3);
      }
      break;
    }
  }

  // Beat accent: four corner ticks for one frame. Enough to feel the rhythm in
  // peripheral vision, not enough to be a strobe.
  if (v.beat) {
    u8g2.drawHLine(0, 0, 4);
    u8g2.drawHLine(W - 4, 0, 4);
    u8g2.drawVLine(0, 0, 3);
    u8g2.drawVLine(W - 1, 0, 3);
  }
}

static void draw_vu(const PlayerInfo &s, const AudioVis &v, uint32_t dt) {
  const int BAR_X = 10;
  const int BAR_W = 115;
  const float VU_DB_SPAN = 48.0f;  // must match audio_probe.cpp

  u8g2.setFont(FONT_TEXT);
  u8g2.drawUTF8(1, 9, "L");
  u8g2.drawUTF8(1, 19, "R");

  draw_seg_bar(BAR_X, 2, BAR_W, 8, v.vu_l / 255.0f, 4, 1);
  draw_seg_bar(BAR_X, 12, BAR_W, 8, v.vu_r / 255.0f, 4, 1);

  // Peak-hold needles.
  const int pl = BAR_X + (int)(v.peak_l / 255.0f * (BAR_W - 1));
  const int pr = BAR_X + (int)(v.peak_r / 255.0f * (BAR_W - 1));
  if (v.peak_l > 4) u8g2.drawVLine(pl, 1, 10);
  if (v.peak_r > 4) u8g2.drawVLine(pr, 11, 10);

  // dB scale. Ticks at the values an engineer would look for; labels only where
  // there is room for them without collisions.
  static const int8_t ticks[] = {-40, -30, -20, -12, -6, -3, 0};
  for (uint8_t i = 0; i < sizeof(ticks); i++) {
    const float u = (ticks[i] + VU_DB_SPAN) / VU_DB_SPAN;
    const int x = BAR_X + (int)(u * (BAR_W - 1));
    const bool major = (ticks[i] == -40 || ticks[i] == -20 || ticks[i] == -6 ||
                        ticks[i] == 0);
    u8g2.drawVLine(x, 21, major ? 4 : 2);
  }
  u8g2.setFont(FONT_SMALL);
  struct {
    int8_t db;
    const char *label;
  } labels[] = {{-40, "-40"}, {-20, "-20"}, {-6, "-6"}, {0, "0"}};
  for (uint8_t i = 0; i < 4; i++) {
    const float u = (labels[i].db + VU_DB_SPAN) / VU_DB_SPAN;
    int x = BAR_X + (int)(u * (BAR_W - 1)) - u8g2.getUTF8Width(labels[i].label) / 2;
    if (x < 0) x = 0;
    if (x + u8g2.getUTF8Width(labels[i].label) > W) {
      x = W - u8g2.getUTF8Width(labels[i].label);
    }
    u8g2.drawUTF8(x, 31, labels[i].label);
  }

  (void)s;
  (void)dt;
}

static void draw_scope(const AudioVis &v) {
  // Graticule: dots on a grid, the way a scope screen is ruled. Drawn first so
  // the trace sits on top of it.
  for (int x = 0; x < W; x += 8) {
    for (int y = 4; y < H; y += 7) u8g2.drawPixel(x, y);
  }
  for (int x = 0; x < W; x += 2) u8g2.drawPixel(x, 16);

  int prev_y = 16 - (v.wave[0] * 15) / 127;
  for (int x = 1; x < VIS_WAVE_POINTS; x++) {
    int y = 16 - (v.wave[x] * 15) / 127;
    if (y < 0) y = 0;
    if (y > H - 1) y = H - 1;
    // Join consecutive samples: at 22 kHz a loud low note moves several pixels
    // per column, and unjoined dots look like noise rather than a waveform.
    u8g2.drawLine(x - 1, prev_y, x, y);
    prev_y = y;
  }

  // Corner brackets, for the instrument look.
  u8g2.drawHLine(0, 0, 5);
  u8g2.drawVLine(0, 0, 4);
  u8g2.drawHLine(W - 5, 0, 5);
  u8g2.drawVLine(W - 1, 0, 4);
  u8g2.drawHLine(0, H - 1, 5);
  u8g2.drawVLine(0, H - 4, 4);
  u8g2.drawHLine(W - 5, H - 1, 5);
  u8g2.drawVLine(W - 1, H - 4, 4);
}

/*
 * Scrolling spectrogram.
 *
 * One display column per frame, one display row per band: time runs right to
 * left, frequency bottom to top, and intensity comes out of the dither. It is
 * kept in its own buffer because it is cumulative -- the previous 127 columns
 * have to survive, which they would not if it were drawn into the frame buffer
 * that gets cleared and composited every frame.
 */
static void draw_waterfall(const AudioVis &v) {
  for (int p = 0; p < PAGES; p++) {
    memmove(&wf_fb[p * W], &wf_fb[p * W + 1], W - 1);
    wf_fb[p * W + (W - 1)] = 0;
  }
  const int x = W - 1;
  for (uint8_t b = 0; b < VIS_BANDS && b < H; b++) {
    const int y = H - 1 - b;  // low frequencies at the bottom
    if (dither((int)frame_counter, y, v.bands[b])) wf_set(x, y);
  }
  memcpy(fb(), wf_fb, FB_BYTES);
}

/*
 * HH:MM in whichever format the owner picked. The 12-hour form drops the
 * leading zero -- "6:51 PM" -- because these readouts share a 128 px row with
 * other text and the zero buys nothing.
 */
static void format_hhmm(char *out, size_t size, const struct tm &t) {
  if (soft_clock_use_24h()) {
    snprintf(out, size, "%02d:%02d", t.tm_hour, t.tm_min);
    return;
  }
  int hour = t.tm_hour % 12;
  if (hour == 0) hour = 12;
  snprintf(out, size, "%d:%02d%s", hour, t.tm_min, t.tm_hour < 12 ? "a" : "p");
}

static void draw_clock_screen(uint32_t now, uint32_t dt) {
  struct tm t;
  soft_clock_now(&t);

  int hour = t.tm_hour;
  const char *ampm = nullptr;
  if (!soft_clock_use_24h()) {
    ampm = (hour < 12) ? "AM" : "PM";
    hour = hour % 12;
    if (hour == 0) hour = 12;
  }

  /*
   * Burn-in wander. These panels do retain a bright static image, and a clock is
   * the most static thing here -- so the whole layout shifts one pixel left or
   * right as the minutes pass. Slow enough not to be noticed, fast enough that
   * no pixel stays lit for hours.
   */
  const int dx = (t.tm_min % 3) - 1;

  const int DW = 13, DH = 24, DT = 3;
  int x = 1 + dx;
  draw_7seg(x, 1, DW, DH, DT, SEG_DIGIT[hour / 10]);
  draw_7seg(x + 14, 1, DW, DH, DT, SEG_DIGIT[hour % 10]);
  // Colon blinks once a second, which is the only cue that the clock is live.
  if (t.tm_sec & 1) {
    u8g2.drawBox(x + 30, 7, 3, 3);
    u8g2.drawBox(x + 30, 16, 3, 3);
  }
  draw_7seg(x + 37, 1, DW, DH, DT, SEG_DIGIT[t.tm_min / 10]);
  draw_7seg(x + 51, 1, DW, DH, DT, SEG_DIGIT[t.tm_min % 10]);

  static const char *WDAY[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  static const char *MON[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  char line[24];
  const int rx = 69 + dx;

  u8g2.setFont(FONT_TEXT);
  snprintf(line, sizeof(line), "%s %d", WDAY[t.tm_wday % 7], t.tm_mday);
  u8g2.drawUTF8(rx, 9, line);

  // A trailing "?" is the honest way to say the clock came from the build stamp
  // and nobody has confirmed it since.
  snprintf(line, sizeof(line), "%s %d%s", MON[t.tm_mon % 12], t.tm_year + 1900,
           soft_clock_trusted() ? "" : "?");
  u8g2.drawUTF8(rx, 19, line);

  u8g2.setFont(FONT_SMALL);
  if (ampm != nullptr) {
    snprintf(line, sizeof(line), ":%02d %s", t.tm_sec, ampm);
  } else {
    snprintf(line, sizeof(line), ":%02d", t.tm_sec);
  }
  u8g2.drawUTF8(rx, 27, line);

  // Seconds sweep along the bottom edge: a whole minute at a glance.
  const int sweep = (int)(((t.tm_sec * 1000 + (now % 1000)) * W) / 60000);
  u8g2.drawBox(0, H - 2, sweep < 1 ? 1 : sweep, 2);

  (void)dt;
}

static void draw_info(const PlayerInfo &s, const AudioVis &v, uint32_t now,
                      uint32_t dt) {
  char line[72];

  // --- row 1: who we are ---
  u8g2.drawXBMP(0, 0, 8, 8, ICON_BT);
  u8g2.setFont(FONT_TEXT);
  draw_marquee(MQ_NAME, 10, 7, W - 10, ps_device_name(), dt);

  // --- row 2: who is connected, and at what rate ---
  u8g2.setFont(FONT_TEXT);
  if (s.connected) {
    const char *who = s.peer[0] ? s.peer : "phone";
    char rate[10];
    snprintf(rate, sizeof(rate), "%u.%uk", (unsigned)(s.sample_rate / 1000),
             (unsigned)((s.sample_rate % 1000) / 100));
    const int rw = u8g2.getUTF8Width(rate);
    draw_right(W, 15, rate);
    u8g2.drawXBMP(0, 8, 8, 8, ICON_PHONE);
    draw_marquee(MQ_PEER, 10, 15, W - 10 - rw - 3, who, dt);
  } else {
    u8g2.drawUTF8(0, 15, "waiting for a phone");
  }

  // --- row 3: volume, and the battery on the right when there is one ---
  u8g2.setFont(FONT_SMALL);
  const int pct = (s.volume * 100 + 63) / 127;
  snprintf(line, sizeof(line), "%d%%", pct);
  const int pw = u8g2.getUTF8Width(line);
  u8g2.drawUTF8(0, 22, "VOL");

  BatteryStatus bat;
  battery_snapshot(&bat);
  int right_edge = W;
  if (bat.enabled) {
    char bpct[8];
    if (bat.present) snprintf(bpct, sizeof(bpct), "%u%%", (unsigned)bat.percent);
    else strlcpy(bpct, "--", sizeof(bpct));
    const int bpw = u8g2.getUTF8Width(bpct);
    draw_battery(W - 14, 16, bat);
    u8g2.drawUTF8(W - 14 - bpw - 2, 22, bpct);
    right_edge = W - 14 - bpw - 5;
  }
  draw_right(right_edge, 22, line);
  draw_seg_bar(16, 17, right_edge - 16 - pw - 4, 5, s.volume / 127.0f, 3, 1);

  // --- row 4: the numbers you want when something is odd ---
  char up[12];
  fmt_uptime(now, up, sizeof(up));
  static char network_line[72] = "network starting";
  static uint32_t network_at;
  if (now - network_at >= 2000 || network_at == 0) {
    network_at = now;
    const RadioMode mode = management_radio_mode();
    if (!radio_mode_has_wifi(mode)) {
      // The Wi-Fi driver was never initialised in this mode; do not ask it.
      strlcpy(network_line, "bluetooth mode  wifi off", sizeof(network_line));
    } else if (WiFi.status() == WL_CONNECTED) {
      const IPAddress ip = WiFi.localIP();
      const String ssid = WiFi.SSID();
      snprintf(network_line, sizeof(network_line),
               "%s%s  %u.%u.%u.%u  %ddBm",
               mode == RADIO_MODE_COMBO      ? "wifi+bt "
               : mode == RADIO_MODE_NET      ? "wifi+ble "
               : mode == RADIO_MODE_DFPLAYER ? "sd+wifi "
                                             : "wifi ",
               ssid.c_str(), ip[0], ip[1], ip[2], ip[3], WiFi.RSSI());
    } else if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
      const IPAddress ip = WiFi.softAPIP();
      snprintf(network_line, sizeof(network_line), "setup AP  %u.%u.%u.%u", ip[0],
               ip[1], ip[2], ip[3]);
    } else {
      strlcpy(network_line, "wifi offline", sizeof(network_line));
    }
  }
  /*
   * Three lines, not two, when there is a DFPlayer to talk about: what the
   * module is doing is the thing you would look at this screen for in that mode,
   * and it has no home on any other screen.
   */
  static char df_line[72] = "dfplayer  starting";
  static uint32_t df_at;
  const bool have_df = radio_mode_has_dfplayer(management_radio_mode());
  DfStatus d;
  // The snapshot's return value matters here: a failed one is zero-filled, and
  // acting on it would put "no reply - check TX/RX" on screen about a module
  // that is answering perfectly well. Keep the last line instead.
  if (have_df && (df_at == 0 || now - df_at >= 1000) && df_player_snapshot(&d)) {
    df_at = now | 1;  // never 0: that is the "not yet" sentinel
    if (!d.running) {
      strlcpy(df_line, "dfplayer  driver off", sizeof(df_line));
    } else if (d.asleep) {
      // Before the offline test: nothing is asked of a sleeping module, so it
      // answers nothing, and "no reply" would be the wrong diagnosis.
      strlcpy(df_line, "dfplayer  standby - wake it to play", sizeof(df_line));
    } else if (!d.online) {
      strlcpy(df_line, "dfplayer  no reply - check TX/RX", sizeof(df_line));
    } else if (d.pcLink) {
      strlcpy(df_line, "dfplayer  card mounted on a computer", sizeof(df_line));
    } else {
      snprintf(df_line, sizeof(df_line), "%s  %s  trk %u/%u  vol %u/%u  eq %s",
               df_source_name(d.source), df_state_name(d.state),
               (unsigned)d.track, (unsigned)d.totalTracks, (unsigned)d.volume,
               (unsigned)DF_VOLUME_MAX, df_eq_name(d.eq));
    }
  }

  const uint32_t slot = (now / 5000) % (have_df ? 3 : 2);
  if (slot == 0) {
    strlcpy(line, network_line, sizeof(line));
  } else if (slot == 1) {
    int n = snprintf(line, sizeof(line),
                     "up %s  heap %uk  %ufps  agc %ddB  clk %s", up,
                     (unsigned)(ESP.getFreeHeap() / 1024),
                     (unsigned)(fps_avg + 0.5f), (int)v.agc_db,
                     soft_clock_source_name());
    if (bat.present && n > 0 && (size_t)n < sizeof(line)) {
      snprintf(line + n, sizeof(line) - n, "  bat %.2fV %s", bat.volts,
               battery_state_name(bat.state));
    }
  } else {
    strlcpy(line, df_line, sizeof(line));
  }
  u8g2.setFont(FONT_SMALL);
  draw_marquee(MQ_STATS, 0, 30, W, line, dt);
}

static void draw_pairing(uint32_t now, uint32_t dt) {
  /*
   * Animated beacon: rings expanding out of the Bluetooth rune, on the right
   * hand side only, so it reads as "broadcasting" rather than "loading". Two
   * rings a half period apart keeps it continuous.
   */
  PlayerInfo s;
  ps_snapshot(&s);

  // "Waiting for something to play" is the same screen whichever mode we are
  // in; only the words change. The beacon animates whenever a source is
  // actually listening -- an advertising A2DP sink, or a DLNA renderer.
  const bool listening = s.bt_active || s.source == PS_SRC_NETWORK ||
                         (s.source == PS_SRC_DFPLAYER && s.connected);

  const int cx = 12, cy = 16;
  u8g2.drawXBMP(cx - 8, cy - 8, 16, 16, ICON_BT_BIG);
  for (int k = 0; listening && k < 2; k++) {
    // now % 4096 rather than now: a float loses its fractional bits once the
    // millisecond count gets large, and the animation would start to stutter
    // after a few weeks of uptime.
    const float phase = fmodf(((now % 4096) * 0.0009f) + k * 0.5f, 1.0f);
    const int r = 10 + (int)(phase * 8.0f);
    // Fade the outermost ring out by drawing it as a dotted arc.
    if (phase < 0.75f) {
      u8g2.drawCircle(cx, cy, r, U8G2_DRAW_UPPER_RIGHT | U8G2_DRAW_LOWER_RIGHT);
    } else {
      for (int a = -50; a <= 50; a += 20) {
        const float rad = a * (float)M_PI / 180.0f;
        u8g2.drawPixel(cx + (int)(cosf(rad) * r), cy + (int)(sinf(rad) * r));
      }
    }
  }

  u8g2.setFont(FONT_TITLE);
  // This screen is the first place anyone looks, so it never claims a capability
  // the running mode does not have: no "ready to pair" without an A2DP sink, and
  // no "cast to me" without a network renderer.
  const char *headline = s.bt_active                     ? "Ready to pair"
                         : s.source == PS_SRC_NETWORK   ? "Ready to cast"
                         : s.source == PS_SRC_DFPLAYER  ? (s.connected
                                                              ? "Card ready"
                                                              : "No module")
                                                        : "Wi-Fi mode";
  u8g2.drawUTF8(32, 11, headline);

  u8g2.setFont(FONT_TEXT);
  if (s.source == PS_SRC_DFPLAYER) {
    // The title line already carries the module's own words ("Nothing playing",
    // "Card mounted on a computer"), which is more useful here than the device
    // name -- nothing is going to connect to it in this mode.
    draw_marquee(MQ_NAME, 32, 21, W - 32,
                 s.title[0] ? s.title : "Waiting for the module", dt);
  } else if (s.bt_active || s.source == PS_SRC_NETWORK) {
    draw_marquee(MQ_NAME, 32, 21, W - 32, ps_device_name(), dt);
  } else {
    draw_marquee(MQ_NAME, 32, 21, W - 32,
                 "Bluetooth off - hold BOOT to switch", dt);
  }

  struct tm t;
  soft_clock_now(&t);
  char line[32], clock[12];
  format_hhmm(clock, sizeof(clock), t);
  snprintf(line, sizeof(line), "%s  %d/%d", clock, t.tm_mday, t.tm_mon + 1);
  u8g2.setFont(FONT_SMALL);
  u8g2.drawUTF8(32, 31, line);
}

static void draw_saver(uint32_t now) {
  /*
   * The screensaver exists for the panel, not for the user: after five minutes
   * of silence every lit pixel needs to move. So the clock bounces around, and
   * nothing is ever static long enough to burn in.
   */
  static int sx = 20, sy = 8, vx = 1, vy = 1;
  static uint32_t last_step;
  static const int BW = 44, BH = 15;

  if (now - last_step > 40) {
    last_step = now;
    sx += vx;
    sy += vy;
    if (sx <= 0) { sx = 0; vx = 1; }
    if (sx >= W - BW) { sx = W - BW; vx = -1; }
    if (sy <= 0) { sy = 0; vy = 1; }
    if (sy >= H - BH) { sy = H - BH; vy = -1; }
  }

  struct tm t;
  soft_clock_now(&t);
  char line[12];
  format_hhmm(line, sizeof(line), t);

  u8g2.drawRFrame(sx, sy, BW, BH, 3);
  u8g2.drawXBMP(sx + 4, sy + 4, 8, 8, ICON_CLOCK);
  u8g2.setFont(FONT_TEXT);
  u8g2.drawUTF8(sx + 15, sy + 11, line);
}

// ================================================================ overlays ===

static void draw_volume_popup(const PlayerInfo &s) {
  draw_panel(0, 0, W, H);

  u8g2.drawXBMP(6, 6, 16, 8, s.volume == 0 ? ICON_MUTE : ICON_SPEAKER);

  u8g2.setFont(FONT_SMALL);
  u8g2.drawUTF8(26, 12, "VOLUME");

  char num[8];
  const int pct = (s.volume * 100 + 63) / 127;
  snprintf(num, sizeof(num), "%d", pct);
  u8g2.setFont(FONT_BIGNUM);
  const int nw = u8g2.getUTF8Width(num);
  u8g2.drawUTF8(104 - nw, 17, num);
  u8g2.setFont(FONT_SMALL);
  u8g2.drawUTF8(106, 17, "%");

  draw_seg_bar(7, 22, W - 14, 7, s.volume / 127.0f, 4, 1);
}

static void draw_toast(const PlayerInfo &s, uint32_t dt) {
  draw_panel(0, 0, W, H);

  const uint8_t *icon = ICON_BT;
  const char *head = "";
  const char *detail = "";

  switch (toast_kind) {
    case TOAST_CONNECTED:
      head = "Connected";
      detail = s.peer[0] ? s.peer : "ready to play";
      break;
    case TOAST_DISCONNECTED:
      head = "Disconnected";
      detail = ps_device_name();
      break;
    case TOAST_TRACK:
      icon = ICON_NOTE;
      head = s.title;
      detail = s.artist[0] ? s.artist : s.album;
      break;
    default:
      return;
  }

  u8g2.drawXBMP(6, 4, 8, 8, icon);
  u8g2.setFont(FONT_TITLE);
  draw_marquee(MQ_TOAST_A, 18, 12, W - 24, head, dt);
  u8g2.setFont(FONT_TEXT);
  draw_marquee(MQ_TOAST_B, 7, 26, W - 14, detail, dt);
}

/// Is an overlay on screen right now? A read with no side effects, unlike the
/// snapshot below, which also retires an expired one.
static bool system_overlay_visible(uint32_t now) {
  bool visible;
  portENTER_CRITICAL(&system_overlay_mux);
  visible = system_overlay.active &&
            (system_overlay.until == 0 ||
             (int32_t)(now - system_overlay.until) < 0);
  portEXIT_CRITICAL(&system_overlay_mux);
  return visible;
}

static bool system_overlay_snapshot(SystemOverlay *out, uint32_t now) {
  bool visible;
  portENTER_CRITICAL(&system_overlay_mux);
  if (system_overlay.active && system_overlay.until != 0 &&
      (int32_t)(now - system_overlay.until) >= 0) {
    system_overlay.active = false;
  }
  visible = system_overlay.active;
  if (visible) memcpy(out, &system_overlay, sizeof(*out));
  portEXIT_CRITICAL(&system_overlay_mux);
  return visible;
}

static void draw_system_overlay(const SystemOverlay &status, uint32_t dt) {
  draw_panel(0, 0, W, H);

  const uint8_t *icon = ICON_UPDATE;
  const char *label = "FIRMWARE";
  switch (status.kind) {
    case UI_STATUS_NETWORK:
      icon = ICON_WIFI;
      label = "NETWORK";
      break;
    case UI_STATUS_SUCCESS:
      icon = ICON_OK;
      label = "COMPLETE";
      break;
    case UI_STATUS_ERROR:
      icon = ICON_ERROR;
      label = "ATTENTION";
      break;
    case UI_STATUS_RESTART:
      icon = ICON_UPDATE;
      label = "SYSTEM";
      break;
    default:
      break;
  }

  u8g2.drawXBMP(5, 3, 8, 8, icon);
  u8g2.setFont(FONT_SMALL);
  u8g2.drawUTF8(17, 9, label);
  if (status.progress >= 0) {
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", status.progress);
    draw_right(W - 5, 9, pct);
  }

  u8g2.setFont(FONT_TITLE);
  draw_marquee(MQ_SYSTEM_A, 5, 20, W - 10, status.title, dt);

  if (status.progress >= 0) {
    const int fill = (status.progress * (W - 12)) / 100;
    u8g2.drawRFrame(4, 24, W - 8, 7, 2);
    if (fill > 0) u8g2.drawBox(6, 26, fill, 3);
  } else {
    u8g2.setFont(FONT_SMALL);
    draw_marquee(MQ_SYSTEM_B, 5, 29, W - 10, status.detail, dt);
  }
}

// ============================================================= transitions ===
/*
 * Screen changes are animated by compositing the previous frame (kept in
 * prev_fb) with the one just rendered. All three work directly on the 512-byte
 * buffer, which is laid out as four 128-byte pages of eight vertical pixels --
 * so anything that only moves content horizontally is a memmove, and costs
 * nothing worth measuring.
 */
static void compose_transition(uint32_t elapsed) {
  float p = (float)elapsed / (float)UI_TRANSITION_MS;
  if (p >= 1.0f) {
    trans_active = false;
    return;
  }
  // Ease-out, so the motion arrives rather than stops.
  p = 1.0f - (1.0f - p) * (1.0f - p);

  uint8_t *cur = fb();

  switch (trans_kind) {
    case TRANS_WIPE: {
      const int edge = (int)(p * W);
      for (int page = 0; page < PAGES; page++) {
        uint8_t *c = cur + page * W;
        const uint8_t *o = prev_fb + page * W;
        for (int x = edge; x < W; x++) c[x] = o[x];
      }
      if (edge > 0 && edge < W) u8g2.drawVLine(edge, 0, H);  // leading edge
      break;
    }
    case TRANS_DISSOLVE: {
      // Per-pixel, but only 4096 of them: the new frame appears through a
      // growing dither mask, which on a one-bit panel looks like a cross-fade.
      const uint8_t thr = (uint8_t)(p * 16.0f);
      for (int page = 0; page < PAGES; page++) {
        uint8_t *c = cur + page * W;
        const uint8_t *o = prev_fb + page * W;
        for (int x = 0; x < W; x++) {
          uint8_t mask = 0;
          for (int bit = 0; bit < 8; bit++) {
            const int y = page * 8 + bit;
            if (BAYER4[(y & 3) * 4 + (x & 3)] < thr) mask |= (uint8_t)(1u << bit);
          }
          c[x] = (uint8_t)((c[x] & mask) | (o[x] & (uint8_t)~mask));
        }
      }
      break;
    }
    default: {  // TRANS_SLIDE -- old frame exits left, new one follows it in
      const int shift = (int)(p * W);
      static uint8_t row[W];
      for (int page = 0; page < PAGES; page++) {
        uint8_t *c = cur + page * W;
        const uint8_t *o = prev_fb + page * W;
        const int keep = W - shift;
        for (int x = 0; x < keep; x++) row[x] = o[x + shift];
        for (int x = keep; x < W; x++) row[x] = c[x - keep];
        memcpy(c, row, W);
      }
      break;
    }
  }
}

// ================================================================== engine ===

static bool screen_eligible(UiScreen s, const PlayerInfo &info, uint32_t now) {
  // The analyser reports 0 until it has ever seen audio, which is not "active
  // 0 ms ago" -- without the first test, every visualiser would be eligible for
  // the first 2.5 seconds after boot with nothing to show.
  const uint32_t heard = audio_probe_last_active();
  const bool audio = heard != 0 && (now - heard) < 2500;
  switch (s) {
    case SCR_NOW_PLAYING:
      // Needs a phone. A title is not required -- plenty of players never send
      // AVRCP metadata at all, and the progress row and spectrum are still worth
      // showing.
      return info.connected;
    case SCR_SPECTRUM:
    case SCR_VU:
    case SCR_SCOPE:
    case SCR_WATERFALL:
      return audio;
    case SCR_CLOCK:
    case SCR_INFO:
      return true;
    default:
      return false;
  }
}

static UiScreen next_eligible(UiScreen from, const PlayerInfo &info,
                              uint32_t now) {
  for (uint8_t i = 1; i <= SCR_ROTATE_COUNT; i++) {
    const UiScreen c = (UiScreen)((from + i) % SCR_ROTATE_COUNT);
    if (screen_eligible(c, info, now)) return c;
  }
  return SCR_CLOCK;  // always eligible, so this is unreachable in practice
}

static void on_enter(UiScreen s) {
  if (s == SCR_SPECTRUM) {
    // A different style every time round, so the carousel does not feel like a
    // loop of three fixed pictures.
    spectrum_style = (uint8_t)((spectrum_style + 1) % 3);
  }
  if (s == SCR_WATERFALL) memset(wf_fb, 0, sizeof(wf_fb));
}

static UiScreen pick_screen(const PlayerInfo &info, uint32_t now,
                            bool deep_idle) {
  // An explicit "screen N" from the console wins over everything, including the
  // eligibility rules: if it was asked for by hand, show it.
  const int8_t req = req_screen;
  if (req >= 0) {
    req_screen = -1;
    carousel_paused = true;
    screen_since = now;
    return (UiScreen)req;
  }

  if (deep_idle) return SCR_SAVER;
  if (!info.connected) return SCR_PAIRING;

  if (cur_screen == SCR_PAIRING || cur_screen == SCR_SAVER) {
    screen_since = now;
    return screen_eligible(SCR_NOW_PLAYING, info, now) ? SCR_NOW_PLAYING
                                                       : SCR_CLOCK;
  }

  if (req_next) {
    req_next = false;
    carousel_paused = false;
    screen_since = now;
    return next_eligible(cur_screen, info, now);
  }

  // A pinned screen stays pinned even when it has nothing to show -- otherwise
  // "hold the spectrum" would spring back to the clock the moment the music
  // paused, which is not what pinning means.
  if (carousel_paused) return cur_screen;

  if (!screen_eligible(cur_screen, info, now) ||
      now - screen_since >= UI_SCREEN_DWELL_MS) {
    screen_since = now;
    return next_eligible(cur_screen, info, now);
  }
  return cur_screen;
}

static void detect_events(const PlayerInfo &info, uint32_t now) {
  if (!have_prev_state) {
    // First frame: adopt the current values so boot does not fire three toasts.
    have_prev_state = true;
    prev_connected = info.connected;
    prev_track_seq = info.track_seq;
    prev_volume_seq = info.volume_seq;
    return;
  }

  if (info.connected != prev_connected) {
    prev_connected = info.connected;
    toast_kind = info.connected ? TOAST_CONNECTED : TOAST_DISCONNECTED;
    toast_until = now + UI_TOAST_MS;
    last_activity_ms = now;
  }

  if (info.track_seq != prev_track_seq) {
    prev_track_seq = info.track_seq;
    if (info.title[0] != 0) {
      toast_kind = TOAST_TRACK;
      toast_until = now + UI_TOAST_MS;
    }
    last_activity_ms = now;
  }

  if (info.volume_seq != prev_volume_seq) {
    prev_volume_seq = info.volume_seq;
    popup_until = now + UI_VOLUME_POPUP_MS;
    last_activity_ms = now;
  }
}

/*
 * One button, four tiers, longest wins:
 *
 *   < 600 ms          next screen
 *   600 ms - 2.5 s    pin the current screen / release it
 *   2.5 s             brightness step
 *   3 s .. 8 s        factory reset countdown -- release cancels
 *
 * The countdown owns the panel while it runs so there is no way to trigger a
 * reset without watching it happen, and it clears itself on release.
 */
static void poll_button(uint32_t now) {
  if (PIN_UI_BUTTON < 0) return;
  const bool down = digitalRead(PIN_UI_BUTTON) == LOW;

  if (down && !btn_down) {
    btn_down = true;
    btn_since = now;
    btn_consumed = false;
    btn_reset_shown = 0xFF;
    btn_reset_fired = false;
    btn_mode_offered = false;
    return;
  }

  if (down && !btn_consumed && now - btn_since >= UI_BTN_HOLD_MS) {
    // Held: brightness, and stop there so releasing does not also switch screen.
    btn_consumed = true;
    bright_level = (uint8_t)((bright_level + 1) % 3);
    bright_override = 0;
    note_input(now);
    return;
  }

  // Keep holding and the speaker offers to swap which radio is running. The
  // offer is only made here; it is confirmed by a separate short press below,
  // so a pocket cannot toggle the mode on its own.
  if (down && !btn_mode_offered && now - btn_since >= UI_BTN_MODE_MS) {
    btn_mode_offered = true;
    btn_mode_until = now + UI_BTN_MODE_CONFIRM_MS;
    char title[24];
    snprintf(title, sizeof(title), "%s mode?",
             management_mode_name(management_next_mode()));
    ui_show_system_status(UI_STATUS_NETWORK, title, "Let go, then press BOOT",
                          -1, UI_BTN_MODE_CONFIRM_MS);
    status_led_blip(2);
    note_input(now);
    return;
  }

  if (down && now - btn_since >= UI_BTN_RESET_ARM_MS) {
    const uint32_t into = now - btn_since - UI_BTN_RESET_ARM_MS;
    if (into >= UI_BTN_RESET_COUNT_MS) {
      if (!btn_reset_fired) {
        btn_reset_fired = true;
        btn_reset_request = true;
        ui_show_system_status(UI_STATUS_RESTART, "Factory reset",
                              "Release the button", 100, 0);
      }
      return;
    }
    // One redraw per second rather than per frame, so the digit is readable and
    // the LED blip marks each tick.
    const uint8_t left = (uint8_t)((UI_BTN_RESET_COUNT_MS - into + 999) / 1000);
    if (left != btn_reset_shown) {
      btn_reset_shown = left;
      char detail[32];
      snprintf(detail, sizeof(detail), "Release to cancel  %u", left);
      ui_show_system_status(UI_STATUS_ERROR, "Factory reset", detail,
                            (int16_t)(100 - into * 100 / UI_BTN_RESET_COUNT_MS),
                            0);
      status_led_blip(1);
    }
    note_input(now);
    return;
  }

  if (!down && btn_down) {
    btn_down = false;
    const uint32_t held = now - btn_since;
    const bool was_dark = panel_off;
    note_input(now);
    // With the panel blanked the first press only brings it back. Anything else
    // means the screen you asked to see is one you never got to look at.
    if (was_dark) {
      btn_reset_shown = 0xFF;
      return;
    }
    if (btn_reset_shown != 0xFF && !btn_reset_fired) {
      btn_reset_shown = 0xFF;
      ui_show_system_status(UI_STATUS_SUCCESS, "Factory reset",
                            "Cancelled", -1, 2000);
      return;
    }
    btn_reset_shown = 0xFF;
    if (btn_consumed) return;

    // A short press while the offer stands is the confirmation.
    if (held < UI_BTN_LONG_MS && held > 25 && btn_mode_until &&
        (int32_t)(now - btn_mode_until) < 0) {
      btn_mode_until = 0;
      btn_mode_request = true;
      ui_show_system_status(UI_STATUS_RESTART, "Switching mode", "Restarting",
                            -1, 0);
      return;
    }
    btn_mode_until = 0;

    if (held >= UI_BTN_LONG_MS) {
      carousel_paused = !carousel_paused;
    } else if (held > 25) {  // anything shorter is contact bounce
      req_next = true;
    }
  }
}

static void apply_brightness(bool dim) {
  uint8_t want;
  if (bright_override != 0) {
    want = bright_override;
  } else if (dim) {
    want = UI_BRIGHT_DIM;
  } else {
    want = bright_level == 0 ? UI_BRIGHT_LOW
                             : (bright_level == 1 ? UI_BRIGHT_MID
                                                  : UI_BRIGHT_HIGH);
  }
  if (want != bright_applied) {
    bright_applied = want;
    u8g2.setContrast(want);
  }
}

static void ui_frame() {
  const uint32_t now = millis();
  uint32_t dt = now - last_frame_ms;
  if (dt == 0) dt = 1;
  if (dt > 500) dt = 500;
  last_frame_ms = now;
  frame_counter++;
  fps_avg += (1000.0f / dt - fps_avg) * 0.05f;

  PlayerInfo info;
  ps_snapshot(&info);
  // A copy rather than a reference into the analyser: the lighting task reads
  // the same analysis, and a reference would be live under its feet.
  AudioVis vis;
  audio_probe_frame(&vis, 1000 / UI_FPS);

  /*
   * What counts as the speaker being in use.
   *
   * Not info.streaming, which is the transport's *declared* state and is wrong
   * about the one case this has to get right. Pausing on a phone normally
   * leaves the AVDTP stream in STARTED and simply sends silence -- no state
   * change is signalled, often none ever is -- so `streaming` stayed true from
   * the first track until the phone disconnected, and the panel never blanked
   * however short the timeout was set.
   *
   * The analyser is the honest answer because it is looking at the samples, and
   * every source that passes through this chip feeds it: A2DP, the network
   * player and the chimes alike. Its last-heard timestamp rather than the
   * instantaneous flag, so a fade or the gap between two tracks does not read
   * as "stopped" -- and df_player_active() alongside it, because that module
   * decodes its own card and the analyser never hears it at all.
   */
  const uint32_t heard = audio_probe_last_active();
  const bool audible = heard != 0 && (now - heard) < UI_AUDIO_GRACE_MS;
  if (audible || df_player_active()) last_activity_ms = now;
  detect_events(info, now);
  poll_button(now);

  const uint32_t idle = now - last_activity_ms;
  const bool dim = idle > UI_DIM_AFTER_MS;
  const bool deep_idle = idle > UI_SLEEP_AFTER_MS;

  /*
   * Blanking, decided before anything is drawn: the I2C frame is by far the
   * expensive part of this task, and a panel that is off has no use for one.
   *
   * The two modes differ only in which clock they read. IDLE reads the one
   * audio keeps warm, so playback holds the display open; ALWAYS reads the one
   * only the owner touches, so it does not.
   */
  bool blank = power_save;  // saving takes the panel outright, with no timeout
  const uint8_t mode = blank_mode;
  const uint32_t after = blank_after_ms;
  if (mode == UI_BLANK_IDLE) blank = blank || idle > after;
  else if (mode == UI_BLANK_ALWAYS) blank = blank || (now - last_input_ms) > after;
  // An update, a restart or the reset countdown suspends it: those are exactly
  // the moments somebody is watching the panel, and going dark through one is
  // indistinguishable from a crash.
  if (blank && system_overlay_visible(now)) blank = false;

  if (blank != panel_off) {
    panel_off = blank;
    u8g2.setPowerSave(blank ? 1 : 0);
    // Coming back, the contrast register is re-applied on the next frame
    // anyway; going away, forget what was applied so it cannot be skipped.
    if (!blank) bright_applied = 0;
  }
  if (panel_off) return;

  const UiScreen want = pick_screen(info, now, deep_idle);
  if (want != cur_screen) {
    // The buffer still holds the last frame, which is exactly what the
    // transition needs to slide out.
    memcpy(prev_fb, fb(), FB_BYTES);
    trans_active = true;
    trans_start = now;
    trans_kind = (uint8_t)((trans_kind + 1) % TRANS_COUNT);
    cur_screen = want;
    on_enter(want);
  }

  u8g2.clearBuffer();
  u8g2.setFontMode(1);  // transparent: glyphs never erase what is behind them
  u8g2.setDrawColor(1);

  switch (cur_screen) {
    case SCR_NOW_PLAYING: draw_now_playing(info, vis, now, dt); break;
    case SCR_SPECTRUM: draw_spectrum(vis, now); break;
    case SCR_VU: draw_vu(info, vis, dt); break;
    case SCR_SCOPE: draw_scope(vis); break;
    case SCR_WATERFALL: draw_waterfall(vis); break;
    case SCR_CLOCK: draw_clock_screen(now, dt); break;
    case SCR_INFO: draw_info(info, vis, now, dt); break;
    case SCR_PAIRING: draw_pairing(now, dt); break;
    case SCR_SAVER: draw_saver(now); break;
    default: break;
  }

  if (trans_active) compose_transition(now - trans_start);

  // System operations win while active: losing update progress behind a volume
  // popup would make a deliberate restart look like a crash. Ordinary volume
  // still wins over Bluetooth/track toasts.
  SystemOverlay status;
  if (system_overlay_snapshot(&status, now)) {
    draw_system_overlay(status, dt);
  } else if ((int32_t)(popup_until - now) > 0) {
    draw_volume_popup(info);
  } else if ((int32_t)(toast_until - now) > 0 && toast_kind != TOAST_NONE) {
    draw_toast(info, dt);
  } else {
    toast_kind = TOAST_NONE;
  }

  apply_brightness(dim);
  u8g2.sendBuffer();
}

static void ui_task(void *) {
  TickType_t period = pdMS_TO_TICKS(1000 / UI_FPS);
  if (period < 1) period = 1;
  TickType_t next = xTaskGetTickCount() + period;
  last_frame_ms = millis();

  for (;;) {
    if (suspended) {
      if (!panel_off) {
        panel_off = true;
        u8g2.setPowerSave(1);
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    ui_frame();

    /*
     * Fixed cadence, so a slow frame does not make every following frame late
     * as well -- but never a zero delay.
     *
     * vTaskDelayUntil() on its own returns immediately when the deadline has
     * already passed, and if rendering ever became slower than the frame budget
     * that would spin this task forever at priority 1 on core 0, starve the
     * core-0 idle task and trip the task watchdog. So an overrun explicitly
     * resets the schedule and still yields for a tick.
     */
    const TickType_t now_t = xTaskGetTickCount();
    if ((int32_t)(next - now_t) <= 0) {
      next = now_t + period;
      vTaskDelay(1);
    } else {
      vTaskDelay(next - now_t);
      next += period;
    }
  }
}

// ================================================================== public ===

static bool probe_addr(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

bool ui_begin() {
#if !UI_ENABLED
  return false;
#else
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL, 100000);

  uint8_t addr = 0;
  if (probe_addr(OLED_ADDR_PRIMARY)) {
    addr = OLED_ADDR_PRIMARY;
  } else if (probe_addr(OLED_ADDR_ALTERNATE)) {
    addr = OLED_ADDR_ALTERNATE;
  } else {
    // No panel: say so once and leave. Everything else in this file is then
    // never called, and the speaker behaves exactly as it did without a display.
    Serial.printf("[ui] no SSD1306 at 0x%02X or 0x%02X on SDA=%d SCL=%d\n",
                  OLED_ADDR_PRIMARY, OLED_ADDR_ALTERNATE, PIN_OLED_SDA,
                  PIN_OLED_SCL);
    return false;
  }

  u8g2.setI2CAddress(addr << 1);  // u8g2 wants the 8-bit form
  u8g2.setBusClock(OLED_BUS_HZ);
  if (!u8g2.begin()) {
    Serial.println("[ui] panel found but init failed");
    return false;
  }
  g_present = true;

  if (PIN_UI_BUTTON >= 0) pinMode(PIN_UI_BUTTON, INPUT_PULLUP);

  // Splash, drawn synchronously so there is something on the panel during the
  // couple of seconds the Bluetooth stack takes to come up.
  u8g2.clearBuffer();
  u8g2.drawXBMP(4, 8, 16, 16, ICON_BT_BIG);
  u8g2.setFont(FONT_TITLE);
  u8g2.drawUTF8(26, 14, ps_device_name()[0] ? ps_device_name() : "ESP32");
  u8g2.setFont(FONT_SMALL);
  char version[28];
  snprintf(version, sizeof(version), "firmware v%s", FW_VERSION);
  u8g2.drawUTF8(26, 24, version);
  u8g2.drawHLine(26, 27, 96);
  u8g2.setContrast(UI_BRIGHT_MID);
  bright_applied = UI_BRIGHT_MID;
  u8g2.sendBuffer();

  Serial.printf("[ui] SSD1306 128x32 at 0x%02X, %u kHz, %u fps\n", addr,
                (unsigned)(OLED_BUS_HZ / 1000), (unsigned)UI_FPS);
  return true;
#endif
}

void ui_start() {
  if (!g_present) return;
  note_input(millis());
  screen_since = millis();
  // Core 0, priority 1: the Bluetooth controller and the audio path both
  // outrank it, so a slow I2C frame can never delay a sample.
  xTaskCreatePinnedToCore(ui_task, "ui", 5120, nullptr, 1, nullptr, 0);
}

bool ui_present() { return g_present; }

void ui_wake() {
  note_input(millis());
  // Standby counts down from the same events. Hanging it here rather than
  // duplicating the call sites is what keeps the two from disagreeing.
  power_note_activity();
}

void ui_set_blank(UiBlankMode mode, uint16_t after_seconds) {
  if (mode > UI_BLANK_ALWAYS) mode = UI_BLANK_NEVER;
  if (after_seconds < UI_BLANK_AFTER_S_MIN) after_seconds = UI_BLANK_AFTER_S_MIN;
  if (after_seconds > UI_BLANK_AFTER_S_MAX) after_seconds = UI_BLANK_AFTER_S_MAX;
  blank_mode = (uint8_t)mode;
  blank_after_ms = (uint32_t)after_seconds * 1000UL;
  // Waking is the point: whatever the owner just chose, they should see the
  // panel come back rather than wonder whether the setting took.
  ui_wake();
}

bool ui_blanked() { return panel_off; }

void ui_suspend() {
  if (!g_present) return;
  // The task does the powering down, on the task that owns the I2C bus. Setting
  // the flag and waiting is what keeps two writers off one bus.
  suspended = true;
}

void ui_set_power_save(bool on) {
  if (on == power_save) return;
  power_save = on;
  // Leaving saving is an event worth seeing, and the panel coming back to a
  // stale screen a timeout away from blanking again is not.
  if (!on) ui_wake();
}

uint32_t ui_idle_ms() { return millis() - last_activity_ms; }
uint32_t ui_untouched_ms() { return millis() - last_input_ms; }

void ui_show_system_status(UiSystemStatus kind, const char *title,
                           const char *detail, int16_t progress,
                           uint32_t duration_ms) {
  portENTER_CRITICAL(&system_overlay_mux);
  system_overlay.kind = kind;
  strlcpy(system_overlay.title, title ? title : "", sizeof(system_overlay.title));
  strlcpy(system_overlay.detail, detail ? detail : "", sizeof(system_overlay.detail));
  system_overlay.progress = progress < 0 ? -1 : (progress > 100 ? 100 : progress);
  system_overlay.until = duration_ms ? millis() + duration_ms : 0;
  system_overlay.active = true;
  portEXIT_CRITICAL(&system_overlay_mux);
  ui_wake();
}

void ui_clear_system_status() {
  portENTER_CRITICAL(&system_overlay_mux);
  system_overlay.active = false;
  portEXIT_CRITICAL(&system_overlay_mux);
}

bool ui_take_mode_switch_request() {
  if (!btn_mode_request) return false;
  btn_mode_request = false;
  return true;
}

bool ui_take_factory_reset_request() {
  if (!btn_reset_request) return false;
  btn_reset_request = false;
  return true;
}

bool ui_command(const char *line) {
  if (!g_present) return false;
  int n = 0;

  if (strcmp(line, "next") == 0) {
    req_next = true;
    ui_wake();
    return true;
  }
  if (strcmp(line, "auto") == 0) {
    carousel_paused = false;
    req_next = true;
    ui_wake();
    Serial.println("[ui] carousel on");
    return true;
  }
  if (sscanf(line, "screen %d", &n) == 1) {
    if (n >= 0 && n < SCR_ROTATE_COUNT) {
      req_screen = (int8_t)n;
      ui_wake();
      Serial.printf("[ui] screen %d, carousel paused\n", n);
    } else {
      Serial.printf("[ui] screen must be 0..%d\n", SCR_ROTATE_COUNT - 1);
    }
    return true;
  }
  if (sscanf(line, "bright %d", &n) == 1) {
    bright_override = (uint8_t)(n < 0 ? 0 : (n > 255 ? 255 : n));
    ui_wake();
    Serial.printf("[ui] contrast %d%s\n", bright_override,
                  bright_override == 0 ? " (auto)" : "");
    return true;
  }
  if (strcmp(line, "ui") == 0) {
    Serial.printf("[ui] screen %d  %s  %.1f fps  style %d\n", cur_screen,
                  carousel_paused ? "paused" : "auto", fps_avg, spectrum_style);
    return true;
  }
  return false;
}
