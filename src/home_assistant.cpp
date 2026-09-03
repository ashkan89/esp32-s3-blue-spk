#include "home_assistant.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <string.h>

#include "alarm_clock.h"
#include "audio_eq.h"
#include "battery.h"
#include "df_player.h"
#include "leds.h"
#include "management.h"
#include "net_radio.h"
#include "player_state.h"
#include "power.h"
#include "soft_clock.h"
#include "telemetry.h"
#include "voice.h"

namespace {

/*
 * PubSubClient's buffer has to hold the largest single message, and the largest
 * here is a discovery document, not a state update. 1280 bytes covers the
 * biggest of them -- the radio station select, whose options list grows with
 * the favourites -- with room left for a long device name.
 */
const uint16_t MQTT_BUFFER = 1280;

/// Reconnect backoff. A broker that is down is usually down for a while, and
/// hammering it from a device that also has a radio stream to keep fed helps
/// nobody.
const uint32_t RECONNECT_MIN_MS = 5000;
const uint32_t RECONNECT_MAX_MS = 120000;

/*
 * Discovery is published a few entities at a time.
 *
 * Twenty-five documents at once is about 12 kB through a synchronous client on
 * the same task as the web server, which is a visible stall in the dashboard.
 * Spreading them over successive passes of loop() makes the whole announcement
 * take a quarter of a second that nobody notices instead of one that everybody
 * does.
 */
const uint8_t DISCOVERY_PER_PASS = 3;

WiFiClient net;
PubSubClient mqtt(net);

HaConfig config;
HaStatus status;
bool started;

uint32_t nextAttemptMs;
uint32_t backoffMs = RECONNECT_MIN_MS;
uint32_t lastPublishMs;

/// Where the staged discovery run has got to. Equal to the entity count when
/// there is nothing left to send.
uint8_t discoveryCursor;

char clientId[40];
char availabilityTopic[HA_TOPIC_MAX + 16];
char stateTopic[HA_TOPIC_MAX + 16];
char commandRoot[HA_TOPIC_MAX + 8];

/// Remembered so the mute button has something to unmute to, exactly as the
/// dashboard's does.
uint8_t mutedFrom = 80;

void copyString(char *dest, size_t size, const char *src) {
  if (!dest || !size) return;
  snprintf(dest, size, "%s", src ? src : "");
}

/// A topic-safe version of a name: lower case, and nothing MQTT or Home
/// Assistant would rather not see in an identifier.
void slugify(const char *in, char *out, size_t size) {
  size_t n = 0;
  for (const char *p = in; *p && n + 1 < size; ++p) {
    const char c = *p;
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
      out[n++] = c;
    } else if (c >= 'A' && c <= 'Z') {
      out[n++] = (char)(c - 'A' + 'a');
    } else if (n && out[n - 1] != '_') {
      out[n++] = '_';
    }
  }
  while (n && out[n - 1] == '_') n--;
  out[n] = 0;
  if (!n) copyString(out, size, "speaker");
}

void setError(HaState state, const char *message) {
  status.state = state;
  copyString(status.error, sizeof(status.error), message);
}

/// PubSubClient's numeric state, as something a person can read.
const char *brokerError(int code) {
  switch (code) {
    case -4: return "The broker did not answer in time";
    case -3: return "The connection was lost";
    case -2: return "Could not reach the broker";
    case -1: return "The broker disconnected us";
    case 1: return "The broker refused our MQTT version";
    case 2: return "The broker rejected the client id";
    case 3: return "The broker is unavailable";
    case 4: return "The user name or password was rejected";
    case 5: return "The broker refused to authorise this client";
    default: return "The broker refused the connection";
  }
}

// ------------------------------------------------------------- state -------

/// One word for what the speaker is doing, which is the entity everything else
/// in an automation keys off.
const char *playingWhat() {
  if (net_radio_active()) return "radio";
  if (df_player_active()) return "card";
  PlayerInfo p;
  ps_snapshot(&p);
  if (p.connected && p.streaming) return "bluetooth";
  if (p.connected) return "connected";
  return "idle";
}

uint8_t currentVolume() {
  if (net_radio_running() && net_radio_active()) return net_radio_volume();
  if (df_player_running()) return df_player_volume();
  PlayerInfo p;
  ps_snapshot(&p);
  return p.volume;
}

void buildState(JsonDocument &doc) {
  PlayerInfo p;
  ps_snapshot(&p);

  doc["state"] = playingWhat();
  doc["playing"] = (net_radio_active() || df_player_active() ||
                    (p.connected && p.streaming))
                       ? "ON"
                       : "OFF";
  doc["title"] = p.title[0] ? p.title : "";
  doc["artist"] = p.artist[0] ? p.artist : "";
  doc["peer"] = p.peer[0] ? p.peer : "";
  doc["mode"] = management_mode_name(management_radio_mode());

  const uint8_t volume = currentVolume();
  doc["volume"] = (uint8_t)((volume * 100 + 63) / 127);
  doc["mute"] = volume == 0 ? "ON" : "OFF";

  EqConfig eq;
  audio_eq_get(&eq);
  doc["eq"] = audio_eq_preset_name(eq.preset);

  RadioStatus radio;
  net_radio_snapshot(&radio);
  doc["station"] = (radio.station >= 0 && net_radio_active()) ? radio.name : "Stopped";
  doc["radio_state"] = net_radio_state_name(radio.state);
  doc["radio_buffer"] = radio.bufferPercent;
  doc["radio_bitrate"] = radio.bitrate;

  TelemetrySample now;
  telemetry_now(&now);
  if (now.millivolts) {
    doc["battery"] = now.percent;
    doc["voltage"] = serialized(String(now.millivolts / 1000.0f, 3));
  }
  doc["charging"] = (now.flags & TELEMETRY_CHARGING) ? "ON" : "OFF";
  if (now.deciCelsius != INT16_MIN) {
    doc["temperature"] = serialized(String(now.deciCelsius / 10.0f, 1));
  }
  doc["heap"] = now.heapKb;
  if (now.rssi) doc["rssi"] = now.rssi;
  doc["uptime"] = telemetry_uptime_seconds();
  doc["runtime"] = telemetry_runtime_seconds() / 3600;

  AlarmStatus alarm;
  alarm_status(&alarm);
  doc["alarm_state"] = alarm.state == ALARM_RINGING  ? "ringing"
                       : alarm.state == ALARM_SNOOZED ? "snoozed"
                                                      : "idle";
  doc["alarm_next"] = alarm.nextInSecs;
  doc["sleep"] = alarm.sleepRunning ? (alarm.sleepLeftSecs + 59) / 60 : 0;
  for (uint8_t i = 0; i < alarm_count(); i++) {
    Alarm a;
    if (!alarm_get(i, &a)) continue;
    char key[12];
    snprintf(key, sizeof(key), "alarm%u", (unsigned)(i + 1));
    doc[key] = a.enabled ? "ON" : "OFF";
  }

  LedConfig leds;
  leds_get(&leds);
  JsonObject light = doc["light"].to<JsonObject>();
  light["state"] = leds.enabled && leds.effect != LED_FX_OFF ? "ON" : "OFF";
  light["brightness"] = leds.brightness;
  JsonObject color = light["color"].to<JsonObject>();
  color["r"] = (leds.color >> 16) & 0xFF;
  color["g"] = (leds.color >> 8) & 0xFF;
  color["b"] = leds.color & 0xFF;
  light["effect"] = leds_effect_name(leds.effect);
}

bool publishState() {
  if (!mqtt.connected()) return false;
  JsonDocument doc;
  buildState(doc);
  String payload;
  serializeJson(doc, payload);
  const bool ok = mqtt.publish(stateTopic, payload.c_str(), true);
  if (ok) status.published++;
  return ok;
}

// ----------------------------------------------------------- discovery -----

/*
 * The entity table.
 *
 * Everything a Home Assistant discovery document needs that is not the same for
 * every entity. The abbreviated keys are Home Assistant's own -- "stat_t" for
 * state_topic and so on -- and are used rather than the long forms because the
 * documents have to fit in the MQTT buffer, and between them the short keys
 * save about a third.
 */
struct Entity {
  const char *component;  ///< sensor, switch, number, button, select, light
  const char *object;     ///< object id, unique within this device
  const char *name;       ///< what a person sees
  const char *key;        ///< field in the state JSON, or null for command-only
  const char *deviceClass;
  const char *unit;
  const char *icon;
  const char *command;    ///< command topic suffix, or null for read-only
};

const Entity ENTITIES[] = {
    // --- readings ---------------------------------------------------------
    {"sensor", "state", "Source", "state", nullptr, nullptr, "mdi:speaker", nullptr},
    {"sensor", "title", "Now playing", "title", nullptr, nullptr, "mdi:music-note", nullptr},
    {"sensor", "peer", "Connected device", "peer", nullptr, nullptr, "mdi:cellphone", nullptr},
    {"sensor", "battery", "Battery", "battery", "battery", "%", nullptr, nullptr},
    {"sensor", "voltage", "Battery voltage", "voltage", "voltage", "V", nullptr, nullptr},
    {"sensor", "temperature", "Chip temperature", "temperature", "temperature", "°C", nullptr, nullptr},
    {"sensor", "heap", "Free memory", "heap", nullptr, "kB", "mdi:memory", nullptr},
    {"sensor", "rssi", "Wi-Fi signal", "rssi", "signal_strength", "dBm", nullptr, nullptr},
    {"sensor", "uptime", "Uptime", "uptime", "duration", "s", nullptr, nullptr},
    {"sensor", "runtime", "Total runtime", "runtime", "duration", "h", nullptr, nullptr},
    {"sensor", "radio_state", "Radio status", "radio_state", nullptr, nullptr, "mdi:radio", nullptr},
    {"sensor", "radio_buffer", "Radio buffer", "radio_buffer", nullptr, "%", "mdi:download", nullptr},
    {"sensor", "alarm_state", "Alarm status", "alarm_state", nullptr, nullptr, "mdi:alarm", nullptr},
    {"binary_sensor", "charging", "Charging", "charging", "battery_charging", nullptr, nullptr, nullptr},
    {"binary_sensor", "playing", "Playing", "playing", "running", nullptr, nullptr, nullptr},

    // --- controls ---------------------------------------------------------
    {"number", "volume", "Volume", "volume", nullptr, "%", "mdi:volume-high", "volume"},
    {"switch", "mute", "Mute", "mute", nullptr, nullptr, "mdi:volume-off", "mute"},
    {"button", "play_pause", "Play / pause", nullptr, nullptr, nullptr, "mdi:play-pause", "toggle"},
    {"button", "next", "Next track", nullptr, nullptr, nullptr, "mdi:skip-next", "next"},
    {"button", "previous", "Previous track", nullptr, nullptr, nullptr, "mdi:skip-previous", "previous"},
    {"select", "eq", "Equaliser", "eq", nullptr, nullptr, "mdi:tune", "eq"},
    {"select", "station", "Radio station", "station", nullptr, nullptr, "mdi:radio", "station"},
    {"number", "sleep", "Sleep timer", "sleep", nullptr, "min", "mdi:timer-sand", "sleep"},
    {"button", "standby", "Standby (cannot wake over MQTT)", nullptr, nullptr, nullptr,
     "mdi:power-sleep", "standby"},
};

const uint8_t ENTITY_COUNT = sizeof(ENTITIES) / sizeof(ENTITIES[0]);

/// The light and the per-alarm switches come after the table, because both have
/// a shape the table cannot express: one is a different schema entirely, and the
/// others depend on how many alarms exist.
const uint8_t EXTRA_LIGHT = ENTITY_COUNT;
const uint8_t EXTRA_ALARMS = ENTITY_COUNT + 1;
uint8_t discoveryTotal() { return (uint8_t)(ENTITY_COUNT + 1 + alarm_count()); }

/// The device block every entity carries, which is what makes Home Assistant
/// group them under one card instead of scattering two dozen loose entities.
void addDevice(JsonObject doc) {
  JsonObject dev = doc["dev"].to<JsonObject>();
  JsonArray ids = dev["ids"].to<JsonArray>();
  ids.add(clientId);
  dev["name"] = management_device_name(APP_NAME);
  dev["mdl"] = "ESP32 Blue Speaker";
  dev["mf"] = "esp32-blue-spk";
  dev["sw"] = FW_VERSION;
  dev["cu"] = String("http://") + WiFi.localIP().toString();
}

bool publishDiscovery(const char *component, const char *object, JsonDocument &doc) {
  char topic[HA_TOPIC_MAX + 96];
  snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config", config.discoveryPrefix,
           component, clientId, object);
  String payload;
  serializeJson(doc, payload);
  const bool ok = mqtt.publish(topic, payload.c_str(), true);
  if (ok) status.published++;
  return ok;
}

void discoverTableEntity(const Entity &e) {
  JsonDocument doc;
  doc["~"] = config.baseTopic;
  doc["name"] = e.name;
  doc["uniq_id"] = String(clientId) + "_" + e.object;
  doc["avty_t"] = "~/status";
  if (e.key) {
    doc["stat_t"] = "~/state";
    const bool numeric = strcmp(e.component, "number") == 0;
    doc["val_tpl"] = String("{{ value_json.") + e.key +
                     (numeric ? " | default(0) }}" : " | default('') }}");
  }
  if (e.command) doc["cmd_t"] = String("~/cmd/") + e.command;
  if (e.deviceClass) doc["dev_cla"] = e.deviceClass;
  if (e.unit) doc["unit_of_meas"] = e.unit;
  if (e.icon) doc["ic"] = e.icon;

  if (strcmp(e.component, "sensor") == 0 && e.unit) {
    const bool climbs = strcmp(e.object, "uptime") == 0 ||
                        strcmp(e.object, "runtime") == 0;
    doc["stat_cla"] = climbs ? "total_increasing" : "measurement";
  }

  if (strcmp(e.component, "number") == 0) {
    doc["min"] = 0;
    doc["max"] = strcmp(e.object, "sleep") == 0 ? 600 : 100;
    doc["step"] = strcmp(e.object, "sleep") == 0 ? 5 : 1;
    doc["mode"] = "slider";
  }
  if (strcmp(e.component, "button") == 0) doc["payload_press"] = "press";

  if (strcmp(e.object, "eq") == 0) {
    JsonArray options = doc["options"].to<JsonArray>();
    for (uint8_t i = 0; i < EQ_PRESET_COUNT; i++) options.add(audio_eq_preset_name(i));
  } else if (strcmp(e.object, "station") == 0) {
    JsonArray options = doc["options"].to<JsonArray>();
    options.add("Stopped");
    for (uint8_t i = 0; i < net_radio_station_count(); i++) {
      RadioStation s;
      if (net_radio_station(i, &s)) options.add(s.name);
    }
  }

  addDevice(doc.as<JsonObject>());
  publishDiscovery(e.component, e.object, doc);
}

void discoverLight() {
  JsonDocument doc;
  doc["~"] = config.baseTopic;
  doc["name"] = "Ring";
  doc["uniq_id"] = String(clientId) + "_ring";
  doc["avty_t"] = "~/status";
  doc["schema"] = "json";
  doc["stat_t"] = "~/state";
  doc["val_tpl"] = "{{ value_json.light | tojson }}";
  doc["cmd_t"] = "~/cmd/light";
  doc["brightness"] = true;
  JsonArray modes = doc["supported_color_modes"].to<JsonArray>();
  modes.add("rgb");
  doc["effect"] = true;
  JsonArray effects = doc["effect_list"].to<JsonArray>();
  for (uint8_t i = 0; i < LED_FX_COUNT; i++) effects.add(leds_effect_name(i));
  addDevice(doc.as<JsonObject>());
  publishDiscovery("light", "ring", doc);
}

void discoverAlarm(uint8_t index) {
  Alarm a;
  if (!alarm_get(index, &a)) return;
  char object[16];
  snprintf(object, sizeof(object), "alarm%u", (unsigned)(index + 1));

  char name[48];
  if (a.label[0]) {
    snprintf(name, sizeof(name), "Alarm %u - %s", (unsigned)(index + 1), a.label);
  } else {
    snprintf(name, sizeof(name), "Alarm %u - %02u:%02u", (unsigned)(index + 1),
             (unsigned)a.hour, (unsigned)a.minute);
  }

  JsonDocument doc;
  doc["~"] = config.baseTopic;
  doc["name"] = name;
  doc["uniq_id"] = String(clientId) + "_" + object;
  doc["avty_t"] = "~/status";
  doc["stat_t"] = "~/state";
  doc["val_tpl"] = String("{{ value_json.") + object + " | default('OFF') }}";
  doc["cmd_t"] = String("~/cmd/") + object;
  doc["ic"] = "mdi:alarm";
  addDevice(doc.as<JsonObject>());
  publishDiscovery("switch", object, doc);
}

/// Sends the next few discovery documents. Returns true while there are more.
bool serviceDiscovery() {
  if (!config.discovery || discoveryCursor >= discoveryTotal()) return false;
  for (uint8_t sent = 0; sent < DISCOVERY_PER_PASS && discoveryCursor < discoveryTotal();
       sent++, discoveryCursor++) {
    if (discoveryCursor < ENTITY_COUNT) {
      discoverTableEntity(ENTITIES[discoveryCursor]);
    } else if (discoveryCursor == EXTRA_LIGHT) {
      discoverLight();
    } else {
      discoverAlarm((uint8_t)(discoveryCursor - EXTRA_ALARMS));
    }
  }
  if (discoveryCursor >= discoveryTotal()) {
    status.discoveryDone = true;
    Serial.printf("[mqtt] published %u discovery documents\n",
                  (unsigned)discoveryTotal());
    return false;
  }
  return true;
}

// ------------------------------------------------------------- commands ----

void applyLightCommand(const char *payload) {
  JsonDocument doc;
  if (deserializeJson(doc, payload)) return;
  LedConfig cfg;
  leds_get(&cfg);

  if (!doc["state"].isNull()) {
    const bool on = strcmp(doc["state"] | "", "ON") == 0;
    cfg.enabled = on;
    // Turning a Home Assistant light "on" when the stored effect is Off would
    // leave it dark and looking broken, so it lands on the plain solid colour.
    if (on && cfg.effect == LED_FX_OFF) cfg.effect = LED_FX_SOLID;
  }
  if (!doc["brightness"].isNull()) cfg.brightness = doc["brightness"] | cfg.brightness;
  if (!doc["color"].isNull()) {
    const uint8_t r = doc["color"]["r"] | 0;
    const uint8_t g = doc["color"]["g"] | 0;
    const uint8_t b = doc["color"]["b"] | 0;
    cfg.color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
  }
  if (!doc["effect"].isNull()) {
    const char *want = doc["effect"] | "";
    for (uint8_t i = 0; i < LED_FX_COUNT; i++) {
      if (strcasecmp(want, leds_effect_name(i)) == 0) {
        cfg.effect = i;
        break;
      }
    }
  }
  leds_configure(cfg);
  management_store_leds();
}

void onMessage(char *topic, uint8_t *payloadBytes, unsigned int length) {
  // PubSubClient hands over the payload without a terminator, and every command
  // here is short text.
  char payload[128];
  const unsigned int n = length < sizeof(payload) - 1 ? length : sizeof(payload) - 1;
  memcpy(payload, payloadBytes, n);
  payload[n] = 0;

  const char *what = strrchr(topic, '/');
  if (!what) return;
  what++;
  status.received++;

  if (strcmp(what, "volume") == 0) {
    const int percent = constrain(atoi(payload), 0, 100);
    const uint8_t value = (uint8_t)((percent * 127 + 50) / 100);
    if (value) mutedFrom = value;
    management_media_action("volume", value);
  } else if (strcmp(what, "mute") == 0) {
    const bool on = strcmp(payload, "ON") == 0;
    management_media_action("volume", on ? 0 : (mutedFrom ? mutedFrom : 80));
  } else if (strcmp(what, "toggle") == 0 || strcmp(what, "next") == 0 ||
             strcmp(what, "previous") == 0) {
    management_media_action(what, 0);
  } else if (strcmp(what, "eq") == 0) {
    EqConfig eq;
    audio_eq_get(&eq);
    for (uint8_t i = 0; i < EQ_PRESET_COUNT; i++) {
      if (strcasecmp(payload, audio_eq_preset_name(i)) == 0) {
        eq.preset = i;
        eq.enabled = true;
        audio_eq_preset_gains(i, eq.gain);
        audio_eq_configure(eq);
        management_store_audio();
        break;
      }
    }
  } else if (strcmp(what, "station") == 0) {
    if (strcasecmp(payload, "Stopped") == 0) {
      net_radio_stop();
    } else {
      for (uint8_t i = 0; i < net_radio_station_count(); i++) {
        RadioStation s;
        if (net_radio_station(i, &s) && strcmp(payload, s.name) == 0) {
          net_radio_play_station(i);
          break;
        }
      }
    }
  } else if (strcmp(what, "sleep") == 0) {
    const int minutes = constrain(atoi(payload), 0, 600);
    if (minutes) alarm_sleep_start((uint16_t)minutes, alarm_sleep_standby_default());
    else alarm_sleep_cancel();
  } else if (strcmp(what, "standby") == 0) {
    if (power_sleep_possible()) power_sleep_now();
  } else if (strcmp(what, "light") == 0) {
    applyLightCommand(payload);
  } else if (strncmp(what, "alarm", 5) == 0) {
    const int which = atoi(what + 5);
    Alarm a;
    if (which >= 1 && alarm_get((uint8_t)(which - 1), &a)) {
      a.enabled = strcmp(payload, "ON") == 0;
      alarm_set((uint8_t)(which - 1), a);
    }
  } else {
    status.received--;  // not ours after all
    return;
  }

  // Home Assistant's optimistic update is replaced by the real one immediately,
  // so a command the speaker could not honour -- a radio station in Bluetooth
  // mode, say -- snaps back rather than lying on the dashboard.
  publishState();
}

bool connectBroker() {
  if (!config.host[0]) {
    setError(HA_FAILED, "No broker address is set");
    return false;
  }
  status.state = HA_CONNECTING;
  mqtt.setServer(config.host, config.port ? config.port : 1883);
  mqtt.setBufferSize(MQTT_BUFFER);
  mqtt.setKeepAlive(30);
  mqtt.setCallback(onMessage);

  const bool ok =
      config.user[0]
          ? mqtt.connect(clientId, config.user, config.password, availabilityTopic,
                         0, true, "offline")
          : mqtt.connect(clientId, availabilityTopic, 0, true, "offline");

  if (!ok) {
    setError(HA_FAILED, brokerError(mqtt.state()));
    Serial.printf("[mqtt] %s (%d)\n", status.error, mqtt.state());
    return false;
  }

  status.state = HA_CONNECTED;
  status.error[0] = 0;
  status.connectedAt = millis();
  status.connects++;
  backoffMs = RECONNECT_MIN_MS;

  mqtt.publish(availabilityTopic, "online", true);
  char wildcard[sizeof(commandRoot) + 4];
  snprintf(wildcard, sizeof(wildcard), "%s/#", commandRoot);
  mqtt.subscribe(wildcard);

  discoveryCursor = 0;
  status.discoveryDone = false;
  Serial.printf("[mqtt] connected to %s:%u as %s\n", config.host,
                (unsigned)config.port, clientId);
  return true;
}

void buildTopics() {
  const uint32_t id = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFF);
  char slug[HA_TOPIC_MAX];
  slugify(config.baseTopic[0] ? config.baseTopic : APP_NAME, slug, sizeof(slug));
  copyString(config.baseTopic, sizeof(config.baseTopic), slug);
  snprintf(clientId, sizeof(clientId), "%s_%06lX", slug, (unsigned long)id);
  snprintf(availabilityTopic, sizeof(availabilityTopic), "%s/status", slug);
  snprintf(stateTopic, sizeof(stateTopic), "%s/state", slug);
  snprintf(commandRoot, sizeof(commandRoot), "%s/cmd", slug);
}

}  // namespace

void ha_defaults(HaConfig *out, const char *hostname) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  out->enabled = false;
  out->port = 1883;
  copyString(out->baseTopic, sizeof(out->baseTopic), hostname ? hostname : APP_NAME);
  copyString(out->discoveryPrefix, sizeof(out->discoveryPrefix), "homeassistant");
  out->discovery = true;
  out->publishSeconds = 15;
}

void ha_configure(const HaConfig &cfg) {
  const bool moved = strcmp(config.host, cfg.host) != 0 ||
                     config.port != cfg.port ||
                     strcmp(config.user, cfg.user) != 0 ||
                     strcmp(config.password, cfg.password) != 0 ||
                     strcmp(config.baseTopic, cfg.baseTopic) != 0 ||
                     config.enabled != cfg.enabled;

  config = cfg;
  if (config.port == 0) config.port = 1883;
  if (config.publishSeconds < 5) config.publishSeconds = 5;
  if (config.publishSeconds > 600) config.publishSeconds = 600;
  if (!config.discoveryPrefix[0]) {
    copyString(config.discoveryPrefix, sizeof(config.discoveryPrefix), "homeassistant");
  }
  buildTopics();

  if (moved && mqtt.connected()) {
    mqtt.publish(availabilityTopic, "offline", true);
    mqtt.disconnect();
  }
  if (!config.enabled) {
    status.state = HA_OFF;
    status.error[0] = 0;
  } else {
    nextAttemptMs = millis();  // try at once rather than after the backoff
    backoffMs = RECONNECT_MIN_MS;
  }
}

void ha_get(HaConfig *out) {
  if (out) *out = config;
}

void ha_status(HaStatus *out) {
  if (!out) return;
  *out = status;
  if (!config.enabled) out->state = HA_OFF;
  else if (WiFi.status() != WL_CONNECTED) out->state = HA_UNAVAILABLE;
}

void ha_begin() {
  started = true;
  if (!config.baseTopic[0]) ha_defaults(&config, APP_NAME);
  buildTopics();
  status.state = config.enabled ? HA_CONNECTING : HA_OFF;
  nextAttemptMs = millis();
}

void ha_loop() {
  if (!started || !config.enabled) return;
  if (WiFi.status() != WL_CONNECTED) return;

  if (!mqtt.connected()) {
    const uint32_t now = millis();
    if ((int32_t)(now - nextAttemptMs) < 0) return;
    if (!connectBroker()) {
      nextAttemptMs = now + backoffMs;
      backoffMs = backoffMs * 2 > RECONNECT_MAX_MS ? RECONNECT_MAX_MS : backoffMs * 2;
      return;
    }
  }

  mqtt.loop();

  // Discovery first, and nothing else while it runs: an entity's state is
  // meaningless to Home Assistant until the entity exists.
  if (serviceDiscovery()) return;

  const uint32_t now = millis();
  if (now - lastPublishMs >= (uint32_t)config.publishSeconds * 1000u) {
    lastPublishMs = now;
    publishState();
  }
}

void ha_announce() {
  if (!config.enabled || !mqtt.connected()) return;
  discoveryCursor = 0;
  status.discoveryDone = false;
  lastPublishMs = 0;
}

void ha_publish_state() {
  if (!config.enabled || !mqtt.connected()) return;
  publishState();
  lastPublishMs = millis();
}

bool ha_command(const char *line) {
  if (!line || strcmp(line, "mqtt") != 0) return false;

  HaStatus s;
  ha_status(&s);
  const char *name = s.state == HA_OFF           ? "off"
                     : s.state == HA_UNAVAILABLE ? "no network"
                     : s.state == HA_CONNECTING  ? "connecting"
                     : s.state == HA_CONNECTED   ? "connected"
                                                 : "failed";
  Serial.printf("[mqtt] %s", name);
  if (config.enabled) {
    Serial.printf(" | %s:%u | topic %s", config.host[0] ? config.host : "(no broker)",
                  (unsigned)config.port, config.baseTopic);
  }
  if (s.error[0]) Serial.printf(" | %s", s.error);
  Serial.printf(" | %lu published, %lu commands, %lu connects%s\n",
                (unsigned long)s.published, (unsigned long)s.received,
                (unsigned long)s.connects,
                s.discoveryDone ? ", discovery sent" : "");
  return true;
}
