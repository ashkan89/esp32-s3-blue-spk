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
 *   DIN  -> GPIO23   (not GPIO22 -- that is I2C SCL, see below)
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
 *   SCL  -> GPIO22
 *
 * I2C gets the canonical 21/22 pair, which is what the modules are labelled for,
 * and I2S data moves to GPIO23 to make room. Two signals cannot share a pin.
 *
 * Wiring (DS3231 RTC -> ESP32), optional and on the same bus:
 *   VCC  -> 3.3V     GND -> GND
 *   SDA  -> GPIO21   SCL -> GPIO22    (in parallel with the OLED; 0x68 vs 0x3C)
 *
 * Both are optional: if nothing answers on the I2C bus the UI switches itself
 * off at boot, the clock falls back to network/serial/build time, and everything
 * else behaves as it did before.
 *
 * Two blocks of optional hardware live in hw_config.h rather than here, because
 * they are pure wiring and the list is long: a DFPlayer Mini (a fifth radio
 * mode, in which the audio comes off a microSD card or a USB stick instead of
 * over the air) and a battery gauge. Both are absent-tolerant in the same way
 * the display is -- no module on the UART reports itself offline, no divider on
 * the ADC reports no battery, and nothing else changes.
 *
 * One thing about the DFPlayer is worth knowing before wiring it: it produces
 * *analog* audio on its own DAC pins, so it does not feed the PCM5102A. It joins
 * the chain at the output jack through a passive summing network, which is safe
 * because the modes are mutually exclusive and only one source is ever running.
 * The full wiring is in hw_config.h.
 *
 */

#include <WiFi.h>
#include <esp_gap_bt_api.h>
#include <esp_system.h>

#include "AudioTools.h"
#include "app_config.h"
#include "audio_probe.h"
#include "battery.h"
#include "ble_control.h"
#include "df_player.h"
#include "hw_config.h"
#include "leds.h"
#include "management.h"
#include "net_audio.h"
#include "player_state.h"
#include "power.h"
#include "soft_clock.h"
#include "status_led.h"
#include "ui.h"
#include "ui_config.h"

// ---------------------------------------------------------------- config ----
static const char *DEVICE_NAME = APP_NAME;

static const int PIN_I2S_BCLK = 26;
static const int PIN_I2S_LRCK = 25;
// GPIO23, not the GPIO22 most I2S examples use: the OLED and the DS3231 share
// the canonical I2C pair on GPIO21/22, and two signals cannot share a pin. See
// the wiring note at the top of ui_config.h.
static const int PIN_I2S_DOUT = 23;

// The on-board LED of most WROOM-32D devkits. It is the only indicator this
// board has, so status_led.h drives it as a real one: a distinct blink pattern
// per state, plus one-shot blips for events. Override either from build_flags
// if your board wires the LED the other way round or brings it out elsewhere.
#ifndef PIN_STATUS_LED
#define PIN_STATUS_LED 2
#endif
#ifndef STATUS_LED_ACTIVE_HIGH
#define STATUS_LED_ACTIVE_HIGH 1
#endif

// Bluetooth "Audio/Video" minor device class 0x05, Loudspeaker. The IDF header
// only names the major classes, so this one is spelled out.
static const uint8_t BT_COD_MINOR_LOUDSPEAKER = 0x05;

static const int SAMPLE_RATE = 44100;  // A2DP/SBC is 44.1 kHz in practice

// Where the DLNA renderer serves its description and SOAP endpoints in Wi-Fi +
// BLE mode. Deliberately not 80: the dashboard is already there, and control
// points are perfectly happy with any port as long as SSDP advertises it.
static const uint16_t NET_AUDIO_HTTP_PORT = 9000;

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
      "  radio                     current mode and radio state\n"
      "  mode                      next radio mode, reboots\n"
      "  bt                        switch to Bluetooth only mode, reboots\n"
      "  wifi                      switch to Wi-Fi only mode, reboots\n"
      "  both                      switch to Wi-Fi + Bluetooth mode, reboots\n"
      "  net                       switch to Wi-Fi + BLE mode, reboots\n"
      "  sd                        switch to DFPlayer mode, reboots\n"
      "  pair                      force Bluetooth discoverable again\n"
      "  play <url>                play a network stream (Wi-Fi + BLE)\n"
      "  stop                      stop network playback\n"
      "  df                        DFPlayer status ('df' alone lists its\n"
      "                            own commands: play, folder, source, eq,\n"
      "                            loop, io1, key1, led, reset, ...)\n"
      "  bat                       battery voltage, percentage and state\n"
      "  bat calib <volts>         trim the gauge to a meter reading\n"
      "  leds                      WS2812 ring status ('leds' alone lists its\n"
      "                            own commands: on, off, fx, list, color,\n"
      "                            bright, speed, react)\n"
      "  help"));
}

// Defined below, once the Bluetooth sink exists.
static bool radio_command(const char *line);

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
    if (radio_command(buf)) continue;
    // Before battery_command(): "bat" is a prefix of nothing here, but "df" and
    // "bt" are one letter apart and radio_command() owns "bt", so the order of
    // these three is what keeps them from shadowing each other.
    if (df_player_command(buf)) continue;
    if (battery_command(buf)) continue;
    // Persisted straight away: a colour set over the console is a setting
    // like any other, and losing it at the next reboot would be a surprise.
    if (leds_command(buf)) {
      management_store_leds();
      continue;
    }
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

// The sink is started from loop(), not setup(): see service_bluetooth_start().
static bool bt_started;
static uint32_t bt_started_at;
static bool bt_identity_applied;

// The Wi-Fi + BLE equivalents: see service_network_audio(). BLE comes up as soon
// as the mode is known, the network player only once there is an IP address.
static bool net_started;
static bool ble_started;

// DFPlayer mode: see service_dfplayer(). One attempt, at the first loop, so the
// serial console is up to log into and the network bring-up is not held behind
// a card that takes a second to mount.
static bool df_started;
static bool df_autoplay_pending;

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
    // loop() is stalled for the length of the melody; keep the indicator alive.
    status_led_tick();
    done += n;
  }
}

static void play_melody(const Note *notes, size_t count) {
  for (size_t i = 0; i < count; i++) {
    play_note(notes[i]);
  }
}

/// True while some audio path is actively writing to the DAC. Two tasks writing
/// into one I2S channel interleaves their samples, so the melodies check this
/// before taking the DAC -- the A2DP rule in the comment above, extended to the
/// network player, which is a different task with exactly the same problem.
static bool dac_busy();

/// Plays whatever a callback queued. Called from loop(), never from the
/// Bluetooth task.
static void service_melody() {
  const uint8_t id = pending_melody;
  if (id == MELODY_NONE) return;
  pending_melody = MELODY_NONE;

  // Music already flowing? Then another task owns the I2S channel.
  if (dac_busy()) return;

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
      status_led_blip(2);
      ps_set_connection(true, *a2dp_sink.get_current_peer_address());
      pending_melody = MELODY_ID_CONNECT;
      break;
    case ESP_A2D_CONNECTION_STATE_DISCONNECTED:
      status_led_blip(3);
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

void on_avrc_track_change(uint8_t *) {
  ps_new_track();
  status_led_blip(1);
}

void on_peer_name(char *name) { ps_set_peer_name(name); }

void on_sample_rate(uint16_t rate) { ps_set_sample_rate(rate); }

/// Chooses the resting blink pattern. The network and update layer gets first
/// refusal -- an access point waiting to be configured or an OTA write in
/// progress matters more than what Bluetooth is doing -- and Bluetooth fills in
/// the rest. status_led_tick() does the actual blinking, off millis().
static void update_status_led() {
  StatusLedState state;
  if (!management_led_state(&state)) {
    if (radio_mode_has_dfplayer(management_radio_mode())) {
      /*
       * The DFPlayer reuses the same vocabulary as the two radio sources --
       * solid means audio is flowing, a lone flash means up and waiting -- with
       * one addition it genuinely needs: a module with no card in it, or with
       * its card mounted on a computer, is a state the other sources have no
       * equivalent of and the one thing you would actually go and fix.
       */
      // update_status_led() runs every loop and the snapshot takes a mutex, so
      // it is cached: none of these states is worth re-deciding at 100 Hz, and
      // the pattern itself only advances every 125 ms.
      // Only replaced when the read succeeded: a timed-out snapshot comes back
      // zero-filled, which reads as an offline module, and one busy mutex would
      // otherwise flash the fault pattern for a fifth of a second.
      static DfStatus d;
      static uint32_t d_at;
      if (d_at == 0 || millis() - d_at >= 200) {
        DfStatus fresh;
        if (df_player_snapshot(&fresh)) {
          d = fresh;
          d_at = millis() | 1;
        }
      }
      if (!df_started) state = LED_BOOT;
      // Standby before the offline test: a sleeping module answers nothing on
      // purpose, and the fault pattern is for things that went wrong.
      else if (d.asleep) state = LED_IDLE;
      else if (!d.online) state = LED_FAULT;
      else if (d.busy) state = LED_BT_STREAMING;
      else if (d.pcLink) state = LED_NO_MEDIA;
      else if (d.source == DF_SRC_SD && !d.sdPresent) state = LED_NO_MEDIA;
      else if (d.source == DF_SRC_USB && !d.usbPresent) state = LED_NO_MEDIA;
      else if (d.state == DF_PAUSED) state = LED_BT_CONNECTED;
      else state = LED_IDLE;
    } else if (radio_mode_has_ble(management_radio_mode())) {
      // Network audio reuses the Bluetooth patterns rather than inventing two
      // more: "solid" still means audio is flowing and a lone flash still means
      // up and waiting. What is on the other end of the link is not something
      // one LED can usefully distinguish.
      const NetAudioState net = net_audio_state();
      if (!net_started) state = LED_BOOT;
      else if (net_audio_active()) state = LED_BT_STREAMING;
      else if (net == NET_AUDIO_ERROR) state = LED_FAULT;
      else if (net != NET_AUDIO_IDLE) state = LED_BT_CONNECTED;
      else state = LED_IDLE;
    } else if (!bt_started) {
      state = LED_BOOT;
    } else if (a2dp_sink.get_audio_state() == ESP_A2D_AUDIO_STATE_STARTED) {
      state = LED_BT_STREAMING;
    } else if (a2dp_sink.is_connected()) {
      state = LED_BT_CONNECTED;
    } else {
      state = LED_IDLE;
    }
  }
  status_led_state(state);
  status_led_tick();
}

static bool dac_busy() {
  if (a2dp_sink.get_audio_state() == ESP_A2D_AUDIO_STATE_STARTED) return true;
  if (net_audio_active()) return true;
  /*
   * The DFPlayer does not share the I2S channel -- its audio is analog and joins
   * further downstream -- so a melody here cannot interleave samples with it the
   * way it could with A2DP. It can still talk over it, because both land on the
   * same jack, and that is reason enough: the chimes exist to be noticed, not to
   * be played on top of a song.
   */
  return df_player_active();
}

/// Radio state and the mode switch, from the serial console. Handy precisely
/// when the access point is unreachable and the dashboard therefore is too.
static bool radio_command(const char *line) {
  if (strcmp(line, "radio") == 0) {
    const RadioMode mode = management_radio_mode();
    Serial.printf("[radio] mode %s", management_mode_name(mode));
    // Both halves are reported when both are running, so "wifi is up but no
    // phone can find me" is one line rather than a guess.
    if (radio_mode_has_wifi(mode)) {
      const bool sta = WiFi.status() == WL_CONNECTED;
      Serial.printf(" | wifi %s", sta ? WiFi.SSID().c_str() : "disconnected");
      if (sta) {
        Serial.printf(" %s rssi %d", WiFi.localIP().toString().c_str(),
                      (int)WiFi.RSSI());
      }
      // Combo mode never raises the setup AP, so reporting it there is noise.
      if (mode != RADIO_MODE_COMBO) {
        Serial.printf(" | ap %s", management_ap_running() ? "up" : "down");
      }
      if (management_ap_running()) {
        Serial.printf(" %s ch %d clients %u", WiFi.softAPIP().toString().c_str(),
                      WiFi.channel(), WiFi.softAPgetStationNum());
      }
    }
    if (radio_mode_has_a2dp(mode)) {
      Serial.printf(" | bt %s", !bt_started                ? "starting"
                                : a2dp_sink.is_connected() ? "connected"
                                : bt_identity_applied      ? "discoverable"
                                                           : "starting");
    }
    if (radio_mode_has_ble(mode)) {
      Serial.printf(" | ble %s", !ble_control_running() ? "off" : "advertising");
      if (ble_control_clients()) {
        Serial.printf(" %u client%s", ble_control_clients(),
                      ble_control_clients() == 1 ? "" : "s");
      }
      NetAudioStatus n;
      net_audio_snapshot(&n);
      Serial.printf(" | dlna %s", n.renderer_up ? "up" : "down");
      Serial.printf(" | audio %s",
                    !net_started                  ? "waiting for wifi"
                    : n.state == NET_AUDIO_ERROR  ? n.error
                    : net_audio_active()          ? "playing"
                    : n.state == NET_AUDIO_PAUSED ? "paused"
                    : n.url[0]                    ? "buffering"
                                                  : "idle");
      if (n.url[0]) Serial.printf(" | url %s", n.url);
    }
    if (radio_mode_has_dfplayer(mode)) {
      DfStatus d;
      // A failed snapshot is zero-filled; printing it would say "OFFLINE (check
      // TX/RX)" about a module that is answering, in the command somebody types
      // precisely to find out whether it is.
      const bool fresh = df_player_snapshot(&d);
      Serial.printf(" | df %s", !fresh             ? "status unreadable"
                                : !df_started      ? "starting"
                                : d.asleep         ? "standby"
                                : !d.online        ? "OFFLINE (check TX/RX)"
                                : d.busy           ? "playing"
                                : d.state == DF_PAUSED ? "paused"
                                                   : "idle");
      if (fresh) {
        Serial.printf(" %s", df_source_name(d.source));
        if (d.totalTracks) {
          Serial.printf(" %u/%u", (unsigned)d.track, (unsigned)d.totalTracks);
        }
      }
      if (fresh && d.pcLink) Serial.print(" | card on a computer");
      if (fresh && d.error[0]) Serial.printf(" | %s", d.error);
    }
    if (battery_present()) {
      Serial.printf(" | bat %u%% %s", (unsigned)battery_percent(),
                    battery_state_name(battery_state()));
    }
    Serial.printf(" | heap %u\n", (unsigned)ESP.getFreeHeap());
    return true;
  }
  if (strcmp(line, "mode") == 0) {
    management_switch_mode(management_next_mode());  // does not return
    return true;
  }
  if (strcmp(line, "bt") == 0) {
    management_switch_mode(RADIO_MODE_BLUETOOTH);  // does not return
    return true;
  }
  if (strcmp(line, "wifi") == 0) {
    management_switch_mode(RADIO_MODE_MANAGEMENT);  // does not return
    return true;
  }
  if (strcmp(line, "both") == 0 || strcmp(line, "combo") == 0) {
    management_switch_mode(RADIO_MODE_COMBO);  // does not return
    return true;
  }
  if (strcmp(line, "net") == 0 || strcmp(line, "ble") == 0) {
    management_switch_mode(RADIO_MODE_NET);  // does not return
    return true;
  }
  if (strcmp(line, "sd") == 0 || strcmp(line, "dfplayer") == 0) {
    management_switch_mode(RADIO_MODE_DFPLAYER);  // does not return
    return true;
  }
  if (strncmp(line, "play ", 5) == 0) {
    if (!radio_mode_has_ble(management_radio_mode())) {
      Serial.println("[net] not in Wi-Fi + BLE mode; type 'net' to switch");
    } else if (!net_audio_running()) {
      Serial.println("[net] the player is still waiting for a Wi-Fi address");
    } else if (!net_audio_play_url(line + 5, "console")) {
      Serial.println("[net] that URL is empty or too long");
    } else {
      Serial.printf("[net] playing %s\n", line + 5);
    }
    return true;
  }
  if (strcmp(line, "stop") == 0) {
    if (!net_audio_running()) {
      Serial.println("[net] the network player is not running in this mode");
    } else {
      net_audio_stop();
      Serial.println("[net] stopped");
    }
    return true;
  }
  if (strcmp(line, "pair") == 0) {
    if (radio_mode_has_ble(management_radio_mode())) {
      Serial.println("[bt] Wi-Fi + BLE mode has no A2DP sink to pair with. "
                     "Cast to it over DLNA, or type 'bt' / 'both' for a "
                     "Bluetooth mode.");
    } else if (radio_mode_has_dfplayer(management_radio_mode())) {
      Serial.println("[bt] DFPlayer mode has no Bluetooth at all -- the whole "
                     "controller is released at boot. Type 'bt' or 'both' for a "
                     "Bluetooth mode.");
    } else if (!radio_mode_has_a2dp(management_radio_mode())) {
      Serial.println("[bt] Wi-Fi mode; type 'bt' or 'both' to start Bluetooth");
    } else if (!bt_started) {
      Serial.println("[bt] still starting");
    } else if (a2dp_sink.is_connected()) {
      Serial.println("[bt] a phone is already connected; disconnect it first");
    } else {
      a2dp_sink.set_discoverability(ESP_BT_GENERAL_DISCOVERABLE);
      Serial.println("[bt] discoverable now");
    }
    return true;
  }
  return false;
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
  /*
   * The battery, on transitions only.
   *
   * The percentage moves all the time and logging it would drown everything
   * else, but the four moments that matter -- charger on, charger off, low,
   * critical -- are exactly the ones somebody reading the log later wants
   * timestamped, and they are also the ones nobody is watching the dashboard
   * for when they happen.
   */
  {
    static BatteryState seen_battery = BAT_UNKNOWN;
    static bool seen_any;
    const BatteryState now_state = battery_state();
    if (!seen_any || now_state != seen_battery) {
      seen_any = true;
      seen_battery = now_state;
      if (battery_present()) {
        BatteryStatus b;
        battery_snapshot(&b);
        Serial.printf("[bat] %s: %u%% (%.2f V)\n", battery_state_name(now_state),
                      (unsigned)b.percent, b.volts);
      }
    }
  }

  if (s.track_seq != seen_track_seq) {
    seen_track_seq = s.track_seq;
    // The tag says where the metadata came from: AVRCP off a phone, or ICY /
    // DIDL off the network. Worth the two characters when the question being
    // asked of the log is usually "is the source sending me anything at all".
    const char *tag = s.source == PS_SRC_NETWORK    ? "net"
                      : s.source == PS_SRC_DFPLAYER ? "df"
                                                    : "avrc";
    if (s.title[0]) {
      if (s.artist[0]) {
        Serial.printf("[%s] %s - %s\n", tag, s.artist, s.title);
      } else {
        Serial.printf("[%s] title: %s\n", tag, s.title);
      }
    }
  }
}

/// Completes a factory reset the BOOT button asked for.
///
/// The wipe itself is management's, but the reboot has to happen here and only
/// once the button is up: GPIO0 is the download-mode strap, and restarting
/// while it is still held drops the chip into the ROM serial bootloader, where
/// it looks bricked.
/// The BOOT button when there is no panel to draw the countdown on. ui.cpp owns
/// the button whenever a display was found; this is the fallback so a speaker
/// built without one is still resettable. Same hold time, LED instead of digits.
static bool poll_reset_button_headless() {
  if (PIN_UI_BUTTON < 0 || ui_present()) return false;

  static uint32_t since;
  static bool announced;
  static bool mode_armed;
  const bool down = digitalRead(PIN_UI_BUTTON) == LOW;

  if (!down) {
    // Released between the mode tier and the reset tier: switch. There is no
    // panel to show a "press again to confirm" offer on, so the hold itself is
    // the confirmation -- three seconds is already a deliberate act.
    const bool switch_now = mode_armed;
    since = 0;
    announced = false;
    mode_armed = false;
    if (switch_now) management_switch_mode(management_next_mode());
    return false;
  }
  if (!since) {
    since = millis();
    return false;
  }

  const uint32_t held = millis() - since;
  if (held >= UI_BTN_MODE_MS && !mode_armed) {
    mode_armed = true;
    Serial.printf("[mode] release BOOT to switch to %s mode, or keep holding "
                  "to factory reset\n",
                  management_mode_name(management_next_mode()));
    status_led_blip(2);
  }
  if (held < UI_BTN_RESET_ARM_MS) return false;
  mode_armed = false;  // past the mode tier now; releasing must not switch
  if (!announced) {
    announced = true;
    Serial.println("[reset] keep holding BOOT to wipe settings and pairings");
  }
  status_led_state(LED_FAULT);
  if (held < (uint32_t)UI_BTN_RESET_ARM_MS + UI_BTN_RESET_COUNT_MS) return false;
  since = 0;
  return true;
}

static void service_factory_reset() {
  if (!ui_take_factory_reset_request() && !poll_reset_button_headless()) return;

  Serial.println("[reset] BOOT held through the countdown; clearing everything");
  management_factory_reset();

  while (PIN_UI_BUTTON >= 0 && digitalRead(PIN_UI_BUTTON) == LOW) {
    status_led_state(LED_UPDATING);
    status_led_tick();
    delay(10);
  }
  delay(300);  // let the OLED land on the message before the panel goes dark
  ESP.restart();
}

/// Names the device properly, once the stack is actually up.
///
/// a2dp_sink.start() only queues the bring-up: esp_a2d_sink_init() and the
/// first esp_bt_gap_set_scan_mode() run later, on the library's own work task.
/// esp_bt_gap_set_cod() has to come after that -- the IDF header is explicit
/// that a class of device written before the profiles register gets
/// overwritten, and writing it mid-init raced the scan-mode setup and left the
/// speaker invisible. A couple of seconds is comfortably past both.
static void service_bluetooth_identity() {
  if (!bt_started || bt_identity_applied) return;
  if (millis() - bt_started_at < 2500) return;
  bt_identity_applied = true;

  // ESP32-A2DP never sets a class of device, so the sink inherits Bluedroid's
  // default and phones list it as a nondescript "other" device: a generic icon,
  // and on some Android builds no offer to connect it for media at all. Say
  // what this is -- an audio/video loudspeaker that renders audio.
  esp_bt_cod_t cod = {};
  cod.major = ESP_BT_COD_MAJOR_DEV_AV;
  cod.minor = BT_COD_MINOR_LOUDSPEAKER;
  cod.service = ESP_BT_COD_SRVC_RENDERING | ESP_BT_COD_SRVC_AUDIO;
  const esp_err_t err = esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_ALL);
  Serial.printf("[bt] class of device: %s\n", esp_err_to_name(err));

  // And belt-and-braces: say out loud that we want to be findable. Nothing
  // above should have left us hidden, but "the speaker is invisible" is an
  // expensive failure to debug from the other end of a phone.
  if (!a2dp_sink.is_connected()) {
    a2dp_sink.set_discoverability(ESP_BT_GENERAL_DISCOVERABLE);
    Serial.println("[bt] discoverable");
  }
}

/// Brings the A2DP sink up, in the two modes that have Bluetooth in them.
///
/// In Wi-Fi mode the sink is never started at all -- not started and quiet, but
/// never initialised, so the antenna and the controller's heap stay entirely
/// with Wi-Fi. In Bluetooth mode it is the other way round. In Wi-Fi + BT both
/// run and the coexistence scheduler shares the radio between them; nothing
/// here changes for that, because start() is the same call either way.
/// See the note in management.h.
static void service_bluetooth_start() {
  if (bt_started) return;
  if (!radio_mode_has_a2dp(management_radio_mode())) return;

  const uint32_t heap_before = ESP.getFreeHeap();
  a2dp_sink.start(DEVICE_NAME);
  a2dp_sink.set_volume(START_VOLUME);
  bt_started = true;
  bt_started_at = millis();
  management_set_bt_active(true);
  ps_set_bt_active(true);
  // The heap either side of the bring-up, because in Wi-Fi + BT mode both
  // stacks are resident and this is the number that decides whether the OTA
  // TLS handshake later has room. Cheap to print once, tedious to guess at.
  Serial.printf("Discoverable as \"%s\" - pair from your phone. (heap %u -> %u)\n",
                DEVICE_NAME, (unsigned)heap_before,
                (unsigned)ESP.getFreeHeap());
  // Chime once the radio is up: the speaker is ready to be paired.
  pending_melody = MELODY_ID_NOTIFY;
}

/*
 * Brings up Wi-Fi + BLE mode, in two steps because they are ready at different
 * times.
 *
 * BLE is normally already up by the time this runs: management_begin() starts
 * it before Wi-Fi, because the dual-mode Bluedroid host needs more contiguous
 * heap than is left once Wi-Fi and the dashboard have taken theirs (see the
 * note there). The call below is the idempotent second chance -- it returns
 * immediately when the service is already advertising, and covers the mode
 * being entered by a path that did not go through management_begin().
 *
 * Starting it early is the point either way: if the saved network is wrong, BLE
 * provisioning is the way back in, and it has to be there before anyone gives
 * up waiting for the dashboard.
 *
 * The network player needs an IP address. The DLNA renderer bakes the local
 * address into the description document it advertises, so starting it before
 * DHCP has answered would publish a URL pointing at 0.0.0.0 and no control
 * point would ever reach us.
 */
static void service_network_audio() {
  if (!radio_mode_has_ble(management_radio_mode())) return;

  if (!ble_started) {
    ble_started = true;  // set first: one attempt, success or not
    ble_control_begin(DEVICE_NAME);
  }

  if (!net_started && WiFi.status() == WL_CONNECTED) {
    net_started = true;
    if (net_audio_begin(DEVICE_NAME, NET_AUDIO_HTTP_PORT)) {
      ps_set_net_connection(false, nullptr);
      // Same chime the Bluetooth modes play when the radio is up: the speaker
      // is ready to be sent something.
      pending_melody = MELODY_ID_NOTIFY;
    }
  }
}

/*
 * Brings up the DFPlayer, and then leaves it alone.
 *
 * Started from loop() rather than setup() for two reasons. The module takes
 * about 1.5 s to come back from the reset the driver opens with, and another
 * second to mount a card, and there is nothing to be gained by making the
 * network wait behind that. And starting it after the console exists means the
 * one failure that actually happens -- TX and RX swapped, which looks exactly
 * like a dead module -- is reported somewhere you can read it.
 *
 * There is no equivalent of service_bluetooth_start()'s "wait for the access
 * point to be done with the antenna" here: the DFPlayer is a serial peripheral
 * and does not touch the radio, so it comes up immediately whatever Wi-Fi is
 * doing.
 *
 * Autoplay is deliberately not "send play at boot". The module reports the card
 * a moment after it has mounted it, and a play command sent before that is
 * answered with "file not found" -- so the request is held until there is a
 * library with files in it to play from, and dropped if that never happens.
 */
static void service_dfplayer() {
  if (!radio_mode_has_dfplayer(management_radio_mode())) return;

  if (!df_started) {
    df_started = true;  // set first: one attempt, success or not

    uint8_t source = 0, volume = 0, eq = 0, loop = 0, loop_folder = 1;
    bool autoplay = false;
    management_df_defaults(&source, &volume, &eq, &loop, &loop_folder, &autoplay);

    if (!df_player_begin((DfSource)source, volume, eq, (DfLoop)loop)) {
      Serial.println("[df] driver did not start; DFPlayer mode has no audio "
                     "source this boot. The dashboard still works -- switch to "
                     "another mode from there.");
      return;
    }
    if (loop == DF_LOOP_FOLDER) df_player_set_loop(DF_LOOP_FOLDER, loop_folder);
    df_autoplay_pending = autoplay;
    // Same chime the other modes play once their source is up.
    pending_melody = MELODY_ID_NOTIFY;
    return;
  }

  if (!df_autoplay_pending) return;

  static uint32_t give_up_at;
  if (give_up_at == 0) give_up_at = millis() + 15000;

  DfStatus d;
  // A stale copy looks like "no module, no files", which the deadline below
  // would then report as the reason autoplay gave up. Wait for a real one; the
  // deadline has fifteen seconds of passes to work with.
  if (!df_player_snapshot(&d)) return;
  if (d.online && d.totalTracks > 0) {
    df_autoplay_pending = false;
    df_player_play_track(1);
    Serial.printf("[df] autoplay: starting track 1 of %u on the %s\n",
                  (unsigned)d.totalTracks, df_source_name(d.source));
  } else if ((int32_t)(millis() - give_up_at) >= 0) {
    df_autoplay_pending = false;
    Serial.printf("[df] autoplay gave up: %s\n",
                  !d.online ? "the module never answered"
                  : d.pcLink ? "the card is mounted on a computer"
                             : "no files were found on the selected source");
  }
}

/// Acts on a mode switch the BOOT button asked for. management_switch_mode()
/// persists the choice and restarts, so this never returns when it fires.
static void service_mode_switch() {
  if (!ui_take_mode_switch_request()) return;
  management_switch_mode(management_next_mode());
}

/*
 * Why the chip started.
 *
 * When a mode misbehaves this is the first question and it used to have no
 * answer in the log: a panic, a watchdog and a failing power supply all look
 * identical from the outside -- the speaker just comes back. They need
 * completely different fixes, so the reason goes at the top of every boot.
 *
 * BROWNOUT in particular is not a firmware bug at all. Bringing up Wi-Fi and
 * Bluetooth together roughly doubles the peak current, and a thin USB cable or
 * a marginal supply that carried one radio will not carry two.
 */
static const char *reset_reason_text(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_EXT: return "external reset";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "PANIC - crash, see the backtrace above";
    case ESP_RST_INT_WDT: return "interrupt watchdog - something blocked in an ISR";
    case ESP_RST_TASK_WDT: return "task watchdog - a task stopped feeding it";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "BROWNOUT - the supply sagged, not a firmware fault";
    case ESP_RST_DEEPSLEEP: return "wake from deep sleep";
    default: return "unknown";
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n=== %s v%s ===\n", APP_NAME, FW_VERSION);
  Serial.printf("[boot] reset reason: %s | heap %u free, %u largest block\n",
                reset_reason_text(esp_reset_reason()),
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  status_led_begin(PIN_STATUS_LED, STATUS_LED_ACTIVE_HIGH);

  AudioLogger::instance().begin(Serial, AudioLogger::Warning);

  // The shared state and the analyser have to exist before anything can write
  // to them, which for the analyser means before the first audio packet.
  DEVICE_NAME = management_device_name(DEVICE_NAME);
  ps_init(DEVICE_NAME);
  audio_probe_init();

  // The ring is claimed here so it goes dark immediately, rather than sitting
  // on whatever the pixels powered up holding for the two seconds the radio
  // takes. Its task starts later, once the stored settings have been applied.
  const bool have_leds = leds_begin();

  // Display first: it brings up I2C, which the DS3231 path in soft_clock also
  // uses, and it puts a splash on the panel during the seconds the radio takes.
  const bool have_display = ui_begin();
  // ui_begin() only claims the button when it found a panel; without one the
  // headless reset poll still needs the pull-up.
  if (!have_display && PIN_UI_BUTTON >= 0) pinMode(PIN_UI_BUTTON, INPUT_PULLUP);

  // Before a2dp_sink.start(), because the optional NTP sync needs the radio to
  // itself -- Wi-Fi and Bluetooth Classic share one antenna, and overlapping
  // them is how you get dropouts.
  soft_clock_begin();

  // Wi-Fi is brought up before Bluetooth starts so the shared radio is never
  // reconfigured in the middle of an A2DP stream. Connection is asynchronous;
  // an unreachable network falls back to the setup AP after 15 seconds.
  management_begin(a2dp_sink);
  // The number that decides whether the rest of this boot can succeed: what is
  // left once Wi-Fi has taken its share, and the budget the audio stack is
  // about to be asked to fit into. In the Bluetooth modes the sink has not
  // taken its share yet; in Wi-Fi + BLE the Bluedroid host already has, because
  // it cannot wait until after Wi-Fi -- see the note in management_begin().
  Serial.printf("[boot] after network start: heap %u free, %u largest block\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

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

  // Bluetooth is started from loop() rather than here. While the setup access
  // point is open, management asks for the radio to itself -- see the note in
  // management.h. On a speaker that already has Wi-Fi, or with the setup window
  // switched off, this fires immediately and nothing is delayed.
  service_bluetooth_start();

  // The mode decides which audio path exists at all, and the screens say
  // different things for each -- there is no "pair your phone" on a speaker
  // that receives over the network. Fixed here and never changed again;
  // switching modes reboots.
  ps_set_source(radio_mode_has_a2dp(management_radio_mode())     ? PS_SRC_BLUETOOTH
                : radio_mode_has_ble(management_radio_mode())      ? PS_SRC_NETWORK
                : radio_mode_has_dfplayer(management_radio_mode()) ? PS_SRC_DFPLAYER
                                                                  : PS_SRC_NONE);
  // BLE can come up now; the network player waits for an address (see there).
  service_network_audio();
  // The DFPlayer needs nothing from the network, so it starts here too.
  service_dfplayer();

  // Radio is up: hand the panel over to the UI task, which from here on owns it.
  if (have_display) ui_start();
  // management_begin() has loaded and applied the stored lighting by now, so
  // the first frame this task draws is already the one the owner chose.
  if (have_leds) leds_start();

  Serial.println("Type 'help' for the serial commands (clock, screens, radio).");
}

void loop() {
  // A2DP, I2S and the display all run in their own FreeRTOS tasks. What is left
  // here is the status LED, the melodies a callback queued, the serial log and
  // the console.
  service_factory_reset();
  service_mode_switch();
  service_bluetooth_start();
  service_bluetooth_identity();
  service_network_audio();
  service_dfplayer();
  service_melody();
  // Before update_status_led(): the LED asks whether the cell is critical, and a
  // sample taken after that decision is one frame stale in the one indicator
  // that is meant to be urgent.
  battery_loop();
  // Straight after the gauge and before the indicator: the policy reads the
  // sample battery_loop() just took, and muting the LED is one of the things it
  // may decide to do.
  power_tick();
  update_status_led();
  log_state_changes();
  soft_clock_tick();
  management_loop();
  net_audio_loop();
  ble_control_loop();
  df_player_loop();
  poll_console();
  delay(10);
}
