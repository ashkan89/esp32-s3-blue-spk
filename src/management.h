#pragma once

#include <Arduino.h>
#include "app_config.h"
#include "status_led.h"

class BluetoothA2DPSink;

/*
 * Radio mode.
 *
 * This chip has one 2.4 GHz front end shared by Wi-Fi and Bluetooth Classic.
 * What Espressif's coexistence scheduler supports is Wi-Fi *station* alongside
 * Bluetooth. A SoftAP alongside an A2DP sink is not a supported combination,
 * and in practice neither side works properly -- the access point cannot be
 * joined, and the sink is not reliably discoverable. That is the line these
 * three modes are drawn along: the setup access point is exclusive, a station
 * is not.
 *
 *   MANAGEMENT  Wi-Fi is up -- station if a network is configured, the setup
 *               access point if not -- and the dashboard is reachable.
 *               Bluetooth is not started at all, and its RAM is handed back.
 *   BLUETOOTH   the A2DP sink owns the antenna. Wi-Fi is never initialised.
 *   COMBO       station plus A2DP sink, sharing the radio under coexistence.
 *               The dashboard stays reachable while music plays, which is what
 *               makes its media controls worth having. No setup access point
 *               ever comes up here, so the mode needs a network already saved:
 *               management_begin() demotes it to MANAGEMENT when there is none.
 *   NET         station plus BLE. Audio arrives over Wi-Fi instead of
 *               Bluetooth -- a DLNA/UPnP renderer anything can cast to, or a
 *               URL -- and BLE carries status, transport control and Wi-Fi
 *               provisioning. Bluetooth Classic is never started, so the setup
 *               access point is allowed here exactly as in MANAGEMENT.
 *   DFPLAYER    Wi-Fi exactly as in MANAGEMENT -- station if a network is
 *               saved, the setup access point if not, dashboard either way --
 *               and the audio comes from a DFPlayer Mini instead of the radio.
 *               The whole Bluetooth controller is released, because neither
 *               half of it is used: the DFPlayer speaks 9600 baud serial and
 *               produces its own analog output, so nothing about this mode
 *               wants the antenna and Wi-Fi gets all of it.
 *
 * Switching is a deliberate act (hold BOOT, or the dashboard) and takes effect
 * through a restart, so every mode begins from a clean radio.
 *
 * On BLE and audio: this is a classic ESP32, Bluetooth 4.2. LE Audio -- the
 * profile that carries music over Bluetooth Low Energy -- needs 5.2 silicon and
 * does not exist on this chip, and BLE 4.2 here sustains a small fraction of
 * what stereo music costs. So BLE is never an audio path in any mode: audio is
 * either A2DP (BR/EDR) or Wi-Fi. Each mode hands back the half of the
 * controller it does not use, which is where the heap for the updater's TLS
 * handshake comes from.
 */
enum RadioMode : uint8_t {
  RADIO_MODE_MANAGEMENT = 0,
  RADIO_MODE_BLUETOOTH = 1,
  RADIO_MODE_COMBO = 2,
  RADIO_MODE_NET = 3,
  RADIO_MODE_DFPLAYER = 4,
  RADIO_MODE_COUNT
};

/// Whether a mode brings up the Wi-Fi driver and the dashboard.
inline bool radio_mode_has_wifi(RadioMode mode) {
  return mode != RADIO_MODE_BLUETOOTH;
}

/// Whether a mode starts Bluetooth Classic and the A2DP sink. Deliberately not
/// "has Bluetooth": NET has Bluetooth too, just not the half that carries audio.
inline bool radio_mode_has_a2dp(RadioMode mode) {
  return mode == RADIO_MODE_BLUETOOTH || mode == RADIO_MODE_COMBO;
}

/// Whether a mode starts the BLE control service and the network audio player.
inline bool radio_mode_has_ble(RadioMode mode) {
  return mode == RADIO_MODE_NET;
}

/// Whether a mode drives the DFPlayer Mini. Nothing else in the firmware talks
/// to it, so this is also "is the DFPlayer the audio source".
inline bool radio_mode_has_dfplayer(RadioMode mode) {
  return mode == RADIO_MODE_DFPLAYER;
}

#if MANAGEMENT_ENABLED

// Reads the persisted Bluetooth name. The returned pointer remains valid for
// the lifetime of the program and is available before management_begin().
const char *management_device_name(const char *fallback);

/// The mode this boot is running in. Available after management_begin().
RadioMode management_radio_mode();

/// The next one in the cycle, for the BOOT button and the console:
/// Wi-Fi -> Bluetooth -> Wi-Fi + BT -> Wi-Fi + BLE -> DFPlayer -> Wi-Fi.
RadioMode management_next_mode();

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

/*
 * The DFPlayer's stored start-up values, for main.cpp to hand to the driver.
 *
 * The module keeps nothing across a power cycle -- not the volume, not the
 * source, not the EQ -- so every boot has to tell it again, and these are what
 * the dashboard's "save as startup defaults" button stored. Plain integers
 * rather than the driver's enums so management.h does not have to include
 * df_player.h, which would drag hw_config.h into every mode.
 *
 * Any pointer may be null. Values are on the module's own scales: source is a
 * DfSource, volume is 0..DF_VOLUME_MAX, loop is a DfLoop.
 */
void management_df_defaults(uint8_t *source, uint8_t *volume, uint8_t *eq,
                           uint8_t *loop, uint8_t *loopFolder, bool *autoplay);

// Saves a network and restarts into it. Used by the BLE provisioning
// characteristic, which is the way back in when the dashboard is unreachable
// because the saved credentials are wrong. Does not return.
void management_provision_wifi(const char *ssid, const char *password);

/*
 * Writes the live WS2812 configuration to NVS.
 *
 * The dashboard does not need this -- it goes through /api/leds, which persists
 * on its own after the sliders stop moving. This is for the serial console,
 * which reaches leds_configure() directly and would otherwise be the one way to
 * change a setting that does not survive a reboot.
 */
void management_store_leds();

// True when the network or update layer has something to show on the status LED
// that outranks anything Bluetooth has to say, in which case *out is filled in.
bool management_led_state(StatusLedState *out);
#else
inline const char *management_device_name(const char *fallback) { return fallback; }
inline RadioMode management_radio_mode() { return RADIO_MODE_BLUETOOTH; }
inline RadioMode management_next_mode() { return RADIO_MODE_BLUETOOTH; }
inline const char *management_mode_name(RadioMode) { return "Bluetooth"; }
inline void management_switch_mode(RadioMode) {}
inline void management_begin(BluetoothA2DPSink &) {}
inline void management_loop() {}
inline bool management_ap_running() { return false; }
inline void management_factory_reset() {}
inline void management_set_bt_active(bool) {}
inline void management_df_defaults(uint8_t *, uint8_t *, uint8_t *, uint8_t *,
                                   uint8_t *, bool *) {}
inline void management_provision_wifi(const char *, const char *) {}
inline void management_store_leds() {}
inline bool management_led_state(StatusLedState *) { return false; }
#endif
