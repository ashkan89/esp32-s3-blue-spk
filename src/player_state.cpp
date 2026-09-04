#include "player_state.h"

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include "ui_config.h"
#include "stability_policy.h"

static PlayerInfo g_info;
static uint32_t g_seq;  // even = stable, odd = write in progress
static char g_device_name[PS_NAME_MAX];
// A seqlock needs one writer at a time. AVRCP/Bluetooth callbacks and loop()
// both update this model, so serialize writers before changing the sequence.
static portMUX_TYPE g_write_mux = portMUX_INITIALIZER_UNLOCKED;

// A write is bracketed by two increments with full barriers around the payload,
// so a reader that sees the same even sequence twice knows nothing moved in
// between. See the header for why this is not a mutex.
static inline void begin_write() {
  portENTER_CRITICAL(&g_write_mux);
  __atomic_add_fetch(&g_seq, 1U, __ATOMIC_RELAXED);
  __sync_synchronize();
}

static inline void end_write() {
  __sync_synchronize();
  __atomic_add_fetch(&g_seq, 1U, __ATOMIC_RELEASE);
  portEXIT_CRITICAL(&g_write_mux);
}

/*
 * Copies AVRCP text into a fixed field, made safe for the display.
 *
 * Three things get cleaned up:
 *   - control characters and tabs/newlines become spaces, so a player that
 *     sends a multi-line comment cannot break the layout;
 *   - runs of spaces collapse and the result is trimmed, because padded
 *     fixed-width tags are common and they look like a rendering bug;
 *   - anything that is not valid UTF-8 is taken as a Latin-1 byte and
 *     re-encoded, because plenty of players send tags that way.
 *
 * What it deliberately does NOT do any more is flatten everything above
 * U+00FF to a question mark. That was here because the "_tf" fonts carry
 * Latin-1 and nothing else, so anything else drew as an invisible gap -- but
 * it meant a Persian or Arabic title arrived at the display as a row of
 * question marks, and no font could have saved it. Codepoints now survive
 * this function intact; picking a font that can draw them is ui.cpp's job,
 * and text_arabic.cpp shapes them on the way to the panel.
 */
static void copy_sanitised(char *dst, size_t cap, const char *src) {
  if (src == nullptr) {
    dst[0] = 0;
    return;
  }

  size_t o = 0;
  bool pending_space = false;

  for (const uint8_t *p = (const uint8_t *)src; *p && o + 1 < cap;) {
    uint32_t cp;
    uint8_t c = *p;
    const uint8_t encoded =
        stability_utf8_sequence_length((const char *)p);

    if (c < 0x80) {
      cp = c;
      p += 1;
    } else if (encoded == 2) {
      cp = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
      p += 2;
    } else if (encoded == 3) {
      cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) |
           (uint32_t)(p[2] & 0x3F);
      p += 3;
    } else if (encoded == 4) {
      /*
       * All three continuation bytes are checked, and that is not pedantry.
       * The lead byte alone used to be enough to advance by four, so a string
       * ending in a truncated sequence -- "\xF0" then the terminator, which is
       * exactly what a title cut to a fixed field length produces -- stepped
       * the cursor past the NUL and the loop went on reading whatever followed
       * the buffer until it happened to find a zero. The write side was always
       * bounded, so nothing overflowed; the read side was not. A NUL fails the
       * 0xC0 test, so the checks terminate the sequence safely and the byte is
       * handled by the Latin-1 fallback below.
       */
      // Astral plane: emoji and the like. Decoded so the loop advances
      // correctly, then dropped -- no font on this device has these, and a
      // 16x16 emoji would not fit a 32 px panel anyway.
      cp = 0xFFFD;
      p += 4;
    } else {
      // Not valid UTF-8 at all. Plenty of players send Latin-1 bytes raw, so
      // take the byte at face value rather than dropping the rest of the tag.
      cp = c;
      p += 1;
    }

    if (cp == ' ' || cp < 0x20 || cp == 0x7F) {
      pending_space = (o > 0);  // never leading, never doubled
      continue;
    }
    if (pending_space) {
      dst[o++] = ' ';
      pending_space = false;
      if (o + 1 >= cap) break;
    }

    // Re-encoded as UTF-8 rather than stored as a codepoint, because
    // drawUTF8() is what renders this and the width helpers measure it.
    if (cp < 0x80) {
      dst[o++] = (char)cp;
    } else if (cp < 0x800) {
      if (o + 2 >= cap) break;
      dst[o++] = (char)(0xC0 | (cp >> 6));
      dst[o++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp != 0xFFFD) {
      // Three bytes. Arabic and Persian are two, but the presentation
      // forms the shaper emits are three, so a title that already
      // arrived shaped has to survive this too.
      if (o + 3 >= cap) break;
      dst[o++] = (char)(0xE0 | (cp >> 12));
      dst[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      dst[o++] = (char)(0x80 | (cp & 0x3F));
    }
  }

  dst[o] = 0;
}

void ps_init(const char *device_name) {
  memset(&g_info, 0, sizeof(g_info));
  g_info.volume = 127;
  g_info.sample_rate = 44100;
  g_info.playback = PS_STOPPED;
  copy_sanitised(g_device_name, sizeof(g_device_name), device_name);
}

const char *ps_device_name() { return g_device_name; }

void ps_set_source(PsSource source) {
  begin_write();
  g_info.source = source;
  end_write();
}

void ps_snapshot(PlayerInfo *out) {
  if (!out) return;
  // A write holds the odd sequence for only a few microseconds. Yield after a
  // burst of collisions, but never return a knowingly torn snapshot.
  uint8_t collisions = 0;
  for (;;) {
    const uint32_t s1 = __atomic_load_n(&g_seq, __ATOMIC_ACQUIRE);
    if (!(s1 & 1)) {
      memcpy(out, &g_info, sizeof(PlayerInfo));
      __sync_synchronize();
      if (__atomic_load_n(&g_seq, __ATOMIC_RELAXED) == s1) return;
    }
    if (++collisions == 8) {
      taskYIELD();
      collisions = 0;
    }
  }
}

uint32_t ps_position_ms(const PlayerInfo &s, uint32_t now_ms) {
  uint32_t pos = s.pos_ms;
  if (s.playback == PS_PLAYING) pos += now_ms - s.pos_at;
  if (s.track_ms > 0 && pos > s.track_ms) pos = s.track_ms;
  return pos;
}

void ps_set_connection(bool connected, const esp_bd_addr_t addr) {
  begin_write();
  g_info.connected = connected;
  if (connected) {
    if (addr != nullptr) memcpy(g_info.peer_addr, addr, sizeof(esp_bd_addr_t));
    g_info.connected_at = millis();
  } else {
    // Everything we knew came from the phone that just left.
    g_info.title[0] = g_info.artist[0] = g_info.album[0] = 0;
    g_info.genre[0] = g_info.peer[0] = 0;
    g_info.track_ms = g_info.pos_ms = 0;
    g_info.track_num = g_info.track_count = 0;
    g_info.playback = PS_STOPPED;
    g_info.streaming = false;
    g_info.avrc = false;
  }
  end_write();
}

void ps_set_peer_name(const char *name) {
  begin_write();
  copy_sanitised(g_info.peer, sizeof(g_info.peer), name);
  end_write();
}

void ps_set_avrc(bool up) {
  begin_write();
  g_info.avrc = up;
  end_write();
}

void ps_set_bt_active(bool active) {
  begin_write();
  g_info.bt_active = active;
  end_write();
}

// The local-source twin of ps_set_connection(). Same clearing rules on the way
// down: everything on the screen came from whoever just went away, and leaving
// a dead track title up is worse than showing nothing.
void ps_set_source_connection(bool connected, const char *who) {

  begin_write();
  g_info.connected = connected;
  if (connected) {
    if (who != nullptr) copy_sanitised(g_info.peer, sizeof(g_info.peer), who);
    g_info.connected_at = millis();
  } else {
    g_info.title[0] = g_info.artist[0] = g_info.album[0] = 0;
    g_info.genre[0] = g_info.peer[0] = 0;
    g_info.track_ms = g_info.pos_ms = 0;
    g_info.track_num = g_info.track_count = 0;
    g_info.playback = PS_STOPPED;
    g_info.streaming = false;
  }
  end_write();
}

// ICY and DIDL hand over finished strings rather than AVRCP attribute ids, so
// this is ps_set_metadata() without the id dispatch. The track counter is only
// bumped when the title actually changes: an ICY stream repeats the same title
// every few seconds, and a toast per repeat would be unbearable.
void ps_set_track_text(const char *title, const char *artist,
                       const char *album) {
  begin_write();
  if (title != nullptr) {
    char clean[PS_TITLE_MAX];
    copy_sanitised(clean, sizeof(clean), title);
    if (strcmp(clean, g_info.title) != 0) {
      memcpy(g_info.title, clean, sizeof(clean));
      g_info.track_seq++;
      // A new title is a new track: the position restarts whether or not the
      // source bothers to tell us.
      g_info.pos_ms = 0;
      g_info.pos_at = millis();
    }
  }
  if (artist != nullptr) {
    copy_sanitised(g_info.artist, sizeof(g_info.artist), artist);
  }
  if (album != nullptr) {
    copy_sanitised(g_info.album, sizeof(g_info.album), album);
  }
  end_write();
}

void ps_set_streaming(bool on) {
  begin_write();
  g_info.streaming = on;
  // A2DP suspending is the only "not playing" signal some players ever give us.
  if (!on && g_info.playback == PS_PLAYING) {
    g_info.pos_ms += millis() - g_info.pos_at;
    g_info.pos_at = millis();
    g_info.playback = PS_PAUSED;
  }
  end_write();
}

void ps_set_playback(PsPlayback state) {
  begin_write();
  if (state == PS_PLAYING && g_info.playback != PS_PLAYING) {
    // Resume: restart the interpolation clock from the last known position, or
    // the progress bar jumps forward by however long we were paused.
    g_info.pos_at = millis();
  } else if (g_info.playback == PS_PLAYING && state != PS_PLAYING) {
    g_info.pos_ms += millis() - g_info.pos_at;
    g_info.pos_at = millis();
  }
  g_info.playback = state;
  end_write();
}

void ps_set_volume(uint8_t vol) {
  if (vol == g_info.volume) return;
  begin_write();
  g_info.volume = vol;
  g_info.volume_seq++;
  end_write();
}

void ps_set_sample_rate(uint16_t rate) {
  begin_write();
  g_info.sample_rate = rate;
  end_write();
}

void ps_set_position(uint32_t pos_ms) {
  begin_write();
  g_info.pos_ms = pos_ms;
  g_info.pos_at = millis();
  end_write();
}

void ps_new_track() {
  begin_write();
  // Clear rather than keep: a player that sends only a title for the next track
  // would otherwise leave the previous artist and album sitting underneath it.
  g_info.artist[0] = g_info.album[0] = g_info.genre[0] = 0;
  g_info.track_ms = 0;
  g_info.pos_ms = 0;
  g_info.pos_at = millis();
  end_write();
}

void ps_set_metadata(uint8_t attr_id, const uint8_t *text) {
  if (!text) return;
  const char *s = (const char *)text;

  switch (attr_id) {
    case ESP_AVRC_MD_ATTR_TITLE: {
      char clean[PS_TITLE_MAX];
      copy_sanitised(clean, sizeof(clean), s);
      const bool changed = strcmp(clean, g_info.title) != 0;
      begin_write();
      memcpy(g_info.title, clean, sizeof(clean));
      // The title is the one field every player sends, so it is the most
      // reliable "this is a different song now" signal available.
      if (changed) g_info.track_seq++;
      end_write();
      break;
    }
    case ESP_AVRC_MD_ATTR_ARTIST:
      begin_write();
      copy_sanitised(g_info.artist, sizeof(g_info.artist), s);
      end_write();
      break;
    case ESP_AVRC_MD_ATTR_ALBUM:
      begin_write();
      copy_sanitised(g_info.album, sizeof(g_info.album), s);
      end_write();
      break;
    case ESP_AVRC_MD_ATTR_GENRE:
      begin_write();
      copy_sanitised(g_info.genre, sizeof(g_info.genre), s);
      end_write();
      break;
    case ESP_AVRC_MD_ATTR_TRACK_NUM:
      begin_write();
      g_info.track_num = (uint16_t)atoi(s);
      end_write();
      break;
    case ESP_AVRC_MD_ATTR_NUM_TRACKS:
      begin_write();
      g_info.track_count = (uint16_t)atoi(s);
      end_write();
      break;
    case ESP_AVRC_MD_ATTR_PLAYING_TIME:
      // AVRCP sends the track length as milliseconds in an ASCII string.
      begin_write();
      g_info.track_ms = (uint32_t)strtoul(s, nullptr, 10);
      end_write();
      break;
    default:
      break;
  }
}
