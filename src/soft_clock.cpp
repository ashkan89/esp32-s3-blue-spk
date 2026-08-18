#include "soft_clock.h"

#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "ui_config.h"

#ifndef USE_DS3231
#define USE_DS3231 0
#endif
#ifndef USE_NTP
#define USE_NTP 0
#endif

static ClockSource g_source = CLOCK_SRC_BUILD;
static Preferences g_prefs;
static uint32_t g_last_persist_ms;
static bool g_prefs_ok;

/// NVS is written at most this often. Ten minutes is 144 writes a day, which the
/// flash wear levelling shrugs off, and bounds how far behind a power cut can
/// leave the clock.
static const uint32_t PERSIST_EVERY_MS = 10UL * 60UL * 1000UL;

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

// ---------------------------------------------------------------- DS3231 -----
#if USE_DS3231
static const uint8_t DS3231_ADDR = 0x68;

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

static bool ds3231_read(struct tm *out) {
  ds3231_bus();
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom((int)DS3231_ADDR, 7) != 7) return false;

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

  memset(out, 0, sizeof(*out));
  out->tm_sec = bcd_to_bin(sec & 0x7F);
  out->tm_min = bcd_to_bin(min & 0x7F);
  // Bit 6 selects 12-hour mode. Every library writes 24-hour mode, but a chip
  // configured by something else could still be in 12-hour mode.
  if (hour & 0x40) {
    uint8_t h = bcd_to_bin(hour & 0x1F) % 12;
    if (hour & 0x20) h += 12;  // PM
    out->tm_hour = h;
  } else {
    out->tm_hour = bcd_to_bin(hour & 0x3F);
  }
  out->tm_mday = bcd_to_bin(mday & 0x3F);
  out->tm_mon = bcd_to_bin(month & 0x1F) - 1;
  out->tm_year = bcd_to_bin(year) + 100;  // register year is 20xx
  out->tm_isdst = 0;
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
  Wire.endTransmission();
}
#endif  // USE_DS3231

// ------------------------------------------------------------------- NTP -----
#if USE_NTP
#include <WiFi.h>

#ifndef WIFI_SSID
#error "USE_NTP=1 also needs WIFI_SSID and WIFI_PASS defined in build_flags"
#endif

/*
 * One shot, then the radio is handed to Bluetooth for good.
 *
 * This must run before a2dp_sink.start(). Wi-Fi and Bluetooth Classic share the
 * one antenna and one PHY; the coexistence scheduler makes them work together,
 * but it does it by giving each of them gaps -- and a gap in an A2DP stream is
 * an audible dropout. Syncing before the sink exists sidesteps the whole issue.
 */
static bool ntp_sync(struct tm *out) {
  Serial.printf("[clock] wifi: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; i++) delay(250);
  bool ok = false;

  if (WiFi.status() == WL_CONNECTED) {
    // configTime takes the offset in seconds; DST is left at 0 because this
    // firmware has no way to be told when the rules change.
    configTime(CLOCK_TZ_OFFSET_MIN * 60, 0, "pool.ntp.org", "time.nist.gov");
    for (int i = 0; i < 40; i++) {
      time_t now = time(nullptr);
      if (now > 1700000000) {  // anything after late 2023 means SNTP answered
        localtime_r(&now, out);
        ok = true;
        break;
      }
      delay(250);
    }
  }
  Serial.println(ok ? "[clock] ntp ok" : "[clock] ntp failed");

  // Tear the radio down completely, not just disconnect.
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(100);
  return ok;
}
#endif  // USE_NTP

// ----------------------------------------------------------------- public ----
void soft_clock_begin() {
  // Always start from the build stamp, so nothing downstream ever sees 1970.
  apply_epoch(build_epoch());
  g_source = CLOCK_SRC_BUILD;

  g_prefs_ok = g_prefs.begin("clock", false);
  if (g_prefs_ok) {
    const uint32_t saved = g_prefs.getULong("epoch", 0);
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
    struct tm t;
    if (ds3231_read(&t)) {
      apply_epoch(mktime(&t));
      g_source = CLOCK_SRC_RTC;
      Serial.println("[clock] ds3231");
    } else {
      Serial.println("[clock] ds3231 present but unset -- seeding it");
      struct tm seed;
      time_t now = time(nullptr);
      localtime_r(&now, &seed);
      ds3231_write(seed);
    }
  } else {
    Serial.println("[clock] no ds3231 on the bus");
  }
#endif

#if USE_NTP
  {
    struct tm t;
    if (ntp_sync(&t)) {
      soft_clock_set(t, CLOCK_SRC_NTP);
    }
  }
#endif

  struct tm now;
  soft_clock_now(&now);
  Serial.printf("[clock] %04d-%02d-%02d %02d:%02d:%02d (%s)\n",
                now.tm_year + 1900, now.tm_mon + 1, now.tm_mday, now.tm_hour,
                now.tm_min, now.tm_sec, soft_clock_source_name());
  g_last_persist_ms = millis();
}

void soft_clock_now(struct tm *out) {
  time_t now = time(nullptr);
  localtime_r(&now, out);
}

bool soft_clock_trusted() { return g_source != CLOCK_SRC_BUILD; }

ClockSource soft_clock_source() { return g_source; }

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
  g_prefs.putULong("epoch", (uint32_t)time(nullptr));
  g_last_persist_ms = millis();
}

void soft_clock_set(const struct tm &t, ClockSource source) {
  struct tm copy = t;
  copy.tm_isdst = 0;
  const time_t epoch = mktime(&copy);
  if (epoch == (time_t)-1) return;

  apply_epoch(epoch);
  g_source = source;

#if USE_DS3231
  // Keep the hardware clock in step, so the next power cut is free.
  struct tm norm;
  localtime_r(&epoch, &norm);
  if (ds3231_present()) ds3231_write(norm);
#endif
  persist_now();
}

void soft_clock_tick() {
  const uint32_t now = millis();
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
    Serial.printf("[clock] set %04d-%02d-%02d %02d:%02d:%02d\n", y, mo, d, h,
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
    Serial.printf("[clock] set %02d:%02d:%02d\n", h, mi, s);
    return true;
  }

  // date 2026-08-18
  if (sscanf(line, "date %d-%d-%d", &y, &mo, &d) == 3) {
    now.tm_year = y - 1900;
    now.tm_mon = mo - 1;
    now.tm_mday = d;
    soft_clock_set(now, CLOCK_SRC_SERIAL);
    Serial.printf("[clock] set %04d-%02d-%02d\n", y, mo, d);
    return true;
  }

  if (strcmp(line, "time") == 0) {
    Serial.printf("[clock] %04d-%02d-%02d %02d:%02d:%02d (%s)\n",
                  now.tm_year + 1900, now.tm_mon + 1, now.tm_mday, now.tm_hour,
                  now.tm_min, now.tm_sec, soft_clock_source_name());
    return true;
  }

  return false;
}
