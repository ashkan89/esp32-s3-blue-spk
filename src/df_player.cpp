#include "df_player.h"

#if DFPLAYER_ENABLED

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "player_state.h"

/*
 * The YX5200 serial protocol, in full, because it is small and the libraries
 * that wrap it all disagree about the parts that matter here.
 *
 * Every frame in both directions is ten bytes:
 *
 *   0  0x7E   start
 *   1  0xFF   version
 *   2  0x06   length of bytes 2..6 inclusive
 *   3  CMD
 *   4  ACK    1 = please acknowledge, 0 = do not bother
 *   5  PARAM high byte
 *   6  PARAM low byte
 *   7  CKSUM high byte
 *   8  CKSUM low byte
 *   9  0xEF   end
 *
 * and the checksum is the two's complement of the sum of bytes 1..6. Some
 * modules ship with the checksum disabled and will accept 0x0000 there, but
 * they all *send* a correct one, so it is verified on the way in and always
 * sent on the way out.
 *
 * ACK is requested on nothing. It sounds like the safe choice and is not: the
 * module answers an acknowledged command with 0x41 before it acts on it, so a
 * queue of acknowledged commands interleaves replies with the asynchronous
 * notifications the module also sends, and the parser can no longer tell which
 * 0x42 belongs to which query. The status poll a moment later is a better
 * confirmation than the ACK anyway, because it reports what actually happened
 * rather than that a frame was received.
 */

namespace {

// -------------------------------------------------------------- protocol ----

constexpr uint8_t FRAME_START = 0x7E;
constexpr uint8_t FRAME_VERSION = 0xFF;
constexpr uint8_t FRAME_LEN = 0x06;
constexpr uint8_t FRAME_END = 0xEF;
constexpr size_t FRAME_SIZE = 10;

// Commands we send.
constexpr uint8_t CMD_NEXT = 0x01;
constexpr uint8_t CMD_PREV = 0x02;
constexpr uint8_t CMD_TRACK = 0x03;
constexpr uint8_t CMD_VOL_UP = 0x04;
constexpr uint8_t CMD_VOL_DOWN = 0x05;
constexpr uint8_t CMD_VOLUME = 0x06;
constexpr uint8_t CMD_EQ = 0x07;
constexpr uint8_t CMD_SOURCE = 0x09;
constexpr uint8_t CMD_STANDBY = 0x0A;
constexpr uint8_t CMD_WAKE = 0x0B;
constexpr uint8_t CMD_RESET = 0x0C;
constexpr uint8_t CMD_PLAY = 0x0D;
constexpr uint8_t CMD_PAUSE = 0x0E;
constexpr uint8_t CMD_FOLDER_FILE = 0x0F;
constexpr uint8_t CMD_REPEAT_ALL = 0x11;
constexpr uint8_t CMD_MP3_FOLDER = 0x12;
constexpr uint8_t CMD_ADVERT = 0x13;
constexpr uint8_t CMD_ADVERT_STOP = 0x15;
constexpr uint8_t CMD_STOP = 0x16;
constexpr uint8_t CMD_LOOP_FOLDER = 0x17;
constexpr uint8_t CMD_RANDOM = 0x18;
constexpr uint8_t CMD_LOOP_TRACK = 0x19;
constexpr uint8_t CMD_DAC = 0x1A;

// Queries we send, and the replies that come back under the same number.
constexpr uint8_t Q_STATUS = 0x42;
constexpr uint8_t Q_VOLUME = 0x43;
constexpr uint8_t Q_EQ = 0x44;
constexpr uint8_t Q_VERSION = 0x46;
constexpr uint8_t Q_FILES_USB = 0x47;
constexpr uint8_t Q_FILES_SD = 0x48;
constexpr uint8_t Q_FILES_FLASH = 0x49;
constexpr uint8_t Q_TRACK_USB = 0x4B;
constexpr uint8_t Q_TRACK_SD = 0x4C;
constexpr uint8_t Q_TRACK_FLASH = 0x4D;
constexpr uint8_t Q_FOLDER_FILES = 0x4E;
constexpr uint8_t Q_FOLDERS = 0x4F;

// Notifications the module sends unprompted.
constexpr uint8_t EV_INSERTED = 0x3A;
constexpr uint8_t EV_REMOVED = 0x3B;
constexpr uint8_t EV_DONE_USB = 0x3C;
constexpr uint8_t EV_DONE_SD = 0x3D;
constexpr uint8_t EV_DONE_FLASH = 0x3E;
constexpr uint8_t EV_ONLINE = 0x3F;
constexpr uint8_t EV_ERROR = 0x40;
constexpr uint8_t EV_ACK = 0x41;

// Bits in the 0x3F init report and the 0x3A/0x3B device field.
constexpr uint16_t DEV_USB = 0x01;
constexpr uint16_t DEV_SD = 0x02;
constexpr uint16_t DEV_PC = 0x04;
constexpr uint16_t DEV_FLASH = 0x08;

const char *error_text(uint8_t code) {
  switch (code) {
    case 0x01: return "Module busy: it was still acting on the last command";
    case 0x02: return "Module is in standby; wake it first";
    case 0x03: return "The module could not read that frame";
    case 0x04: return "Frame checksum rejected";
    case 0x05: return "Track number is past the end of the library";
    case 0x06: return "No such file on the card";
    case 0x07: return "Announcement failed: nothing was playing to interrupt";
    case 0x08: return "Card read failed: reseat it, or reformat as FAT32";
    case 0x0A: return "Module went to sleep";
    default: return "Module reported an error";
  }
}

// ------------------------------------------------------------------ state ----

DfStatus status;
SemaphoreHandle_t statusLock;
QueueHandle_t commands;
bool running;

/// Read without the mutex, by the status LED and the melody gate, which run
/// every loop and only need one value each. A read that races the driver task
/// returns the previous value for one frame, which no indicator can show.
volatile DfState liveState = DF_STOPPED;
volatile bool liveBusy;

/// Which source the *driver* believes is selected. Kept outside the mutex too,
/// because the reply parser needs it to decide whether a 0x4C ("current track
/// on SD") is about the library we are playing from.
volatile uint8_t activeSource = DF_SRC_SD;

/*
 * Whether the module has been told to sleep.
 *
 * Standby was not a state that stayed put without this. The 900 ms poll kept
 * running; a sleeping module answers 0x42 with a transport state of 0, which the
 * parser read as DF_STOPPED and wrote over DF_SLEEPING; and it answers every
 * *other* query with error 0x02 ("in standby"), which latched as a permanent
 * fault in the dashboard, on the console and on the OLED. So while this is set,
 * the poller asks nothing and the parser leaves the state alone -- which is also
 * the honest behaviour, because a module in standby has nothing to report.
 */
volatile bool sleeping;

enum Op : uint8_t {
  OP_FRAME,    ///< put one protocol frame on the wire
  OP_PULSE,    ///< pull one of the module's button pins to ground
  OP_STARTUP,  ///< the whole wake-up sequence, expanded by the task
  OP_REFRESH,  ///< the library queries, likewise
  OP_LED,      ///< change the dedicated LED's mode
};

struct Command {
  Op op;
  uint8_t cmd;    ///< protocol command for OP_FRAME, DfPin for OP_PULSE
  uint16_t param; ///< protocol parameter, or press length in ms
};

/*
 * `wait` is 0 for anything queued from the driver task itself.
 *
 * queueStartup() and queueRefresh() expand into frames on that task -- they are
 * the consumer -- so a blocking push there would have the consumer wait on its
 * own queue. Callers from other tasks can afford the 20 ms, and want it: it is
 * the difference between a dashboard button working and one that silently drops
 * a command because two arrived at once.
 */
bool push(const Command &cmd, uint32_t wait = 20) {
  if (commands == nullptr) return false;
  return xQueueSend(commands, &cmd, pdMS_TO_TICKS(wait)) == pdTRUE;
}

/*
 * The folder index. See the header for why it exists and why it is a scan.
 *
 * `scanAt` is the folder the last 0x4E asked about, which is how a reply that
 * does not name a folder gets attributed to one. It is written only by the
 * driver task, where the frames are sent, and read only by the reply parser,
 * which runs on that same task -- so it needs no lock; the counts do, because
 * the web task reads them.
 */
uint16_t folderCounts[DF_MAX_FOLDERS];
uint8_t scanAt;        ///< folder the last 0x4E query named, 0 for none
uint8_t scanNext;      ///< next folder to ask about, 0 when no scan is running
bool scanComplete;     ///< a scan finished and the cache has not been dropped
uint32_t scanSentAt;   ///< when the last scan query went out

/// The scan's own pacing. Slower than DF_COMMAND_GAP_MS deliberately: the
/// attribution above assumes the answer to folder N arrives before the question
/// about N+1 goes out, and 120 ms is comfortably longer than the module takes
/// to answer a query it can answer instantly.
constexpr uint16_t SCAN_GAP_MS = 120;

/// Forgets the index. Anything that could have changed what is on the card
/// calls this -- a source switch, a card event, a reset -- because a stale
/// count is worse than no count: the browser would offer a track that is not
/// there and the module would refuse it with no explanation.
void dropFolderIndex() {
  memset(folderCounts, 0, sizeof(folderCounts));
  scanAt = 0;
  scanNext = 0;
  scanComplete = false;
}

bool frame(uint8_t cmd, uint16_t param = 0) {
  return push(Command{OP_FRAME, cmd, param});
}

/// The same, from the driver task. Never blocks; a full queue drops the frame,
/// which for a query means the next poll asks again.
bool frameNow(uint8_t cmd, uint16_t param = 0) {
  return push(Command{OP_FRAME, cmd, param}, 0);
}

// The four pins that are driven, in DfPin order.
constexpr int PIN_TABLE[DF_PIN_COUNT] = {
    PIN_DF_IO1,
    PIN_DF_IO2,
    PIN_DF_ADKEY1,
    PIN_DF_ADKEY2,
};

// ----------------------------------------------------------- status writes ---
/*
 * Everything below runs on the driver task and takes the mutex for as long as
 * it takes to store a field. Nothing in here can block on anything else: the
 * lock is only ever held by this task and by snapshot readers, and no reader
 * does work while holding it.
 */

struct Locked {
  bool held;
  Locked() : held(statusLock != nullptr &&
                  xSemaphoreTake(statusLock, pdMS_TO_TICKS(100)) == pdTRUE) {}
  ~Locked() { if (held) xSemaphoreGive(statusLock); }
};

void setError(const char *text) {
  Locked lock;
  if (!lock.held) return;
  if (text == nullptr) status.error[0] = 0;
  else strlcpy(status.error, text, sizeof(status.error));
}

// ------------------------------------------------------------------ frames ---

void writeFrame(uint8_t cmd, uint16_t param) {
  uint8_t buf[FRAME_SIZE];
  buf[0] = FRAME_START;
  buf[1] = FRAME_VERSION;
  buf[2] = FRAME_LEN;
  buf[3] = cmd;
  buf[4] = 0;  // no ACK requested; see the note at the top
  buf[5] = (uint8_t)(param >> 8);
  buf[6] = (uint8_t)(param & 0xFF);
  uint16_t sum = 0;
  for (int i = 1; i <= 6; i++) sum += buf[i];
  const uint16_t checksum = (uint16_t)(-(int16_t)sum);
  buf[7] = (uint8_t)(checksum >> 8);
  buf[8] = (uint8_t)(checksum & 0xFF);
  buf[9] = FRAME_END;
  Serial2.write(buf, FRAME_SIZE);
  Serial2.flush();
}

/*
 * The reply parser.
 *
 * Framing on this link is not reliable enough to trust a byte count: the module
 * powers up mid-frame, a card access makes it stutter, and a 1k series resistor
 * on a breadboard does the rest. So the parser resynchronises on the start byte
 * and validates everything -- version, length, terminator, checksum -- before a
 * frame is believed. A frame that fails any of those is dropped silently rather
 * than reported, because the usual cause is the first partial frame after boot
 * and a start-up error message that always appears is worse than none.
 */
void handleFrame(uint8_t cmd, uint16_t param);

bool frameValid(const uint8_t *buf) {
  if (buf[0] != FRAME_START || buf[1] != FRAME_VERSION || buf[2] != FRAME_LEN ||
      buf[9] != FRAME_END) {
    return false;
  }
  uint16_t sum = 0;
  for (int i = 1; i <= 6; i++) sum += buf[i];
  const uint16_t want = (uint16_t)(-(int16_t)sum);
  const uint16_t got = (uint16_t)((buf[7] << 8) | buf[8]);
  // Modules with the checksum switched off send 0x0000. Accept that rather than
  // throwing away everything such a module ever says.
  return got == want || got == 0;
}

void readSerial() {
  static uint8_t buf[FRAME_SIZE];
  static uint8_t len;

  while (Serial2.available()) {
    // Nothing before a start byte can be the beginning of a frame, so those
    // bytes are dropped without occupying the window at all. Everything after
    // one goes in, 0x7E included: the protocol has no escaping, so a parameter
    // or a checksum byte may legitimately *be* 0x7E, and treating one as a
    // start byte mid-frame would throw away every reply about, say, a card with
    // 126 files on it.
    const uint8_t byte = (uint8_t)Serial2.read();
    if (len == 0 && byte != FRAME_START) continue;
    buf[len++] = byte;
    if (len < FRAME_SIZE) continue;

    if (frameValid(buf)) {
      handleFrame(buf[3], (uint16_t)((buf[5] << 8) | buf[6]));
      len = 0;
      continue;
    }

    /*
     * Ten bytes that are not a frame. The module powers up mid-frame, a card
     * access makes it stutter, and a 1k series resistor on a breadboard does the
     * rest -- so this has to self-heal rather than assume the next ten bytes
     * will line up. Slide the window to the next candidate start byte inside it
     * and try again; if there is none, drop the lot. That converges in at most
     * one frame's worth of bytes, and it costs a memmove of nine bytes at 9600
     * baud, which is not a cost worth avoiding.
     *
     * Nothing is reported here. A failed frame is almost always the first
     * partial one after the module powers up, and a start-up error message that
     * always appears is worse than none.
     */
    uint8_t next = 0;
    for (uint8_t k = 1; k < FRAME_SIZE; k++) {
      if (buf[k] == FRAME_START) {
        next = k;
        break;
      }
    }
    if (next == 0) {
      len = 0;
      continue;
    }
    len = (uint8_t)(FRAME_SIZE - next);
    memmove(buf, buf + next, len);
  }
}

void applyDevices(uint16_t mask, bool present) {
  Locked lock;
  if (!lock.held) return;
  if (mask & DEV_USB) status.usbPresent = present;
  if (mask & DEV_SD) status.sdPresent = present;
  if (mask & DEV_FLASH) status.flashPresent = present;
  if (mask & DEV_PC) status.pcLink = present;
}

void handleFrame(uint8_t cmd, uint16_t param) {
  {
    Locked lock;
    if (lock.held) {
      status.online = true;
      status.lastFrameAt = millis();
    }
  }

  switch (cmd) {
    case EV_ONLINE:
      // The module's own start-up report, and the only place it volunteers
      // which libraries it found. A bare 0x3F with no bits set happens on some
      // clones; treat it as "the card, presumably", which is what it means in
      // practice and what the following file-count query will confirm.
      applyDevices(param ? param : DEV_SD, true);
      setError(nullptr);
      break;

    case EV_INSERTED:
      applyDevices(param, true);
      dropFolderIndex();
      if (param & DEV_PC) {
        setError("A computer has the card: playback from it stops until the "
                 "USB cable is unplugged");
      } else {
        setError(nullptr);
      }
      break;

    case EV_REMOVED:
      applyDevices(param, false);
      dropFolderIndex();
      if (param & DEV_PC) setError(nullptr);
      else setError("Storage was removed while the module was using it");
      break;

    case EV_DONE_USB:
    case EV_DONE_SD:
    case EV_DONE_FLASH: {
      Locked lock;
      if (!lock.held) break;
      status.finished++;
      if (param) status.track = param;
      // Whether the module goes on to the next file depends on the loop mode it
      // is in, and it does not say. The status poll settles it within a second;
      // until then leave the state alone rather than flapping the UI between
      // "playing" and "stopped" at every track boundary.
      break;
    }

    case EV_ERROR:
      // "In standby" needs different words depending on who provoked it. The
      // poller is switched off while the module sleeps, so anything that reaches
      // this point was a command somebody sent -- and the useful answer is what
      // to do about it, not the module's own phrasing repeated as a fault.
      if (sleeping && (uint8_t)(param & 0xFF) == 0x02) {
        setError("The module is in standby and ignored that. Wake it first.");
        break;
      }
      setError(error_text((uint8_t)(param & 0xFF)));
      break;

    case EV_ACK:
      break;  // not requested, but a module that sends one anyway is harmless

    case Q_STATUS: {
      // High byte is the device, low byte the transport state.
      const uint8_t device = (uint8_t)(param >> 8);
      const uint8_t play = (uint8_t)(param & 0xFF);
      Locked lock;
      if (!lock.held) break;
      if (device >= DF_SRC_USB && device <= DF_SRC_FLASH) {
        status.reported = (DfSource)device;
      }
      // A late reply that crossed a standby command would otherwise report the
      // module as merely stopped, and nothing would correct it: the poller is
      // switched off while it sleeps.
      if (sleeping) break;
      status.state = play == 1 ? DF_PLAYING : play == 2 ? DF_PAUSED : DF_STOPPED;
      liveState = status.state;
      break;
    }

    case Q_VOLUME: {
      Locked lock;
      if (!lock.held) break;
      status.volume = param > DF_VOLUME_MAX ? DF_VOLUME_MAX : (uint8_t)param;
      break;
    }

    case Q_EQ: {
      Locked lock;
      if (!lock.held) break;
      status.eq = param > 5 ? 5 : (uint8_t)param;
      break;
    }

    case Q_VERSION: {
      Locked lock;
      if (!lock.held) break;
      status.version = param;
      break;
    }

    case Q_FILES_USB:
    case Q_FILES_SD:
    case Q_FILES_FLASH: {
      const uint8_t want = activeSource == DF_SRC_USB   ? Q_FILES_USB
                           : activeSource == DF_SRC_FLASH ? Q_FILES_FLASH
                                                          : Q_FILES_SD;
      Locked lock;
      if (!lock.held) break;
      // A count for a library we are not playing from still proves it is there,
      // which is worth recording -- but only the active one drives the picker.
      if (param) {
        if (cmd == Q_FILES_USB) status.usbPresent = true;
        if (cmd == Q_FILES_SD) status.sdPresent = true;
        if (cmd == Q_FILES_FLASH) status.flashPresent = true;
      }
      if (cmd == want) status.totalTracks = param;
      break;
    }

    case Q_TRACK_USB:
    case Q_TRACK_SD:
    case Q_TRACK_FLASH: {
      const uint8_t want = activeSource == DF_SRC_USB   ? Q_TRACK_USB
                           : activeSource == DF_SRC_FLASH ? Q_TRACK_FLASH
                                                          : Q_TRACK_SD;
      if (cmd != want) break;
      Locked lock;
      if (!lock.held) break;
      if (param) status.track = param;
      break;
    }

    case Q_FOLDER_FILES: {
      Locked lock;
      if (!lock.held) break;
      status.folderTracks = param;
      // The reply does not name a folder, so it belongs to whichever one the
      // last query named. One task owns the wire and the module answers in
      // order, so that is the right one.
      if (scanAt >= 1 && scanAt <= DF_MAX_FOLDERS) folderCounts[scanAt - 1] = param;
      break;
    }

    case Q_FOLDERS: {
      Locked lock;
      if (!lock.held) break;
      status.folders = param;
      break;
    }

    default:
      break;
  }
}

// ------------------------------------------------------------------- pins ----

/// Open-drain press. At rest the pin is an input, so the module's own pull-up
/// holds the line and a real button wired in parallel still works; a press is
/// an output driven low for as long as the module needs to see it.
struct Press {
  uint32_t until;  // 0 = not pressed
};
Press presses[DF_PIN_COUNT];

void releasePin(uint8_t index) {
  const int pin = PIN_TABLE[index];
  if (pin < 0) return;
  pinMode(pin, INPUT);
  presses[index].until = 0;
}

void pressPin(uint8_t index, uint16_t ms) {
  const int pin = PIN_TABLE[index];
  if (pin < 0) return;
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
  // `| 1` because 0 is the "not pressed" sentinel. Without it, a press whose
  // deadline landed exactly on the millis() rollover would never be released
  // and would leave the module's button input held down for good.
  presses[index].until = (millis() + ms) | 1;
}

void servicePresses() {
  const uint32_t now = millis();
  for (uint8_t i = 0; i < DF_PIN_COUNT; i++) {
    if (presses[i].until && (int32_t)(now - presses[i].until) >= 0) {
      releasePin(i);
    }
  }
}

// -------------------------------------------------------------------- LED ----

void writeLed(bool on) {
#if PIN_DF_LED >= 0
  digitalWrite(PIN_DF_LED, (on == !!DF_LED_ACTIVE_HIGH) ? HIGH : LOW);
#endif
  Locked lock;
  if (lock.held) status.ledOn = on;
}

void serviceLed(DfLedMode mode, bool busy) {
#if PIN_DF_LED >= 0
  static bool last;
  static bool primed;
  bool want;
  switch (mode) {
    case DF_LED_OFF: want = false; break;
    case DF_LED_ON: want = true; break;
    case DF_LED_BLINK: want = (millis() % 1000) < 500; break;
    default: want = busy; break;  // DF_LED_AUTO
  }
  if (want != last || !primed) {
    primed = true;
    last = want;
    writeLed(want);
  }
#else
  (void)mode;
  (void)busy;
#endif
}

// ---------------------------------------------------------------- sequences ---

/// The wake-up sequence, as protocol frames. Queued rather than sent inline so
/// it obeys the same inter-frame gap as everything else, and so a reset from the
/// dashboard runs exactly the same steps as the one at boot.
void queueStartup(DfSource source, uint8_t volume, uint8_t eq, DfLoop loop) {
  // After the reset frame is on the queue, not before. A frame that was dropped
  // leaves the module asleep, and a cleared flag would then restart the poller
  // against it -- every query answered with "in standby", and that error latched
  // as a permanent fault because the suppression is gated on this same flag.
  if (frameNow(CMD_RESET, 0)) sleeping = false;
  // The task inserts DF_RESET_SETTLE_MS after a reset frame; see the task loop.
  frameNow(CMD_SOURCE, (uint16_t)source);
  frameNow(CMD_VOLUME, volume);
  frameNow(CMD_EQ, eq);
  frameNow(CMD_DAC, 0);  // 0 = DAC on
  switch (loop) {
    case DF_LOOP_ALL: frameNow(CMD_REPEAT_ALL, 1); break;
    case DF_LOOP_TRACK: frameNow(CMD_LOOP_TRACK, 0); break;
    case DF_LOOP_RANDOM: break;  // starts playback; never done at boot
    case DF_LOOP_FOLDER: break;  // needs a folder; set from the dashboard
    default: break;
  }
  push(Command{OP_REFRESH, 0, 0}, 0);
}

void queueRefresh() {
  frameNow(Q_VERSION);
  frameNow(Q_VOLUME);
  frameNow(Q_EQ);
  frameNow(Q_FOLDERS);
  frameNow(Q_FILES_SD);
  frameNow(Q_FILES_USB);
  frameNow(Q_STATUS);
}

// ------------------------------------------------------------------- task ----

/// Startup arguments, so a reset can re-run the same sequence.
DfSource bootSource = DF_SRC_SD;
uint8_t bootVolume = DF_VOLUME_DEFAULT;
uint8_t bootEq;
DfLoop bootLoop = DF_LOOP_OFF;

DfLedMode ledMode = DF_LED_AUTO;

/// How long BUSY has to stay high before the driver believes the track really
/// ended. Longer than the gap the module leaves between two files (tens of
/// milliseconds on a healthy card, up to ~200 ms on a slow one), short enough
/// that a real stop still registers well inside one display frame.
constexpr uint32_t BUSY_SETTLE_MS = 400;

void driverTask(void *) {
  uint32_t idleSince = 0;  // millis() when BUSY went high, 0 while playing
  uint32_t nextSend = 0;
  uint32_t nextPoll = millis() + 1500;
  uint8_t pollStep = 0;
  Command cmd;

  for (;;) {
    readSerial();
    servicePresses();

    // BUSY is the fastest and most honest indicator the module has: it is a
    // hardware output of the decoder, so it does not wait for a poll.
#if PIN_DF_BUSY >= 0
    const bool busy = digitalRead(PIN_DF_BUSY) == LOW;
#else
    const bool busy = liveState == DF_PLAYING;
#endif
    liveBusy = busy;
    // When BUSY first went high, so the settle test below has something to
    // measure. `| 1` keeps 0 as the "currently playing" sentinel.
    if (busy) idleSince = 0;
    else if (idleSince == 0) idleSince = millis() | 1;
    serviceLed(ledMode, busy);

    const bool asleep = sleeping;
    {
      Locked lock;
      if (lock.held) {
        status.busy = busy;
        status.ledMode = ledMode;
        status.asleep = asleep;
        // Offline is decided here rather than in the parser, because the whole
        // point is that no frame arrived.
        //
        // Which is exactly why standby is excluded: the poller is switched off
        // while the module sleeps, so no frame arrives *by design*, and letting
        // this timeout run would report a deliberate standby as a dead module
        // six seconds in -- fault LED, "No module" on the OLED, and a dashboard
        // telling the owner to check wiring that is perfectly fine.
        if (!asleep && status.online && status.lastFrameAt &&
            millis() - status.lastFrameAt > DF_ONLINE_TIMEOUT_MS) {
          status.online = false;
        }
#if PIN_DF_BUSY >= 0
        /*
         * BUSY disagreeing with the last polled state is BUSY being right: the
         * pin changes at the track boundary and the poll is up to a second
         * behind. It is trusted immediately when it says "playing" and only
         * after BUSY_SETTLE_MS when it says "not playing", because the pin also
         * goes high in the gap *between* two tracks -- while the module closes
         * one file and seeks the next. Without the delay, every auto-advance in
         * a repeat-all playlist would flicker the dashboard badge and the OLED
         * through "stopped" and back, and fire a playback-state change in
         * player_state.h on the way.
         */
        //
        // Not while it is going to sleep, though. Standby is queued like any
        // other command, so for the few tens of milliseconds before the frame
        // reaches the module BUSY is still low and would put the state back to
        // DF_PLAYING -- and when the decoder finally powers down and BUSY
        // releases, this would write DF_STOPPED rather than DF_SLEEPING. With
        // the poller off, that wrong answer would then stand indefinitely.
        if (asleep) {
          // nothing: the module is not playing and not stopped, it is asleep
        } else if (busy && status.state != DF_PLAYING) {
          status.state = DF_PLAYING;
          liveState = DF_PLAYING;
        } else if (!busy && status.state == DF_PLAYING &&
                   idleSince && millis() - idleSince >= BUSY_SETTLE_MS) {
          status.state = DF_STOPPED;
          liveState = DF_STOPPED;
        }
#endif
      }
    }

    const uint32_t now = millis();
    if ((int32_t)(now - nextSend) >= 0 &&
        xQueueReceive(commands, &cmd, 0) == pdTRUE) {
      switch (cmd.op) {
        case OP_FRAME:
          writeFrame(cmd.cmd, cmd.param);
          // A reset is not a command, it is a power cycle of the decoder: it
          // answers nothing and ignores everything until it has come back.
          nextSend = now + (cmd.cmd == CMD_RESET ? DF_RESET_SETTLE_MS
                                                 : DF_COMMAND_GAP_MS);
          break;
        case OP_PULSE:
          if (cmd.cmd < DF_PIN_COUNT) pressPin(cmd.cmd, cmd.param);
          // The press itself is asynchronous, but the module reacts to it the
          // same way it reacts to a frame, so give it the same room afterwards.
          nextSend = now + cmd.param + DF_COMMAND_GAP_MS;
          break;
        case OP_STARTUP:
          queueStartup(bootSource, bootVolume, bootEq, bootLoop);
          break;
        case OP_REFRESH:
          queueRefresh();
          break;
        case OP_LED:
          ledMode = (DfLedMode)cmd.cmd;
          break;
      }
      continue;  // straight back round: there may be more, and reads are cheap
    }

    /*
     * The folder scan, one query per pass, ahead of the poller.
     *
     * It goes first because a scan the owner asked for and is watching a
     * progress bar for should not be held up by routine polling, and it only
     * ever runs when the queue is empty, so it cannot get in the way of a
     * command either. Ninety-nine folders at SCAN_GAP_MS is about twelve
     * seconds; the counts appear in the dashboard as they land.
     */
    if (!sleeping && scanNext && uxQueueMessagesWaiting(commands) == 0 &&
        (int32_t)(now - scanSentAt) >= (int32_t)SCAN_GAP_MS) {
      scanAt = scanNext;
      scanSentAt = now;
      frameNow(Q_FOLDER_FILES, scanAt);
      if (++scanNext > DF_MAX_FOLDERS) {
        scanNext = 0;
        scanComplete = true;
      }
      continue;
    }

    // The status poll, spread over several rounds so no single wake-up puts four
    // frames on a 9600 baud link at once. Nothing is asked of a module that has
    // been told to sleep: it answers queries with an error, and that error would
    // latch as a fault for as long as it stayed asleep.
    if (!sleeping && (int32_t)(now - nextPoll) >= 0 &&
        uxQueueMessagesWaiting(commands) == 0) {
      nextPoll = now + DF_POLL_MS;
      switch (pollStep++ & 0x03) {
        case 0: frameNow(Q_STATUS); break;
        case 1:
          frameNow(activeSource == DF_SRC_USB     ? Q_TRACK_USB
                   : activeSource == DF_SRC_FLASH ? Q_TRACK_FLASH
                                                  : Q_TRACK_SD);
          break;
        case 2: frameNow(Q_VOLUME); break;
        default: {
          // The library size only changes when a card does, and that arrives as
          // a notification -- but a module that missed its own boot report has
          // no other way of ever learning it.
          //
          // The read is copied out and the lock released before anything is
          // queued. Holding the status mutex across a push would block whichever
          // task happened to be taking a snapshot, and this file's own rule is
          // that nothing holds that lock while it waits on something else.
          bool unknown;
          {
            Locked lock;
            unknown = lock.held && status.totalTracks == 0;
          }
          if (unknown) {
            frameNow(activeSource == DF_SRC_USB     ? Q_FILES_USB
                     : activeSource == DF_SRC_FLASH ? Q_FILES_FLASH
                                                    : Q_FILES_SD);
          }
          break;
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

}  // namespace

// ============================================================== public API ===

bool df_player_begin(DfSource source, uint8_t volume, uint8_t eq, DfLoop loop) {
  if (running) return true;

  bootSource = (source >= DF_SRC_USB && source <= DF_SRC_FLASH &&
                source != DF_SRC_SLEEP)
                   ? source
                   : DF_SRC_SD;
  bootVolume = volume > DF_VOLUME_MAX ? DF_VOLUME_MAX : volume;
  bootEq = eq > 5 ? 0 : eq;
  bootLoop = loop > DF_LOOP_RANDOM ? DF_LOOP_OFF : loop;

  status = DfStatus{};
  status.source = bootSource;
  status.reported = bootSource;
  status.volume = bootVolume;
  status.eq = bootEq;
  status.loop = bootLoop;
  status.dacOn = true;
  status.ledMode = DF_LED_AUTO;
  activeSource = bootSource;

  statusLock = xSemaphoreCreateMutex();
  commands = xQueueCreate(24, sizeof(Command));
  if (statusLock == nullptr || commands == nullptr) {
    Serial.println("[df] out of memory: driver not started");
    return false;
  }

  Serial2.begin(DF_BAUD, SERIAL_8N1, PIN_DF_RX, PIN_DF_TX);
  Serial2.setTimeout(0);

#if PIN_DF_BUSY >= 0
  pinMode(PIN_DF_BUSY, INPUT);
#endif
#if PIN_DF_USB_DETECT >= 0
  pinMode(PIN_DF_USB_DETECT, INPUT);
#endif
#if PIN_DF_LED >= 0
  pinMode(PIN_DF_LED, OUTPUT);
  digitalWrite(PIN_DF_LED, DF_LED_ACTIVE_HIGH ? LOW : HIGH);
#endif
  // The button pins rest as inputs, which is what makes the press open-drain.
  for (uint8_t i = 0; i < DF_PIN_COUNT; i++) {
    if (PIN_TABLE[i] >= 0) pinMode(PIN_TABLE[i], INPUT);
  }

  // Priority 1 on core 0, next to the UI task rather than the Arduino loop.
  // Nothing here is time critical -- the slowest thing in the system is the
  // 9600 baud link it is waiting on -- and core 1 is where the loop and the
  // web server live.
  if (xTaskCreatePinnedToCore(driverTask, "dfplayer", 4096, nullptr, 1, nullptr,
                              0) != pdPASS) {
    Serial.println("[df] could not start the driver task");
    return false;
  }

  running = true;
  status.running = true;
  push(Command{OP_STARTUP, 0, 0});
  Serial.printf("[df] DFPlayer driver up: uart2 rx=%d tx=%d busy=%d, "
                "source %s, volume %u/%u\n",
                PIN_DF_RX, PIN_DF_TX, PIN_DF_BUSY, df_source_name(bootSource),
                (unsigned)bootVolume, (unsigned)DF_VOLUME_MAX);
  return true;
}

bool df_player_running() { return running; }

bool df_player_active() { return running && (liveBusy || liveState == DF_PLAYING); }

DfState df_player_state() { return liveState; }

bool df_player_snapshot(DfStatus *out) {
  if (out == nullptr) return false;
  if (!running || statusLock == nullptr) {
    *out = DfStatus{};
    return false;
  }
  if (xSemaphoreTake(statusLock, pdMS_TO_TICKS(100)) != pdTRUE) {
    /*
     * A timed-out copy is not a reading, and callers used to have no way to
     * tell. The zero-filled struct that came back said "module offline, volume
     * 0, no files" -- which flashed the fault pattern on the LED, printed
     * "check TX/RX" on the OLED, and, if the dashboard's "save as startup
     * defaults" landed in the same window, wrote volume 0 and a bogus source
     * into NVS for the next boot to start from.
     */
    *out = DfStatus{};
    out->running = true;
    return false;
  }
  *out = status;
  xSemaphoreGive(statusLock);
#if PIN_DF_USB_DETECT >= 0
  // A wired VBUS sense outranks the module's notification: it is a wire, and it
  // is right even if the module missed the event or we booted with it plugged.
  out->pcLink = digitalRead(PIN_DF_USB_DETECT) == HIGH;
#endif
  return true;
}

/*
 * Publishing into player_state.h.
 *
 * The OLED screens are written against PlayerInfo and know nothing about any of
 * the sources. So the DFPlayer is described in the same terms: the "peer" is the
 * library it is playing from, the "title" is the track, and connected means the
 * module answered. Track length is not knowable -- the YX5200 reports neither
 * duration nor position -- so track_ms stays 0, which is exactly the case the
 * progress row already handles for phones that send no metadata.
 */
void df_player_loop() {
  if (!running) return;

  static uint32_t last;
  const uint32_t now = millis();
  if (now - last < 250) return;
  last = now;

  /*
   * Every write here is behind a change test, and that is not an optimisation.
   *
   * ps_set_net_connection() restamps connected_at and, when disconnecting,
   * clears the track text; ps_set_track_text() bumps track_seq whenever the
   * title differs from what is stored. Calling the pair unconditionally four
   * times a second would therefore restart the connection timer continuously
   * and -- while the module is offline, where the clear and the set disagree --
   * fire a track-change event on every pass, which the OLED shows as a toast
   * and main.cpp writes to the serial log. Same shape as net_audio_loop().
   */
  static bool last_online;
  static bool last_primed;
  static char last_title[PS_TITLE_MAX];
  static char last_detail[PS_TEXT_MAX];
  static PsPlayback last_playback = PS_STOPPED;
  static bool last_busy;
  static uint8_t last_volume = 0xFF;

  DfStatus s;
  // A stale copy would publish "No module" and then correct itself, which the
  // OLED shows as a toast and main.cpp writes to the log. Skipping the pass
  // costs 250 ms of latency and says nothing untrue.
  if (!df_player_snapshot(&s)) return;

  char title[PS_TITLE_MAX];
  // Standby before "no module": a sleeping module answers nothing, so `online`
  // is not the question and would give the wrong answer.
  if (s.asleep || s.state == DF_SLEEPING) {
    strlcpy(title, "Module in standby", sizeof(title));
  } else if (s.pcLink) {
    strlcpy(title, "Card mounted on a computer", sizeof(title));
  } else if (!s.online) {
    strlcpy(title, "No module", sizeof(title));
  } else if (s.state == DF_STOPPED && !s.busy) {
    strlcpy(title, "Nothing playing", sizeof(title));
  } else if (s.folder) {
    snprintf(title, sizeof(title), "Folder %u  track %u", (unsigned)s.folder,
             (unsigned)s.track);
  } else {
    snprintf(title, sizeof(title), "Track %u", (unsigned)s.track);
  }

  char detail[PS_TEXT_MAX];
  if (s.totalTracks) {
    snprintf(detail, sizeof(detail), "%s  %u files", df_source_name(s.source),
             (unsigned)s.totalTracks);
  } else {
    strlcpy(detail, df_source_name(s.source), sizeof(detail));
  }

  if (!last_primed || s.online != last_online) {
    last_primed = true;
    last_online = s.online;
    // Connecting clears nothing, but disconnecting wipes the track text -- so
    // the strings below have to be re-sent afterwards rather than suppressed by
    // a cache that still holds what was just thrown away.
    last_title[0] = last_detail[0] = 0;
    ps_set_net_connection(s.online, df_source_name(s.source));
  }
  if (strcmp(title, last_title) != 0 || strcmp(detail, last_detail) != 0) {
    strlcpy(last_title, title, sizeof(last_title));
    strlcpy(last_detail, detail, sizeof(last_detail));
    ps_set_track_text(title, detail, "");
  }
  const PsPlayback playback = s.state == DF_PLAYING  ? PS_PLAYING
                              : s.state == DF_PAUSED ? PS_PAUSED
                                                     : PS_STOPPED;
  if (playback != last_playback) {
    last_playback = playback;
    ps_set_playback(playback);
  }
  if (s.busy != last_busy) {
    last_busy = s.busy;
    ps_set_streaming(s.busy);
  }
  const uint8_t volume = df_player_volume();
  if (volume != last_volume) {
    last_volume = volume;
    ps_set_volume(volume);
  }
  // The module decodes at whatever the file is; it never says. 44.1 kHz is the
  // truth for all but a handful of oddly encoded files and is better than 0,
  // which the screens read as "unknown" and print as a dash. Written once --
  // ps_set_sample_rate has no change test of its own and this never changes.
  static bool rate_sent;
  if (!rate_sent) {
    rate_sent = true;
    ps_set_sample_rate(44100);
  }
}

// --- transport --------------------------------------------------------------

bool df_player_play() { return frame(CMD_PLAY); }
bool df_player_pause() { return frame(CMD_PAUSE); }
bool df_player_stop() { return frame(CMD_STOP); }
bool df_player_next() { return frame(CMD_NEXT); }
bool df_player_previous() { return frame(CMD_PREV); }

bool df_player_toggle() {
  return liveState == DF_PLAYING ? df_player_pause() : df_player_play();
}

bool df_player_play_track(uint16_t track) {
  if (track < 1 || track > 2999) return false;
  {
    Locked lock;
    if (lock.held) status.folder = 0;  // a flat index is not in any folder
  }
  return frame(CMD_TRACK, track);
}

bool df_player_play_folder(uint8_t folder, uint8_t file) {
  if (folder < 1 || folder > 99 || file < 1) return false;
  {
    Locked lock;
    if (lock.held) status.folder = folder;
  }
  return frame(CMD_FOLDER_FILE, (uint16_t)((folder << 8) | file));
}

bool df_player_play_mp3(uint16_t track) {
  if (track < 1 || track > 3000) return false;
  {
    Locked lock;
    if (lock.held) status.folder = 0;
  }
  return frame(CMD_MP3_FOLDER, track);
}

bool df_player_advertise(uint16_t track) {
  if (track < 1 || track > 3000) return false;
  return frame(CMD_ADVERT, track);
}

bool df_player_advertise_stop() { return frame(CMD_ADVERT_STOP); }

// --- volume, EQ, source, loop ----------------------------------------------

bool df_player_set_volume_raw(uint8_t volume) {
  if (volume > DF_VOLUME_MAX) volume = DF_VOLUME_MAX;
  {
    Locked lock;
    if (lock.held) status.volume = volume;
  }
  return frame(CMD_VOLUME, volume);
}

bool df_player_set_volume(uint8_t volume127) {
  if (volume127 > 127) volume127 = 127;
  // Round rather than truncate, or 127 lands on 29 and the top of the slider is
  // not the top of the module.
  return df_player_set_volume_raw(
      (uint8_t)(((uint16_t)volume127 * DF_VOLUME_MAX + 63) / 127));
}

uint8_t df_player_volume() {
  uint8_t raw = DF_VOLUME_DEFAULT;
  if (statusLock != nullptr &&
      xSemaphoreTake(statusLock, pdMS_TO_TICKS(20)) == pdTRUE) {
    raw = status.volume;
    xSemaphoreGive(statusLock);
  }
  return (uint8_t)(((uint16_t)raw * 127 + DF_VOLUME_MAX / 2) / DF_VOLUME_MAX);
}

bool df_player_volume_step(bool up) {
  return frame(up ? CMD_VOL_UP : CMD_VOL_DOWN);
}

bool df_player_set_eq(uint8_t eq) {
  if (eq > 5) return false;
  {
    Locked lock;
    if (lock.held) status.eq = eq;
  }
  return frame(CMD_EQ, eq);
}

bool df_player_set_source(DfSource source) {
  if (source != DF_SRC_USB && source != DF_SRC_SD && source != DF_SRC_AUX &&
      source != DF_SRC_FLASH) {
    return false;
  }
  activeSource = source;
  {
    Locked lock;
    if (lock.held) {
      status.source = source;
      // The new library's size and position are unknown until it answers, and
      // showing the last one's counts would be a lie the picker acts on.
      status.totalTracks = 0;
      status.track = 0;
      status.folder = 0;
      status.folderTracks = 0;
      status.queriedFolder = 0;
    }
  }
  dropFolderIndex();
  if (!frame(CMD_SOURCE, (uint16_t)source)) return false;
  // The module needs about 200 ms after a source change before it will answer
  // questions about the new one; the queue's own pacing covers that, and the
  // queries are behind it.
  return push(Command{OP_REFRESH, 0, 0});
}

bool df_player_set_loop(DfLoop mode, uint8_t folder) {
  bool ok;
  switch (mode) {
    case DF_LOOP_OFF:
      // Each loop mode has its own off switch and the module does not have a
      // "no loop" command. Clearing both of the two that can be left latched is
      // what actually stops it: 0x19/1 disables single-track repeat, 0x11/0
      // disables repeat-all. Folder loop and random both end at the next stop.
      ok = frame(CMD_LOOP_TRACK, 1) && frame(CMD_REPEAT_ALL, 0);
      break;
    case DF_LOOP_TRACK: ok = frame(CMD_LOOP_TRACK, 0); break;
    case DF_LOOP_ALL: ok = frame(CMD_REPEAT_ALL, 1); break;
    case DF_LOOP_FOLDER:
      if (folder < 1 || folder > 99) return false;
      ok = frame(CMD_LOOP_FOLDER, folder);
      break;
    case DF_LOOP_RANDOM: ok = frame(CMD_RANDOM); break;
    default: return false;
  }
  if (!ok) return false;
  Locked lock;
  if (lock.held) {
    status.loop = mode;
    if (mode == DF_LOOP_FOLDER) status.folder = folder;
  }
  return true;
}

bool df_player_set_dac(bool on) {
  {
    Locked lock;
    if (lock.held) status.dacOn = on;
  }
  return frame(CMD_DAC, on ? 0 : 1);  // the parameter is "disable", inverted
}

// --- housekeeping -----------------------------------------------------------

bool df_player_reset() {
  // `sleeping` is cleared by queueStartup() once its reset frame is queued, for
  // the reason given there -- not here, where the enqueue has not happened yet.
  {
    Locked lock;
    if (lock.held) {
      status.online = false;
      status.totalTracks = 0;
      status.track = 0;
      status.state = DF_STOPPED;
      status.error[0] = 0;
    }
  }
  liveState = DF_STOPPED;
  dropFolderIndex();
  return push(Command{OP_STARTUP, 0, 0});
}

bool df_player_standby() {
  if (!frame(CMD_STANDBY)) return false;
  // Set after the frame is queued, not before: if the queue is full the module
  // is not going to sleep, and a flag that said otherwise would switch the
  // poller off and leave the dashboard describing a state that never happened.
  sleeping = true;
  Locked lock;
  if (lock.held) {
    status.state = DF_SLEEPING;
    liveState = DF_SLEEPING;
    status.asleep = true;
    status.error[0] = 0;
  }
  return true;
}

bool df_player_wake() {
  if (!frame(CMD_WAKE)) return false;
  sleeping = false;
  liveState = DF_STOPPED;
  {
    Locked lock;
    if (lock.held) {
      status.state = DF_STOPPED;
      status.asleep = false;
      status.error[0] = 0;
      // The link went quiet for as long as it slept, so the stored timestamp is
      // older than the offline timeout and the very next tick would declare the
      // module dead. Restart the window; the refresh queued below is what
      // actually proves it is back.
      status.lastFrameAt = millis();
    }
  }
  return push(Command{OP_REFRESH, 0, 0});
}

bool df_player_refresh() { return push(Command{OP_REFRESH, 0, 0}); }

bool df_player_query_folder(uint8_t folder) {
  if (folder < 1 || folder > DF_MAX_FOLDERS) return false;
  {
    Locked lock;
    if (lock.held) {
      status.queriedFolder = folder;
      status.folderTracks = 0;
    }
  }
  // Also the folder the next 0x4E reply belongs to. A one-off query and the
  // scan use the same field because they use the same command, and the scan
  // only advances when the queue is empty, so the two cannot interleave.
  scanAt = folder;
  return frame(Q_FOLDER_FILES, folder);
}

// ----------------------------------------------------------- folder index ---
bool df_player_scan() {
  if (!running) return false;
  dropFolderIndex();
  scanNext = 1;
  scanSentAt = millis() - SCAN_GAP_MS;  // start on the next pass, not in 120 ms
  return true;
}

bool df_player_scanning(uint8_t *done, uint8_t *total) {
  const uint8_t next = scanNext;
  if (done) *done = next ? (uint8_t)(next - 1) : (scanComplete ? DF_MAX_FOLDERS : 0);
  if (total) *total = DF_MAX_FOLDERS;
  return next != 0;
}

bool df_player_scanned() { return scanComplete; }

void df_player_folder_counts(uint16_t *out, uint8_t count) {
  if (out == nullptr) return;
  if (count > DF_MAX_FOLDERS) count = DF_MAX_FOLDERS;
  memcpy(out, folderCounts, count * sizeof(folderCounts[0]));
}

// --- pins -------------------------------------------------------------------

bool df_player_pin_available(DfPin pin) {
  return pin < DF_PIN_COUNT && PIN_TABLE[pin] >= 0;
}

bool df_player_pulse(DfPin pin, bool long_press) {
  if (!df_player_pin_available(pin)) return false;
  const bool io = pin == DF_PIN_IO1 || pin == DF_PIN_IO2;
  const uint16_t ms = (io && long_press) ? DF_PRESS_LONG_MS
                      : io               ? DF_PRESS_SHORT_MS
                                         : DF_ADKEY_PRESS_MS;
  return push(Command{OP_PULSE, (uint8_t)pin, ms});
}

void df_player_set_led(DfLedMode mode) {
  if (mode > DF_LED_BLINK) mode = DF_LED_AUTO;
  push(Command{OP_LED, (uint8_t)mode, 0});
}

// --- names ------------------------------------------------------------------

const char *df_source_name(DfSource source) {
  switch (source) {
    case DF_SRC_USB: return "USB drive";
    case DF_SRC_SD: return "SD card";
    case DF_SRC_AUX: return "AUX input";
    case DF_SRC_FLASH: return "On-board flash";
    case DF_SRC_SLEEP: return "Asleep";
    default: return "Unknown";
  }
}

const char *df_state_name(DfState state) {
  switch (state) {
    case DF_PLAYING: return "playing";
    case DF_PAUSED: return "paused";
    case DF_SLEEPING: return "standby";
    default: return "stopped";
  }
}

const char *df_loop_name(DfLoop loop) {
  switch (loop) {
    case DF_LOOP_TRACK: return "track";
    case DF_LOOP_FOLDER: return "folder";
    case DF_LOOP_ALL: return "all";
    case DF_LOOP_RANDOM: return "random";
    default: return "off";
  }
}

const char *df_eq_name(uint8_t eq) {
  switch (eq) {
    case 1: return "pop";
    case 2: return "rock";
    case 3: return "jazz";
    case 4: return "classic";
    case 5: return "bass";
    default: return "normal";
  }
}

// --- console ----------------------------------------------------------------

bool df_player_command(const char *line) {
  if (strncmp(line, "df", 2) != 0) return false;
  const char *arg = line + 2;
  while (*arg == ' ') arg++;

  if (!running) {
    Serial.println("[df] the DFPlayer driver is not running in this mode; "
                   "type 'sd' to switch to DFPlayer mode");
    return true;
  }

  if (*arg == 0 || strcmp(arg, "status") == 0) {
    DfStatus s;
    // This is the diagnostic command, so it must not be the thing that lies. A
    // timed-out snapshot comes back zero-filled and would print "OFFLINE" about
    // a module that is answering -- exactly the wrong answer in the one place
    // somebody is looking for the truth.
    if (!df_player_snapshot(&s)) {
      Serial.println("[df] could not read the driver's status (its lock was "
                     "busy); try again");
      return true;
    }
    Serial.printf("[df] %s | %s | source %s",
                  s.asleep    ? "standby"
                  : s.online  ? "online"
                              : "OFFLINE",
                  df_state_name(s.state), df_source_name(s.source));
    if (s.reported != s.source) {
      Serial.printf(" (module says %s)", df_source_name(s.reported));
    }
    Serial.printf(" | track %u", (unsigned)s.track);
    if (s.totalTracks) Serial.printf("/%u", (unsigned)s.totalTracks);
    if (s.folder) Serial.printf(" | folder %u", (unsigned)s.folder);
    Serial.printf(" | vol %u/%u | eq %s | loop %s | busy %s", (unsigned)s.volume,
                  (unsigned)DF_VOLUME_MAX, df_eq_name(s.eq),
                  df_loop_name(s.loop), s.busy ? "yes" : "no");
    Serial.printf(" | media%s%s%s%s", s.sdPresent ? " sd" : "",
                  s.usbPresent ? " usb" : "", s.flashPresent ? " flash" : "",
                  s.pcLink ? " pc" : "");
    if (s.folders) Serial.printf(" | %u folders", (unsigned)s.folders);
    if (s.version) Serial.printf(" | fw %u", (unsigned)s.version);
    if (s.error[0]) Serial.printf("\n[df] last error: %s", s.error);
    Serial.println();
    return true;
  }

  if (strncmp(arg, "play", 4) == 0) {
    const char *n = arg + 4;
    while (*n == ' ') n++;
    if (*n) df_player_play_track((uint16_t)atoi(n));
    else df_player_play();
    Serial.println("[df] play");
    return true;
  }
  if (strcmp(arg, "pause") == 0) { df_player_pause(); Serial.println("[df] pause"); return true; }
  if (strcmp(arg, "stop") == 0) { df_player_stop(); Serial.println("[df] stop"); return true; }
  if (strcmp(arg, "next") == 0) { df_player_next(); Serial.println("[df] next"); return true; }
  if (strcmp(arg, "prev") == 0 || strcmp(arg, "previous") == 0) {
    df_player_previous();
    Serial.println("[df] previous");
    return true;
  }
  if (strncmp(arg, "vol ", 4) == 0) {
    const int v = atoi(arg + 4);
    df_player_set_volume_raw((uint8_t)constrain(v, 0, (int)DF_VOLUME_MAX));
    Serial.printf("[df] volume %d/%u\n", constrain(v, 0, (int)DF_VOLUME_MAX),
                  (unsigned)DF_VOLUME_MAX);
    return true;
  }
  if (strncmp(arg, "folder ", 7) == 0) {
    int folder = 0, file = 1;
    if (sscanf(arg + 7, "%d %d", &folder, &file) >= 1) {
      if (df_player_play_folder((uint8_t)folder, (uint8_t)file)) {
        Serial.printf("[df] folder %d track %d\n", folder, file);
      } else {
        Serial.println("[df] folder must be 1-99 and track 1-255");
      }
    }
    return true;
  }
  if (strncmp(arg, "source ", 7) == 0) {
    const char *s = arg + 7;
    const DfSource want = strcmp(s, "usb") == 0     ? DF_SRC_USB
                          : strcmp(s, "flash") == 0 ? DF_SRC_FLASH
                          : strcmp(s, "aux") == 0   ? DF_SRC_AUX
                                                    : DF_SRC_SD;
    df_player_set_source(want);
    Serial.printf("[df] source %s\n", df_source_name(want));
    return true;
  }
  if (strncmp(arg, "eq ", 3) == 0) {
    const int eq = atoi(arg + 3);
    if (df_player_set_eq((uint8_t)eq)) Serial.printf("[df] eq %s\n", df_eq_name((uint8_t)eq));
    else Serial.println("[df] eq must be 0-5 (normal pop rock jazz classic bass)");
    return true;
  }
  if (strncmp(arg, "loop ", 5) == 0) {
    const char *s = arg + 5;
    DfLoop mode = DF_LOOP_OFF;
    uint8_t folder = 1;
    if (strcmp(s, "track") == 0) mode = DF_LOOP_TRACK;
    else if (strncmp(s, "folder", 6) == 0) {
      mode = DF_LOOP_FOLDER;
      const int f = atoi(s + 6);
      if (f >= 1) folder = (uint8_t)f;
    } else if (strcmp(s, "all") == 0) mode = DF_LOOP_ALL;
    else if (strcmp(s, "random") == 0 || strcmp(s, "shuffle") == 0) mode = DF_LOOP_RANDOM;
    df_player_set_loop(mode, folder);
    Serial.printf("[df] loop %s\n", df_loop_name(mode));
    return true;
  }
  if (strcmp(arg, "reset") == 0) { df_player_reset(); Serial.println("[df] resetting module"); return true; }
  if (strcmp(arg, "sleep") == 0 || strcmp(arg, "standby") == 0) {
    df_player_standby();
    Serial.println("[df] standby");
    return true;
  }
  if (strcmp(arg, "wake") == 0) { df_player_wake(); Serial.println("[df] wake"); return true; }
  if (strcmp(arg, "refresh") == 0) { df_player_refresh(); Serial.println("[df] re-reading the card"); return true; }

  for (uint8_t i = 0; i < DF_PIN_COUNT; i++) {
    static const char *names[DF_PIN_COUNT] = {"io1", "io2", "key1", "key2"};
    const size_t n = strlen(names[i]);
    if (strncmp(arg, names[i], n) != 0) continue;
    const bool long_press = strstr(arg + n, "long") != nullptr;
    if (df_player_pulse((DfPin)i, long_press)) {
      Serial.printf("[df] %s %s press\n", names[i], long_press ? "long" : "short");
    } else {
      Serial.printf("[df] %s is not wired on this board\n", names[i]);
    }
    return true;
  }

  if (strncmp(arg, "led", 3) == 0) {
    const char *s = arg + 3;
    while (*s == ' ') s++;
    const DfLedMode mode = strcmp(s, "on") == 0      ? DF_LED_ON
                           : strcmp(s, "off") == 0   ? DF_LED_OFF
                           : strcmp(s, "blink") == 0 ? DF_LED_BLINK
                                                     : DF_LED_AUTO;
    df_player_set_led(mode);
    Serial.printf("[df] led %s\n", s && *s ? s : "auto");
    return true;
  }

  Serial.println("[df] try: df | df play [n] | df pause | df stop | df next | "
                 "df prev | df vol 0-30 | df folder F T | df source sd|usb|flash"
                 " | df eq 0-5 | df loop off|track|folderN|all|random | "
                 "df io1[long] | df io2[long] | df key1 | df key2 | "
                 "df led auto|on|off|blink | df reset | df standby | df wake");
  return true;
}

#endif  // DFPLAYER_ENABLED
