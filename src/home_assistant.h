/*
 * home_assistant.h -- MQTT, and the Home Assistant discovery that makes it
 * useful without anybody writing YAML.
 *
 * What this is for. The dashboard in this firmware is complete, and it is also
 * a thing you have to open. Half of what a speaker in a house should do is
 * conditional on something else -- turn the radio on when the alarm in the
 * bedroom goes off, drop the volume when the doorbell rings, tell me when the
 * battery is low -- and none of that can live in the speaker, because the
 * speaker does not know about the doorbell. So the speaker publishes what it
 * knows and subscribes to what it can be told, and the automation lives where
 * the rest of the house's automation already lives.
 *
 * Discovery, and why it is not optional here. Home Assistant will happily talk
 * to a device that publishes to hand-written topics, provided somebody writes
 * about eighty lines of YAML per entity and keeps it in step with the firmware.
 * Nobody does that twice. Publishing the discovery documents means the speaker
 * appears in Home Assistant, complete, within a second of connecting, with the
 * right names and units and device-class icons -- and, crucially, with the
 * entity list following the firmware rather than the configuration file: add a
 * radio station and the select entity has one more option next time it
 * connects.
 *
 * What appears. One device, with:
 *
 *   controls    volume, mute, play/pause, next, previous, the equaliser preset,
 *               the radio station (as a dropdown of the stored favourites), the
 *               LED ring as a light with colour and brightness, the sleep timer
 *               as a number of minutes, and one switch per configured alarm.
 *   readings    what is playing and what it is called, the battery voltage,
 *               charge and charging state, the die temperature, free heap, Wi-Fi
 *               signal, uptime and total runtime.
 *   the switch  standby, as a switch that turns off and cannot turn back on --
 *               which is the honest shape for it, because a chip in deep sleep
 *               is not listening to MQTT and no amount of protocol will change
 *               that. The entity says so in its name.
 *
 * Which modes. The two with Wi-Fi, obviously -- MQTT is TCP. In Bluetooth mode
 * this whole module is inert and the dashboard's Home Assistant page says why.
 *
 * Where it runs. From loop(), through the ordinary synchronous PubSubClient,
 * alongside the web server. That is a deliberate choice against an async
 * client: the radio's decoder task already wants a steady share of the network
 * stack, and a second task doing TLS-less MQTT keepalives is contention for no
 * benefit -- the traffic here is a few hundred bytes every ten seconds.
 */

#pragma once

#include <stdint.h>

class String;

static const uint8_t HA_HOST_MAX = 64;
static const uint8_t HA_TOPIC_MAX = 48;
static const uint8_t HA_USER_MAX = 32;
static const uint8_t HA_PASS_MAX = 64;

struct HaConfig {
  bool enabled;
  char host[HA_HOST_MAX];
  uint16_t port;
  char user[HA_USER_MAX];
  char password[HA_PASS_MAX];
  /// The prefix every topic from this speaker sits under. Defaults to the
  /// hostname, so two speakers on one broker do not collide.
  char baseTopic[HA_TOPIC_MAX];
  /// Where Home Assistant listens for discovery documents. "homeassistant"
  /// unless somebody changed it, which almost nobody has.
  char discoveryPrefix[HA_TOPIC_MAX];
  bool discovery;
  /// Seconds between state publications. Every reading is also published the
  /// moment it changes, so this is a heartbeat rather than a poll.
  uint16_t publishSeconds;
};

enum HaState : uint8_t {
  HA_OFF = 0,        ///< switched off in settings
  HA_UNAVAILABLE,    ///< no Wi-Fi, or this mode has none
  HA_CONNECTING,
  HA_CONNECTED,
  HA_FAILED,         ///< the broker refused or could not be reached
};

struct HaStatus {
  HaState state;
  char error[64];
  uint32_t connectedAt;
  uint32_t connects;      ///< how many times, so a flapping link is visible
  uint32_t published;     ///< messages sent since boot
  uint32_t received;      ///< commands acted on since boot
  bool discoveryDone;
};

void ha_defaults(HaConfig *out, const char *hostname);

/// Applies a configuration. A change of broker, credentials or topic drops the
/// current connection so the next attempt uses the new one; a change to the
/// publish interval alone does not.
void ha_configure(const HaConfig &cfg);
void ha_get(HaConfig *out);
void ha_status(HaStatus *out);

/// Starts the client. Call in a mode with Wi-Fi, after the settings are loaded.
void ha_begin();

/// Services the connection, the reconnect backoff and the publish heartbeat.
/// Arduino loop task only.
void ha_loop();

/// Publishes everything now, and re-publishes the discovery documents. What the
/// dashboard's "resend to Home Assistant" button calls, and what happens by
/// itself whenever the station list or the alarms change -- because both change
/// the shape of an entity rather than its value.
void ha_announce();

/// Publishes the volatile state now rather than at the next heartbeat. Called
/// from the API layer after anything that changes what is playing, so Home
/// Assistant reflects a dashboard press immediately.
void ha_publish_state();

/// Serial console: "mqtt". Returns false if the line was something else.
bool ha_command(const char *line);
