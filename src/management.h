#pragma once

#include <Arduino.h>
#include "app_config.h"
#include "status_led.h"

class BluetoothA2DPSink;

/*
 * Radio mode.
 *
 * This chip has one 2.4 GHz front end shared by Wi-Fi and Bluetooth Classic.
 * Espressif's coexistence scheduler covers Wi-Fi *station* plus Bluetooth; a
 * SoftAP alongside an A2DP sink is not a supported combination, and in practice
 * neither side works properly -- the access point cannot be joined, and the
 * sink is not reliably discoverable. Trying to referee that contest with
 * coexistence preferences and quiet windows was worse than choosing.
 *
 * So the speaker does one thing at a time:
 *
 *   MANAGEMENT  Wi-Fi is up -- station if a network is configured, the setup
 *               access point if not -- and the dashboard is reachable.
 *               Bluetooth is not started at all.
 *   BLUETOOTH   the A2DP sink owns the antenna. Wi-Fi is never initialised.
 *
 * Switching is a deliberate act (hold BOOT, or the dashboard button) and takes
 * effect through a restart, so each mode always begins from a clean radio.
 */
enum RadioMode : uint8_t {
  RADIO_MODE_MANAGEMENT = 0,
  RADIO_MODE_BLUETOOTH = 1,
};

#if MANAGEMENT_ENABLED

// Reads the persisted Bluetooth name. The returned pointer remains valid for
// the lifetime of the program and is available before management_begin().
const char *management_device_name(const char *fallback);

/// The mode this boot is running in. Available after management_begin().
RadioMode management_radio_mode();

/// The other one, for prompts and toggles.
RadioMode management_other_mode();

/// Human-readable, for the console and the display.
const char *management_mode_name(RadioMode mode);

/// Persists the mode and restarts into it. Does not return.
void management_switch_mode(RadioMode mode);

// Starts whatever this mode needs. In MANAGEMENT that is Wi-Fi provisioning,
// the HTTP dashboard and the OTA services; in BLUETOOTH it reads the stored
// settings and otherwise leaves the radio alone.
void management_begin(BluetoothA2DPSink &sink);

// Services HTTP requests and deferred operations. Must be called from loop().
void management_loop();

// True while the setup access point is up.
bool management_ap_running();

// Clears every stored setting and every Bluetooth bond. Does NOT reboot: the
// BOOT button is also the download-mode strap, so the caller has to wait for it
// to be released before restarting.
void management_factory_reset();

// Told by main.cpp so the dashboard can report whether Bluetooth is live.
void management_set_bt_active(bool active);

// True when the network or update layer has something to show on the status LED
// that outranks anything Bluetooth has to say, in which case *out is filled in.
bool management_led_state(StatusLedState *out);
#else
inline const char *management_device_name(const char *fallback) { return fallback; }
inline RadioMode management_radio_mode() { return RADIO_MODE_BLUETOOTH; }
inline RadioMode management_other_mode() { return RADIO_MODE_BLUETOOTH; }
inline const char *management_mode_name(RadioMode) { return "Bluetooth"; }
inline void management_switch_mode(RadioMode) {}
inline void management_begin(BluetoothA2DPSink &) {}
inline void management_loop() {}
inline bool management_ap_running() { return false; }
inline void management_factory_reset() {}
inline void management_set_bt_active(bool) {}
inline bool management_led_state(StatusLedState *) { return false; }
#endif
