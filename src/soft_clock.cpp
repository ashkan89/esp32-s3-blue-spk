#include "app_config.h"
#include "soft_clock.h"

#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>
#include <esp_sntp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "ui_config.h"

#ifndef USE_DS3231
#define USE_DS3231 0
#endif

static ClockSource g_source = CLOCK_SRC_BUILD;
static Preferences g_prefs;
static uint32_t g_last_persist_ms;
static bool g_prefs_ok;

/// Minutes east of UTC. Seeded from CLOCK_TZ_OFFSET_MIN, then whatever the
/// dashboard last learned from the browser.
static int32_t g_offset_min = CLOCK_TZ_OFFSET_MIN;

/// Set from the SNTP callback, which runs on lwIP's thread -- so it does
/// nothing but raise this, and soft_clock_tick() does the work.
static volatile bool g_ntp_fresh;
static bool g_ntp_running;
static bool g_ntp_synced;

/// An SNTP answer only counts once it is plausibly this decade. The IDF only
/// calls the notification for a real reply, but a garbage timestamp from a
/// broken server would otherwise be adopted as gospel.
static const time_t EPOCH_SANITY_FLOOR = 1700000000;  // late 2023

/// NVS is written at most this often. Ten minutes is 144 writes a day, which the
/// flash wear levelling shrugs off, and bounds how far behind a power cut can
/// leave the clock.
static const uint32_t PERSIST_EVERY_MS = 10UL * 60UL * 1000UL;

/*
 * The saved epoch is UTC, and the key says so.
 *
 * Firmware before the automatic network sync kept *local* time in the system
 * clock and left TZ unset, so its saved "epoch" is out by the UTC offset. There
 * is no way to tell the two apart from the number itself, and restoring one as
 * the other silently shifts the clock -- so the new meaning gets a new key and
 * the old one is simply ignored. The cost is one boot on the build stamp.
 */
static const char *EPOCH_KEY = "utc";
static const char *OFFSET_KEY = "tzmin";
static const char *H24_KEY = "h24";
static const char *SYNC_KEY = "autosync";
static const char *ZONE_KEY = "tzrule";

/// Presentation, not time: 24-hour or 12-hour with AM/PM. CLOCK_24H is only the
/// value a speaker that has never been opened in a browser comes up with.
static bool g_use_24h = CLOCK_24H != 0;

/// Whether SNTP is allowed to correct the clock. Off is for an owner who set the
/// time by hand and wants it kept, and it is checked in network_begin() so no
/// caller has to remember.
static bool g_auto_sync = true;

// ------------------------------------------------------------ build stamp ----
/*
 * __DATE__ is "Aug 18 2026" (day space-padded) and __TIME__ is "14:29:33", both
 * in the compiler locale, so the month has to be looked up rather than parsed.
 */
static time_t build_epoch() {
  static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon[4] = {__DATE__[0], __DATE__[1], __DATE__[2], 0};
  const char *found = strstr(months, mon);

  struct tm t = {};
  t.tm_mon = found ? (int)((found - months) / 3) : 0;
  t.tm_mday = atoi(__DATE__ + 4);
  t.tm_year = atoi(__DATE__ + 7) - 1900;
  t.tm_hour = atoi(__TIME__);
  t.tm_min = atoi(__TIME__ + 3);
  t.tm_sec = atoi(__TIME__ + 6);
  t.tm_isdst = 0;
  return mktime(&t);
}

static void apply_epoch(time_t epoch) {
  struct timeval tv = {.tv_sec = epoch, .tv_usec = 0};
  settimeofday(&tv, nullptr);
}

// ------------------------------------------------------------- time zone ----
/*
 * The system clock holds UTC and TZ turns it into local time. That is the only
 * arrangement in which an SNTP answer -- which is UTC, and which the IDF writes
 * straight into the system clock behind our back -- and a hand-set local time
 * can both be right at once.
 *
 * POSIX writes the offset with the sign inverted (west is positive), so UTC+5:30
 * is spelled "UTC-5:30". Naming the zone "UTC" regardless is deliberate: the
 * device has no zone database, only an offset, and pretending otherwise by
 * printing "CEST" somewhere would be a lie half the year.
 */
static void timezone_string(char *out, size_t len) {
  const int32_t posix = -g_offset_min;  // west-positive, as POSIX wants it
  const int32_t magnitude = posix < 0 ? -posix : posix;
  snprintf(out, len, "UTC%c%ld:%02ld", posix < 0 ? '-' : '+',
           (long)(magnitude / 60), (long)(magnitude % 60));
}

/*
 * The POSIX TZ rule, when one has been chosen, and the offset it works out to
 * right now.
 *
 * These two are kept in step deliberately. Everything else in the firmware --
 * the SNTP path, the DS3231 write, the dashboard, the alarm -- was written
 * against a single integer offset, and the honest way to slide a zone with a
 * daylight-saving rule underneath all of it is to let that integer keep meaning
 * what it always meant and simply recompute it whenever the rule says it has
 * changed. Nothing downstream had to learn anything new.
 */
static char g_zone[48];

/// How often the cached offset is re-derived from the rule. See soft_clock_tick().
static const uint32_t ZONE_RECHECK_MS = 60000;
static uint32_t g_last_zone_check_ms;

/// The offset the installed zone is on at instant `when`, in minutes east of
/// UTC. Derived the only way a POSIX zone can be interrogated: ask for the same
/// instant twice, once local and once UTC, and subtract.
static int32_t zone_offset_at(time_t when) {
  struct tm local = {}, utc = {};
  localtime_r(&when, &local);
  gmtime_r(&when, &utc);
  local.tm_isdst = 0;
  utc.tm_isdst = 0;
  const double delta = difftime(mktime(&local), mktime(&utc));
  return (int32_t)(delta / 60.0);
}

static void apply_timezone() {
  if (g_zone[0]) {
    setenv("TZ", g_zone, 1);
    tzset();
    // The rule is authoritative now, so the stored offset becomes a readout of
    // it rather than a setting of its own. It is re-derived on every tick as
    // well, which is what carries the clock across a daylight-saving
    // transition without anybody having to do anything.
    const int32_t derived = zone_offset_at(time(nullptr));
    if (derived >= -840 && derived <= 840) g_offset_min = derived;
    return;
  }
  char tz[24];
  timezone_string(tz, sizeof(tz));
  setenv("TZ", tz, 1);
  tzset();
}

/*
 * Is this string something newlib can actually use?
 *
 * There is no validator in the C library -- tzset() accepts anything and falls
 * back to UTC on nonsense without a word -- so this checks it the one way that
 * is available: install it, ask what it works out to, and see whether the
 * answer is a zone at all. A rule that lands on exactly UTC, under the name
 * "UTC", when the owner did not ask for UTC is how a typo presents, and
 * rejecting it beats a clock that is quietly eight hours wrong.
 */
static bool zone_is_usable(const char *tz) {
  if (!tz || !tz[0]) return false;
  if (strlen(tz) >= sizeof(g_zone)) return false;
  char saved[sizeof(g_zone)];
  const char *current = getenv("TZ");
  snprintf(saved, sizeof(saved), "%s", current ? current : "");

  setenv("TZ", tz, 1);
  tzset();
  const int32_t offset = zone_offset_at(time(nullptr));
  const bool named = tzname[0] && tzname[0][0] && strcmp(tzname[0], "UTC") != 0;
  const bool ok = offset >= -840 && offset <= 840 &&
                  (named || strncmp(tz, "UTC", 3) == 0);

  if (saved[0]) setenv("TZ", saved, 1);
  else unsetenv("TZ");
  tzset();
  return ok;
}

// ---------------------------------------------------------------- DS3231 -----
#if USE_DS3231
static const uint8_t DS3231_ADDR = 0x68;

/// Control/status register, and the oscillator-stop flag in its top bit. See
/// ds3231_oscillator_stopped() for why this matters more than it looks.
static const uint8_t DS3231_REG_STATUS = 0x0F;
static const uint8_t DS3231_OSF = 0x80;

/// Whether a DS3231 was found and, if so, whether it had anything credible to
/// say. Reported by soft_clock_rtc_state() so a clock that keeps coming back
/// wrong can be diagnosed by asking rather than by guessing.
static RtcState g_rtc_state = RTC_ABSENT;

/*
 * I2C transactions to the RTC that did not complete, since boot.
 *
 * The only I2C traffic this firmware issues and then checks the result of --
 * U8g2 reports nothing about its own transfers -- so this is the bus health
 * indicator the `diag` command has. It should stay at zero forever. A number
 * that climbs means the bus itself is marginal: leads too long, both modules'
 * pull-ups in parallel too weak for the capacitance, or the OLED's 400 kHz
 * transfers colliding with something. The panel would be suffering equally and
 * silently.
 */
static uint32_t g_rtc_i2c_errors;

/// The DS3231 is rated for 400 kHz, and U8g2 re-asserts its own (possibly
/// faster) bus clock before each of its own transfers -- so it is enough to set
/// a safe clock here, around our own transfers, and let U8g2 put it back.
static void ds3231_bus() { Wire.setClock(400000); }

static uint8_t bcd_to_bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t bin_to_bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static bool ds3231_present() {
  ds3231_bus();
  Wire.beginTransmission(DS3231_ADDR);
  return Wire.endTransmission() == 0;
}

/// Reads one register. `out` is left alone on failure.
static bool ds3231_reg(uint8_t reg, uint8_t *out) {
  ds3231_bus();
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) {
    g_rtc_i2c_errors++;
    return false;
  }
  if (Wire.requestFrom((int)DS3231_ADDR, 1) != 1) {
    g_rtc_i2c_errors++;
    return false;
  }
  *out = (uint8_t)Wire.read();
  return true;
}

/*
 * The oscillator-stop flag, which is the only honest answer to "has this chip
 * been counting the whole time?".
 *
 * The DS3231 sets OSF whenever its oscillator has been interrupted since the
 * flag was last cleared -- a dead backup cell, a cell removed and refitted, or
 * a first power-up out of the packet. The registers still read back *something*
 * plausible afterwards, and a clock that has been stopped for a month is
 * exactly as wrong as one that reads 2000-01-01, but only the second one looks
 * wrong. Without this test the stale time was adopted as CLOCK_SRC_RTC, which
 * outranks NVS, so a speaker with a flat coin cell came back with a confident
 * and completely incorrect clock and no way to tell.
 *
 * Returns false when the register cannot be read at all -- an unreadable status
 * register is a bus problem, and ds3231_read() will fail for the same reason.
 */
static bool ds3231_oscillator_stopped(bool *known) {
  uint8_t reg = 0;
  if (!ds3231_reg(DS3231_REG_STATUS, &reg)) {
    if (known) *known = false;
    return false;
  }
  if (known) *known = true;
  return (reg & DS3231_OSF) != 0;
}

/// Clears OSF, after the chip has been given a time worth keeping. Read-modify-
/// write rather than a blind zero: the other bits of 0x0F are the 32 kHz output
/// enable and the two alarm flags, and clearing those uninvited would switch off
/// an output somebody else's board may depend on.
static void ds3231_clear_osf() {
  uint8_t reg = 0;
  if (!ds3231_reg(DS3231_REG_STATUS, &reg)) return;
  if (!(reg & DS3231_OSF)) return;
  ds3231_bus();
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(DS3231_REG_STATUS);
  Wire.write((uint8_t)(reg & (uint8_t)~DS3231_OSF));
  Wire.endTransmission();
}

static bool ds3231_read(struct tm *out) {
  ds3231_bus();
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission() != 0) {
    g_rtc_i2c_errors++;
    return false;
  }
  if (Wire.requestFrom((int)DS3231_ADDR, 7) != 7) {
    g_rtc_i2c_errors++;
    return false;
  }

  const uint8_t sec = Wire.read();
  const uint8_t min = Wire.read();
  const uint8_t hour = Wire.read();
  Wire.read();  // day of week: recomputed by mktime, so ignored
  const uint8_t mday = Wire.read();
  const uint8_t month = Wire.read();
  const uint8_t year = Wire.read();

  // A DS3231 that has lost its cell comes back as 00:00:00 2000-01-01. Treat a
  // year of 00 as "never set" rather than warping the display back to 2000.
  if (bcd_to_bin(year) == 0 && (month & 0x1F) == 1 && bcd_to_bin(mday) <= 1) {
    return false;
  }

  struct tm t;
  memset(&t, 0, sizeof(t));
  t.tm_sec = bcd_to_bin(sec & 0x7F);
  t.tm_min = bcd_to_bin(min & 0x7F);
  // Bit 6 selects 12-hour mode. Every library writes 24-hour mode, but a chip
  // configured by something else could still be in 12-hour mode.
  if (hour & 0x40) {
    uint8_t h = bcd_to_bin(hour & 0x1F) % 12;
    if (hour & 0x20) h += 12;  // PM
    t.tm_hour = h;
  } else {
    t.tm_hour = bcd_to_bin(hour & 0x3F);
  }
  t.tm_mday = bcd_to_bin(mday & 0x3F);
  t.tm_mon = bcd_to_bin(month & 0x1F) - 1;
  t.tm_year = bcd_to_bin(year) + 100;  // register year is 20xx
  t.tm_isdst = 0;

  /*
   * Range-check before believing any of it.
   *
   * bcd_to_bin() is a shift and an add: it turns the nibble pair 0x9C into 96,
   * and a register that came back garbled on a noisy bus -- a long lead, a
   * marginal pull-up, an OLED transfer that collided -- produces month 14 or
   * second 92 with no complaint. mktime() then normalises that into a date
   * years away, which is adopted as the trusted time and pushed straight back
   * into the chip and into NVS. One bad read would poison the clock for good.
   */
  if (t.tm_sec > 59 || t.tm_min > 59 || t.tm_hour > 23 || t.tm_mday < 1 ||
      t.tm_mday > 31 || t.tm_mon < 0 || t.tm_mon > 11) {
    return false;
  }

  *out = t;
  return true;
}

static void ds3231_write(const struct tm &t) {
  ds3231_bus();
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write((uint8_t)0x00);
  Wire.write(bin_to_bcd((uint8_t)t.tm_sec));
  Wire.write(bin_to_bcd((uint8_t)t.tm_min));
  Wire.write(bin_to_bcd((uint8_t)t.tm_hour));  // bit 6 clear = 24 hour
  Wire.write(bin_to_bcd((uint8_t)(t.tm_wday + 1)));
  Wire.write(bin_to_bcd((uint8_t)t.tm_mday));
  Wire.write(bin_to_bcd((uint8_t)(t.tm_mon + 1)));
  Wire.write(bin_to_bcd((uint8_t)(t.tm_year % 100)));
  if (Wire.endTransmission() != 0) return;
  // The chip is now counting from a time we believe, so the "I lost track"
  // flag no longer describes anything true. Clearing it is what makes the next
  // boot's test mean something: left set, every boot from here would distrust a
  // perfectly good clock.
  ds3231_clear_osf();
}
#endif  // USE_DS3231

RtcState soft_clock_rtc_state() {
#if USE_DS3231
  return g_rtc_state;
#else
  return RTC_ABSENT;
#endif
}

uint32_t soft_clock_i2c_errors() {
#if USE_DS3231
  return g_rtc_i2c_errors;
#else
  return 0;
#endif
}

const char *soft_clock_rtc_state_name() {
  switch (soft_clock_rtc_state()) {
    case RTC_STOPPED: return "oscillator stopped (check the coin cell)";
    case RTC_UNSET: return "present, never set";
    case RTC_OK: return "present, running";
    case RTC_UNREADABLE: return "present but unreadable";
    default: return "not fitted";
  }
}

// ------------------------------------------------------------------- NTP -----
/*
 * Network time, in the background, for as long as there is a network.
 *
 * The IDF's SNTP client owns the system clock once it is started: it writes UTC
 * straight in with settimeofday() from lwIP's thread, on the first answer and
 * on every re-sync after that. Nothing here has to poll or parse -- the only
 * job left is to notice that it happened, so the info screen can stop saying
 * "build" and the DS3231 and NVS can be brought up to date.
 *
 * The notification callback runs on lwIP's thread, which is not a place to
 * touch I2C or NVS from. It raises a flag; soft_clock_tick() does the work.
 */
static void persist_now();

static void ntp_notification(struct timeval *tv) {
  (void)tv;
  g_ntp_fresh = true;
}

void soft_clock_network_begin() {
  if (!g_auto_sync) return;  // the owner keeps their own time
  // Re-arming a running client is what a reconnect wants: configTzTime() stops
  // it first, so the next poll goes out immediately instead of waiting out the
  // remainder of an hour-long interval that elapsed while the link was down.
  sntp_set_time_sync_notification_cb(ntp_notification);
  // Its own buffer, deliberately: configTzTime() setenv()s what it is handed,
  // and handing it the pointer getenv("TZ") just returned would have it write
  // over its own source string.
  char tz[24];
  timezone_string(tz, sizeof(tz));
  configTzTime(tz, "pool.ntp.org", "time.nist.gov", "time.google.com");
  g_ntp_running = true;
  LOGLN("[clock] sntp started");
}

void soft_clock_network_end() {
  if (!g_ntp_running) return;
  esp_sntp_stop();
  g_ntp_running = false;
  LOGLN("[clock] sntp stopped");
}

bool soft_clock_network_synced() { return g_ntp_synced; }

/// Picks up what the SNTP client already wrote into the system clock.
static void adopt_network_time() {
  g_ntp_fresh = false;
  const time_t now = time(nullptr);
  if (now < EPOCH_SANITY_FLOOR) return;

  const bool first = !g_ntp_synced;
  g_ntp_synced = true;
  g_source = CLOCK_SRC_NTP;

#if USE_DS3231
  struct tm local;
  localtime_r(&now, &local);
  // A network answer is the best time this speaker will ever have, so it is
  // also what re-arms a chip whose oscillator had stopped: ds3231_write()
  // clears OSF, and from here the RTC is trustworthy again.
  if (ds3231_present()) {
    ds3231_write(local);
    g_rtc_state = RTC_OK;
  }
#endif
  persist_now();

  if (first) {
    struct tm local;
    localtime_r(&now, &local);
    LOGF("[clock] sntp %04d-%02d-%02d %02d:%02d:%02d (utc%+ld min)\n",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec,
                  (long)g_offset_min);
  }
}

// ----------------------------------------------------------------- public ----
void soft_clock_begin() {
  // The offset has to be in place before anything else: build_epoch() and the
  // DS3231 both hand out *local* broken-down time, and mktime() cannot turn
  // that into the right UTC instant without knowing the zone.
  g_prefs_ok = g_prefs.begin("clock", false);
  if (g_prefs_ok) {
    g_offset_min = (int32_t)g_prefs.getLong(OFFSET_KEY, CLOCK_TZ_OFFSET_MIN);
    if (g_offset_min < -840 || g_offset_min > 840) g_offset_min = CLOCK_TZ_OFFSET_MIN;
    g_use_24h = g_prefs.getBool(H24_KEY, CLOCK_24H != 0);
    g_auto_sync = g_prefs.getBool(SYNC_KEY, true);
    // Read into a scratch buffer and vet it before it becomes the live zone: a
    // rule that stopped parsing between firmware versions must not take the
    // clock with it, because the clock is what the alarm runs on.
    char stored[sizeof(g_zone)];
    const String rule = g_prefs.getString(ZONE_KEY, "");
    if (rule.length() && rule.length() < sizeof(stored)) {
      snprintf(stored, sizeof(stored), "%s", rule.c_str());
      if (zone_is_usable(stored)) snprintf(g_zone, sizeof(g_zone), "%s", stored);
      else LOGF("[clock] stored zone \"%s\" no longer parses; keeping "
                         "the fixed offset\n", stored);
    }
  }
  apply_timezone();

  // Always start from the build stamp, so nothing downstream ever sees 1970.
  apply_epoch(build_epoch());
  g_source = CLOCK_SRC_BUILD;

  if (g_prefs_ok) {
    const uint32_t saved = g_prefs.getULong(EPOCH_KEY, 0);
    // Only trust the saved value if it is newer than this build: an older one is
    // left over from a previous firmware and is worse than the build stamp.
    if (saved > (uint32_t)build_epoch()) {
      apply_epoch((time_t)saved);
      g_source = CLOCK_SRC_NVS;
    }
  }

#if USE_DS3231
  // Normally the display has already brought I2C up; this covers the case where
  // there is no display (or UI_ENABLED is 0) but there is an RTC. Calling
  // Wire.begin twice with the same pins is harmless.
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL, 400000);

  if (ds3231_present()) {
    /*
     * Three questions, in this order, because a "yes" to the first makes the
     * other two meaningless: has the oscillator been interrupted, do the
     * registers read back sanely, and is the date one that was ever set.
     */
    bool osfKnown = false;
    const bool stopped = ds3231_oscillator_stopped(&osfKnown);
    struct tm t;
    const bool readable = ds3231_read(&t);

    if (osfKnown && stopped) {
      /*
       * The chip has not been counting continuously, so whatever it reads is
       * fiction however plausible it looks. Do not adopt it and do not seed it
       * either: the best time available right now is the NVS one already
       * applied above, and writing that back would clear OSF and make the next
       * boot trust a clock that is still only as good as the last save. The
       * first real time from the network or the dashboard clears the flag; see
       * ds3231_write().
       */
      g_rtc_state = RTC_STOPPED;
      LOGLN("[clock] ds3231 reports its oscillator stopped -- the coin "
                     "cell is flat or was refitted. Ignoring its time; using "
                     "NVS/network instead. It is re-seeded and trusted again "
                     "from the next real time set.");
    } else if (readable) {
      g_rtc_state = RTC_OK;
      apply_epoch(mktime(&t));
      g_source = CLOCK_SRC_RTC;
      LOGLN("[clock] ds3231");
    } else if (!osfKnown) {
      g_rtc_state = RTC_UNREADABLE;
      LOGLN("[clock] ds3231 answered its address but not its registers "
                     "-- check the pull-ups and the lead length");
    } else {
      g_rtc_state = RTC_UNSET;
      LOGLN("[clock] ds3231 present but unset -- seeding it");
      struct tm seed;
      time_t now = time(nullptr);
      localtime_r(&now, &seed);
      ds3231_write(seed);
    }
  } else {
    g_rtc_state = RTC_ABSENT;
    LOGLN("[clock] no ds3231 on the bus");
  }
#endif

  struct tm now;
  soft_clock_now(&now);
  LOGF("[clock] %04d-%02d-%02d %02d:%02d:%02d (%s)\n",
                now.tm_year + 1900, now.tm_mon + 1, now.tm_mday, now.tm_hour,
                now.tm_min, now.tm_sec, soft_clock_source_name());
  g_last_persist_ms = millis();
}

void soft_clock_now(struct tm *out) {
  time_t now = time(nullptr);
  localtime_r(&now, out);
}

bool soft_clock_trusted() { return g_source != CLOCK_SRC_BUILD; }

const char *soft_clock_source_name() {
  switch (g_source) {
    case CLOCK_SRC_NVS: return "nvs";
    case CLOCK_SRC_SERIAL: return "set";
    case CLOCK_SRC_RTC: return "ds3231";
    case CLOCK_SRC_NTP: return "ntp";
    default: return "build";
  }
}

static void persist_now() {
  if (!g_prefs_ok) return;
  g_prefs.putULong(EPOCH_KEY, (uint32_t)time(nullptr));
  g_last_persist_ms = millis();
}

bool soft_clock_use_24h() { return g_use_24h; }

void soft_clock_set_use_24h(bool on) {
  if (on == g_use_24h) return;
  g_use_24h = on;
  if (g_prefs_ok) g_prefs.putBool(H24_KEY, on);
  LOGF("[clock] %s clock\n", on ? "24-hour" : "12-hour");
}

bool soft_clock_auto_sync() { return g_auto_sync; }

void soft_clock_set_auto_sync(bool on) {
  if (on != g_auto_sync) {
    g_auto_sync = on;
    if (g_prefs_ok) g_prefs.putBool(SYNC_KEY, on);
    LOGF("[clock] network sync %s\n", on ? "on" : "off");
  }
  // Unconditional, so switching it off stops a client that is already polling
  // rather than waiting for the next disconnect to do it.
  if (!on) soft_clock_network_end();
}

int32_t soft_clock_utc_offset_min() { return g_offset_min; }

void soft_clock_set_utc_offset_min(int32_t minutes) {
  if (minutes < -840 || minutes > 840) return;
  // A zone rule outranks a bare offset, and the browser sends its offset along
  // with every clock sync -- so without this, one press of "Sync browser time"
  // would quietly demote a configured zone back to a fixed offset and the next
  // daylight-saving change would be missed. Choosing the zone is the Settings
  // page's job, and it goes through soft_clock_set_zone().
  if (g_zone[0]) return;
  if (minutes == g_offset_min) return;
  g_offset_min = minutes;
  apply_timezone();
  if (g_prefs_ok) g_prefs.putLong(OFFSET_KEY, (int32_t)minutes);
  LOGF("[clock] utc offset %+ld min\n", (long)minutes);

  // The SNTP client caches nothing zone-related, but re-arming it costs a
  // packet and puts the new offset on the display without waiting for the next
  // poll -- which is up to an hour away.
  if (g_ntp_running) soft_clock_network_begin();
}

const char *soft_clock_zone() { return g_zone; }

bool soft_clock_set_zone(const char *tz) {
  if (!tz || !tz[0]) {
    if (!g_zone[0]) return true;
    g_zone[0] = 0;
    if (g_prefs_ok) g_prefs.remove(ZONE_KEY);
    apply_timezone();
    LOGLN("[clock] zone cleared; back to the fixed offset");
    return true;
  }
  if (!zone_is_usable(tz)) {
    LOGF("[clock] \"%s\" is not a POSIX TZ rule this C library "
                  "understands\n", tz);
    return false;
  }
  if (strcmp(g_zone, tz) == 0) return true;
  snprintf(g_zone, sizeof(g_zone), "%s", tz);
  apply_timezone();
  if (g_prefs_ok) g_prefs.putString(ZONE_KEY, g_zone);
  LOGF("[clock] zone %s -> %s, %+ld min\n", g_zone,
                soft_clock_zone_abbrev(), (long)g_offset_min);
  if (g_ntp_running) soft_clock_network_begin();
  return true;
}

const char *soft_clock_zone_abbrev() {
  if (!g_zone[0]) return "UTC";
  struct tm local = {};
  const time_t now = time(nullptr);
  localtime_r(&now, &local);  // fills tzname[] for the current instant
  const int which = local.tm_isdst > 0 ? 1 : 0;
  return (tzname[which] && tzname[which][0]) ? tzname[which] : "UTC";
}

bool soft_clock_dst_active() {
  if (!g_zone[0]) return false;
  struct tm local = {};
  const time_t now = time(nullptr);
  localtime_r(&now, &local);
  return local.tm_isdst > 0;
}

void soft_clock_set(const struct tm &t, ClockSource source) {
  struct tm copy = t;
  /*
   * On a fixed offset there is no daylight saving to resolve, and saying so
   * explicitly keeps mktime() from guessing. With a zone rule installed the
   * opposite is true: the owner typed a wall-clock time, and only the rule
   * knows which side of a transition it falls on -- so hand it the question,
   * which is what tm_isdst = -1 means.
   */
  copy.tm_isdst = g_zone[0] ? -1 : 0;
  const time_t epoch = mktime(&copy);
  if (epoch == (time_t)-1) return;

  apply_epoch(epoch);
  g_source = source;

#if USE_DS3231
  // Keep the hardware clock in step, so the next power cut is free. This is
  // also the manual route back from RTC_STOPPED: a time somebody typed in is a
  // time worth keeping, and ds3231_write() clears the stop flag with it.
  struct tm norm;
  localtime_r(&epoch, &norm);
  if (ds3231_present()) {
    ds3231_write(norm);
    g_rtc_state = RTC_OK;
  }
#endif
  persist_now();
}

void soft_clock_tick() {
  if (g_ntp_fresh) adopt_network_time();
  const uint32_t now = millis();

  /*
   * Follow the zone across a daylight-saving transition.
   *
   * newlib applies the rule on its own -- localtime() is already right the
   * instant the clocks go forward -- but g_offset_min is a cached readout of
   * it, and half the firmware asks for the offset rather than for the time.
   * A minute is far finer than something that happens twice a year needs, and
   * it costs two mktime() calls, so there is no reason to be cleverer about
   * when to look.
   */
  if (g_zone[0] && now - g_last_zone_check_ms >= ZONE_RECHECK_MS) {
    g_last_zone_check_ms = now;
    const int32_t derived = zone_offset_at(time(nullptr));
    if (derived >= -840 && derived <= 840 && derived != g_offset_min) {
      LOGF("[clock] %s: %+ld min -> %+ld min (%s)\n", g_zone,
                    (long)g_offset_min, (long)derived, soft_clock_zone_abbrev());
      g_offset_min = derived;
    }
  }

  if (now - g_last_persist_ms >= PERSIST_EVERY_MS) persist_now();
}

bool soft_clock_command(const char *line) {
  struct tm now;
  soft_clock_now(&now);

  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;

  // time 2026-08-18 14:30:00  /  time 2026-08-18 14:30
  if (sscanf(line, "time %d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6 ||
      (sscanf(line, "time %d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) == 5 &&
       (s = 0, true))) {
    now.tm_year = y - 1900;
    now.tm_mon = mo - 1;
    now.tm_mday = d;
    now.tm_hour = h;
    now.tm_min = mi;
    now.tm_sec = s;
    soft_clock_set(now, CLOCK_SRC_SERIAL);
    LOGF("[clock] set %04d-%02d-%02d %02d:%02d:%02d\n", y, mo, d, h,
                  mi, s);
    return true;
  }

  // time 14:30:00  /  time 14:30   -- today, just the clock
  if (sscanf(line, "time %d:%d:%d", &h, &mi, &s) == 3 ||
      (sscanf(line, "time %d:%d", &h, &mi) == 2 && (s = 0, true))) {
    now.tm_hour = h;
    now.tm_min = mi;
    now.tm_sec = s;
    soft_clock_set(now, CLOCK_SRC_SERIAL);
    LOGF("[clock] set %02d:%02d:%02d\n", h, mi, s);
    return true;
  }

  // date 2026-08-18
  if (sscanf(line, "date %d-%d-%d", &y, &mo, &d) == 3) {
    now.tm_year = y - 1900;
    now.tm_mon = mo - 1;
    now.tm_mday = d;
    soft_clock_set(now, CLOCK_SRC_SERIAL);
    LOGF("[clock] set %04d-%02d-%02d\n", y, mo, d);
    return true;
  }

  if (strcmp(line, "time") == 0) {
    LOGF("[clock] %04d-%02d-%02d %02d:%02d:%02d (%s)\n",
                  now.tm_year + 1900, now.tm_mon + 1, now.tm_mday, now.tm_hour,
                  now.tm_min, now.tm_sec, soft_clock_source_name());
    return true;
  }

  return false;
}
