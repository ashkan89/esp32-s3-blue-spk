/*
 * ble_control.h -- the BLE half of Wi-Fi + BLE mode.
 *
 * BLE cannot carry the music on this chip, so it carries everything else: what
 * the speaker is playing, the transport controls, and -- the reason this is
 * worth having at all -- the Wi-Fi credentials.
 *
 * That last one closes the hole the other network mode leaves open. Wi-Fi + BT
 * mode never raises the setup access point (an access point beside an audio
 * sink is the one combination this chip cannot do), so a speaker whose router
 * password changed is unreachable until somebody walks over and holds BOOT.
 * BLE is a second, independent way in that costs no antenna time worth
 * measuring: stand next to it with a phone and hand it a new network.
 *
 * Three characteristics on one service:
 *
 *   status   read + notify. Compact JSON, pushed whenever it changes.
 *   command  write. One line of text: play / pause / stop / vol N / url U.
 *   wifi     write. "<ssid>\n<password>", saved and applied through a restart.
 *
 * Everything a phone writes lands on the Bluedroid task. Nothing is acted on
 * there: commands go into net_audio's queue (which is built for exactly this),
 * and the Wi-Fi write sets a flag that ble_control_loop() picks up on the
 * Arduino task, where rebooting and writing NVS are safe things to do.
 */

#pragma once

#include <Arduino.h>

/// Starts the BLE controller, the GATT server and advertising. `device_name` is
/// what a phone shows in its scan list. Returns false if BLE could not start,
/// in which case the speaker still plays audio and still serves the dashboard.
bool ble_control_begin(const char *device_name);

/// Refreshes the status characteristic and services deferred writes. Arduino
/// loop task only.
void ble_control_loop();

/// True once ble_control_begin() has succeeded.
bool ble_control_running();

/// Number of phones currently connected. The dashboard shows it so "is BLE
/// actually doing anything" has an answer.
uint8_t ble_control_clients();
