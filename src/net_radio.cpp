#include "app_config.h"
#include "net_radio.h"

#include <Arduino.h>
#include <NetworkClientSecure.h>
#include <new>
#include <Preferences.h>
#include <WiFi.h>
#include <string.h>

#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecAACHelix.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "AudioTools/Communication/AudioHttp.h"

#include "audio_eq.h"
#include "audio_probe.h"
#include "management.h"
#include "df_player.h"
#include "player_state.h"
#include "voice.h"

using namespace audio_tools;

namespace {

/*
 * The jitter buffer, and when it exists.
 *
 * 20 kB holds about a second and a quarter of a 128 kbps stream -- enough to
 * ride out the pauses a domestic connection produces, not enough to hide a
 * genuinely inadequate one, which is the right place for that line to sit.
 *
 * It is allocated when a stream starts and freed when it ends, and that is not
 * a detail. This buffer, the socket chunk and the decoder feed together are
 * over 20 kB, and holding them from boot on a speaker that may never play a
 * station costs the firmware updater its TLS handshake: the check in
 * management.cpp wants 60 kB free and a 34 kB contiguous block, and a 24 kB
 * allocation made early sits in the middle of the heap splitting exactly the
 * block the handshake needs. Idle radio now costs nothing at all.
 */
const size_t RING_BYTES = 20 * 1024;

/// How full the ring has to be before decoding starts, as a percentage. Filling
/// it completely would make every station take twice as long to start for no
/// benefit; below about half, the first underrun arrives before the buffer has
/// found its level.
const uint8_t PREBUFFER_PERCENT = 55;

/// How much is read from the socket in one go: two MTUs. Larger reads made no
/// measurable difference to the buffer level and every byte of this is resident
/// for the whole stream.
const size_t READ_CHUNK = 1460 * 2;

/// How much encoded audio is handed to the decoder at a time. Small enough that
/// the socket gets looked at often, large enough to amortise the call.
const size_t DECODE_CHUNK = 1024;

/*
 * The reconnect backoff.
 *
 * Doubling from two seconds to a minute and then staying there, forever. The
 * ceiling matters more than the curve: a station that is down for an hour must
 * not leave the speaker silent once it comes back, and nobody is watching, so
 * the cost of trying again every minute for an hour is nothing at all.
 */
const uint32_t RECONNECT_MIN_MS = 2000;
const uint32_t RECONNECT_MAX_MS = 60000;

/// How long to wait for the reply headers before calling a station unreachable.
const uint32_t CONNECT_TIMEOUT_MS = 12000;

/*
 * How much heap a stream needs before it is allowed to try.
 *
 * This is not a safety margin, it is a hard lesson. Opening a stream allocates
 * a socket, a read buffer, the 22 kB arena and a ~30 kB decoder -- and the
 * layers underneath allocate too, invisibly: esp_vfs_select() creates a
 * semaphore per call, and when that allocation fails the IDF does not return an
 * error, it asserts and panics. A firmware that runs out of heap inside the
 * HTTP client does not fail to play a station, it reboots -- and if the station
 * was set to resume at boot, it reboots again.
 *
 * So the check happens here, where it can be reported, rather than there, where
 * it is fatal. The numbers are what a connect plus a decoder actually needs
 * with room for the network stack to breathe underneath it.
 */
const uint32_t STREAM_HEAP_FLOOR = 70000;
const uint32_t STREAM_BLOCK_FLOOR = 26000;

/*
 * What https costs on top, and why it usually does not fit.
 *
 * Measured on this board, for a 128 kbps MP3 stream:
 *
 *   ring + socket chunk + decode feed   23 kB   one allocation, this file
 *   libhelix MP3 state                 ~29 kB   eight allocations, its own
 *   helix frame + PCM buffers            7 kB
 *   HTTP, lwIP, socket                  ~8 kB
 *                                      ------
 *   plain http                         ~67 kB
 *   TLS record buffers + handshake     ~45 kB   mbedtls, 16 kB in + 16 kB out
 *                                      ------
 *   https                             ~112 kB
 *
 * A speaker with the dashboard up has about 108 kB free. The first column fits
 * with room to spare; the second does not fit at all, and what "does not fit"
 * looks like from inside mbedtls is an allocation failure that asserts rather
 * than returns -- so this is checked here, in advance, where it can be a
 * sentence instead of a panic.
 *
 * The 16 kB record buffers are an IDF sdkconfig option, not something the
 * application can shrink: the Arduino core arrives precompiled.
 */
const uint32_t STREAM_TLS_EXTRA = 45000;

/*
 * The autostart sentinel.
 *
 * A station that is set to resume at boot and crashes while connecting is a
 * speaker that never finishes booting, and the dashboard that could switch it
 * off is on the other side of the crash. So the attempt is written to flash
 * before it is made and cleared once the stream is playing; a boot that finds
 * the flag still set knows the last one did not survive, and disarms autostart
 * rather than trying again.
 *
 * This is the same shape as the radio-mode boot sentinel in management.cpp, and
 * for exactly the same reason: a setting the owner can change from a web page
 * must not be able to make the web page unreachable.
 */
const char *AUTOSTART_TRY_KEY = "autotry";

/// A stream that has delivered nothing for this long is dead, whatever the
/// socket believes. Icecast servers routinely leave a half-open connection
/// behind when they restart, and without this the speaker sits on it silently.
const uint32_t STALL_TIMEOUT_MS = 10000;

Preferences prefs;

SemaphoreHandle_t lock;
RadioStatus status;

/// What the task is being asked to do, as opposed to what it is doing. The two
/// are separate so that a request can be made from any task without waiting for
/// the current connection to notice.
struct Request {
  volatile bool changed;
  volatile bool play;
  int8_t station;
  char url[RADIO_URL_MAX];
  char name[RADIO_NAME_MAX];
};
Request request;

TaskHandle_t task;
bool running;
uint8_t volume127 = 90;
bool autostart;

/*
 * Deferred flash write for the volume.
 *
 * The alarm's fade-up and the sleep timer's fade-out both walk this value in
 * small steps, and writing each one to NVS would be tens of thousands of
 * erase cycles a year on one key for a level nobody chose. So a change marks
 * the setting dirty and net_radio_loop() writes it once the moving has
 * stopped -- the same arrangement management.cpp uses for the lighting
 * sliders, and for the same reason.
 */
bool volumeDirty;
uint32_t volumeDirtyAt;
const uint32_t VOLUME_PERSIST_QUIET_MS = 4000;

I2SStream *i2s;

/*
 * One allocation for all three working buffers.
 *
 * The ring, the socket chunk and the decoder feed have exactly the same
 * lifetime -- a stream -- so making them one block rather than three means one
 * malloc, one free, and no chance of leaving the heap with three
 * differently-sized holes in it after every station change.
 */
const size_t STREAM_ARENA = RING_BYTES + READ_CHUNK + DECODE_CHUNK;
uint8_t *arena;
uint8_t *ring;    // arena
uint8_t *chunk;   // arena + RING_BYTES
uint8_t *feed;    // arena + RING_BYTES + READ_CHUNK
size_t ringHead, ringTail, ringUsed;

/// The favourites. On the heap and not in .bss because 2.4 kB of DRAM that only
/// two of the three radio modes can ever use is 2.4 kB Bluedroid does not get
/// in the third.
RadioStation *stations;
uint8_t stationCount;

bool arenaAcquire() {
  if (arena) return true;
  arena = (uint8_t *)malloc(STREAM_ARENA);
  if (!arena) return false;
  ring = arena;
  chunk = arena + RING_BYTES;
  feed = chunk + READ_CHUNK;
  return true;
}

void arenaRelease() {
  free(arena);
  arena = nullptr;
  ring = chunk = feed = nullptr;
}

/// Set by the metadata callback, which the HTTP reader calls from inside
/// readBytes() -- so on the radio task, but at a point where taking the status
/// mutex would nest a lock inside the read path. Copied across at the top of
/// the next loop instead.
volatile bool metaDirty;
char metaTitle[RADIO_TEXT_MAX];
char metaName[RADIO_NAME_MAX];
char metaGenre[RADIO_TEXT_MAX];

void statusLock() {
  if (lock) xSemaphoreTake(lock, portMAX_DELAY);
}
void statusUnlock() {
  if (lock) xSemaphoreGive(lock);
}

void copyString(char *dest, size_t size, const char *src) {
  if (!dest || size == 0) return;
  if (!src) {
    dest[0] = 0;
    return;
  }
  snprintf(dest, size, "%s", src);
}

/*
 * Strips the control characters and trims a station's text.
 *
 * Stations send whatever their playout system had in a database field, which
 * includes tabs, stray CRs, and on a bad day a fragment of HTML. None of it
 * survives the OLED's font, and a control character in the middle of a JSON
 * string is a dashboard that fails to parse rather than a display glitch.
 */
void sanitize(char *text) {
  if (!text) return;
  char *out = text;
  for (char *in = text; *in; ++in) {
    const unsigned char c = (unsigned char)*in;
    if (c >= 0x20 || c == 0) *out++ = *in;
    else if (out != text && out[-1] != ' ') *out++ = ' ';
  }
  *out = 0;
  while (out > text && out[-1] == ' ') *--out = 0;
}

bool urlLooksPlayable(const char *url) {
  if (!url) return false;
  return strncasecmp(url, "http://", 7) == 0 || strncasecmp(url, "https://", 8) == 0;
}

bool urlIsSecure(const char *url) {
  return url && strncasecmp(url, "https://", 8) == 0;
}

/// The host part of a URL, for when a station sends no name of its own.
void hostFromUrl(const char *url, char *out, size_t size) {
  if (!out || size == 0) return;
  out[0] = 0;
  if (!url) return;
  const char *start = strstr(url, "://");
  start = start ? start + 3 : url;
  const char *at = strchr(start, '@');
  if (at) start = at + 1;
  size_t n = 0;
  while (start[n] && start[n] != '/' && start[n] != ':' && n + 1 < size) n++;
  memcpy(out, start, n);
  out[n] = 0;
}

// ------------------------------------------------------------- persistence --

void loadStations() {
  stationCount = 0;
  if (!prefs.begin("radio", false)) return;

  volume127 = (uint8_t)prefs.getUChar("vol", 90);
  if (volume127 > 127) volume127 = 127;
  autostart = prefs.getBool("auto", false);

  const size_t want = sizeof(RadioStation) * RADIO_MAX_STATIONS;
  // isKey() first: getBytesLength() on a key that has never been written logs
  // an error from inside Preferences, and a first boot is not an error.
  const size_t have = prefs.isKey("list") ? prefs.getBytesLength("list") : 0;
  if (have > 0 && have <= want) {
    prefs.getBytes("list", stations, have);
    stationCount = (uint8_t)prefs.getUChar("count", 0);
    if (stationCount > RADIO_MAX_STATIONS) stationCount = RADIO_MAX_STATIONS;
    // A blob written by a firmware with a different RADIO_MAX_STATIONS, or a
    // half-written one, must not be trusted past what it actually contains.
    const uint8_t fits = (uint8_t)(have / sizeof(RadioStation));
    if (stationCount > fits) stationCount = fits;
    for (uint8_t i = 0; i < stationCount; i++) {
      stations[i].name[RADIO_NAME_MAX - 1] = 0;
      stations[i].url[RADIO_URL_MAX - 1] = 0;
    }
  }

  if (stationCount == 0) {
    /*
     * A first boot with an empty list is a dashboard with nothing to press, and
     * "add a station" is a much harder first step than it looks -- finding a
     * working stream URL means knowing that the page you are looking at is not
     * one. So the list starts with four that are public, long-lived and
     * plain-HTTP MP3, chosen to span the range: two music, one talk, one
     * classical. They are ordinary favourites and can be edited or deleted like
     * any other.
     */
    struct Seed {
      const char *name;
      const char *url;
    };
    static const Seed seeds[] = {
        {"BBC Radio 6 Music",
         "http://stream.live.vc.bbcmedia.co.uk/bbc_6music"},
        {"FIP", "http://icecast.radiofrance.fr/fip-midfi.mp3"},
        {"SomaFM Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3"},
        {"Venice Classic Radio", "http://174.36.206.197:8000/stream"},
    };
    for (const Seed &seed : seeds) {
      copyString(stations[stationCount].name, RADIO_NAME_MAX, seed.name);
      copyString(stations[stationCount].url, RADIO_URL_MAX, seed.url);
      stationCount++;
    }
  }
}

void storeStations() {
  if (!prefs.isKey("vol") && stationCount == 0) return;
  prefs.putBytes("list", stations, sizeof(RadioStation) * stationCount);
  prefs.putUChar("count", stationCount);
}

// ------------------------------------------------------------------ output --

/*
 * Where decoded audio goes.
 *
 * Between the decoder and the DAC there are four jobs, and this is the only
 * place all four can be done at once:
 *
 *   mono to stereo    a good number of talk stations are mono, and the I2S
 *                     channel is not. Duplicating here rather than reopening
 *                     the channel keeps the DAC's clocks alone.
 *   the equaliser     the same five bands the Bluetooth path uses, so a curve
 *                     dialled in for one source is the curve for all of them.
 *   announcements     mixed in with the music ducked underneath, exactly as in
 *                     the A2DP path.
 *   volume            there is no AVRCP here to do it, so it is done in
 *                     software, before the soft knee inside the equaliser.
 *
 * Everything is int16 and in place, in a buffer the decoder owns. This runs on
 * the radio task and must not block for longer than the I2S write it ends with.
 */
class RadioOutput : public AudioOutput {
 public:
  size_t write(const uint8_t *data, size_t len) override {
    if (!i2s || !data || len < 4) return len;

    const size_t samples = len / sizeof(int16_t);
    int16_t *pcm = (int16_t *)data;

    if (channels == 1) {
      // Expanded into our own buffer: the decoder's is only as big as the mono
      // data, and writing past it would be a heap corruption that shows up
      // somewhere else entirely.
      size_t done = 0;
      while (done < samples) {
        const size_t take = min(samples - done, STEREO_FRAMES);
        for (size_t i = 0; i < take; i++) {
          stereo[2 * i] = pcm[done + i];
          stereo[2 * i + 1] = pcm[done + i];
        }
        emit(stereo, take);
        done += take;
      }
    } else {
      emit(pcm, samples / 2);
    }
    return len;
  }

  void setAudioInfo(AudioInfo info) override {
    AudioOutput::setAudioInfo(info);
    if (info.sample_rate == 0 || info.channels == 0) return;
    channels = info.channels;

    // The I2S channel is opened at 44.1 kHz in setup() and the stream may be at
    // any of the half-dozen rates a station might use. Retuning the channel
    // rather than resampling is both cheaper and better; it is safe here
    // because in the modes that have a radio nothing else is writing to it.
    AudioInfo out = info;
    out.channels = 2;
    if (out.sample_rate != lastRate) {
      lastRate = out.sample_rate;
      i2s->setAudioInfo(out);
      audio_eq_set_sample_rate(out.sample_rate);
      voice_set_sample_rate(out.sample_rate);
    }

    statusLock();
    status.sampleRate = info.sample_rate;
    status.channels = info.channels;
    statusUnlock();
  }

  void reset() {
    channels = 2;
    lastRate = 0;
  }

 private:
  /// Frames of the mono-to-stereo scratch buffer. Small on purpose: this is
  /// resident for the life of the firmware, and emit() loops over it anyway.
  static const size_t STEREO_FRAMES = 128;
  int16_t stereo[STEREO_FRAMES * 2];
  uint16_t channels = 2;
  uint32_t lastRate = 0;

  void emit(int16_t *frames, size_t count) {
    // Volume first, so the equaliser's soft knee is the last thing in the chain
    // and a boosted band cannot clip above a level the owner has already turned
    // down.
    if (volume127 < 127) {
      const int32_t gain = volume127;
      for (size_t i = 0; i < count * 2; i++) {
        frames[i] = (int16_t)(((int32_t)frames[i] * gain) >> 7);
      }
    }
    audio_eq_process(frames, count);
    voice_mix(frames, count);
    audio_probe_feed((const Frame *)frames, (uint16_t)count);
    i2s->write((const uint8_t *)frames, count * 4);
  }
};

RadioOutput output;

// --------------------------------------------------------------- ring buffer --

void ringClear() {
  ringHead = ringTail = ringUsed = 0;
}

size_t ringWrite(const uint8_t *data, size_t len) {
  size_t written = 0;
  while (written < len && ringUsed < RING_BYTES) {
    const size_t space = RING_BYTES - ringUsed;
    size_t run = min(len - written, space);
    run = min(run, RING_BYTES - ringHead);
    memcpy(ring + ringHead, data + written, run);
    ringHead = (ringHead + run) % RING_BYTES;
    ringUsed += run;
    written += run;
  }
  return written;
}

size_t ringRead(uint8_t *out, size_t len) {
  size_t taken = 0;
  while (taken < len && ringUsed > 0) {
    size_t run = min(len - taken, ringUsed);
    run = min(run, RING_BYTES - ringTail);
    memcpy(out + taken, ring + ringTail, run);
    ringTail = (ringTail + run) % RING_BYTES;
    ringUsed -= run;
    taken += run;
  }
  return taken;
}

// --------------------------------------------------------------- metadata --

/*
 * The ICY callback.
 *
 * Called from inside readBytes() on the radio task, so it does the least
 * possible: copy into a scratch buffer and set a flag. The status mutex is
 * taken by the loop that owns the read, not from underneath it.
 */
void onMetadata(MetaDataType type, const char *str, int len) {
  if (!str || len <= 0) return;
  char *dest = nullptr;
  size_t size = 0;
  switch (type) {
    case Title:
      dest = metaTitle;
      size = sizeof(metaTitle);
      break;
    case Name:
      dest = metaName;
      size = sizeof(metaName);
      break;
    case Genre:
      dest = metaGenre;
      size = sizeof(metaGenre);
      break;
    default:
      return;
  }
  const size_t n = min((size_t)len, size - 1);
  memcpy(dest, str, n);
  dest[n] = 0;
  sanitize(dest);
  metaDirty = true;
}

void adoptMetadata() {
  if (!metaDirty) return;
  metaDirty = false;
  statusLock();
  if (metaTitle[0]) copyString(status.title, sizeof(status.title), metaTitle);
  if (metaName[0]) copyString(status.name, sizeof(status.name), metaName);
  if (metaGenre[0]) copyString(status.genre, sizeof(status.genre), metaGenre);
  statusUnlock();
}

// ------------------------------------------------------------------- task --

void setState(RadioState state, const char *error = nullptr) {
  statusLock();
  if (status.state != state) status.stateSince = millis();
  status.state = state;
  if (error) copyString(status.error, sizeof(status.error), error);
  else if (state != RADIO_ERROR) status.error[0] = 0;
  statusUnlock();
}

/// Picks a decoder from what the server said it was sending, falling back to
/// the file extension and finally to MP3 -- which is what an Icecast server
/// that reports "application/octet-stream" is almost always serving.
/// Case-insensitive substring search. newlib has no strcasestr, and the two
/// needles here are short enough that the naive loop is the right answer.
bool containsCI(const char *haystack, const char *needle) {
  if (!haystack || !needle) return false;
  const size_t n = strlen(needle);
  for (const char *p = haystack; *p; ++p) {
    if (strncasecmp(p, needle, n) == 0) return true;
  }
  return false;
}

bool wantsAac(const char *contentType, const char *url) {
  if (contentType && (containsCI(contentType, "aac") || containsCI(contentType, "mp4a"))) {
    return true;
  }
  if (url) {
    const char *dot = strrchr(url, '.');
    if (dot && (strncasecmp(dot, ".aac", 4) == 0 || strncasecmp(dot, ".m4a", 4) == 0)) {
      return true;
    }
  }
  return false;
}

/*
 * One connection, from the socket opening to the stream ending.
 *
 * Returns when the stream stops for any reason. `stopped` distinguishes "the
 * owner pressed stop", which must not reconnect, from every other ending, which
 * must.
 */
void runStream(const char *url, bool *stopped) {
  *stopped = false;

  /*
   * Not while the updater holds TLS. This is a wait, not a failure -- the
   * caller's backoff brings us back in a couple of seconds, and by then the
   * check has finished and given its forty kilobytes back.
   */
  if (management_update_busy()) {
    setState(RADIO_RECONNECTING, "Waiting for the update check to finish");
    LOGLN("[radio] deferring: the firmware update check has the network");
    vTaskDelay(pdMS_TO_TICKS(2000));
    return;
  }

  /*
   * Refuse before allocating anything, rather than dying inside the HTTP
   * client. The message carries the actual numbers because "not enough memory"
   * on its own tells nobody what to do about it.
   */
  const bool secure = urlIsSecure(url);
  const uint32_t needed = STREAM_HEAP_FLOOR + (secure ? STREAM_TLS_EXTRA : 0);
  const uint32_t heap = ESP.getFreeHeap();
  const uint32_t block = ESP.getMaxAllocHeap();
  if (heap < needed || block < STREAM_BLOCK_FLOOR) {
    char why[RADIO_TEXT_MAX];
    if (secure && heap >= STREAM_HEAP_FLOOR) {
      // It would have fitted without the encryption, which makes the fix a
      // one-character edit to the address rather than a lost cause.
      snprintf(why, sizeof(why), "Not enough memory for https - try http://");
    } else {
      snprintf(why, sizeof(why), "Low memory: %u free, %u block", (unsigned)heap,
               (unsigned)block);
    }
    setState(RADIO_ERROR, why);
    LOGF("[radio] refusing to start a %s stream: %u B free, %u B largest block "
         "(needs %u / %u)\n", secure ? "https" : "http", (unsigned)heap,
         (unsigned)block, (unsigned)needed, (unsigned)STREAM_BLOCK_FLOOR);
    if (secure && heap >= STREAM_HEAP_FLOOR) {
      LOGLN("[radio] the same station over plain http would fit. TLS needs "
            "another ~45 kB for its record buffers, and this chip does not "
            "have it while the dashboard is up.");
    }
    return;
  }

  NetworkClientSecure *tls = nullptr;
  ICYStream *stream = new (std::nothrow) ICYStream(READ_CHUNK);
  if (!stream) {
    setState(RADIO_ERROR, "Out of memory opening the stream");
    return;
  }
  if (secure) {
    tls = new (std::nothrow) NetworkClientSecure();
    if (!tls) {
      delete stream;
      setState(RADIO_ERROR, "Out of memory for the TLS connection");
      return;
    }
    /*
     * No certificate verification for a radio stream, deliberately.
     *
     * The firmware updater checks the full Mozilla root bundle and must: it
     * writes executable code into flash, and a stream that can be substituted
     * there owns the device. A radio station is public audio, unauthenticated,
     * that anybody can fetch -- the worst a successful attacker achieves is
     * that you hear the wrong music. Verifying it costs a chain walk and the
     * allocations that go with it, at the exact moment this chip has none to
     * spare.
     *
     * That is a real trade and it is worth writing down rather than leaving as
     * a missing line: this connection is encrypted but not authenticated.
     */
    tls->setInsecure();
    stream->setClient(*tls);
  }

  metaTitle[0] = metaName[0] = metaGenre[0] = 0;
  metaDirty = false;
  stream->setMetadataCallback(onMetadata);
  // Shoutcast servers answer a plain GET with an ICY/1.0 status line rather
  // than HTTP/1.0, and some of them only send the metadata this asks for.
  stream->addRequestHeader("User-Agent", "esp32-blue-spk");

  setState(RADIO_CONNECTING);
  const uint32_t startedAt = millis();
  const bool opened = stream->begin(url, "audio/mpeg");

  const uint32_t spent = millis() - startedAt;
  const uint32_t remaining = spent >= CONNECT_TIMEOUT_MS ? 0 : CONNECT_TIMEOUT_MS - spent;
  if (!opened || !stream->waitForData((int)remaining)) {
    stream->end();
    delete stream;
    delete tls;
    setState(RADIO_ERROR, opened ? "The station accepted the connection but "
                                   "sent nothing"
                                 : "Could not reach the station");
    return;
  }

  const char *contentType = stream->getReplyHeader("content-type");
  const char *bitrateText = stream->getReplyHeader("icy-br");

  statusLock();
  status.bitrate = bitrateText ? (uint32_t)atoi(bitrateText) : 0;
  status.underruns = 0;
  status.bytes = 0;
  status.sampleRate = 0;
  status.channels = 0;
  const bool aac = wantsAac(contentType, url);
  copyString(status.codec, sizeof(status.codec), aac ? "AAC" : "MP3");
  statusUnlock();

  /*
   * The working buffers, claimed only now.
   *
   * After the handshake rather than before it: an https connection's peak heap
   * use is during the certificate walk, and holding 22 kB across that is the
   * difference between a station that plays and one that reports being out of
   * memory. The socket has not been read from yet, so nothing is lost by
   * waiting.
   */
  if (!arenaAcquire()) {
    stream->end();
    delete stream;
    delete tls;
    setState(RADIO_ERROR, "Not enough memory left for the stream buffer");
    return;
  }

  // Built here and destroyed at the end of the stream, so the ~30 kB the codec
  // needs is only held while something is playing. In a mode where the web
  // server may be asked to do TLS at any moment, that is not a small thing.
  AudioDecoder *codec = aac ? (AudioDecoder *)new (std::nothrow) AACDecoderHelix()
                            : (AudioDecoder *)new (std::nothrow) MP3DecoderHelix();
  EncodedAudioStream *decoder =
      codec ? new (std::nothrow) EncodedAudioStream(&output, codec) : nullptr;
  if (!decoder) {
    delete codec;
    arenaRelease();
    stream->end();
    delete stream;
    delete tls;
    setState(RADIO_ERROR, "Not enough memory left to start the decoder");
    return;
  }
  output.reset();
  decoder->begin();

  ringClear();
  setState(RADIO_BUFFERING);
  adoptMetadata();

  const size_t prebuffer = RING_BYTES * PREBUFFER_PERCENT / 100;
  bool decoding = false;
  uint32_t lastByteAt = millis();
  uint32_t lastPercentAt = 0;

  while (true) {
    // A new request, or a stop, outranks everything.
    if (request.changed) break;
    if (WiFi.status() != WL_CONNECTED) {
      setState(RADIO_ERROR, "Wi-Fi went away");
      break;
    }

    // Fill.
    const size_t space = RING_BYTES - ringUsed;
    if (space >= 512 && stream->available() > 0) {
      const size_t want = min(space, READ_CHUNK);
      const size_t got = stream->readBytes(chunk, want);
      if (got > 0) {
        ringWrite(chunk, got);
        lastByteAt = millis();
        statusLock();
        status.bytes += got;
        statusUnlock();
      }
    }

    adoptMetadata();

    if (!*stream) {
      setState(RADIO_ERROR, "The station closed the connection");
      break;
    }
    if ((uint32_t)(millis() - lastByteAt) > STALL_TIMEOUT_MS) {
      setState(RADIO_ERROR, "The stream stopped sending");
      break;
    }

    // Report the buffer level about ten times a second. More often than that is
    // a mutex taken in the middle of the decode path for a number nobody can
    // read that fast.
    const uint32_t now = millis();
    if (now - lastPercentAt >= 100) {
      lastPercentAt = now;
      statusLock();
      status.bufferPercent = (uint8_t)(ringUsed * 100 / RING_BYTES);
      statusUnlock();
    }

    if (!decoding) {
      if (ringUsed < prebuffer) {
        // Nothing to do but wait for the socket. Yielding here rather than
        // spinning is what keeps the Wi-Fi task fed while the buffer fills.
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      decoding = true;
      setState(RADIO_PLAYING);
      // Audio is reaching the DAC, so whatever this boot was going to do to
      // itself, it did not. Safe to arm autostart again for the next one.
      if (prefs.isKey(AUTOSTART_TRY_KEY)) prefs.remove(AUTOSTART_TRY_KEY);
      statusLock();
      status.playingSince = millis();
      statusUnlock();
      voice_say(VOICE_RADIO_PLAYING, VOICE_CAT_RADIO);
    }

    if (ringUsed == 0) {
      // The buffer ran dry with the stream still open: the connection cannot
      // keep up. Go back to filling rather than feeding the decoder silence,
      // which is what makes the difference audible as a pause rather than as a
      // burst of noise.
      statusLock();
      status.underruns++;
      statusUnlock();
      decoding = false;
      setState(RADIO_BUFFERING);
      continue;
    }

    const size_t take = ringRead(feed, DECODE_CHUNK);
    // This blocks inside the I2S write, which is what paces the whole loop.
    decoder->write(feed, take);
  }

  /*
   * Order matters, and so does the second delete.
   *
   * EncodedAudioStream holds the decoder as a raw pointer and does not own it:
   * its end() releases libhelix's frame and PCM buffers, but nothing ever frees
   * the wrapper. Deleting the stream first and the codec second is the only
   * arrangement in which the buffers are released before the object that owns
   * them goes away, and it is the difference between a speaker that reconnects
   * for a week and one that runs out of heap overnight.
   */
  decoder->end();
  delete decoder;
  delete codec;
  stream->end();
  delete stream;
  delete tls;
  // Everything this stream held goes back before the next attempt, so a station
  // that is retrying on a backoff is not sitting on 22 kB while it waits.
  arenaRelease();

  *stopped = request.changed && !request.play;
}

void radioTask(void *) {
  uint32_t backoff = RECONNECT_MIN_MS;
  char url[RADIO_URL_MAX] = {0};

  for (;;) {
    // Pick up whatever was last asked for.
    if (request.changed) {
      request.changed = false;
      statusLock();
      status.station = request.station;
      copyString(status.url, sizeof(status.url), request.url);
      copyString(status.name, sizeof(status.name), request.name);
      status.title[0] = 0;
      status.genre[0] = 0;
      status.reconnects = 0;
      status.bufferPercent = 0;
      statusUnlock();
      copyString(url, sizeof(url), request.url);
      backoff = RECONNECT_MIN_MS;
      if (!request.play) setState(RADIO_IDLE);
    }

    if (!request.play || !url[0]) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    if (WiFi.status() != WL_CONNECTED) {
      setState(RADIO_RECONNECTING, "Waiting for Wi-Fi");
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    voice_say(VOICE_RADIO_CONNECTING, VOICE_CAT_RADIO);
    bool stopped = false;
    runStream(url, &stopped);

    if (stopped || !request.play || request.changed) {
      if (!request.changed) setState(RADIO_IDLE);
      continue;
    }

    // The stream ended by itself. Announce the first failure only: a station
    // that has been down for an hour should not be saying so every minute.
    if (status.reconnects == 0) voice_say(VOICE_RADIO_FAILED, VOICE_CAT_RADIO);
    statusLock();
    status.reconnects++;
    statusUnlock();
    setState(RADIO_RECONNECTING);

    const uint32_t waitUntil = millis() + backoff;
    while ((int32_t)(waitUntil - millis()) > 0 && !request.changed) {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    backoff = min(backoff * 2, RECONNECT_MAX_MS);
  }
}

/*
 * Brings the decoder task up, if it is not already.
 *
 * Created on the first station rather than at boot, for the same reason the
 * buffers are: a task's stack comes out of the heap, and ten kilobytes held
 * from boot on a speaker nobody has asked to play anything is ten kilobytes the
 * updater's TLS handshake does not have. Once created it stays -- tearing a
 * task down and standing it up again around every stop is a race for a saving
 * that no longer matters once the arena is gone.
 */
bool ensureTask() {
  if (task) return true;
  /*
   * 10 kB. The deepest thing that happens on this task by a wide margin is the
   * TLS handshake for an https station, which walks a certificate chain against
   * the Mozilla root bundle; libhelix keeps its frame buffers on the heap, so
   * the decoder itself is shallow.
   */
  const BaseType_t ok =
      xTaskCreatePinnedToCore(radioTask, "radio", 10240, nullptr, 2, &task, 1);
  if (ok != pdPASS) {
    task = nullptr;
    LOGLN("[radio] not enough memory to start the decoder task");
    statusLock();
    copyString(status.error, sizeof(status.error),
               "Not enough memory to start the decoder");
    status.state = RADIO_ERROR;
    statusUnlock();
    return false;
  }
  return true;
}

/// Hands the task a new destination. Safe from any task.
void requestPlay(bool play, int8_t station, const char *url, const char *name) {
  if (play && !ensureTask()) return;
  request.play = play;
  request.station = station;
  copyString(request.url, sizeof(request.url), url);
  copyString(request.name, sizeof(request.name), name);
  request.changed = true;
}

}  // namespace

bool net_radio_begin(void *out) {
  if (running) return true;
  i2s = (I2SStream *)out;
  if (!i2s) return false;

  lock = xSemaphoreCreateMutex();
  if (!lock) return false;

  stations = (RadioStation *)calloc(RADIO_MAX_STATIONS, sizeof(RadioStation));
  if (!stations) {
    LOGLN("[radio] no room for the station list; internet radio is off "
                   "this boot");
    vSemaphoreDelete(lock);
    lock = nullptr;
    return false;
  }
  ringClear();

  memset(&status, 0, sizeof(status));
  status.state = RADIO_IDLE;
  status.station = -1;
  loadStations();

  /*
   * No task and no buffers yet.
   *
   * Everything the radio needs to run -- a 10 kB task stack and a 22 kB arena --
   * is claimed on the first station and given back when the stream ends. A
   * speaker that is never asked to play one pays for the station list and
   * nothing else, which is what keeps the firmware updater's TLS handshake
   * possible. See ensureTask() and arenaAcquire().
   *
   * The task, when it is created, goes on core 1 alongside the Arduino loop and
   * deliberately not on core 0: core 0 runs the Wi-Fi and lwIP tasks and the UI
   * render task, and putting a decoder that wants a steady share of a core on
   * top of the stack that is feeding it is how a stream that is fine on the
   * bench underruns on a busy network. Priority 2 puts it above loop() and
   * below the network stack, which is the order the audio actually needs.
   */
  running = true;
  LOGF("[radio] ready, %u stations, %u kB buffer when playing\n",
                (unsigned)stationCount, (unsigned)(STREAM_ARENA / 1024));

  /*
   * Did the last boot survive its own autostart?
   *
   * If the flag is still set, it did not: the speaker crashed somewhere between
   * asking for a station and hearing one. Disarm rather than repeat, because
   * repeating is a boot loop and a boot loop has no dashboard.
   */
  if (autostart && prefs.getBool(AUTOSTART_TRY_KEY, false)) {
    LOGLN("[radio] the last boot did not survive starting a station; autostart "
          "is now off. Turn it back on from the Radio page once you know why.");
    prefs.remove(AUTOSTART_TRY_KEY);
    autostart = false;
    prefs.putBool("auto", false);
  }

  // Nothing is started here. Autostart is serviced from net_radio_loop() once
  // the network has settled -- see there for why booting straight into a
  // stream is the worst possible moment to ask for 50 kB.
  return true;
}

bool net_radio_running() { return running; }

bool net_radio_active() {
  if (!running) return false;
  const RadioState state = status.state;
  return state == RADIO_PLAYING || state == RADIO_BUFFERING;
}

void net_radio_snapshot(RadioStatus *out) {
  if (!out) return;
  if (!running) {
    memset(out, 0, sizeof(*out));
    out->station = -1;
    return;
  }
  statusLock();
  *out = status;
  statusUnlock();
}

/*
 * Publishes into player_state.h so the OLED can draw a radio the same way it
 * draws a phone. Arduino loop task only -- player_state is a single-writer
 * seqlock, and in the modes that have a radio this is the writer.
 */
void net_radio_loop() {
  if (!running) return;

  /*
   * Autostart, once the speaker has settled.
   *
   * Not from net_radio_begin(): at that point the station is still associating,
   * DHCP has not finished, mDNS and the web server have not started, and the
   * Wi-Fi driver is at its peak allocation. Asking for a socket, a 22 kB arena
   * and a 30 kB decoder in the middle of that is asking for the one failure
   * this chip handles worst -- an allocation that fails inside the IDF, which
   * asserts rather than returning an error.
   *
   * Twelve seconds after the station has an address, everything above has
   * finished and given its temporary memory back.
   */
  static bool autostartDone;
  static uint32_t onlineSince;
  if (!autostartDone && autostart && stationCount > 0) {
    if (WiFi.status() != WL_CONNECTED) {
      onlineSince = 0;
    } else {
      if (!onlineSince) onlineSince = millis() | 1;
      if ((uint32_t)(millis() - onlineSince) > 12000) {
        autostartDone = true;
        // Written before the attempt, cleared when a stream actually plays.
        prefs.putBool(AUTOSTART_TRY_KEY, true);
        LOGLN("[radio] resuming the last station");
        net_radio_play_station(0);
      }
    }
  }

  if (volumeDirty && (uint32_t)(millis() - volumeDirtyAt) >= VOLUME_PERSIST_QUIET_MS) {
    volumeDirty = false;
    prefs.putUChar("vol", volume127);
  }

  RadioStatus s;
  net_radio_snapshot(&s);

  static RadioState lastState = RADIO_IDLE;
  static uint32_t lastTitleHash;

  const bool live = s.state == RADIO_PLAYING || s.state == RADIO_BUFFERING;
  if (s.state != lastState) {
    lastState = s.state;
    ps_set_source_connection(live || s.state == RADIO_CONNECTING, s.name);
    ps_set_streaming(s.state == RADIO_PLAYING);
    ps_set_playback(s.state == RADIO_PLAYING ? PS_PLAYING : PS_STOPPED);
  }

  if (live) {
    // A stream has no track list and no length, so the fields that would carry
    // them are used for what a station does supply: the station in the artist
    // line, the genre in the album line, and the now-playing text as the title.
    uint32_t hash = 5381;
    for (const char *p = s.title; *p; ++p) hash = hash * 33 + (uint8_t)*p;
    if (hash != lastTitleHash) {
      lastTitleHash = hash;
      ps_new_track();
      ps_set_track_text(s.title[0] ? s.title : s.name, s.name, s.genre);
    }
    if (s.sampleRate) ps_set_sample_rate((uint16_t)s.sampleRate);
  }
  ps_set_volume(volume127);
}

bool net_radio_play_station(uint8_t index) {
  if (!running || index >= stationCount) return false;
  // Two sources on one jack. The module keeps playing otherwise, and the two
  // sum in the passive network at the output.
  if (df_player_running()) df_player_pause();
  requestPlay(true, (int8_t)index, stations[index].url, stations[index].name);
  /*
   * Pressing play does NOT arm autostart.
   *
   * It used to, on the reasoning that "what it was doing" is what it should
   * come back doing. That reasoning is fine and the behaviour was not: one
   * press quietly turned a setting on that makes every future boot start a
   * stream, and when that went wrong it went wrong before the dashboard came
   * up. Resuming at boot is a decision with consequences at boot, so it is a
   * switch on the Radio page and nothing else sets it.
   */
  return true;
}

bool net_radio_play_url(const char *url, const char *name) {
  if (!running || !urlLooksPlayable(url)) return false;
  if (df_player_running()) df_player_pause();
  char fallback[RADIO_NAME_MAX];
  if (!name || !name[0]) {
    hostFromUrl(url, fallback, sizeof(fallback));
    name = fallback;
  }
  requestPlay(true, -1, url, name);
  return true;
}

void net_radio_stop() {
  if (!running) return;
  requestPlay(false, status.station, status.url, status.name);
}

bool net_radio_toggle() {
  if (!running) return false;
  if (net_radio_active() || status.state == RADIO_CONNECTING ||
      status.state == RADIO_RECONNECTING) {
    net_radio_stop();
    return true;
  }
  if (status.url[0]) {
    requestPlay(true, status.station, status.url, status.name);
    return true;
  }
  return net_radio_play_station(0);
}

bool net_radio_step_station(bool forward) {
  if (!running || stationCount < 2) return false;
  int8_t current = status.station;
  if (current < 0) current = 0;
  const int8_t next = (int8_t)((current + (forward ? 1 : stationCount - 1)) % stationCount);
  return net_radio_play_station((uint8_t)next);
}

void net_radio_set_volume(uint8_t volume) {
  const uint8_t want = volume > 127 ? 127 : volume;
  if (want == volume127) return;
  volume127 = want;
  volumeDirty = true;
  volumeDirtyAt = millis();
}

uint8_t net_radio_volume() { return volume127; }

bool net_radio_autostart() { return autostart; }

void net_radio_set_autostart(bool on) {
  if (autostart == on) return;
  autostart = on;
  if (running) prefs.putBool("auto", on);
}

uint8_t net_radio_station_count() { return stationCount; }

bool net_radio_station(uint8_t index, RadioStation *out) {
  if (!out || index >= stationCount) return false;
  *out = stations[index];
  return true;
}

bool net_radio_set_station(uint8_t index, const char *name, const char *url) {
  if (!urlLooksPlayable(url)) return false;
  if (index >= stationCount) {
    if (stationCount >= RADIO_MAX_STATIONS) return false;
    index = stationCount++;
  }
  copyString(stations[index].url, RADIO_URL_MAX, url);
  if (name && name[0]) {
    copyString(stations[index].name, RADIO_NAME_MAX, name);
  } else {
    hostFromUrl(url, stations[index].name, RADIO_NAME_MAX);
  }
  sanitize(stations[index].name);
  return true;
}

bool net_radio_remove_station(uint8_t index) {
  if (index >= stationCount) return false;
  if (running && status.station == (int8_t)index && net_radio_active()) {
    net_radio_stop();
  }
  for (uint8_t i = index; i + 1 < stationCount; i++) stations[i] = stations[i + 1];
  stationCount--;
  memset(&stations[stationCount], 0, sizeof(RadioStation));
  // The playing index refers to a list that just shifted underneath it.
  if (running) {
    statusLock();
    if (status.station == (int8_t)index) status.station = -1;
    else if (status.station > (int8_t)index) status.station--;
    statusUnlock();
  }
  return true;
}

bool net_radio_move_station(uint8_t index, bool up) {
  if (index >= stationCount) return false;
  const uint8_t other = up ? (uint8_t)(index - 1) : (uint8_t)(index + 1);
  if (up ? index == 0 : other >= stationCount) return false;
  const RadioStation tmp = stations[index];
  stations[index] = stations[other];
  stations[other] = tmp;
  if (running) {
    statusLock();
    if (status.station == (int8_t)index) status.station = (int8_t)other;
    else if (status.station == (int8_t)other) status.station = (int8_t)index;
    statusUnlock();
  }
  return true;
}

void net_radio_store_stations() {
  if (!running) return;
  storeStations();
}

const char *net_radio_state_name(RadioState state) {
  switch (state) {
    case RADIO_IDLE: return "idle";
    case RADIO_CONNECTING: return "connecting";
    case RADIO_BUFFERING: return "buffering";
    case RADIO_PLAYING: return "playing";
    case RADIO_RECONNECTING: return "reconnecting";
    case RADIO_ERROR: return "error";
  }
  return "unknown";
}

bool net_radio_screen_wanted() {
  if (!running) return false;
  RadioStatus s;
  net_radio_snapshot(&s);
  switch (s.state) {
    case RADIO_PLAYING:
    case RADIO_BUFFERING:
    case RADIO_CONNECTING:
    case RADIO_RECONNECTING:
      return true;
    case RADIO_ERROR:
      // A failure is worth showing, but not forever: after a minute the clock
      // is more use than an error nobody is standing there reading.
      return s.error[0] && (uint32_t)(millis() - s.stateSince) < 60000;
    default:
      return false;
  }
}

const char *net_radio_screen_line() {
  static char line[RADIO_TEXT_MAX + RADIO_NAME_MAX];
  RadioStatus s;
  net_radio_snapshot(&s);
  switch (s.state) {
    case RADIO_CONNECTING:
      snprintf(line, sizeof(line), "Connecting to %s", s.name[0] ? s.name : "the station");
      break;
    case RADIO_BUFFERING:
      snprintf(line, sizeof(line), "Buffering %u%%", (unsigned)s.bufferPercent);
      break;
    case RADIO_RECONNECTING:
      snprintf(line, sizeof(line), "Reconnecting to %s", s.name[0] ? s.name : "the station");
      break;
    case RADIO_ERROR:
      snprintf(line, sizeof(line), "%s", s.error[0] ? s.error : "Stream stopped");
      break;
    case RADIO_PLAYING:
      snprintf(line, sizeof(line), "%s", s.title[0] ? s.title : s.name);
      break;
    default:
      snprintf(line, sizeof(line), "%s",
               stationCount ? "Pick a station on the dashboard"
                            : "Add a station on the dashboard");
      break;
  }
  return line;
}

bool net_radio_command(const char *line) {
  if (!line || strncmp(line, "station", 7) != 0) return false;
  const char *rest = line + 7;
  while (*rest == ' ') rest++;

  if (!running) {
    LOGLN("[radio] internet radio is not running in this mode");
    return true;
  }

  if (*rest == 0 || strcmp(rest, "list") == 0) {
    RadioStatus s;
    net_radio_snapshot(&s);
    LOGF("[radio] %s", net_radio_state_name(s.state));
    if (s.name[0]) LOGF(" | %s", s.name);
    if (s.title[0]) LOGF(" | %s", s.title);
    if (s.bitrate) LOGF(" | %u kbps", (unsigned)s.bitrate);
    if (s.sampleRate) {
      LOGF(" | %s %u Hz %s", s.codec, (unsigned)s.sampleRate,
                    s.channels == 1 ? "mono" : "stereo");
    }
    LOGF(" | buffer %u%%", (unsigned)s.bufferPercent);
    if (s.underruns) LOGF(" | %u underruns", (unsigned)s.underruns);
    if (s.error[0]) LOGF(" | %s", s.error);
    LOGF(" | volume %u", (unsigned)volume127);
    /*
     * The two ways this feature can go wrong on a chip this size, as numbers.
     *
     * An https handshake is by a wide margin the deepest thing that runs on the
     * decoder task, so a stack margin down to a few hundred bytes is a crash
     * waiting for a longer certificate chain. And "arena" is the 22 kB a stream
     * holds while it plays -- if that says "held" with nothing playing, it has
     * leaked, and the heap figures next to it are what the firmware updater's
     * TLS handshake has to work with.
     */
    if (task) {
      LOGF(" | task stack %u B free",
                    (unsigned)(uxTaskGetStackHighWaterMark(task) * sizeof(StackType_t)));
    } else {
      LOGP(" | task not started");
    }
    LOGF(" | arena %s | heap %u free, %u largest\n",
                  arena ? "held" : "released", (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap());
    for (uint8_t i = 0; i < stationCount; i++) {
      LOGF("  %u%c %-28s %s\n", (unsigned)(i + 1),
                    s.station == (int8_t)i ? '*' : '.', stations[i].name,
                    stations[i].url);
    }
    if (!stationCount) LOGLN("  (no stations stored)");
    return true;
  }
  if (strcmp(rest, "stop") == 0) {
    net_radio_stop();
    LOGLN("[radio] stopped");
    return true;
  }
  if (strcmp(rest, "next") == 0 || strcmp(rest, "prev") == 0) {
    if (!net_radio_step_station(rest[0] == 'n')) {
      LOGLN("[radio] fewer than two stations stored");
    }
    return true;
  }
  const int index = atoi(rest);
  if (index >= 1 && index <= stationCount) {
    net_radio_play_station((uint8_t)(index - 1));
    LOGF("[radio] playing %s\n", stations[index - 1].name);
    return true;
  }
  if (urlLooksPlayable(rest)) {
    net_radio_play_url(rest, nullptr);
    LOGF("[radio] playing %s\n", rest);
    return true;
  }
  LOGLN("[radio] usage: station | station <n> | station <url> | "
                 "station stop|next|prev");
  return true;
}
