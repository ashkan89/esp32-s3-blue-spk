# esp32-blue-spk

Turns an **ESP32 WROOM-32D** into a Bluetooth audio receiver. Pair your phone,
press play, and the audio comes out of the headphone jack on a **PCM5102A** I2S
DAC board. It chimes when a phone connects or disconnects, and drives the DAC at
full level rather than through the A2DP library's very quiet default curve.

A **0.91" 128×32 OLED** shows what is going on: the track, the phone it came
from, the volume, a clock, and a live spectrum analyser fed from the actual audio
on its way to the DAC. The display is optional — with nothing on the I2C bus the
firmware notices at boot and behaves exactly as it did without one.

Audio does not have to arrive over the air. A **DFPlayer Mini** on the second
UART turns the speaker into a standalone player: a microSD card or a USB flash
drive, driven entirely from the dashboard, with Bluetooth switched off and Wi-Fi
kept for the dashboard. Plug the module's USB port into a computer and the card
mounts as a drive, so loading it needs no card reader. There is also an optional
**battery gauge** — percentage, voltage, charge state and a low-battery
indicator on the display, the LED and the dashboard. Both are absent-tolerant in
the same way the display is.

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
| DS3231 RTC module      | shares the OLED's two I2C wires; enabled by default |
| WS2812 RGB ring, 7-bit | optional; the 23 mm "5050 RGB LED Ring 7-Bit". Any WS2812B strip works |
| DFPlayer Mini (MP3-TF-16P) | optional; YX5200 or the AA104/GD3200B clones. Adds *DFPlayer mode* |
| microSD card, FAT32    | for the DFPlayer. 32 GB or less; ≤ 3000 files per folder |
| USB-A socket           | optional, for a flash drive on the DFPlayer's host port |
| USB-B / micro-B socket | optional, so the card mounts on a computer as a drive |
| 1S Li-ion / LiPo cell + TP4056 board | optional; two 100k resistors make the divider |

## Wiring

### PCM5102A → ESP32

| PCM5102A | ESP32       | Notes                                            |
|----------|-------------|--------------------------------------------------|
| VIN      | 5V (VIN)    | The board has its own 3.3 V regulator            |
| GND      | GND         |                                                  |
| BCK      | GPIO26      | Bit clock                                        |
| DIN      | GPIO23      | Serial data — **not** GPIO22, see below           |
| LCK      | GPIO25      | Word select / LR clock                           |
| SCK      | **GND**     | Required — selects the internal PLL              |
| FMT      | GND         | Standard I2S framing                             |
| XMT      | 3.3V        | Un-mute (usually already pulled high on-module)  |
| FLT      | GND         | Normal latency filter                            |
| DEMP     | GND         | De-emphasis off                                  |

The single most common failure is leaving **SCK floating** — the DAC then waits
for an external master clock and you get silence or noise. Tie it to GND.

Most I2S examples put DIN on GPIO22, and it is worth knowing why this one does
not: GPIO21/22 is the canonical ESP32 I2C pair, and that is where the OLED and
the DS3231 sit. Two signals cannot share a pin, so the data line moves one over
to GPIO23, which is otherwise unused. If you are following an older build of
this project, this is the wire to move.

Power: the PCM5102A only draws ~20 mA, so USB power off the dev board is fine.
If you hear a hiss or whine that tracks the CPU, give the DAC its own supply and
join the grounds.

### DFPlayer Mini → ESP32

Optional, and only used in *DFPlayer mode*. Every pin the module brings out is
either wired or accounted for — the point of this mode is that nothing on the
module is out of reach from the dashboard.

| DFPlayer | ESP32 | Notes |
|----------|-------|-------|
| 1 VCC     | 5V (VIN) | 4.0–5.0 V. 3.3 V works but is quiet and browns out on card access |
| 7, 10 GND | GND      | both, and share ground with the DAC |
| 2 RX      | GPIO17 **through 1k** | the resistor is not optional in practice — see below |
| 3 TX      | GPIO16   | 3.3 V logic, connect directly |
| 4 DAC_R   | output stage R | line level, ~1 Vrms — **not** the PCM5102A's input |
| 5 DAC_L   | output stage L |  |
| 6 SPK2    | — | the on-board 3 W amp is unused; this build drives a line output |
| 8 SPK1    | — |  |
| 9 IO1     | GPIO32 | the module's own *previous* button input |
| 11 IO2    | GPIO33 | the module's own *next* button input |
| 12 ADKEY1 | GPIO14 | ADC key bank 1 — plays track 1 when grounded |
| 13 ADKEY2 | GPIO27 | ADC key bank 2 — plays track 11 when grounded |
| 14 USB+   | USB socket D+ | see *USB* below |
| 15 USB−   | USB socket D− |  |
| 16 BUSY   | GPIO35 | LOW while a file is playing |

**The 1k on RX.** Without it the module picks up switching noise off the ESP32's
output and answers frames that were never sent — which looks like a module with
a mind of its own rather than a wiring problem. Every DFPlayer application note
says to fit it; fit it.

**IO1, IO2, ADKEY1 and ADKEY2 are inputs on the module**, held high by its own
pull-ups and acted on when pulled to ground. The firmware drives them
*open-drain*: high-impedance inputs at rest, outputs driven low for the length
of a press. So real buttons wired in parallel still work, nothing fights over
the line, and the dashboard's hardware-pin buttons are a second control path
that works even when the serial link does not.

GPIO14 emits a brief pulse during boot on the ESP32. It is far too short for the
module's ADC key sampler to see, but it is the reason ADKEY1 is on 14 rather
than something the firmware drives.

#### Audio wiring: the DFPlayer does not feed the PCM5102A

DAC_L and DAC_R are **analog line outputs**. The PCM5102A's input is **digital
I2S**. Nothing turns one into the other, so the DFPlayer joins the signal chain
*after* the PCM5102A, at the output jack:

```
  DFPlayer DAC_L ---||--- 10k ---+
                   10uF          |
  PCM5102A  LOUT ------- 10k ----+---- jack tip (left)

  DFPlayer DAC_R ---||--- 10k ---+
                   10uF          |
  PCM5102A  ROUT ------- 10k ----+---- jack ring (right)
```

and grounds joined. The series resistors make it a passive summing node; the
10 µF blocks the DFPlayer's DC bias, which sits at about half its supply and
would otherwise be pushed into whatever is downstream.

This is safe precisely **because the radio modes are mutually exclusive**.
DFPlayer mode never starts the A2DP sink or the network player, and no other
mode starts the DFPlayer, so only one of the two sources is ever producing
anything — the other contributes its output impedance and nothing else. The
~6 dB the resistors cost is recovered on the DFPlayer's own 31-step volume and
on the ESP32's soft-clipped gain path.

If you would rather not solder a summing network, give the DFPlayer its own jack.
Nothing in the firmware cares.

#### USB: a flash drive, or the card on a computer

Pins 14/15 are the module's USB data lines, and they do two different jobs
depending on what is on the other end.

- **A USB flash drive.** The module is the host and plays from the stick. Wire a
  USB-A socket — D+ to pin 14, D− to pin 15, 5 V and GND from the same supply as
  VCC — and pick source *USB drive* on the dashboard's Media page. It appears as
  a second library alongside the card, with its own file count.
- **A computer.** The module becomes a card reader and the microSD shows up as a
  mass storage volume, which is how the card gets loaded without taking it out.
  Wire a USB-B or micro-B socket the same way. The module reports the event over
  the serial link (`0x3A`/`0x3B` with device 4), so the dashboard says *card
  mounted on a computer*, the status LED shows the no-media pattern, and playback
  from the card stops until the cable is unplugged — the card belongs to the
  computer while it is there, and that is the module's behaviour, not a policy
  this firmware invented.

One OTG-style connector can do both. Two sockets wired in parallel is simpler
and is what the table above assumes — do not plug both in at once.

`PIN_DF_USB_DETECT` can sense VBUS on that socket through a divider, so a
computer is visible even if the module missed its own event. It defaults to `-1`
because an input-only pin with nothing on it floats, and a floating pin invents
events.

#### Card layout

The YX5200 reports counts and indices and **never a filename**, so the dashboard
works in numbers. Three ways to address a file, in decreasing order of
reliability:

| Addressing | Path | Notes |
|------------|------|-------|
| folder + track | `/01/003.mp3` … `/99/255.mp3` | stable; the one to use |
| MP3 folder | `/MP3/0007.mp3` | flat playlist, up to 3000, survives re-copying |
| flat index | *n*-th file on the card | follows FAT directory order, which changes when you re-copy the card |

Zero-pad exactly as shown — the module's own firmware parses those names and
will not find `/1/3.mp3`. FAT32, 32 GB or smaller. Copy files in the order you
want the flat index to run, and delete the `.Trashes` / `._*` files a Mac leaves
behind: the module counts them.

### Battery gauge → ESP32

Optional. One divider is the whole requirement; the charger pins add charge
detection.

```
  BAT+ --- 100k ---+--- 100k --- GND
                   |
                GPIO34 (ADC1_CH6)
```

| Signal | ESP32 | Notes |
|--------|-------|-------|
| divider tap | GPIO34 | **ADC1**, and input-only — see below. Configured, but the gauge is off until enabled in Settings |
| TP4056 CHRG  | GPIO36 (`PIN_BATTERY_CHARGING`, default `-1`) | open drain, needs its own 10k to 3V3 |
| TP4056 STDBY | GPIO39 (`PIN_BATTERY_FULL`, default `-1`) | likewise |

**ADC1, not ADC2.** ADC2 shares hardware with the Wi-Fi radio and reads garbage
whenever the driver is up, which in this firmware is nearly always. That leaves
GPIO32–39, and 34/35/36/39 are input-only, which is exactly what a sense pin
wants. 100k/100k halves 4.2 V to 2.1 V, inside the ~2.45 V the 11 dB attenuator
can read.

The two charger pins default to off, so a speaker with only the divider still
reports voltage and percentage correctly — it just cannot tell charging from
resting, and says so rather than guessing. GPIO34–39 have no internal pull-ups,
so each charger pin needs an external 10k to 3V3; wire them and the dashboard
gains *Charging* and *Full*.

Divider ratio, trim, cell count, full/empty voltages (**per cell**) and both
warning thresholds are stored in NVS and editable from **Settings → Battery**, so
one firmware serves a 1S and a 2S build — for 2S, set *Series cells* to 2 and the
divider to 4, and leave full/empty at 4.20/3.30 because they describe one cell.
Defaults live in [src/hw_config.h](src/hw_config.h), and the gauge itself is off
until switched on there.

### OLED → ESP32

| OLED | ESP32   | Notes                                                      |
|------|---------|------------------------------------------------------------|
| VCC  | **3.3V**| These modules have no regulator — 5 V destroys them        |
| GND  | GND     |                                                            |
| SDA  | GPIO21  |                                                            |
| SCL  | GPIO22  |                                                            |

This is the canonical ESP32 I2C pair, so the module's silkscreen, every tutorial
and every scrap of example code agree with it. The cost is paid at the DAC:
GPIO22 is where most I2S examples put DIN, so `PIN_I2S_DOUT` in
[src/main.cpp](src/main.cpp) is GPIO23 here instead. Two signals cannot share a
pin — if you ever move I2C off 21/22, move the DAC's DIN wire back with it.

Both the address (0x3C, falling back to 0x3D) and the pins are checked at boot;
the serial log says which it found, or says it found nothing:

```
[ui] SSD1306 128x32 at 0x3C, 400 kHz, 30 fps
```

A few 0.91" modules use a Winstar panel with a different column offset, which
shows up as the image being shifted sideways by two pixels. If yours does that,
swap `U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C` for
`U8G2_SSD1306_128X32_WINSTAR_F_HW_I2C` in [src/ui.cpp](src/ui.cpp).

### DS3231 RTC → ESP32

I2C is a bus, so the clock module costs no extra GPIO — it hangs off the same two
wires as the panel, in parallel with it.

| DS3231 | ESP32   | Notes                                                    |
|--------|---------|-----------------------------------------------------------|
| VCC    | 3.3V    | 5 V also works on these boards, but there is no reason    |
| GND    | GND     |                                                           |
| SDA    | GPIO21  | the same wire as the OLED's SDA                           |
| SCL    | GPIO22  | the same wire as the OLED's SCL                           |
| SQW    | —       | not used                                                  |
| 32K    | —       | not used                                                  |

The RTC answers at address 0x68 and the panel at 0x3C, so nothing collides.
Both modules carry their own pull-up resistors; two sets in parallel is a lower
resistance than either alone, which on short dupont leads is harmless.

The driver is compiled in by default (`-DUSE_DS3231=1` in
[platformio.ini](platformio.ini)). A board with no RTC fitted needs no change —
`soft_clock_begin()` probes 0x68 at boot, logs `no ds3231 on the bus`, and falls
back to the other time sources — but you can set the flag to 0 to drop the code
entirely.

One caveat about the coin cell, which is a property of the common ZS-042 board
rather than of the chip: it wires a trickle-charge circuit meant for a
rechargeable LIR2032, and most of them ship with a non-rechargeable CR2032 in
the holder. Fit a LIR2032, or lift the charging resistor, or accept it — the
timekeeping is the same either way.

### WS2812 ring → ESP32

Seven addressable pixels on one wire — one in the middle, six around the rim.
Any WS2812B strip works; `LED_COUNT` and `LED_CENTRE_INDEX` in
[src/hw_config.h](src/hw_config.h) describe what you actually fitted.

| Ring | ESP32       | Notes                                                    |
|------|-------------|-----------------------------------------------------------|
| DIN  | GPIO18 via 330R | the resistor damps the edge; see *Logic levels* below |
| 5V   | **5V (VIN)**| not the 3.3 V rail — see *Power* below                    |
| GND  | GND         | shared with the ESP32 and with the DAC                    |

**Power.** A WS2812B is three 20 mA emitters, so seven of them at full white is
about **420 mA** — more than a devkit's 3.3 V regulator will give you, and enough
to brown the board out mid-track if you take it from there. Feed VCC from the
same 5 V that runs the DFPlayer, and fit a 470–1000 µF capacitor across 5 V and
GND at the ring itself. The other half of the answer is `LED_BRIGHTNESS_MAX`,
which caps every effect: it ships at 160/255, roughly 260 mA, so the ceiling is a
number you chose rather than whatever the brightest frame happens to draw.

**Logic levels.** WS2812B wants its data line at 0.7 × VDD, which at 5 V is 3.5 V,
and the ESP32 drives 3.3 V. Nearly every module accepts it anyway — which is why
the series resistor and a short lead matter. If the **first** pixel is unreliable
and the rest are fine, that is exactly this: either put a level shifter in the
data line, or feed the ring about 4.5 V (a signal diode in series with its 5 V)
so 3.3 V clears the threshold with room to spare.

**Why GPIO18.** It is free, it is not a strapping pin, and it has no boot-time
constraints. Any output-capable GPIO works — but not 34–39, which are input-only
and cannot drive anything, and preferably not 0/2/12/15, which are sampled at
reset.

The pixels are clocked out by the **RMT** peripheral rather than bit-banged. That
is not a detail: the WS2812 protocol encodes each bit as a pulse width, so a
bit-banger that gets preempted by the audio path mid-frame writes visible
garbage, and the usual way round that is to disable interrupts for the length of
the frame — which on a speaker is not a trade worth making. RMT is otherwise
unused here.

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
console is available at the IP printed on the serial monitor, at
`http://esp32-blue-spk.local/`, and in the dashboard. If saved credentials stop
working, the setup network comes back after 15 seconds, so the speaker cannot be
locked out by a router change.

Seven pages: **Overview** (the transport, the source card for whichever mode is
running, the battery, the radio mode picker, firmware), **Devices** (Bluetooth
pairings), **Media** (the DFPlayer library and every one of its controls),
**Lighting** (the WS2812 ring: effects, both colour pickers, and how much of the
music is allowed to show), **Wi-Fi**, **Updates** and **Settings**. Pages that do not apply to the running
mode say so and explain how to get to one where they do, rather than showing
dead controls — the Devices page in a mode with no A2DP sink, the Media page in a
mode with no DFPlayer.

### Five radio modes

This chip has a single 2.4 GHz front end shared by Wi-Fi and Bluetooth. What
Espressif's coexistence scheduler supports is Wi-Fi *station* alongside
Bluetooth. A SoftAP beside an A2DP sink is **not** a supported combination, and
in practice neither side works — the access point cannot be joined, and the sink
is not reliably discoverable. Earlier versions tried to referee that contest
with coexistence preferences, beacon tuning and a "quiet window", and gave up.
That line — *a station is fine, a hotspot next to Bluetooth Classic is not* — is
what the modes are drawn along:

| Mode | Wi-Fi | Bluetooth | Audio arrives over |
|------|-------|-----------|--------------------|
| **Wi-Fi only** | station, or setup hotspot if no network is saved | not started at all; its DRAM goes back to the heap | nothing |
| **Bluetooth only** | never initialised | A2DP sink owns the antenna | A2DP |
| **Wi-Fi + Bluetooth** | station only; never raises the setup hotspot | A2DP sink, sharing the radio under coexistence | A2DP |
| **Wi-Fi + BLE** | station, or setup hotspot; both are fine here | BLE only — Classic is never started | **Wi-Fi** (DLNA or a URL) |
| **DFPlayer + Wi-Fi** | station, or setup hotspot; identical to Wi-Fi only mode | neither half is started; the *whole* controller goes back to the heap | **a microSD card or USB drive**, over serial |

The mode is remembered across restarts, so a power cut brings the speaker back
doing whatever it was doing. A factory-fresh board starts in Wi-Fi only mode
with the setup hotspot up, which is where configuration happens.

A mode that fails to stay up twice in a row falls back to Wi-Fi only mode, where
the dashboard is reachable and the mode can be changed again. That sentinel lives
in NVS, so it survives the reboot loop it exists to break; DFPlayer mode is
covered by it like every other non-default mode.

#### Wi-Fi + Bluetooth

The mode that makes the dashboard's media controls worth having: play, pause,
skip, seek and volume act on the phone that is streaming right now, because the
Bluetooth stack and the web server are both up at the same time. Three things
make it work rather than merely start:

- **Station only.** `startAccessPoint()` refuses to run in this mode, at the
  call sites and inside the function itself. A speaker with no saved network is
  demoted to Wi-Fi only at boot, and the demotion is persisted, because
  otherwise it would have no dashboard and no way to be given one.
- **Bluetooth keeps its RAM.** The `esp_bt_mem_release(ESP_BT_MODE_BTDM)` that
  Wi-Fi only mode performs is skipped. ESP32-A2DP starts the controller in
  `ESP_BT_MODE_CLASSIC_BT` and releases the BLE half itself (~30 KB).
- **The scheduler is told who matters.** `esp_coex_preference_set(ESP_COEX_PREFER_BT)`
  before either stack starts, and Wi-Fi modem sleep left on so there are windows
  to hand over.

The trade is heap: both stacks resident at once leaves noticeably less free than
either alone, and the updater's TLS handshake is the deepest allocation this
firmware makes. If an update fails for memory here, do it from Wi-Fi only mode
and switch back.

#### Wi-Fi + BLE

The other way to answer "both radios at once", and the only one where the audio
does **not** come over Bluetooth.

**Why not BLE audio.** This is a classic ESP32: Bluetooth 4.2, no BLE 5 features
and no isochronous channels. LE Audio — the profile that carries music over
Bluetooth Low Energy — requires Bluetooth 5.2 silicon, and this build's
`sdkconfig` ships no LE Audio profile for the target at all (`CONFIG_BT_A2DP_ENABLE`
is the only audio profile present). The bandwidth says the same thing
independently: BLE 4.2 here sustains roughly 100–300 kbps in practice, against
about 1,411 kbps for CD-quality stereo. And no phone will stream media to a
generic BLE peripheral — iOS and Android only offer A2DP for that. So BLE is not
an audio path on this hardware, on any firmware.

What it is good at is control. So in this mode the audio comes over Wi-Fi and
BLE does the rest:

- **DLNA / UPnP MediaRenderer.** The speaker advertises itself by SSDP and
  anything that can cast to a network speaker finds it — BubbleUPnP, VLC, Hi-Fi
  Cast, foobar2000, Windows' own *Cast to Device*. No app to write and nothing
  to pair. The renderer serves its description and SOAP endpoints on port 9000;
  the dashboard stays on 80.
- **A stream URL.** Paste one into the Overview page, send it over BLE, or type
  `play <url>` on the console. Internet radio, or any HTTP audio on the network.
- **BLE control service.** Three characteristics on one custom service: `status`
  (read + notify, compact JSON), `command` (write: `play`, `pause`, `stop`,
  `vol N`, `url U`) and `wifi` (write: `<ssid>\n<password>`).

That last one is the reason BLE earns its place. Wi-Fi + Bluetooth mode never
raises the setup hotspot, so a speaker whose router password changed is
unreachable until somebody walks over and holds BOOT. Here there are two ways
back in: the setup hotspot — allowed in this mode, because the combination this
chip cannot do is a hotspot beside *Classic*, and BLE is not that — and BLE
provisioning, which works from across the room with any GATT app.

Decoding is MP3, AAC and WAV, via the fixed-point Helix decoders. FLAC and Opus
decoders exist in AudioTools but do not fit the heap budget next to Wi-Fi, BLE
and the web server. The Classic half of the controller is released before BLE
starts, which is the mirror image of what the A2DP sink does in the Bluetooth
modes.

Two honest limits. There is no skip or seek: a stream URL is not a playlist, and
the dashboard disables those buttons rather than pretending. And a DLNA control
point that sends a format outside those three gets an error rather than silence.

#### DFPlayer + Wi-Fi

The only mode where nothing arrives over the air. A DFPlayer Mini holds the card,
decodes the file and produces its own analog output; the ESP32 does not touch the
audio at all. What it does is *control* — a 9600 baud serial link and six GPIOs —
and report what it learns.

That is also why this is a Wi-Fi mode rather than a third radio arrangement.
Nothing about it wants the antenna, so Wi-Fi gets all of it: station when a
network is saved, the setup hotspot when not, and the dashboard either way,
behaving exactly as it does in Wi-Fi only mode. Both halves of the Bluetooth
controller are released at boot, which is the largest heap saving any mode makes
and leaves the most room the OTA updater's TLS handshake ever gets.

**What the dashboard drives.** Everything the module's protocol exposes, on the
**Media** page:

- **Source** — SD card, USB drive, on-board flash, AUX. Switching re-reads the
  file count, and each source's presence is reported from the module's own
  insert/remove notifications, so pulling the card out shows up.
- **Playback** — the Overview transport works as it does for every other source:
  play, pause, stop, next, previous, volume, mute. Next and previous are real
  here, because the module has a real playlist. Forward and rewind are disabled
  with the reason written underneath: the YX5200 reports no position and takes no
  seek command, so there is nothing to seek with.
- **Track selection** — folder + track, flat index, `/MP3/nnnn.mp3`, and the
  `/ADVERT/` announcement channel that interrupts the current track and resumes
  afterwards. Type a folder number and the module is asked how many tracks it
  holds, so the range is known before anything plays.
- **Sound** — the module's 31-step volume (the Overview slider is the same
  setting on a 0–100% scale), all six EQ presets, all four repeat modes plus off,
  and the module's own DAC mute.
- **Hardware pins** — IO1 and IO2 pressed short or long, ADKEY1 and ADKEY2
  triggered, the BUSY pin's state, and the dedicated DFPlayer LED (follow BUSY /
  on / off / blink). Pins the board does not wire are greyed out with a tooltip
  rather than hidden, so the page describes the hardware rather than pretending.
- **The module** — firmware version, file and folder counts, tracks finished this
  boot, the last protocol error in plain words, and reset / standby / wake.
  Standby powers the decoder down, worth about 20 mA, which matters on a battery.

**Standby is a state, not a silence.** A sleeping YX5200 answers every query with
"I am in standby", so the firmware stops asking — which means no frame arrives,
which is indistinguishable from a dead module unless somebody says otherwise. So
standby is carried through the whole stack as its own state: the offline timeout
is suspended, the BUSY pin is not allowed to overwrite it, the status LED shows
*idle* rather than the fault pattern, and the dashboard, the OLED and the console
all say *standby* and tell you to wake it. A command sent to a sleeping module
comes back with "wake it first" rather than a protocol error code.

**Startup defaults.** The module forgets everything at power-off — volume, source
and EQ included — so the firmware sends them again at every boot. *Save as
startup defaults* on the Media page stores what is set now; **Settings → DFPlayer
startup defaults** edits them by hand and adds *start playing at power-on*, which
waits for the card to mount and report a file count before sending anything and
gives up after fifteen seconds rather than sitting on a command the module would
refuse.

**Browsing it.** There is no directory listing in the protocol and there cannot
be: the YX5200 answers in counts and indices and has no idea what a file is
called. What it *will* answer is "how many files are in folder N", one folder per
round trip — so **Media → Library → Scan library** asks all ninety-nine, caches
what comes back, and draws a browser out of the numbers. Click a folder, click a
track, it plays; transport controls sit under the grid so the page is
self-contained.

The scan takes about twelve seconds on a 9600 baud link, with the folders
appearing as their answers land, and only runs when you ask for it. The result is
kept until the library under it could have changed — a source switch, a card
going in or out, a reset — at which point it is dropped rather than left to
describe a card that is no longer there. The reply to 0x4E does not say which
folder it is for, so each answer is attributed to whichever folder the last query
named: safe, because one task owns the wire and the module replies in order, and
the scan paces itself slower than the ordinary command gap to keep it that way.

Folder-and-track is also the *reliable* way to play something here. The flat
index follows FAT directory order, which changes when you re-copy the card;
`/01/003.mp3` does not.

**Two honest limits.** The module never reports a filename, a duration or a
position, so the dashboard works in track numbers and the OLED's progress bar
stays empty — the same case it already handles for phones that send no AVRCP
metadata. And the spectrum, VU, scope and waterfall screens never appear in this
mode: the analyser is fed from samples on their way through the ESP32's I2S, and
in this mode there are none. The screens simply drop out of the carousel, which
is the existing rule for "nothing to show" rather than anything new.

**How the state is decided.** BUSY is a hardware output of the decoder, so it is
a second ahead of anything the serial poll can say, and it wins — immediately
when it says *playing*, and after 400 ms when it says *not playing*. The delay is
there because the pin also goes high in the gap **between** two tracks, while the
module closes one file and seeks the next; without it, every auto-advance in a
repeat-all playlist would flicker the badge and the display through *stopped* and
back. With BUSY unwired (`PIN_DF_BUSY = -1`) the driver falls back on the poll
alone, which is a second slower and cannot see a track boundary at all.

**Diagnosing it.** `df` on the console prints the whole picture in one line, and
`df` with no argument also lists its own subcommands. The single most common
failure is TX and RX not crossed over, which looks exactly like a dead module —
the dashboard says *not answering* and names that cause, and the hardware-pin
buttons keep working regardless, which is how you tell a wiring fault from a
dead module.

#### Switching

**From the speaker:** hold BOOT for three seconds. The panel offers the next
mode in the cycle — Wi-Fi → Bluetooth → Wi-Fi + BT → Wi-Fi + BLE → DFPlayer →
Wi-Fi — so
let go and press BOOT once within eight seconds to confirm. Ignore it and it
goes away; keep holding and you are into the factory-reset countdown instead. On
a board with no display, a three-second hold released before six seconds
switches immediately.

**From the dashboard:** **Overview → Radio mode**, which lists all five and
marks the current one. Switching to *Bluetooth only* takes Wi-Fi and the page
with it, so it asks first; the others come back in a few seconds. *Wi-Fi +
Bluetooth* stays greyed out until a network has been saved, and *DFPlayer +
Wi-Fi* is greyed out only in a build compiled with `-DDFPLAYER_ENABLED=0`.

**From the console:** `mode` steps to the next one; `wifi`, `bt`, `both`, `net`
and `sd` go somewhere specific; `radio` prints where you are and what each half
is doing.

Every switch goes through a restart. Both stacks own controller state, DMA
channels and tasks, and ESP32-A2DP's `end()` also forgets the last paired
device — a reboot costs about a second and guarantees each mode starts from a
clean radio.

In Wi-Fi only mode the dashboard says so rather than pretending: the Bluetooth
card reads *Off*, the transport buttons are disabled with the reason written
underneath them, the Devices page explains why the list is empty, and the media
and device endpoints answer `409` instead of poking a stack that is not running.

**The Class of Device is set.** ESP32-A2DP never calls `esp_bt_gap_set_cod()`,
so the sink inherited Bluedroid's default and phones listed it as a nondescript
"other" device — a generic icon, and on some Android builds no offer to connect
it for media at all. It now identifies as an Audio/Video loudspeaker with the
rendering and audio service classes. This is written a couple of seconds *after*
`a2dp_sink.start()`, not right after it: `start()` only queues the bring-up, and
a class of device written before the profiles register gets overwritten — and
writing it mid-init raced the scan-mode setup and left the speaker invisible.
The same deferred pass re-asserts discoverability, and `pair` forces it by hand.

### Diagnosing a failed join

If a join still fails, the serial log now says where. `[ap] joined ...` means
association worked; a following `[ap] left ... (reason 15)` is a WPA2 handshake
timeout; `[ap] lease ...` means DHCP handed out an address and the client is
really on. Nothing at all means the client never got as far as associating.

The station side is logged the same way — `[sta] associated`, `[sta] address`,
and `[sta] disconnected: <reason>`. `AUTH_FAIL` or `4WAY_HANDSHAKE_TIMEOUT`
after a fresh save means the password is wrong; `BEACON_TIMEOUT` or
`NO_AP_FOUND` after a working connection means the radio lost the router.

### Station behaviour

Wi-Fi power save is left at its default `WIFI_PS_MIN_MODEM`. It used to be
turned off for a snappier dashboard, which was one of the causes of "the display
shows an IP address but the router shows no client": with `WiFi.setSleep(false)`
and an A2DP sink running, coexistence had no modem-sleep windows to hand the
antenna over in, and the station associated, took a lease and then quietly
missed beacons until the router aged it out. Bluetooth no longer runs alongside
it, but the default costs nothing a dashboard would notice.

The station enables 802.11d so it adopts the router's regulatory domain
from its beacons. Without it the radio is stuck on the "01" world-safe channels
and a router on channel 12 or 13 appears in a scan but can never be joined.

The saved network gets 30 seconds to connect before the recovery access point is
raised, and the same grace again after any later drop — raising the AP parks the
station, so calling a slow router a dead one is expensive. Once the station is
back, the recovery AP is torn down again (unless **Always keep setup hotspot
on** is set, or somebody is still connected to it).

Change both default passwords under **Settings → Identity & access**. The
dashboard uses authenticated HTTP on the local network; it is not intended to
be port-forwarded or exposed directly to the internet. Wi-Fi credentials,
dashboard password, and an optional GitHub token live in NVS on the device.

The console provides:

- live track metadata, progress, link state, memory, uptime, RSSI and OTA state;
- play/pause, stop, previous/next, rewind/fast-forward, mute and absolute volume;
- active-device disconnect plus a list of bonded devices that can be forgotten;
- Wi-Fi scanning/provisioning, automatic recovery AP, hostname and AP controls;
- the mode switch between Wi-Fi and Bluetooth;
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
update**. Public repositories need no token; a fine-grained token can be saved
for a private repository. GitHub API and release downloads are verified over TLS
against the Mozilla root store — see **Trust anchors** below.

Set the version reported by the dashboard in [src/app_config.h](src/app_config.h)
for each release.

**Only a newer release is an update.** The tags are compared as numbers,
component by component: a leading `v` and any pre-release suffix are ignored, and
missing components count as zero, so `v2.3` and `2.3.0` are the same release and
`2.10.0` is newer than `2.9.0`. A latest release that is older than or equal to
what is running reports *up to date* and offers no button, and the install
endpoint refuses it too — the dashboard is not the only way in, and the OTA
writer would flash an older image quite happily. There is no downgrade path
short of the **Upload firmware** box, which takes whatever `.bin` you hand it.

Restarts — after an update, a settings change, a factory reset or the dashboard
button — are run from a dedicated high-priority task rather than from a deadline
checked in the main loop. That used to work right up until the loop task was the
thing that was stuck, and then the OLED sat on its last frame and the restart
never arrived. A restart is exactly the wrong thing to make conditional on the
health of the task asking for it.

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

### Trust anchors

The updater trusts the **Mozilla root store**, shipped with the IDF as a compact
bundle linked into `libmbedtls` (about 68 KB of flash). Verification stays on —
this connection writes executable code to flash.

It used to pin a single root, DigiCert Global Root G2. GitHub has since moved:
`api.github.com` now chains to *Sectigo Public Server Authentication Root E46*,
and release assets come from a host with a Let's Encrypt chain. Neither
validates against a DigiCert root, so every request failed the handshake and
`HTTPClient` reported it as **HTTP -1** — its code for "connection refused",
which tells you nothing about what actually went wrong. Pinning one root was the
mistake, not the choice of root; the bundle survives the next CA change without
a firmware update. Transport failures are now reported in words rather than as a
small negative number.

### Checking on its own

The speaker checks GitHub once by itself, shortly after it comes up — and the
Overview page's **Firmware** card says so, with an **Install update** button, so
finding out does not mean going looking.

The check waits for the clock rather than running at boot. Certificate validity
is checked against it, and until the first network sync lands the clock is the
build stamp — which on a board that has been in a drawer for a month reads as a
certificate that has not started yet and fails the handshake exactly as if the
network were down. If SNTP cannot get out at all, the wait gives up after a
minute and tries anyway, since a DS3231 or a recent NVS write may well have left
the clock good enough.

### Following the download redirect by hand

`browser_download_url` points at `github.com`, which answers `302` with a signed,
time-limited URL on a storage host with a different name and a different
certificate chain. `HTTPClient` can follow that on its own, but it does it by
switching hosts underneath a live `NetworkClientSecure` — stop the socket,
reconnect the same mbedtls context to a different name. **Check GitHub** worked
and **Install update** then failed with **HTTP -1**: a refused connection, which
is what a handshake that never completes looks like from up there.

So the redirect is followed by hand, one hop at a time, each with its own client
constructed and destroyed in its own scope. Every handshake starts from a clean
context, only one TLS session is ever allocated at a time, and the token stays
with `github.com` instead of being forwarded to a storage host that rejects
requests carrying two sets of credentials. A hop that fails in transport — DNS,
TCP or TLS, never an HTTP status — is retried twice, which fits inside the few
minutes a signed URL stays valid.

Two other things came out of the same failure: the connect timeout is 20 s
rather than the five-second default (a handshake plus a chain walk is
comfortably achievable and comfortably missable on a slow uplink), and the task
no longer leaks. It ends in `vTaskDelete()`, which never returns and so never
unwinds the stack, and every early exit used to abandon a TLS context and a JSON
document — which is how a board that updated fine when it was fresh runs out of
heap for a handshake several checks later. Transport errors now carry the
free-heap figure, because the number alone does not distinguish "the CDN was
unreachable" from "there was not enough room left to talk to it".

Certificate validity is checked against the speaker's clock, which is one more
reason the [automatic time sync](#the-clock) matters: a chain that has not
started yet fails the handshake exactly as if the network were down.

### Room for a handshake

A TLS session against the root bundle needs roughly 45 KB, a good part of it in
one piece. Wi-Fi mode used to leave about **49 KB** of heap free, so the check
itself failed with `connection refused (-1)` — not a refused connection, a
handshake that could not be allocated.

The memory was never missing, only reserved. The Bluetooth controller and
Bluedroid own several fixed regions of DRAM that the linker sets aside whether
or not they are ever initialised, and in Wi-Fi mode they never are.
`esp_bt_mem_release(ESP_BT_MODE_BTDM)` at the top of Wi-Fi mode hands those back.
Adding up what the build actually reserves — `CONFIG_BTDM_RESERVE_DRAM` is
`0xdb5c`, and the ELF puts `_bt_data`, `_bt_bss` and the two `_bt_controller_*`
regions at another 17 KB — that is about **72 KB**. The boot log prints the real
before and after, which is the number to trust:

```
[mode] bluetooth memory released: ESP_OK (heap <before> -> <after>)
```

The memory does not come back until a reset, which costs nothing here: switching
modes already persists the choice and reboots.

None of this happens in **Wi-Fi + Bluetooth** mode, where both stacks have to be
resident: the release is skipped and only the BLE half goes back, freed by
ESP32-A2DP's own `start()`. That mode therefore runs with materially less heap
than Wi-Fi only, and this handshake is the first thing to feel it. If a GitHub
check or install fails there, do it from Wi-Fi only mode and switch back.

**Wi-Fi + BLE** splits the difference: it releases the Classic half only (in
`ble_control_begin()`, before the controller initialises) and keeps BLE, then
spends part of what it saved on the decoders and the DLNA renderer. The boot log
prints the heap either side of both the BLE start and the network player start,
which between them are the numbers to watch in that mode. The updater also refuses in words
now, rather than as `-1`, when the heap or the largest free block is too small
before it starts.

The updater task's stack is 16 KB, not 20: a task stack comes out of the same
heap the handshake then has to allocate from, so the margin is not free.

Only plain application images are offered for OTA. Assets whose names contain
`bootloader`, `partition`, `littlefs`, `spiffs`, `factory` or `merged` are
skipped — a factory image starts with the bootloader at `0x1000` and passes the
`0xE9` magic-byte check, so an OTA would write one into the application slot and
then fail to boot from it.

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

### Switching the panel off

Dimming and the screensaver both keep the display lit; they only keep the lit
pixels moving, which is a burn-in measure and nothing more. **Settings → OLED
display → Switch the panel off** turns the display off at the controller, which
is the only thing that stops the panel ageing at all, and the only one that gets
back the ~15 mA it draws — the reason to want it on a battery.

Three choices, one at a time:

| | |
|---|---|
| **Always on** | The panel never switches itself off. This is the default. |
| **Off when idle** | Off once the timeout passes with nothing playing and nobody touching it. Audio holds it open, so a playing speaker keeps its display however long the album is. |
| **Off on a timer** | Off once the timeout passes since the last thing *you* did — a button press, a dashboard action, a serial command. Playback does not hold it open, so it goes dark mid-track. |

That difference is the whole reason there are two of them, and why the firmware
keeps two idle clocks rather than one: the first reads the clock audio keeps
warm, the second reads the one only the owner touches.

**What counts as playing.** The analyser, and only the analyser, plus the
DFPlayer asked directly. That is the whole rule, and getting to it took removing
two things that looked like better answers and were not.

The first was the A2DP transport state. `streaming` means the AVDTP stream is in
STARTED, which is not the same as audio existing: pausing on a phone normally
leaves the stream started and simply sends silence, and plenty of phones never
signal a state change at all. So a paused speaker reported itself as playing from
the first track until the phone disconnected, and the panel never blanked however
short the timeout was set.

The second was inside the probe. It re-analyses the newest window of its ring
buffer, and nothing clears that buffer when playback stops — the writer is a hot
path with no "stopped" hook to hang it on. So the last few milliseconds of the
last track were re-reported as live audio for as long as the speaker stayed
powered. The probe now watches its own write counter instead: a quarter-second
with no new samples is a stopped stream, and the window is treated as the silence
it is. That one costs the writer nothing.

The third was the test itself, and it was the one that mattered. `band_tilt`
lifts the high bands by up to 11 dB so a spectrum of real music fills the display
evenly instead of sloping away to the right — a cosmetic, and applied *after*
`to_db()` has already clamped at `FLOOR_DB`. Digital silence therefore did not
come out at the floor; it came out at the floor plus the tilt, which at the top
band is −67 dBFS. Tested against `FLOOR_DB + 8` (−70), that is above the
threshold, so **silence read as audio** — `active` was true from boot, in every
mode, with nothing connected and nothing playing. Nothing downstream could have
been right: the idle timers never counted, and the visualiser screens were always
eligible. The tilt stays where it is for the bars; the test reads the untilted
peak, and the threshold has a name (`ACTIVE_ABOVE_FLOOR_DB`) now that it is
capable of being false.

What is left is honest, because it is looking at the samples, and every source
that passes through this chip feeds it — A2DP, the network player, the chimes.
The timers read its last-heard timestamp rather than its instantaneous flag, so a
fade or the gap between two tracks does not read as *stopped*
(`UI_AUDIO_GRACE_MS`, four seconds). DFPlayer audio never reaches it at all, so
both timers also ask `df_player_active()`; without that the panel would blank
mid-track in the one mode where it is most obviously playing.

Both countdowns are reported on the card, along with the analyser's own reading
— *nothing has played for 2m 14s; nobody has touched it for 6m 52s. Analyser
peak −78 dBFS* — and the ring's countdown on the Lighting page. Silence reads
about −78 and anything playing reads well above it, so a panel that will not
blank is now a thing you can look at rather than guess about: a peak stuck high
with nothing playing is the analyser, a peak at the floor with the timer stuck at
zero is not.

Timeouts run from ten seconds to twelve hours. Both modes are suspended while a
system overlay is up — an update, a restart, the factory-reset countdown — since
those are the moments somebody is watching the panel, and going dark through one
is indistinguishable from a crash. Anything wakes it: the BOOT button, a
dashboard action, the serial console, a Bluetooth connection. With the panel
dark the first press of BOOT only brings it back and does not also change
screen, since a screen you never saw is not one you asked for.

### Controls

The **BOOT button** on the dev board (GPIO0) is the only control, and it needs no
wiring — it means nothing to the bootloader unless RESET is held at the same
time.

| Press | Action |
|-------|--------|
| short (< 600 ms) | next screen, or confirm a pending mode switch |
| long (600 ms – 1.5 s) | pin the current screen / release it |
| hold 1.5 s | brightness: low → mid → high |
| hold 3 s | offer the next radio mode — release, then press once to confirm |
| hold 6 s | factory reset countdown — release to cancel |

Keep holding past the mode offer and the panel starts a five-second countdown with a shrinking progress bar. Let go at any point and it says
*Cancelled* and nothing happens. Hold it to zero and every setting, the
dashboard password and every Bluetooth bond are wiped, then the speaker
restarts. On a board built without a display the same hold works with the LED
strobing instead of a countdown.

The restart deliberately waits for you to let go of the button: GPIO0 is also
the download-mode strap, and rebooting while it is held drops the chip into the
ROM serial bootloader, where it looks bricked.

Set `PIN_UI_BUTTON` to `-1` in [src/ui_config.h](src/ui_config.h) to disable it.

Everything is also reachable from the serial monitor — type `help`:

```
next                   next screen
screen 0..6            hold one screen (0 now playing, 1 spectrum, 2 VU,
                       3 scope, 4 waterfall, 5 clock, 6 info)
auto                   resume the carousel
bright 0..255          fix the contrast (0 = back to automatic)
ui                     current screen, frame rate, style
radio                  current mode and radio state
mode                   next radio mode, reboots
bt                     switch to Bluetooth only mode, reboots
wifi                   switch to Wi-Fi only mode, reboots
both                   switch to Wi-Fi + Bluetooth mode, reboots
net                    switch to Wi-Fi + BLE mode, reboots
sd                     switch to DFPlayer mode, reboots
pair                   force Bluetooth discoverable again
play <url>             play a network stream (Wi-Fi + BLE mode)
stop                   stop network playback
df                     DFPlayer status in one line
df play [n]            resume, or play track n
df pause | stop | next | prev
df vol 0-30            module volume
df folder F T          play /0F/00T.mp3
df source sd|usb|flash|aux
df eq 0-5              normal pop rock jazz classic bass
df loop off|track|folderN|all|random
df io1[long] | io2[long] | key1 | key2   press the module's own inputs
df led auto|on|off|blink
df reset | standby | wake | refresh
bat                    battery voltage, percentage and state
bat calib <volts>      trim the gauge to a meter reading (not saved)
```

`bat calib` applies the trim but does not store it, because the console has no
password on it; **Settings → Battery → Calibrate against a meter** does the same
calculation and saves the result.

### The status LED

The on-board LED (GPIO2 on most WROOM-32D devkits) is the only indicator the
bare board has, so it carries a pattern per state rather than a single bit. Each
pattern repeats every two seconds:

| Pattern | Meaning |
|---------|---------|
| solid | booting, or audio is streaming |
| almost solid, one short wink | a phone is connected but not playing |
| one short flash | up and waiting — discoverable, or Wi-Fi connected and idle |
| even 500 ms blink | joining the saved Wi-Fi network |
| three quick blinks, then dark | the setup hotspot is open and waiting for you |
| fast strobe | writing flash — do not remove power |
| two double blinks | an update failed |
| two slow winks | the source has nothing to play from — no card, or the card is mounted on a computer |
| long flash then a short one | the battery is below the critical threshold |

The battery pattern outranks everything, in every mode including *Bluetooth
only* where the LED otherwise belongs entirely to the audio state — a cell about
to cut out is the one fact that matters more than what is playing. It is
suppressed while the charger says it is charging, so a speaker being fixed stops
shouting about it.

Events are shown as a short burst of fast blinks over whatever pattern is
running: two on a phone connecting or a Wi-Fi client joining, three on a
disconnect, one on a track change. Override `PIN_STATUS_LED` or
`STATUS_LED_ACTIVE_HIGH` from `build_flags` for boards that wire it differently.


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
  The expensive half — Hann window, 256-point FFT, log-spaced banding, auto-gain,
  peak hold, beat detection — happens on whichever renderer asks for it first.
  The display and the WS2812 ring both watch it and share one FFT between them;
  see [One analysis, two watchers](#one-analysis-two-watchers).

The renderer runs on **its own task, pinned to core 0 at priority 1**. Pushing one
512-byte frame over I2C blocks for 7–12 ms, and doing that in `loop()` would
fight with the melody playback that also lives there. Down on core 0 at the
bottom of the priority list, it gets preempted by anything that matters, so the
animation degrades instead of the audio.

## The RGB ring

Seven WS2812 pixels on one wire, fifteen effects, both colours picked from the
dashboard, and a music sync that runs off the same analysis the spectrum screen
draws. Wiring and the current budget are under
[WS2812 ring → ESP32](#ws2812-ring--esp32); this is what it does.

### The effects

The first six ignore the audio, the next four are decorative, and the last four
are driven by the music outright. **Off** is an effect rather than a separate
flag, so "off" survives a reboot like any other choice.

| # | Effect | What it does |
|---|--------|--------------|
| 0 | Off | Dark. The ring stays configured and comes back where you left it |
| 1 | Solid | The picked colour, steady |
| 2 | Breathing | The picked colour, rising and falling |
| 3 | Rainbow | The full spectrum wrapped round the ring, rotating |
| 4 | Colour cycle | The whole ring on one hue, drifting through the wheel |
| 5 | Strobe | Hard flashes — **on the beat** once reactivity is up, otherwise on a timer |
| 6 | Comet | A bright head chasing round the rim, trailing a tail |
| 7 | Chase | Theatre chase: every third pixel marching round |
| 8 | Twinkle | Random sparks rising and fading, in both picked colours |
| 9 | Fire | Cool/diffuse/spark flicker, in the hue you picked. Bass feeds the flare |
| 10 | Gradient | A smooth sweep between your two colours, rotating |
| 11 | VU meter | The rim fills with the level, the centre thumps with the bass |
| 12 | Spectrum | One frequency band per pixel; bass red through treble violet |
| 13 | Beat flash | Dark between beats, a burst of colour on each one |
| 14 | Music sync | The rim is a circular spectrum, the centre is the bass, the hue steps on every beat |

### How the music sync works

[src/audio_probe.h](src/audio_probe.h) already produces 32 log-spaced bands,
stereo VU levels and a beat flag for the spectrum screen. The lighting reuses
that rather than running a second FFT, which is why it reacts identically to
Bluetooth, to a network stream and to the start-up chimes — all three feed the
same probe.

Three signals are pulled out and smoothed again, more gently, because what reads
well as a 32-bar graph is twitchy as a light: **loudness**, **bass**, and the
**beat** flag, latched on its edge and used as a trigger rather than a level.

**Reactivity applies to every effect, not just the reactive ones.** That is the
slider worth understanding. At 0 each effect runs exactly as it would in silence,
which is what you want from a lamp. Turn it up and the global brightness starts
following the music, so even a plain rainbow breathes with the track — and the
strobe stops running on a timer and starts firing on the beat. The four
music-driven effects opt out of that global dimming, because they have already
spent the audio on something more interesting than overall brightness.

**One mode has nothing to react to.** In DFPlayer mode the module decodes its own
card and hands out analog audio that never passes through the ESP32, so there is
nothing to analyse and the reactive effects rest at an idle breath rather than
freezing. The dashboard says so rather than leaving you to work it out.

### One analysis, two watchers

The display wants the FFT at 30 fps and the ring at 60, they run on separate
tasks, and **neither is guaranteed to exist** — there may be no panel, no ring, or
neither. So the analysis is a service rather than something one task owns:
whichever caller arrives first in a given window does the work and publishes the
result, and everyone else is handed the published frame. That keeps the cost at
one FFT per window however many tasks are watching, and keeps the lighting
reactive on a speaker with no display at all.

The published frame is copied out under a **seqlock**, the same trick
[src/player_state.h](src/player_state.h) uses, so a reader never sees half of one
frame and half of the next. The analysis is serialised by a mutex that callers
only ever *try* to take: a task that finds the FFT already running takes the
previous frame and gets on with drawing, rather than blocking a renderer for it.

### From the dashboard

**Lighting** in the sidebar. Master switch, the effect grid, both colour pickers
with a row of preset swatches, and brightness / speed / music-reaction sliders.
Changes apply within a frame; the flash write is deferred until you stop moving
the controls, so dragging a slider is not 60 NVS writes. There is nothing to
press.

The effect list the page draws comes from the firmware, not from the page, so
adding an effect stays a one-file change in [src/leds.cpp](src/leds.cpp).

**Resting.** The card at the bottom of the page puts the ring out when the
speaker is not being used: with it on, the ring goes dark once the timeout
passes with nothing heard and nothing changed, and comes straight back on the
first note or the first change made on the page. Off — the default — keeps it
lit for as long as the speaker is powered.

This is deliberately not the same thing as the master switch at the top. That
one is you saying the ring should be off, and it survives a reboot as exactly
that. Resting is the ring waiting between uses: the effect and both colours are
kept, the page goes on showing them, and the master switch stays on throughout.

The timer runs off the same analysis the effects react to, so it hears
Bluetooth, network audio and the start-up chimes alike, and it also asks the
DFPlayer directly — that module decodes its own card and its audio never passes
through this chip, so nothing else here would know it was playing. See *What
counts as playing* under the display, which covers both timers.

### From the serial console

```
leds                     status, and the list of subcommands
leds on | off            switch the ring on or off
leds list                every effect with its description
leds fx 14               choose an effect by number
leds color 00E0FF        primary colour, hex, no leading #
leds color2 FF0080       secondary colour
leds bright 0..255       brightness
leds speed 0..255        effect speed
leds react 0..100        how much the music is allowed to show
```

Anything set here is written to NVS straight away, so it survives a reboot like a
dashboard change would.

## The clock

The ESP32 has no battery-backed RTC of its own, so the clock is a software one
with a DS3231 module bolted onto the display's I2C bus to give it something that
survives a power cut — but you should never have to set it. Whenever the speaker is in management mode and its Wi-Fi station
has an address, it starts SNTP and keeps the clock on network time for as long
as the link is up. That covers boot and every reconnection after it; there is
nothing to configure and no build flag involved.

Bluetooth only mode has no Wi-Fi at all, so the clock there runs on what the
last sync left behind in NVS and the RTC, and it is right again the next time
you switch back. Both Wi-Fi + Bluetooth and Wi-Fi + BLE keep SNTP running like
Wi-Fi only mode does, so a speaker left in either never drifts.

### Time zone

Network time arrives as UTC, so the speaker needs to know its offset to show a
local wall clock. It learns one: **Settings → Sync browser time** sends the
browser’s UTC offset along with the time, and that offset is stored and used for
every sync from then on. Press it once after setting the speaker up and the
matter is closed. `CLOCK_TZ_OFFSET_MIN` in [src/ui_config.h](src/ui_config.h)
(minutes east of UTC) is only the starting value, for a speaker that is never
opened in a browser.

### Without a network

The clock still works offline, from whichever of these you set up:

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

**3. A DS3231 module** on GPIO21/22, the same two I2C wires as the display — the
only option that properly survives a power cut. About a euro, keeps time for
years on its coin cell, no extra GPIO. Wiring is under
[DS3231 RTC → ESP32](#ds3231-rtc--esp32), and the driver is **on by default**:

```ini
build_flags = ${env.build_flags} -DUSE_DS3231=1
```

Set that to 0 if you have no RTC fitted and would rather not carry the code; the
probe at boot is otherwise harmless, and a board without the module simply logs
`no ds3231 on the bus` and carries on.

It is read at boot and written whenever you set the time by hand — and after
every network sync — so it only needs setting once ever. A chip that has lost its
cell (which reads back as 2000-01-01) is detected and seeded rather than
believed.

Whichever it is, the current time is written to NVS every ten minutes, so a power
cut comes back within ten minutes rather than back to the build stamp. The saved
value is UTC; firmware older than the automatic sync saved local time under a
different key, so the first boot after upgrading falls back to the build stamp
until the first sync lands.

**Settings → Clock** owns the rest of it. It shows the speaker's own time next
to the source it came from, switches between 24-hour and 12-hour with AM/PM —
which the OLED clock, the pairing screen and the screensaver all follow — and
carries the switch for network sync. Turning that off stops SNTP: the clock
then keeps whatever you last set and nothing corrects it, which is what you
want on a speaker whose time you set by hand and not otherwise — the update
check validates GitHub's certificate against this clock, and a clock that has
drifted far enough fails the handshake. All three apply immediately; there is
nothing to save. `-DCLOCK_24H=0` in [src/ui_config.h](src/ui_config.h) is only
the starting value, for a speaker that is never opened in a browser.

## The battery gauge

Reading a battery with an ADC is easy; getting a number worth showing is not, so
it is worth being clear about which of the two this is.

**What it does well: resting voltage.** The divider is read nine times, the
*median* is taken — the ESP32's SAR ADC produces the occasional wild sample and a
mean carries it through — and the result is smoothed with a slow EMA.
`analogReadMilliVolts()` applies the chip's factory ADC calibration, worth about
40 mV over converting the raw count by hand. That gives a voltage good to a few
tens of millivolts once the trim is set, which is the accuracy limit of the
resistors rather than of the converter.

**What no voltage gauge does well: percentage under load.** A Li-ion cell's
terminal voltage sags with current, so a speaker that starts playing looks like
it lost 10% and gets it back when the track ends. That is physics, not a bug — a
coulomb counter is the fix and this board does not have one. Two things keep it
presentable: the curve is the *loaded discharge* shape rather than the
open-circuit one, and the smoothing is slow enough that the number does not jump
around. Read it as "roughly how full", which is what a battery indicator is for.

**The curve.** A straight line from empty to full is the obvious thing and is
wrong in the way that matters: a Li-ion cell spends most of its charge between
3.9 V and 3.6 V, so a linear gauge shows 50% for an hour and then falls off a
cliff. [src/battery.cpp](src/battery.cpp) instead interpolates between fourteen
knees of a moderately loaded 18650 discharge — each one a number you can check
against a datasheet curve rather than a polynomial nobody can audit.

The table is fixed and the configurable end points are applied *afterwards*, by
rescaling: the curve is evaluated at the cell voltage, at `full` and at `empty`,
and the answer is where the first sits between the other two. So a pack
deliberately charged to 4.10 V — or to 3.90 V, which is where longevity really
lives — reads 100% when it is as full as it gets and 98% just below, rather than
jumping from 100% straight to whatever the raw curve happens to say. Full and
empty are **per cell**, because that is what the curve consumes; the divider
reports pack volts and the cell count converts between them.

**Charge state comes from the charger, not from the voltage.** A cell resting at
4.15 V and a cell being topped up at 4.15 V are indistinguishable from the
voltage alone, so without the TP4056's CHRG and STDBY pins the state is reported
as *unknown* and the dashboard says why. Guessing would mean the indicator lies
at the one moment anybody is watching it.

**It starts switched off.** A sense pin with no divider on it floats, a floating
input invents readings, and one that happened to settle inside a cell's voltage
window would have the speaker insisting on a critically flat battery — flashing
the status LED about it, in every mode, on a board with no battery in it. So the
firmware never assumes a pack is fitted. The pin is configured, the Overview card
shows *Gauge off* and points at the switch, and **Settings → Battery → Battery
gauge enabled** starts it reading. Nothing has to be rebuilt, and turning it off
again clears the reading rather than leaving a stale *critical* behind.

**Where it shows up.** The Overview page gains a battery card — percentage, a
bar, voltage, state, and the reason when something is off. The OLED's Info screen
draws a battery glyph with a fill level, and the rotating stats line carries the
voltage and state; a broken sense wire draws a crossed-out battery rather than an
empty one, because "no cell" and "flat cell" are different problems. The status
LED gets its own pattern below the critical threshold. `bat` on the console
prints everything including the raw millivolts at the pin, which is the number
you want when the divider is wrong.

**Calibration.** Put a meter across the pack, type what it reads into **Settings
→ Battery → Calibrate against a meter**, and the trim that makes the firmware
agree is computed and stored. A target more than twice what the divider suggests
is refused rather than stored, because that is a wrong divider ratio rather than
a tolerance to trim out.

**Nothing is switched off automatically at any threshold.** That is deliberate: a
reading that sagged under load should not cut the speaker out mid-track, and a
firmware that powers down a speaker on an ADC reading is a firmware that
sometimes powers it down for no reason. The gauge warns — on the display, the LED
and the dashboard — and leaves the decision to whoever is listening. Cell
protection belongs to the protection circuit on the pack, which is where it can
actually be trusted.

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

## Troubleshooting the DFPlayer

| Symptom | Cause |
|---------|-------|
| dashboard says *not answering*, nothing plays | TX/RX not crossed over. The module's TX goes to `PIN_DF_RX` (GPIO16) and its RX to `PIN_DF_TX` (GPIO17). This is the failure, most of the time |
| answers, then stops answering | no 1k in series with the module's RX. It picks up switching noise and acts on frames that were never sent |
| the dashboard says *standby* and nothing plays | somebody pressed Standby. Press Wake, or Reset |
| plays for a second and cuts out, or resets the ESP32 | powered from 3.3 V, or from a supply that cannot carry the card's peak draw. The module wants 4–5 V and a real 500 mA |
| *No files found on this source* | not FAT32, larger than 32 GB, or the files are not zero-padded the way the module's own parser needs (`/01/003.mp3`, not `/1/3.mp3`) |
| the file count is higher than the number of songs | a Mac wrote `._*` and `.Trashes`. The module counts them; delete them |
| the flat track index plays the wrong song | that index follows FAT directory order, which changes when you re-copy the card. Use folder + track |
| *card mounted on a computer* and playback stops | a USB cable to a PC is plugged into the module. The card belongs to the computer while it is there; eject and unplug |
| no sound but the module says *playing* | the summing network. Check the 10 µF and the 10k on each channel, and that the module's grounds are joined to the DAC's. Also check *Module output enabled* on the Media page — it is the module's own DAC mute |
| audible hum only in this mode | the DFPlayer and the ESP32 sharing a thin ground. Star-ground both to the supply rather than daisy-chaining |
| hardware pin buttons work, serial does not | the module is alive and the UART is not. Confirms a wiring fault rather than a dead module |

## Troubleshooting the battery gauge

| Symptom | Cause |
|---------|-------|
| *Not detected*, and the hint shows a low millivolt reading | no divider, wrong pin, or the pack is disconnected. `bat` on the console prints the raw millivolts |
| the voltage is about half or double what the meter says | the divider ratio does not match the resistors. 100k/100k is 2.0; fix the ratio before calibrating |
| the percentage is 30–60 mV out | 5% resistors. Calibrate against a meter |
| the percentage drops sharply when playback starts and recovers after | the cell sagging under load. Expected; see *The battery gauge* above |
| the state is always *On battery*, even on a charger | the CHRG and STDBY pins are not wired. Voltage and percentage are still right |
| the card says *Gauge off* | that is the default. **Settings → Battery → Battery gauge enabled** |
| a 2S pack reads 100% at every voltage | full/empty were typed as pack voltage. They are **per cell**: 4.20 and 3.30 for a 2S pack too, with *Series cells* set to 2 |
| the reading wanders while Wi-Fi is up | a sense pin on ADC2. Use GPIO32–39, which are ADC1 |

## Troubleshooting the RGB ring

**Nothing lights, and the log says `[leds] no RMT channel`.** Another driver has
taken every RMT channel, which in this firmware should not happen — nothing else
uses the peripheral. Check that `PIN_LEDS` is not also assigned to something else
in [src/hw_config.h](src/hw_config.h).

**Nothing lights and there is no `[leds]` line at all.** `LEDS_ENABLED` is 0, or
`PIN_LEDS` is -1. The Lighting page says so too.

**Red and green are swapped.** The module is RGB rather than GRB. Build with
`-DLED_STRIP_GRB=0`.

**The first pixel is wrong or flickers and the rest are fine.** The classic 3.3 V
data into a 5 V pixel problem. Fit the 330R in the data line if you have not, keep
the lead short, and if it persists either add a level shifter or drop the ring's
supply to about 4.5 V with a signal diode. See *Logic levels* under the wiring.

**Colours are right but the board resets, or the audio crackles when the ring is
bright.** The supply is sagging. Seven pixels at full white is about 420 mA;
power the ring from 5 V rather than the 3.3 V rail, fit the capacitor across it,
and lower `LED_BRIGHTNESS_MAX`.

**The music effects sit at a slow, dim breath and never react.** Nothing is
reaching the analyser. In **DFPlayer mode** that is expected and permanent — the
module's audio never passes through the ESP32. In any other mode, check that
something is actually playing; the Lighting page reports whether the analyser is
hearing anything.

**Everything reacts but it is too frantic, or too subtle.** That is the *Music
reaction* slider, not a bug. It is the depth of the effect, and it applies to
every effect rather than only the four music-driven ones.

## Troubleshooting the display

**Nothing on the panel, and the log says `[ui] no SSD1306 at 0x3C or 0x3D`.**
Nothing answered on the bus. Check SDA on **GPIO21** and SCL on **GPIO22**, and
that VCC is on 3.3 V. Swapped SDA/SCL is the other classic; the panel simply
stays silent. If you are coming from an older build of this firmware, SCL used to
be on GPIO19 and I2S DIN on GPIO22 — both wires moved, so check that the DAC's
DIN is on **GPIO23** as well, or you will have fixed the display and lost the
audio.

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
- `UI_BLANK_MODE_DEFAULT`, `UI_BLANK_AFTER_S_DEFAULT` / `_MIN` / `_MAX` — the
  panel blanking the dashboard then owns
- `PIN_UI_BUTTON`, `UI_BTN_LONG_MS`, `UI_BTN_HOLD_MS`
- `FFT_SIZE`, `VIS_BANDS`, `VIS_RANGE_DB`, `VIS_FALL_PER_S`,
  `VIS_PEAK_FALL_PER_S`, `VIS_PEAK_HANG_MS`, `VIS_AGC_RELEASE_S`
- `CLOCK_24H`, `CLOCK_TZ_OFFSET_MIN`

DFPlayer, battery and WS2812 knobs live in
[src/hw_config.h](src/hw_config.h), whose header comment carries the full wiring
for all three. All of them are `#ifndef`-guarded, so a variant board needs no
source edit:

```ini
build_flags =
    -DPIN_DF_BUSY=34          ; BUSY moved
    -DPIN_DF_LED=-1           ; no DFPlayer LED fitted
    -DPIN_BATTERY_CHARGING=36 ; TP4056 CHRG wired, with its 10k pull-up
    -DPIN_BATTERY_FULL=39     ; TP4056 STDBY likewise
    -DDFPLAYER_ENABLED=0      ; drop the driver, the mode and the Media page
    -DBATTERY_ENABLED=0       ; drop the gauge
    -DPIN_LEDS=19             ; WS2812 data on a different pin
    -DLED_COUNT=16            ; a 16-pixel ring
    -DLED_CENTRE_INDEX=-1     ; a plain strip, with no middle pixel
    -DLEDS_ENABLED=0          ; drop the lighting, its task and its page
```

- `PIN_DF_TX` / `PIN_DF_RX` / `PIN_DF_BUSY`, `PIN_DF_IO1` / `IO2`,
  `PIN_DF_ADKEY1` / `ADKEY2`, `PIN_DF_LED`, `PIN_DF_USB_DETECT` — `-1` disables
  any individual pin, and the dashboard greys out what it cannot drive.
- `DF_COMMAND_GAP_MS`, `DF_POLL_MS`, `DF_PRESS_SHORT_MS`, `DF_PRESS_LONG_MS`,
  `DF_ONLINE_TIMEOUT_MS`, `DF_VOLUME_DEFAULT`
- `PIN_BATTERY_SENSE`, `PIN_BATTERY_CHARGING`, `PIN_BATTERY_FULL`,
  `BATTERY_STAT_ACTIVE_LOW`
- `BATTERY_DIVIDER_DEFAULT`, `BATTERY_CALIBRATION_DEFAULT`,
  `BATTERY_FULL_V_DEFAULT`, `BATTERY_EMPTY_V_DEFAULT`,
  `BATTERY_LOW_PCT_DEFAULT`, `BATTERY_CRITICAL_PCT_DEFAULT` — only the
  factory-fresh values; everything here is editable from **Settings → Battery**
  and stored in NVS, so one firmware serves a 1S and a 2S build. The two voltages
  are per cell.
- `BATTERY_OVERSAMPLE`, `BATTERY_SMOOTHING`, `BATTERY_SAMPLE_MS`,
  `BATTERY_MIN_PLAUSIBLE_V` / `BATTERY_MAX_PLAUSIBLE_V` — the window a reading
  has to fall inside to count as a cell rather than as a floating pin
- `PIN_LEDS`, `LED_COUNT`, `LED_CENTRE_INDEX` (`-1` for a strip with no middle
  pixel), `LED_STRIP_GRB` (`0` if red and green come out swapped), `LED_FPS`
- `LED_BRIGHTNESS_MAX` — the hard ceiling every effect is scaled by, which is the
  current budget in disguise: 255 is about 420 mA for seven pixels, and the
  default 160 about 260 mA
- `LED_DEFAULT_EFFECT`, `LED_DEFAULT_BRIGHTNESS`, `LED_DEFAULT_SPEED`,
  `LED_DEFAULT_REACTIVITY`, `LED_DEFAULT_COLOR` / `LED_DEFAULT_COLOR2`,
  `LED_AUDIO_IDLE_MS`, `LED_IDLE_OFF_DEFAULT`, `LED_IDLE_AFTER_S_DEFAULT` /
  `_MIN` / `_MAX` — only the factory-fresh values; everything here is stored
  in NVS and editable from **Lighting**

To make the ESP32 forget the paired phone, call
`a2dp_sink.clean_last_connection()` once in `setup()`, flash, then remove it again.

## Source layout

| File | What is in it |
|------|---------------|
| [src/main.cpp](src/main.cpp) | A2DP sink, volume control, melodies, serial console |
| [src/status_led.h](src/status_led.h) / [.cpp](src/status_led.cpp) | the on-board LED: one blink pattern per state, plus event blips |
| [src/df_player.h](src/df_player.h) / [.cpp](src/df_player.cpp) | the DFPlayer Mini: the YX5200 protocol, its GPIOs, and the driver task that owns the UART |
| [src/battery.h](src/battery.h) / [.cpp](src/battery.cpp) | the battery gauge: median-filtered ADC, the discharge curve, charger pins |
| [src/hw_config.h](src/hw_config.h) | every DFPlayer and battery pin and tunable, with the wiring in the header comment |
| [src/management.h](src/management.h) / [.cpp](src/management.cpp) | Wi-Fi, authenticated API, Bluetooth/media control, OTA and GitHub updater |
| [src/web_assets.h](src/web_assets.h) | responsive dashboard source, gzip-embedded at build time; its `<script>` is syntax-checked by [scripts/embed_web.py](scripts/embed_web.py) before every build |
| [src/ui_config.h](src/ui_config.h) | every display, analyser and clock knob |
| [src/player_state.h](src/player_state.h) / [.cpp](src/player_state.cpp) | the shared "what is playing" model (seqlock) |
| [src/audio_probe.h](src/audio_probe.h) / [.cpp](src/audio_probe.cpp) | sample tap, FFT, bands, VU, waveform, beat |
| [src/soft_clock.h](src/soft_clock.h) / [.cpp](src/soft_clock.cpp) | timekeeping: build stamp, serial, DS3231, NTP, NVS |
| [src/ui.h](src/ui.h) / [.cpp](src/ui.cpp) | the screens, overlays, transitions, marquees, 7-segment |
| [src/leds.h](src/leds.h) / [.cpp](src/leds.cpp) | the WS2812 ring: fifteen effects, the music sync, the RMT wire protocol |
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
- The WS2812 ring has no library either. Its wire protocol is forty lines of RMT
  symbol building in [src/leds.cpp](src/leds.cpp), talking to the IDF's `rmt_tx`
  driver rather than to Adafruit NeoPixel or to the Arduino core's
  `rmtInit()`/`rmtWrite()` wrapper. Both of those choices are about **IRAM**, not
  taste — see below.
- **`build_unflags` turns off the PSRAM cache workaround.** The platform compiles
  every board the same way, which means with the erratum workaround for ESP32
  revisions below 3: a `memw` barrier after loads and stores, plus a matching
  multilib of libc. That bug only exists when external SPI RAM is in use, and a
  WROOM-32D has none — this board has 16 MB of *flash* and 520 KB of internal
  SRAM.

  It is switched off for **room**, not for speed. IRAM is the binding constraint
  on this chip: there are 128 KB, the Bluetooth controller alone holds 32 KB of
  them, and IDF maps the whole workaround build of libc in there as well. Adding
  the WS2812 driver overflowed the segment by 216 bytes and would not link.
  Dropping the barriers shrinks libc, and going straight to `rmt_tx` instead of
  the Arduino wrapper avoids dragging `rmt_rx.c` in for a direction the speaker
  never uses — another 523 bytes. Together that leaves about 780 bytes of IRAM
  free, where the alternative was a build that did not link at all.

  If you ever build this for a WROVER **and enable PSRAM**, delete the
  `build_unflags` block: there the workaround is doing a real job.
