/*
 * df_player.h -- the DFPlayer Mini (MP3-TF-16P / YX5200) as an audio source.
 *
 * Every other source in this firmware hands the ESP32 a PCM stream and the
 * ESP32 writes it to the PCM5102A. This one does not: the DFPlayer holds the
 * card, decodes the file and produces analog audio on its own DAC pins. So
 * there is no audio path here at all -- no I2S, no decoder, no analyser. What
 * this module does is *control*, over a 9600 baud serial link and six GPIOs,
 * and report what it learns so the dashboard and the OLED can show it.
 *
 * There is no directory listing in the protocol and there never will be: the
 * YX5200 answers in counts and indices and has no idea what a file is called.
 * What it will answer is "how many files are in folder N", one folder at a
 * time -- so the closest thing to a library is assembled here, by df_player_scan()
 * walking the folders once and caching what comes back. See the folder index
 * further down; the dashboard draws a browser out of those numbers.
 *
 * That is also why DFPlayer mode is a Wi-Fi mode rather than a third radio
 * arrangement. Nothing about it needs the antenna, so Wi-Fi gets all of it:
 * station when a network is saved, the setup access point when not, and the
 * dashboard either way. Bluetooth is left uninitialised and the whole
 * controller's RAM is handed back, exactly as in Wi-Fi only mode.
 *
 * Threading. The YX5200 is a request/response device on a slow link with a
 * minimum gap between frames, and commands arrive from two places: the web
 * server and the Arduino loop, the latter carrying the serial console. Nothing
 * touches the UART directly -- every command goes through a queue and one task
 * owns the port, which is also where the reply parser and the status poller
 * live. Status travels back under a mutex, and df_player_loop() -- on the
 * Arduino loop task -- is the single writer that publishes it into
 * player_state.h, keeping that seqlock's one-writer rule intact.
 *
 * What "full control" means here, concretely. Everything the module's serial
 * protocol exposes (transport, track and folder selection, volume, EQ, the four
 * loop modes, source selection, DAC mute, standby and reset) plus the pins that
 * are not the serial link: IO1/IO2 pressed as the module's own buttons,
 * ADKEY1/ADKEY2 triggered, BUSY read, and a status LED of its own. The one
 * thing no protocol command can do is enumerate filenames -- the YX5200 only
 * ever reports counts and indices -- so the dashboard works in track numbers
 * and folder numbers, which is what the module itself understands.
 */

#pragma once

#include <Arduino.h>

#include "hw_config.h"

/// Which library the module is playing from. The numbers are the protocol's own
/// (command 0x09), so they can be sent straight through.
enum DfSource : uint8_t {
  DF_SRC_USB = 1,    ///< a USB flash drive on the module's own host port
  DF_SRC_SD = 2,     ///< the microSD card
  DF_SRC_AUX = 3,    ///< the module's analog input; no transport control
  DF_SRC_SLEEP = 4,  ///< not a source: the protocol's way of powering down
  DF_SRC_FLASH = 5,  ///< the 16 Mbit SPI flash, only on some module variants
};

enum DfState : uint8_t {
  DF_STOPPED = 0,
  DF_PLAYING,
  DF_PAUSED,
  DF_SLEEPING,
};

/// The four repeat behaviours the module implements, plus off. These are not
/// one protocol field -- each is a different command -- so the driver remembers
/// which one it last asked for.
enum DfLoop : uint8_t {
  DF_LOOP_OFF = 0,
  DF_LOOP_TRACK,   ///< 0x19: repeat the current file
  DF_LOOP_FOLDER,  ///< 0x17: repeat one folder
  DF_LOOP_ALL,     ///< 0x11: repeat the whole card
  DF_LOOP_RANDOM,  ///< 0x18: shuffle everything
};

/// Folders are /01 to /99 in the protocol -- two zero-padded digits, and no
/// more of them than that whatever the card holds.
static const uint8_t DF_MAX_FOLDERS = 99;

/// The pins that are inputs on the module and are driven from here.
enum DfPin : uint8_t {
  DF_PIN_IO1 = 0,
  DF_PIN_IO2,
  DF_PIN_ADKEY1,
  DF_PIN_ADKEY2,
  DF_PIN_COUNT,
};

/// What the dedicated DFPlayer LED does.
enum DfLedMode : uint8_t {
  DF_LED_AUTO = 0,  ///< follows BUSY: lit while a file is playing
  DF_LED_OFF,
  DF_LED_ON,
  DF_LED_BLINK,     ///< 1 Hz, for finding the unit in a rack
};

struct DfStatus {
  bool running;   ///< the driver started: UART open, task alive
  bool online;    ///< the module has answered at least one frame recently
  bool busy;      ///< the BUSY pin says a file is playing (-1 pin: from state)
  /*
   * The module has been told to sleep.
   *
   * Published rather than kept inside the driver because "asleep" and "not
   * answering" look identical from outside and want opposite reactions. A
   * sleeping module is asked nothing -- it replies to every query with an error
   * -- so no frame arrives and the online timeout would otherwise expire,
   * turning a deliberate standby into a fault on the LED, on the OLED and in the
   * dashboard. Anything that reports on the module has to check this first.
   */
  bool asleep;
  DfState state;
  DfSource source;      ///< what we last selected
  DfSource reported;    ///< what the module says it is using, when it says

  // Which libraries the module can see. From its 0x3F init report and the
  // 0x3A/0x3B insert/remove notifications, so these track a card being pulled
  // out while running.
  bool sdPresent;
  bool usbPresent;
  bool flashPresent;
  /// A computer is plugged into the module's USB port, so the card is mounted
  /// as mass storage over there and is not ours to play from until it goes.
  bool pcLink;

  uint16_t track;         ///< current file index on the active source, 1-based
  uint16_t totalTracks;   ///< files on the active source, 0 until known
  uint16_t folders;       ///< folder count on the card, 0 until known
  uint16_t folder;        ///< folder last played from, 0 for none
  uint16_t folderTracks;  ///< files in `queriedFolder`
  uint16_t queriedFolder; ///< which folder folderTracks refers to

  uint8_t volume;   ///< module scale, 0..DF_VOLUME_MAX
  uint8_t eq;       ///< 0 normal, 1 pop, 2 rock, 3 jazz, 4 classic, 5 bass
  DfLoop loop;
  bool dacOn;       ///< the module's own output mute (command 0x1A)

  uint16_t version;    ///< module firmware version, if it answers 0x46
  uint32_t finished;   ///< tracks that have played to the end this boot
  uint32_t lastFrameAt;///< millis() of the last frame the module sent
  char error[72];      ///< last protocol error, "" when clear

  DfLedMode ledMode;
  bool ledOn;

  /*
   * Link health, for the `diag` console command and the dashboard.
   *
   * Four counters, all free: they are increments inside code paths that were
   * already running. They exist because "the DFPlayer is being unreliable" is
   * otherwise an unfalsifiable complaint -- these separate the three things it
   * can mean. A clone that answers everything with 0x40/0x01 shows up in
   * `errors`; a swapped or noisy TX/RX pair shows up as `framesBad` climbing
   * with `framesGood` flat; a module that has genuinely gone away shows up in
   * `offlineEvents`. A healthy link at the default poll rate adds about one to
   * `framesGood` per second and nothing at all to the other three.
   */
  uint32_t framesSent;     ///< protocol frames written to the module
  uint32_t framesGood;     ///< well-formed frames received from it
  uint32_t framesBad;      ///< ten-byte windows rejected by frameValid()
  uint32_t errors;         ///< 0x40 error notifications it sent us
  uint16_t offlineEvents;  ///< times the online timeout expired this boot
};

#if DFPLAYER_ENABLED

/// Opens the UART, claims the GPIOs and starts the driver task, then queues the
/// wake-up sequence (reset, source select, volume, EQ, library queries).
/// `volume` is on the module's 0..30 scale, `source` and `eq` are the stored
/// defaults. Returns false only if the task or its queue could not be created;
/// a module that is absent or miswired still starts the driver, and reports
/// itself offline through the status.
bool df_player_begin(DfSource source, uint8_t volume, uint8_t eq, DfLoop loop);

/// Publishes the current state into player_state.h. Arduino loop task only.
void df_player_loop();

/// True once df_player_begin() has succeeded.
bool df_player_running();

/// True when the module says it is actually playing something. main.cpp uses
/// this to keep the connect/disconnect melodies from talking over it, the same
/// way it does for A2DP and the network player.
bool df_player_active();

/// A consistent copy of everything above. Safe from any task; takes the mutex,
/// so it is a per-request call rather than a per-loop one.
///
/// Returns false when the copy is *not* a reading -- the driver is not running,
/// or the mutex could not be taken inside 100 ms. `*out` is zero-filled in that
/// case, which reads as "module offline with no files", so anything that would
/// act on the contents rather than merely display them has to check. Storing it
/// as the next boot's defaults is the case that made this a return value.
bool df_player_snapshot(DfStatus *out);

/// Just the state, read without locking, for the status LED.
DfState df_player_state();

/// Command queue observability; high-water is since this boot.
uint8_t df_player_queue_depth();
uint8_t df_player_queue_high_water();

// --- commands. All safe from any task; all return immediately. --------------
//
// Nothing here blocks on the module. A command is queued, the driver task puts
// it on the wire in order and at the protocol's pace, and the effect shows up in
// the status a poll later. A queue that is full (which needs ~24 commands in
// flight) drops the newest and returns false.

bool df_player_play();
bool df_player_pause();
bool df_player_toggle();
bool df_player_stop();
bool df_player_next();
bool df_player_previous();

/// Plays file `track` in the module's flat index over the whole card, 1-based.
bool df_player_play_track(uint16_t track);

/// Plays `file` inside `folder`, which is how a card with more than 3000 files
/// or any organisation at all is addressed. folder 1..99, file 1..255, both as
/// they appear in the zero-padded names the module requires ("/01/003.mp3").
bool df_player_play_folder(uint8_t folder, uint8_t file);

/// Plays `track` from the special "MP3" folder ("/MP3/0007.mp3"), which is the
/// module's own convention for a flat playlist that survives re-copying the
/// card -- the flat index above depends on FAT directory order and does not.
bool df_player_play_mp3(uint16_t track);

/// Interrupts whatever is playing with a file from the "ADVERT" folder and
/// resumes afterwards. The module's announcement channel.
bool df_player_advertise(uint16_t track);
bool df_player_advertise_stop();

/// 0..127, matching every other source so the OLED and the dashboard slider do
/// not need to know which mode they are in. Converted to the module's 0..30 on
/// the way out and back on the way in.
bool df_player_set_volume(uint8_t volume127);
uint8_t df_player_volume();
/// The module's own scale, for the dashboard's numeric field.
bool df_player_set_volume_raw(uint8_t volume);
bool df_player_volume_step(bool up);

bool df_player_set_eq(uint8_t eq);
bool df_player_set_source(DfSource source);
bool df_player_set_loop(DfLoop mode, uint8_t folder);
bool df_player_set_dac(bool on);

/// Module housekeeping. `reset` re-runs the whole start-up sequence, which is
/// the way out of a module that has stopped answering; `standby` powers its
/// decoder down (roughly 20 mA back, which matters on a battery) and `wake`
/// brings it round.
bool df_player_reset();
bool df_player_standby();
bool df_player_wake();

/// Re-runs the library queries: totals, folder count, volume, EQ, state. Cheap
/// and idempotent; the dashboard calls it after a card change.
bool df_player_refresh();

/// Asks how many files are in one folder, so the dashboard's file picker knows
/// its range. The answer lands in folderTracks/queriedFolder.
bool df_player_query_folder(uint8_t folder);

// ----------------------------------------------------------- folder index ---
/*
 * The nearest thing to a library the protocol allows.
 *
 * 0x4E answers "how many files in folder N" for one folder per round trip, and
 * the reply carries the count without saying which folder it was for. So the
 * scan walks the folders itself and attributes each answer to whichever folder
 * it last asked about -- safe, because one task owns the wire and the module
 * answers in order -- and caches the result. Ninety-nine round trips on a 9600
 * baud link takes several seconds: fine once, intolerable on every page load,
 * so it is asked for rather than done on its own.
 *
 * The cache is dropped whenever the library under it could have changed: a
 * source switch, a card going in or out, a reset.
 */

/// Starts a walk of every folder on the current source, or restarts one that is
/// already running. False if the driver is not up.
bool df_player_scan();

/// Progress, for the dashboard's bar: `done` and `total` are folder counts.
/// Returns true while a scan is running.
bool df_player_scanning(uint8_t *done, uint8_t *total);

/// True once a scan has finished and the cache has not been dropped since.
bool df_player_scanned();

/// Copies the cache out. `out[i]` is the file count for folder `i + 1`; 0 means
/// empty, absent, or not asked about yet. `count` is clamped to DF_MAX_FOLDERS.
void df_player_folder_counts(uint16_t *out, uint8_t count);

/// Presses one of the module's own button inputs by pulling it to ground.
/// `long_press` picks the module's second meaning for IO1/IO2 (volume down/up
/// instead of previous/next); it is ignored for the ADKEY pins, which have only
/// one. Returns false if that pin is not wired (compiled as -1).
bool df_player_pulse(DfPin pin, bool long_press);

/// True if that pin is wired on this board.
bool df_player_pin_available(DfPin pin);

/// The dedicated DFPlayer LED. No-op when PIN_DF_LED is -1.
void df_player_set_led(DfLedMode mode);

/// Human-readable names, shared by the console, the dashboard and the OLED.
const char *df_source_name(DfSource source);
const char *df_state_name(DfState state);
const char *df_loop_name(DfLoop loop);
const char *df_eq_name(uint8_t eq);

/// Serial console: "df", "df play [n]", "df pause", "df stop", "df next",
/// "df prev", "df vol 0-30", "df folder F T", "df source sd|usb|flash",
/// "df eq 0-5", "df loop off|track|folder|all|random", "df reset", "df io1",
/// "df io2", "df key1", "df key2", "df led auto|on|off|blink".
/// Returns false if the line was not one of those.
bool df_player_command(const char *line);

#else

inline bool df_player_begin(DfSource, uint8_t, uint8_t, DfLoop) { return false; }
inline void df_player_loop() {}
inline bool df_player_running() { return false; }
inline bool df_player_active() { return false; }
inline bool df_player_snapshot(DfStatus *out) {
  if (out) *out = DfStatus{};
  return false;
}
inline DfState df_player_state() { return DF_STOPPED; }
inline bool df_player_play() { return false; }
inline bool df_player_pause() { return false; }
inline bool df_player_toggle() { return false; }
inline bool df_player_stop() { return false; }
inline bool df_player_next() { return false; }
inline bool df_player_previous() { return false; }
inline bool df_player_play_track(uint16_t) { return false; }
inline bool df_player_play_folder(uint8_t, uint8_t) { return false; }
inline bool df_player_play_mp3(uint16_t) { return false; }
inline bool df_player_advertise(uint16_t) { return false; }
inline bool df_player_advertise_stop() { return false; }
inline bool df_player_set_volume(uint8_t) { return false; }
inline uint8_t df_player_volume() { return 0; }
inline bool df_player_set_volume_raw(uint8_t) { return false; }
inline bool df_player_volume_step(bool) { return false; }
inline bool df_player_set_eq(uint8_t) { return false; }
inline bool df_player_set_source(DfSource) { return false; }
inline bool df_player_set_loop(DfLoop, uint8_t) { return false; }
inline bool df_player_set_dac(bool) { return false; }
inline bool df_player_reset() { return false; }
inline bool df_player_standby() { return false; }
inline bool df_player_wake() { return false; }
inline bool df_player_refresh() { return false; }
inline bool df_player_query_folder(uint8_t) { return false; }
inline bool df_player_scan() { return false; }
inline bool df_player_scanning(uint8_t *, uint8_t *) { return false; }
inline bool df_player_scanned() { return false; }
inline void df_player_folder_counts(uint16_t *, uint8_t) {}
inline bool df_player_pulse(DfPin, bool) { return false; }
inline bool df_player_pin_available(DfPin) { return false; }
inline void df_player_set_led(DfLedMode) {}
inline const char *df_source_name(DfSource) { return "none"; }
inline const char *df_state_name(DfState) { return "stopped"; }
inline const char *df_loop_name(DfLoop) { return "off"; }
inline const char *df_eq_name(uint8_t) { return "normal"; }
inline bool df_player_command(const char *) { return false; }

#endif
