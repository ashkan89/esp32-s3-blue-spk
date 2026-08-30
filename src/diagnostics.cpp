#include "diagnostics.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include "app_config.h"
#include "audio_probe.h"
#include "battery.h"
#include "ble_control.h"
#include "df_player.h"
#include "hw_config.h"
#include "leds.h"
#include "management.h"
#include "net_audio.h"
// For PIN_MAP_I2S_*: pin_check.h is where the whole pin map is visible at once,
// which is exactly what a report of the pin map wants.
#include "pin_check.h"
#include "player_state.h"
#include "power.h"
#include "soft_clock.h"
#include "status_led.h"
#include "ui.h"
#include "ui_config.h"

namespace {

const char *yes_no(bool value) { return value ? "yes" : "no"; }

const char *reset_reason(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_EXT: return "external reset";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "PANIC (crash)";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_BROWNOUT: return "BROWNOUT (supply sagged)";
    case ESP_RST_DEEPSLEEP: return "wake from deep sleep";
    default: return "unknown";
  }
}

void print_uptime(uint32_t ms) {
  const uint32_t s = ms / 1000;
  Serial.printf("%ud %02uh %02um %02us", (unsigned)(s / 86400),
                (unsigned)((s % 86400) / 3600), (unsigned)((s % 3600) / 60),
                (unsigned)(s % 60));
}

/*
 * Stack headroom for one task, by name.
 *
 * uxTaskGetStackHighWaterMark() reports the *smallest* the free part of the
 * stack has ever been, in words -- so it is a watermark that only ever falls,
 * and it is the only warning available before an overflow, which on this chip
 * is a panic with a backtrace pointing at whatever unlucky function was running
 * when the guard was crossed. Under about 400 bytes is worth acting on.
 *
 * The lookup is by name because these tasks are created in five different files
 * and none of them keeps its handle; xTaskGetHandle() is a linear walk of the
 * task list, which for a dozen tasks in a command nobody types twice a minute
 * is not a cost worth engineering around.
 */
void print_stack(const char *name) {
  const TaskHandle_t task = xTaskGetHandle(name);
  if (task == nullptr) {
    Serial.printf("  %-10s not running\n", name);
    return;
  }
  const unsigned bytes = uxTaskGetStackHighWaterMark(task) * sizeof(StackType_t);
  Serial.printf("  %-10s %5u bytes never used%s\n", name, bytes,
                bytes < 400 ? "   <-- tight, raise this task's stack" : "");
}

void print_partitions() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_partition_iterator_t it =
      esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  while (it != nullptr) {
    const esp_partition_t *p = esp_partition_get(it);
    Serial.printf("  %-9s type %u.%-2u  0x%06X  %7u KB%s\n", p->label,
                  (unsigned)p->type, (unsigned)p->subtype, (unsigned)p->address,
                  (unsigned)(p->size / 1024),
                  (running && p->address == running->address) ? "  <-- running"
                                                              : "");
    it = esp_partition_next(it);
  }
  esp_partition_iterator_release(it);
}

void print_report() {
  const uint32_t now = millis();

  // --- identity -------------------------------------------------------------
  Serial.println();
  Serial.println(F("=============================== diagnostics ================"
                   "==============="));
  Serial.printf("firmware      %s v%s, built %s %s\n", APP_NAME, FW_VERSION,
                __DATE__, __TIME__);

  esp_chip_info_t chip;
  esp_chip_info(&chip);
  Serial.printf("chip          ESP32 rev %u, %u core%s, %u MHz\n",
                (unsigned)chip.revision, (unsigned)chip.cores,
                chip.cores == 1 ? "" : "s", (unsigned)getCpuFrequencyMhz());

  /*
   * Two flash sizes, and they are allowed to differ.
   *
   * getFlashChipSize() reads the size out of the image header the bootloader
   * was flashed with -- which is what board_upload.flash_size in
   * platformio.ini decides -- and esp_flash_read_id() asks the chip itself.
   * A board configured for 4 MB with a 16 MB chip on it boots perfectly and
   * silently wastes three quarters of its flash, and that is exactly the
   * mistake this line exists to make visible.
   */
  uint32_t jedec = 0;
  if (esp_flash_read_id(nullptr, &jedec) != ESP_OK) jedec = 0;
  // The low byte of the JEDEC id is log2 of the capacity in bytes on every part
  // these modules carry, so 0x18 is 16 MB. Decoded by hand rather than through a
  // helper, because the IDF has moved that API more than once and this file is
  // the last place that should stop compiling across a platform bump.
  const uint32_t chipBytes = jedec ? (1u << (jedec & 0xFF)) : 0;
  Serial.printf("flash         %u MB configured",
                (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
  if (chipBytes >= 1024u * 1024u) {
    Serial.printf(", %u MB on the chip%s",
                  (unsigned)(chipBytes / (1024 * 1024)),
                  chipBytes == ESP.getFlashChipSize()
                      ? ""
                      : "   <-- MISMATCH: see board_upload.flash_size");
  }
  Serial.printf(" @ %u MHz\n", (unsigned)(ESP.getFlashChipSpeed() / 1000000));

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  Serial.printf("mac           %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1],
                mac[2], mac[3], mac[4], mac[5]);

  // --- this boot ------------------------------------------------------------
  Serial.printf("reset reason  %s%s\n", reset_reason(esp_reset_reason()),
                power_woke_from_sleep() ? " (woke from standby)" : "");
  Serial.print("uptime        ");
  print_uptime(now);
  Serial.println();

  struct tm clock_now;
  soft_clock_now(&clock_now);
  Serial.printf("clock         %04d-%02d-%02d %02d:%02d:%02d, source %s, utc%+ld min\n",
                clock_now.tm_year + 1900, clock_now.tm_mon + 1, clock_now.tm_mday,
                clock_now.tm_hour, clock_now.tm_min, clock_now.tm_sec,
                soft_clock_source_name(), (long)soft_clock_utc_offset_min());

  // --- memory ---------------------------------------------------------------
  /*
   * Three numbers, because they fail in three different ways. Free heap is what
   * is left now; the minimum is how close this boot has ever come to running
   * out, which is the one a peak that happened an hour ago shows up in; and the
   * largest contiguous block is what actually decides whether a TLS handshake
   * or a Bluedroid bring-up can be allocated at all. A heap with 60 KB free in
   * 2 KB pieces will refuse both while looking perfectly healthy.
   */
  Serial.println(F("--- memory -----------------------------------------------"
                   "---------------"));
  const uint32_t free_now = ESP.getFreeHeap();
  const uint32_t min_free = ESP.getMinFreeHeap();
  const uint32_t largest = ESP.getMaxAllocHeap();
  Serial.printf("  heap free   %6u bytes\n", (unsigned)free_now);
  Serial.printf("  heap min    %6u bytes  (lowest this boot)\n", (unsigned)min_free);
  Serial.printf("  largest blk %6u bytes  (%u%% of free -- fragmentation)\n",
                (unsigned)largest,
                free_now ? (unsigned)(largest * 100 / free_now) : 0);
  Serial.printf("  internal 8b %6u bytes free, %u largest\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                           MALLOC_CAP_8BIT));
  Serial.printf("  DMA-capable %6u bytes free  (I2S descriptors live here)\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
  // No PSRAM on a WROOM-32D, and saying so is worth one line: every third
  // ESP32 answer on the internet assumes there is some.
  Serial.println(F("  psram       none (WROOM-32D has no external SPI RAM)"));

  Serial.println(F("--- task stacks (smallest free ever) ----------------------"
                   "---------------"));
  print_stack("loopTask");
  print_stack("ui");
  print_stack("leds");
  print_stack("dfplayer");
  print_stack("net_audio");
  print_stack("dlna");
  Serial.printf("  tasks       %u running\n",
                (unsigned)uxTaskGetNumberOfTasks());

  // --- what is fitted -------------------------------------------------------
  /*
   * Every one of these is optional and every one of them fails by being
   * quietly absent rather than by complaining, which is the whole design -- so
   * this is the list that answers "why is nothing happening on the ring".
   */
  Serial.println(F("--- peripherals detected at boot -------------------------"
                   "---------------"));
  Serial.printf("  OLED        %s (SDA %d, SCL %d, %u kHz)\n",
                ui_present() ? "found" : "not found", PIN_OLED_SDA, PIN_OLED_SCL,
                (unsigned)(OLED_BUS_HZ / 1000));
  Serial.printf("  DS3231 RTC  %s\n", soft_clock_rtc_state_name());
  Serial.printf("  WS2812 ring %s (%u px on GPIO%d, cap %u/255)\n",
                leds_present() ? "driving" : "off", (unsigned)LED_COUNT,
                (int)PIN_LEDS, (unsigned)LED_BRIGHTNESS_MAX);
  Serial.printf("  battery     %s\n",
                battery_present() ? "reading" : "no cell / gauge off");
#if BATTERY_ENABLED
  if (battery_present()) {
    BatteryStatus b;
    battery_snapshot(&b);
    Serial.printf("              %.3f V pack, %.3f V/cell, %u%%, %s, pin %u mV\n",
                  b.volts, b.cellVolts, (unsigned)b.percent,
                  battery_state_name(b.state), (unsigned)b.millivoltsAtPin);
    Serial.printf("              divider %.2f, trim %.4f, charger pins %s\n",
                  b.divider, b.calibration,
                  b.haveChargePins ? "wired" : "not wired (charging unknown)");
  }
#endif

  DfStatus df;
  const bool df_fresh = df_player_snapshot(&df);
  Serial.printf("  DFPlayer    %s\n",
                !df_player_running() ? "driver not running in this mode"
                : !df_fresh          ? "status locked, try again"
                : df.asleep          ? "standby"
                : df.online          ? "online"
                                     : "OFFLINE (check TX/RX)");
  if (df_fresh && df_player_running()) {
    Serial.printf("              frames sent %u, good %u, bad %u, errors %u, "
                  "offline %u\n",
                  (unsigned)df.framesSent, (unsigned)df.framesGood,
                  (unsigned)df.framesBad, (unsigned)df.errors,
                  (unsigned)df.offlineEvents);
    Serial.printf("              source %s, track %u/%u, media%s%s%s%s\n",
                  df_source_name(df.source), (unsigned)df.track,
                  (unsigned)df.totalTracks, df.sdPresent ? " sd" : "",
                  df.usbPresent ? " usb" : "", df.flashPresent ? " flash" : "",
                  df.pcLink ? " pc" : "");
    if (df.error[0]) Serial.printf("              last error: %s\n", df.error);
  }
  Serial.printf("  I2C errors  %u (DS3231 transactions that did not complete)\n",
                (unsigned)soft_clock_i2c_errors());

  // --- radio and audio ------------------------------------------------------
  Serial.println(F("--- radio and audio --------------------------------------"
                   "---------------"));
  const RadioMode mode = management_radio_mode();
  Serial.printf("  mode        %s\n", management_mode_name(mode));
  if (radio_mode_has_wifi(mode)) {
    const bool up = WiFi.status() == WL_CONNECTED;
    Serial.printf("  wifi        %s", up ? WiFi.SSID().c_str() : "not connected");
    if (up) {
      Serial.printf(", %s, rssi %d dBm, ch %d", WiFi.localIP().toString().c_str(),
                    (int)WiFi.RSSI(), WiFi.channel());
    }
    Serial.printf("\n  setup ap    %s\n",
                  management_ap_running() ? "up" : "down");
  }
  if (radio_mode_has_ble(mode)) {
    Serial.printf("  ble         %s, %u client(s)\n",
                  ble_control_running() ? "advertising" : "off",
                  (unsigned)ble_control_clients());
    Serial.printf("  net player  %s\n", net_audio_running() ? "running" : "off");
  }

  PlayerInfo info;
  ps_snapshot(&info);
  Serial.printf("  connected   %s%s%s\n", yes_no(info.connected),
                info.peer[0] ? " -- " : "", info.peer);

  /*
   * What the analyser is hearing, which is the honest answer to "is anything
   * playing" and the input every idle timer in the firmware keys off. Silence
   * reads at the floor (-78 dBFS); anything anybody is listening to reads well
   * above -70. A speaker that will not blank its panel or rest its ring is
   * asking this question, and the number here settles it.
   */
  const uint32_t heard = audio_probe_last_active();
  Serial.printf("  audio peak  %.1f dBFS (silence floor is -78; above -70 counts "
                "as playing)\n", audio_probe_peak_db());
  if (heard) {
    Serial.printf("  last heard  %u ms ago\n", (unsigned)(now - heard));
  } else {
    Serial.println(F("  last heard  never, this boot"));
  }
  Serial.printf("  i2s         %d Hz, 16-bit stereo on BCLK %d / LRCK %d / DOUT %d\n",
                (int)info.sample_rate, PIN_MAP_I2S_BCLK, PIN_MAP_I2S_LRCK,
                PIN_MAP_I2S_DOUT);

  // --- power ----------------------------------------------------------------
  Serial.println(F("--- power ------------------------------------------------"
                   "---------------"));
  Serial.printf("  saving      %s -- %s\n", yes_no(power_saving()),
                power_reason());
  Serial.printf("  standby     %s", power_sleep_possible()
                                        ? "available on BOOT"
                                        : "unavailable (no wake button)");
  Serial.printf(", idle for %u s\n", (unsigned)(power_idle_ms() / 1000));
  Serial.printf("  status led  %s, muted %s\n",
                ui_present() ? "GPIO2 pattern" : "GPIO2 pattern (only indicator)",
                yes_no(status_led_muted()));

  // --- flash layout ---------------------------------------------------------
  Serial.println(F("--- partitions -------------------------------------------"
                   "---------------"));
  print_partitions();
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (running) {
    Serial.printf("  app image   %u KB of %u KB in %s (%u%% used)\n",
                  (unsigned)(ESP.getSketchSize() / 1024),
                  (unsigned)(running->size / 1024), running->label,
                  (unsigned)(ESP.getSketchSize() * 100 / running->size));
  }
  Serial.println(F("==========================================================="
                   "==============="));
}

}  // namespace

bool diagnostics_command(const char *line) {
  if (strcmp(line, "diag") != 0 && strcmp(line, "diagnostics") != 0) {
    return false;
  }
  print_report();
  return true;
}
