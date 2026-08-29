#include "app_config.h"
#include "management.h"

#if MANAGEMENT_ENABLED

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <Update.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_bt.h>
#include <esp_gap_bt_api.h>
#include <esp_ota_ops.h>
#include <nvs_flash.h>
#include <esp_system.h>
#include <esp_wifi.h>

#include "BluetoothA2DPSink.h"
#include "audio_probe.h"
#include "battery.h"
#include "ble_control.h"
#include "df_player.h"
#include "leds.h"
#include "net_audio.h"
#include "player_state.h"
#include "soft_clock.h"
#include "status_led.h"
#include "ui.h"
#include "web_assets_gzip.h"

namespace {

struct Settings {
  String ssid;
  String wifiPassword;
  String hostname;
  String apPassword;
  String adminPassword;
  String deviceName;
  String githubRepo;
  String githubAsset;
  String githubToken;
  bool apAlways;

  // DFPlayer boot defaults. The module forgets everything at power-off, so the
  // firmware has to hand it a source, a volume and an EQ every time; these are
  // the values the dashboard's "Save as startup defaults" button stores.
  uint8_t dfSource;
  uint8_t dfVolume;  // module scale, 0..DF_VOLUME_MAX
  uint8_t dfEq;
  uint8_t dfLoop;
  uint8_t dfLoopFolder;
  bool dfAutoplay;   // start playing as soon as the card is found

  // The battery pack. These describe hardware, not preference: the divider and
  // the trim are what turn an ADC reading into a voltage, and getting them from
  // NVS rather than from a build flag is what lets one firmware serve a 1S and
  // a 2S build.
  bool batteryEnabled;
  float batteryDivider;
  float batteryCalibration;
  float batteryFull;
  float batteryEmpty;
  uint8_t batteryCells;
  uint8_t batteryLow;
  uint8_t batteryCritical;

  // Panel blanking. A UiBlankMode and how long it waits; see ui.h for what the
  // two timed modes each count as a reason to stay on.
  uint8_t oledBlankMode;
  uint16_t oledBlankAfterS;

  LedConfig leds;
};

struct UpdateState {
  char phase[16];
  char message[128];
  char tag[40];
  char releaseName[80];
  char assetName[96];
  char releaseUrl[192];
  uint32_t done;
  uint32_t total;
  bool available;
  bool busy;
};

Preferences prefs;
Settings settings;
WebServer server(80);
BluetoothA2DPSink *sink;
String stableDeviceName;
String apName;
bool apRunning;
bool announcedIp;
uint32_t wifiStartedAt;
bool rebootPending;
uint32_t updatePhaseAt;  // when updateState.phase last changed
// When the station last came up, and whether the boot-time update check has
// already run. See serviceStartupUpdateCheck().
uint32_t stationUpAt;
bool startupCheckDone;
bool startupCheckHadClock;
// Deliberate station retries while the setup AP is up. See startAccessPoint()
// for why the core's own auto-reconnect cannot be left switched on.
uint32_t staRetryAt;
uint32_t staRetryStartedAt;
constexpr uint32_t STA_RETRY_PERIOD_MS = 120000;
constexpr uint32_t STA_RETRY_WINDOW_MS = 15000;
// How long the saved network gets before the recovery access point is raised.
// A cold boot brings up Wi-Fi, Bluetooth and DHCP at once; 15 s was short
// enough that a merely slow router looked like a dead one, and raising the AP
// parks the station and quiets Bluetooth, so a wrong call here is expensive.
constexpr uint32_t FIRST_CONNECT_GRACE_MS = 30000;
constexpr uint16_t AP_BEACON_INTERVAL_MS = 300;

bool btActive;
uint8_t apClients;
// Which radio owns the antenna this boot. Persisted, so a power cut brings the
// speaker back doing whatever it was doing.
RadioMode radioMode = RADIO_MODE_MANAGEMENT;
// Combo mode has no setup access point to fall back on, so a station that never
// arrives is a dead end the user has to be told about. Said once, not per loop.
bool comboOfflineWarned;

/*
 * The boot sentinel.
 *
 * A mode that crashes during start-up takes the dashboard and the serial
 * console down with it, and the BOOT button needs the UI task to be running to
 * offer anything -- so a speaker that reboot-loops in one mode has no way back
 * to a mode that works. That is not an acceptable failure for a setting the
 * user can change from a web page.
 *
 * So each boot into a mode other than Wi-Fi writes a strike to NVS before it
 * tries anything, and clears it once the speaker has been up long enough to
 * call it stable. Two boots that never reach that point and the next one falls
 * back to Wi-Fi mode, where the dashboard is reachable and the mode can be
 * changed again. The count survives a reset by construction: that is the whole
 * point of keeping it in flash rather than in RAM.
 */
constexpr uint8_t BOOT_STRIKES_MAX = 2;
constexpr uint32_t BOOT_STABLE_MS = 20000;
uint8_t bootStrikes;
bool bootStrikePending;
uint8_t mutedFrom = 80;
bool browserUploadAccepted;
bool browserUploadOk;
size_t browserUploadTotal;
char browserUploadError[96];
portMUX_TYPE updateMux = portMUX_INITIALIZER_UNLOCKED;
UpdateState updateState = {"idle", "Ready", "", "", "", "", 0, 0, false, false};

/*
 * Trust anchors for the updater.
 *
 * This used to pin a single root, DigiCert Global Root G2. GitHub has since
 * moved: api.github.com now chains to Sectigo Public Server Authentication
 * Root E46, and release assets are served from a host with a Let's Encrypt
 * chain. Neither validates against a DigiCert root, so every request failed the
 * TLS handshake and HTTPClient reported it as HTTP -1 (connection refused) --
 * a number that tells you nothing about what actually went wrong.
 *
 * Pinning one root was the mistake, not the choice of root. The IDF ships the
 * Mozilla root store as a compact bundle linked into libmbedtls; using it costs
 * about 68 KB of flash, which this 16 MB board has in abundance, and survives
 * the next CA change without a firmware update. Verification stays on: this
 * connection writes executable code to flash.
 */
extern const uint8_t rootCaBundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t rootCaBundleEnd[] asm("_binary_x509_crt_bundle_end");

void trustPublicRoots(NetworkClientSecure &tls) {
  tls.setCACertBundle(rootCaBundleStart,
                      (size_t)(rootCaBundleEnd - rootCaBundleStart));
}

// HTTPClient reports transport failures as small negative numbers that mean
// nothing on a dashboard. Turn them back into words.
String httpErrorText(int code) {
  if (code > 0) return String("HTTP ") + code;
  return HTTPClient::errorToString(code) + " (" + code + ")";
}

void startResponder();  // defined with the other web plumbing, below

String cleanHostname(String value) {
  value.trim();
  value.toLowerCase();
  String out;
  for (size_t i = 0; i < value.length() && out.length() < 31; ++i) {
    const char c = value[i];
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        (c == '-' && out.length() && out[out.length() - 1] != '-')) out += c;
  }
  while (out.endsWith("-")) out.remove(out.length() - 1);
  return out.length() ? out : APP_NAME;
}

String cleanDeviceName(String value, const char *fallback) {
  value.trim();
  if (!value.length()) value = fallback;
  if (value.length() > 31) value.remove(31);
  return value;
}

String defaultApName() {
  const uint32_t id = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFF);
  char buf[32];
  snprintf(buf, sizeof(buf), "esp32-blue-spk-%06lX", (unsigned long)id);
  return String(buf);
}

// Deferred flash write for the lighting: set when something changes, cleared
// by saveLedSettings() once the dashboard has stopped sending.
bool ledsDirty;
uint32_t ledsDirtyAt;
constexpr uint32_t LEDS_PERSIST_QUIET_MS = 1200;
void saveLedSettings();

void loadSettings(const char *fallbackName) {
  prefs.begin("speaker-web", false);
  settings.ssid = prefs.getString("ssid", "");
  settings.wifiPassword = prefs.getString("wifiPass", "");
  settings.hostname = cleanHostname(prefs.getString("hostname", APP_NAME));
  if (settings.hostname == "esp32-speaker") settings.hostname = APP_NAME;
  settings.apPassword = prefs.getString("apPass", "speaker-setup");
  if (settings.apPassword.length() < 8) settings.apPassword = "speaker-setup";
  settings.adminPassword = prefs.getString("adminPass", "admin");
  if (!settings.adminPassword.length()) settings.adminPassword = "admin";
  settings.deviceName = cleanDeviceName(prefs.getString("deviceName", fallbackName), fallbackName);
  if (settings.deviceName == "ESP32 Speaker") settings.deviceName = APP_NAME;
  settings.githubRepo = prefs.getString("ghRepo", DEFAULT_GITHUB_REPO);
  settings.githubAsset = prefs.getString("ghAsset", DEFAULT_GITHUB_ASSET);
  settings.githubToken = prefs.getString("ghToken", "");
  settings.apAlways = prefs.getBool("apAlways", false);

  settings.dfSource = prefs.getUChar("dfSrc", (uint8_t)DF_SRC_SD);
  settings.dfVolume = prefs.getUChar("dfVol", DF_VOLUME_DEFAULT);
  settings.dfEq = prefs.getUChar("dfEq", 0);
  settings.dfLoop = prefs.getUChar("dfLoop", (uint8_t)DF_LOOP_OFF);
  settings.dfLoopFolder = prefs.getUChar("dfLoopF", 1);
  settings.dfAutoplay = prefs.getBool("dfAuto", false);
  if (settings.dfVolume > DF_VOLUME_MAX) settings.dfVolume = DF_VOLUME_MAX;
  if (settings.dfEq > 5) settings.dfEq = 0;
  if (settings.dfLoop > (uint8_t)DF_LOOP_RANDOM) settings.dfLoop = DF_LOOP_OFF;

  // Off until asked for: see the note on PIN_BATTERY_SENSE in hw_config.h. The
  // pin is ready, the settings card is there, and one switch starts it.
  settings.batteryEnabled = prefs.getBool("batOn", false);
  settings.batteryDivider = prefs.getFloat("batDiv", BATTERY_DIVIDER_DEFAULT);
  settings.batteryCalibration = prefs.getFloat("batCal", BATTERY_CALIBRATION_DEFAULT);
  settings.batteryCells = prefs.getUChar("batCells", 1);
  settings.batteryFull = prefs.getFloat("batFull", BATTERY_FULL_V_DEFAULT);
  settings.batteryEmpty = prefs.getFloat("batEmpty", BATTERY_EMPTY_V_DEFAULT);
  settings.batteryLow = prefs.getUChar("batLow", BATTERY_LOW_PCT_DEFAULT);
  settings.batteryCritical = prefs.getUChar("batCrit", BATTERY_CRITICAL_PCT_DEFAULT);

  settings.oledBlankMode = prefs.getUChar("uiBlank", UI_BLANK_MODE_DEFAULT);
  settings.oledBlankAfterS = prefs.getUShort("uiBlankS", UI_BLANK_AFTER_S_DEFAULT);
  if (settings.oledBlankMode > (uint8_t)UI_BLANK_ALWAYS) {
    settings.oledBlankMode = (uint8_t)UI_BLANK_NEVER;
  }
  settings.oledBlankAfterS = (uint16_t)constrain(
      (int)settings.oledBlankAfterS, (int)UI_BLANK_AFTER_S_MIN,
      (int)UI_BLANK_AFTER_S_MAX);
  // The panel may not exist yet -- ui_begin() runs later in some modes -- but
  // ui_set_blank() only writes two variables the render task reads, so this is
  // safe here and saves a second place that has to remember to apply it.
  ui_set_blank((UiBlankMode)settings.oledBlankMode, settings.oledBlankAfterS);

  /*
   * The ring. Loaded in every radio mode, not just the ones with a dashboard:
   * a Bluetooth-only speaker still has lights on it, and they should come back
   * the colour they were left. leds_configure() only writes a struct, so this
   * is safe before leds_begin() has claimed the pin.
   */
  settings.leds.enabled = prefs.getBool("ledOn", true);
  settings.leds.effect = prefs.getUChar("ledFx", LED_DEFAULT_EFFECT);
  settings.leds.brightness = prefs.getUChar("ledBri", LED_DEFAULT_BRIGHTNESS);
  settings.leds.speed = prefs.getUChar("ledSpd", LED_DEFAULT_SPEED);
  settings.leds.reactivity = prefs.getUChar("ledRct", LED_DEFAULT_REACTIVITY);
  settings.leds.color = prefs.getULong("ledCol", LED_DEFAULT_COLOR);
  settings.leds.color2 = prefs.getULong("ledCol2", LED_DEFAULT_COLOR2);
  settings.leds.idleOff = prefs.getBool("ledIdle", LED_IDLE_OFF_DEFAULT);
  settings.leds.idleAfterS = prefs.getUShort("ledIdleS", LED_IDLE_AFTER_S_DEFAULT);
  if (settings.leds.effect >= LED_FX_COUNT) settings.leds.effect = LED_DEFAULT_EFFECT;
  if (settings.leds.reactivity > 100) settings.leds.reactivity = 100;
  settings.leds.idleAfterS = (uint16_t)constrain(
      (int)settings.leds.idleAfterS, (int)LED_IDLE_AFTER_S_MIN,
      (int)LED_IDLE_AFTER_S_MAX);
  leds_configure(settings.leds);

  stableDeviceName = settings.deviceName;
}

/// Hands the stored pack description to the gauge. Called from
/// management_begin() before the gauge starts, and again on every settings save.
void applyBatterySettings() {
  battery_configure(settings.batteryEnabled, settings.batteryDivider,
                    settings.batteryCalibration, settings.batteryFull,
                    settings.batteryEmpty, settings.batteryCells,
                    settings.batteryLow, settings.batteryCritical);
}

void saveSettings() {
  prefs.putString("ssid", settings.ssid);
  prefs.putString("wifiPass", settings.wifiPassword);
  prefs.putString("hostname", settings.hostname);
  prefs.putString("apPass", settings.apPassword);
  prefs.putString("adminPass", settings.adminPassword);
  prefs.putString("deviceName", settings.deviceName);
  prefs.putString("ghRepo", settings.githubRepo);
  prefs.putString("ghAsset", settings.githubAsset);
  prefs.putString("ghToken", settings.githubToken);
  prefs.putBool("apAlways", settings.apAlways);

  prefs.putUChar("dfSrc", settings.dfSource);
  prefs.putUChar("dfVol", settings.dfVolume);
  prefs.putUChar("dfEq", settings.dfEq);
  prefs.putUChar("dfLoop", settings.dfLoop);
  prefs.putUChar("dfLoopF", settings.dfLoopFolder);
  prefs.putBool("dfAuto", settings.dfAutoplay);

  prefs.putBool("batOn", settings.batteryEnabled);
  prefs.putFloat("batDiv", settings.batteryDivider);
  prefs.putFloat("batCal", settings.batteryCalibration);
  prefs.putUChar("batCells", settings.batteryCells);
  prefs.putFloat("batFull", settings.batteryFull);
  prefs.putFloat("batEmpty", settings.batteryEmpty);
  prefs.putUChar("batLow", settings.batteryLow);
  prefs.putUChar("batCrit", settings.batteryCritical);

  prefs.putUChar("uiBlank", settings.oledBlankMode);
  prefs.putUShort("uiBlankS", settings.oledBlankAfterS);

  saveLedSettings();
}

/*
 * The ring's own keys, split out of saveSettings() because they are written on
 * a different schedule from everything else: a colour picker being dragged
 * produces a request per frame, and each one of those must not be a flash
 * write. See handleLeds() and the flush in management_loop().
 */
void saveLedSettings() {
  prefs.putBool("ledOn", settings.leds.enabled);
  prefs.putUChar("ledFx", settings.leds.effect);
  prefs.putUChar("ledBri", settings.leds.brightness);
  prefs.putUChar("ledSpd", settings.leds.speed);
  prefs.putUChar("ledRct", settings.leds.reactivity);
  prefs.putULong("ledCol", settings.leds.color);
  prefs.putULong("ledCol2", settings.leds.color2);
  prefs.putBool("ledIdle", settings.leds.idleOff);
  prefs.putUShort("ledIdleS", settings.leds.idleAfterS);
  ledsDirty = false;
}

/*
 * A colour as the dashboard's <input type="color"> hands it over: "#rrggbb".
 * A plain integer is accepted too, because the serial console and any script
 * anyone writes against this API will reach for one. Anything unparseable
 * leaves the current colour alone rather than turning the ring black, which is
 * the failure mode a typo in a script would otherwise have.
 */
uint32_t parseColor(JsonVariantConst value, uint32_t fallback) {
  if (value.is<const char *>()) {
    String text = value.as<String>();
    text.trim();
    if (text.startsWith("#")) text.remove(0, 1);
    if (text.length() != 6) return fallback;
    char *end = nullptr;
    const unsigned long parsed = strtoul(text.c_str(), &end, 16);
    if (!end || *end) return fallback;
    return (uint32_t)parsed & 0xFFFFFFu;
  }
  if (value.is<uint32_t>()) return value.as<uint32_t>() & 0xFFFFFFu;
  return fallback;
}

String colorText(uint32_t color) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%06lX", (unsigned long)(color & 0xFFFFFFu));
  return String(buf);
}

bool authenticated() {
  return server.authenticate("admin", settings.adminPassword.c_str());
}

template <typename T>
void sendJson(const T &doc, int code = 200) {
  String body;
  serializeJson(doc, body);
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

void sendError(int code, const String &message) {
  JsonDocument doc;
  doc["ok"] = false;
  doc["error"] = message;
  sendJson(doc, code);
}

// A plain 401, deliberately without a WWW-Authenticate challenge.
//
// The dashboard has its own sign-in modal and sends the Basic header itself.
// Adding the challenge makes the browser stack its native password dialog on
// top of ours for the same request, and cancelling that dialog reads as a
// failed login -- "admin does not work, it just asks again".
bool requireAuth() {
  if (authenticated()) return true;
  sendError(401, "Sign in required");
  return false;
}

bool readBody(JsonDocument &doc) {
  if (!server.hasArg("plain")) {
    sendError(400, "Missing JSON request body");
    return false;
  }
  const DeserializationError error = deserializeJson(doc, server.arg("plain"));
  if (error) {
    sendError(400, String("Invalid JSON: ") + error.c_str());
    return false;
  }
  return true;
}

const char *playbackName(PsPlayback playback) {
  switch (playback) {
    case PS_PLAYING: return "playing";
    case PS_PAUSED: return "paused";
    case PS_SEEKING: return "seeking";
    default: return "stopped";
  }
}

/*
 * Restarting, from a task that cannot be blocked.
 *
 * This used to be a deadline checked in management_loop(), which works right up
 * until the Arduino loop task is the thing that is stuck -- and then the OLED
 * sits on its last frame and the restart the dashboard promised never arrives.
 * A restart is exactly the wrong thing to make conditional on the health of the
 * task asking for it.
 *
 * So it gets its own task at a priority above everything this firmware runs,
 * doing nothing but sleeping and then resetting. esp_restart() rather than
 * ESP.restart(): the Arduino wrapper stops the Bluetooth controller on the way
 * out, which is both unnecessary here and one more thing that can block.
 */
void rebootTask(void *arg) {
  vTaskDelay(pdMS_TO_TICKS((uint32_t)(uintptr_t)arg));
  Serial.flush();
  esp_restart();
}

void scheduleReboot(uint32_t delayMs) {
  if (rebootPending) return;
  rebootPending = true;
  if (xTaskCreate(rebootTask, "reboot", 2048, (void *)(uintptr_t)delayMs,
                  configMAX_PRIORITIES - 2, nullptr) != pdPASS) {
    // Nothing left to schedule it with, so take the delay in line and go.
    delay(delayMs);
    Serial.flush();
    esp_restart();
  }
}

String macString(const uint8_t *mac) {
  char out[18];
  snprintf(out, sizeof(out), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
  return String(out);
}

bool parseMac(const String &text, esp_bd_addr_t out) {
  unsigned int b[6];
  if (sscanf(text.c_str(), "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3],
             &b[4], &b[5]) != 6) return false;
  for (int i = 0; i < 6; ++i) {
    if (b[i] > 255) return false;
    out[i] = (uint8_t)b[i];
  }
  return true;
}

void updateSet(const char *phase, const char *message, bool busy) {
  bool available;
  char tag[40];
  updatePhaseAt = millis();
  portENTER_CRITICAL(&updateMux);
  strlcpy(updateState.phase, phase, sizeof(updateState.phase));
  strlcpy(updateState.message, message, sizeof(updateState.message));
  updateState.busy = busy;
  available = updateState.available;
  strlcpy(tag, updateState.tag, sizeof(tag));
  portEXIT_CRITICAL(&updateMux);

  if (strcmp(phase, "checking") == 0) {
    ui_show_system_status(UI_STATUS_UPDATE, "Checking for update", message, -1, 0);
  } else if (strcmp(phase, "downloading") == 0) {
    ui_show_system_status(UI_STATUS_UPDATE, "Downloading firmware", message, 0, 0);
  } else if (strcmp(phase, "uploading") == 0) {
    ui_show_system_status(UI_STATUS_UPDATE, "Uploading firmware", message, 0, 0);
  } else if (strcmp(phase, "success") == 0) {
    ui_show_system_status(UI_STATUS_SUCCESS, "Firmware installed", message, 100, 0);
  } else if (strcmp(phase, "error") == 0) {
    ui_show_system_status(UI_STATUS_ERROR, "Update failed", message, -1, 9000);
  } else if (strcmp(phase, "ready") == 0) {
    ui_show_system_status(available ? UI_STATUS_UPDATE : UI_STATUS_SUCCESS,
                          available ? "Update available" : "Firmware current",
                          tag[0] ? tag : message, -1, 6500);
  }
}

void updateProgress(size_t done, size_t total) {
  portENTER_CRITICAL(&updateMux);
  updateState.done = (uint32_t)done;
  updateState.total = (uint32_t)total;
  char phase[16];
  strlcpy(phase, updateState.phase, sizeof(phase));
  portEXIT_CRITICAL(&updateMux);

  const int progress = total ? (int)((done * 100ULL) / total) : 0;
  ui_show_system_status(UI_STATUS_UPDATE,
                        strcmp(phase, "uploading") == 0 ? "Uploading firmware"
                                                         : "Installing firmware",
                        "Do not remove power", progress, 0);
}

UpdateState updateSnapshot() {
  UpdateState copy;
  portENTER_CRITICAL(&updateMux);
  memcpy(&copy, &updateState, sizeof(copy));
  portEXIT_CRITICAL(&updateMux);
  return copy;
}

void addUpdateJson(JsonObject obj) {
  const UpdateState u = updateSnapshot();
  obj["phase"] = u.phase;
  obj["message"] = u.message;
  obj["tag"] = u.tag;
  obj["releaseName"] = u.releaseName;
  obj["asset"] = u.assetName;
  obj["releaseUrl"] = u.releaseUrl;
  obj["done"] = u.done;
  obj["total"] = u.total;
  obj["available"] = u.available;
  obj["busy"] = u.busy;
}

bool globMatch(const char *pattern, const char *text) {
  while (*pattern) {
    if (*pattern == '*') {
      ++pattern;
      if (!*pattern) return true;
      while (*text) {
        if (globMatch(pattern, text)) return true;
        ++text;
      }
      return false;
    }
    if (*pattern != '?' && tolower((unsigned char)*pattern) !=
                               tolower((unsigned char)*text)) return false;
    if (!*text) return false;
    ++pattern;
    ++text;
  }
  return *text == 0;
}

bool isFirmwareAsset(const String &name) {
  String lower = name;
  lower.toLowerCase();
  // "factory" and "merged" images start with the bootloader at 0x1000, not with
  // the application. They pass the 0xE9 magic-byte check, so an OTA would
  // happily write one into the app slot and then fail to boot from it. Only a
  // plain application image belongs here.
  return lower.endsWith(".bin") && lower.indexOf("bootloader") < 0 &&
         lower.indexOf("partition") < 0 && lower.indexOf("littlefs") < 0 &&
         lower.indexOf("spiffs") < 0 && lower.indexOf("factory") < 0 &&
         lower.indexOf("merged") < 0;
}

String normalizedVersion(String value) {
  value.trim();
  // Anything ahead of the first digit is decoration -- "v2.3.0", "release-2.3.0"
  // and "firmware_2.3.0" are all the same release.
  while (value.length() && !isdigit((unsigned char)value[0])) value.remove(0, 1);
  return value;
}

/// One dotted component, advancing the cursor past it and its separator. Stops
/// at anything that is neither a digit nor a dot, so the "-rc1" in 2.3.0-rc1
/// reads as the end of the number.
long versionComponent(const char *&p) {
  if (!isdigit((unsigned char)*p)) return 0;
  long value = 0;
  while (isdigit((unsigned char)*p)) value = value * 10 + (*p++ - '0');
  if (*p == '.') ++p;
  return value;
}

/*
 * -1 if left is older than right, 0 if they are the same release, 1 if newer.
 *
 * This is what decides whether a release is an *update*. Comparing the strings
 * instead, as this used to, makes every release that is merely *different* look
 * installable -- so a speaker on 2.3.0 offers to "update" to the 2.0.2 that
 * happens to be latest on GitHub, and installing it is a silent downgrade.
 *
 * Missing components count as zero, so 2.3 and 2.3.0 are the same release, and
 * a pre-release suffix is ignored rather than ordered: 2.3.0-rc1 compares equal
 * to 2.3.0, which keeps a candidate build from being offered over the final.
 */
int compareVersions(const String &left, const String &right) {
  const String a = normalizedVersion(left);
  const String b = normalizedVersion(right);
  const char *pa = a.c_str();
  const char *pb = b.c_str();
  for (int i = 0; i < 4; ++i) {
    const long va = versionComponent(pa);
    const long vb = versionComponent(pb);
    if (va != vb) return va < vb ? -1 : 1;
  }
  return 0;
}

struct GithubJob {
  bool install;
  String repo;
  String pattern;
  String token;
};

struct GithubRelease {
  String tag;
  String name;
  String url;
  String assetName;
  String assetUrl;
  uint32_t assetSize;
};

// Transport failures are almost always about memory or the network, and the
// number alone says neither. The free-heap figure is what distinguishes "the
// CDN was unreachable" from "there was not enough room left for a handshake".
String heapNote() {
  return String(" (heap ") + (unsigned)ESP.getFreeHeap() + " B free)";
}

// Only github.com is entitled to the token.
bool isGithubHost(const String &url) {
  return url.startsWith("https://github.com/") ||
         url.startsWith("https://api.github.com/");
}

void prepareRequest(HTTPClient &http, const String &url, const String &token) {
  http.setUserAgent(String(APP_NAME) + "/" + FW_VERSION);
  // The default connect timeout is five seconds, which is a TCP handshake, a
  // TLS handshake and a certificate chain walk on a 240 MHz core -- comfortably
  // achievable, and comfortably missable on a slow uplink. Missing it looks
  // exactly like a refused connection.
  http.setConnectTimeout(20000);
  http.setTimeout(20000);
  // Nothing here reuses a connection: every request in this file goes to a
  // different host from the one before it.
  http.setReuse(false);
  if (token.length() && isGithubHost(url)) {
    http.addHeader("Authorization", "Bearer " + token);
  }
}

// ------------------------------------------------------------- metadata -----
bool fetchLatestRelease(const GithubJob &job, GithubRelease &out, String &error) {
  NetworkClientSecure tls;
  trustPublicRoots(tls);
  HTTPClient http;
  const String api =
      "https://api.github.com/repos/" + job.repo + "/releases/latest";
  if (!http.begin(tls, api)) {
    error = "Could not initialize HTTPS";
    return false;
  }
  prepareRequest(http, api, job.token);
  http.addHeader("Accept", "application/vnd.github+json");
  http.addHeader("X-GitHub-Api-Version", "2022-11-28");

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    if (code == HTTP_CODE_NOT_FOUND) {
      error = "No releases found for " + job.repo;
    } else {
      error = "GitHub check failed: " + httpErrorText(code);
      if (code < 0) error += heapNote();
    }
    return false;
  }

  JsonDocument filter;
  filter["tag_name"] = true;
  filter["name"] = true;
  filter["html_url"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;
  filter["assets"][0]["size"] = true;
  JsonDocument release;
  const DeserializationError jsonError = deserializeJson(
      release, http.getStream(), DeserializationOption::Filter(filter));
  if (jsonError) {
    error = "GitHub returned invalid release metadata";
    return false;
  }

  out.tag = release["tag_name"] | "";
  out.name = release["name"] | out.tag;
  out.url = release["html_url"] | "";
  out.assetSize = 0;
  for (JsonObject asset : release["assets"].as<JsonArray>()) {
    const String candidate = asset["name"] | "";
    if (isFirmwareAsset(candidate) &&
        globMatch(job.pattern.c_str(), candidate.c_str())) {
      out.assetName = candidate;
      out.assetUrl = asset["browser_download_url"] | "";
      out.assetSize = asset["size"] | 0;
      break;
    }
  }
  if (!out.assetUrl.length()) {
    error = "No release firmware matched the asset pattern";
    return false;
  }
  return true;
}

// -------------------------------------------------------------- install -----
bool flashFromStream(HTTPClient &http, String &error) {
  const int contentLength = http.getSize();
  if (contentLength <= 0) {
    error = "Release asset did not declare a size";
    return false;
  }
  if ((size_t)contentLength > ESP.getFreeSketchSpace()) {
    error = String("Firmware is ") + contentLength + " B; the OTA slot holds " +
            (unsigned)ESP.getFreeSketchSpace() + " B";
    return false;
  }
  if (!Update.begin((size_t)contentLength, U_FLASH)) {
    error = Update.errorString();
    return false;
  }
  Update.onProgress(updateProgress);
  const size_t written = Update.writeStream(http.getStream());
  if (written != (size_t)contentLength) {
    error = String("Transfer stopped after ") + (unsigned)written + " of " +
            contentLength + " B: " + Update.errorString();
    Update.abort();
    return false;
  }
  if (!Update.end(true)) {
    error = Update.errorString();
    return false;
  }
  return true;
}

/*
 * Where the install used to fail.
 *
 * browser_download_url points at github.com, which answers 302 with a signed,
 * time-limited URL on a storage host with a different name and a different
 * certificate chain. HTTPClient will follow that by itself, but it does it by
 * switching hosts underneath a live NetworkClientSecure: stop the socket,
 * reconnect the same mbedtls context to a different name. The check reached
 * api.github.com and the next handshake came back as HTTP -1 -- a refused
 * connection, which is what a handshake that never completes looks like from
 * up here.
 *
 * So the redirect is followed by hand. Each hop constructs its own client in
 * its own scope, which means every handshake starts from a clean context, only
 * one TLS session is ever allocated at a time, and the token stays with
 * github.com rather than being forwarded to a storage host that rejects
 * requests carrying two sets of credentials.
 */
constexpr int MAX_REDIRECTS = 5;
constexpr int CONNECT_ATTEMPTS = 3;

bool downloadAndFlash(const String &startUrl, const String &token, String &error) {
  String url = startUrl;

  for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
    for (int attempt = 1; attempt <= CONNECT_ATTEMPTS; ++attempt) {
      NetworkClientSecure tls;
      trustPublicRoots(tls);
      HTTPClient http;
      http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
      if (!http.begin(tls, url)) {
        error = "Could not initialize firmware download";
        return false;
      }
      prepareRequest(http, url, token);
      http.addHeader("Accept", "application/octet-stream");

      const int code = http.GET();
      if (code == HTTP_CODE_MOVED_PERMANENTLY || code == HTTP_CODE_FOUND ||
          code == HTTP_CODE_SEE_OTHER || code == HTTP_CODE_TEMPORARY_REDIRECT ||
          code == HTTP_CODE_PERMANENT_REDIRECT) {
        const String next = http.getLocation();
        if (!next.startsWith("https://")) {
          error = "Release download redirected off HTTPS";
          return false;
        }
        Serial.printf("[ota] redirect %d -> %.60s\n", code, next.c_str());
        url = next;
        break;  // next hop, with a fresh client
      }

      if (code == HTTP_CODE_OK) return flashFromStream(http, error);

      // A negative code never reached the server: DNS, TCP or TLS. Those are
      // worth one more try -- a redirect to a storage host is signed and only
      // valid for a few minutes, but three attempts fit inside that window.
      // An HTTP status did reach us, and retrying it would say the same thing.
      if (code >= 0 || attempt == CONNECT_ATTEMPTS) {
        error = "Firmware download failed: " + httpErrorText(code);
        if (code < 0) error += heapNote();
        return false;
      }
      Serial.printf("[ota] %s, retrying (%d/%d)\n",
                    httpErrorText(code).c_str(), attempt + 1, CONNECT_ATTEMPTS);
      delay(1000);
    }
  }

  error = "Release download redirected too many times";
  return false;
}

// ------------------------------------------------------------------ job -----
void runGithubJob(const GithubJob &job) {
  updateSet("checking", "Contacting GitHub releases", true);
  updateProgress(0, 0);

  GithubRelease release;
  String error;
  if (!fetchLatestRelease(job, release, error)) {
    updateSet("error", error.c_str(), false);
    return;
  }

  portENTER_CRITICAL(&updateMux);
  strlcpy(updateState.tag, release.tag.c_str(), sizeof(updateState.tag));
  strlcpy(updateState.releaseName, release.name.c_str(), sizeof(updateState.releaseName));
  strlcpy(updateState.assetName, release.assetName.c_str(), sizeof(updateState.assetName));
  strlcpy(updateState.releaseUrl, release.url.c_str(), sizeof(updateState.releaseUrl));
  updateState.total = release.assetSize;
  // Strictly newer, never merely different: see compareVersions().
  const bool available = compareVersions(release.tag, FW_VERSION) > 0;
  updateState.available = available;
  portEXIT_CRITICAL(&updateMux);

  if (!job.install) {
    updateSet("ready",
              available ? "A newer firmware release is available"
                        : "Firmware is up to date",
              false);
    return;
  }

  // The dashboard disables its own button, but the endpoint is reachable
  // without it and a downgrade is not a thing this speaker does: the OTA writer
  // would happily flash an older image, and whatever settings that firmware
  // does not understand would be the owner's problem afterwards.
  if (!available) {
    const String refusal = String("Release ") + release.tag +
                           " is not newer than " + FW_VERSION +
                           "; downgrading is not allowed";
    updateSet("error", refusal.c_str(), false);
    return;
  }

  updateSet("downloading", "Downloading and verifying firmware", true);
  if (sink && sink->is_connected()) sink->pause();
  Serial.printf("[ota] installing %s (%s, %u B, heap %u)\n",
                release.tag.c_str(), release.assetName.c_str(),
                (unsigned)release.assetSize, (unsigned)ESP.getFreeHeap());

  if (!downloadAndFlash(release.assetUrl, job.token, error)) {
    Serial.printf("[ota] %s\n", error.c_str());
    updateSet("error", error.c_str(), false);
    return;
  }

  updateSet("success", "Update installed; restarting", false);
  scheduleReboot(1800);
}

void githubTask(void *raw) {
  // Every object this job touches has to be destroyed before vTaskDelete(),
  // which never returns and so never unwinds the stack. Leaving a TLS context
  // or a JSON document behind on each check is how a board that updated fine
  // when it was fresh runs out of heap for a handshake three checks later.
  {
    std::unique_ptr<GithubJob> job((GithubJob *)raw);
    runGithubJob(*job);
  }
  vTaskDelete(nullptr);
}


/*
 * A TLS session against the root bundle needs roughly 45 KB, and it needs a
 * good part of it in one piece: two mbedtls record buffers, the session
 * context, and the server's certificate chain while it is being parsed. Failing
 * that allocation surfaces as HTTPClient's HTTP -1, which reads as a network
 * problem and sends you looking in the wrong place. Refuse in words instead.
 */
constexpr uint32_t TLS_HEAP_FLOOR = 60000;
constexpr uint32_t TLS_BLOCK_FLOOR = 34000;

bool startGithubJob(bool install) {
  const UpdateState u = updateSnapshot();
  if (u.busy) return false;
  if (WiFi.status() != WL_CONNECTED) {
    updateSet("error", "Internet connection required", false);
    return false;
  }
  const uint32_t heap = ESP.getFreeHeap();
  const uint32_t block = ESP.getMaxAllocHeap();
  if (heap < TLS_HEAP_FLOOR || block < TLS_BLOCK_FLOOR) {
    const String message = String("Not enough memory for a secure connection (") +
                           (unsigned)heap + " B free, largest block " +
                           (unsigned)block + " B). Restart the speaker.";
    updateSet("error", message.c_str(), false);
    return false;
  }
  if (settings.githubRepo.indexOf('/') <= 0 || settings.githubAsset.length() == 0) {
    updateSet("error", "Configure owner/repository and an asset pattern first", false);
    return false;
  }
  GithubJob *job = new GithubJob{install, settings.githubRepo, settings.githubAsset,
                                 settings.githubToken};
  // 16 KB. A TLS handshake that walks a certificate chain against the Mozilla
  // root bundle is the deepest thing this firmware does and the OTA writer runs
  // on top of it, so 14 KB was tight -- but a task stack comes out of the same
  // heap the handshake then has to allocate from, and this margin is not free.
  if (xTaskCreatePinnedToCore(githubTask, "github_ota", 16384, job, 1, nullptr, 0) != pdPASS) {
    delete job;
    updateSet("error", "Could not start update task", false);
    return false;
  }
  return true;
}

// Where a failed join actually fails.
//
// "Cannot connect to this network" is all a phone will tell you, and the three
// causes look identical from the outside. These events separate them:
//   nothing logged at all      -> association never completed. The radio was
//                                 off channel or too busy (see the setup window
//                                 below), or the client is out of range.
//   joined then left, reason 15 -> WPA2 four-way handshake timed out. Wrong
//                                 password, or the AP could not answer in time.
//   joined but no lease         -> the DHCP server could not allocate. Look at
//                                 the free-heap figure logged at AP start.
void onWifiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED: {
      const uint8_t *m = info.wifi_ap_staconnected.mac;
      apClients = WiFi.softAPgetStationNum();
      status_led_blip(2);
      Serial.printf("[ap] joined %s (aid %u, %u client%s)\n",
                    macString(m).c_str(), info.wifi_ap_staconnected.aid,
                    apClients, apClients == 1 ? "" : "s");
      break;
    }
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: {
      const uint8_t *m = info.wifi_ap_stadisconnected.mac;
      apClients = WiFi.softAPgetStationNum();
      Serial.printf("[ap] left %s (reason %u, %u client%s)\n",
                    macString(m).c_str(), info.wifi_ap_stadisconnected.reason,
                    apClients, apClients == 1 ? "" : "s");
      break;
    }
    // Station side. WIFI_REASON_BEACON_TIMEOUT (200) or NO_AP_FOUND (201)
    // after a working connection means the radio lost the router rather than
    // the credentials being wrong; AUTH_FAIL (202) / 4WAY_HANDSHAKE_TIMEOUT
    // (15) means they are.
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.printf("[sta] associated with %s on channel %u\n",
                    settings.ssid.c_str(),
                    info.wifi_sta_connected.channel);
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      const uint8_t reason = info.wifi_sta_disconnected.reason;
      Serial.printf("[sta] disconnected: %s (%u)\n",
                    WiFi.disconnectReasonName((wifi_err_reason_t)reason),
                    reason);
      break;
    }
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[sta] address %s\n",
                    IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
      Serial.println("[sta] lost the DHCP lease");
      break;
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
      Serial.printf("[ap] lease %s\n",
                    IPAddress(info.wifi_ap_staipassigned.ip.addr).toString().c_str());
      break;
    default:
      break;
  }
}

// Stop the station half from stealing the radio.
//
// The ESP32 has one transceiver on one channel. In AP_STA the SoftAP is dragged
// along wherever the station goes, and the Arduino core retries a failed
// station connection forever: STA.cpp's disconnect handler fires
// disconnect() + connect() on every failure, and each connect() sweeps the
// band. During those sweeps the SoftAP is off channel, so a client that saw the
// beacon gets no reply to its authentication or association frame and gives up.
// The channel argument to softAP() cannot help: the station half decides the
// channel.
void parkStation() {
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  staRetryStartedAt = 0;
  staRetryAt = millis() + STA_RETRY_PERIOD_MS;
}

void startAccessPoint() {
  if (apRunning) return;
  // The one combination the coexistence scheduler does not support. Combo mode
  // exists precisely to avoid it; refuse here as well as at every call site, so
  // a future caller cannot reintroduce it by accident.
  if (radioMode == RADIO_MODE_COMBO) return;

  // AP_STA is only worth its cost when the station is actually associated --
  // then the channel is settled and the AP simply shares it. Any other time the
  // setup AP is the interface people need to reach, so give it the radio alone.
  const bool stationUp = WiFi.status() == WL_CONNECTED;
  if (stationUp) {
    WiFi.mode(WIFI_AP_STA);
  } else {
    parkStation();
    WiFi.mode(WIFI_AP);
  }

  apName = defaultApName();
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                    IPAddress(255, 255, 255, 0));
  // 20 MHz only. HT40 in the 2.4 GHz band buys nothing for a config page and is
  // a known way to lose clients that will not associate to a 40 MHz AP.
  WiFi.softAPbandwidth(WIFI_BW_HT20);
  apRunning = WiFi.softAP(apName.c_str(), settings.apPassword.c_str(), 1, 0, 4);

  if (!apRunning) {
    Serial.printf("[web] setup AP failed to start (heap %u)\n",
                  (unsigned)ESP.getFreeHeap());
    return;
  }

  apClients = 0;
  // 300 ms beacons instead of the 100 ms default. A config page does not need
  // to be discovered three times a second, and the two thirds of beacon airtime
  // this gives back is airtime Bluetooth can use -- the difference between an
  // access point that coexists with A2DP and one that fights it.
  wifi_config_t apConf;
  if (esp_wifi_get_config(WIFI_IF_AP, &apConf) == ESP_OK) {
    apConf.ap.beacon_interval = AP_BEACON_INTERVAL_MS;
    esp_wifi_set_config(WIFI_IF_AP, &apConf);
  }
  startResponder();  // http://<hostname>.local works on the setup network too
  const String oled = apName + " / " + settings.apPassword + " / " +
                      WiFi.softAPIP().toString();
  ui_show_system_status(UI_STATUS_NETWORK, "Setup Wi-Fi", oled.c_str(), -1,
                        12000);
  Serial.printf("[web] setup AP: %s / %s -> http://%s (ch %d, heap %u)\n",
                apName.c_str(), settings.apPassword.c_str(),
                WiFi.softAPIP().toString().c_str(), WiFi.channel(),
                (unsigned)ESP.getFreeHeap());
}

// The recovery access point has done its job once the station is back. Leaving
// it up costs a permanent AP_STA mode, a degraded access point nobody can join
// while Bluetooth runs, and a second network for the user to trip over.
void stopAccessPoint(const char *why) {
  if (!apRunning) return;
  apRunning = false;
  apClients = 0;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  Serial.printf("[web] setup AP stopped (%s)\n", why);
}

// Periodic, deliberate station retry while the setup AP is serving. Auto
// reconnect stays off; one attempt every couple of minutes costs the AP a few
// seconds off channel instead of making it permanently unreachable, and no
// attempt is made at all while somebody is associated to the AP.
void serviceStationRetry() {
  if (!apRunning || !settings.ssid.length()) return;

  if (staRetryStartedAt) {
    if (WiFi.status() == WL_CONNECTED) {
      staRetryStartedAt = 0;
      return;
    }
    if (millis() - staRetryStartedAt > STA_RETRY_WINDOW_MS) {
      staRetryStartedAt = 0;
      WiFi.disconnect(false, false);
      WiFi.mode(WIFI_AP);
      staRetryAt = millis() + STA_RETRY_PERIOD_MS;
    }
    return;
  }

  if (WiFi.status() == WL_CONNECTED) return;
  if (!staRetryAt || (int32_t)(millis() - staRetryAt) < 0) return;
  staRetryAt = millis() + STA_RETRY_PERIOD_MS;
  if (apClients) return;  // somebody is configuring; do not go off channel

  staRetryStartedAt = millis();
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(settings.ssid.c_str(), settings.wifiPassword.c_str());
}

/*
 * The check that runs by itself, once, after the speaker comes up.
 *
 * It waits for the clock. A TLS handshake validates the server's certificate
 * against this clock, and until the first sync lands the clock is the build
 * stamp -- which on a board that has been in a drawer for a month reads as a
 * certificate that has not started yet, and fails the handshake exactly as if
 * the network were down. If SNTP cannot get out at all, the wait gives up after
 * a minute and tries anyway: a DS3231 or a recent NVS write may well have left
 * the clock good enough.
 */
constexpr uint32_t STARTUP_CHECK_SYNC_WAIT_MS = 60000;

void serviceStartupUpdateCheck() {
  if (!stationUpAt || WiFi.status() != WL_CONNECTED) return;
  if (settings.githubRepo.indexOf('/') <= 0) return;
  const UpdateState u = updateSnapshot();
  if (u.busy) return;  // a manual check is already running

  const bool synced = soft_clock_network_synced();
  if (startupCheckDone) {
    // One retry, and only this shape of it: the first attempt gave up waiting
    // for SNTP and went ahead on an unverified clock, it failed, and the sync
    // has since landed. That is precisely the failure a correct clock fixes.
    if (startupCheckHadClock || !synced) return;
    if (strcmp(u.phase, "error") != 0) return;
  } else if (!synced && millis() - stationUpAt < STARTUP_CHECK_SYNC_WAIT_MS) {
    return;
  }

  startupCheckDone = true;
  startupCheckHadClock = synced;
  Serial.printf("[ota] startup update check (clock %s)\n",
                soft_clock_source_name());
  startGithubJob(false);
}

void handleStatus() {
  if (!requireAuth()) return;
  PlayerInfo p;
  ps_snapshot(&p);
  JsonDocument doc;
  doc["ok"] = true;
  JsonObject fw = doc["firmware"].to<JsonObject>();
  fw["version"] = FW_VERSION;
  fw["built"] = String(__DATE__) + " " + __TIME__;
  fw["runningPartition"] = esp_ota_get_running_partition()->label;
  fw["nextPartition"] = esp_ota_get_next_update_partition(nullptr)->label;
  fw["freeOtaBytes"] = ESP.getFreeSketchSpace();

  JsonObject system = doc["system"].to<JsonObject>();
  system["chip"] = ESP.getChipModel();
  system["cores"] = ESP.getChipCores();
  system["cpuMHz"] = ESP.getCpuFreqMHz();
  system["heapFree"] = ESP.getFreeHeap();
  system["heapMin"] = ESP.getMinFreeHeap();
  system["heapMaxBlock"] = ESP.getMaxAllocHeap();
  system["flashBytes"] = ESP.getFlashChipSize();
  system["uptimeMs"] = millis();
  system["resetReason"] = (int)esp_reset_reason();

  /*
   * The speaker's own wall clock. Sent as a UTC epoch plus the stored offset
   * rather than as formatted text: the dashboard polls every two seconds, and a
   * clock that only moves when a poll lands looks broken. With the epoch and
   * the moment it arrived, the page can tick the seconds by itself between
   * polls and still never drift away from the speaker.
   */
  system["epoch"] = (uint32_t)time(nullptr);
  system["tzOffsetMinutes"] = soft_clock_utc_offset_min();
  system["clockSource"] = soft_clock_source_name();
  system["clockTrusted"] = soft_clock_trusted();
  system["clock24h"] = soft_clock_use_24h();

  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["connected"] = WiFi.status() == WL_CONNECTED;
  wifi["ssid"] = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "";
  wifi["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  wifi["ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
  wifi["hostname"] = settings.hostname;
  wifi["apRunning"] = apRunning;
  wifi["apSsid"] = apRunning ? apName : "";
  wifi["apIp"] = apRunning ? WiFi.softAPIP().toString() : "";
  wifi["apClients"] = apRunning ? WiFi.softAPgetStationNum() : 0;
  wifi["apChannel"] = apRunning ? WiFi.channel() : 0;
  wifi["radioMode"] = management_mode_name(radioMode);

  // Which radios this boot is running, so the dashboard can explain itself
  // rather than guessing from whether Bluetooth happens to be up.
  JsonObject mode = doc["mode"].to<JsonObject>();
  mode["id"] = (int)radioMode;
  mode["name"] = management_mode_name(radioMode);
  mode["wifi"] = radio_mode_has_wifi(radioMode);
  mode["bluetooth"] = radio_mode_has_a2dp(radioMode);
  mode["ble"] = radio_mode_has_ble(radioMode);
  mode["dfplayer"] = radio_mode_has_dfplayer(radioMode);
  // The mode is offerable only if the driver was compiled in. Reported rather
  // than assumed, so a -DDFPLAYER_ENABLED=0 build greys the button out instead
  // of rebooting into a mode with no audio source in it.
  mode["dfBuilt"] = DFPLAYER_ENABLED ? true : false;
  // Combo mode cannot raise the setup access point, so it is only offerable
  // once a network has been saved. The dashboard greys it out until then.
  mode["comboReady"] = settings.ssid.length() > 0;

  JsonObject bt = doc["bluetooth"].to<JsonObject>();
  bt["active"] = btActive;
  bt["connected"] = p.connected;
  bt["avrcp"] = p.avrc;
  bt["streaming"] = p.streaming;
  bt["device"] = p.peer;
  bt["address"] = p.connected ? macString(p.peer_addr) : "";
  bt["sampleRate"] = p.sample_rate;
  bt["connectedMs"] = p.connected ? millis() - p.connected_at : 0;

  JsonObject media = doc["media"].to<JsonObject>();
  media["title"] = p.title;
  media["artist"] = p.artist;
  media["album"] = p.album;
  media["genre"] = p.genre;
  media["track"] = p.track_num;
  media["trackCount"] = p.track_count;
  media["durationMs"] = p.track_ms;
  media["positionMs"] = ps_position_ms(p, millis());
  media["state"] = playbackName(p.playback);
  media["volume"] = p.volume;

  // Wi-Fi + BLE mode only. The dashboard uses `running` to decide whether to
  // draw the network player at all, so the block is always present and always
  // honest rather than appearing and disappearing.
  JsonObject net = doc["network"].to<JsonObject>();
  net["running"] = net_audio_running();
  net["ble"] = ble_control_running();
  net["bleClients"] = ble_control_clients();
  if (net_audio_running()) {
    NetAudioStatus n;
    net_audio_snapshot(&n);
    const char *state = "idle";
    switch (n.state) {
      case NET_AUDIO_OPENING: state = "opening"; break;
      case NET_AUDIO_PLAYING: state = net_audio_active() ? "playing" : "buffering"; break;
      case NET_AUDIO_PAUSED: state = "paused"; break;
      case NET_AUDIO_ERROR: state = "error"; break;
      default: break;
    }
    net["state"] = state;
    net["url"] = n.url;
    net["origin"] = n.origin;
    net["error"] = n.error;
    net["renderer"] = n.renderer_up;
    net["sampleRate"] = n.sample_rate;
  }

  /*
   * DFPlayer mode only, and always present for the same reason the network block
   * is: the dashboard decides whether to draw the page from `running`, and a key
   * that appears and disappears is harder to write against than one that is
   * always there and sometimes false.
   */
  JsonObject df = doc["dfplayer"].to<JsonObject>();
  df["running"] = df_player_running();
  if (df_player_running()) {
    DfStatus d;
    // A stale copy is reported as stale rather than dressed up as an offline
    // module: the dashboard leaves the last values on screen for one poll
    // instead of flashing "not answering" at a mutex that was merely busy.
    df["stale"] = !df_player_snapshot(&d);
    df["online"] = d.online;
    df["asleep"] = d.asleep;
    df["state"] = df_state_name(d.state);
    df["busy"] = d.busy;
    df["source"] = (int)d.source;
    df["sourceName"] = df_source_name(d.source);
    df["reported"] = (int)d.reported;
    df["sd"] = d.sdPresent;
    df["usb"] = d.usbPresent;
    df["flash"] = d.flashPresent;
    df["pc"] = d.pcLink;
    df["track"] = d.track;
    df["totalTracks"] = d.totalTracks;
    df["folders"] = d.folders;
    df["folder"] = d.folder;
    df["folderTracks"] = d.folderTracks;
    df["queriedFolder"] = d.queriedFolder;
    df["volume"] = d.volume;
    df["volumeMax"] = DF_VOLUME_MAX;
    df["eq"] = d.eq;
    df["eqName"] = df_eq_name(d.eq);
    df["loop"] = (int)d.loop;
    df["loopName"] = df_loop_name(d.loop);
    df["dac"] = d.dacOn;
    df["version"] = d.version;
    df["finished"] = d.finished;
    df["error"] = d.error;
    df["led"] = (int)d.ledMode;
    df["ledOn"] = d.ledOn;
    // Which of the module's own pins this board actually wired, so the hardware
    // buttons on the dashboard are only offered when they would do something.
    JsonObject pins = df["pins"].to<JsonObject>();
    pins["io1"] = df_player_pin_available(DF_PIN_IO1);
    pins["io2"] = df_player_pin_available(DF_PIN_IO2);
    pins["adkey1"] = df_player_pin_available(DF_PIN_ADKEY1);
    pins["adkey2"] = df_player_pin_available(DF_PIN_ADKEY2);
    pins["busy"] = PIN_DF_BUSY >= 0;
    pins["led"] = PIN_DF_LED >= 0;
    pins["usbDetect"] = PIN_DF_USB_DETECT >= 0;
  }

  // The battery is not mode-specific: it powers the whole speaker, so it is
  // reported in every mode, and `enabled` is false rather than absent when no
  // pack is wired.
  BatteryStatus b;
  battery_snapshot(&b);
  JsonObject bat = doc["battery"].to<JsonObject>();
  // "wired" is about the build, "enabled" about the setting. The dashboard needs
  // both: a board with no sense pin should not show a battery card at all, but a
  // board that has one and has the gauge switched off should show the card and
  // say where the switch is -- otherwise the feature leaves no trace of itself.
  bat["wired"] = PIN_BATTERY_SENSE >= 0;
  bat["enabled"] = b.enabled;
  bat["present"] = b.present;
  bat["state"] = battery_state_name(b.state);
  bat["percent"] = b.percent;
  bat["volts"] = serialized(String(b.volts, 3));
  bat["cellVolts"] = serialized(String(b.cellVolts, 3));
  bat["cells"] = b.cells;
  bat["low"] = b.low;
  bat["critical"] = b.critical;
  bat["charging"] = b.charging;
  bat["chargeDone"] = b.chargeDone;
  bat["chargePins"] = b.haveChargePins;
  bat["pinMillivolts"] = b.millivoltsAtPin;
  bat["samples"] = b.samples;
  bat["lowPercent"] = b.lowPercent;
  bat["criticalPercent"] = b.criticalPercent;

  JsonObject led = doc["leds"].to<JsonObject>();
  led["wired"] = LEDS_ENABLED && PIN_LEDS >= 0;
  led["present"] = leds_present();
  led["enabled"] = settings.leds.enabled;
  led["effect"] = settings.leds.effect;
  led["effectName"] = leds_effect_name(settings.leds.effect);
  led["hearingAudio"] = leds_hearing_audio();
  led["resting"] = leds_resting();
  led["idleSeconds"] = leds_idle_ms() / 1000;

  addUpdateJson(doc["update"].to<JsonObject>());
  sendJson(doc);
}

void handleAuth() {
  if (!requireAuth()) return;
  JsonDocument doc;
  doc["ok"] = true;
  doc["defaultPassword"] = settings.adminPassword == "admin";
  sendJson(doc);
}

/*
 * Playback control, whichever source is running.
 *
 * The two paths answer the same verbs so the dashboard does not need a second
 * set of buttons: the Bluetooth one forwards them to the phone over AVRCP, the
 * network one drives the player directly. Where they differ is what they can
 * do -- there is no "next track" on a single stream URL -- and the difference
 * is reported rather than faked.
 */
void handleNetworkMedia(const String &action, JsonDocument &body) {
  if (action == "play") {
    net_audio_play();
  } else if (action == "pause") {
    net_audio_pause();
  } else if (action == "stop") {
    net_audio_stop();
  } else if (action == "toggle") {
    NetAudioStatus n;
    net_audio_snapshot(&n);
    n.state == NET_AUDIO_PLAYING ? net_audio_pause() : net_audio_play();
  } else if (action == "volume") {
    const int volume = constrain(body["value"] | 0, 0, 127);
    if (volume > 0) mutedFrom = (uint8_t)volume;
    net_audio_set_volume((uint8_t)volume);
  } else if (action == "mute") {
    net_audio_set_volume(net_audio_volume() ? 0
                                            : (mutedFrom ? mutedFrom : 80));
  } else if (action == "url") {
    const String url = body["value"] | "";
    if (!url.length()) {
      sendError(400, "No stream address given");
      return;
    }
    if (!net_audio_play_url(url.c_str(), "url")) {
      sendError(400, "That address is empty or longer than the player accepts");
      return;
    }
  } else {
    // next / previous / forward / rewind. A single stream has nothing to skip
    // to, and pretending otherwise makes the buttons look broken instead of
    // inapplicable.
    sendError(409, "Network playback is a single stream: there is nothing to "
                   "skip to. Use play, pause, stop or volume.");
    return;
  }
  JsonDocument reply;
  reply["ok"] = true;
  sendJson(reply);
}

/*
 * The same verbs again, for the DFPlayer.
 *
 * This one can do more of them than the network player: the module has a real
 * playlist, so next and previous mean something. What it cannot do is seek --
 * the YX5200 has no position, forwards or backwards -- so fast forward and
 * rewind are refused with a reason rather than wired to something approximate.
 */
void handleDfMedia(const String &action, JsonDocument &body) {
  bool ok = true;
  if (action == "play") ok = df_player_play();
  else if (action == "pause") ok = df_player_pause();
  else if (action == "toggle") ok = df_player_toggle();
  else if (action == "stop") ok = df_player_stop();
  else if (action == "next") ok = df_player_next();
  else if (action == "previous") ok = df_player_previous();
  else if (action == "volume") {
    const int volume = constrain(body["value"] | 0, 0, 127);
    if (volume > 0) mutedFrom = (uint8_t)volume;
    ok = df_player_set_volume((uint8_t)volume);
  } else if (action == "mute") {
    ok = df_player_set_volume(df_player_volume() ? 0
                                                : (mutedFrom ? mutedFrom : 80));
  } else if (action == "forward" || action == "rewind") {
    sendError(409, "The DFPlayer cannot seek within a track: it reports no "
                   "position and takes no seek command. Use next and previous.");
    return;
  } else if (action == "url") {
    sendError(409, "The DFPlayer plays from its own card or USB drive, not from "
                   "a network address. Pick a track on the Media page.");
    return;
  } else {
    sendError(400, "Unknown media action");
    return;
  }
  if (!ok) {
    sendError(503, "The DFPlayer command queue is full; the module is not "
                   "keeping up. Try again in a moment.");
    return;
  }
  JsonDocument reply;
  reply["ok"] = true;
  sendJson(reply);
}

void handleMedia() {
  if (!requireAuth()) return;
  if (df_player_running()) {
    JsonDocument body;
    if (!readBody(body)) return;
    handleDfMedia(body["action"] | "", body);
    return;
  }
  if (radio_mode_has_dfplayer(radioMode)) {
    sendError(409, "The DFPlayer driver did not start in this boot; check the "
                   "serial log.");
    return;
  }
  if (net_audio_running()) {
    JsonDocument body;
    if (!readBody(body)) return;
    handleNetworkMedia(body["action"] | "", body);
    return;
  }
  if (radio_mode_has_ble(radioMode)) {
    sendError(409, "The network player is still waiting for a Wi-Fi address");
    return;
  }
  if (!btActive) {
    sendError(409, "Bluetooth is off in Wi-Fi mode. Switch to Wi-Fi + BT to "
                   "control playback from here.");
    return;
  }
  JsonDocument body;
  if (!readBody(body)) return;
  const String action = body["action"] | "";
  PlayerInfo p;
  ps_snapshot(&p);
  if (action != "volume" && action != "mute" && !p.avrc) {
    sendError(409, "The connected device has no AVRCP control channel");
    return;
  }
  if (action == "play") sink->play();
  else if (action == "pause") sink->pause();
  else if (action == "toggle") p.playback == PS_PLAYING ? sink->pause() : sink->play();
  else if (action == "stop") sink->stop();
  else if (action == "next") sink->next();
  else if (action == "previous") sink->previous();
  else if (action == "forward") sink->fast_forward();
  else if (action == "rewind") sink->rewind();
  else if (action == "volume") {
    const int volume = constrain(body["value"] | 0, 0, 127);
    if (volume > 0) mutedFrom = volume;
    sink->set_volume((uint8_t)volume);
  } else if (action == "mute") {
    if (p.volume) {
      mutedFrom = p.volume;
      sink->set_volume(0);
    } else sink->set_volume(mutedFrom ? mutedFrom : 80);
  } else {
    sendError(400, "Unknown media action");
    return;
  }
  JsonDocument reply;
  reply["ok"] = true;
  sendJson(reply);
}

void handleDevices() {
  if (!requireAuth()) return;
  PlayerInfo p;
  ps_snapshot(&p);
  JsonDocument doc;
  doc["ok"] = true;
  JsonArray devices = doc["devices"].to<JsonArray>();
  // Bluedroid is not running in Wi-Fi mode, so there is nothing to enumerate
  // and its API would fail anyway. Answer honestly rather than erroring.
  if (!btActive) {
    doc["unavailable"] = radio_mode_has_ble(radioMode)
                             ? "Wi-Fi + BLE mode runs no A2DP sink"
                         : radio_mode_has_dfplayer(radioMode)
                             ? "DFPlayer mode runs no A2DP sink"
                             : "Bluetooth is off in Wi-Fi mode";
    sendJson(doc);
    return;
  }
  const int count = esp_bt_gap_get_bond_device_num();
  if (count > 0) {
    esp_bd_addr_t *list = (esp_bd_addr_t *)malloc((size_t)count * sizeof(esp_bd_addr_t));
    if (list) {
      int actual = count;
      if (esp_bt_gap_get_bond_device_list(&actual, list) == ESP_OK) {
        for (int i = 0; i < actual; ++i) {
          JsonObject item = devices.add<JsonObject>();
          const bool current = p.connected && memcmp(list[i], p.peer_addr, 6) == 0;
          item["address"] = macString(list[i]);
          item["name"] = current && p.peer[0] ? p.peer : "Paired device";
          item["connected"] = current;
          item["streaming"] = current && p.streaming;
        }
      }
      free(list);
    }
  }
  sendJson(doc);
}

void handleDeviceAction() {
  if (!requireAuth()) return;
  if (!btActive) {
    sendError(409, radio_mode_has_ble(radioMode)
                       ? "Wi-Fi + BLE mode runs no A2DP sink, so there are no "
                         "pairings to manage. Switch to a Bluetooth mode first."
                   : radio_mode_has_dfplayer(radioMode)
                       ? "DFPlayer mode runs no A2DP sink, so there are no "
                         "pairings to manage. Switch to a Bluetooth mode first."
                       : "Bluetooth is off in Wi-Fi mode. Switch to Wi-Fi + BT "
                         "to manage devices from here.");
    return;
  }
  JsonDocument body;
  if (!readBody(body)) return;
  const String action = body["action"] | "";
  if (action == "disconnect") {
    sink->disconnect();
  } else if (action == "forget") {
    esp_bd_addr_t address;
    if (!parseMac(body["address"] | "", address)) {
      sendError(400, "Invalid Bluetooth address");
      return;
    }
    PlayerInfo p;
    ps_snapshot(&p);
    if (p.connected && memcmp(address, p.peer_addr, 6) == 0) sink->disconnect();
    const esp_err_t result = esp_bt_gap_remove_bond_device(address);
    if (result != ESP_OK) {
      sendError(500, String("Bluetooth stack error: ") + esp_err_to_name(result));
      return;
    }
  } else {
    sendError(400, "Unknown device action");
    return;
  }
  JsonDocument reply;
  reply["ok"] = true;
  sendJson(reply);
}

/*
 * Everything about the DFPlayer that is not a transport verb.
 *
 * One endpoint rather than a dozen, because the module's controls are a flat set
 * of independent settings and the dashboard sends whichever one the user
 * touched. Each action maps onto exactly one driver call; the driver does the
 * range checking, and a rejected value comes back as a 400 with the reason
 * rather than being silently clamped -- a track number the card does not have is
 * worth telling somebody about.
 */
/*
 * The folder index, as a page of its own.
 *
 * Deliberately not part of /api/status: it is up to ninety-nine numbers, it
 * only changes while a scan is running, and the status poll fires every two
 * seconds whether or not anybody is looking at the Media page. The browser asks
 * for this when it opens and again while a scan is in progress.
 *
 * Folders with no files are dropped rather than sent as zeroes -- a card with
 * three folders on it should not cost ninety-six entries of nothing.
 */
void handleDfLibrary() {
  if (!requireAuth()) return;
  if (!df_player_running()) {
    sendError(409, "The DFPlayer only runs in DFPlayer mode.");
    return;
  }
  DfStatus d;
  const bool read = df_player_snapshot(&d);

  uint16_t counts[DF_MAX_FOLDERS];
  df_player_folder_counts(counts, DF_MAX_FOLDERS);

  uint8_t done = 0, total = 0;
  const bool scanning = df_player_scanning(&done, &total);

  JsonDocument doc;
  doc["ok"] = true;
  doc["source"] = df_source_name(d.source);
  doc["online"] = read && d.online;
  doc["totalTracks"] = d.totalTracks;
  doc["reportedFolders"] = d.folders;
  doc["scanning"] = scanning;
  doc["scanned"] = df_player_scanned();
  doc["scanDone"] = done;
  doc["scanTotal"] = total;
  doc["track"] = d.track;
  doc["folder"] = d.folder;
  doc["busy"] = d.busy;

  uint16_t known = 0;
  JsonArray folders = doc["folders"].to<JsonArray>();
  for (uint8_t i = 0; i < DF_MAX_FOLDERS; ++i) {
    if (!counts[i]) continue;
    ++known;
    JsonObject entry = folders.add<JsonObject>();
    entry["folder"] = i + 1;
    entry["files"] = counts[i];
  }
  doc["knownFolders"] = known;
  sendJson(doc);
}

void handleDfPlayer() {
  if (!requireAuth()) return;
  if (!df_player_running()) {
    sendError(409, radio_mode_has_dfplayer(radioMode)
                       ? "The DFPlayer driver did not start; check the serial "
                         "log for a UART or memory failure."
                       : "The DFPlayer only runs in DFPlayer mode. Switch to it "
                         "under Radio mode on the Overview page.");
    return;
  }
  JsonDocument body;
  if (!readBody(body)) return;
  const String action = body["action"] | "";

  /*
    * `ok` is "the driver accepted it" and `failure` is why the *value* was
    * wrong. Keeping them apart matters for the error message: folding the range
    * test into the same expression made a full command queue report itself as
    * "track number must be between 1 and 2999", which is the least helpful
    * possible answer in the one place somebody is trying to work out what went
    * wrong. `bad` is set instead when the value itself is out of range.
    */
  bool ok = true;
  bool bad = false;
  String failure;

  if (action == "refresh") {
    ok = df_player_refresh();
  } else if (action == "source") {
    const String want = body["value"] | "";
    const DfSource source = want == "usb"     ? DF_SRC_USB
                            : want == "flash" ? DF_SRC_FLASH
                            : want == "aux"   ? DF_SRC_AUX
                            : want == "sd"    ? DF_SRC_SD
                                              : (DfSource)0;
    if (source == (DfSource)0) {
      sendError(400, "Source must be sd, usb, flash or aux");
      return;
    }
    ok = df_player_set_source(source);
  } else if (action == "track") {
    // Checked as an int before it is narrowed. Casting first turns 65537 into 1
    // and accepts it as track 1, which is a request nobody made.
    const int track = body["value"] | 0;
    bad = track < 1 || track > 2999;
    ok = bad || df_player_play_track((uint16_t)track);
    failure = "Track number must be between 1 and 2999";
  } else if (action == "folder") {
    const int folder = body["folder"] | 0;
    const int file = body["file"] | 0;
    bad = folder < 1 || folder > 99 || file < 1 || file > 255;
    ok = bad || df_player_play_folder((uint8_t)folder, (uint8_t)file);
    failure = "Folder must be 1-99 and track 1-255, matching the zero-padded "
              "names the module needs (/01/003.mp3)";
  } else if (action == "mp3") {
    const int track = body["value"] | 0;
    bad = track < 1 || track > 3000;
    ok = bad || df_player_play_mp3((uint16_t)track);
    failure = "The MP3 folder holds tracks 1-3000 (/MP3/0007.mp3)";
  } else if (action == "advert") {
    const int track = body["value"] | 0;
    bad = track < 1 || track > 3000;
    ok = bad || df_player_advertise((uint16_t)track);
    failure = "The ADVERT folder holds tracks 1-3000, and something has to be "
              "playing for an announcement to interrupt";
  } else if (action == "advertStop") {
    ok = df_player_advertise_stop();
  } else if (action == "volumeRaw") {
    ok = df_player_set_volume_raw(
        (uint8_t)constrain(body["value"] | 0, 0, (int)DF_VOLUME_MAX));
  } else if (action == "volumeStep") {
    ok = df_player_volume_step(body["up"] | true);
  } else if (action == "eq") {
    const int eq = body["value"] | -1;
    bad = eq < 0 || eq > 5;
    ok = bad || df_player_set_eq((uint8_t)eq);
    failure = "EQ must be 0-5 (normal, pop, rock, jazz, classic, bass)";
  } else if (action == "loop") {
    const String want = body["value"] | "off";
    const DfLoop loop = want == "track"    ? DF_LOOP_TRACK
                        : want == "folder" ? DF_LOOP_FOLDER
                        : want == "all"    ? DF_LOOP_ALL
                        : want == "random" ? DF_LOOP_RANDOM
                                           : DF_LOOP_OFF;
    // Only folder-repeat takes a folder, so only folder-repeat can be refused
    // for a bad one. A dashboard sending "folder": 0 alongside "loop off" was
    // being told to fix a field that mode never reads.
    const int folder = body["folder"] | 1;
    bad = loop == DF_LOOP_FOLDER && (folder < 1 || folder > 99);
    ok = bad || df_player_set_loop(loop, (uint8_t)(bad ? 1 : folder));
    failure = "Folder repeat needs a folder between 1 and 99";
  } else if (action == "dac") {
    ok = df_player_set_dac(body["value"] | true);
  } else if (action == "reset") {
    ok = df_player_reset();
  } else if (action == "standby") {
    ok = df_player_standby();
  } else if (action == "wake") {
    ok = df_player_wake();
  } else if (action == "scan") {
    ok = df_player_scan();
  } else if (action == "queryFolder") {
    const int folder = body["folder"] | 0;
    bad = folder < 1 || folder > 99;
    ok = bad || df_player_query_folder((uint8_t)folder);
    failure = "Folder must be between 1 and 99";
  } else if (action == "pin") {
    const String which = body["pin"] | "";
    const DfPin pin = which == "io1"      ? DF_PIN_IO1
                      : which == "io2"    ? DF_PIN_IO2
                      : which == "adkey1" ? DF_PIN_ADKEY1
                      : which == "adkey2" ? DF_PIN_ADKEY2
                                          : DF_PIN_COUNT;
    if (pin == DF_PIN_COUNT) {
      sendError(400, "Pin must be io1, io2, adkey1 or adkey2");
      return;
    }
    bad = !df_player_pin_available(pin);
    ok = bad || df_player_pulse(pin, body["long"] | false);
    failure = "That pin is not wired on this board; see hw_config.h";
  } else if (action == "led") {
    const String want = body["value"] | "auto";
    df_player_set_led(want == "on"      ? DF_LED_ON
                      : want == "off"   ? DF_LED_OFF
                      : want == "blink" ? DF_LED_BLINK
                                        : DF_LED_AUTO);
  } else if (action == "saveDefaults") {
    // The module keeps nothing across a power cycle, so "remember this" means
    // storing it here and sending it again at every boot.
    //
    // Which is exactly why a snapshot that is not a reading cannot be stored: a
    // timed-out copy is all zeroes, and writing it would set the next boot's
    // volume to 0 and its source to something the driver then silently
    // substitutes. Refuse and say so; the caller can try again.
    DfStatus d;
    if (!df_player_snapshot(&d)) {
      sendError(503, "Could not read the module's current settings just now. "
                     "Nothing was saved; try again in a moment.");
      return;
    }
    settings.dfSource = (uint8_t)d.source;
    settings.dfVolume = d.volume;
    settings.dfEq = d.eq;
    settings.dfLoop = (uint8_t)d.loop;
    if (d.folder) settings.dfLoopFolder = (uint8_t)d.folder;
    if (!body["autoplay"].isNull()) settings.dfAutoplay = body["autoplay"].as<bool>();
    saveSettings();
  } else {
    sendError(400, "Unknown DFPlayer action");
    return;
  }

  if (bad) {
    sendError(400, failure.length() ? failure : "That value is out of range");
    return;
  }
  if (!ok) {
    sendError(503, "The DFPlayer command queue is full; the module is not "
                   "keeping up. Try again in a moment.");
    return;
  }
  JsonDocument reply;
  reply["ok"] = true;
  sendJson(reply);
}

/*
 * Battery actions that are not settings.
 *
 * Calibration is the only interesting one, and it is here rather than in the
 * settings form because it is not a number the user knows -- it is a number
 * derived from one they can measure. Put a meter on the pack, type what it says,
 * and the trim that makes the firmware agree is computed and stored.
 */
void handleBattery() {
  if (!requireAuth()) return;
  JsonDocument body;
  if (!readBody(body)) return;
  const String action = body["action"] | "";

  if (action == "calibrate") {
    const float actual = body["volts"] | 0.0f;
    const float trim = battery_calibration_for(actual);
    if (trim <= 0.0f) {
      sendError(400, "Cannot calibrate against that. Either nothing plausible "
                     "is on the sense pin, or the reading you gave is more than "
                     "twice what the divider suggests -- check the divider "
                     "ratio first.");
      return;
    }
    settings.batteryCalibration = trim;
    saveSettings();
    applyBatterySettings();
    JsonDocument reply;
    reply["ok"] = true;
    reply["calibration"] = serialized(String(trim, 4));
    sendJson(reply);
    return;
  }
  if (action == "refresh") {
    applyBatterySettings();  // re-samples as a side effect
    JsonDocument reply;
    reply["ok"] = true;
    sendJson(reply);
    return;
  }
  sendError(400, "Unknown battery action");
}

/*
 * The lighting.
 *
 * Every field is optional, so the dashboard can send just the one thing that
 * changed as a slider moves rather than the whole configuration each time.
 * Changes apply on the ring within a frame; the flash write is deferred, and
 * management_loop() makes it once the requests stop arriving.
 */
void handleLeds() {
  if (!requireAuth()) return;
  JsonDocument body;
  if (!readBody(body)) return;

  LedConfig next = settings.leds;
  if (!body["enabled"].isNull()) next.enabled = body["enabled"].as<bool>();
  if (!body["effect"].isNull()) {
    const int effect = body["effect"].as<int>();
    if (effect < 0 || effect >= LED_FX_COUNT) {
      sendError(400, "Unknown lighting effect");
      return;
    }
    next.effect = (uint8_t)effect;
  }
  if (!body["brightness"].isNull()) {
    next.brightness = (uint8_t)constrain(body["brightness"].as<int>(), 0, 255);
  }
  if (!body["speed"].isNull()) {
    next.speed = (uint8_t)constrain(body["speed"].as<int>(), 0, 255);
  }
  if (!body["reactivity"].isNull()) {
    next.reactivity = (uint8_t)constrain(body["reactivity"].as<int>(), 0, 100);
  }
  next.color = parseColor(body["color"], next.color);
  next.color2 = parseColor(body["color2"], next.color2);
  if (!body["idleOff"].isNull()) next.idleOff = body["idleOff"].as<bool>();
  if (!body["idleAfterSeconds"].isNull()) {
    next.idleAfterS = (uint16_t)constrain(body["idleAfterSeconds"].as<int>(),
                                          (int)LED_IDLE_AFTER_S_MIN,
                                          (int)LED_IDLE_AFTER_S_MAX);
  }

  settings.leds = next;
  leds_configure(next);
  ledsDirty = true;
  ledsDirtyAt = millis();

  JsonDocument reply;
  reply["ok"] = true;
  reply["effectName"] = leds_effect_name(next.effect);
  reply["effectHint"] = leds_effect_hint(next.effect);
  reply["hearingAudio"] = leds_hearing_audio();
  reply["resting"] = leds_resting();
  sendJson(reply);
}

void handleWifiScan() {
  if (!requireAuth()) return;
  ui_show_system_status(UI_STATUS_NETWORK, "Scanning Wi-Fi", "Radio scan in progress",
                        -1, 0);
  const int found = WiFi.scanNetworks(false, true, false, 250);
  JsonDocument doc;
  doc["ok"] = found >= 0;
  JsonArray networks = doc["networks"].to<JsonArray>();
  for (int i = 0; i < found; ++i) {
    JsonObject network = networks.add<JsonObject>();
    network["ssid"] = WiFi.SSID(i);
    network["rssi"] = WiFi.RSSI(i);
    network["channel"] = WiFi.channel(i);
    network["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  char result[36];
  snprintf(result, sizeof(result), "%d network%s found", found < 0 ? 0 : found,
           found == 1 ? "" : "s");
  ui_show_system_status(found >= 0 ? UI_STATUS_SUCCESS : UI_STATUS_ERROR,
                        found >= 0 ? "Wi-Fi scan complete" : "Wi-Fi scan failed",
                        result, -1, 3500);
  sendJson(doc);
}

void handleWifiSave() {
  if (!requireAuth()) return;
  JsonDocument body;
  if (!readBody(body)) return;
  const String ssid = body["ssid"] | "";
  if (!ssid.length() || ssid.length() > 32) {
    sendError(400, "SSID must contain 1 to 32 characters");
    return;
  }
  settings.ssid = ssid;
  if (!body["password"].isNull()) settings.wifiPassword = body["password"].as<String>();
  saveSettings();
  JsonDocument reply;
  reply["ok"] = true;
  reply["message"] = "Wi-Fi saved; restarting";
  sendJson(reply);
  ui_show_system_status(UI_STATUS_RESTART, "Wi-Fi saved", "Restarting speaker", -1,
                        0);
  scheduleReboot(900);
}

void handleSettingsGet() {
  if (!requireAuth()) return;
  JsonDocument doc;
  doc["ok"] = true;
  doc["hostname"] = settings.hostname;
  doc["deviceName"] = settings.deviceName;
  doc["savedSsid"] = settings.ssid;
  doc["apAlways"] = settings.apAlways;
  doc["apPasswordSet"] = settings.apPassword.length() >= 8;
  doc["defaultAdminPassword"] = settings.adminPassword == "admin";
  doc["githubRepo"] = settings.githubRepo;
  doc["githubAsset"] = settings.githubAsset;
  doc["githubTokenSet"] = settings.githubToken.length() > 0;
  doc["clockSource"] = soft_clock_source_name();
  doc["clockOffsetMinutes"] = soft_clock_utc_offset_min();
  doc["clockNetworkSynced"] = soft_clock_network_synced();
  doc["clock24h"] = soft_clock_use_24h();
  doc["clockAutoSync"] = soft_clock_auto_sync();

  JsonObject oled = doc["display"].to<JsonObject>();
  oled["present"] = ui_present();
  oled["blankMode"] = settings.oledBlankMode;
  oled["blankAfterSeconds"] = settings.oledBlankAfterS;
  oled["blankMinSeconds"] = UI_BLANK_AFTER_S_MIN;
  oled["blankMaxSeconds"] = UI_BLANK_AFTER_S_MAX;
  oled["blanked"] = ui_blanked();
  // The live countdowns, so a panel that will not blank can be diagnosed from
  // the page rather than guessed at: whichever number is not climbing is the
  // one being held open, and by what.
  oled["idleSeconds"] = ui_idle_ms() / 1000;
  oled["untouchedSeconds"] = ui_untouched_ms() / 1000;
  // What the analyser is actually hearing, which is what both timers are
  // decided from. Silence reads as the floor; anything playing reads well above
  // it, and a number that will not fall is the answer to "why is it not idle".
  oled["audioPeakDb"] = serialized(String(audio_probe_peak_db(), 1));
  oled["audioHeard"] = audio_probe_last_active() != 0;
  oled["dfBusy"] = df_player_active();

  JsonObject df = doc["dfplayer"].to<JsonObject>();
  df["source"] = settings.dfSource;
  df["volume"] = settings.dfVolume;
  df["volumeMax"] = DF_VOLUME_MAX;
  df["eq"] = settings.dfEq;
  df["loop"] = settings.dfLoop;
  df["loopFolder"] = settings.dfLoopFolder;
  df["autoplay"] = settings.dfAutoplay;

  JsonObject bat = doc["battery"].to<JsonObject>();
  bat["enabled"] = settings.batteryEnabled;
  bat["divider"] = serialized(String(settings.batteryDivider, 3));
  bat["calibration"] = serialized(String(settings.batteryCalibration, 4));
  bat["cells"] = settings.batteryCells;
  bat["full"] = serialized(String(settings.batteryFull, 2));
  bat["empty"] = serialized(String(settings.batteryEmpty, 2));
  bat["low"] = settings.batteryLow;
  bat["critical"] = settings.batteryCritical;
  // Whether there is any hardware behind the form at all, so the page can say
  // "not wired on this board" instead of offering a divider ratio for a pin
  // that does not exist.
  bat["sensePin"] = PIN_BATTERY_SENSE;
  bat["chargePins"] = PIN_BATTERY_CHARGING >= 0 || PIN_BATTERY_FULL >= 0;

  JsonObject led = doc["leds"].to<JsonObject>();
  led["wired"] = LEDS_ENABLED && PIN_LEDS >= 0;
  led["present"] = leds_present();
  led["pin"] = PIN_LEDS;
  led["count"] = LED_COUNT;
  led["enabled"] = settings.leds.enabled;
  led["effect"] = settings.leds.effect;
  led["brightness"] = settings.leds.brightness;
  led["speed"] = settings.leds.speed;
  led["reactivity"] = settings.leds.reactivity;
  led["color"] = colorText(settings.leds.color);
  led["color2"] = colorText(settings.leds.color2);
  led["hearingAudio"] = leds_hearing_audio();
  led["idleOff"] = settings.leds.idleOff;
  led["idleAfterSeconds"] = settings.leds.idleAfterS;
  led["idleMinSeconds"] = LED_IDLE_AFTER_S_MIN;
  led["idleMaxSeconds"] = LED_IDLE_AFTER_S_MAX;
  led["resting"] = leds_resting();
  led["idleSeconds"] = leds_idle_ms() / 1000;
  // Whether there is anything for the reactive effects to react to in this
  // mode at all. DFPlayer audio never passes through this chip, so the music
  // sync has nothing to work with and the page should say so rather than
  // leaving the owner to wonder why the ring is idling.
  led["audioPath"] = !radio_mode_has_dfplayer(radioMode);
  // The effect table comes from the firmware rather than being repeated in the
  // dashboard, so adding an effect stays a one-file change in leds.cpp.
  JsonArray effects = led["effects"].to<JsonArray>();
  for (uint8_t i = 0; i < LED_FX_COUNT; ++i) {
    JsonObject entry = effects.add<JsonObject>();
    entry["name"] = leds_effect_name(i);
    entry["hint"] = leds_effect_hint(i);
  }

  sendJson(doc);
}

void handleSettingsSave() {
  if (!requireAuth()) return;
  JsonDocument body;
  if (!readBody(body)) return;
  if (!body["hostname"].isNull()) settings.hostname = cleanHostname(body["hostname"].as<String>());
  if (!body["deviceName"].isNull()) settings.deviceName = cleanDeviceName(body["deviceName"].as<String>(), APP_NAME);
  if (!body["apAlways"].isNull()) settings.apAlways = body["apAlways"].as<bool>();
  if (!body["apPassword"].isNull()) {
    const String password = body["apPassword"].as<String>();
    if (password.length() < 8) {
      sendError(400, "Setup AP password must be at least 8 characters");
      return;
    }
    settings.apPassword = password;
  }
  if (!body["adminPassword"].isNull()) {
    const String password = body["adminPassword"].as<String>();
    if (password.length() < 6) {
      sendError(400, "Admin password must be at least 6 characters");
      return;
    }
    settings.adminPassword = password;
  }
  if (!body["githubRepo"].isNull()) settings.githubRepo = body["githubRepo"].as<String>();
  if (!body["githubAsset"].isNull()) settings.githubAsset = body["githubAsset"].as<String>();
  if (!body["githubToken"].isNull()) {
    const String token = body["githubToken"].as<String>();
    if (token.length()) settings.githubToken = token;
  }
  if (body["clearGithubToken"] | false) settings.githubToken = "";

  // DFPlayer start-up defaults. Stored only; they are sent to the module at the
  // next boot, because changing them mid-session would move the volume under
  // somebody who is listening. The live controls on the Media page are the way
  // to change what is playing now.
  if (!body["dfSource"].isNull()) {
    const int want = body["dfSource"].as<int>();
    if (want == DF_SRC_USB || want == DF_SRC_SD || want == DF_SRC_FLASH ||
        want == DF_SRC_AUX) {
      settings.dfSource = (uint8_t)want;
    }
  }
  if (!body["dfVolume"].isNull()) {
    settings.dfVolume =
        (uint8_t)constrain(body["dfVolume"].as<int>(), 0, (int)DF_VOLUME_MAX);
  }
  if (!body["dfEq"].isNull()) {
    settings.dfEq = (uint8_t)constrain(body["dfEq"].as<int>(), 0, 5);
  }
  if (!body["dfLoop"].isNull()) {
    settings.dfLoop = (uint8_t)constrain(body["dfLoop"].as<int>(), 0,
                                        (int)DF_LOOP_RANDOM);
  }
  if (!body["dfLoopFolder"].isNull()) {
    settings.dfLoopFolder =
        (uint8_t)constrain(body["dfLoopFolder"].as<int>(), 1, 99);
  }
  if (!body["dfAutoplay"].isNull()) settings.dfAutoplay = body["dfAutoplay"].as<bool>();

  // Panel blanking, applied immediately rather than at the next boot: the whole
  // card is about what the panel does while you are looking at it.
  bool blankChanged = false;
  if (!body["oledBlankMode"].isNull()) {
    settings.oledBlankMode = (uint8_t)constrain(body["oledBlankMode"].as<int>(),
                                                0, (int)UI_BLANK_ALWAYS);
    blankChanged = true;
  }
  if (!body["oledBlankAfterSeconds"].isNull()) {
    settings.oledBlankAfterS =
        (uint16_t)constrain(body["oledBlankAfterSeconds"].as<int>(),
                            (int)UI_BLANK_AFTER_S_MIN, (int)UI_BLANK_AFTER_S_MAX);
    blankChanged = true;
  }
  if (blankChanged) {
    ui_set_blank((UiBlankMode)settings.oledBlankMode, settings.oledBlankAfterS);
  }

  // The battery pack. Applied immediately rather than at the next boot: these
  // describe the hardware, and a wrong divider showing 8.4 V should be fixable
  // without a restart.
  bool batteryChanged = false;
  if (!body["batteryEnabled"].isNull()) {
    settings.batteryEnabled = body["batteryEnabled"].as<bool>();
    batteryChanged = true;
  }
  if (!body["batteryDivider"].isNull()) {
    settings.batteryDivider = body["batteryDivider"].as<float>();
    batteryChanged = true;
  }
  if (!body["batteryCalibration"].isNull()) {
    settings.batteryCalibration = body["batteryCalibration"].as<float>();
    batteryChanged = true;
  }
  if (!body["batteryCells"].isNull()) {
    settings.batteryCells = (uint8_t)constrain(body["batteryCells"].as<int>(), 1, 4);
    batteryChanged = true;
  }
  if (!body["batteryFull"].isNull()) {
    settings.batteryFull = body["batteryFull"].as<float>();
    batteryChanged = true;
  }
  if (!body["batteryEmpty"].isNull()) {
    settings.batteryEmpty = body["batteryEmpty"].as<float>();
    batteryChanged = true;
  }
  if (!body["batteryLow"].isNull()) {
    settings.batteryLow = (uint8_t)constrain(body["batteryLow"].as<int>(), 1, 90);
    batteryChanged = true;
  }
  if (!body["batteryCritical"].isNull()) {
    settings.batteryCritical =
        (uint8_t)constrain(body["batteryCritical"].as<int>(), 1, 50);
    batteryChanged = true;
  }

  saveSettings();
  if (batteryChanged) {
    applyBatterySettings();
    // The gauge clamps what it cannot use, so read back what it settled on
    // rather than storing a value the dashboard would then show as accepted.
    BatteryStatus b;
    battery_snapshot(&b);
    settings.batteryDivider = b.divider;
    settings.batteryCalibration = b.calibration;
    settings.batteryFull = b.fullVolts;
    settings.batteryEmpty = b.emptyVolts;
    settings.batteryCells = b.cells;
    settings.batteryLow = b.lowPercent;
    settings.batteryCritical = b.criticalPercent;
    saveSettings();
  }
  JsonDocument reply;
  reply["ok"] = true;
  reply["restartRequired"] = true;
  sendJson(reply);
}

void handleDisplay() {
  if (!requireAuth()) return;
  JsonDocument body;
  if (!readBody(body)) return;
  const String action = body["action"] | "";
  bool handled = false;
  if (action == "next") handled = ui_command("next");
  else if (action == "auto") handled = ui_command("auto");
  else if (action == "screen") {
    char command[20];
    snprintf(command, sizeof(command), "screen %d", constrain(body["value"] | 0, 0, 6));
    handled = ui_command(command);
  } else if (action == "brightness") {
    char command[20];
    snprintf(command, sizeof(command), "bright %d", constrain(body["value"] | 0, 0, 255));
    handled = ui_command(command);
  }
  ui_wake();
  if (!handled) {
    sendError(400, "Unknown or unavailable display action");
    return;
  }
  JsonDocument reply;
  reply["ok"] = true;
  sendJson(reply);
}

void handleClock() {
  if (!requireAuth()) return;
  JsonDocument body;
  if (!readBody(body)) return;

  /*
   * Display format and network sync ride on this endpoint and apply on their
   * own, without a time in the body -- so the Clock card behaves like the OLED
   * card next to it: change it, see it, no Save button. A full sync sends them
   * alongside the time and both paths land here.
   */
  if (!body["use24h"].isNull()) soft_clock_set_use_24h(body["use24h"].as<bool>());
  if (!body["autoSync"].isNull()) {
    const bool wanted = body["autoSync"].as<bool>();
    soft_clock_set_auto_sync(wanted);
    // Switching it back on should not wait for the next reconnect to take
    // effect. soft_clock_network_begin() is idempotent and now honours the
    // flag itself, so this is safe either way.
    if (wanted && WiFi.status() == WL_CONNECTED) soft_clock_network_begin();
  }

  const int year = body["year"] | 0;
  const int month = body["month"] | 0;
  const int day = body["day"] | 0;
  const int hour = body["hour"] | -1;
  const int minute = body["minute"] | -1;
  const int second = body["second"] | -1;
  // A preferences-only request carries no date at all, and setting the clock is
  // not what it asked for.
  if (body["year"].isNull()) {
    JsonDocument reply;
    reply["ok"] = true;
    sendJson(reply);
    return;
  }

  if (year < 2024 || year > 2099 || month < 1 || month > 12 || day < 1 ||
      day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
      second < 0 || second > 59) {
    sendError(400, "Invalid browser time");
    return;
  }

  // The browser sends its UTC offset along with the time. Storing it is what
  // keeps the automatic network sync honest: SNTP hands over UTC, and without
  // an offset the speaker would helpfully correct the time the owner just set
  // to something an hour or eight wrong. Sent as minutes east of UTC; the
  // dashboard has already flipped the sign of getTimezoneOffset().
  if (body["offsetMinutes"].is<int32_t>()) {
    soft_clock_set_utc_offset_min(body["offsetMinutes"].as<int32_t>());
  }
  struct tm local = {};
  local.tm_year = year - 1900;
  local.tm_mon = month - 1;
  local.tm_mday = day;
  local.tm_hour = hour;
  local.tm_min = minute;
  local.tm_sec = second;
  local.tm_isdst = -1;
  soft_clock_set(local, CLOCK_SRC_SERIAL);
  ui_show_system_status(UI_STATUS_SUCCESS, "Clock synchronized", "Browser local time",
                        -1, 3000);
  JsonDocument reply;
  reply["ok"] = true;
  sendJson(reply);
}

void handleUpdateAction(bool install) {
  if (!requireAuth()) return;
  if (!startGithubJob(install)) {
    sendError(409, updateSnapshot().message);
    return;
  }
  JsonDocument reply;
  reply["ok"] = true;
  reply["started"] = install ? "install" : "check";
  sendJson(reply, 202);
}

void handleUploadComplete() {
  if (!requireAuth()) return;
  const bool ok = browserUploadOk;
  JsonDocument reply;
  reply["ok"] = ok;
  reply[ok ? "message" : "error"] =
      ok ? "Firmware installed; restarting" : browserUploadError;
  sendJson(reply, ok ? 200 : 500);
  browserUploadAccepted = false;
  browserUploadOk = false;
  if (ok) {
    updateSet("success", "Upload installed; restarting", false);
    scheduleReboot(1200);
  }
}

void handleUploadChunk() {
  if (!authenticated()) return;
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    browserUploadAccepted = false;
    browserUploadOk = false;
    browserUploadTotal = (size_t)strtoul(
        server.header("X-Firmware-Size").c_str(), nullptr, 10);
    strlcpy(browserUploadError, "Upload could not be started", sizeof(browserUploadError));
    const UpdateState u = updateSnapshot();
    if (u.busy) {
      strlcpy(browserUploadError, "Another update is already in progress",
              sizeof(browserUploadError));
      return;
    }
    if (sink && sink->is_connected()) sink->pause();
    updateSet("uploading", "Receiving firmware from browser", true);
    updateProgress(0, browserUploadTotal);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      strlcpy(browserUploadError, Update.errorString(), sizeof(browserUploadError));
      updateSet("error", Update.errorString(), false);
    } else browserUploadAccepted = true;
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (browserUploadAccepted && !Update.hasError()) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        strlcpy(browserUploadError, Update.errorString(), sizeof(browserUploadError));
        updateSet("error", Update.errorString(), false);
      } else {
        updateProgress(Update.progress(), browserUploadTotal);
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (browserUploadAccepted && Update.end(true)) {
      updateProgress(Update.progress(), Update.size());
      updateSet("success", "Upload installed; restarting", false);
      browserUploadOk = true;
    } else {
      if (browserUploadAccepted)
        strlcpy(browserUploadError, Update.errorString(), sizeof(browserUploadError));
      updateSet("error", browserUploadError, false);
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (browserUploadAccepted) Update.abort();
    strlcpy(browserUploadError, "Upload cancelled", sizeof(browserUploadError));
    updateSet("error", "Upload cancelled", false);
  }
}

// Wipes settings and Bluetooth bonds. Shared by the dashboard action and the
// BOOT-button hold, so the two can never drift apart. The caller reboots.
void factoryReset() {
  prefs.clear();

  // Bonds belong to Bluedroid, which is only running in Bluetooth mode. Ask it
  // nicely when it is there...
  if (btActive) {
    const int count = esp_bt_gap_get_bond_device_num();
    if (count > 0) {
      esp_bd_addr_t *list =
          (esp_bd_addr_t *)malloc((size_t)count * sizeof(esp_bd_addr_t));
      if (list) {
        int actual = count;
        if (esp_bt_gap_get_bond_device_list(&actual, list) == ESP_OK) {
          for (int i = 0; i < actual; ++i) esp_bt_gap_remove_bond_device(list[i]);
        }
        free(list);
      }
    }
  }

  // ...and either way wipe the whole NVS partition, which is where Bluedroid
  // keeps them. A reset done from Wi-Fi mode would otherwise leave every
  // pairing behind, and "factory reset" has to mean it whichever mode you are
  // standing in. The caller reboots immediately; the partition reformats itself
  // on the next boot.
  prefs.end();
  const esp_err_t err = nvs_flash_erase();
  Serial.printf("[web] factory reset: settings cleared, nvs erase %s\n",
                esp_err_to_name(err));
}

void handleSystem() {
  if (!requireAuth()) return;
  JsonDocument body;
  if (!readBody(body)) return;
  const String action = body["action"] | "";
  if (action == "reboot") {
    JsonDocument reply;
    reply["ok"] = true;
    reply["message"] = "Restarting";
    sendJson(reply);
    ui_show_system_status(UI_STATUS_RESTART, "Restarting", "Please wait", -1, 0);
    scheduleReboot(700);
  } else if (action == "mode" || action == "startBluetooth") {
    // "startBluetooth" is what older dashboards send; it means the same thing
    // as {"action":"mode","mode":1} and is kept so a cached page still works.
    const int wanted = action == "startBluetooth"
                           ? (int)RADIO_MODE_BLUETOOTH
                           : (body["mode"] | (int)RADIO_MODE_MANAGEMENT);
    if (wanted < 0 || wanted >= (int)RADIO_MODE_COUNT) {
      sendError(400, "Unknown radio mode");
      return;
    }
    const RadioMode target = (RadioMode)wanted;
    if (target == management_radio_mode()) {
      sendError(409, String("Already in ") + management_mode_name(target) +
                         " mode");
      return;
    }
    // Combo mode never raises the setup access point, so sending the speaker
    // there without a saved network strands it: no dashboard, and no way to
    // give it one except the BOOT button. Refuse while it would.
    if (target == RADIO_MODE_COMBO && !settings.ssid.length()) {
      sendError(409, "Save a Wi-Fi network first: Wi-Fi + BT joins your "
                     "network and does not open the setup hotspot.");
      return;
    }
    // Every switch reboots, and the two that leave Wi-Fi behind take this
    // dashboard with them. Answer while there is still a connection to answer
    // on, and say which of the two just happened.
    JsonDocument reply;
    reply["ok"] = true;
    reply["mode"] = (int)target;
    reply["message"] =
        radio_mode_has_wifi(target)
            ? String("Switching to ") + management_mode_name(target) +
                  " mode; the speaker restarts and this dashboard comes back "
                  "in a few seconds."
            : String("Switching to ") + management_mode_name(target) +
                  " mode; Wi-Fi and this dashboard go away. Hold BOOT on the "
                  "speaker to cycle back.";
    sendJson(reply);
    server.client().flush();
    delay(150);
    management_switch_mode(target);  // does not return
  } else if (action == "factoryReset") {
    factoryReset();
    JsonDocument reply;
    reply["ok"] = true;
    reply["message"] = "Settings and Bluetooth bonds cleared; restarting";
    sendJson(reply);
    ui_show_system_status(UI_STATUS_RESTART, "Factory reset", "Clearing settings", -1,
                          0);
    scheduleReboot(900);
  } else {
    sendError(400, "Unknown system action");
  }
}

// http://<hostname>.local/ so the dashboard can be reached without hunting for
// the DHCP lease. Started once the station has an address; harmless to call
// again, MDNS.end() makes the restart idempotent.
void startResponder() {
  MDNS.end();
  if (!MDNS.begin(settings.hostname.c_str())) {
    Serial.println("[web] mDNS responder failed to start");
    return;
  }
  MDNS.addService("http", "tcp", 80);
  MDNS.addServiceTxt("http", "tcp", "path", "/");
  MDNS.addServiceTxt("http", "tcp", "version", FW_VERSION);
}

void configureRoutes() {
  const char *requestHeaders[] = {"X-Firmware-Size"};
  server.collectHeaders(requestHeaders, 1);
  server.on("/", HTTP_GET, [] {
    server.sendHeader("Cache-Control", "no-cache");
    server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "text/html; charset=utf-8", (PGM_P)DASHBOARD_GZIP,
                  DASHBOARD_GZIP_LEN);
  });
  server.on("/favicon.svg", HTTP_GET, [] {
    server.sendHeader("Cache-Control", "public, max-age=86400");
    server.send_P(200, "image/svg+xml", DASHBOARD_ICON);
  });
  server.on("/api/auth", HTTP_GET, handleAuth);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/media", HTTP_POST, handleMedia);
  server.on("/api/devices", HTTP_GET, handleDevices);
  server.on("/api/devices", HTTP_POST, handleDeviceAction);
  server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  server.on("/api/wifi", HTTP_POST, handleWifiSave);
  server.on("/api/settings", HTTP_GET, handleSettingsGet);
  server.on("/api/settings", HTTP_POST, handleSettingsSave);
  server.on("/api/dfplayer", HTTP_POST, handleDfPlayer);
  server.on("/api/dfplayer/library", HTTP_GET, handleDfLibrary);
  server.on("/api/battery", HTTP_POST, handleBattery);
  server.on("/api/display", HTTP_POST, handleDisplay);
  server.on("/api/leds", HTTP_POST, handleLeds);
  server.on("/api/clock", HTTP_POST, handleClock);
  server.on("/api/update/check", HTTP_POST, [] { handleUpdateAction(false); });
  server.on("/api/update/install", HTTP_POST, [] { handleUpdateAction(true); });
  server.on("/api/update/upload", HTTP_POST, handleUploadComplete, handleUploadChunk);
  server.on("/api/system", HTTP_POST, handleSystem);
  server.onNotFound([] {
    if (server.uri().startsWith("/api/")) sendError(404, "API endpoint not found");
    else server.sendHeader("Location", "/", true), server.send(302, "text/plain", "");
  });
  server.begin();
}

}  // namespace

const char *management_device_name(const char *fallback) {
  if (!stableDeviceName.length()) loadSettings(fallback);
  return stableDeviceName.c_str();
}

RadioMode management_radio_mode() { return radioMode; }

RadioMode management_next_mode() {
  return (RadioMode)((radioMode + 1) % RADIO_MODE_COUNT);
}

// Short on purpose: these land in a 24-character OLED title ("<name> mode") and
// in one-line serial logs. The dashboard builds its own longer labels from the
// mode id.
const char *management_mode_name(RadioMode mode) {
  switch (mode) {
    case RADIO_MODE_BLUETOOTH: return "Bluetooth";
    case RADIO_MODE_COMBO: return "Wi-Fi + BT";
    case RADIO_MODE_NET: return "Wi-Fi + BLE";
    case RADIO_MODE_DFPLAYER: return "DFPlayer";
    default: return "Wi-Fi";
  }
}

void management_switch_mode(RadioMode mode) {
  // Through a restart rather than by tearing one stack down and building the
  // other up in place. Both stacks own controller state, DMA and tasks, and
  // ESP32-A2DP's end() also forgets the last paired device; a reboot costs a
  // second and guarantees each mode starts from a clean radio.
  if (!stableDeviceName.length()) loadSettings(APP_NAME);
  if (mode >= RADIO_MODE_COUNT) mode = RADIO_MODE_MANAGEMENT;
  prefs.putUChar("radioMode", (uint8_t)mode);
  // Asking for a mode is a fresh start: it gets its full allowance of attempts
  // even if the last thing in that slot fell over.
  prefs.putUChar("bootFail", 0);
  Serial.printf("[mode] switching to %s mode\n", management_mode_name(mode));
  char title[24];
  snprintf(title, sizeof(title), "%s mode", management_mode_name(mode));
  ui_show_system_status(UI_STATUS_RESTART, title, "Restarting", -1, 0);
  scheduleReboot(700);  // long enough for the panel to land on the message
  while (true) delay(50);
}

bool management_ap_running() { return apRunning; }

void management_provision_wifi(const char *ssid, const char *password) {
  if (ssid == nullptr || *ssid == 0) return;
  if (!stableDeviceName.length()) loadSettings(APP_NAME);
  settings.ssid = ssid;
  settings.wifiPassword = password != nullptr ? password : "";
  saveSettings();
  Serial.printf("[web] network saved over BLE: %s; restarting\n", ssid);
  ui_show_system_status(UI_STATUS_RESTART, "Wi-Fi saved", "Restarting speaker",
                        -1, 0);
  scheduleReboot(900);
  while (true) delay(50);
}

void management_factory_reset() {
  factoryReset();
  ui_show_system_status(UI_STATUS_RESTART, "Factory reset",
                        "Settings cleared", -1, 0);
}

void management_set_bt_active(bool active) { btActive = active; }

void management_store_leds() {
  // Straight to flash rather than through the deferred path: this comes from a
  // typed console command, so there is exactly one of them and no slider to
  // debounce. Reading the live configuration back rather than trusting our own
  // copy keeps the two in step whichever side made the change.
  leds_get(&settings.leds);
  saveLedSettings();
}

void management_df_defaults(uint8_t *source, uint8_t *volume, uint8_t *eq,
                           uint8_t *loop, uint8_t *loopFolder, bool *autoplay) {
  if (!stableDeviceName.length()) loadSettings(APP_NAME);
  if (source) *source = settings.dfSource;
  if (volume) *volume = settings.dfVolume;
  if (eq) *eq = settings.dfEq;
  if (loop) *loop = settings.dfLoop;
  if (loopFolder) *loopFolder = settings.dfLoopFolder;
  if (autoplay) *autoplay = settings.dfAutoplay;
}

bool management_led_state(StatusLedState *out) {
  /*
   * The battery outranks everything, including Bluetooth mode where the rest of
   * this function declines to say anything. A cell about to cut out is the one
   * fact that matters more than what is playing -- and unlike the network
   * states below, it is true in every mode, so the early return for Bluetooth
   * mode comes after this rather than before it.
   *
   * `battery_critical()` is already "critical and not charging", so a speaker on
   * a charger stops flashing about it and goes back to showing the audio state.
   */
  if (battery_critical()) {
    *out = LED_BATTERY_LOW;
    return true;
  }

  if (radioMode == RADIO_MODE_BLUETOOTH) return false;

  const UpdateState u = updateSnapshot();
  if (u.busy) {
    *out = LED_UPDATING;
    return true;
  }
  // Failures are worth shouting about, but not forever.
  if (strcmp(u.phase, "error") == 0 && millis() - updatePhaseAt < 20000) {
    *out = LED_FAULT;
    return true;
  }
  // In the two modes that play audio *and* run Wi-Fi, the network only gets the
  // LED while it is still finding its feet. Once the station is home, what the
  // speaker is doing is playing, so decline here and let main.cpp show that
  // instead -- for combo that is the A2DP state, for Wi-Fi + BLE the network
  // player's. (Combo never raises an access point, so only NET needs the
  // setup-AP case; it is written once for both because the answer is the same.)
  if (radioMode == RADIO_MODE_COMBO || radioMode == RADIO_MODE_NET ||
      radioMode == RADIO_MODE_DFPLAYER) {
    if (apRunning && WiFi.status() != WL_CONNECTED) {
      *out = LED_SETUP_AP;
      return true;
    }
    if (WiFi.status() == WL_CONNECTED) return false;
    *out = LED_WIFI_CONNECTING;
    return true;
  }
  // In Wi-Fi mode the LED is entirely the network's to talk about: Bluetooth
  // has nothing to say because it is not running.
  if (apRunning && WiFi.status() != WL_CONNECTED) {
    *out = LED_SETUP_AP;
    return true;
  }
  *out = WiFi.status() == WL_CONNECTED ? LED_IDLE : LED_WIFI_CONNECTING;
  return true;
}

void management_begin(BluetoothA2DPSink &a2dp) {
  sink = &a2dp;
  if (!stableDeviceName.length()) loadSettings(APP_NAME);
  radioMode = (RadioMode)prefs.getUChar("radioMode", RADIO_MODE_MANAGEMENT);
  if (radioMode >= RADIO_MODE_COUNT) radioMode = RADIO_MODE_MANAGEMENT;

  // See the boot sentinel note above. Wi-Fi mode is the fallback, so it never
  // takes a strike -- it just clears whatever the last mode left behind.
  bootStrikes = prefs.getUChar("bootFail", 0);
  if (radioMode == RADIO_MODE_MANAGEMENT) {
    if (bootStrikes) prefs.putUChar("bootFail", 0);
  } else if (bootStrikes >= BOOT_STRIKES_MAX) {
    Serial.printf("[mode] %s mode failed to stay up %u times in a row. Falling "
                  "back to Wi-Fi mode so the dashboard is reachable; pick a "
                  "mode again from there once you know why.\n",
                  management_mode_name(radioMode), (unsigned)bootStrikes);
    ui_show_system_status(UI_STATUS_ERROR, "Mode failed", "Back to Wi-Fi", -1,
                          10000);
    radioMode = RADIO_MODE_MANAGEMENT;
    prefs.putUChar("radioMode", (uint8_t)radioMode);
    prefs.putUChar("bootFail", 0);
  } else {
    prefs.putUChar("bootFail", (uint8_t)(bootStrikes + 1));
    bootStrikePending = true;
  }

  // Combo mode is a station and a sink; it never raises the setup access point,
  // because that is the pairing the coexistence scheduler does not support. So
  // without a saved network there is nothing for it to join and no way left to
  // configure one. Demote to Wi-Fi mode -- and persist the demotion, or the
  // BOOT button would keep cycling out of a mode the speaker never entered.
  if (radioMode == RADIO_MODE_COMBO && !settings.ssid.length()) {
    radioMode = RADIO_MODE_MANAGEMENT;
    prefs.putUChar("radioMode", (uint8_t)radioMode);
    Serial.println("[mode] Wi-Fi + BT needs a saved network; starting the setup "
                   "access point in Wi-Fi mode instead.");
  }

  /*
   * The battery gauge, before any mode-specific bring-up.
   *
   * It is the one block here that belongs to every mode: it powers the speaker
   * whatever the radio is doing, and Bluetooth mode returns from this function
   * a few lines below without ever reaching the Wi-Fi code. Configured from NVS
   * and started in one go, because the stored divider and trim are what make the
   * first reading meaningful rather than a number to be corrected later.
   */
  applyBatterySettings();
  battery_begin();

  if (radioMode == RADIO_MODE_BLUETOOTH) {
    // Not one Wi-Fi call. Leaving the driver uninitialised is the point: the
    // antenna, the coexistence scheduler and ~50 KB of heap all stay with the
    // Bluetooth stack. main.cpp starts the sink.
    Serial.println("[mode] Bluetooth mode: Wi-Fi is off. Hold BOOT to switch.");
    return;
  }

  if (radioMode == RADIO_MODE_COMBO) {
    /*
     * Both radios at once, refereed by the software coexistence scheduler.
     *
     * Three things make this work rather than merely start:
     *
     *   - Station only. startAccessPoint() refuses to run in this mode. An AP
     *     beaconing on a fixed channel beside an A2DP link is the combination
     *     that fails, and it is the only one.
     *   - Bluetooth keeps its RAM. The esp_bt_mem_release() below belongs to
     *     Wi-Fi mode and must not happen here. The sink's own start() hands
     *     back the BLE half (~30 KB) instead, which is memory this chip could
     *     not have used for audio anyway -- LE Audio is 5.2 hardware.
     *   - The scheduler is left to Bluedroid. There used to be an
     *     esp_coex_preference_set(ESP_COEX_PREFER_BT) here, before either stack
     *     started. It is gone for two reasons: this build has
     *     CONFIG_BT_BLUEDROID_ESP_COEX_VSC=y, which means Bluedroid already
     *     tells the scheduler when A2DP is streaming and gets the priority
     *     right on its own; and the call is deprecated, lives in a binary blob,
     *     and was being made before anything it might touch existed. If audio
     *     ever does need help here, esp_coex_status_bit_set() is the supported
     *     way and it belongs after the sink is up, not before.
     */
    Serial.println("[mode] Wi-Fi + BT mode: station plus A2DP sink, sharing the "
                   "radio under coexistence.");

    WiFi.persistent(false);
    WiFi.setHostname(settings.hostname.c_str());
    WiFi.onEvent(onWifiEvent);
    WiFi.mode(WIFI_STA);
    // As in Wi-Fi mode: adopt the router's regulatory domain from its beacons,
    // or channels 12-13 are visible in a scan and impossible to associate with.
    esp_wifi_set_country_code("01", true);
    // Modem sleep is what leaves the coexistence scheduler windows to hand to
    // Bluetooth between beacons. It is the Arduino default, but this is the one
    // mode where switching it off would be audible, so say it out loud.
    WiFi.setSleep(WIFI_PS_MIN_MODEM);
    WiFi.begin(settings.ssid.c_str(), settings.wifiPassword.c_str());
    wifiStartedAt = millis();
    ui_show_system_status(UI_STATUS_NETWORK, "Connecting Wi-Fi",
                          settings.ssid.c_str(), -1, 0);
    Serial.printf("[web] connecting Wi-Fi: %s\n", settings.ssid.c_str());
    configureRoutes();
    Serial.println("[web] dashboard login: admin (change the default password)");
    return;
  }

  if (radioMode == RADIO_MODE_DFPLAYER) {
    /*
     * Wi-Fi gets the radio to itself, exactly as in Wi-Fi only mode, and for a
     * better reason: there is no second radio in this mode to share it with.
     * The audio source is a serial peripheral, so the setup access point is
     * allowed, the dashboard behaves identically, and the whole Bluetooth
     * controller -- both halves -- is released below, which is the largest heap
     * saving any mode makes.
     *
     * main.cpp starts the module; see service_dfplayer() there. It is not
     * started here because the driver wants to log after the serial console is
     * up and because a card that takes a second to mount should not hold up the
     * network bring-up.
     */
    Serial.println("[mode] DFPlayer mode: audio comes from the DFPlayer Mini "
                   "over serial. Both Bluetooth radios are off.");
  }

  if (radioMode == RADIO_MODE_NET) {
    // Everything below this point is shared with Wi-Fi mode: the same station
    // bring-up, the same setup access point, the same dashboard. Only two
    // things differ, and both are about who owns the Bluetooth memory.
    //
    // The release further down hands back the *whole* controller, BLE included,
    // which is right in Wi-Fi mode and fatal here -- BLE is the control channel
    // this mode is built around. So it is skipped, and ble_control_begin()
    // gives back only the Classic half instead, which nothing here uses.
    //
    // The access point is allowed. The combination this chip cannot do is a
    // SoftAP beside an A2DP sink; BLE is not that, and coexists with an access
    // point perfectly well. So unlike Wi-Fi + BT, this mode can configure
    // itself from cold -- and BLE provisioning is a second way in on top.
    Serial.println("[mode] Wi-Fi + BLE mode: audio arrives over the network, "
                   "BLE carries control. Bluetooth Classic is off.");
  } else if (radioMode == RADIO_MODE_MANAGEMENT) {
    Serial.println("[mode] Wi-Fi mode: Bluetooth is off. Hold BOOT to switch.");
  }

  /*
   * Give the Bluetooth stack's RAM back before anything asks for it.
   *
   * The controller and Bluedroid own several fixed regions of DRAM that the
   * linker reserves whether or not they are ever initialised. In Wi-Fi mode
   * they never are -- and holding on to them left about 49 KB of heap free,
   * which is not enough for a TLS handshake against the root bundle. That is
   * what "GitHub check failed: connection refused (-1)" was: not a refused
   * connection, a handshake that could not be allocated.
   *
   * Releasing is one way. The memory does not come back until a reset, which is
   * exactly how mode switching works here anyway -- management_switch_mode()
   * persists the choice and reboots -- so there is nothing to lose. Every
   * esp_bt_* call in this file is behind btActive, which stays false all the
   * way through this mode.
   */
  if (radioMode == RADIO_MODE_MANAGEMENT || radioMode == RADIO_MODE_DFPLAYER) {
    const uint32_t heapBefore = ESP.getFreeHeap();
    const esp_err_t released = esp_bt_mem_release(ESP_BT_MODE_BTDM);
    Serial.printf("[mode] bluetooth memory released: %s (heap %u -> %u)\n",
                  esp_err_to_name(released), (unsigned)heapBefore,
                  (unsigned)ESP.getFreeHeap());
  }

  WiFi.persistent(false);
  WiFi.setHostname(settings.hostname.c_str());
  WiFi.onEvent(onWifiEvent);

  // Leave Wi-Fi power save at its default. Nothing shares the antenna in this
  // mode, but modem sleep costs nothing a dashboard would notice.

  if (settings.ssid.length()) {
    // Station first and on its own, even when the AP is meant to stay up. The
    // initial connect sweeps the band, and an AP raised before that finishes is
    // dragged off channel with it -- clients see the SSID and then fail to
    // associate. management_loop() raises the AP once the station has settled
    // on a channel, or given up.
    WiFi.mode(WIFI_STA);
    // Adopt the router's regulatory domain from its beacons. Without 802.11d
    // the station is stuck on the "01" world-safe channels and a router sitting
    // on channel 12 or 13 shows up in a scan but can never be associated with.
    esp_wifi_set_country_code("01", true);
    WiFi.begin(settings.ssid.c_str(), settings.wifiPassword.c_str());
    wifiStartedAt = millis();
    ui_show_system_status(UI_STATUS_NETWORK, "Connecting Wi-Fi",
                          settings.ssid.c_str(), -1, 0);
    Serial.printf("[web] connecting Wi-Fi: %s\n", settings.ssid.c_str());
  } else {
    startAccessPoint();
  }
  configureRoutes();
  Serial.println("[web] dashboard login: admin (change the default password)");
}

void management_loop() {
  // Before the mode check: every mode has to be able to clear its own strike,
  // including the ones that return immediately below.
  if (bootStrikePending && millis() > BOOT_STABLE_MS) {
    bootStrikePending = false;
    prefs.putUChar("bootFail", 0);
    Serial.printf("[mode] %s mode is stable\n", management_mode_name(radioMode));
  }

  // Before the mode check for the same reason the strike above is: the console
  // can change the lighting in a mode that has no dashboard at all.
  if (ledsDirty && millis() - ledsDirtyAt >= LEDS_PERSIST_QUIET_MS) {
    saveLedSettings();
  }

  if (!radio_mode_has_wifi(radioMode)) return;

  server.handleClient();
  if (WiFi.status() == WL_CONNECTED && !announcedIp) {
    announcedIp = true;
    // parkStation() switched the core's own retry off to keep the access point
    // on one channel. The station is home now, so hand normal reconnection back.
    WiFi.setAutoReconnect(true);
    staRetryStartedAt = 0;
    if (!settings.apAlways && !apClients) stopAccessPoint("station connected");
    const String ip = WiFi.localIP().toString();
    startResponder();
    stationUpAt = millis() | 1;  // never 0: that is the "not yet" sentinel
    // There is internet now, so there is no reason for the clock to be a guess.
    // SNTP keeps re-syncing on its own from here; soft_clock_tick() adopts each
    // answer. This is also what makes the updater's TLS work on a board that
    // has been unplugged for a month -- certificate validity is checked against
    // this clock, and a chain that has not started yet fails the handshake.
    soft_clock_network_begin();
    status_led_blip(2);
    ui_show_system_status(UI_STATUS_NETWORK, "Dashboard ready", ip.c_str(), -1,
                          6000);
    Serial.printf("[web] dashboard: http://%s/ or http://%s.local/\n",
                  ip.c_str(), settings.hostname.c_str());
  } else if (WiFi.status() != WL_CONNECTED && announcedIp) {
    announcedIp = false;
    soft_clock_network_end();
    // Restart the grace period. Without this the stale boot timestamp makes any
    // momentary drop raise the access point on the very next loop, which parks
    // the station and stops it reconnecting on its own.
    wifiStartedAt = millis();
    ui_show_system_status(UI_STATUS_NETWORK, "Wi-Fi disconnected",
                          "Reconnecting", -1, 4000);
  }
  // Combo mode has no access point to fall back on, so a station that never
  // arrives leaves a working Bluetooth speaker with no dashboard on it. The
  // core keeps retrying by itself; say once where the way out is, because from
  // the outside this looks like the dashboard has simply vanished.
  if (radioMode == RADIO_MODE_COMBO) {
    if (WiFi.status() == WL_CONNECTED) {
      comboOfflineWarned = false;
    } else if (!comboOfflineWarned &&
               millis() - wifiStartedAt > FIRST_CONNECT_GRACE_MS) {
      comboOfflineWarned = true;
      Serial.printf("[mode] no Wi-Fi yet (%s). Bluetooth audio still works, the "
                    "dashboard does not. Hold BOOT to cycle round to Wi-Fi mode "
                    "for the setup access point.\n",
                    settings.ssid.c_str());
      ui_show_system_status(UI_STATUS_ERROR, "No Wi-Fi", "Bluetooth only", -1,
                            5000);
    }
    serviceStartupUpdateCheck();
    return;
  }
  // Raise the setup AP once the station has settled -- associated, if the AP is
  // configured to stay up alongside it, or clearly failed. Never mid-scan.
  if (settings.ssid.length() && !apRunning &&
      ((settings.apAlways && WiFi.status() == WL_CONNECTED) ||
       (WiFi.status() != WL_CONNECTED &&
        millis() - wifiStartedAt > FIRST_CONNECT_GRACE_MS))) {
    startAccessPoint();
  }
  serviceStationRetry();
  serviceStartupUpdateCheck();
}

#endif
