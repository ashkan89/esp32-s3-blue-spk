# esp32-blue-spk

Turns an **ESP32 WROOM-32D** into a Bluetooth audio receiver. Pair your phone,
press play, and the audio comes out of the headphone jack on a **PCM5102A** I2S
DAC board. It chimes when a phone connects or disconnects, and drives the DAC at
full level rather than through the A2DP library's very quiet default curve.

A **0.91" 128×32 OLED** shows what is going on: the track, the phone it came
from, the volume, a clock, and a live spectrum analyser fed from the actual audio
on its way to the DAC. The display is optional — with nothing on the I2C bus the
firmware notices at boot and behaves exactly as it did without one.

## Hardware requirement

This needs a **classic ESP32** (WROOM-32, -32D, -32E, or a WROVER). Bluetooth
Classic — and therefore A2DP — does **not** exist on the ESP32-S3, S2, C3 or C6;
those chips are BLE-only and physically cannot run this sketch, regardless of
what the folder is named.

| Part | Notes |
|------|-------|
| ESP32-WROOM-32D devkit, 16 MB flash | classic ESP32 only; this build uses the full 16 MB layout |
| PCM5102A I2S DAC board | the usual purple breakout with a 3.5 mm jack |
| 0.91" 128×32 I2C OLED  | SSD1306, 4 pins (VCC/GND/SDA/SCL) — optional |
| DS3231 RTC module      | optional, shares the OLED's two I2C wires |

## Wiring

### PCM5102A → ESP32

| PCM5102A | ESP32       | Notes                                            |
|----------|-------------|--------------------------------------------------|
| VIN      | 5V (VIN)    | The board has its own 3.3 V regulator            |
| GND      | GND         |                                                  |
| BCK      | GPIO26      | Bit clock                                        |
| DIN      | GPIO22      | Serial data                                      |
| LCK      | GPIO25      | Word select / LR clock                           |
| SCK      | **GND**     | Required — selects the internal PLL              |
| FMT      | GND         | Standard I2S framing                             |
| XMT      | 3.3V        | Un-mute (usually already pulled high on-module)  |
| FLT      | GND         | Normal latency filter                            |
| DEMP     | GND         | De-emphasis off                                  |

The single most common failure is leaving **SCK floating** — the DAC then waits
for an external master clock and you get silence or noise. Tie it to GND.

Power: the PCM5102A only draws ~20 mA, so USB power off the dev board is fine.
If you hear a hiss or whine that tracks the CPU, give the DAC its own supply and
join the grounds.

### OLED → ESP32

| OLED | ESP32   | Notes                                                      |
|------|---------|------------------------------------------------------------|
| VCC  | **3.3V**| These modules have no regulator — 5 V destroys them        |
| GND  | GND     |                                                            |
| SDA  | GPIO21  |                                                            |
| SCL  | GPIO19  | **not** GPIO22 — see below                                 |

Everyone's ESP32 I2C example uses GPIO21/22, and **GPIO22 is already I2S DIN**
here. Two signals cannot share a pin, so SCL moves to GPIO19, which is otherwise
unused. If you would rather keep the familiar 21/22 pair for I2C, change
`PIN_I2S_DOUT` in [src/main.cpp](src/main.cpp) to GPIO23 and move the DAC's DIN
wire instead — either is fine, but pick one.

Both the address (0x3C, falling back to 0x3D) and the pins are checked at boot;
the serial log says which it found, or says it found nothing:

```
[ui] SSD1306 128x32 at 0x3C, 400 kHz, 30 fps
```

A few 0.91" modules use a Winstar panel with a different column offset, which
shows up as the image being shifted sideways by two pixels. If yours does that,
swap `U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C` for
`U8G2_SSD1306_128X32_WINSTAR_F_HW_I2C` in [src/ui.cpp](src/ui.cpp).

## Windows note: the short toolchain path

The Arduino core 3.x / IDF 5 package contains header paths close to the legacy
260-character Windows limit. To stay clear of it, [platformio.ini](platformio.ini)
pins the toolchain to a short root:

```ini
[platformio]
core_dir = C:/p
```

If you'd rather keep the toolchain in the normal `~/.platformio`, enable long
paths once in an **elevated** PowerShell, reboot, then delete that section:

```powershell
Set-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem" `
  -Name LongPathsEnabled -Value 1 -Type DWord
git config --system core.longpaths true
```

This is Espressif's own recommended setup for ESP-IDF on Windows, not a
workaround specific to this project. Either way, the **first build downloads and
unpacks several GB** and takes roughly 25 minutes; later builds are ~20 seconds.

## Build & flash

```sh
pio run                        # compile
pio run -e esp32dev -t upload  # flash the speaker firmware
pio device monitor             # serial log at 115200
```

Then on your phone: Bluetooth settings → pair with **"esp32-blue-spk"** → play.

## Web dashboard and OTA updates

The normal firmware includes a responsive management console for desktop and
mobile. On its first boot it creates a WPA2 setup network:

| Setting | First-boot value |
|---------|------------------|
| Wi-Fi network | `esp32-blue-spk-XXXXXX` (the suffix is unique to the board) |
| Wi-Fi password | `speaker-setup` |
| Dashboard address | `http://192.168.4.1/` |
| Dashboard user | `admin` |
| Dashboard password | `admin` |

Connect to that network, open the address, sign in, then use **Wi-Fi → Scan
networks** to join the speaker to your normal network. After it connects the
console is available at the IP printed on the serial monitor (and shown in the
dashboard). If saved credentials stop working, the setup network comes
back after 15 seconds, so the speaker cannot be locked out by a router change.

Change both default passwords under **Settings → Identity & access**. The
dashboard uses authenticated HTTP on the local network; it is not intended to
be port-forwarded or exposed directly to the internet. Wi-Fi credentials,
dashboard password, and an optional GitHub token live in NVS on the device.

The console provides:

- live track metadata, progress, link state, memory, uptime, RSSI and OTA state;
- play/pause, stop, previous/next, rewind/fast-forward, mute and absolute volume;
- active-device disconnect plus a list of bonded devices that can be forgotten;
- Wi-Fi scanning/provisioning, automatic recovery AP, hostname and AP controls;
- every OLED screen, carousel, wake and brightness control;
- browser clock sync, device identity, restart, and full factory reset;
- firmware upload and background check/install from the latest GitHub Release.

### Initial OTA migration

This version targets the board's full 16 MB flash and uses two 6.25 MB A/B
application slots plus 3.375 MB reserved filesystem space. The **first
installation must be done over USB** so the 16 MB bootloader header and new
partition table are flashed along with the application:

```sh
pio run -e esp32dev -t upload
```

After that, use **Updates → Upload firmware** with
`.pio/build/esp32dev/firmware.bin`. Do not upload `firmware.factory.bin`; that
combined image is only for a serial flash at address zero. An OTA write always
targets the inactive slot and restarts only after `Update.end()` validates the
complete ESP32 image.

### GitHub Releases updater

Under **Settings → GitHub Releases**, enter `owner/repository` and an asset
pattern such as `*.bin` or `speaker-*.bin`. The updater ignores bootloader,
partition-table, LittleFS and SPIFFS images. Publish the normal PlatformIO
`firmware.bin` as a release asset, then use **Check GitHub** and **Install
release**. Public repositories need no token; a fine-grained token can be saved
for a private repository. GitHub API and release downloads are verified over
TLS against DigiCert Global Root G2 (valid through 2038).

Set the version reported by the dashboard in [src/app_config.h](src/app_config.h)
for each release. A leading `v` in a GitHub tag is ignored during comparison.

Wi-Fi and Bluetooth Classic share one radio. Ordinary dashboard polling is
lightweight, but network scans and firmware downloads consume radio time; the
console warns before installation and pauses playback when writing firmware.

The serial monitor stays deliberately sparse — see the note about serial logging
under Troubleshooting:

```
=== esp32-blue-spk v2.0.0 ===
[ui] SSD1306 128x32 at 0x3C, 400 kHz, 30 fps
[clock] 2026-08-18 14:29:33 (build)
Discoverable as "esp32-blue-spk" - pair from your phone.
Type 'help' for the serial commands (clock, screens).
[bt] connected
[bt] peer: Pixel 8
[avrc] Queen - Bohemian Rhapsody
[bt] disconnected
```

The on-board LED (GPIO2) blinks while waiting for a phone and stays solid once
connected.

## The display

Nine screens. Seven of them rotate on a nine-second carousel, skipping any that
have nothing to say; the other two take over when they apply.

**Now playing** — title, artist, and a progress bar that keeps moving between the
once-a-second position updates AVRCP sends. The third row is a live spectrum
strip while music plays and the album name when it stops.

```
 ▶ Bohemian Rhapsody
 Queen
 ▂▄█▇▅▃▂▄▆█▅▃▂▁▂▃▅▇█▆▄▂▁▂▄▆▅▃▂▁▂
 1:23 ─────────■·············· 5:55
```

**Spectrum** — 32 bands, full bleed, with peak caps that hang and then fall. It
cycles through three styles each time the carousel comes round: bars from the
bottom, mirrored around the centre line, and a discrete LED-matrix look. A kick
drum puts four ticks in the corners for one frame.

```
      ▄█                    █▄        ▄█▄  ▄▄▄  ▄█▄
 ▂▄█▇▅▃▂▄▆█▅▃▂▁▂▃▅▇█▆▄▂▁▂▄▆▅▃▂▁▂      ███  ███  ███
 ██████████████████████████████       ▀█▀  ▀▀▀  ▀█▀
```

**VU** — stereo RMS meters with real ballistics (fast rise, slow fall), peak-hold
needles, and a dB scale underneath.

```
 L ▊▊▊▊▊▊▊▊▊▊▊▊▊·│·············
 R ▊▊▊▊▊▊▊▊▊▊▊▊▊▊▊·│···········
      │    │        │  │ │ │
     -40  -20      -6    0
```

**Scope** — the waveform itself, aligned on a rising zero crossing so a steady
tone sits still instead of sliding sideways, on a dotted graticule.

**Waterfall** — a scrolling spectrogram: one column per frame, one row per band,
intensity by ordered dithering. Time runs right to left, frequency bottom to top.

**Clock** — seven-segment hours and minutes with a colon that blinks once a
second, the date beside it, and the seconds as a sweep along the bottom edge.
The whole layout shifts a pixel sideways as the minutes pass, because OLEDs do
burn in.

```
 ███ ███ ▪ ███ ███   TUE 18
 █ █ █ █ ▪ █ █ █ █   AUG 2026
 ███ ███ ▪ ███ ███   :07
 ▓▓▓▓▓▓▓▓░░░░░░░░░░░░░░░░░░░░░░
```

**Info** — device name, the phone that is connected and at what sample rate, the
volume, and a scrolling line of uptime, free heap, measured frame rate, current
analyser gain and where the clock came from.

**Pairing** (takes over while no phone is connected) — the Bluetooth rune with
beacon rings pulsing out of it, the device name, and a small clock.

**Screensaver** (takes over after five minutes of nothing) — the time in a
rounded box, bouncing around the panel so no pixel stays lit.

### Overlays

Three things interrupt whatever is on screen:

- **System status.** Wi-Fi connection/setup details, the dashboard IP, network
  scans, clock sync, restart/factory reset, and GitHub/browser firmware updates
  take priority. Firmware check, download, upload and install state is visible
  directly on the OLED, including a live percentage bar and success/error state.

- **Volume.** Every AVRCP volume change takes the whole panel for 1.4 s: speaker
  icon, the percentage in 16 px digits, and a segmented bar.
- **Toasts.** Connect, disconnect and track changes get a 2.2 s two-line card.

### Transitions

Screen changes are animated by compositing the previous frame with the new one,
rotating through a horizontal slide, a wipe with a bright leading edge, and a
dithered dissolve. The frame buffer is four 128-byte pages of eight vertical
pixels, so the slide and the wipe are `memmove`s and cost nothing measurable.

### Controls

The **BOOT button** on the dev board (GPIO0) is the only control, and it needs no
wiring — it means nothing to the bootloader unless RESET is held at the same
time.

| Press | Action |
|-------|--------|
| short (< 600 ms) | next screen |
| long (> 600 ms)  | pin the current screen / release it |
| hold (> 2.5 s)   | brightness: low → mid → high |

Set `PIN_UI_BUTTON` to `-1` in [src/ui_config.h](src/ui_config.h) to disable it.

Everything is also reachable from the serial monitor — type `help`:

```
next                   next screen
screen 0..6            hold one screen (0 now playing, 1 spectrum, 2 VU,
                       3 scope, 4 waterfall, 5 clock, 6 info)
auto                   resume the carousel
bright 0..255          fix the contrast (0 = back to automatic)
ui                     current screen, frame rate, style
```

### Where the display gets its data

Two read-only sources, and no path back into the audio:

- **[src/player_state.h](src/player_state.h)** holds the track, the phone, the
  volume and the transport state. AVRCP callbacks write it from the Bluetooth
  task; the display reads it 30 times a second. It is a **seqlock**, not a mutex:
  the writer bumps a counter, writes, bumps it again, and never waits for
  anything — because a mutex here would put the audio task at the mercy of the
  display, and this project's whole noise story is about not blocking in there.
- **[src/audio_probe.h](src/audio_probe.h)** is the analyser. The volume control
  hands it every frame on its way to the DAC; all it does there is downmix,
  average sample pairs down to 22.05 kHz, and store — no FFT, no floats, no logs.
  The UI task does the expensive half: Hann window, 256-point FFT, log-spaced
  banding, auto-gain, peak hold, beat detection.

The renderer runs on **its own task, pinned to core 0 at priority 1**. Pushing one
512-byte frame over I2C blocks for 7–12 ms, and doing that in `loop()` would
fight with the melody playback that also lives there. Down on core 0 at the
bottom of the priority list, it gets preempted by anything that matters, so the
animation degrades instead of the audio.

## The clock

There is no built-in RTC. The quickest accurate option is **Settings → Sync
browser time** in the dashboard. The clock also works without a network and can
come from whichever of these you set up, in this order of preference:

**1. Nothing at all.** The clock is seeded from the build timestamp, so a fresh
flash shows roughly the right time rather than 1 Jan 1970. It drifts, and it is
wrong by however long the firmware sat on disk — the clock screen puts a `?`
after the year to say so, and the info screen shows `clk build`.

**2. Typed in.** Over the serial monitor:

```
time 14:30            set the time, today
time 14:30:00
time 2026-08-18 14:30 set date and time
date 2026-08-18       set just the date
time                  show it, and where it came from
```

**3. A DS3231 module** on the same two I2C wires as the display — the only option
that properly survives a power cut. About a euro, keeps time for years on its
coin cell, no extra GPIO:

```ini
build_flags = ${env.build_flags} -DUSE_DS3231=1
```

It is read at boot and written whenever you set the time by hand, so it only
needs setting once ever. A chip that has lost its cell (which reads back as
2000-01-01) is detected and seeded rather than believed.

**4. One NTP sync at boot**, before the radio starts:

```ini
build_flags =
    ${env.build_flags}
    -DUSE_NTP=1
    '-DWIFI_SSID="yournetwork"'
    '-DWIFI_PASS="yourpassword"'
```

Wi-Fi is brought up, the time is fetched, and Wi-Fi is torn down completely
before `a2dp_sink.start()` is ever called, so the two radios never overlap. Costs
about four seconds of boot time. Set `CLOCK_TZ_OFFSET_MIN` in
[src/ui_config.h](src/ui_config.h) for your timezone (minutes east of UTC; the
other three paths set local time directly and do not need it).

Whichever it is, the current time is written to NVS every ten minutes, so a power
cut comes back within ten minutes rather than back to the build stamp.

`-DCLOCK_24H=0` switches the clock screen to 12-hour with AM/PM.

## Melodies

Three short tone sequences are synthesised in software and written straight into
the same I2S stream the music uses — no sample data, no extra flash:

| When | Melody |
|------|--------|
| Radio up, ready to pair | two blips, A5 → D6 |
| Phone connected | C5 → E5 → G5, rising |
| Phone disconnected | G5 → E5 → C5, falling |

Two details in [src/main.cpp](src/main.cpp) matter if you edit them:

- **Melodies are never played from a Bluetooth callback.** Those callbacks run in
  line with the audio path, so half a second of blocking I2S writes inside one
  would stall the decoder — the same failure mode as printing from a callback.
  The callback sets a flag and `loop()` does the playing.
- **`set_output_active_by_state(false)`** keeps the I2S channel open when a
  stream suspends. By default ESP32-A2DP calls `end()` on the output at every
  pause, and `I2SStream::write()` silently returns 0 on a closed channel, so the
  melodies would go nowhere. Leaving the channel up also avoids the click the
  DAC makes when I2S stops and restarts.

Notes are `{frequency_hz, duration_ms}` pairs, with `0` Hz meaning a rest, and
each note is faded in and out over ~4 ms — skip that and every note edge clicks.

The chimes are fed to the analyser as well, so the display moves in time with
them.

## Output level

If the stock ESP32-A2DP setup sounds quiet, the volume curve is why. The library
scales every sample in software, and its default (`A2DPDefaultVolumeControl`) is
exponential. Your phone does not attenuate the stream itself — with AVRCP
absolute volume it just forwards its slider position and expects the sink to do
the work — so that curve *is* the volume you hear:

| Phone slider | Stock exponential | Linear (this project) |
|--------------|-------------------|-----------------------|
| 100 %        | 0 dB              | 0 dB                  |
| 75 %         | −9 dB             | −2.5 dB               |
| 50 %         | −18.5 dB          | −6 dB                 |
| 25 %         | −30 dB            | −12 dB                |

`LoudVolumeControl` replaces it with a linear curve, which is worth about 12 dB
at normal listening settings, and `START_VOLUME` is 127 so nothing is thrown
away before the phone gets a say.

Past that the stream is at digital full scale and there is no headroom left, so
`OUTPUT_GAIN` buys further level with soft clipping: samples are multiplied, then
peaks above 75 % of full scale are rounded into the ceiling by a quadratic knee
instead of being clamped flat — a limiter rather than a square wave. At the
default `1.5` quiet passages get the full +3.5 dB and only peaks compress. Set it
to `1.0` for a bit-perfect path (the shaper then does nothing at all), or raise
it toward `2.0` if you want volume more than you want fidelity.

If you need more level than that, it has to come from the analog side — the
PCM5102A puts out line level, which is loud into an amplifier but weak into
low-sensitivity headphones or passive speakers.

Note that the analyser taps the stream *after* all of this, so the meters and the
spectrum show what actually leaves the DAC. It has its own auto-gain on top, so
turning the phone down does not flatten the bars — otherwise, at a comfortable
listening level 20 dB below full scale, every band would be a two-pixel stub.

## Troubleshooting noise ("heavy rain", crackle, static)

Start with the hardware path, in order of likelihood:

1. **SCK is not tied to GND.** A floating SCK leaves the PCM5102A hunting for a
   master clock it will never see. This is the number-one cause and it sounds
   exactly like rain.
2. **Breadboard / long dupont jumpers.** BCK runs at 1.4 MHz. Loose contacts and
   20 cm unshielded leads corrupt bits. Shorten the three signal wires, keep them
   away from the power leads, and solder if you can.
3. **Only one ground path.** Make sure DAC GND and ESP32 GND share a solid,
   short connection — not a daisy chain through a breadboard rail.
4. **Power.** Feed VIN from 5V/VIN, not the 3.3V pin (the dev board's regulator
   is already loaded by the radio). A 100 µF cap across the DAC's VIN/GND helps.
5. **XMT, FMT, FLT, DEMP floating.** Tie FMT/FLT/DEMP to GND and XMT to 3.3 V.

If the hardware path is sound but Bluetooth playback is noisy, check:

- `-DCORE_DEBUG_LEVEL` above `1` in [platformio.ini](platformio.ini). Verbose IDF
  logging prints from inside the audio path and starves the I2S writer.
- `Serial.print` inside an A2DP/AVRC callback. Same problem — those callbacks run
  in line with the audio. This is why the metadata callback only stores the text
  and `loop()` does the printing.
- `buffer_count` / `buffer_size` in `make_i2s_config()` in
  [src/main.cpp](src/main.cpp). On IDF 5 these do not reach the driver as-is —
  AudioTools folds them into `dma_frame_num = buffer_size * buffer_count /
  frame_size`, with `dma_desc_num` left at 6. The 6 × 512 here gives ~104 ms of
  DMA slack, which is already generous for A2DP jitter. **Do not raise the
  product past 4092**: IDF caps a DMA descriptor at 4092 bytes and rejects the
  channel outright, so you get silence rather than more headroom.
- `auto_clear = true`. Without it, an underrun re-plays the stale DMA buffer and
  turns a silent gap into a buzz.
- `use_apll = true`, so 44.1 kHz is clocked exactly rather than by fractional
  approximation.
- Distance and interference. Bluetooth and 2.4 GHz Wi-Fi collide; move the phone
  within a metre and away from the router to confirm.

Other things it might be:

- **Distortion only when loud** is clipping, not noise. Lower `OUTPUT_GAIN`
  toward `1.0` — above `1.0` it deliberately trades headroom for level — or turn
  the phone down.
- **A whine that changes pitch with activity** is supply noise coupling in — give
  the DAC its own 5 V supply and join the grounds.

## Troubleshooting the display

**Nothing on the panel, and the log says `[ui] no SSD1306 at 0x3C or 0x3D`.**
Nothing answered on the bus. Check SDA on **GPIO21** and SCL on **GPIO19** — not
GPIO22, which is I2S data — and that VCC is on 3.3 V. Swapped SDA/SCL is the
other classic; the panel simply stays silent.

**Nothing on the panel and no `[ui]` line at all.** `UI_ENABLED` is 0.

**The image is shifted sideways by two pixels.** Winstar panel: swap the
constructor as described under Wiring.

**Upside down.** `-DUI_FLIP_180=1`.

**Animation is jerky, `ui` reports well under 30 fps.** The frame rate is bounded
by the I2C transfer, not by rendering: 512 bytes at 400 kHz is ~11.5 ms. Raise
`OLED_BUS_HZ` to 700000 (fine on short wires — the 400 kHz in
[src/ui_config.h](src/ui_config.h) is the SSD1306's *rated* maximum, not its
actual one) and `UI_FPS` with it.

**Bars barely move / bars are always full height.** That is the analyser
auto-gain settling. `VIS_RANGE_DB` sets how many dB the full bar height spans —
lower it for a punchier, calmer display, raise it to see quiet detail.
`VIS_FALL_PER_S` sets how fast bars drop back.

**The clock is wrong.** See The clock above; `time` on the serial monitor tells
you where it thinks it got the time from.

**The display works but audio started crackling after adding it.** The renderer
should be incapable of that — it holds no locks the audio path touches and runs
below it in priority. Check you did not raise `UI_FPS` and `OLED_BUS_HZ` far
enough to saturate core 0, and check `CORE_DEBUG_LEVEL` is still 1.

## Customising

Audio knobs live at the top of [src/main.cpp](src/main.cpp):

- `DEVICE_NAME` — the name shown in your phone's Bluetooth list.
- `PIN_I2S_*` — move the I2S pins. Almost any output-capable GPIO works; avoid
  the strapping pins (0, 2, 12, 15) and the flash pins (6–11).
- `START_VOLUME` — 0–127, linear. Your phone overrides this via AVRCP once
  connected, so there is little reason to lower it from 127.
- `OUTPUT_GAIN` — extra gain on top, `1.0` = bit-perfect. See Output level.
- `MELODY_AMPLITUDE` — how loud the chimes are, 0–32767, independent of volume.
- `MELODY_CONNECT` / `MELODY_DISCONNECT` / `MELODY_NOTIFY` — the tunes
  themselves, as `{Hz, ms}` notes.

Display knobs live in [src/ui_config.h](src/ui_config.h), all commented in place:

- `UI_ENABLED`, `PIN_OLED_SDA` / `PIN_OLED_SCL`, `OLED_BUS_HZ`, `UI_FPS`,
  `UI_FLIP_180`
- `UI_SCREEN_DWELL_MS`, `UI_TRANSITION_MS`, `UI_VOLUME_POPUP_MS`, `UI_TOAST_MS`
- `UI_BRIGHT_*`, `UI_DIM_AFTER_MS`, `UI_SLEEP_AFTER_MS`
- `PIN_UI_BUTTON`, `UI_BTN_LONG_MS`, `UI_BTN_HOLD_MS`
- `FFT_SIZE`, `VIS_BANDS`, `VIS_RANGE_DB`, `VIS_FALL_PER_S`,
  `VIS_PEAK_FALL_PER_S`, `VIS_PEAK_HANG_MS`, `VIS_AGC_RELEASE_S`
- `CLOCK_24H`, `CLOCK_TZ_OFFSET_MIN`

To make the ESP32 forget the paired phone, call
`a2dp_sink.clean_last_connection()` once in `setup()`, flash, then remove it again.

## Source layout

| File | What is in it |
|------|---------------|
| [src/main.cpp](src/main.cpp) | A2DP sink, volume control, melodies, serial console |
| [src/management.h](src/management.h) / [.cpp](src/management.cpp) | Wi-Fi, authenticated API, Bluetooth/media control, OTA and GitHub updater |
| [src/web_assets.h](src/web_assets.h) | responsive dashboard source, gzip-embedded at build time |
| [src/ui_config.h](src/ui_config.h) | every display, analyser and clock knob |
| [src/player_state.h](src/player_state.h) / [.cpp](src/player_state.cpp) | the shared "what is playing" model (seqlock) |
| [src/audio_probe.h](src/audio_probe.h) / [.cpp](src/audio_probe.cpp) | sample tap, FFT, bands, VU, waveform, beat |
| [src/soft_clock.h](src/soft_clock.h) / [.cpp](src/soft_clock.cpp) | timekeeping: build stamp, serial, DS3231, NTP, NVS |
| [src/ui.h](src/ui.h) / [.cpp](src/ui.cpp) | the screens, overlays, transitions, marquees, 7-segment |
| [src/ui_assets.h](src/ui_assets.h) | hand-drawn XBM icons |

## Notes on the toolchain

- Audio comes from [ESP32-A2DP](https://github.com/pschatzmann/ESP32-A2DP)
  v1.8.11, driven through
  [arduino-audio-tools](https://github.com/pschatzmann/arduino-audio-tools)
  v1.2.5 via `I2SStream` — the output path the library documents. AudioTools is
  a hard dependency of current ESP32-A2DP, not an optional extra.
- The display uses [U8g2](https://github.com/olikraus/u8g2) in full-buffer mode
  (512 bytes for 128×32). It is the only one of the three libraries that comes
  from the PlatformIO registry; the other two point at tagged GitHub commits.
- The platform is the [pioarduino](https://github.com/pioarduino/platform-espressif32)
  build of Arduino core 3.3.11 / IDF 5.5.5. The official `platformio/espressif32`
  platform never shipped a core 3.x release, and ESP32-A2DP ≥ 1.8.10 uses IDF 5
  symbols (e.g. `ESP_A2D_AUDIO_STATE_SUSPEND`) that do not exist on core 2.x.
- `board_upload.flash_size = 16MB` and `default_16MB.csv` provide two 6.25 MB
  application slots and 3.375 MB of filesystem space. Dashboard assets are
  gzip-compressed by `scripts/embed_web.py` before compile.
- The FFT is written out in [src/audio_probe.cpp](src/audio_probe.cpp) rather
  than pulled from a library: it is 20 lines, and having the window, the banding
  and the scaling in one place is what makes the display look right.
