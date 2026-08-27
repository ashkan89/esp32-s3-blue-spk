#include "ble_control.h"

#include <atomic>

#include <WiFi.h>

#include <BLE2902.h>
#include <BLECharacteristic.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#include "app_config.h"
#include "management.h"
#include "net_audio.h"
#include "player_state.h"

namespace {

/*
 * Custom 128-bit UUIDs. These are not registered with the Bluetooth SIG and are
 * not meant to be -- there is no adopted profile for "small speaker with a
 * dashboard". They are fixed constants so that anything written against this
 * firmware keeps working across updates; do not renumber them casually.
 */
const char *SVC_UUID = "d8f1c000-9b4e-4c2a-8f3d-1a2b3c4d5e6f";
const char *CHR_STATUS_UUID = "d8f1c001-9b4e-4c2a-8f3d-1a2b3c4d5e6f";
const char *CHR_COMMAND_UUID = "d8f1c002-9b4e-4c2a-8f3d-1a2b3c4d5e6f";
const char *CHR_WIFI_UUID = "d8f1c003-9b4e-4c2a-8f3d-1a2b3c4d5e6f";

BLEServer *server;
BLECharacteristic *statusChr;
bool running;
// Connect and disconnect both arrive on the Bluedroid task, so this only ever
// changes from one place -- but it is read from the Arduino task, hence atomic
// rather than plain. (Not volatile: read-modify-write on a volatile is
// deprecated in C++20 and was never actually atomic anyway.)
std::atomic<uint8_t> clients{0};

// Set on the Bluedroid task, acted on by the Arduino task. A Wi-Fi change means
// writing NVS and restarting, and neither belongs in a GATT write callback.
volatile bool wifiPending;
char pendingSsid[33];
char pendingPassword[65];

/// The last status we notified. Notifying an unchanged string every second
/// would wake the phone's radio for nothing.
String lastStatus;

/// Escapes the few characters that would break the JSON we hand out. Track
/// titles arrive from the internet and routinely contain quotes.
void appendJsonString(String &out, const char *value) {
  out += '"';
  for (const char *p = value; p != nullptr && *p; ++p) {
    const char c = *p;
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if ((uint8_t)c < 0x20) {
      out += ' ';
    } else {
      out += c;
    }
  }
  out += '"';
}

/*
 * The status document.
 *
 * Deliberately small. The default BLE MTU is 23 bytes, of which 20 carry
 * payload, and a client that never negotiates a larger one sees a truncated
 * notification. Anything that negotiates properly -- which every modern phone
 * does -- gets up to 517 and sees the whole thing. Keeping the common fields
 * first means even a truncated read is useful.
 */
String buildStatus() {
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

  String out;
  out.reserve(200);
  out += "{\"state\":";
  appendJsonString(out, state);
  out += ",\"vol\":";
  out += n.volume;
  out += ",\"title\":";
  appendJsonString(out, n.title);
  out += ",\"artist\":";
  appendJsonString(out, n.artist);
  out += ",\"src\":";
  appendJsonString(out, n.origin[0] ? n.origin : "");
  out += ",\"wifi\":";
  appendJsonString(out, WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "");
  out += ",\"ip\":";
  appendJsonString(out,
                   WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "");
  if (n.state == NET_AUDIO_ERROR && n.error[0]) {
    out += ",\"error\":";
    appendJsonString(out, n.error);
  }
  out += '}';
  return out;
}

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *) override {
    if (clients < 255) clients++;
  }
  void onDisconnect(BLEServer *s) override {
    if (clients) clients--;
    // Without this the speaker stops advertising after the first phone leaves
    // and can never be found again until a reboot.
    s->startAdvertising();
  }
};

/*
 * One line of text per write. A phone app or nRF Connect can drive the whole
 * player with a keyboard, which is worth more here than a packed binary format
 * nobody can debug from the other end.
 */
class CommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *chr) override {
    String line = chr->getValue();
    line.trim();
    if (!line.length()) return;

    if (line == "play") {
      net_audio_play();
    } else if (line == "pause") {
      net_audio_pause();
    } else if (line == "stop") {
      net_audio_stop();
    } else if (line.startsWith("vol ")) {
      const int value = line.substring(4).toInt();
      net_audio_set_volume((uint8_t)constrain(value, 0, 127));
    } else if (line.startsWith("url ")) {
      String url = line.substring(4);
      url.trim();
      net_audio_play_url(url.c_str(), "ble");
    }
    // Anything else is ignored on purpose. A GATT write has nowhere to report
    // an error to, and the status characteristic will simply not change -- the
    // client can see that for itself.
  }
};

/*
 * Wi-Fi provisioning: "<ssid>\n<password>".
 *
 * The password may be empty for an open network, so the newline is required
 * even then. This only records the intent; ble_control_loop() does the saving
 * and the restart, on a task where blocking is allowed.
 */
class WifiCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *chr) override {
    if (wifiPending) return;  // one at a time; a reboot is already coming

    const String body = chr->getValue();
    const int split = body.indexOf('\n');
    if (split <= 0) return;  // no SSID, or no separator: not a provisioning write

    String ssid = body.substring(0, split);
    String password = body.substring(split + 1);
    ssid.trim();
    password.trim();
    if (!ssid.length() || ssid.length() > 32) return;

    strlcpy(pendingSsid, ssid.c_str(), sizeof(pendingSsid));
    strlcpy(pendingPassword, password.c_str(), sizeof(pendingPassword));
    wifiPending = true;
  }
};

ServerCallbacks serverCallbacks;
CommandCallbacks commandCallbacks;
WifiCallbacks wifiCallbacks;

}  // namespace

bool ble_control_begin(const char *device_name) {
  if (running) return true;

  /*
   * Hand back the Bluetooth Classic half before the controller comes up.
   *
   * BR/EDR is not used in this mode -- there is no A2DP sink here -- and its
   * reserved DRAM is the single biggest block available to give back. It is the
   * mirror image of what the A2DP sink does in the Bluetooth modes, where the
   * library releases the BLE half for the same reason. This has to happen
   * before esp_bt_controller_init(), which BLEDevice::init() below performs.
   */
  const uint32_t heapBefore = ESP.getFreeHeap();
  btMemRelease(BT_MODE_CLASSIC_BT);

  BLEDevice::init(device_name);
  // The default MTU is 23 bytes, which truncates every status notification. The
  // controller still negotiates down for clients that ask for less.
  BLEDevice::setMTU(247);

  server = BLEDevice::createServer();
  if (server == nullptr) {
    Serial.println("[ble] could not create the GATT server");
    return false;
  }
  server->setCallbacks(&serverCallbacks);

  BLEService *service = server->createService(SVC_UUID);
  if (service == nullptr) {
    Serial.println("[ble] could not create the control service");
    return false;
  }

  statusChr = service->createCharacteristic(
      CHR_STATUS_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  // The Client Characteristic Configuration descriptor. Without it a phone has
  // no way to subscribe and only ever sees the value it explicitly reads.
  statusChr->addDescriptor(new BLE2902());
  statusChr->setValue(buildStatus().c_str());

  BLECharacteristic *commandChr = service->createCharacteristic(
      CHR_COMMAND_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  commandChr->setCallbacks(&commandCallbacks);

  BLECharacteristic *wifiChr = service->createCharacteristic(
      CHR_WIFI_UUID, BLECharacteristic::PROPERTY_WRITE);
  wifiChr->setCallbacks(&wifiCallbacks);

  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SVC_UUID);
  // The scan response carries the name; without this a phone lists an unnamed
  // device and there is no way to tell two speakers apart.
  advertising->setScanResponse(true);
  // iOS refuses connection intervals it considers too aggressive and the
  // connection then drops; these are the values Apple's own guidance asks for.
  advertising->setMinPreferred(0x06);
  advertising->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();

  running = true;
  Serial.printf("[ble] control service advertising as \"%s\" (heap %u -> %u)\n",
                device_name, (unsigned)heapBefore, (unsigned)ESP.getFreeHeap());
  return true;
}

bool ble_control_running() { return running; }

uint8_t ble_control_clients() { return clients; }

void ble_control_loop() {
  if (!running) return;

  // The deferred Wi-Fi change. Saving writes NVS and the restart has to happen
  // somewhere that is allowed to block, which a GATT callback is not.
  if (wifiPending) {
    wifiPending = false;
    Serial.printf("[ble] Wi-Fi credentials received for \"%s\"; restarting\n",
                  pendingSsid);
    management_provision_wifi(pendingSsid, pendingPassword);  // reboots
    return;
  }

  // Notify on change only, and no more than a few times a second. A phone that
  // is subscribed pays radio time for every notification.
  static uint32_t lastAt;
  const uint32_t now = millis();
  if (now - lastAt < 500) return;
  lastAt = now;

  const String next = buildStatus();
  if (next == lastStatus) return;
  lastStatus = next;
  statusChr->setValue(next.c_str());
  if (clients) statusChr->notify();
}
