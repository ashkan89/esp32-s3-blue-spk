/*
 * net_radio.h -- internet radio: the audio source Wi-Fi mode never had.
 *
 * Until now the three radio modes offered two audio paths between them --
 * A2DP in Bluetooth mode, the DFPlayer in DFPlayer mode -- and Wi-Fi mode had
 * none at all. It brought up a dashboard and a clock and then sat there with an
 * idle DAC. This is what fills that gap, and it needs no hardware that is not
 * already fitted: the I2S channel is opened in setup() before the mode is even
 * consulted, so in Wi-Fi mode it is simply free.
 *
 * Which modes. Both of the ones that have Wi-Fi. In Wi-Fi mode the radio is the
 * only thing making sound and it has the DAC to itself. In DFPlayer mode the
 * module's analog output and the PCM5102A meet at the same jack through the
 * passive summing network hw_config.h describes, so both playing at once would
 * be two songs at the same time -- net_radio_play() pauses the module first,
 * and that is the whole of the arbitration. Bluetooth mode has no Wi-Fi at all,
 * one antenna being one antenna, so there is nothing to arbitrate and the
 * dashboard says so rather than offering a button that cannot work.
 *
 * How it runs. On its own FreeRTOS task, pinned to core 1, and not in loop().
 * That is not a preference. loop() serves HTTP, plays melodies and talks to the
 * DFPlayer, and any one of those blocks for tens of milliseconds at a time; a
 * decoder that misses its deadline by that much empties the I2S DMA ring and
 * the result is a click. On its own task the decode is paced by the I2S write
 * itself -- write() blocks until the DMA has room, which is exactly the clock
 * the stream should be running on -- and the web server can take as long as it
 * likes.
 *
 * The buffer, and why there is one. A radio stream arrives in whatever lumps
 * the internet felt like sending, and a decoder consumes it at a constant rate.
 * Between them sits a ring buffer of 20 kB -- two and a half seconds of a
 * 64 kbps stream, a little over one of a 128 kbps one -- which is filled ahead
 * before playback starts and topped up between decodes. What the dashboard
 * shows as a buffering indicator is how full it is, and an underrun is counted
 * rather than hidden: a station that will not stay above the floor is a station
 * that is too far away or too fast for this link, and that is worth being able
 * to see.
 *
 * Nothing is allocated until a station is played. The buffer, the socket chunk
 * and the decoder feed are one 24 kB block claimed when a stream starts and
 * freed when it ends, and the decoder task's 10 kB stack is created on the
 * first station and then kept. That is not tidiness: held from boot, they cost
 * the firmware updater its TLS handshake, which wants 80 kB free and a 45 kB
 * contiguous block -- and a 24 kB allocation made early sits in the middle of
 * the heap splitting exactly the block the handshake needs. A speaker that
 * never plays a station now pays for the station list and nothing else.
 *
 * Reconnection. Streams drop, and they drop most often for reasons that fix
 * themselves. A failure retries on a backoff that widens to a minute, forever,
 * because the alternative -- giving up after five tries -- means a speaker that
 * is silent when the network comes back an hour later. The station and the
 * intent to be playing are both persisted, so a power cut resumes the stream
 * rather than the silence.
 *
 * Codecs. MP3 and AAC, both through libhelix, which is what every ESP32 radio
 * uses because it is the only pair of decoders small enough. Between them they
 * cover essentially all of Shoutcast and Icecast. Ogg/Vorbis and Opus are not
 * supported: neither has a decoder that fits in the RAM this chip has left over
 * once Wi-Fi has taken its share.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/// How many favourites the dashboard may store. Twelve because the NVS blob
/// that holds them has to stay under a page and because a list longer than this
/// wants a scrollbar, which the OLED cannot offer.
static const uint8_t RADIO_MAX_STATIONS = 12;

static const size_t RADIO_NAME_MAX = 40;
static const size_t RADIO_URL_MAX = 160;

/// Text fields the stream itself supplies. Sized for what a 128x32 OLED and a
/// dashboard card can actually show, not for what a station might send.
static const size_t RADIO_TEXT_MAX = 64;

struct RadioStation {
  char name[RADIO_NAME_MAX];
  char url[RADIO_URL_MAX];
};

/*
 * What the player is doing.
 *
 * BUFFERING and PLAYING are deliberately separate states rather than one
 * "playing" with a level attached. The difference matters to the person
 * listening: buffering after a press is the speaker working, and buffering
 * thirty seconds in is the speaker struggling, and a UI that cannot tell them
 * apart cannot say which.
 */
enum RadioState : uint8_t {
  RADIO_IDLE = 0,      ///< nothing asked for
  RADIO_CONNECTING,    ///< opening the socket and reading the reply headers
  RADIO_BUFFERING,     ///< connected, filling the ring before audio starts
  RADIO_PLAYING,       ///< decoding, and the buffer is holding
  RADIO_RECONNECTING,  ///< the stream dropped; waiting out the backoff
  RADIO_ERROR,         ///< stopped, with a reason in `error`
};

struct RadioStatus {
  RadioState state;
  int8_t station;  ///< index of the favourite playing, or -1 for an ad-hoc URL

  char url[RADIO_URL_MAX];
  /// What the station calls itself: the icy-name header if it sent one, the
  /// stored favourite's name if not, and the host from the URL as a last
  /// resort. Never empty while anything is playing.
  char name[RADIO_NAME_MAX];
  char title[RADIO_TEXT_MAX];   ///< the in-band StreamTitle, i.e. the track
  char genre[RADIO_TEXT_MAX];
  char error[RADIO_TEXT_MAX];

  uint32_t bitrate;      ///< kbps, from icy-br, or 0 if the station did not say
  uint32_t sampleRate;   ///< as the decoder reports it
  uint8_t channels;
  char codec[8];         ///< "MP3" or "AAC"

  uint8_t bufferPercent; ///< how full the jitter buffer is, 0..100
  uint32_t underruns;    ///< times the buffer emptied since this stream started
  uint32_t reconnects;   ///< times it has had to reconnect since this stream started
  uint32_t playingSince; ///< millis() when the current connection reached PLAYING
  /// millis() when `state` last changed. Separate from playingSince because the
  /// display needs to know how long a *failure* has been on screen, and a
  /// stream that never played has no playingSince to measure that from.
  uint32_t stateSince;
  uint32_t bytes;        ///< received on this connection, for a rough rate readout
};

/*
 * Starts the driver.
 *
 * `out` is main.cpp's I2S stream, handed over rather than opened here because
 * it is opened once in setup() before the radio mode is known and the melodies
 * and the A2DP sink write to the same channel. Passed as void* so this header
 * does not drag AudioTools into every file that wants to ask whether the radio
 * is playing.
 *
 * Call only in a mode with Wi-Fi. Returns false if the task or its buffer could
 * not be created, which on this chip means the heap is gone -- the caller
 * should carry on without radio rather than treat it as fatal.
 */
bool net_radio_begin(void *out);

/// True once net_radio_begin() has succeeded.
bool net_radio_running();

/// Publishes the current state into player_state.h, for the OLED. Arduino loop
/// task only, once per pass -- the same contract df_player_loop() has.
void net_radio_loop();

/// True while audio is actually reaching the DAC, so the melodies and the
/// announcements know the channel is taken. This is what main.cpp's dac_busy()
/// consults for the network path.
bool net_radio_active();

/// A consistent copy of everything above. Safe from any task.
void net_radio_snapshot(RadioStatus *out);

// ----------------------------------------------------------------- control --

/// Plays a stored favourite. False if there is no station at that index.
bool net_radio_play_station(uint8_t index);

/// Plays a URL that is not in the list -- the dashboard's "try this address"
/// box. The station index in the status becomes -1.
bool net_radio_play_url(const char *url, const char *name);

/// Stops, and remembers that stopping is what was wanted, so the reconnect
/// logic does not immediately undo it.
void net_radio_stop();

/// Stop if playing, start the last station if not. What the OLED button and the
/// dashboard's play/pause control do. A stream has no pause -- the audio that
/// arrives while it is paused is simply gone -- so this really is stop/start,
/// and calling it what it is beats a pause that silently loses ten seconds.
bool net_radio_toggle();

/// Moves to the next or previous favourite, wrapping. False when fewer than two
/// stations are stored.
bool net_radio_step_station(bool forward);

/// 0..127, on the same scale as every other source so the dashboard slider and
/// the OLED do not have to know which mode they are in. Applied in the output
/// path, after the equaliser.
void net_radio_set_volume(uint8_t volume127);
uint8_t net_radio_volume();

/// Whether the first favourite should start automatically after boot. A failed
/// boot-time attempt disarms it so a bad station cannot create a boot loop.
bool net_radio_autostart();
void net_radio_set_autostart(bool on);

// ---------------------------------------------------------------- stations --

uint8_t net_radio_station_count();

/// Copies one favourite out. False if `index` is past the end.
bool net_radio_station(uint8_t index, RadioStation *out);

/*
 * Stores a favourite.
 *
 * `index` at or past the current count appends, up to RADIO_MAX_STATIONS; an
 * existing index overwrites. The URL is checked for a scheme this player can
 * actually open, so a mistyped address is refused at the point somebody could
 * still fix it rather than at the point it fails to play.
 */
bool net_radio_set_station(uint8_t index, const char *name, const char *url);

/// Removes one, closing the gap. Stops playback first if it was the one
/// playing, because the alternative is a station that plays but is not in
/// the list and cannot be got back to.
bool net_radio_remove_station(uint8_t index);

/// Moves a favourite up or down the list, which is the only ordering control
/// the dashboard needs given how short the list is.
bool net_radio_move_station(uint8_t index, bool up);

/// Writes the list to NVS. Called by the API layer after a batch of edits, so
/// reordering five stations is one flash write rather than five.
void net_radio_store_stations();

/// One word for a RadioState, for the dashboard, the OLED and the console.
const char *net_radio_state_name(RadioState state);

/*
 * The two questions the OLED asks, answered here rather than by handing it a
 * whole RadioStatus.
 *
 * The display's carousel needs "is there anything to show" and "one line of
 * text", and both are decisions about *presentation of the radio* -- which is
 * this module's business, not the renderer's. Keeping them here also means
 * ui.cpp does not take a mutex thirty times a second for a status struct it
 * would use two fields of.
 */

/// Whether the radio screen has anything worth a slot in the carousel: playing,
/// buffering, connecting, or recently failed with something to say.
bool net_radio_screen_wanted();

/// One line describing what the player is doing, for the waiting screen. Always
/// a valid string; points at a static buffer that is rewritten on each call.
const char *net_radio_screen_line();

/// Serial console: "radio stations", "radio play <n>", "radio stop",
/// "radio next|prev". Returns false if the line was something else.
bool net_radio_command(const char *line);
