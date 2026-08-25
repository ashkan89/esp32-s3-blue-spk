#include "app_config.h"
#include "management.h"

#if MANAGEMENT_ENABLED

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_gap_bt_api.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

#include "BluetoothA2DPSink.h"
#include "player_state.h"
#include "soft_clock.h"
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
uint32_t rebootAt;
uint8_t mutedFrom = 80;
bool browserUploadAccepted;
bool browserUploadOk;
size_t browserUploadTotal;
char browserUploadError[96];
portMUX_TYPE updateMux = portMUX_INITIALIZER_UNLOCKED;
UpdateState updateState = {"idle", "Ready", "", "", "", "", 0, 0, false, false};

// DigiCert Global Root G2, valid until 2038. GitHub's API and release asset
// endpoints chain to this public root. Keeping certificate verification on is
// important here: this connection writes executable code to flash.
static const char GITHUB_ROOT_CA[] PROGMEM = R"CERT(-----BEGIN CERTIFICATE-----
MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh
MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3
d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH
MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT
MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j
b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG
9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI
2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx
1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ
q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz
tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ
vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP
BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV
5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY
1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4
NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG
Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91
8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe
pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl
MrY=
-----END CERTIFICATE-----)CERT";

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
  stableDeviceName = settings.deviceName;
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
}

bool authenticated() {
  return server.authenticate("admin", settings.adminPassword.c_str());
}

bool requireAuth() {
  if (authenticated()) return true;
  server.sendHeader("Cache-Control", "no-store");
  server.requestAuthentication(BASIC_AUTH, APP_NAME, "Sign in required");
  return false;
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
  return lower.endsWith(".bin") && lower.indexOf("bootloader") < 0 &&
         lower.indexOf("partition") < 0 && lower.indexOf("littlefs") < 0 &&
         lower.indexOf("spiffs") < 0;
}

String normalizedVersion(String value) {
  value.trim();
  if (value.startsWith("v") || value.startsWith("V")) value.remove(0, 1);
  return value;
}

struct GithubJob {
  bool install;
  String repo;
  String pattern;
  String token;
};

void githubTask(void *raw) {
  std::unique_ptr<GithubJob> job((GithubJob *)raw);
  updateSet("checking", "Contacting GitHub releases", true);
  updateProgress(0, 0);

  NetworkClientSecure tls;
  tls.setCACert(GITHUB_ROOT_CA);
  HTTPClient http;
  const String api = "https://api.github.com/repos/" + job->repo + "/releases/latest";
  if (!http.begin(tls, api)) {
    updateSet("error", "Could not initialize HTTPS", false);
    vTaskDelete(nullptr);
    return;
  }
  http.setUserAgent(String(APP_NAME) + "/" + FW_VERSION);
  http.setTimeout(15000);
  if (job->token.length()) http.addHeader("Authorization", "Bearer " + job->token);
  http.addHeader("Accept", "application/vnd.github+json");
  http.addHeader("X-GitHub-Api-Version", "2022-11-28");
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    char message[96];
    snprintf(message, sizeof(message), "GitHub returned HTTP %d", code);
    updateSet("error", message, false);
    http.end();
    vTaskDelete(nullptr);
    return;
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
    updateSet("error", "GitHub returned invalid release metadata", false);
    http.end();
    vTaskDelete(nullptr);
    return;
  }

  const String tag = release["tag_name"] | "";
  const String releaseName = release["name"] | tag;
  const String releaseUrl = release["html_url"] | "";
  String assetName;
  String assetUrl;
  uint32_t assetSize = 0;
  for (JsonObject asset : release["assets"].as<JsonArray>()) {
    const String candidate = asset["name"] | "";
    if (isFirmwareAsset(candidate) && globMatch(job->pattern.c_str(), candidate.c_str())) {
      assetName = candidate;
      assetUrl = asset["browser_download_url"] | "";
      assetSize = asset["size"] | 0;
      break;
    }
  }
  http.end();

  if (!assetUrl.length()) {
    updateSet("error", "No release firmware matched the asset pattern", false);
    vTaskDelete(nullptr);
    return;
  }

  portENTER_CRITICAL(&updateMux);
  strlcpy(updateState.tag, tag.c_str(), sizeof(updateState.tag));
  strlcpy(updateState.releaseName, releaseName.c_str(), sizeof(updateState.releaseName));
  strlcpy(updateState.assetName, assetName.c_str(), sizeof(updateState.assetName));
  strlcpy(updateState.releaseUrl, releaseUrl.c_str(), sizeof(updateState.releaseUrl));
  updateState.total = assetSize;
  updateState.available = normalizedVersion(tag) != normalizedVersion(FW_VERSION);
  portEXIT_CRITICAL(&updateMux);

  if (!job->install) {
    updateSet("ready", updateState.available ? "A firmware release is available"
                                              : "Firmware is up to date", false);
    vTaskDelete(nullptr);
    return;
  }

  updateSet("downloading", "Downloading and verifying firmware", true);
  if (sink && sink->is_connected()) sink->pause();
  HTTPClient download;
  download.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  download.setTimeout(20000);
  if (!download.begin(tls, assetUrl)) {
    updateSet("error", "Could not initialize firmware download", false);
    vTaskDelete(nullptr);
    return;
  }
  download.setUserAgent(String(APP_NAME) + "/" + FW_VERSION);
  if (job->token.length()) download.addHeader("Authorization", "Bearer " + job->token);
  const int downloadCode = download.GET();
  if (downloadCode != HTTP_CODE_OK) {
    char message[96];
    snprintf(message, sizeof(message), "Firmware download returned HTTP %d", downloadCode);
    updateSet("error", message, false);
    download.end();
    vTaskDelete(nullptr);
    return;
  }
  const int contentLength = download.getSize();
  const size_t expected = contentLength > 0 ? (size_t)contentLength : UPDATE_SIZE_UNKNOWN;
  if (!Update.begin(expected, U_FLASH)) {
    updateSet("error", Update.errorString(), false);
    download.end();
    vTaskDelete(nullptr);
    return;
  }
  Update.onProgress(updateProgress);
  const size_t written = Update.writeStream(download.getStream());
  const bool complete = Update.end(true);
  download.end();
  if (!complete || (contentLength > 0 && written != (size_t)contentLength)) {
    updateSet("error", Update.errorString(), false);
    vTaskDelete(nullptr);
    return;
  }
  updateSet("success", "Update installed; restarting", false);
  rebootAt = millis() + 1800;
  vTaskDelete(nullptr);
}

bool startGithubJob(bool install) {
  const UpdateState u = updateSnapshot();
  if (u.busy) return false;
  if (WiFi.status() != WL_CONNECTED) {
    updateSet("error", "Internet connection required", false);
    return false;
  }
  if (settings.githubRepo.indexOf('/') <= 0 || settings.githubAsset.length() == 0) {
    updateSet("error", "Configure owner/repository and an asset pattern first", false);
    return false;
  }
  GithubJob *job = new GithubJob{install, settings.githubRepo, settings.githubAsset,
                                 settings.githubToken};
  if (xTaskCreatePinnedToCore(githubTask, "github_ota", 14336, job, 1, nullptr, 0) != pdPASS) {
    delete job;
    updateSet("error", "Could not start update task", false);
    return false;
  }
  return true;
}

void startAccessPoint() {
  if (apRunning) return;
  apName = defaultApName();
  apRunning = WiFi.softAP(apName.c_str(), settings.apPassword.c_str());
  if (apRunning) {
    const String oled = apName + " / " + settings.apPassword + " / " +
                        WiFi.softAPIP().toString();
    ui_show_system_status(UI_STATUS_NETWORK, "Setup Wi-Fi", oled.c_str(), -1,
                          12000);
    Serial.printf("[web] setup AP: %s / %s -> http://%s\n", apName.c_str(),
                  settings.apPassword.c_str(), WiFi.softAPIP().toString().c_str());
  }
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

  JsonObject bt = doc["bluetooth"].to<JsonObject>();
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

void handleMedia() {
  if (!requireAuth()) return;
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
  rebootAt = millis() + 900;
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
  saveSettings();
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
  const int year = body["year"] | 0;
  const int month = body["month"] | 0;
  const int day = body["day"] | 0;
  const int hour = body["hour"] | -1;
  const int minute = body["minute"] | -1;
  const int second = body["second"] | -1;
  if (year < 2024 || year > 2099 || month < 1 || month > 12 || day < 1 ||
      day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
      second < 0 || second > 59) {
    sendError(400, "Invalid browser time");
    return;
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
    rebootAt = millis() + 1200;
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
    rebootAt = millis() + 700;
  } else if (action == "factoryReset") {
    prefs.clear();
    const int count = esp_bt_gap_get_bond_device_num();
    if (count > 0) {
      esp_bd_addr_t *list = (esp_bd_addr_t *)malloc((size_t)count * sizeof(esp_bd_addr_t));
      if (list) {
        int actual = count;
        if (esp_bt_gap_get_bond_device_list(&actual, list) == ESP_OK) {
          for (int i = 0; i < actual; ++i) esp_bt_gap_remove_bond_device(list[i]);
        }
        free(list);
      }
    }
    JsonDocument reply;
    reply["ok"] = true;
    reply["message"] = "Settings and Bluetooth bonds cleared; restarting";
    sendJson(reply);
    ui_show_system_status(UI_STATUS_RESTART, "Factory reset", "Clearing settings", -1,
                          0);
    rebootAt = millis() + 900;
  } else {
    sendError(400, "Unknown system action");
  }
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
  server.on("/api/display", HTTP_POST, handleDisplay);
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

void management_begin(BluetoothA2DPSink &a2dp) {
  sink = &a2dp;
  if (!stableDeviceName.length()) loadSettings(APP_NAME);
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setHostname(settings.hostname.c_str());

  if (settings.ssid.length()) {
    WiFi.mode(settings.apAlways ? WIFI_AP_STA : WIFI_STA);
    if (settings.apAlways) startAccessPoint();
    WiFi.begin(settings.ssid.c_str(), settings.wifiPassword.c_str());
    wifiStartedAt = millis();
    ui_show_system_status(UI_STATUS_NETWORK, "Connecting Wi-Fi",
                          settings.ssid.c_str(), -1, 0);
    Serial.printf("[web] connecting Wi-Fi: %s\n", settings.ssid.c_str());
  } else {
    WiFi.mode(WIFI_AP);
    startAccessPoint();
  }
  configureRoutes();
  Serial.println("[web] dashboard login: admin (change the default password)");
}

void management_loop() {
  server.handleClient();
  if (WiFi.status() == WL_CONNECTED && !announcedIp) {
    announcedIp = true;
    const String ip = WiFi.localIP().toString();
    ui_show_system_status(UI_STATUS_NETWORK, "Dashboard ready", ip.c_str(), -1,
                          6000);
    Serial.printf("[web] dashboard: http://%s/ (hostname: %s)\n",
                  ip.c_str(), settings.hostname.c_str());
  } else if (WiFi.status() != WL_CONNECTED && announcedIp) {
    announcedIp = false;
    ui_show_system_status(UI_STATUS_NETWORK, "Wi-Fi disconnected",
                          "Starting recovery", -1, 4000);
  }
  if (settings.ssid.length() && WiFi.status() != WL_CONNECTED && !apRunning &&
      millis() - wifiStartedAt > 15000) {
    WiFi.mode(WIFI_AP_STA);
    startAccessPoint();
  }
  if (rebootAt && (int32_t)(millis() - rebootAt) >= 0) {
    delay(50);
    ESP.restart();
  }
}

#endif
