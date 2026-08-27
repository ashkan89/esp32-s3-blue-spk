#include "net_audio.h"

#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecAACHelix.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "AudioTools/AudioCodecs/CodecWAV.h"
#include "AudioTools/Communication/AudioHttp.h"
#include "AudioTools/Disk/AudioSourceURL.h"
#include "DLNA.h"

#include "app_config.h"
#include "audio_probe.h"
#include "player_state.h"

/*
 * The I2S channel is main.cpp's, and it is the same channel in every mode --
 * same pins, same 16-bit stereo framing, opened once in setup(). Borrowing it
 * rather than opening a second one is deliberate: only one radio mode runs per
 * boot, so exactly one of the A2DP sink and this player is ever writing to it,
 * and duplicating the pin configuration in two files is how the two drift apart.
 */
extern I2SStream i2s;

namespace {

// ------------------------------------------------------------------ state ---

NetAudioStatus status;
SemaphoreHandle_t statusLock;
QueueHandle_t commands;
bool running;
bool rendererUp;

// Written by the audio task, read everywhere. Only ever a whole word, so the
// snapshot does not need the mutex for it and net_audio_active() stays cheap
// enough to call from the melody path on every loop.
volatile bool audioFlowing;
volatile uint32_t lastAudioAt;

/// Raised by the audio task when a stream ends, lowered by the DLNA task once
/// it has told the control point. It is not called in line because notifying
/// subscribers is HTTP traffic, and the audio task is the last place that
/// should be waiting on a socket.
volatile bool playbackCompleted;

/// How long the output can go quiet before we stop claiming to be playing. A
/// stalled stream and a finished one look identical from here; either way the
/// screens should stop saying "live".
constexpr uint32_t AUDIO_IDLE_MS = 1500;

enum CmdOp : uint8_t { CMD_PLAY_URL, CMD_PLAY, CMD_PAUSE, CMD_STOP, CMD_VOLUME };

struct Command {
  CmdOp op;
  uint8_t value;  // volume for CMD_VOLUME
  char url[NET_URL_MAX + 1];
  char origin[8];
};

void statusLocked(void (*fn)(void *), void *arg) {
  if (statusLock == nullptr) return;
  if (xSemaphoreTake(statusLock, pdMS_TO_TICKS(50)) != pdTRUE) return;
  fn(arg);
  xSemaphoreGive(statusLock);
}

// ------------------------------------------------------------ audio chain ---

/*
 * The tap between the decoder and the DAC.
 *
 * It exists for the spectrum analyser. In the Bluetooth modes the analyser is
 * fed from inside the volume control, after shaping, so the meters show what
 * actually leaves the DAC; this is the same idea one layer further out. The
 * work per buffer is a downmix and a store -- see audio_probe.h for why that
 * limit matters -- and everything else is forwarded straight through.
 *
 * setAudioInfo() has to be forwarded too. A network stream is whatever sample
 * rate it happens to be, not the 44.1 kHz A2DP guarantees, and if the rate
 * change never reaches the I2S driver the audio plays at the wrong speed.
 */
class ProbeTap : public AudioOutput {
 public:
  size_t write(const uint8_t *data, size_t len) override {
    if (len == 0) return 0;
    lastAudioAt = millis();
    audioFlowing = true;
    // audio_probe wants interleaved stereo int16, which is exactly what the
    // decoder produces for everything this firmware plays. Anything else (a
    // mono voice stream, 8-bit PCM) still reaches the DAC, it just does not
    // draw bars.
    //
    // The alignment test is not paranoia: this core traps unaligned 16-bit
    // loads and fixes them up in an exception handler, so reading a Frame off
    // an odd address would be correct and ruinously slow, in the one place
    // where slow is audible.
    if (channels == 2 && (len & 3) == 0 && ((uintptr_t)data & 1) == 0) {
      audio_probe_feed((const Frame *)data, (uint16_t)(len / 4));
    }
    return i2s.write(data, len);
  }

  /*
   * Silence while paused, without lying about it.
   *
   * AudioPlayer calls this on every copy() once playback stops, and the base
   * implementation writes the zeros through write() *two bytes at a time* --
   * several hundred calls per buffer, each one stamping lastAudioAt and
   * re-raising audioFlowing. The speaker would then insist it was streaming for
   * as long as it stayed paused, and burn real CPU doing it.
   *
   * So the zeros go straight to I2S in one block, the flags are left alone, and
   * the analyser is not fed -- silence is not a signal and the meters should
   * fall, not sit flat at zero pretending to be live.
   */
  void writeSilence(size_t len) override {
    static const uint8_t zeros[256] = {0};
    while (len > 0) {
      const size_t chunk = len > sizeof(zeros) ? sizeof(zeros) : len;
      const size_t wrote = i2s.write(zeros, chunk);
      if (wrote == 0) break;  // channel is full or closed; do not spin
      len -= wrote;
    }
  }

  void setAudioInfo(AudioInfo info) override {
    AudioOutput::setAudioInfo(info);
    channels = info.channels;
    i2s.setAudioInfo(info);
    auto set = [](void *raw) {
      auto *in = (AudioInfo *)raw;
      status.sample_rate = in->sample_rate;
      status.channels = (uint8_t)in->channels;
    };
    statusLocked(set, &info);
  }

  int availableForWrite() override { return i2s.availableForWrite(); }

 private:
  uint16_t channels = 2;
};

URLStream urlStream;
AudioSourceDynamicURL source(urlStream);
MultiDecoder multiDecoder;
MP3DecoderHelix decMp3;
AACDecoderHelix decAac;
WAVDecoder decWav;
ProbeTap probeTap;
AudioPlayer player(source, probeTap, multiDecoder);

// ------------------------------------------------------------------- DLNA ---

WiFiServer dlnaSocket(0);  // real port set in net_audio_begin()
tiny_dlna::HttpServer<WiFiClient, WiFiServer> dlnaHttp(dlnaSocket);

/*
 * The plain, polled UDP service -- not UDPAsyncService.
 *
 * The async one is tempting because SSDP is bursty, but it is built on
 * AsyncUDP: it runs its own high-priority task, and its packet handler
 * allocates a copy of every datagram from inside the LwIP callback and then
 * asserts that the sender address is non-zero. An assert in a network callback
 * is a reset, and SSDP is multicast traffic from every device on the segment --
 * exactly the place not to trust the contents.
 *
 * This one is polled from dlnaTask() instead, which is where the rest of the
 * renderer already runs, and is what the library's own example uses.
 */
tiny_dlna::UDPService<WiFiUDP> dlnaUdp;
tiny_dlna::DLNAMediaRenderer<WiFiClient> renderer(dlnaHttp, dlnaUdp);

/*
 * Pulls one value out of a DIDL-Lite fragment.
 *
 * Control points send the track's metadata as an XML document, and all we want
 * from it is two strings. A real parser would be a lot of code and a lot of
 * heap for that, so this finds "<tag" ... ">" and copies to the closing tag --
 * which is enough for dc:title and upnp:artist and honest about being nothing
 * more. Returns false when the tag is not there, leaving `out` untouched.
 */
bool extractTag(const char *xml, const char *tag, char *out, size_t cap) {
  if (xml == nullptr || *xml == 0) return false;
  char open[24];
  snprintf(open, sizeof(open), "<%s", tag);
  const char *start = strstr(xml, open);
  if (start == nullptr) return false;
  start = strchr(start, '>');
  if (start == nullptr) return false;
  ++start;
  char close[24];
  snprintf(close, sizeof(close), "</%s", tag);
  const char *end = strstr(start, close);
  if (end == nullptr || end < start) return false;

  size_t n = (size_t)(end - start);
  if (n > cap - 1) n = cap - 1;
  memcpy(out, start, n);
  out[n] = 0;
  return true;
}

/// Reads the title and artist the control point sent alongside the URI. Runs on
/// the DLNA task, so it only touches `status` under the lock.
void adoptDlnaMetadata() {
  const char *didl = renderer.getCurrentUriMetadata();
  char title[sizeof(status.title)] = "";
  char artist[sizeof(status.artist)] = "";
  const bool haveTitle = extractTag(didl, "dc:title", title, sizeof(title));
  const bool haveArtist = extractTag(didl, "upnp:artist", artist, sizeof(artist));
  if (!haveTitle && !haveArtist) return;

  struct Both {
    const char *title;
    const char *artist;
    bool haveTitle, haveArtist;
  } both{title, artist, haveTitle, haveArtist};
  statusLocked(
      [](void *raw) {
        auto *b = (Both *)raw;
        if (b->haveTitle) strlcpy(status.title, b->title, sizeof(status.title));
        if (b->haveArtist)
          strlcpy(status.artist, b->artist, sizeof(status.artist));
      },
      &both);
}

/*
 * Everything a control point asks for arrives here, on the DLNA task.
 *
 * Nothing is done in line. The player belongs to the audio task, so each event
 * turns into a queued command exactly like a dashboard button does -- which
 * also means a DLNA client and the dashboard cannot race each other into the
 * player, whichever order they arrive in.
 */
void onMediaEvent(tiny_dlna::MediaEvent event,
                  tiny_dlna::DLNAMediaRenderer<WiFiClient> &mr) {
  switch (event) {
    case tiny_dlna::MediaEvent::SET_URI:
      adoptDlnaMetadata();
      net_audio_play_url(mr.getCurrentUri(), "dlna");
      break;
    case tiny_dlna::MediaEvent::PLAY:
      net_audio_play();
      break;
    case tiny_dlna::MediaEvent::PAUSE:
      net_audio_pause();
      break;
    case tiny_dlna::MediaEvent::STOP:
      net_audio_stop();
      break;
    case tiny_dlna::MediaEvent::SET_VOLUME:
      // DLNA volume is 0..100; everything in this firmware is 0..127.
      net_audio_set_volume((uint8_t)((mr.getVolume() * 127 + 50) / 100));
      break;
    case tiny_dlna::MediaEvent::SET_MUTE:
      if (mr.isMuted()) net_audio_set_volume(0);
      break;
    default:
      break;
  }
}

// --------------------------------------------------------------- metadata ---

/// ICY (Shoutcast) stream titles and ID3 tags. Runs on the audio task.
void onMetadata(MetaDataType type, const char *str, int len) {
  if (str == nullptr || *str == 0) return;
  struct Item {
    MetaDataType type;
    const char *str;
  } item{type, str};
  statusLocked(
      [](void *raw) {
        auto *it = (Item *)raw;
        switch (it->type) {
          case Title:
            strlcpy(status.title, it->str, sizeof(status.title));
            break;
          case Artist:
            strlcpy(status.artist, it->str, sizeof(status.artist));
            break;
          default:
            break;
        }
      },
      &item);
}

/// The stream ended by itself. Tell the control point, so a phone's transport
/// controls stop showing a track that finished minutes ago.
void onEndOfStream(AudioPlayer &) {
  audioFlowing = false;
  statusLocked([](void *) { status.state = NET_AUDIO_IDLE; }, nullptr);
  playbackCompleted = true;  // the DLNA task passes it on; see dlnaTask()
}

// ------------------------------------------------------------------ tasks ---

void setState(NetAudioState state, const char *error) {
  struct Arg {
    NetAudioState state;
    const char *error;
  } arg{state, error};
  statusLocked(
      [](void *raw) {
        auto *a = (Arg *)raw;
        status.state = a->state;
        if (a->error != nullptr) {
          strlcpy(status.error, a->error, sizeof(status.error));
        } else if (a->state != NET_AUDIO_ERROR) {
          status.error[0] = 0;
        }
      },
      &arg);
}

void applyCommand(const Command &cmd) {
  switch (cmd.op) {
    case CMD_PLAY_URL: {
      setState(NET_AUDIO_OPENING, nullptr);
      struct Arg {
        const char *url;
        const char *origin;
      } arg{cmd.url, cmd.origin};
      statusLocked(
          [](void *raw) {
            auto *a = (Arg *)raw;
            strlcpy(status.url, a->url, sizeof(status.url));
            strlcpy(status.origin, a->origin, sizeof(status.origin));
            // A new URL is a new track. Whatever the last one was called, it is
            // not this; clear it rather than leaving it under the wrong stream
            // until metadata happens to arrive.
            status.title[0] = status.artist[0] = 0;
            status.sample_rate = 0;
          },
          &arg);

      // The source keeps every URL it has been handed, which for a speaker that
      // has been streaming all day is a slow heap leak. Only the current one
      // matters here -- there is no playlist -- so drop the rest.
      source.clear();
      if (player.setPath(cmd.url)) {
        player.setActive(true);
        setState(NET_AUDIO_PLAYING, nullptr);
      } else {
        audioFlowing = false;
        setState(NET_AUDIO_ERROR, "Could not open that URL");
      }
      break;
    }
    case CMD_PLAY:
      if (status.url[0]) {
        player.setActive(true);
        setState(NET_AUDIO_PLAYING, nullptr);
      }
      break;
    case CMD_PAUSE:
      player.setActive(false);
      audioFlowing = false;
      setState(NET_AUDIO_PAUSED, nullptr);
      break;
    case CMD_STOP:
      player.stop();
      urlStream.end();
      source.clear();
      audioFlowing = false;
      statusLocked(
          [](void *) {
            status.url[0] = status.title[0] = status.artist[0] = 0;
            status.origin[0] = 0;
            status.sample_rate = 0;
          },
          nullptr);
      setState(NET_AUDIO_IDLE, nullptr);
      break;
    case CMD_VOLUME: {
      const uint8_t vol = cmd.value;
      player.setVolume((float)vol / 127.0f);
      uint8_t copy = vol;
      statusLocked([](void *raw) { status.volume = *(uint8_t *)raw; }, &copy);
      break;
    }
  }
}

/*
 * The only task that ever touches the player.
 *
 * copy() moves one buffer from the decoder to the DAC and returns how much it
 * moved. Zero means there is nothing to do -- paused, stopped, or the network
 * has not delivered yet -- and in that case the task sleeps rather than
 * spinning, because this runs at a priority above the UI and would otherwise
 * starve the display for no benefit.
 *
 * Priority 2 and core 1: the same side of the chip the A2DP sink uses in the
 * other modes, and above the UI task at 1, so a slow I2C frame can never stall
 * the audio. Wi-Fi's own tasks sit above this on core 0.
 */
void audioTask(void *) {
  Command cmd;
  for (;;) {
    while (xQueueReceive(commands, &cmd, 0) == pdTRUE) applyCommand(cmd);

    const size_t moved = player.copy();
    if (moved == 0) {
      // Nothing moved. If that has been true for a while, the stream is over or
      // stalled and the "live" indicators should stop saying otherwise.
      if (audioFlowing && millis() - lastAudioAt > AUDIO_IDLE_MS) {
        audioFlowing = false;
      }
      // Block on the queue rather than delay(): a command then wakes the task
      // immediately instead of waiting out the sleep.
      if (xQueueReceive(commands, &cmd, pdMS_TO_TICKS(20)) == pdTRUE) {
        applyCommand(cmd);
      }
    }
  }
}

/// SSDP, the description documents and the SOAP endpoints. Its own task because
/// the renderer's HTTP server blocks on accept(), and because a control point
/// polling GetPositionInfo has nothing to do with the audio path.
void dlnaTask(void *) {
  for (;;) {
    if (playbackCompleted) {
      playbackCompleted = false;
      renderer.setPlaybackCompleted();
    }
    renderer.loop();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

bool post(const Command &cmd) {
  if (commands == nullptr) return false;
  return xQueueSend(commands, &cmd, pdMS_TO_TICKS(20)) == pdTRUE;
}

}  // namespace

// ------------------------------------------------------------------- API ----

bool net_audio_begin(const char *device_name, uint16_t http_port) {
  if (running) return true;

  statusLock = xSemaphoreCreateMutex();
  commands = xQueueCreate(4, sizeof(Command));
  if (statusLock == nullptr || commands == nullptr) {
    Serial.println("[net] out of memory starting the network player");
    return false;
  }

  /*
   * Check there is room before starting anything.
   *
   * Between them the decoder, the HTTP client, the copy buffers and two task
   * stacks want a good chunk of what is left after Wi-Fi and BLE have taken
   * theirs. Finding that out by running into a failed allocation deep inside a
   * decoder means a panic and a reboot loop, which tells the user nothing. Ask
   * first, and if the answer is no, say so and leave the rest of the firmware
   * running -- the dashboard is still worth having.
   */
  constexpr uint32_t NET_AUDIO_MIN_HEAP = 48000;
  const uint32_t heap = ESP.getFreeHeap();
  if (heap < NET_AUDIO_MIN_HEAP) {
    Serial.printf("[net] only %u bytes of heap free, need about %u. The network "
                  "player is not starting; the dashboard still works.\n",
                  (unsigned)heap, (unsigned)NET_AUDIO_MIN_HEAP);
    return false;
  }

  memset(&status, 0, sizeof(status));
  status.state = NET_AUDIO_IDLE;
  status.volume = 100;  // ~79%, a sane level for a source with no slider yet

  // The decoder is chosen from the Content-Type the HTTP response carried,
  // which URLStream keeps for exactly this purpose. Three formats covers
  // internet radio (MP3 and AAC) and anything a DLNA server transcodes to WAV;
  // FLAC and Opus decoders exist in AudioTools but do not fit the heap budget
  // next to Wi-Fi, BLE and the web server.
  multiDecoder.setMimeSource(urlStream);
  multiDecoder.addDecoder(decMp3, "audio/mpeg");
  multiDecoder.addDecoder(decAac, "audio/aac");
  multiDecoder.addDecoder(decWav, "audio/wav");

  player.setAutoNext(false);   // there is no playlist; one URL at a time
  player.setVolume((float)status.volume / 127.0f);
  player.setOnEOFCallback(onEndOfStream);
  player.setMetadataCallback(onMetadata);
  // Without this a pause leaves the DMA buffers repeating their last contents,
  // which on this DAC is an audible buzz rather than silence.
  player.setSilenceOnInactive(true);

  renderer.setFriendlyName(device_name);
  renderer.setManufacturer(APP_NAME);
  renderer.setModelName(APP_NAME);
  renderer.setBaseURL(WiFi.localIP(), http_port);
  renderer.setMediaEventHandler(onMediaEvent);
  dlnaSocket = WiFiServer(http_port);

  rendererUp = renderer.begin();
  if (!rendererUp) {
    Serial.println("[net] DLNA renderer failed to start; URL playback still "
                   "works from the dashboard");
  }
  status.renderer_up = rendererUp;

  // 8 KB for the audio task: the Helix decoders keep their working buffers on
  // the heap, but the copier's stack frames plus the HTTP client underneath
  // URLStream do not fit in the 4 KB an Arduino task gets by default.
  BaseType_t ok = xTaskCreatePinnedToCore(audioTask, "net_audio", 8192, nullptr,
                                          2, nullptr, 1);
  if (ok != pdPASS) {
    Serial.println("[net] could not start the audio task");
    return false;
  }
  if (rendererUp &&
      xTaskCreatePinnedToCore(dlnaTask, "dlna", 8192, nullptr, 1, nullptr, 0) !=
          pdPASS) {
    Serial.println("[net] could not start the DLNA task");
    rendererUp = false;
    status.renderer_up = false;
  }

  running = true;
  Serial.printf("[net] network audio ready as \"%s\"", device_name);
  if (rendererUp) {
    Serial.printf(" | DLNA renderer on http://%s:%u/",
                  WiFi.localIP().toString().c_str(), (unsigned)http_port);
  }
  Serial.printf(" | heap %u\n", (unsigned)ESP.getFreeHeap());
  return true;
}

bool net_audio_running() { return running; }

void net_audio_snapshot(NetAudioStatus *out) {
  if (out == nullptr) return;
  if (statusLock == nullptr) {
    memset(out, 0, sizeof(*out));
    return;
  }
  if (xSemaphoreTake(statusLock, pdMS_TO_TICKS(50)) == pdTRUE) {
    memcpy(out, &status, sizeof(*out));
    xSemaphoreGive(statusLock);
  } else {
    memcpy(out, &status, sizeof(*out));  // stale beats nothing
  }
  out->renderer_up = rendererUp;
}

bool net_audio_active() { return running && audioFlowing; }

NetAudioState net_audio_state() { return status.state; }

bool net_audio_play_url(const char *url, const char *origin) {
  if (!running || url == nullptr) return false;
  const size_t len = strlen(url);
  if (len == 0 || len > NET_URL_MAX) return false;
  Command cmd{};
  cmd.op = CMD_PLAY_URL;
  strlcpy(cmd.url, url, sizeof(cmd.url));
  strlcpy(cmd.origin, origin != nullptr ? origin : "url", sizeof(cmd.origin));
  return post(cmd);
}

void net_audio_play() {
  Command cmd{};
  cmd.op = CMD_PLAY;
  post(cmd);
}

void net_audio_pause() {
  Command cmd{};
  cmd.op = CMD_PAUSE;
  post(cmd);
}

void net_audio_stop() {
  Command cmd{};
  cmd.op = CMD_STOP;
  post(cmd);
}

void net_audio_set_volume(uint8_t volume) {
  Command cmd{};
  cmd.op = CMD_VOLUME;
  cmd.value = volume > 127 ? 127 : volume;
  post(cmd);
}

uint8_t net_audio_volume() { return status.volume; }

/*
 * Publishes into player_state, once per Arduino loop.
 *
 * Everything above runs on the audio and DLNA tasks and only ever writes to
 * `status`. This is the one place that writes player_state, which is what keeps
 * its seqlock honest -- see the note at the top of player_state.h. It also
 * means the OLED sees a change at most one loop late, which at 10 ms is not a
 * delay anybody can perceive.
 */
void net_audio_loop() {
  if (!running) return;

  static NetAudioState lastState = NET_AUDIO_IDLE;
  static char lastTitle[sizeof(status.title)];
  static char lastArtist[sizeof(status.artist)];
  static uint8_t lastVolume = 0xFF;
  static uint32_t lastRate;
  static bool lastFlowing;

  NetAudioStatus s;
  net_audio_snapshot(&s);

  const bool loaded = s.state != NET_AUDIO_IDLE && s.state != NET_AUDIO_ERROR;
  if (s.state != lastState) {
    lastState = s.state;
    // "Connected" means a source has given us something to play. The screens
    // and the dashboard both key off it the same way they do for a paired
    // phone, so a DLNA controller that has pushed a track reads as connected.
    ps_set_net_connection(loaded, loaded ? (s.origin[0] == 'd' ? "DLNA" : "Network")
                                         : nullptr);
    switch (s.state) {
      case NET_AUDIO_PLAYING: ps_set_playback(PS_PLAYING); break;
      case NET_AUDIO_PAUSED: ps_set_playback(PS_PAUSED); break;
      case NET_AUDIO_OPENING: ps_set_playback(PS_SEEKING); break;
      default: ps_set_playback(PS_STOPPED); break;
    }
  }

  if (strcmp(s.title, lastTitle) != 0 || strcmp(s.artist, lastArtist) != 0) {
    strlcpy(lastTitle, s.title, sizeof(lastTitle));
    strlcpy(lastArtist, s.artist, sizeof(lastArtist));
    ps_set_track_text(s.title, s.artist, nullptr);
  }
  if (s.volume != lastVolume) {
    lastVolume = s.volume;
    ps_set_volume(s.volume);
  }
  if (s.sample_rate != lastRate) {
    lastRate = s.sample_rate;
    if (s.sample_rate) ps_set_sample_rate((uint16_t)s.sample_rate);
  }
  const bool flowing = audioFlowing;
  if (flowing != lastFlowing) {
    lastFlowing = flowing;
    ps_set_streaming(flowing);
  }
}
