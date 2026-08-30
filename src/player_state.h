/*
 * player_state.h -- the one shared picture of "what is going on", written by
 * the Bluetooth task and read by the UI task.
 *
 * The problem this solves: AVRCP metadata, volume changes and connection events
 * all arrive on the Bluetooth task, in line with the audio path. The UI task
 * wants a consistent snapshot of all of it, 30 times a second. A mutex would
 * work but would put the audio task at the mercy of the display, and the README
 * is emphatic about not blocking in there.
 *
 * So this is a seqlock. The writer bumps a counter to an odd value, writes, and
 * bumps it to even. The reader notes the counter, copies the struct, and checks
 * the counter again: if it moved, the copy might be a mix of two versions, so it
 * retries. The writer never waits for anything -- which is the whole point.
 *
 * Single writer only. Every ps_set_* below must be called from one task and one
 * task only. Which task that is depends on the radio mode, and the modes are
 * mutually exclusive so the two never overlap:
 *
 *   Bluetooth                the Bluetooth task, in line with the audio path.
 *   DFPlayer                 the Arduino loop task, via df_player_loop(). The
 *                            driver task owns the serial link and keeps its own
 *                            status behind a mutex; loop() copies it in here.
 */

#pragma once

#include <stdint.h>
#include <esp_a2dp_api.h>
#include <esp_avrc_api.h>

static const size_t PS_TITLE_MAX = 64;
static const size_t PS_TEXT_MAX = 40;
static const size_t PS_NAME_MAX = 32;

/// Where the audio being played is arriving from. The screens word themselves
/// differently for each: there is no "pair your phone" in DFPlayer mode, and no
/// "insert a card" in a Bluetooth one.
enum PsSource : uint8_t {
  PS_SRC_NONE = 0,   ///< no audio path is running at all (Wi-Fi only mode)
  PS_SRC_BLUETOOTH,  ///< A2DP sink
  PS_SRC_DFPLAYER,   ///< a DFPlayer Mini playing its own card or USB stick
};

/// What the phone is doing, as far as AVRCP has told us.
enum PsPlayback : uint8_t {
  PS_STOPPED = 0,
  PS_PLAYING,
  PS_PAUSED,
  PS_SEEKING,
};

struct PlayerInfo {
  // --- track ------------------------------------------------------------
  char title[PS_TITLE_MAX];
  char artist[PS_TEXT_MAX];
  char album[PS_TEXT_MAX];
  char genre[PS_TEXT_MAX];

  /// Track length in ms, from AVRCP's PLAYING_TIME attribute. 0 when the phone
  /// does not send it -- some players never do, so the UI must cope.
  uint32_t track_ms;

  /// Last reported playback position, and the millis() at which it arrived.
  /// Position notifications come once a second at best, so the UI interpolates
  /// between them; see ps_position_ms().
  uint32_t pos_ms;
  uint32_t pos_at;

  uint16_t track_num;
  uint16_t track_count;

  /// Bumped on every track change, so the UI can tell "same title reported
  /// again" from "actually a new track" and fire a toast.
  uint32_t track_seq;

  // --- link -------------------------------------------------------------
  char peer[PS_NAME_MAX];  ///< the phone's Bluetooth name, "" until it arrives
  esp_bd_addr_t peer_addr;
  /// Which audio path is live this boot. Fixed at startup by the radio mode;
  /// it never changes while running, because switching modes reboots.
  PsSource source;
  bool connected;
  /// The Bluetooth stack is actually running. False while the setup access
  /// point holds the antenna: nothing can pair, and the screens should say so
  /// rather than claiming to be discoverable.
  bool bt_active;
  bool avrc;      ///< remote control channel up (no AVRC = no metadata at all)
  bool streaming; ///< A2DP audio state is STARTED
  PsPlayback playback;
  uint16_t sample_rate;  ///< as reported by the sink, usually 44100
  uint32_t connected_at;

  // --- volume -----------------------------------------------------------
  uint8_t volume;       ///< 0..127, the phone's AVRCP absolute-volume slider
  uint32_t volume_seq;  ///< bumped on every change, drives the volume popup
};

void ps_init(const char *device_name);

/// Declares which audio path this boot is running. Call once from setup(),
/// before the source starts producing anything.
void ps_set_source(PsSource source);

/// The local device name, for the pairing screen. Never changes after init.
const char *ps_device_name();

/// Copies a consistent snapshot. Always succeeds (it retries on collision, and
/// a colliding writer only ever holds the sequence for a few microseconds).
void ps_snapshot(PlayerInfo *out);

/// Playback position right now, interpolated from the last notification while
/// the track is playing. Clamped to track_ms when the length is known.
uint32_t ps_position_ms(const PlayerInfo &s, uint32_t now_ms);

// --- writers, Bluetooth task only -----------------------------------------
void ps_set_connection(bool connected, const esp_bd_addr_t addr);

/// Whether the Bluetooth stack is up at all. Written from setup()/loop().
void ps_set_bt_active(bool active);

/// The local-source equivalent of ps_set_connection(): a source is feeding us
/// audio and here is what to call it. `who` may be nullptr to leave the name
/// alone. There is no address to record, which is the only reason this is
/// separate.
void ps_set_source_connection(bool connected, const char *who);

/// Sets the track text directly, for sources that hand over plain strings
/// rather than AVRCP attribute ids. Any argument may be nullptr to leave that
/// field untouched.

void ps_set_track_text(const char *title, const char *artist, const char *album);
void ps_set_peer_name(const char *name);
void ps_set_avrc(bool up);
void ps_set_streaming(bool on);
void ps_set_playback(PsPlayback state);
void ps_set_volume(uint8_t vol);
void ps_set_sample_rate(uint16_t rate);

/// Feeds one raw AVRCP metadata attribute (the id/text pair the library hands
/// to the metadata callback). Handles the sanitising and the track-change
/// bookkeeping, so the callback in main.cpp stays a one-liner.
void ps_set_metadata(uint8_t attr_id, const uint8_t *text);

/// Called when AVRCP says the track changed: clears the old metadata so a
/// player that only sends a title does not leave the previous artist on screen.
void ps_new_track();

void ps_set_position(uint32_t pos_ms);
