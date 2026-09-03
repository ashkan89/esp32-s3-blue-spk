/*
 * alarm_clock.h -- alarms and the sleep timer.
 *
 * A speaker with a clock on its display and a radio inside it is most of an
 * alarm clock already; this is the rest of it. Five alarms, each with its own
 * days, its own source and its own wake-up ramp, plus a sleep timer at the
 * other end of the day.
 *
 * What makes it worth calling smart, in the order the features earn it:
 *
 *   it wakes you with something          an alarm can start an internet radio
 *                                        station, a folder on the DFPlayer's
 *                                        card, or the built-in chime. Waking to
 *                                        the news is a different thing from
 *                                        waking to a beep.
 *   it fades up                          the volume ramps from near silence to
 *                                        the target over up to five minutes.
 *                                        This is the single feature that makes
 *                                        an alarm pleasant, and it costs a
 *                                        variable and a multiply.
 *   it falls back                        if the station will not connect -- the
 *                                        router is down, the stream moved -- the
 *                                        chime takes over after twenty seconds.
 *                                        An alarm that silently fails to make a
 *                                        sound is not an alarm, and a network
 *                                        this depends on is a network that will
 *                                        eventually be down at 6 a.m.
 *   it knows what day it is              per-weekday scheduling, and a one-shot
 *                                        mode for "tomorrow only".
 *   it can be skipped                    tomorrow's occurrence can be waved off
 *                                        without disarming the alarm, which is
 *                                        what everybody actually wants on a
 *                                        Friday night.
 *
 * What it runs on. soft_clock, which means whatever the clock is running on --
 * the DS3231 if one is fitted, SNTP when the network is up, and the persisted
 * time after a power cut. With a POSIX time zone rule configured, the alarm
 * follows daylight saving on its own; see soft_clock.h for why that is a
 * separate thing from the UTC offset and why an alarm is the reason it exists.
 *
 * Which modes. All three. The alarm itself is arithmetic on the wall clock and
 * needs no radio at all, and the chime plays through the same I2S channel the
 * melodies use -- so an alarm set from the dashboard in Wi-Fi mode still goes
 * off in Bluetooth mode, where there is no dashboard to set it from. Only the
 * sources differ: radio needs Wi-Fi, the card needs DFPlayer mode, and the
 * chime works everywhere, which is why it is also the fallback.
 *
 * Where it runs. alarm_loop(), from the Arduino loop task, once a pass. There is
 * no task and no timer: the whole of it is a comparison against the current
 * minute plus a volume ramp that updates ten times a second.
 */

#pragma once

#include <stdint.h>

/// How many alarms can be stored. Five is a weekday, a weekend, and three
/// spare; the dashboard draws them as a list and the NVS blob stays small.
static const uint8_t ALARM_MAX = 5;

static const uint8_t ALARM_LABEL_MAX = 24;

/// Weekday bits, matching struct tm's tm_wday so no conversion is needed.
enum AlarmDay : uint8_t {
  ALARM_SUN = 1 << 0,
  ALARM_MON = 1 << 1,
  ALARM_TUE = 1 << 2,
  ALARM_WED = 1 << 3,
  ALARM_THU = 1 << 4,
  ALARM_FRI = 1 << 5,
  ALARM_SAT = 1 << 6,
  ALARM_WEEKDAYS = ALARM_MON | ALARM_TUE | ALARM_WED | ALARM_THU | ALARM_FRI,
  ALARM_WEEKEND = ALARM_SAT | ALARM_SUN,
  ALARM_EVERY_DAY = 0x7F,
  /// No day bits set means "the next time this clock reaches that minute, once",
  /// which is what a one-shot alarm is. It disarms itself afterwards.
  ALARM_ONCE = 0,
};

/// What an alarm makes happen.
enum AlarmSource : uint8_t {
  ALARM_SRC_CHIME = 0,  ///< the built-in tone sequence; works in every mode
  ALARM_SRC_RADIO,      ///< an internet radio favourite; Wi-Fi modes only
  ALARM_SRC_DFPLAYER,   ///< a folder on the card; DFPlayer mode only
  ALARM_SRC_COUNT
};

struct Alarm {
  bool enabled;
  uint8_t hour;    ///< 0..23, local time
  uint8_t minute;  ///< 0..59
  uint8_t days;    ///< a bitwise or of AlarmDay, or ALARM_ONCE

  uint8_t source;  ///< an AlarmSource
  /// For ALARM_SRC_RADIO, the favourite's index. For ALARM_SRC_DFPLAYER, the
  /// folder number, 1..99. Ignored for the chime.
  uint8_t target;

  uint8_t volume;      ///< 0..127, what the ramp climbs to
  uint16_t fadeSecs;   ///< how long the ramp takes; 0 starts at full volume
  uint16_t durationSecs;  ///< stops itself after this long, so a house nobody is
                          ///< in does not play the radio for a week
  uint8_t snoozeMins;  ///< how long snooze lasts

  /// True to sit out the next occurrence and then behave normally. Cleared when
  /// that occurrence passes.
  bool skipNext;

  char label[ALARM_LABEL_MAX];
};

/// What the alarm engine is doing.
enum AlarmState : uint8_t {
  ALARM_IDLE = 0,
  ALARM_RINGING,
  ALARM_SNOOZED,
};

struct AlarmStatus {
  AlarmState state;
  int8_t active;      ///< which alarm is ringing or snoozed, or -1
  int8_t next;        ///< which alarm fires next, or -1 if none is armed
  uint32_t nextInSecs;  ///< how long until it does; 0 when there is no next
  uint32_t ringingForSecs;
  uint32_t snoozeLeftSecs;
  uint8_t rampVolume;   ///< where the fade has got to, 0..127

  /// The sleep timer, which shares this status because the dashboard shows the
  /// two together and because they are the same idea pointing in opposite
  /// directions.
  bool sleepRunning;
  uint32_t sleepLeftSecs;
  uint32_t sleepTotalSecs;
};

/*
 * The hooks main.cpp fills in.
 *
 * Three things this module needs are owned by main.cpp and cannot be reached
 * from here without dragging the Bluetooth stack into every translation unit
 * that wants to know what time the alarm is set for: the volume of whatever
 * source is live, the chime, and a way to stop the audio. They are function
 * pointers rather than includes for exactly that reason.
 *
 * All three may be null, in which case the alarm still fires, still announces
 * itself and still shows on the display -- it simply makes no sound, which is
 * the correct behaviour for a build with no audio path rather than a crash.
 */
typedef void (*AlarmVolumeFn)(uint8_t volume127);
typedef bool (*AlarmChimeFn)(bool start);
typedef void (*AlarmStopFn)(void);

void alarm_set_hooks(AlarmVolumeFn setVolume, AlarmChimeFn chime, AlarmStopFn stop);

/// Reads the stored alarms and the sleep-timer preference. Call after
/// soft_clock_begin(), because arming depends on knowing what time it is.
void alarm_begin();

/// Services the clock comparison, the fade ramp and the sleep countdown.
/// Arduino loop task only.
void alarm_loop();

void alarm_status(AlarmStatus *out);

uint8_t alarm_count();

/// Copies one out. False if `index` is past ALARM_MAX.
bool alarm_get(uint8_t index, Alarm *out);

/// Stores one, clamping every field. Writing to an index at or past the current
/// count appends. Persists immediately -- alarms are set rarely and losing one
/// to a power cut is the failure that matters here.
bool alarm_set(uint8_t index, const Alarm &alarm);

/// Removes one, closing the gap.
bool alarm_remove(uint8_t index);

/// Silences a ringing alarm until its next scheduled occurrence.
void alarm_dismiss();

/// Silences it for the alarm's own snooze length. Does nothing when nothing is
/// ringing, so it is safe to wire straight to a button.
void alarm_snooze();

/// Fires an alarm now, whatever the clock says -- the dashboard's "test" button.
/// Nothing else in the firmware calls this.
bool alarm_trigger(uint8_t index);

// ------------------------------------------------------------ sleep timer --

/*
 * Starts the sleep timer.
 *
 * `minutes` of playing, then the audio fades out over the last minute and
 * stops. If `standby` is set the speaker then goes into standby proper, which
 * is a deeper thing than stopping the music -- see power.h -- and is what you
 * want overnight on a battery.
 *
 * Passing 0 cancels a running timer, which is also what alarm_sleep_cancel()
 * does; both exist because the dashboard's control is a single field and the
 * OLED's is a button.
 */
bool alarm_sleep_start(uint16_t minutes, bool standby);
void alarm_sleep_cancel();

/// Adds to a running timer, or starts one if none is running. What a "+15 min"
/// button does.
bool alarm_sleep_extend(uint16_t minutes);

/// Whether the sleep timer's default is to go to standby at the end.
bool alarm_sleep_standby_default();
void alarm_set_sleep_standby_default(bool on);

/// Serial console: "alarm", "alarm off", "alarm snooze", "sleep <minutes>".
/// Returns false if the line was something else.
bool alarm_command(const char *line);
