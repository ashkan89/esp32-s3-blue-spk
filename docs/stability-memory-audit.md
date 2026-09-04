# Stability and memory audit

Audit date: 2026-09-04

Target: classic dual-core ESP32, ESP32-WROOM-32D, 16 MB flash, no PSRAM,
PCM5102A I2S output. This report covers the repository as built with
pioarduino 55.03.311, Arduino-ESP32 3.3.11 and ESP-IDF libraries 5.5.5.

## Result and evidence boundary

All three declared PlatformIO environments compile and link. Host regression
tests exercise pin conflicts, settings-backup coverage and encryption, Arabic
shaping, and the production policy functions used for memory hysteresis,
alarms, sleep deadlines, history/ring wrap, mode cycling, URL validation,
metadata bounds, reconnect backoff, volume mapping and PCM saturation. The
numerical EQ suite additionally sweeps 1,000 designs across eight sample rates,
all five bands and the full supported gain range.

No physical device, UART log, panic dump, matching historical ELF, or core dump
was available in the repository. Consequently:

- linked image, partition and static-memory results below are measured;
- runtime allocation figures marked `recorded` come from the existing source's
  prior on-device measurements and current constants;
- flash capacity is configured as 16 MB but was not read from a chip in this
  audit;
- WROOM-32D has no PSRAM by module specification, and diagnostics now also
  report the runtime probe rather than assuming;
- no historical panic can honestly be decoded or declared fixed. The defects
  below are concrete code-level failure mechanisms, not invented attribution
  of an unavailable crash.

The `diag` command and `/api/status` make the first hardware run decisive. The
diagnostic image reports reset reason, current and prior-boot lifecycle event,
mode/source, heap free/minimum/largest block, internal/DMA/executable memory,
runtime PSRAM detection, stack high-water marks, queue current/peak depth,
radio buffer/underruns/reconnects/bytes, RSSI, uptime and partitions.

## Reproducible baseline

The worktree was clean on `main` before changes. There is no `AGENTS.md` in the
tree. The baseline build matrix completed without compile errors:

| Environment | Static RAM | OTA-slot image | App-slot use | `.bin` |
|---|---:|---:|---:|---:|
| `esp32dev` | 89,436 B | 2,853,680 B | 43.5% | 2,897,168 B |
| `release` | 89,284 B | 2,763,912 B | 42.2% | 2,806,176 B |
| `release-verbose` | 89,284 B | 2,776,372 B | 42.4% | 2,817,392 B |

Baseline diagnostic sections were `.dram0.data` 30,673 B, `.dram0.bss`
58,760 B, `.iram0.vectors` 1,028 B, `.iram0.text` 129,283 B,
`.flash.text` 1,833,260 B and `.flash.rodata` 859,436 B. IRAM therefore used
130,311 of 131,072 bytes, leaving only 761 bytes. IRAM, not flash, is the
link-time constraint.

The only compiler warning was `SERIAL_LOG` being redefined in
`release-verbose`. That environment now removes the inherited definition before
setting it to 1. PlatformIO also prints non-code warnings about Windows long
paths and terminal codepage. `pio check` was sampled but is not a valid gate for
this project: it recursively diagnoses third-party sources with their own
configuration and produced dependency false positives. Builds plus focused
tests are the release gates.

Build identity:

| Item | Value |
|---|---|
| Board definition | generic `esp32dev`; its banner says 4 MB |
| Effective flash override | `board_upload.flash_size = 16MB` |
| Partition table | Arduino `default_16MB.csv` |
| CPU / flash | 240 MHz; release uses 80 MHz DIO |
| Base flags | `CORE_DEBUG_LEVEL=1`, `USE_DS3231=1`; PSRAM erratum flags removed because this target has no PSRAM |
| Release flags | debug level 0, `NDEBUG`, serial/console/diagnostics off, `-fno-math-errno` |
| AudioTools | 1.2.5 (`2390c44`) |
| ESP32-A2DP | 1.8.11 (`b6da474`) |
| libhelix | 0.9.4 (`6183305`) |
| U8g2 / ArduinoJson / PubSubClient | 2.36.18 / 7.4.3 / 2.8.0 |

The effective 16 MB partition map is:

| Partition | Offset | Size |
|---|---:|---:|
| NVS | `0x009000` | 20 KiB |
| OTA data | `0x00e000` | 8 KiB |
| app0 | `0x010000` | 6.25 MiB |
| app1 | `0x650000` | 6.25 MiB |
| SPIFFS | `0xc90000` | 3.375 MiB |
| core dump | `0xff0000` | 64 KiB |

The generic board banner does not override these project settings. On hardware,
`diag` compares the image-configured flash size with the JEDEC capacity and
marks a mismatch.

## Architecture and ownership

Mode switching is a centralized, boot-time resource manager. The requested
mode and a boot-failure strike are committed to NVS, a software restart is
scheduled, and the new boot starts only that mode's radio stack. This is
intentional: live teardown of Bluedroid, Wi-Fi tasks, controller memory and I2S
DMA is more failure-prone, while reset provides deterministic reclamation.
ESP32-A2DP's `end()` also clears pairing state. Two failed non-Wi-Fi boots cause
a fallback to management mode so the dashboard remains reachable. A major-event
breadcrumb is committed to RTC no-init memory with checked magic ordering so a
reset during the write cannot make a torn record look valid.

Application task map:

| Owner/task | Core | Priority | Stack | Lifetime / blocking rule |
|---|---:|---:|---:|---|
| Arduino `loopTask` | 1 | framework default | framework default (normally 8 KiB) | persistent; web, MQTT, controls, telemetry |
| `ui` | 0 | 1 | 5,120 B | only with OLED; I2C rendering cannot delay audio |
| `leds` | 0 | 1 | 4,096 B | only with enabled ring; RMT transmit, late frames are disposable |
| `dfplayer` | 0 | 1 | 4,096 B | DFPlayer mode; sole UART2 owner, 24-command queue |
| `radio` | 1 | 2 | 10,240 B | created on first station, then persistent; decode paced by I2S |
| `github_ota` | 0 | 1 | 16,384 B | one update/check; all C++ owners unwind before self-delete |
| `reboot` | any | `configMAX_PRIORITIES-2` | 2,048 B | short, one-shot delayed restart |
| Bluedroid/BTC/BTU/audio | framework | framework | framework | Bluetooth mode only; observed by `diag` |

Voice has no task: exactly one of the current PCM audio task or `loopTask` owns
its decoder. The web server and PubSubClient run synchronously on `loopTask`;
there is no WebSocket/SSE task or unbounded client queue. One HTTP request is
processed at a time. The documented hardware validation limit is two polling
dashboard pages; more tabs are not a production workload on this RAM budget.

## Feature/mode matrix

| Feature | Wi-Fi management/radio | Bluetooth A2DP | DFPlayer + Wi-Fi |
|---|---|---|---|
| Wi-Fi, dashboard, OTA | yes | deliberately off | yes |
| MQTT / HA discovery | yes | off with Wi-Fi | yes |
| Bluetooth Classic A2DP | off; BT memory released | yes | off; BT memory released |
| Internet MP3/AAC radio | yes | unavailable | yes, pauses DFPlayer first |
| DFPlayer UART/analog source | off | off | yes |
| ESP32 five-band PCM EQ | radio, chime, voice | A2DP, chime, voice | radio, chime, voice only |
| DFPlayer hardware EQ | n/a | n/a | yes, six module presets |
| Spectrum/FFT from music | radio | A2DP | not from DFPlayer analog audio |
| OLED, battery, alarms, voice, history, sleep | yes | yes (without dashboard) | yes |

The DFPlayer's analog output never enters ESP32 memory, so the software EQ and
FFT cannot process it. Claiming otherwise would be a hardware error; the module's
own EQ remains available. Alarms fall back to the built-in I2S chime when their
selected source cannot operate in the active mode.

## Static and dynamic memory budget

Final development-image sections are `.dram0.data` 30,737 B and `.dram0.bss`
58,944 B.
The major application-owned entries are:

| Resource | Size / bound | Capability | Lifetime and owner |
|---|---:|---|---|
| Audio sample/analyser ring | 2,048 B | internal static DRAM | whole boot; lock-free PCM producer |
| FFT real/imag/window/twiddles/indexes | about 4.6 KiB plus bands | internal static DRAM | whole boot; UI/LED analysis under one mutex |
| OLED current/previous/wave framebuffers | 3 × 512 B plus U8g2 state | internal static DRAM | whole boot |
| EQ coefficients and stereo state | under 350 B | internal static DRAM | whole boot; audio-task-owned active copy/state |
| RMT symbols | 672 B | internal static DRAM | whole boot |
| Voice PCM/chime scratch | 2 × 512 B | internal static DRAM | whole boot, non-overlapping uses |
| Spoken clips | about 355 KiB | immutable flash | whole image, never copied to RAM |
| Gzip dashboard | about 50 KiB | immutable flash | served directly |
| I2S DMA audio buffers | 18,432 B plus descriptors | internal DMA-capable heap | whole boot; I2S driver |
| Telemetry history | 120 × 12 = 1,440 B | general internal heap | once at boot; fixed two-hour ring |
| Radio favourites | 12 × 200 = 2,400 B | general internal heap | Wi-Fi modes, once at boot |
| Radio stream arena | 24,424 B | `INTERNAL | 8BIT` heap | one allocation per live connection, always freed |
| MP3/AAC decoder | recorded about 30 KiB plus about 7 KiB frame/PCM | internal heap | one live stream, destroyed before arena release |
| Radio task stack | 10,240 B | internal task stack | first use until reboot |
| MQTT packet buffer | 1,280 B | internal heap | broker client, bounded by PubSubClient |
| DF command queue | 24 bounded commands plus queue control | internal heap | DF mode; freed on partial startup failure |
| Voice queue | four pending clips | static DRAM | bounded; current and peak depth reported |
| Update TLS/task | recorded about 45 KiB TLS + 16,384 B stack | internal heap | one check/install, mutually exclusive with radio |
| HTTP request + ArduinoJson tree | 12,288 / 8,192 / 4,096 B normal/constrained/critical ceiling | internal heap | one synchronous request |

AudioTools 1.2.5 translates `buffer_count=6`, `buffer_size=512`, stereo 16-bit
to 768 frames per descriptor while IDF leaves `dma_desc_num=6`. Therefore the
correct DMA storage is `768 × 4 × 6 = 18,432` bytes and latency is about 104 ms
at 44.1 kHz. Earlier documentation counted only one descriptor (3,072 B). The
timing was correct and remains unchanged.

The radio's ring (20,480 B), socket read chunk (2,920 B) and decoder feed
(1,024 B) share one capability-aware 24,424-byte arena. This avoids three
differently sized holes on every reconnect. The stream object, TLS client,
decoder wrapper and codec are destroyed on every exit path. The task stack is
kept after first use because repeated task teardown would add lifecycle races;
its cost is visible and bounded.

Large JSON responses no longer create a second full `String`: normal API and
settings-backup responses measure the document, send Content-Length, and
serialize directly to the connected client. Telemetry was already emitted in
chunks. Home Assistant discovery builds and publishes one entity document at a
time (three per loop pass), rather than retaining the entire entity set.

## Hot paths

| Path | Frequency/deadline | Allocation and blocking finding |
|---|---|---|
| A2DP PCM callback | every received audio block | integer volume, EQ, voice mix and sample tap only; no allocation, network, I2C, JSON or mutex wait |
| EQ | five biquads × two channels × sample rate when active | coefficients/trigonometry outside audio; immutable snapshot adopted once per block; no hot-path lock/allocation |
| Voice mix | per audio frame only while queued/duck release | ADPCM/resample integer work; owner and cancellation adopted at buffer boundary |
| Radio receive/decode/output | dedicated priority-2 task | bounded 2-MTU reads and 1,024-byte decode chunks; blocking I2S write is the pacing clock |
| PCM sample tap | per stereo frame | downmix, peak/square accumulation and ring write only; FFT is elsewhere |
| FFT/spectrum | requested at 30 FPS OLED / 60 FPS LEDs, shared result | one analysis mutex; recomputation throttles to 2×/4× interval under pressure |
| OLED | 30 FPS, core 0 priority 1 | full-buffer I2C, never on audio task |
| LED ring | 60 FPS, core 0 priority 1 | bounded RMT frame; late optional frames may be skipped |
| MQTT | loop task, configured publish interval | 1,280-byte bounded packets; reconnect backoff; discovery split across passes |
| Dashboard | one request on loop task | body bounded before JSON tree; output streamed; can delay UI/control but not audio tasks |
| Alarm fade / telemetry | 100 ms / 60 s | constant-time arithmetic; timer comparisons rollover safe |

No watchdog, assertion, brownout detector, radio or feature was disabled. No
new logging occurs inside the per-sample audio callback.

## Concrete defects repaired

| Defect | Failure mechanism | Repair |
|---|---|---|
| EQ publication and low-rate bands | two rapid cross-core writers could wrap to and overwrite coefficients still read by an audio block; sample-rate setter also cleared live state from another task; the 12 kHz corner exceeded Nyquist on 8/16 kHz content | sequence-checked immutable publication; generation rejects stale designs; consumer copies and clears state only between blocks; corners cap at 45% of sample rate |
| Voice shared decoder/config/queue | `volatile` did not synchronize cores; dequeue and `voice_silence()` could race ADPCM state used by A2DP/radio | atomic packed configuration/status, locked bounded producer queue, explicit decoder owner, owner-serviced cancellation/rate changes, four real queue slots |
| Radio command structure | play/name/URL/version were observable as different requests and rapid commands could be lost | publisher lock plus seqlock snapshot; stream exits on version change and adopts newest complete request |
| Player-state seqlock writers | Bluetooth callbacks and `loopTask` could overlap writes, letting an even sequence value appear while one writer was still active | a short writer spinlock now serializes all model updates; UI/API readers still take lock-free stable snapshots |
| Cross-core radio/DF status | `volatile` enum/bool reads had no C++ memory-order guarantee | acquire/release single-word publications; compound status still uses its mutex |
| Optional-device/task allocation failure | partially created DF queue/mutex/UART or RMT encoder/channel, and failed UI/LED tasks, could leak resources or remain in a misleading state | check all creation results, delete partial resources, close UART/RMT and mark the optional device unavailable |
| UI/alarm/clock/LED shared state | plain or `volatile` words and compound alarm data crossed between core-0 UI/LED tasks and core-1 loop without synchronization | acquire/release scalar publication and short critical-section snapshots; external work remains outside locks |
| Stack diagnostic units | ESP-IDF returns high-water mark in bytes, but report multiplied by `sizeof(StackType_t)` and overstated every margin 4× | report native byte value; add radio, OTA and Bluetooth/audio task names |
| HTTP/JSON peak | response document plus a growing serialized `String` duplicated large payloads | serialize directly to client and reject oversized input before creating a second JSON representation |
| Reconnect synchronization | identical speakers retried in lockstep | bounded exponential backoff with ±25% jitter and host-tested bounds |
| Radio full-volume math | `(sample * 127) >> 7` attenuated the supposedly transparent maximum | explicit 127 bypass; saturation and representative gains host tested |
| Metadata, UTF-8 and URL edges | nullable AVRCP text reached C parsing; malformed/truncated UTF-8 could advance incorrectly; URL schemes and ICY copy limits lacked focused tests | null guard plus semantic UTF-8 sequence validation and production constexpr URL/copy bounds |
| Sleep extension | repeated additions could exceed the declared 600-minute range and eventually invalidate rollover assumptions | cap total duration and use one tested rollover-safe remaining-time primitive |
| Release-verbose flags | inherited `SERIAL_LOG=0` and added `=1`, warning on every unit | remove inherited definition before adding the verbose value |
| Failure forensics | reset reason alone could not say what major subsystem was last active | checked RTC breadcrumb plus current/prior lifecycle fields in serial/API diagnostics |

These defects can plausibly produce stale commands, corrupted audio, incorrect
status, allocation failure, or prohibited-load/heap symptoms. With no captured
panic, they are not assigned to a historical exception address.

## Panic classification guide

| Reported class | Evidence to collect / interpretation in this firmware |
|---|---|
| `LoadProhibited` / `StoreProhibited` | decode the PC against the exact ELF; inspect null/stale pointers and queue/decoder ownership. The known cross-core structures above are now synchronized. |
| `InstrFetchProhibited`, illegal instruction | suspect corrupted return/function pointer or stack first; no evidence was found in-tree. |
| Stack overflow | use byte-correct per-task high-water marks and the task named by the panic; no stack was changed without a hardware watermark. |
| Interrupt watchdog / cache-disabled access | no filesystem, logging, float design, I2C or network call exists in application ISR/audio sample paths; capture the ISR/backtrace before changing watchdog settings. |
| Task watchdog | synchronous HTTP/MQTT and I2S calls have timeouts or block/yield in their owning task. Record the task; do not feed or disable the watchdog as a workaround. |
| MultiHeap corruption / double free | enable heap integrity diagnostics in a dedicated SDK-config debug build and inspect the first corrupt address; radio teardown order and partial DF startup cleanup were audited. |
| FreeRTOS assert | record file/line. Radio and OTA reject before known fatal low-heap socket/TLS allocations; queue/semaphore creation is checked. |
| Brownout | separate power-integrity failure. Check the 5 V rail/cable during RF and audio current peaks; do not label it a heap defect. |
| Software restart | expected for mode switch, OTA or requested reboot; prior lifecycle event distinguishes them. |

The partition table reserves `coredump`, but the precompiled Arduino/IDF
configuration was not proven to write core dumps. Enabling flash core dumps and
heap poisoning requires a dedicated SDK-config/debug core build and must be
validated for this exact pioarduino release. The serial exception decoder and
exact `.pio/build/<env>/firmware.elf` remain the immediate backtrace path.

## Memory-pressure policy

The policy samples free heap and largest 8-bit allocation every 500 ms. It uses
hysteresis and never deletes tasks or reclaims an active decoder/DMA buffer:

| Transition | Free heap / largest block |
|---|---|
| normal → constrained | below 90,000 B or 45,000 B |
| any non-critical → critical | below 70,000 B or 28,000 B |
| critical → constrained | at least 82,000 B and 36,000 B |
| constrained → normal | at least 105,000 B and 52,000 B |

The values are tied to the recorded operation gates: a plain radio connection
requires 70,000 B free and a 26,000 B block; OTA/TLS requires 80,000 B and a
45,000 B block. Under constrained/critical pressure optional FFT refresh backs
off by 2×/4×. JSON bodies are capped at 12,288 B normally, 8,192 B while
constrained and 4,096 B when critical, returning HTTP 413 rather than attempting
another large tree. Active
audio, I2S DMA, stacks, queues and synchronization objects are never reclaimed.
History remains 120 one-minute samples, preserving the two-hour requirement.

## Final measurements

The final table is filled from clean final builds after the generated dashboard
is refreshed:

| Environment | Static RAM | OTA-slot image | App-slot use | `.bin` | Change from baseline |
|---|---:|---:|---:|---:|---:|
| `esp32dev` | 89,588 B | 2,860,972 B | 43.7% | 2,905,264 B | +152 B RAM, +7,292 B image |
| `release` | 89,452 B | 2,769,820 B | 42.3% | pending final rerun | +168 B RAM, +5,908 B image |
| `release-verbose` | pending final rerun | pending final rerun | pending | pending | pending |

IRAM is unchanged at 130,311 bytes, leaving 761 bytes. The observability and
ownership fixes intentionally cost a small amount of static state and flash;
they do not pretend to create runtime heap. Peak RAM is reduced by one complete
serialized JSON response because ordinary and backup JSON are no longer copied
into a second `String`. The exact saving equals the response length and varies
with station/alarm/settings data. The radio working buffers remain one 24,424 B
allocation and all reconnect paths release it.

No task stack was reduced or increased: without real high-water marks, either
would be guesswork. The diagnostic correction may make existing margins appear
four times smaller than earlier firmware reported; that is the accurate byte
value, not a regression.

## Automated verification

Run from the repository root:

```powershell
C:\p\penv\Scripts\pio.exe run -e esp32dev
C:\p\penv\Scripts\pio.exe run -e release
C:\p\penv\Scripts\pio.exe run -e release-verbose
C:\Espressif\tools\python\python.exe scripts\test_pin_check.py
C:\Espressif\tools\python\python.exe scripts\test_settings_backup.py
C:\Espressif\tools\python\python.exe scripts\test_arabic_shaping.py
C:\Espressif\tools\python\python.exe scripts\gen_arabic_tables.py
C:\Espressif\tools\python\python.exe scripts\test_stability_policy.py
C:\Espressif\tools\python\python.exe scripts\test_audio_eq.py
git diff --check
```

The stability test compiles the actual dependency-free production header with
the installed Xtensa compiler and `-Wall -Wextra -Werror`, using compile-time
assertions. The EQ test evaluates the production constants and cookbook math,
but audible hardware response, optional-device electrical failure, TLS
allocation behaviour and task scheduling cannot be executed on the host and
remain in the soak plan.

## Hardware soak plan and pass/fail gates

Use `esp32dev`, save its matching ELF, and collect `diag` at the start, after
warm-up, at each workload boundary and at the end. Record free/minimum/largest
heap, capability heaps, every stack margin, queue peaks, reset/lifecycle reason,
RSSI, radio buffer/underruns/reconnects and audible faults.

1. Play Bluetooth audio + EQ + spectrum/OLED for at least two hours. Exercise
   50 connect/disconnect cycles, metadata changes, chimes, alarms, announcements
   and sleep cancel/expiry.
2. Play stable-LAN MP3 for two hours, then AAC for two hours. Keep two dashboard
   pages polling; run MQTT/HA discovery and broker reconnects. Repeat with weak
   Wi-Fi, AP loss, stalled server, malformed URL and unsupported/corrupt stream.
3. Exercise DFPlayer SD and USB, dashboard control, alarms and sleep. Boot and
   run with DFPlayer absent/unresponsive, OLED absent, battery input absent and
   an I2C device disconnected during use.
4. Perform at least 100 controlled mode changes. Each expected reset must report
   software restart and the prior event `mode switch`; no boot sentinel fallback
   is allowed for a healthy mode.
5. Inject allocation failures before radio, DF and UI creation in a debug build.
   Oversized JSON must return 413. OTA while radio is active must be refused;
   after stopping radio it must meet the 80,000/45,000 B gate and complete.
6. Cross a `millis()` rollover in accelerated/debug timing or keep a unit up for
   50 days; verify alarm, snooze, sleep, telemetry, reconnect and UI timers.

Measurable pass criteria:

- zero unexpected resets, Guru Meditation messages, asserts or watchdogs;
- zero audible wraparound/clipping at volume 127 with flat EQ; boosted EQ may
  engage the documented soft knee but must not integer-wrap;
- on a stable LAN, zero radio underruns over each two-hour run; under induced
  loss, buffering/reconnect recovers and the arena is released between tries;
- after a 10-minute warm-up and return to the same idle mode, free heap may not
  drift downward by more than 2,048 B and largest block by more than 4,096 B
  across a two-hour run or the 100-switch campaign;
- every radio start sees at least 70,000 B free / 26,000 B largest; every OTA
  start sees at least 80,000 B / 45,000 B;
- no application task high-water mark below 400 B; 400–767 B requires review
  before release rather than an automatic stack reduction;
- voice peak stays below its four-pending capacity and DF queue peak below 24
  during ordinary use; reaching a bound must produce a clean refusal/drop, not
  corruption or blocking audio;
- I2S remains 16-bit stereo at the decoder/A2DP-reported rate, APLL enabled,
  six DMA descriptors, with no competing writer.

## Concise manual validation checklist

- [ ] `diag`: configured and JEDEC flash are both 16 MB; PSRAM says not detected.
- [ ] Each mode boots twice, clears its strike, and exposes only its matrix resources.
- [ ] Bluetooth: full-volume sine/music does not wrap; EQ changes during 44.1/48 kHz audio do not click/crash.
- [ ] Announcements and alarm interrupt/duck Bluetooth and radio; silence/cancel is immediate and clean.
- [ ] MP3 and AAC show correct codec/rate/channels/ICY metadata; mono reaches both DAC channels.
- [ ] Pull Wi-Fi and stall the server; buffer, backoff+jitter and recovery behave without heap drift.
- [ ] Two dashboard clients plus MQTT discovery do not interrupt audio; oversized JSON gets 413.
- [ ] DFPlayer missing and OLED/battery/I2C missing or failed do not delay boot or flood logs.
- [ ] Sleep fade/cancel/extend and weekday/one-shot alarms work before and after timer rollover.
- [ ] 100 mode switches show expected software-reset breadcrumbs and stable post-boot heap.
- [ ] OTA is refused during streaming, succeeds when memory gates pass, and moves to the other slot.

## Remaining risks

- Hardware runtime heap, largest-block fragmentation trends, real stack margins,
  audio timing and optional-device electrical behaviour remain unmeasured here.
- IRAM has only 653 bytes spare in `esp32dev` and 997 bytes in both release
  variants. Avoid any dependency or option that marks more code IRAM without
  measuring the map; an otherwise small library update can make the image fail
  to link.
- HTTPS radio deliberately calls `setInsecure()`: audio is encrypted but server
  identity is not authenticated. OTA continues to use the Mozilla trust bundle.
- A synchronous dashboard has one in-flight request. Two polling pages are the
  validation target; large numbers of browser tabs are outside the RAM budget.
- The core-dump partition alone does not prove core-dump writing is enabled in
  the precompiled Arduino core. Preserve UART output and the matching ELF until
  a dedicated SDK-config debug build is validated.
- The 16 MB capacity and absence of PSRAM still require one on-device `diag`
  capture to become runtime evidence rather than target/configuration evidence.

## Authoritative references

Version-specific behaviour was checked against installed sources in
`.pio/libdeps` and the framework packages. Relevant upstream references:

- Espressif [heap capabilities](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/mem_alloc.html)
  and [heap debugging](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/heap_debug.html): capability pools, minimum free heap and largest free block.
- Espressif [IDF FreeRTOS additions](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/freertos_idf.html): ESP-IDF stack sizes and high-water marks are bytes, unlike upstream FreeRTOS words.
- Espressif [I2S driver](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/i2s.html): DMA descriptor/frame sizing and blocking write behaviour.
- Espressif [reset reasons](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/misc_system_api.html)
  and [core dumps](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/core_dump.html): distinguish panic/watchdog/brownout and configure dump storage.
- Espressif [minimizing RAM usage](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/performance/ram-usage.html).
- PlatformIO [Espressif32 options](https://docs.platformio.org/en/stable/platforms/espressif32.html): flash, partition and board overrides.
- AudioTools [v1.2.5 source](https://github.com/pschatzmann/arduino-audio-tools/tree/v1.2.5)
  and [performance notes](https://github.com/pschatzmann/arduino-audio-tools/wiki/Performance).
- ESP32-A2DP [v1.8.11 source](https://github.com/pschatzmann/ESP32-A2DP/tree/v1.8.11)
  and libhelix [v0.9.4 source](https://github.com/pschatzmann/arduino-libhelix/tree/v0.9.4).
- PubSubClient [README](https://github.com/knolleary/pubsubclient/blob/master/README.md): bounded packet buffer and publish limitations.
