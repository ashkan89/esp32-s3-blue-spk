/*
 * Bluetooth speaker: ESP32 WROOM-32D + PCM5102A I2S DAC + 0.91" OLED
 *
 * The ESP32 advertises itself as an A2DP sink. Pair a phone with it, hit play,
 * and the decoded stereo PCM is pushed out over I2S to the PCM5102A, which
 * drives the headphone jack.
 *
 * Built on https://github.com/pschatzmann/ESP32-A2DP, using the AudioTools
 * I2SStream output that the library documents.
 *
 * Three things here beyond a plain sink:
 *   - short tone melodies on connect / disconnect, played straight into the
 *     same I2S stream (see "melodies" below);
 *   - a linear, soft-clipped volume path, because the library's stock
 *     exponential curve throws away most of the level (see "loudness" below);
 *   - a 128x32 OLED showing the track, the phone, the volume, a clock and a
 *     live spectrum analyser (see ui.h -- all of it in its own task, off the
 *     audio path).
 *
 * Wiring (PCM5102A breakout -> ESP32):
 *   VIN  -> 5V (VIN/USB pin; the module regulates down to 3.3V)
 *   GND  -> GND
 *   BCK  -> GPIO26
 *   DIN  -> GPIO22
 *   LCK  -> GPIO25
 *   SCK  -> GND      (mandatory: tells the PCM5102A to use its internal PLL)
 *   FMT  -> GND      (standard I2S framing)
 *   XMT  -> 3.3V     (un-mute; most breakouts already pull this high)
 *   FLT / DEMP -> GND
 *
 * Wiring (0.91" SSD1306 OLED -> ESP32):
 *   VCC  -> 3.3V     (these modules have no regulator; 5V kills them)
 *   GND  -> GND
 *   SDA  -> GPIO21
 *   SCL  -> GPIO19   (not GPIO22 -- that pin is already I2S DIN above)
 *
 * The display is optional: if nothing answers on the I2C bus the UI switches
 * itself off at boot and everything else behaves as it did before.
 *
 */

#include "AudioTools.h"
#include "app_config.h"
#include "audio_probe.h"
#include "management.h"
#include "player_state.h"
#include "soft_clock.h"
#include "ui.h"
#include "ui_config.h"

// ---------------------------------------------------------------- config ----
static const char *DEVICE_NAME = APP_NAME;

static const int PIN_I2S_BCLK = 26;
static const int PIN_I2S_LRCK = 25;
static const int PIN_I2S_DOUT = 22;

static const int PIN_STATUS_LED = 2;  // on-board LED of most WROOM-32D devkits

static const int SAMPLE_RATE = 44100;  // A2DP/SBC is 44.1 kHz in practice

// Starting point for the software volume, 0..127. The phone overrides it over
// AVRCP as soon as it connects. Kept at maximum: with the linear curve below,
// 127 means "pass the stream through untouched", which leaves all attenuation
// to the phone's own slider where you can actually see what you are doing.
static const uint8_t START_VOLUME = 127;

// Extra gain on top of the volume setting, for when the phone is already at
// maximum and it is still not loud enough. A stream at full scale has no
// digital headroom left, so anything above 1.0 is bought with soft clipping:
//   1.0  bit-perfect, no shaping at all
//   1.5  ~+3.5 dB, peaks rounded into the ceiling (default)
//   2.0  ~+6 dB, audibly squashed on loud material
// If you hear distortion only on loud passages, this is the knob to lower.
static const float OUTPUT_GAIN = 1.5f;

// Peak amplitude of the melodies, 0..32767. A sine at this level is about as
// loud as music peaking at the same number, so a third of full scale is already
// prominent without being startling in headphones.
static const int16_t MELODY_AMPLITUDE = 11000;

I2SStream i2s;

/*
 * Shared I2S setup.
 *
 * On IDF 5 AudioTools does NOT pass buffer_count/buffer_size to the driver
 * directly -- it multiplies them and divides by the frame size to derive
 * `dma_frame_num`, while `dma_desc_num` stays at the IDF default of 6:
 *
 *     dma_frame_num = (buffer_size * buffer_count) / (bits/8 * channels)
 *
 * IDF caps one DMA descriptor at 4092 bytes, so buffer_size * buffer_count
 * must stay <= 4092 or i2s_new_channel() rejects the config and you get no
 * sound at all. The defaults below (6 x 512 = 3072 B -> 768 frames/descriptor,
 * 6 descriptors, ~104 ms total) already carry plenty of slack for A2DP jitter;
 * they are spelled out here so the ceiling is visible if you want to tune.
 *
 * use_apll derives the bit clock from the audio PLL so 44.1 kHz is exact
 * rather than a fractional approximation. Set false if an unusual sample rate
 * produces silence.
 */
static I2SConfig make_i2s_config() {
  auto cfg = i2s.defaultConfig(TX_MODE);
  cfg.pin_bck = PIN_I2S_BCLK;
  cfg.pin_ws = PIN_I2S_LRCK;
  cfg.pin_data = PIN_I2S_DOUT;
  cfg.pin_mck = -1;  // PCM5102A SCK is tied to GND instead
  cfg.sample_rate = SAMPLE_RATE;
  cfg.channels = 2;
  cfg.bits_per_sample = 16;
  cfg.i2s_format = I2S_STD_FORMAT;
  cfg.buffer_count = 6;    // keep buffer_count * buffer_size <= 4092
  cfg.buffer_size = 512;
  cfg.use_apll = true;
  cfg.auto_clear = true;   // emit silence on underrun, never replay a stale buffer
  return cfg;
}

// --------------------------------------------------------------- console ----
/*
 * A one-line serial console, polled from loop().
 *
 * It exists mainly so the clock can be set without a network or an RTC, but it
 * is also the easiest way to poke at the display while it is running. Reading
 * from Serial in loop() is safe; printing from a Bluetooth callback is not, and
 * that rule is what the rest of this file is organised around.
 */
static void print_help() {
  Serial.println(F(
      "commands:\n"
      "  time                      show the clock and where it came from\n"
      "  time 14:30[:00]           set the time (today)\n"
      "  time 2026-08-18 14:30     set date and time\n"
      "  date 2026-08-18           set the date\n"
      "  next                      next display screen\n"
      "  screen 0..6               hold one screen (0 now playing, 1 spectrum,\n"
      "                            2 VU, 3 scope, 4 waterfall, 5 clock, 6 info)\n"
      "  auto                      resume the screen carousel\n"
      "  bright 0..255             fix the contrast (0 = automatic)\n"
      "  ui                        display status\n"
      "  help"));
}

static void poll_console() {
  static char buf[64];
  static uint8_t len;

  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (len + 1 < sizeof(buf)) buf[len++] = c;
      continue;
    }
    buf[len] = 0;
    len = 0;
    if (buf[0] == 0) continue;

    ui_wake();
    if (soft_clock_command(buf)) continue;
    if (ui_command(buf)) continue;
    if (strcmp(buf, "help") == 0 || strcmp(buf, "?") == 0) {
      print_help();
      continue;
    }
    Serial.printf("[console] unknown: %s (try 'help')\n", buf);
  }
}

#include "BluetoothA2DPSink.h"

// -------------------------------------------------------------- loudness ----
/*
 * Why the stock sink sounds quiet.
 *
 * ESP32-A2DP scales every sample before it reaches the DAC, and its default
 * curve (A2DPDefaultVolumeControl) is exponential. The phone does not attenuate
 * the stream itself -- with AVRCP absolute volume it forwards its slider
 * position and expects the sink to do the work -- so that curve *is* the volume
 * you get. A phone at half slider (64/127) lands on factor 489/4096, about
 * -18 dB, before the PCM5102A ever sees a sample.
 *
 * This control replaces the curve with a linear one: amplitude tracks the
 * slider, so half slider is -6 dB rather than -18 dB. That alone is worth
 * ~12 dB at normal listening settings.
 *
 * OUTPUT_GAIN is folded into the same factor, so boosting costs nothing extra
 * in the audio path. Since a full-scale stream has no headroom, samples pushed
 * past the ceiling are rounded off by a quadratic knee rather than clamped flat
 * -- the difference between a limiter and a square wave. Below the knee, and
 * whenever the resulting factor is <= 1.0, samples pass through untouched.
 *
 * This runs per sample on the Bluetooth task, hence integer-only.
 */
class LoudVolumeControl : public A2DPVolumeControl {
 public:
  LoudVolumeControl() {
    volumeFactorMax = 128;  // linear: the factor is just the slider position
    volumeFactor = 128;     // unity until the phone tells us otherwise
  }

  void update_audio_data(Frame *data, uint16_t frameCount) override {
    if (data == nullptr) return;
    for (uint16_t i = 0; i < frameCount; i++) {
      data[i].channel1 = shape(data[i].channel1);
      data[i].channel2 = shape(data[i].channel2);
    }
    // The display taps the stream here, after shaping, so the meters show what
    // actually leaves the DAC. It is a downmix and a store per sample pair and
    // nothing else -- see audio_probe.h for why that matters on this task.
    audio_probe_feed(data, frameCount);
  }

 protected:
  /// 0..127 used directly as the factor -> amplitude tracks the slider.
  void set_volume(uint8_t volume) override {
    volumeFactor = (int32_t)(volume * OUTPUT_GAIN + 0.5f);
    ps_set_volume(volume);  // and the UI gets to draw the popup
  }

 private:
  static const int32_t CEILING = 32767;
  static const int32_t KNEE = 24576;           // 0.75 of full scale
  static const int32_t ROOM = CEILING - KNEE;  // width of the soft region

  int16_t shape(int32_t sample) {
    int32_t v = sample * volumeFactor / volumeFactorMax;

    // Not boosting? Then v cannot leave int16 range, so there is nothing to do.
    if (volumeFactor <= volumeFactorMax) return (int16_t)v;

    int32_t mag = v < 0 ? -v : v;
    if (mag > KNEE) {
      const int32_t over = mag - KNEE;
      // y = KNEE + x - x*x / 4R, which arrives at the ceiling flat at x = 2R.
      mag = (over >= 2 * ROOM) ? CEILING
                               : KNEE + over - (over * over) / (4 * ROOM);
      v = v < 0 ? -mag : mag;
    }
    return (int16_t)v;
  }
};

LoudVolumeControl volume_control;
BluetoothA2DPSink a2dp_sink(i2s);

// -------------------------------------------------------------- melodies ----
/*
 * Tone sequences written straight into the same I2S stream the music uses.
 *
 * Two rules keep that safe:
 *
 *   1. Nothing is played from a Bluetooth callback. Those run in line with the
 *      audio path, and half a second of blocking I2S writes there would stall
 *      the decoder. A callback only sets `pending_melody`; loop() plays it.
 *   2. Nothing is played while a stream is actually running, or the Bluetooth
 *      task and the Arduino task would interleave samples into one I2S channel.
 *      On connect the phone has not pressed play yet, and after a disconnect
 *      there is nothing left to collide with, so in practice the melodies get
 *      the DAC to themselves.
 *
 * Each note fades in and out over a few milliseconds. Without that the waveform
 * starts and stops mid-cycle and every note edge clicks.
 */
struct Note {
  uint16_t freq;  // Hz, 0 = rest
  uint16_t ms;
};

// C5 E5 G5, rising -- "I'm here"
static const Note MELODY_CONNECT[] = {
    {523, 90}, {659, 90}, {784, 170}, {0, 40}};

// The same figure falling, G5 E5 C5 -- "gone"
static const Note MELODY_DISCONNECT[] = {
    {784, 90}, {659, 90}, {523, 190}, {0, 40}};

// Two blips -- the generic notification chime, played once the radio is up and
// the speaker is discoverable.
static const Note MELODY_NOTIFY[] = {
    {880, 70}, {0, 45}, {1175, 110}, {0, 40}};

enum MelodyId : uint8_t {
  MELODY_NONE = 0,
  MELODY_ID_CONNECT,
  MELODY_ID_DISCONNECT,
  MELODY_ID_NOTIFY,
};

// Written from the Bluetooth task, read from loop().
static volatile uint8_t pending_melody = MELODY_NONE;

static const uint32_t TONE_CHUNK_FRAMES = 128;

static void play_note(const Note &note) {
  static int16_t frames[TONE_CHUNK_FRAMES * 2];  // interleaved L/R

  const uint32_t total = (uint32_t)SAMPLE_RATE * note.ms / 1000;
  if (total == 0) return;

  // ~4 ms of fade at each end, or half the note if it is shorter than that.
  uint32_t fade = SAMPLE_RATE / 250;
  if (fade > total / 2) fade = total / 2;

  const float step = TWO_PI * note.freq / SAMPLE_RATE;
  float phase = 0.0f;

  for (uint32_t done = 0; done < total;) {
    uint32_t n = total - done;
    if (n > TONE_CHUNK_FRAMES) n = TONE_CHUNK_FRAMES;

    for (uint32_t i = 0; i < n; i++) {
      int16_t sample = 0;
      if (note.freq != 0) {
        const uint32_t pos = done + i;
        float env = 1.0f;
        if (fade > 0) {
          if (pos < fade) {
            env = (float)pos / fade;
          } else if (total - pos < fade) {
            env = (float)(total - pos) / fade;
          }
        }
        sample = (int16_t)(sinf(phase) * MELODY_AMPLITUDE * env);
        phase += step;
        if (phase >= TWO_PI) phase -= TWO_PI;
      }
      frames[2 * i] = sample;      // left
      frames[2 * i + 1] = sample;  // right
    }

    // The chimes are shown on the display too, which costs nothing here and
    // makes the connect animation move in time with the sound.
    audio_probe_feed((const Frame *)frames, (uint16_t)n);
    i2s.write((const uint8_t *)frames, n * 4);  // blocks until the DMA takes it
    done += n;
  }
}

static void play_melody(const Note *notes, size_t count) {
  for (size_t i = 0; i < count; i++) {
    play_note(notes[i]);
  }
}

/// Plays whatever a callback queued. Called from loop(), never from the
/// Bluetooth task.
static void service_melody() {
  const uint8_t id = pending_melody;
  if (id == MELODY_NONE) return;
  pending_melody = MELODY_NONE;

  // Music already flowing? Then the Bluetooth task owns the I2S channel.
  if (a2dp_sink.get_audio_state() == ESP_A2D_AUDIO_STATE_STARTED) return;

  switch (id) {
    case MELODY_ID_CONNECT:
      Serial.println("[bt] connected");
      play_melody(MELODY_CONNECT,
                  sizeof(MELODY_CONNECT) / sizeof(MELODY_CONNECT[0]));
      break;
    case MELODY_ID_DISCONNECT:
      Serial.println("[bt] disconnected");
      play_melody(MELODY_DISCONNECT,
                  sizeof(MELODY_DISCONNECT) / sizeof(MELODY_DISCONNECT[0]));
      break;
    case MELODY_ID_NOTIFY:
      play_melody(MELODY_NOTIFY,
                  sizeof(MELODY_NOTIFY) / sizeof(MELODY_NOTIFY[0]));
      break;
    default:
      break;
  }
}

// ------------------------------------------------------------- callbacks ----
// Callbacks run on the Bluetooth task, in line with the audio path. Keep them
// short and never print from them at any volume -- a blocking Serial write here
// starves the I2S writer and you hear it. Queue the melody, hand the fact to
// player_state (which never blocks, by construction), return, and let loop()
// and the UI task do the slow parts.

void on_connection_state_changed(esp_a2d_connection_state_t state, void *) {
  switch (state) {
    case ESP_A2D_CONNECTION_STATE_CONNECTED:
      digitalWrite(PIN_STATUS_LED, HIGH);
      ps_set_connection(true, *a2dp_sink.get_current_peer_address());
      pending_melody = MELODY_ID_CONNECT;
      break;
    case ESP_A2D_CONNECTION_STATE_DISCONNECTED:
      digitalWrite(PIN_STATUS_LED, LOW);
      ps_set_connection(false, nullptr);
      pending_melody = MELODY_ID_DISCONNECT;
      break;
    default:  // CONNECTING / DISCONNECTING: nothing to announce yet
      break;
  }
}

void on_audio_state_changed(esp_a2d_audio_state_t state, void *) {
  ps_set_streaming(state == ESP_A2D_AUDIO_STATE_STARTED);
}

/// Title, artist, album, genre, track number, duration. Parsed and stored by
/// player_state; nothing is printed here (see the note above), loop() logs the
/// changes instead.
void on_avrc_metadata(uint8_t id, const uint8_t *text) {
  ps_set_metadata(id, text);
}

void on_avrc_connection_state(bool connected) { ps_set_avrc(connected); }

void on_avrc_playstatus(esp_avrc_playback_stat_t playback) {
  switch (playback) {
    case ESP_AVRC_PLAYBACK_PLAYING: ps_set_playback(PS_PLAYING); break;
    case ESP_AVRC_PLAYBACK_PAUSED: ps_set_playback(PS_PAUSED); break;
    case ESP_AVRC_PLAYBACK_FWD_SEEK:
    case ESP_AVRC_PLAYBACK_REV_SEEK: ps_set_playback(PS_SEEKING); break;
    default: ps_set_playback(PS_STOPPED); break;
  }
}

/// Position in ms, once a second (see the notif_interval below). The progress
/// bar interpolates between these.
void on_avrc_play_pos(uint32_t pos_ms) { ps_set_position(pos_ms); }

void on_avrc_track_change(uint8_t *) { ps_new_track(); }

void on_peer_name(char *name) { ps_set_peer_name(name); }

void on_sample_rate(uint16_t rate) { ps_set_sample_rate(rate); }

/// Solid while a phone is connected, a short flash every 2 s while waiting.
/// Timed off millis() rather than delay() so a queued melody starts promptly.
static void update_status_led() {
  const bool connected = a2dp_sink.is_connected();
  digitalWrite(PIN_STATUS_LED,
               connected ? HIGH : (millis() % 2000 < 60 ? HIGH : LOW));
}

/// The serial log the README documents. Driven from loop() by watching the
/// shared state change, precisely so that no printing happens in a callback.
static void log_state_changes() {
  static uint32_t seen_track_seq;
  static bool seen_connected;
  static char seen_peer[PS_NAME_MAX];

  PlayerInfo s;
  ps_snapshot(&s);

  if (s.connected != seen_connected) {
    seen_connected = s.connected;
    seen_peer[0] = 0;
  }
  if (s.connected && s.peer[0] && strcmp(s.peer, seen_peer) != 0) {
    strncpy(seen_peer, s.peer, sizeof(seen_peer) - 1);
    seen_peer[sizeof(seen_peer) - 1] = 0;
    Serial.printf("[bt] peer: %s\n", seen_peer);
  }
  if (s.track_seq != seen_track_seq) {
    seen_track_seq = s.track_seq;
    if (s.title[0]) {
      if (s.artist[0]) {
        Serial.printf("[avrc] %s - %s\n", s.artist, s.title);
      } else {
        Serial.printf("[avrc] title: %s\n", s.title);
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n=== %s v%s ===\n", APP_NAME, FW_VERSION);

  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, LOW);

  AudioLogger::instance().begin(Serial, AudioLogger::Warning);

  // The shared state and the analyser have to exist before anything can write
  // to them, which for the analyser means before the first audio packet.
  DEVICE_NAME = management_device_name(DEVICE_NAME);
  ps_init(DEVICE_NAME);
  audio_probe_init();

  // Display first: it brings up I2C, which the DS3231 path in soft_clock also
  // uses, and it puts a splash on the panel during the seconds the radio takes.
  const bool have_display = ui_begin();

  // Before a2dp_sink.start(), because the optional NTP sync needs the radio to
  // itself -- Wi-Fi and Bluetooth Classic share one antenna, and overlapping
  // them is how you get dropouts.
  soft_clock_begin();

  // Wi-Fi is brought up before Bluetooth starts so the shared radio is never
  // reconfigured in the middle of an A2DP stream. Connection is asynchronous;
  // an unreachable network falls back to the setup AP after 15 seconds.
  management_begin(a2dp_sink);

  auto cfg = make_i2s_config();
  i2s.begin(cfg);

  // Must be installed before set_volume() below, which is what first enables it.
  a2dp_sink.set_volume_control(&volume_control);

  a2dp_sink.set_on_connection_state_changed(on_connection_state_changed);
  a2dp_sink.set_on_audio_state_changed(on_audio_state_changed);
  a2dp_sink.set_avrc_metadata_callback(on_avrc_metadata);
  a2dp_sink.set_avrc_connection_state_callback(on_avrc_connection_state);
  a2dp_sink.set_avrc_rn_playstatus_callback(on_avrc_playstatus);
  a2dp_sink.set_avrc_rn_track_change_callback(on_avrc_track_change);
  a2dp_sink.set_peer_name_callback(on_peer_name);
  a2dp_sink.set_sample_rate_callback(on_sample_rate);

  // Position notifications every second rather than the library's default ten,
  // so the progress bar has something recent to interpolate from. One AVRCP
  // message per second is nothing next to the audio stream.
  a2dp_sink.set_avrc_rn_play_pos_callback(on_avrc_play_pos, 1);

  // The library's default metadata mask leaves out PLAYING_TIME, which is the
  // track length -- without it there is no denominator for the progress bar.
  a2dp_sink.set_avrc_metadata_attribute_mask(
      ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST |
      ESP_AVRC_MD_ATTR_ALBUM | ESP_AVRC_MD_ATTR_TRACK_NUM |
      ESP_AVRC_MD_ATTR_NUM_TRACKS | ESP_AVRC_MD_ATTR_GENRE |
      ESP_AVRC_MD_ATTR_PLAYING_TIME);

  // Reconnect to the last phone automatically after a reboot / walk-out-of-range.
  a2dp_sink.set_auto_reconnect(true);

  // Keep the I2S channel open when the stream suspends. By default the library
  // calls end() on the output at every pause, which would leave the melodies
  // writing into a closed channel (I2SStream::write returns 0 when inactive) --
  // and stopping/restarting the channel pops through the DAC anyway.
  a2dp_sink.set_output_active_by_state(false);

  a2dp_sink.start(DEVICE_NAME);
  a2dp_sink.set_volume(START_VOLUME);

  // Radio is up: hand the panel over to the UI task, which from here on owns it.
  if (have_display) ui_start();

  Serial.printf("Discoverable as \"%s\" - pair from your phone.\n", DEVICE_NAME);
  Serial.println("Type 'help' for the serial commands (clock, screens).");

  // Chime once the radio is up: the speaker is ready to be paired.
  pending_melody = MELODY_ID_NOTIFY;
}

void loop() {
  // A2DP, I2S and the display all run in their own FreeRTOS tasks. What is left
  // here is the status LED, the melodies a callback queued, the serial log and
  // the console.
  service_melody();
  update_status_led();
  log_state_changes();
  soft_clock_tick();
  management_loop();
  poll_console();
  delay(10);
}
