/*
 * net_audio.h -- the audio path for Wi-Fi + BLE mode.
 *
 * In the Bluetooth modes a phone pushes A2DP at the speaker and ESP32-A2DP
 * hands us decoded PCM. There is no such thing on Bluetooth Low Energy: LE
 * Audio needs Bluetooth 5.2 silicon and this is a 4.2 chip, and even the raw
 * bandwidth is an order of magnitude short of stereo music. So in this mode the
 * audio arrives over Wi-Fi instead, and BLE does what BLE is actually good at:
 * control, status and rescuing a speaker whose network credentials are wrong.
 *
 * Two ways in, both landing in the same player:
 *
 *   DLNA / UPnP   the speaker advertises itself as a MediaRenderer. Anything
 *                 that can "cast" to a network speaker -- BubbleUPnP, VLC,
 *                 Hi-Fi Cast, foobar2000, Windows' own "Cast to Device" --
 *                 finds it by SSDP and pushes a URL at it. No app to write.
 *   A URL         handed over by the dashboard or by BLE. Internet radio, or
 *                 any HTTP(S) audio file on the network.
 *
 * Either way the chain is the same: URLStream fetches, MultiDecoder picks a
 * decoder from the Content-Type, and the PCM goes to the same I2SStream the
 * A2DP sink uses, through a tap that feeds the spectrum analyser on the way.
 *
 * Threading. The player is not thread safe and commands arrive from four
 * different tasks (DLNA, web server, BLE, serial console), so nothing touches
 * it directly: every command goes through a queue and the audio task is the
 * only thing that ever calls into AudioPlayer. Status travels the other way
 * under a mutex, and net_audio_loop() -- on the Arduino loop task -- is the
 * single writer that publishes it into player_state.h. That keeps the seqlock's
 * one-writer rule intact.
 */

#pragma once

#include <Arduino.h>

/// Longest URL accepted. DLNA control points send long query strings; anything
/// past this is refused with an error rather than silently truncated into a
/// request that would fail confusingly later.
static const size_t NET_URL_MAX = 250;

enum NetAudioState : uint8_t {
  NET_AUDIO_IDLE = 0,   ///< nothing loaded
  NET_AUDIO_OPENING,    ///< connecting to the URL / waiting for the first bytes
  NET_AUDIO_PLAYING,
  NET_AUDIO_PAUSED,
  NET_AUDIO_ERROR,      ///< see `error`; the last attempt did not come up
};

struct NetAudioStatus {
  NetAudioState state;
  char url[NET_URL_MAX + 1];
  char title[64];
  char artist[40];
  char error[72];
  /// "dlna" when a control point pushed this, "url" when we were told directly,
  /// "" when nothing has played yet.
  char origin[8];
  uint8_t volume;       ///< 0..127, same scale the A2DP path uses
  uint32_t sample_rate; ///< as reported by the decoder, 0 until it starts
  uint8_t channels;
  bool renderer_up;     ///< the DLNA MediaRenderer is advertising
};

/// Brings up the decoder chain, the player task and the DLNA renderer.
/// `device_name` is what shows up in a phone's cast list. Returns false if the
/// tasks or the renderer could not start, in which case nothing is running and
/// every command below is a no-op.
bool net_audio_begin(const char *device_name, uint16_t http_port);

/// Publishes the current state into player_state.h. Arduino loop task only.
void net_audio_loop();

/// True once net_audio_begin() has succeeded.
bool net_audio_running();

/// A consistent copy of the status. Safe from any task, but it copies the whole
/// struct under a mutex -- fine per HTTP request, wasteful per loop.
void net_audio_snapshot(NetAudioStatus *out);

/// Just the state, read without locking. For the status LED and anything else
/// that runs every loop and only needs one byte: a read that races a transition
/// returns the old value for one frame, which no indicator can show anyway.
NetAudioState net_audio_state();

/// True while audio is actually being written to the DAC. main.cpp uses this to
/// keep the connect/disconnect melodies out of a live stream, exactly as it
/// does for A2DP.
bool net_audio_active();

// --- commands. All safe from any task; all return immediately. --------------

/// Starts playing a URL. `origin` is a short label for the dashboard ("url",
/// "dlna", "ble"). Returns false only if the URL is empty or too long -- a URL
/// that turns out to be unreachable fails later and lands in status.error.
bool net_audio_play_url(const char *url, const char *origin);
void net_audio_play();
void net_audio_pause();
void net_audio_stop();

/// 0..127, matching the A2DP volume scale so the OLED and the dashboard slider
/// do not need to know which mode they are in.
void net_audio_set_volume(uint8_t volume);
uint8_t net_audio_volume();
