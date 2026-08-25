#pragma once

#include <Arduino.h>
#include "app_config.h"

class BluetoothA2DPSink;

#if MANAGEMENT_ENABLED

// Reads the persisted Bluetooth name. The returned pointer remains valid for
// the lifetime of the program and is available before management_begin().
const char *management_device_name(const char *fallback);

// Starts Wi-Fi provisioning, the HTTP dashboard, mDNS, and OTA services.
// Call before sink.start() so enabling the Wi-Fi radio cannot interrupt audio.
void management_begin(BluetoothA2DPSink &sink);

// Services HTTP requests and deferred operations. Must be called from loop().
void management_loop();
#else
inline const char *management_device_name(const char *fallback) { return fallback; }
inline void management_begin(BluetoothA2DPSink &) {}
inline void management_loop() {}
#endif
