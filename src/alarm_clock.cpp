#include "app_config.h"
#include "alarm_clock.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>
#include <time.h>

#include "df_player.h"
#include "net_radio.h"
#include "power.h"
#include "soft_clock.h"
#include "ui.h"
#include "voice.h"

namespace {

Preferences prefs;
bool prefsOk;

Alarm alarms[ALARM_MAX];
uint8_t alarmCount;

AlarmVolumeFn setVolumeHook;
AlarmChimeFn chimeHook;
AlarmStopFn stopHook;

AlarmState state = ALARM_IDLE;
int8_t activeIndex = -1;

uint32_t ringStartedMs;
uint32_t snoozeUntilMs;
uint8_t rampVolume;
uint32_t lastRampMs;
bool chimeRunning;

/*
 * How long a radio alarm is given to make a sound before the chime takes over.
 *
 * Twenty seconds is roughly twice a healthy connect-and-prebuffer, and well
 * inside the time somebody would otherwise lie there wondering whether the
 * alarm was going to happen. The fallback is the whole reason an alarm may
 * depend on the internet at all: without it, "the router rebooted overnight"
 * and "you are late for work" are the same event.
 */
const uint32_t SOURCE_GRACE_MS = 20000;

/// How often the fade ramp is recomputed. Ten times a second is far finer than
/// a volume step can be heard at, and it keeps the arithmetic trivially cheap.
const uint32_t RAMP_INTERVAL_MS = 100;

/// The alarm starts here rather than at zero: a ramp from actual silence spends
/// its first minute inaudible, which is a minute of the fade wasted.
const uint8_t RAMP_FLOOR = 6;

/// The sleep timer's fade-out, at the end. One minute, which is long enough to
/// be a fade and short enough that a timer set for thirty minutes plays for
/// twenty-nine of them.
const uint32_t SLEEP_FADE_MS = 60000;

// The sleep timer.
bool sleepRunning;
bool sleepStandby;
bool sleepStandbyDefault;
uint32_t sleepEndsMs;
uint32_t sleepTotalMs;
uint8_t sleepFromVolume;

/*
 * Which minute each alarm last fired in.
 *
 * The comparison is against the wall clock at minute resolution, so without
 * this an alarm at 07:00 fires on every pass of loop() for sixty seconds. The
 * value is the day-of-year and minute-of-day packed together, which is unique
 * within a year and cheap to compute -- and a clock that jumps backwards past
 * one (an SNTP correction, say) simply re-arms the alarm, which is the safe
 * direction for that error to go.
 */
uint32_t lastFiredStamp[ALARM_MAX];

uint32_t minuteStamp(const struct tm &t) {
  return (uint32_t)t.tm_yday * 1440u + (uint32_t)t.tm_hour * 60u + t.tm_min;
}

void saveAlarms() {
  if (!prefsOk) return;
  prefs.putBytes("list", alarms, sizeof(Alarm) * alarmCount);
  prefs.putUChar("count", alarmCount);
}

void clampAlarm(Alarm *a) {
  if (a->hour > 23) a->hour = 23;
  if (a->minute > 59) a->minute = 59;
  a->days &= ALARM_EVERY_DAY;
  if (a->source >= ALARM_SRC_COUNT) a->source = ALARM_SRC_CHIME;
  if (a->volume > 127) a->volume = 127;
  if (a->fadeSecs > 600) a->fadeSecs = 600;
  // A duration of zero would be an alarm that never stops itself, and the one
  // certain thing about an alarm nobody is there to dismiss is that it should
  // eventually stop. An hour is the ceiling and also the default.
  if (a->durationSecs == 0 || a->durationSecs > 3600) a->durationSecs = 3600;
  if (a->snoozeMins == 0 || a->snoozeMins > 60) a->snoozeMins = 9;
  a->label[ALARM_LABEL_MAX - 1] = 0;
}

/*
 * Seconds from `now` until this alarm is next scheduled, or 0 if it never is.
 *
 * "Scheduled" rather than "will make a noise": an alarm marked skipNext is
 * still scheduled for tomorrow, it simply will not sound then. The dashboard
 * shows both facts side by side, so conflating them here would lose the one
 * that explains the other.
 */
uint32_t secondsUntilScheduled(const Alarm &a, const struct tm &now) {
  if (!a.enabled) return 0;
  const int32_t nowSecs = now.tm_hour * 3600 + now.tm_min * 60 + now.tm_sec;
  const int32_t atSecs = a.hour * 3600 + a.minute * 60;

  if (a.days == ALARM_ONCE) {
    int32_t delta = atSecs - nowSecs;
    if (delta <= 0) delta += 86400;
    return (uint32_t)delta;
  }
  for (uint8_t ahead = 0; ahead <= 7; ahead++) {
    const uint8_t wday = (uint8_t)((now.tm_wday + ahead) % 7);
    if (!(a.days & (1 << wday))) continue;
    const int32_t delta = atSecs - nowSecs + (int32_t)ahead * 86400;
    if (delta > 0) return (uint32_t)delta;
  }
  return 0;
}

void applyVolume(uint8_t volume) {
  rampVolume = volume;
  if (setVolumeHook) setVolumeHook(volume);
}

void stopAudio() {
  if (chimeRunning && chimeHook) chimeHook(false);
  chimeRunning = false;
  if (stopHook) stopHook();
}

/*
 * Starts whatever this alarm is supposed to play.
 *
 * The chime is started immediately in every case where the chosen source cannot
 * possibly work in this radio mode -- a radio alarm in Bluetooth mode, a card
 * alarm with no module -- rather than being left to the twenty-second fallback.
 * The fallback is for a source that should work and does not; a source that
 * cannot work in this mode is knowable now, and twenty seconds of silence for
 * something already known is twenty seconds of a person wondering.
 */
void startSource(const Alarm &a) {
  chimeRunning = false;

  switch (a.source) {
    case ALARM_SRC_RADIO:
      if (net_radio_running() && net_radio_play_station(a.target)) return;
      break;
    case ALARM_SRC_DFPLAYER:
      if (df_player_running()) {
        const uint8_t folder = a.target ? a.target : 1;
        if (df_player_play_folder(folder, 1)) return;
      }
      break;
    default:
      break;
  }

  if (chimeHook && chimeHook(true)) chimeRunning = true;
}

/// True once the chosen source is actually making a sound.
bool sourceAudible(const Alarm &a) {
  if (chimeRunning) return true;
  switch (a.source) {
    case ALARM_SRC_RADIO: return net_radio_active();
    case ALARM_SRC_DFPLAYER: return df_player_active();
    default: return false;
  }
}

void beginRinging(uint8_t index) {
  activeIndex = (int8_t)index;
  state = ALARM_RINGING;
  ringStartedMs = millis();
  lastRampMs = ringStartedMs;

  const Alarm &a = alarms[index];
  applyVolume(a.fadeSecs ? RAMP_FLOOR : a.volume);
  startSource(a);

  ui_wake();
  char detail[48];
  if (a.label[0]) snprintf(detail, sizeof(detail), "%s", a.label);
  else snprintf(detail, sizeof(detail), "%02u:%02u", (unsigned)a.hour, (unsigned)a.minute);
  ui_show_system_status(UI_STATUS_NETWORK, "Alarm", detail, -1, 0);
  voice_say(VOICE_ALARM, VOICE_CAT_ALARM);

  LOGF("[alarm] %u ringing (%s)\n", (unsigned)index + 1,
                a.label[0] ? a.label : "no label");
}

void endRinging(const char *why) {
  if (activeIndex >= 0) {
    Alarm &a = alarms[activeIndex];
    if (a.days == ALARM_ONCE) {
      // A one-shot has now happened. Disarming rather than deleting keeps it on
      // the dashboard, where it can be switched back on with one press.
      a.enabled = false;
      saveAlarms();
    }
  }
  stopAudio();
  state = ALARM_IDLE;
  activeIndex = -1;
  ui_show_system_status(UI_STATUS_SUCCESS, "Alarm off", why, -1, 2000);
  LOGF("[alarm] stopped: %s\n", why);
}

void serviceRinging() {
  const Alarm &a = alarms[activeIndex];
  const uint32_t now = millis();
  const uint32_t elapsed = now - ringStartedMs;

  if (elapsed >= (uint32_t)a.durationSecs * 1000u) {
    endRinging("Timed out");
    return;
  }

  // Nothing audible after the grace period: the station did not come up, or the
  // card would not read. Fall back to the one source that cannot fail.
  if (!chimeRunning && elapsed > SOURCE_GRACE_MS && !sourceAudible(a)) {
    LOGLN("[alarm] the chosen source did not start; falling back to the chime");
    if (chimeHook && chimeHook(true)) chimeRunning = true;
  }

  if (a.fadeSecs && now - lastRampMs >= RAMP_INTERVAL_MS) {
    lastRampMs = now;
    const uint32_t fadeMs = (uint32_t)a.fadeSecs * 1000u;
    const uint32_t done = elapsed < fadeMs ? elapsed : fadeMs;
    const uint32_t span = a.volume > RAMP_FLOOR ? a.volume - RAMP_FLOOR : 0;
    const uint8_t want = (uint8_t)(RAMP_FLOOR + (span * done) / (fadeMs ? fadeMs : 1));
    if (want != rampVolume) applyVolume(want);
  }
}

void serviceSleep() {
  if (!sleepRunning) return;
  const uint32_t now = millis();
  const int32_t left = (int32_t)(sleepEndsMs - now);

  if (left <= 0) {
    sleepRunning = false;
    stopAudio();
    LOGLN("[alarm] sleep timer finished");
    voice_say(VOICE_SLEEP_ENDING, VOICE_CAT_ALARM);
    if (sleepStandby && power_sleep_possible()) {
      // power_sleep_now() draws its own farewell screen and does not return.
      ui_show_system_status(UI_STATUS_GOODBYE, "Sleep timer", "Going to standby", -1, 0);
      power_sleep_now();
    } else {
      applyVolume(sleepFromVolume);
      ui_show_system_status(UI_STATUS_SUCCESS, "Sleep timer", "Playback stopped", -1, 3000);
    }
    return;
  }

  // The last minute is a fade rather than a cliff. Announcing it as well would
  // be a voice waking somebody the timer exists to let fall asleep.
  if ((uint32_t)left <= SLEEP_FADE_MS) {
    const uint32_t gone = SLEEP_FADE_MS - (uint32_t)left;
    const uint8_t want = (uint8_t)(sleepFromVolume -
                                   (uint32_t)sleepFromVolume * gone / SLEEP_FADE_MS);
    if (want != rampVolume) applyVolume(want);
  }
}

const char *sourceName(uint8_t source) {
  switch (source) {
    case ALARM_SRC_RADIO: return "radio";
    case ALARM_SRC_DFPLAYER: return "card";
    default: return "chime";
  }
}

}  // namespace

void alarm_set_hooks(AlarmVolumeFn setVolume, AlarmChimeFn chime, AlarmStopFn stop) {
  setVolumeHook = setVolume;
  chimeHook = chime;
  stopHook = stop;
}

void alarm_begin() {
  prefsOk = prefs.begin("alarm", false);
  if (!prefsOk) return;

  sleepStandbyDefault = prefs.getBool("sleepstby", true);

  const size_t have = prefs.getBytesLength("list");
  if (have > 0 && have <= sizeof(alarms)) {
    prefs.getBytes("list", alarms, have);
    alarmCount = prefs.getUChar("count", 0);
    const uint8_t fits = (uint8_t)(have / sizeof(Alarm));
    if (alarmCount > fits) alarmCount = fits;
    if (alarmCount > ALARM_MAX) alarmCount = ALARM_MAX;
    for (uint8_t i = 0; i < alarmCount; i++) clampAlarm(&alarms[i]);
  }

  /*
   * The alarms are compared against the clock, and a clock that is only running
   * on the build timestamp is not a clock. Rather than fire every alarm at once
   * on the way past whatever fictional time that is, the fired-stamps are
   * primed with the current minute, so nothing can go off until the clock has
   * genuinely advanced past an alarm's time.
   */
  struct tm now;
  soft_clock_now(&now);
  const uint32_t stamp = minuteStamp(now);
  for (uint8_t i = 0; i < ALARM_MAX; i++) lastFiredStamp[i] = stamp;

  if (alarmCount) {
    LOGF("[alarm] %u stored\n", (unsigned)alarmCount);
  }
}

void alarm_loop() {
  serviceSleep();

  if (state == ALARM_RINGING) {
    serviceRinging();
    return;
  }

  if (state == ALARM_SNOOZED) {
    if ((int32_t)(snoozeUntilMs - millis()) <= 0 && activeIndex >= 0) {
      beginRinging((uint8_t)activeIndex);
    }
    return;
  }

  if (!alarmCount) return;

  // Only worth looking once a second; the resolution is a minute.
  static uint32_t lastCheckMs;
  const uint32_t now = millis();
  if (now - lastCheckMs < 1000) return;
  lastCheckMs = now;

  struct tm local;
  soft_clock_now(&local);
  const uint32_t stamp = minuteStamp(local);

  for (uint8_t i = 0; i < alarmCount; i++) {
    Alarm &a = alarms[i];
    if (!a.enabled) continue;
    if (a.hour != local.tm_hour || a.minute != local.tm_min) continue;
    if (a.days != ALARM_ONCE && !(a.days & (1 << local.tm_wday))) continue;
    if (lastFiredStamp[i] == stamp) continue;

    lastFiredStamp[i] = stamp;
    if (a.skipNext) {
      a.skipNext = false;
      saveAlarms();
      LOGF("[alarm] %u skipped as asked\n", (unsigned)i + 1);
      continue;
    }
    beginRinging(i);
    return;
  }
}

void alarm_status(AlarmStatus *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  out->state = state;
  out->active = activeIndex;
  out->next = -1;
  out->rampVolume = rampVolume;

  if (state == ALARM_RINGING) out->ringingForSecs = (millis() - ringStartedMs) / 1000;
  if (state == ALARM_SNOOZED) {
    const int32_t left = (int32_t)(snoozeUntilMs - millis());
    out->snoozeLeftSecs = left > 0 ? (uint32_t)left / 1000 : 0;
  }

  struct tm local;
  soft_clock_now(&local);
  uint32_t best = 0;
  for (uint8_t i = 0; i < alarmCount; i++) {
    const uint32_t secs = secondsUntilScheduled(alarms[i], local);
    if (!secs) continue;
    if (best == 0 || secs < best) {
      best = secs;
      out->next = (int8_t)i;
    }
  }
  out->nextInSecs = best;

  out->sleepRunning = sleepRunning;
  out->sleepTotalSecs = sleepTotalMs / 1000;
  if (sleepRunning) {
    const int32_t left = (int32_t)(sleepEndsMs - millis());
    out->sleepLeftSecs = left > 0 ? (uint32_t)left / 1000 : 0;
  }
}

uint8_t alarm_count() { return alarmCount; }

bool alarm_get(uint8_t index, Alarm *out) {
  if (!out || index >= alarmCount) return false;
  *out = alarms[index];
  return true;
}

bool alarm_set(uint8_t index, const Alarm &alarm) {
  if (index >= alarmCount) {
    if (alarmCount >= ALARM_MAX) return false;
    index = alarmCount++;
  }
  alarms[index] = alarm;
  clampAlarm(&alarms[index]);
  // An alarm edited into the current minute must not fire the instant it is
  // saved -- somebody setting 07:00 at 07:00 means tomorrow.
  struct tm now;
  soft_clock_now(&now);
  lastFiredStamp[index] = minuteStamp(now);
  saveAlarms();
  voice_say(VOICE_ALARM_SET, VOICE_CAT_ALARM);
  return true;
}

bool alarm_remove(uint8_t index) {
  if (index >= alarmCount) return false;
  if (activeIndex == (int8_t)index) alarm_dismiss();
  for (uint8_t i = index; i + 1 < alarmCount; i++) {
    alarms[i] = alarms[i + 1];
    lastFiredStamp[i] = lastFiredStamp[i + 1];
  }
  alarmCount--;
  memset(&alarms[alarmCount], 0, sizeof(Alarm));
  if (activeIndex > (int8_t)index) activeIndex--;
  saveAlarms();
  return true;
}

void alarm_dismiss() {
  if (state == ALARM_IDLE) return;
  endRinging("Dismissed");
}

void alarm_snooze() {
  if (state != ALARM_RINGING || activeIndex < 0) return;
  const Alarm &a = alarms[activeIndex];
  stopAudio();
  state = ALARM_SNOOZED;
  snoozeUntilMs = millis() + (uint32_t)a.snoozeMins * 60000u;
  ui_wake();
  char detail[32];
  snprintf(detail, sizeof(detail), "%u more minutes", (unsigned)a.snoozeMins);
  ui_show_system_status(UI_STATUS_SUCCESS, "Snoozed", detail, -1, 3000);
  voice_say(VOICE_ALARM_SNOOZE, VOICE_CAT_ALARM);
  LOGF("[alarm] snoozed for %u min\n", (unsigned)a.snoozeMins);
}

bool alarm_trigger(uint8_t index) {
  if (index >= alarmCount) return false;
  if (state != ALARM_IDLE) alarm_dismiss();
  beginRinging(index);
  return true;
}

bool alarm_sleep_start(uint16_t minutes, bool standby) {
  if (minutes == 0) {
    alarm_sleep_cancel();
    return true;
  }
  if (minutes > 600) minutes = 600;
  sleepTotalMs = (uint32_t)minutes * 60000u;
  sleepEndsMs = millis() + sleepTotalMs;
  sleepStandby = standby;
  // Where the fade starts from. Captured now rather than at fade time because
  // by then it has already been changed by the fade itself.
  sleepFromVolume = rampVolume ? rampVolume : 90;
  sleepRunning = true;

  ui_wake();
  char detail[40];
  snprintf(detail, sizeof(detail), "%u min%s", (unsigned)minutes,
           standby ? ", then standby" : "");
  ui_show_system_status(UI_STATUS_SUCCESS, "Sleep timer", detail, -1, 3000);
  voice_say(VOICE_SLEEP_TIMER, VOICE_CAT_ALARM);
  LOGF("[alarm] sleep timer %u min%s\n", (unsigned)minutes,
                standby ? " then standby" : "");
  return true;
}

void alarm_sleep_cancel() {
  if (!sleepRunning) return;
  sleepRunning = false;
  // The fade may already have pulled the volume down; put it back rather than
  // leaving the owner wondering why cancelling made things quieter.
  applyVolume(sleepFromVolume);
  ui_show_system_status(UI_STATUS_SUCCESS, "Sleep timer", "Cancelled", -1, 2500);
  LOGLN("[alarm] sleep timer cancelled");
}

bool alarm_sleep_extend(uint16_t minutes) {
  if (!minutes) return false;
  if (!sleepRunning) return alarm_sleep_start(minutes, sleepStandbyDefault);
  sleepEndsMs += (uint32_t)minutes * 60000u;
  sleepTotalMs += (uint32_t)minutes * 60000u;
  // Extending past the fade means the fade has to be undone.
  if ((int32_t)(sleepEndsMs - millis()) > (int32_t)SLEEP_FADE_MS) {
    applyVolume(sleepFromVolume);
  }
  return true;
}

bool alarm_sleep_standby_default() { return sleepStandbyDefault; }

void alarm_set_sleep_standby_default(bool on) {
  sleepStandbyDefault = on;
  if (prefsOk) prefs.putBool("sleepstby", on);
}

bool alarm_command(const char *line) {
  if (!line) return false;

  if (strncmp(line, "sleep", 5) == 0) {
    const char *rest = line + 5;
    while (*rest == ' ') rest++;
    if (*rest == 0) {
      AlarmStatus s;
      alarm_status(&s);
      if (s.sleepRunning) {
        LOGF("[alarm] sleep timer: %lu min %lu s left of %lu min\n",
                      (unsigned long)(s.sleepLeftSecs / 60),
                      (unsigned long)(s.sleepLeftSecs % 60),
                      (unsigned long)(s.sleepTotalSecs / 60));
      } else {
        LOGLN("[alarm] no sleep timer running; 'sleep <minutes>' starts one");
      }
      return true;
    }
    if (strcmp(rest, "off") == 0 || strcmp(rest, "cancel") == 0) {
      alarm_sleep_cancel();
      return true;
    }
    const int minutes = atoi(rest);
    if (minutes > 0) {
      alarm_sleep_start((uint16_t)minutes, sleepStandbyDefault);
      return true;
    }
    LOGLN("[alarm] usage: sleep <minutes> | sleep off");
    return true;
  }

  if (strncmp(line, "alarm", 5) != 0) return false;
  const char *rest = line + 5;
  while (*rest == ' ') rest++;

  if (strcmp(rest, "off") == 0 || strcmp(rest, "stop") == 0) {
    alarm_dismiss();
    return true;
  }
  if (strcmp(rest, "snooze") == 0) {
    alarm_snooze();
    return true;
  }
  if (strncmp(rest, "test", 4) == 0) {
    const int which = atoi(rest + 4);
    if (!alarm_trigger((uint8_t)(which > 0 ? which - 1 : 0))) {
      LOGLN("[alarm] no such alarm");
    }
    return true;
  }

  AlarmStatus s;
  alarm_status(&s);
  LOGF("[alarm] %s", s.state == ALARM_RINGING  ? "ringing"
                              : s.state == ALARM_SNOOZED ? "snoozed"
                                                         : "idle");
  if (s.state == ALARM_SNOOZED) LOGF(" (%lu s)", (unsigned long)s.snoozeLeftSecs);
  if (s.next >= 0) {
    LOGF(" | next in %lu h %lu m", (unsigned long)(s.nextInSecs / 3600),
                  (unsigned long)((s.nextInSecs % 3600) / 60));
  }
  if (s.sleepRunning) {
    LOGF(" | sleep %lu min left", (unsigned long)(s.sleepLeftSecs / 60));
  }
  LOGLN();

  static const char *const DAY_LETTER = "SMTWTFS";
  for (uint8_t i = 0; i < alarmCount; i++) {
    const Alarm &a = alarms[i];
    char days[9] = "-------";
    if (a.days == ALARM_ONCE) {
      snprintf(days, sizeof(days), "once");
    } else {
      for (uint8_t d = 0; d < 7; d++) {
        if (a.days & (1 << d)) days[d] = DAY_LETTER[d];
      }
    }
    LOGF("  %u%c %02u:%02u %-7s %-5s vol %u fade %us%s%s%s\n",
                  (unsigned)(i + 1), a.enabled ? '*' : '.', (unsigned)a.hour,
                  (unsigned)a.minute, days, sourceName(a.source),
                  (unsigned)a.volume, (unsigned)a.fadeSecs,
                  a.skipNext ? " [skip next]" : "", a.label[0] ? " " : "", a.label);
  }
  if (!alarmCount) LOGLN("  (no alarms set; the dashboard's Alarms page adds them)");
  return true;
}
