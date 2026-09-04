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

Nor does audio have to arrive at all. In the two modes that bring up Wi-Fi the
speaker is an **internet radio**: favourites stored on the device, MP3 and AAC
decoded on the chip itself, ICY metadata on the OLED, and a jitter buffer whose
level you can watch. On top of that sit a **five-band equaliser** in the sample
path of every source, **spoken announcements** recorded into flash at build time,
a **smart alarm clock** with per-weekday scheduling and a wake-up fade, a **sleep
timer**, **two hours of graphed history** for voltage, temperature, memory and
signal, and **Home Assistant** over MQTT with the discovery documents that make
the speaker appear complete without anybody writing YAML.

Everything in that second paragraph needs the network, so all of it is confined
to **Wi-Fi only** and **DFPlayer + Wi-Fi** mode — with three deliberate
exceptions that work in every mode including Bluetooth, because they are the
ones that matter most when there is no dashboard to look at: the equaliser, the
alarm, and the announcements.

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

### Every pin at a glance

The full map, in one table. Each entry is overridable from `build_flags`, and
[src/pin_check.h](src/pin_check.h) asserts the whole set at compile time — a
clash or an impossible pin is a build error, not a silent misbehaviour.

| GPIO | Used for | Override | Notes |
|------|----------|----------|-------|
| 0  | BOOT button: screens, brightness, mode, factory reset, standby wake | `PIN_UI_BUTTON` | **Strapping pin.** Held low at any reset → ROM download mode. The firmware waits for release before every restart it controls |
| 2  | Status LED (on-board) | `PIN_STATUS_LED`, `STATUS_LED_ACTIVE_HIGH` | **Strapping pin.** Must be low or floating at reset; an LED to ground is |
| 4  | DFPlayer activity LED | `PIN_DF_LED` (`-1` = none) | ordinary output |
| 14 | DFPlayer ADKEY1 | `PIN_DF_ADKEY1` | driven open-drain; JTAG MTMS otherwise |
| 16 | DFPlayer TX → ESP32 RX (UART2) | `PIN_DF_RX` | |
| 17 | ESP32 TX → DFPlayer RX (UART2), **1 k series** | `PIN_DF_TX` | the resistor is not optional in practice |
| 18 | WS2812 ring DIN, **330 R series** | `PIN_LEDS` (`-1` = none) | RMT, not bit-banged |
| 21 | I2C SDA — OLED (0x3C) and DS3231 (0x68) | `PIN_OLED_SDA` | one bus, two devices |
| 22 | I2C SCL — same two devices | `PIN_OLED_SCL` | |
| 23 | I2S DIN → PCM5102A | `PIN_MAP_I2S_DOUT` | **not** GPIO22: I2C owns that |
| 25 | I2S LCK → PCM5102A | `PIN_MAP_I2S_LRCK` | also DAC1/ADC2 — unused as either |
| 26 | I2S BCK → PCM5102A | `PIN_MAP_I2S_BCLK` | also DAC2/ADC2 — unused as either |
| 27 | DFPlayer ADKEY2 | `PIN_DF_ADKEY2` | driven open-drain |
| 32 | DFPlayer IO1 ("previous") | `PIN_DF_IO1` | driven open-drain |
| 33 | DFPlayer IO2 ("next") | `PIN_DF_IO2` | driven open-drain |
| 34 | Battery divider tap | `PIN_BATTERY_SENSE` (`-1` = none) | **Input only, ADC1.** No internal pull-up |
| 35 | DFPlayer BUSY | `PIN_DF_BUSY` (`-1` = none) | **Input only.** The module drives it |
| 36 | TP4056 CHRG, if wired | `PIN_BATTERY_CHARGING` (default `-1`) | **Input only** — needs its own 10 k to 3V3 |
| 39 | TP4056 STDBY, if wired | `PIN_BATTERY_FULL` (default `-1`) | **Input only** — needs its own 10 k to 3V3 |

Free on this build: 5, 12, 13, 15, 19. GPIO1/3 are UART0 (the console and the
programming port) and GPIO6–11 are the module's own SPI flash.

#### The three rules a rewire has to respect

- **GPIO 6–11 are the flash.** Driving one stops the chip executing, because
  that is where the code is being fetched from. There is no configuration that
  makes them usable. `pin_check.h` refuses the build.
- **GPIO 34–39 are input only.** No output driver and no internal pull-up or
  pull-down at all — `pinMode(OUTPUT)` is accepted and does nothing, so a signal
  moved here simply never appears on the wire. They are the right place for the
  battery divider and for BUSY; they cannot carry I2C, I2S, the ring, or the
  DFPlayer's open-drain button lines. `pin_check.h` refuses those too.
- **The strapping pins are sampled on every reset, not only at power-on.**
  `esp_restart()` re-latches them. GPIO0 low at that moment puts the chip in the
  ROM serial bootloader, where it looks dead until it is power-cycled — which is
  why the factory-reset hold and the standby wake both wait for the button to be
  released before restarting. GPIO12 (MTDI) is the one to avoid entirely: it
  selects the flash regulator voltage, and holding it high can stop a 3.3 V
  module booting at all.
- **The battery gauge must be on ADC1 (GPIO32–39).** ADC2 shares hardware with
  the Wi-Fi radio and stops answering whenever the driver is up — which is two
  of the three radio modes. The Arduino wrapper does not report the failure, it
  returns 0, so the symptom is a gauge that reads a flat pack whenever Wi-Fi is
  on and a correct one whenever it is not.

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
DFPlayer mode never starts the A2DP sink, and no other mode starts the
DFPlayer, so only one of the two sources is ever producing
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

pio run -t clean               # throw the build away and start again
pio check -e esp32dev --skip-packages   # cppcheck over src/ only
python scripts/test_pin_check.py        # the pin map, checked on the host
python scripts/test_settings_backup.py  # the settings backup format, ditto
python scripts/test_arabic_shaping.py   # Persian shaping and bidi
python scripts/gen_arabic_tables.py     # ...and its tables, vs Unicode
python scripts/test_stability_policy.py # rollover, bounds and memory policy
python scripts/test_audio_eq.py         # 1,000 stable EQ designs/rates/gains
```

There are three declared environments. Plain `pio run` builds all three;
`pio run -e esp32dev` builds only the development image.

Then on your phone: Bluetooth settings → pair with **"esp32-blue-spk"** → play.

### What a clean build should say

```
RAM:   [===       ]  28.3% (used 92884 bytes from 327680 bytes)
Flash: [====      ]  39.2% (used 2571268 bytes from 6553600 bytes)
```

The flash denominator is the 6.25 MB `app0` slot of the 16 MB partition table,
not the chip — see [16 MB flash layout](#16-mb-flash-layout). The RAM figure is
what the linker places statically; the heap is what is left of the 320 KB after
that, and the radios take most of it at run time. `diag` reports the live
numbers.

A clean build produces **no compiler warnings**. If one appears, it is new.

### Checking the pin map

Every GPIO is asserted at compile time by
[src/pin_check.h](src/pin_check.h): flash pins (6–11), input-only pins (34–39)
asked to drive, pins that do not exist on a WROOM, ADC2 used for the battery
gauge, and every pair that could collide. A violation is a build error that
names both ends, for example:

```
error: static assertion failed: GPIO conflict: I2S DOUT and I2C SCL are the same pin
```

`scripts/test_pin_check.py` compiles that header 21 times with different `-D`
overrides and checks that each one is accepted or rejected as intended, so the
assertions cannot quietly stop asserting. It needs no board and takes about a
second:

```
$ python scripts/test_pin_check.py
ok    stock pin map
ok    I2S data on a flash pin
...
all 21 pin-map cases behaved as specified
```

## Development and release builds

Three environments, and the difference is what the firmware is willing to say.

```
pio run -e esp32dev -t upload      # development: logs, console, diag
pio run -e release  -t upload      # release: none of the above
```

**`esp32dev`** is what you flash while you are working on the thing. It narrates
the boot, names every state change on the UART, offers a console that can drive
every subsystem by hand, and prints the full `diag` report.

**`release`** removes all three. Not silences — *removes*:

| | Development | Release |
|---|---|---|
| Serial log | every event | `LOGF`/`LOGLN` expand to nothing; the format strings never reach the image |
| Console | 30-odd commands | compiled out, and with it every `*_command()` handler, which loses its only caller |
| `diag` report | full | `diagnostics.cpp` compiles to nothing |
| IDF logging | `CORE_DEBUG_LEVEL=1` | `CORE_DEBUG_LEVEL=0` |
| `assert()` | active | `NDEBUG` |
| Flash clock | 40 MHz DIO | **80 MHz** DIO |
| Firmware image | 2,895,728 B | **2,805,392 B** |

You can check the last claim rather than believing it:

```
grep -ac "screen 0..7" .pio/build/esp32dev/firmware.bin    # 1
grep -ac "screen 0..7" .pio/build/release/firmware.bin     # 0
```

The saving is 90 kB of flash and 152 bytes of RAM, which is the honest shape of
it: the log was never a RAM cost. The reasons to build release are that a
production unit should not narrate itself to anybody holding a UART adapter, and
that **the console is an unauthenticated control channel** — mode switches, the
factory reset, the DFPlayer's entire command set — for anybody who can reach two
pads on the board. The dashboard behind it has a password. The console did not.

The BOOT button still cycles radio modes and still runs the factory-reset
countdown with no console at all, so a release unit is never stuck in a mode
whose dashboard it cannot reach.

**`release-verbose`** is the third environment, for the case that is genuinely
hard otherwise: a unit that misbehaves only in the field, where you want the
production timings and the production flash clock but you do want to know what it
says. Everything from `release`, with the log switched back on.

### The optimisation flags, and the one that is not there

`-O2` is **not** applied to the image, and that was measured rather than assumed.
Building the whole firmware at `-O2` instead of the platform's `-Os` grows it by
148 kB — and this chip executes from flash through a 32 kB instruction cache, so
nearly all of that growth is cold code evicting hot code. It is a plausible
optimisation that makes things slower.

What is applied:

- **`#pragma GCC optimize("O2")`** at the top of [src/audio_eq.cpp](src/audio_eq.cpp)
  and [src/voice.cpp](src/voice.cpp), the two translation units that are nothing
  but per-sample DSP. Cost: **544 bytes**, against 148 kB for doing it globally.
- **`-fno-math-errno`**, so the float paths emit the FPU instruction and nothing
  else. Deliberately *not* `-ffast-math`: that implies `-ffinite-math-only`,
  under which the `isnan()` guard on the temperature sensor folds to false and a
  broken reading becomes a number.
- **80 MHz flash clock.** Code runs from flash through the cache, so the SPI
  clock is a direct multiplier on how fast a cache miss is served. 40 MHz is the
  platform default because it is what every module is guaranteed to manage;
  80 MHz is what a WROOM-32D actually has fitted, and it is the largest speed
  change available here.

  It stays on **DIO**, not QIO. QIO is faster again — four data lines rather than
  two — but it needs GPIO9/GPIO10 free, and a module that has them wired for
  something else does not boot at all: no console, no dashboard, nothing to read.
  On a build whose whole point is that it does not talk, that is the wrong risk
  to take by default. The line is in `platformio.ini`, commented, if your board
  is known good.

`-flto` is not used: the Arduino core and IDF arrive precompiled, and link-time
optimisation across that boundary is a well-known source of silent breakage for
no measurable gain here.

## 16 MB flash layout

Three separate settings have to agree before a board actually gets its 16 MB,
and only one of them is the obvious one. All three are in
[platformio.ini](platformio.ini):

```ini
board_upload.flash_size = 16MB          ; the bootloader image header
board_build.partitions  = default_16MB.csv  ; the table itself
board                   = esp32dev      ; whose own JSON still says 4 MB
```

`board_upload.flash_size` is what writes the size nibble into the image header,
so it is what the bootloader believes; `board_build.partitions` is what decides
where anything actually lives. Setting only the second gives a partition table
that runs off the end of what the bootloader thinks the chip is. Setting only
the first gives 16 MB of flash with a 4 MB table on it — a board that boots, runs
and silently wastes three quarters of its storage.

`esp32dev`'s board definition still declares 4 MB, and that is fine: the two
overrides above take precedence, and PlatformIO recomputes the "maximum program
size" from the partition CSV, which is why a build reports **6553600 bytes** and
not 4194304.

The table is the Arduino core's stock `default_16MB.csv`:

| Name | Type | Offset | Size | What it is |
|------|------|--------|------|------------|
| `nvs` | data | `0x009000` | 20 KB | settings, Wi-Fi credentials, Bluetooth bonds, the saved clock |
| `otadata` | data | `0x00E000` | 8 KB | which application slot is current |
| `app0` | app, ota_0 | `0x010000` | 6.25 MB | slot A — where the firmware runs from |
| `app1` | app, ota_1 | `0x650000` | 6.25 MB | slot B — where an OTA writes |
| `spiffs` | data | `0xC90000` | 3.375 MB | reserved; this firmware does not mount it |
| `coredump` | data | `0xFF0000` | 64 KB | a panic dump, for `esp-coredump` |

`0xFF0000 + 0x10000 = 0x1000000` exactly — the table fills the chip with nothing
overlapping and nothing past the end. The application is currently about
2.6 MB, so slot A is ~39 % used and there is room for the OTA to write a much
larger image into slot B.

**OTA is real here, not nominal.** Two same-sized app slots, an `otadata`
partition to arbitrate them, and a firmware that writes with `Update.begin()`
and only restarts once `Update.end()` has validated the whole image. `diag`
prints the live table and marks the slot that is running.

### Verifying it on a real board

```sh
pio run -e esp32dev -t upload
pio device monitor
# then type:  diag
```

The report's `flash` line reads the configured size out of the image header and
the real size out of the chip's JEDEC id, and says so when they disagree:

```
flash         16 MB configured, 16 MB on the chip @ 40 MHz
```

A `<-- MISMATCH` there means `board_upload.flash_size` does not match the part
that is fitted.

> **Flash frequency.** This build clocks flash at 40 MHz, which is the
> `esp32dev` default. A WROOM-32D's flash is rated for 80 MHz, and
> `board_build.f_flash = 80000000L` roughly halves instruction-cache miss
> latency, which is free performance for code that runs from flash. It is left
> at 40 MHz here because it is a hardware-behaviour change that cannot be
> verified without the board in hand; try it, and if the board becomes unstable
> or fails to boot, put it back.

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

Twelve pages:

| Page | What is on it |
|------|---------------|
| **Overview** | the transport, the source card for whichever mode is running, the internet radio and alarm cards when either has something to say, the battery, the radio mode picker, firmware |
| **Devices** | Bluetooth pairings, and the addresses the announcement mapping needs |
| **Media** | the DFPlayer library and every one of its controls |
| **Radio** | internet radio: the favourites, the transport, the buffer level, the stream's own numbers |
| **Sound** | the five-band equaliser and its presets, the output level, and the spoken announcements |
| **Alarms** | five alarms, the sleep timer, and the time-zone rule the alarms depend on |
| **Lighting** | the WS2812 ring: effects, both colour pickers, and how much of the music is allowed to show |
| **Graphs** | two hours of voltage, chip temperature, free memory and Wi-Fi signal |
| **Wi-Fi** | scanning and joining a network |
| **Home Assistant** | the MQTT broker, and what the speaker publishes |
| **Updates** | the A/B firmware updater |
| **Settings** | identity, access, the OLED, power saving, standby, the battery gauge, backup and restore |

Pages that do not apply to the running mode say so and explain how to get to one
where they do, rather than showing dead controls — the Devices page in a mode
with no A2DP sink, the Media page in a mode with no DFPlayer, the Radio and Home
Assistant pages in a mode with no Wi-Fi.

The three that keep working everywhere are deliberate: the **equaliser**, the
**alarms** and the **announcements** are all in the sample path or on the wall
clock rather than on the network, so a curve dialled in over Wi-Fi and an alarm
set from the dashboard both still apply after you switch to Bluetooth mode,
where there is no dashboard at all.

### Three radio modes

This chip has a single 2.4 GHz front end shared by Wi-Fi and Bluetooth, and each
mode gives it to exactly one job. Whichever half of the Bluetooth controller a
mode does not use is handed back at boot, which is where the heap the dashboard
and the OTA download need comes from:

| Mode | Wi-Fi | Bluetooth | Audio arrives over |
|------|-------|-----------|--------------------|
| **Wi-Fi only** | station, or setup hotspot if no network is saved | not started at all; its DRAM goes back to the heap | **the internet**, as MP3 or AAC decoded on the chip |
| **Bluetooth only** | never initialised | A2DP sink owns the antenna | A2DP |
| **DFPlayer + Wi-Fi** | station, or setup hotspot; identical to Wi-Fi only mode | neither half is started; the *whole* controller goes back to the heap | **a microSD card or USB drive** over serial, **or the internet** |

Wi-Fi only mode used to have no audio path at all — it brought up a dashboard
and a clock and then sat there with an idle DAC. Internet radio is what fills
that column, and it needed no hardware that was not already fitted.

The mode is remembered across restarts, so a power cut brings the speaker back
doing whatever it was doing. A factory-fresh board starts in Wi-Fi only mode
with the setup hotspot up, which is where configuration happens.

A mode that fails to stay up twice in a row falls back to Wi-Fi only mode, where
the dashboard is reachable and the mode can be changed again. That sentinel lives
in NVS, so it survives the reboot loop it exists to break; DFPlayer mode is
covered by it like every other non-default mode.

#### DFPlayer + Wi-Fi

The only mode where nothing arrives over the air. A DFPlayer Mini holds the card,
decodes the file and produces its own analog output; the ESP32 does not touch the
audio at all. What it does is *control* — a 9600 baud serial link and six GPIOs —
and report what it learns.

That is also why this is a Wi-Fi mode rather than a radio arrangement of its
own.
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
mode in the cycle — Wi-Fi → Bluetooth → DFPlayer → Wi-Fi — so let go and press
BOOT once within eight seconds to confirm. Ignore it and it
goes away; keep holding and you are into the factory-reset countdown instead. On
a board with no display, a three-second hold released before six seconds
switches immediately.

**From the dashboard:** **Overview → Radio mode**, which lists all three and
marks the current one. Switching to *Bluetooth only* takes Wi-Fi and the page
with it, so it asks first; the others come back in a few seconds. *DFPlayer +
Wi-Fi* is greyed out only in a build compiled with `-DDFPLAYER_ENABLED=0`.

**From the console:** `mode` steps to the next one; `wifi`, `bt` and `sd` go
somewhere specific; `radio` prints where you are and what each half is doing.

Every switch goes through a restart. Each stack owns controller state, DMA
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
- a settings backup to download and restore, covering every stored preference,
  with the credentials in it encrypted under a passphrase of your choosing;
- firmware upload and background check/install from the latest GitHub Release.

### Backing up and restoring settings

**Settings → Backup & restore** downloads every stored preference as one JSON
file, and takes the same file back. It exists for the two cases that otherwise
mean retyping the whole Settings page: a board that has been erased and
reflashed, and a board that has been replaced.

What is in the file is everything `saveSettings()` puts in NVS — identity, the
Wi-Fi network, the GitHub release source, the DFPlayer start-up defaults, the
battery pack description, panel blanking, power saving and standby, the whole
ring configuration — plus the three clock preferences, which live in their own
NVS namespace. That is 42 keys, and
[scripts/test_settings_backup.py](scripts/test_settings_backup.py) holds it to
that: it diffs the backup writer against the restore reader key by key and
counts both against what `saveSettings()` actually stores, because a key added
to one and forgotten in the other is a setting that silently does not survive.

Two stored keys are left out on purpose. `radioMode` would let a file taken in
Bluetooth mode restore a speaker that boots with no dashboard to undo it from;
`bootFail` is the boot sentinel’s strike count, which describes a boot rather
than a preference.

#### The four secrets

A backup without the Wi-Fi passphrase, the dashboard password, the setup hotspot
password and the GitHub token restores a speaker that cannot reach the network
and does not answer to its own password — which is not a restore. Writing them
in clear text makes the file as dangerous as the credentials in it. So they
travel in an authenticated envelope under a passphrase you choose at download
time, and everything else in the file stays readable:

| | |
|---|---|
| key derivation | PBKDF2-HMAC-SHA256, 50 000 iterations, 16-byte salt from the hardware RNG |
| cipher | AES-256-GCM, 12-byte IV, 16-byte tag, no AAD |
| plaintext | a small JSON object of the four values |

GCM rather than CBC because the tag is what turns a wrong passphrase into a
clean refusal instead of four settings quietly restored as line noise. **The
envelope is decrypted and verified before any setting is touched, so a wrong
passphrase changes nothing at all.**

50 000 iterations is well below what you would use on a server, and that is a
deliberate trade: this runs on a 240 MHz microcontroller inside an HTTP handler,
where it is order-of-a-second with the SHA accelerator. The derivation yields
every 2 048 rounds so the web server, the audio path and the task watchdog all
survive it, and the restore reads the iteration count *out of the file* — so
raising the constant later never orphans an old backup.

**A blank passphrase does not mean "write them in clear text" — it means leave
them out.** The firmware never produces a plaintext secret. It will still *read*
one: a hand-written file, or a v1 backup from before the envelope existed, can
carry any of the four next to the ordinary settings and they are applied the
same way. That falls out of the format's one rule — only the keys present are
applied — and it is what lets you type a new Wi-Fi password into a backup
destined for a different network.

The key comes from the passphrase alone, not from anything on this chip. Binding
it to the eFuse MAC would be stronger against a stolen file and would also mean
a backup only ever restores to the board it came from, which is half the reason
the feature exists.

Nothing here is homemade except the iteration loop, which is plain RFC 2898 and
is checked against `hashlib.pbkdf2_hmac` — so a backup is readable by any
standard tool, not just by this firmware:

```python
import base64, hashlib, json
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

doc = json.load(open("esp32-blue-spk-settings.json"))
env = doc["secrets"]
b64 = base64.b64decode
key = hashlib.pbkdf2_hmac("sha256", b"YOUR PASSPHRASE",
                          b64(env["salt"]), env["iterations"], dklen=32)
print(json.loads(AESGCM(key).decrypt(
    b64(env["iv"]), b64(env["data"]) + b64(env["tag"]), None)))
```

#### Restoring

Restoring applies whatever keys the file carries, leaves the rest alone, writes
NVS and restarts — a restore changes the hostname, the network and the dashboard
password at once, none of which take effect without a reboot anyway. **If the
backup carries a different dashboard password, signing back in needs that one.**

Both ends are ordinary authenticated endpoints. Backup is a `POST` rather than a
`GET` so the passphrase never lands in a URL, and therefore never in browser
history or a proxy log:

```sh
# Download. Omit the passphrase and the four secrets are left out of the file.
curl -u admin:PASSWORD -X POST -H "Content-Type: application/json" \
     -d '{"passphrase":"correct horse battery staple"}' \
     -OJ http://esp32-blue-spk.local/api/settings/backup

# Restore, then the speaker restarts. The passphrase rides alongside the file.
jq '. + {passphrase:"correct horse battery staple"}' \
     esp32-blue-spk-settings.json |
  curl -u admin:PASSWORD -H "Content-Type: application/json" --data-binary @- \
       http://esp32-blue-spk.local/api/settings/restore
```

The file is pretty-printed on purpose: this is the one response here somebody
may want to open, compare against another speaker, or hand-edit before restoring
— which is most of the argument for encrypting the four secrets rather than the
whole document. A trimmed-down file is valid, so a single card can be copied
from one speaker to another by deleting the rest.

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

A classic ESP32 is Bluetooth 4.2: no BLE 5, no isochronous channels, and LE
Audio needs 5.2 silicon. BLE could therefore only ever have been a control
channel here, and the dashboard and the setup hotspot already are one — so the
controller is released outright in every mode that does not run the A2DP sink,
and the ~55 KB it was sitting on goes to Wi-Fi, the web server and the updater
instead.

Two smaller things fell out of the same heap accounting. I2S is opened before the radio
starts, because its DMA descriptors have to come from internal DMA-capable RAM
and that is the one pool nothing else can be traded for. And the updater refuses
in words now, rather than as `-1`, when the heap or the largest free block is
too small before it starts.

The updater task's stack is 16 KB, not 20: a task stack comes out of the same
heap the handshake then has to allocate from, so the margin is not free.

Only plain application images are offered for OTA. Assets whose names contain
`bootloader`, `partition`, `littlefs`, `spiffs`, `factory` or `merged` are
skipped — a factory image starts with the bootloader at `0x1000` and passes the
`0xE9` magic-byte check, so an OTA would write one into the application slot and
then fail to boot from it.

## The display

Ten screens. Eight of them rotate on a nine-second carousel, skipping any that
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

**Radio** — only in the modes that have one, and only once it is doing
something. The station's name, the now-playing text it sent, and a bottom row of
codec, bitrate and channels. The third row is the spectrum while it plays and the
**buffering bar** while it does not, with a tick at the level playback starts
from — without that mark "40%" means nothing, and with it the answer to "is it
nearly there" is one glance. The right-hand end shows the buffer level, or the
underrun count once there have been any, because that is the number that
explains the gaps.

```
 ⌂ SomaFM Groove Salad
 Tycho - Awake
      ▁▃▅█▆▄▂▁▂▄▆█▇▅▃▁▂▃▅▇  ▏
 MP3 128k stereo      buf 78%
```

**Clock** — seven-segment hours and minutes with a colon that blinks once a
second, the date beside it, and the seconds as a sweep along the bottom edge.
The whole layout shifts a pixel sideways as the minutes pass, because OLEDs do
burn in.

The bottom right carries **the next alarm** rather than the seconds whenever one
is armed — "is my alarm actually set" is a question people get out of bed to
check, and the blinking colon already says the clock is live. A bell with a line
through it is an alarm whose next occurrence has been skipped.

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
that passes through this chip feeds it — A2DP and the chimes.
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
sd                     switch to DFPlayer mode, reboots
pair                   force Bluetooth discoverable again
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


### Persian, Arabic, and other scripts

Track titles and device names arrive from a phone in UTF-8, and until recently
anything above U+00FF was replaced with a question mark on the way in — so a
Persian title reached the panel as `????????` and no font could have rescued it.
Codepoints now survive intact, and three things happen between the tag and the
glass.

**A font that has the letters.** The Latin fonts here are u8g2's `_tf` variants,
which stop at U+00FF: they carry an accented `e` and nothing whatever above it.
Arabic-script text is drawn with `u8g2_font_unifont_t_arabic` instead, which has
the whole Arabic block, the contextual shapes, and ASCII besides — so a title
mixing Persian and Latin needs one font, not two. It costs 8.9 KB of flash.

**Shaping.** Arabic script is cursive: every letter has up to four shapes —
isolated, initial, medial, final — and which is correct depends on whether its
neighbours join. Unicode keeps the letters in one block and the shapes in
another, so handing the letters straight to a font draws them all isolated,
legible only in the way `S O M E T H I N G  L I K E  T H I S` is legible. Some
pairs — lam followed by alef — must combine into one glyph and are simply wrong
apart. [src/text_arabic.cpp](src/text_arabic.cpp) does this.

**Ordering.** Persian reads right to left; u8g2 draws left to right. So the
string is handed over reversed — but only its right-to-left parts, because a
Latin word or a number embedded in a Persian title still reads left to right and
reversing those would turn `ESP32` into `23PSE`.

The one entertaining detail is farsi yeh, `ی`, the commonest letter in Persian.
Its own contextual shapes are U+FBFC..FBFF and **none of the three Arabic fonts
u8g2 ships contains them** — established by parsing `u8g2_fonts.c`, not assumed.
So it borrows shapes that do exist, and the borrowing is correct rather than
merely convenient: Persian yeh drops its two dots when isolated or final and
keeps them when initial or medial, so it takes the dotless alef-maksura shapes
(U+FEEF/FEF0) for the first two and the dotted Arabic yeh shapes
(U+FEF3/FEF4) for the other two.

#### What it does not do

This is not a full implementation of UAX #9, the bidirectional algorithm. There
are no explicit embedding controls, no bracket-pair resolution, and a neutral run
simply inherits the direction of whatever precedes it. That is deliberate: the
whole of UAX #9 arbitrates cases that do not arise in a track title on a 128×32
panel, and getting the common case right in a page of code is the better trade.
A string of Persian, a string of Latin, and either with the other embedded in it
all come out correct.

The real constraint is vertical. `unifont_t_arabic` is 16 px tall against the 6
to 8 px the rows on a 32 px panel were spaced for, so a Persian line takes half
the display. `draw_marquee()` pushes the baseline down far enough to keep the
glyphs on screen and clips each line to its own glyph box, so a tall line cannot
paint over its neighbours — but on the now-playing screen, which stacks four
rows into 32 px, a Persian title is close-fitting and may crowd the row beneath
it by a pixel or two. Latin rendering is byte-for-byte unchanged: the taller font
and the tighter clip only apply to strings that actually contain Arabic.

#### Checking it

Two host-side scripts, because the interesting failures are all silent ones:

```sh
python scripts/gen_arabic_tables.py     # tables still agree with Unicode
python scripts/test_arabic_shaping.py   # shaping is right, and the font has it
python scripts/test_arabic_shaping.py --art   # ...and draw it, to be looked at
```

`gen_arabic_tables.py` derives the shaping tables rather than trusting them: the
Presentation Forms-B block is laid out in letter order, so walking the base
letters reproduces all 140-odd shapes, and the walk is checked against sixteen
anchors from the Unicode charts. `test_arabic_shaping.py` checks the algorithm
against Persian words whose correct output was worked out by hand, and then
checks every shape the shaper can emit against the font's actual glyph table —
which is how the farsi yeh problem was found rather than shipped.

`--art` draws the result with the font's real bitmaps. That is the check the
codepoint assertions cannot make, and it is worth running once after any change
here: a table can be perfectly self-consistent and still be the wrong letter.

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
you switch back. DFPlayer mode keeps SNTP running exactly as Wi-Fi only mode
does, so a speaker left in it never drifts.

### Time zone

Network time arrives as UTC, so the speaker needs to know its offset to show a
local wall clock. It learns one: **Settings → Sync browser time** sends the
browser’s UTC offset along with the time, and that offset is stored and used for
every sync from then on. Press it once after setting the speaker up and the
matter is closed. `CLOCK_TZ_OFFSET_MIN` in [src/ui_config.h](src/ui_config.h)
(minutes east of UTC) is only the starting value, for a speaker that is never
opened in a browser.

That is enough for a clock on a shelf: it is wrong for half the year in most of
the world, and being an hour out on a display nobody sets an appointment by
costs nothing.

An **alarm** is a different matter. So the zone can also be given as a full
POSIX TZ rule — the same string `/etc/localtime` is compiled from — and newlib,
which is already linked in, applies the daylight-saving transitions itself:

```
CET-1CEST,M3.5.0,M10.5.0/3           central Europe
EST5EDT,M3.2.0,M11.1.0               US eastern
<+0330>-3:30<+0430>,J80/0,J264/0     Iran, which changes on fixed days
```

Pick one from the dropdown on the dashboard's **Alarms** page and the speaker
follows the changes on its own; `soft_clock_utc_offset_min()` keeps meaning what
it always meant and simply changes by itself twice a year, so nothing downstream
had to learn anything new.

There is no zone database on the device and there is not going to be one — it is
700 kB and it goes stale. The list of rules lives in the dashboard, which is to
say in the half of the system that is rewritten by every firmware update anyway,
and the speaker stores the one string it was handed. A rule that stops parsing
between firmware versions is rejected at boot and the clock falls back to the
fixed offset rather than taking the alarm down with it.

While a rule is installed it outranks a bare offset, and **Sync browser time**
will not quietly demote it — otherwise one press would undo the daylight-saving
handling and the next transition would be missed.

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
every network sync — so it only needs setting once ever.

#### When the RTC is not to be believed

Three different things can be wrong with a DS3231, and only one of them looks
wrong:

- **Never set.** The registers read back as 2000-01-01. Obvious, and the
  firmware seeds the chip from whatever better time it has instead.
- **Registers garbled.** A long lead, a weak pull-up, or a collision with the
  OLED's own 400 kHz traffic can return a month of 14 or a second of 92. BCD
  decoding does not complain about either, and `mktime()` would normalise the
  result into a date years away — which would then be adopted as trusted,
  written back into the chip, and saved to NVS. Every field is range-checked
  before any of it is believed.
- **The oscillator stopped.** This is the one that bites. A DS3231 whose backup
  cell has gone flat — or that had its cell removed and refitted — does *not*
  come back blank. It comes back with a perfectly plausible date that is simply
  wrong by however long it was stopped, and `CLOCK_SRC_RTC` outranks both NVS
  and the build stamp, so that wrong time wins.

  The chip says so itself, in the oscillator-stop flag (OSF) of register `0x0F`,
  and the firmware now reads it. A chip reporting OSF is ignored rather than
  adopted; NVS or the network provides the time instead, and the RTC is trusted
  again from the first real time set — which clears the flag on its way past.

`diag` reports which of these applies:

```
DS3231 RTC  oscillator stopped (check the coin cell)
```

> **The coin cell.** The common ZS-042 board wires a charging circuit meant for
> a rechargeable LIR2032. Most of them ship with a **non-rechargeable CR2032**
> in that socket, which the circuit then trickle-charges. Either fit a LIR2032
> or lift the charge resistor. A cell mistreated this way is the usual reason
> OSF turns up months later.

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

## Power saving

The parts of this board that draw current and are not the speaker: seven WS2812
pixels at up to 260 mA, an OLED panel at ~15 mA, an indicator LED, and a Wi-Fi
radio that by default never sleeps between beacons. Each already has its own
setting, and setting four of them by hand every time the pack gets low is not a
feature. **Settings → Power** is the one switch, and it is three modes, one at a
time:

| | |
|---|---|
| **Off** | Nothing is saved. Every setting is the one you chose. The default. |
| **Always saving** | Saving, on mains as well as on battery. |
| **Automatic** | Saving while the pack is at or below a level you set *and* nothing is charging it. |

**Charging wins outright.** A pack at 8% with a charger on it is a pack that is
getting better, and dimming the lights to protect it makes the speaker worse for
nothing — the mode exists to stretch the time until a charger arrives, and one
has. So *Automatic* releases the moment either charge pin says so, whatever the
percentage reads.

**Coming out needs more than going in.** The gauge is a voltage read off a curve,
and voltage moves when the load does — which is exactly what saving changes. A
pack sitting on the threshold would switch the ring off, sag less, read a point
higher, switch it back on, and oscillate. Saving therefore releases five points
above where it engages, and the card says so while it is in that band rather
than claiming to be below a threshold it is above.

**Automatic needs a gauge.** With the battery switched off in settings, no sense
pin compiled in, or no plausible cell voltage on it, there is nothing to read.
The mode reports itself inactive and says which of the three it is, rather than
guessing at a percentage that does not exist.

### What saving actually does

- **the ring** — off. By a wide margin the largest load on the board, and the
  reason the mode is worth having at all.
- **the panel** — held off, the same ~15 mA the blanking modes get back, taken
  at once rather than after a timeout.
- **the indicator LED** — held dark between states.
- **Wi-Fi** — modem sleep on and transmit power at 11 dBm instead of 19.5, which
  costs some dashboard latency and nothing else. Skipped entirely when no radio
  is up, which is every Bluetooth-only boot.

None of it writes a setting. All four come back exactly as you left them the
moment saving ends, and the sidebar says *Power saving* throughout so a dark ring
and a dark panel read as the mode doing its job rather than as a fault.

### Standby

Saving leaves a working speaker that costs less to run. **Standby** stops being a
speaker: the ring is written black, the panel is powered down, the DFPlayer is
stopped and put in its own standby, the indicator goes dark, Wi-Fi and Bluetooth
are both torn down, and the core drops to 10 MHz to watch one GPIO. Pressing
**BOOT** — held briefly, so a speaker in a bag that brushes it stays asleep —
restarts the board, because bringing the radios, the audio path and the module
back up from nothing is exactly what `setup()` does.

Three modes, the same shape as the rest: **Never**, **After the timeout**
(whenever nothing has played and nobody has touched it), and **Only while
saving** (the same, but gated on power saving actually being on — which is the
option for a speaker that should put itself away on battery and stay up on
mains). Timeouts run from one minute to twelve hours, and **Stand by now** does
it on the spot.

The external parts are the ones that need shutting down by hand, because nothing
about a quiet ESP32 reaches them. The WS2812s matter most: they *latch*. A ring
that is merely stopped holds its last colour forever, with no clock and no data,
drawing its full current on a board that is otherwise asleep — so it has to be
written black and then have its task parked, in that order.

**This is not `esp_deep_sleep_start()`, and the difference is real.** Deep sleep
would be tens of microamps against the ~15 mA this manages — five days on a
2000 mAh pack rather than effectively forever. It was the first thing tried. The
sleep entry path has to run with the flash cache disabled, so it lives in IRAM,
and it wants about 1.8 KB of it; this firmware has **653 bytes** of IRAM left.
The Bluetooth controller blob alone holds 33 KB there, which is the same ceiling
that makes the WS2812 driver forty lines of RMT rather than a library and that
switched off the PSRAM cache workaround in [platformio.ini](platformio.ini).
Linking deep sleep in overflows `iram0_0_seg` by 1012 bytes and the image will
not build. A build with Bluetooth compiled out has the room for the real thing;
that is the way in if the microamps matter more than A2DP does.

Against the ~260 mA the ring alone can draw, 15 mA is still worth having.

**What power saving deliberately does not touch is the CPU clock.** Dropping 240 MHz to 160
is the obvious next saving and it is not here: SBC decode with the Bluetooth
stack running has little headroom at 240 and none below it, and a power mode
whose symptom is crackle is not one anybody would leave switched on. This is the
same reasoning that keeps `CORE_DEBUG_LEVEL` at 1 — see the note in
[platformio.ini](platformio.ini). Everything the mode touches is peripheral
current, and none of it can reach a sample.

## The battery gauge

Reading a battery with an ADC is easy; getting a number worth showing is not, so
it is worth being clear about which of the two this is.

**What it does well: resting voltage.** One conversion is thrown away, the
divider is then read nine times, the *median* is taken — the ESP32's SAR ADC
produces the occasional wild sample and a mean carries it through — and the
result is smoothed with a slow EMA. `analogReadMilliVolts()` applies the chip's
factory ADC calibration, worth about 40 mV over converting the raw count by
hand. That gives a voltage good to a few tens of millivolts once the trim is
set, which is the accuracy limit of the resistors rather than of the converter.

**Why the first conversion is discarded, and the one capacitor worth fitting.**
100 k over 100 k puts the ADC behind a **50 kΩ** Thevenin source, and Espressif
recommend an order of magnitude less than that. The ESP32's front end is a
switched sample-and-hold: it connects a small capacitor to the pin for a fixed
acquisition window, and that charge has to arrive through the 50 kΩ. The first
conversion after the input multiplexer has been elsewhere therefore reads *low*
— the cap started at whatever the previous channel left on it and did not finish
charging. Back-to-back conversions on the same channel are fine, because the cap
is already close. Discarding the first one costs about 30 µs twice a second and
removes a systematic negative offset that no calibration trim can distinguish
from a genuinely lower cell.

The hardware fix is better and costs one part: **a 100 nF ceramic from GPIO34 to
GND, at the pin**. It turns the 50 kΩ into a local reservoir the sample-and-hold
can draw from instantly, and it also filters the switching noise the ring and
the DFPlayer put on the rail. Fit it if you can; the firmware is correct without
it.

```
BAT+ ---[100k]---+---[100k]--- GND
                 |
                 +--- GPIO34 (ADC1_CH6)
                 |
               [100nF]
                 |
                GND
```

Sizing check: 4.2 V through 2:1 is **2.10 V** at the pin, inside the ~2.45 V the
11 dB attenuator can read, with headroom. A 3S pack would put 12.6 V into a 2:1
divider and destroy the input — raise the divider ratio *and* the resistor values
together for anything above 1S, and confirm with a meter before connecting the
ESP32.

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

### What a TP4056 is not

A plain TP4056 board — the red one with the micro-USB socket, and the blue
DW01+FS8205 one with protection — is a **linear charger only**. It is not a
power-path controller and it does not do load sharing, whatever the listing
says. That has four consequences no firmware can fix, and they belong in the
wiring rather than in the code:

- **The load hangs off the cell, not off a switched output.** With USB plugged
  in, the charger, the cell and the speaker are all on the same node. The
  charger's constant-current loop cannot tell the difference between current
  going into the cell and current going out to the ESP32, so the cell never gets
  a clean charge profile while the speaker is running.
- **Termination is unreliable under load.** The TP4056 ends a charge when the
  current falls to about a tenth of the programmed rate. A speaker drawing more
  than that means the threshold is never reached, so it either never terminates
  or terminates on the wrong thing. STDBY going low while the speaker is playing
  should not be read as "the pack is full", and the firmware only reports `full`
  when STDBY *and* a percentage of 95 or more agree.
- **The percentage is wrong while charging, and knows it.** The gauge is reading
  a cell that is simultaneously being charged and discharged. The firmware
  handles this by not pretending: charging outranks everything in the power
  policy, so saving switches off and the percentage is shown for information.
- **A B+/B− board is not a protection circuit for the ESP32.** The DW01 on the
  protected boards protects the *cell* (over-discharge, over-current, short). It
  does nothing about the 3.3 V regulator's input, and a 1S cell at 4.2 V into a
  devkit's 5 V pin does not reach the regulator's dropout at all — you need a
  boost converter or a regulator that works from 3.0 V up, not a straight wire.

If you want proper behaviour on USB, that is an **IP5306**, a **TP4056 plus an
ideal-diode power path** (an LM66200 or equivalent), or one of the combined
charger/boost modules. This firmware works correctly with a plain TP4056; it
simply cannot make one into something it is not, and it reports *unknown* rather
than inventing a charge state it has no way to know.

> **Brownouts.** Bringing up Wi-Fi and Bluetooth together roughly doubles peak
> current, and the ring can add 260 mA more. On a marginal supply or a thin USB
> cable that is a brownout reset — which `diag` names explicitly, because a
> brownout, a panic and a watchdog all look identical from outside. Brownout
> detection is deliberately left on: it is protecting the flash from a write at
> an unsafe voltage, and switching it off converts a clean reset into a corrupt
> partition. The fixes are a thicker cable, a bulk capacitor at the board, and
> `LED_BRIGHTNESS_MAX` lower.

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


## The equaliser

Five biquad sections per channel, in the sample path of whatever is playing:

| Band | Kind | What it is for |
|------|------|----------------|
| 60 Hz | low shelf | the bottom, as a whole |
| 250 Hz | peaking | warmth, and where a small cabinet booms |
| 1 kHz | peaking | presence — vocals sit here |
| 4 kHz | peaking | articulation, consonants, snare attack |
| 12 kHz | high shelf | air, as a whole |

The two ends are shelves and the three in the middle are peaking filters, which
is the arrangement a physical five-band tone stack uses and for the same reason:
a peaking filter at 60 Hz leaves the bottom octave — where a small speaker has
nothing anyway — alone, while a shelf lifts the whole of it, which is what "more
bass" means to the person turning the knob.

Five presets, on the dashboard's **Sound** page and from the console:

| Preset | Shape |
|--------|-------|
| Flat | everything at zero |
| Music | a gentle smile: a little at each end, a shallow dip in the upper mid |
| Voice | bass and top cut, presence lifted at 1 kHz and 4 kHz |
| Bass Boost | a large low shelf, a small top lift, the middle left alone |
| Night | deep bass pulled down hard, mid brought up — a loudness curve run backwards, so speech stays intelligible at a volume that will not wake anybody |

Moving a slider makes the curve **Custom**; picking a preset again overwrites it.

**Where it runs.** From inside `LoudVolumeControl::update_audio_data()` for A2DP
and from the radio player's output stage for network audio — both after the
volume control, so the headroom is measured against what you are actually
hearing. It is single-precision float, about 4.4 million operations a second at
44.1 kHz stereo, which is roughly 2% of one core against the ~15% the SBC decoder
itself costs. With every band at zero it returns after one comparison, so an
owner who never opens the page pays nothing.

**Headroom.** Boosting a band pushes samples past full scale and a full-scale
stream has none to give. Two things guard that: an automatic preamp that pulls
the whole signal down by however much the largest boost adds, and the same
quadratic soft knee the volume path uses, so whatever still arrives over the
ceiling is rounded off rather than clipped flat. The automatic preamp can be
switched off from the dashboard by an owner who would rather have the loudness.

**In DFPlayer mode the module does the work instead.** Its audio never passes
through this chip — it comes out of the module's own DAC and joins at the jack —
so there is nothing here to filter. The chosen curve is mapped to the nearest of
the YX5200's six hardware presets and pushed over the serial link, and the
dashboard says so rather than showing five sliders that do nothing.

```
eq                     show the curve, the preamp and whether it is active
eq music               pick a preset (flat, music, voice, bass, night)
eq 1 +6                band 1 (60 Hz) to +6 dB
eq off                 bypass
eq auto                toggle the automatic preamp
```

## Internet radio

In **Wi-Fi only** and **DFPlayer + Wi-Fi** mode the speaker plays streams from
the network. The I2S channel is opened in `setup()` before the radio mode is even
consulted, so in Wi-Fi mode it is simply free — this is the audio source that
mode never had.

- **Codecs**: MP3 and AAC, through [libhelix](https://github.com/pschatzmann/arduino-libhelix).
  Between them they cover essentially all of Shoutcast and Icecast. Ogg and Opus
  are not supported: neither has a decoder that fits in the RAM this chip has
  left once Wi-Fi has taken its share.
- **HTTP and HTTPS**. TLS uses the same Mozilla root bundle the firmware updater
  trusts.
- **Twelve favourites**, stored on the device, editable and reorderable from the
  dashboard's **Radio** page. Four public stations are seeded on first boot so
  there is something to press.
- **ICY metadata**: the station name, genre and the now-playing text, on the
  OLED and the dashboard, plus the bitrate the station reports.
- **Automatic reconnection** on a backoff that widens to a minute and then stays
  there, forever. Giving up after five tries would mean a speaker that is silent
  when the network comes back an hour later.
- **Resumes after a power cut** — the station and the intent to be playing are
  both persisted.

### The buffer, and what the indicator means

A stream arrives in whatever lumps the internet felt like sending and a decoder
consumes it at a constant rate. Between them sits a 20 kB ring — a little over a
second of a 128 kbps stream — filled ahead before playback starts and topped up
between decodes. What the dashboard draws as a buffering bar is how full it is,
with a tick at the 55% mark playback begins from.

The number matters because it is the only warning that arrives *before* the
sound stops. A buffer that sits high is a healthy link; one that sags towards
zero is a connection that cannot keep up, and the underrun counter next to it
says how often it has already run dry. Below about −75 dBm of Wi-Fi signal a
stream starts running out of buffer before it runs out of bandwidth, which is
why the Graphs page plots the signal too.

**Where it runs.** On its own FreeRTOS task pinned to core 1, not in `loop()`.
That is not a preference: `loop()` serves HTTP, plays melodies and talks to the
DFPlayer, and any one of those blocks for tens of milliseconds at a time. On its
own task the decode is paced by the blocking I2S write itself — which is exactly
the clock a stream should run on — and the web server can take as long as it
likes.

**In DFPlayer mode both can make a sound**, since the module's analog output and
the PCM5102A meet at the same jack. Starting a station pauses the module first,
and that is the whole of the arbitration.

### What it costs when it is not playing

Nothing. This matters more than it sounds on a chip with 320 kB of DRAM and no
PSRAM, because the firmware updater needs a *contiguous* 45 kB block for its TLS
handshake against the Mozilla root bundle — and an early 24 kB allocation sits in
the middle of the heap splitting exactly the block the handshake wants. The first
version of this feature took its buffers and its task at boot in both Wi-Fi
modes, and the visible symptom was **Check for updates** failing with "not enough
memory for a secure connection" on a speaker that had never played a station.

So the radio is allocated on demand:

| | When it exists | Size |
|---|---|---|
| Station list | boot, in Wi-Fi modes only | 2.4 kB |
| Decoder task stack | the first station you play | 10 kB |
| Ring + socket chunk + decoder feed | one block, while a stream runs | 24 kB |
| MP3 or AAC decoder | while a stream runs | ~30 kB |

The arena is claimed *after* the TLS handshake for an https station, not before,
because a certificate walk is where an https connection's heap use peaks.

An update check **while the radio is playing** will still be refused — a stream
genuinely is holding ~52 kB — and the message says so and tells you to stop it,
rather than telling you to restart a speaker that would do the same thing again.

```
station                also prints the task's remaining stack, whether the
                       stream arena is held, and free/largest-block heap
diag                   the heap trio and every task's stack high-water mark
```

```
station                status, and the favourites with the playing one marked
station 3              play favourite 3
station http://…       play any stream address
station stop|next|prev
```

## Spoken announcements

The speaker can say things: "battery critically low", "Wi-Fi connected",
"connecting to the station", "good morning, your alarm is going off".

There is no synthesiser on the chip. Everything it can say was written into
[scripts/voice_phrases.txt](scripts/voice_phrases.txt), spoken by the Windows
speech engine, and embedded as IMA ADPCM by
[scripts/make_voice_clips.py](scripts/make_voice_clips.py) — about 355 kB of
flash for 45 seconds of speech at 16 kHz, at exactly 4:1 compression. The build
never runs that script; `src/voice_clips.h` is committed, so a build works on a
machine with no speech engine and produces the same bytes everywhere.

That trade buys the two properties that matter for the things worth announcing:
a battery warning works with **no network, in every radio mode including
Bluetooth**, and it sounds like a person. What it costs is equally plain — the
speaker cannot read out a name it has never been given.

### Changing what it says, and adding your own

```
notepad scripts\voice_phrases.txt          # id | the words to speak
python scripts\make_voice_clips.py         # respeaks and rewrites voice_clips.h
pio run -t upload
```

`--list-voices` prints the installed voices and `--voice "Microsoft David"`
picks one. The ids marked `[fixed]` in the file are referenced by the firmware
and should keep their names; everything else is yours.

This is also the answer to "announce which phone connected". Add a line —
`ash_iphone | Ashkan's iPhone is connected.` — regenerate, rebuild, then map the
phone's Bluetooth address to it on the dashboard's **Sound** page. Addresses are
on the Devices page.

### What may speak

Five categories, each switchable, because announcements differ enormously in how
welcome they are:

| Category | Default | What it covers |
|----------|---------|----------------|
| System | on | boot, shutdown, radio mode |
| Connections | **off** | a phone or the network coming and going |
| Battery | on | low, critical, charging, full |
| Internet radio | on | connecting to a station, and failing to |
| Alarms | on | the alarm, snooze, the sleep timer |

Connection announcements start off on purpose: a phone drifting in and out of
range at the edge of the house would otherwise have the speaker talking to an
empty room all afternoon, and that is the single most common reason somebody
switches a feature like this off and never switches it back on.

### How a clip reaches the speaker

Two paths, because there are two situations. With nothing playing, `loop()`
renders the clip into a buffer and writes it to I2S — the same shape the melodies
use. With music playing, the clip is mixed into the stream on the audio task and
the music is ducked underneath it, ramping over about 60 ms down and 170 ms back
up: fast enough to be out of the way of the first syllable, slow enough not to
sound like a fault. They share one decoder and one queue, and an ownership flag
decides which drains it, so a clip can never be played twice at half speed.

```
say                    the settings, and every clip in this firmware
say battery_low        play one
say off                switch announcements off
```

## The alarm clock and the sleep timer

Five alarms, on the dashboard's **Alarms** page and visible on the OLED's clock
screen. Each has its own days, its own source and its own wake-up ramp.

- **It wakes you with something.** An internet radio station, a folder on the
  DFPlayer's card, or the built-in chime. Waking to the news is a different
  thing from waking to a beep.
- **It fades up**, from near silence to the target over up to ten minutes. This
  is the single feature that makes an alarm pleasant and it costs a variable and
  a multiply.
- **It falls back.** If the station will not connect — the router is down, the
  stream moved — the chime takes over after twenty seconds. An alarm that
  silently fails to make a sound is not an alarm, and a network it depends on
  will eventually be down at 6 a.m.
- **It knows what day it is**: per-weekday scheduling, plus a one-shot mode that
  disarms itself afterwards.
- **It can be skipped.** Tomorrow's occurrence can be waved off without
  disarming the alarm, which is what everybody actually wants on a Friday night.
- **It stops itself**, after anywhere from five minutes to an hour, so a house
  nobody is in does not play the radio for a week.

Alarms work in **every mode**. The comparison is arithmetic on the wall clock and
the chime plays through the same I2S channel the melodies use, so an alarm set
from the dashboard in Wi-Fi mode still goes off in Bluetooth mode, where there is
no dashboard to set it from. Only the sources differ, which is also why the chime
is the fallback.

The **sleep timer** is the same idea pointing the other way: plays for as long as
you say, fades out over the last minute, and stops — optionally going into
standby proper afterwards, which is what you want overnight on a battery.

```
alarm                  the alarms, and the next one due
alarm off | snooze     stop or snooze a ringing one
alarm test2            fire alarm 2 now
sleep 45               start the sleep timer
sleep off              cancel it
```

### Why the time zone matters here

An alarm that fires an hour late on the last Sunday in March is not a clock that
drifted — it is an alarm that failed, and the owner finds out by oversleeping. So
the clock takes a full POSIX TZ rule as well as a plain offset; see
[The clock](#the-clock) below.

## Graphs

Two hours of history, sampled once a minute into a fixed ring of 120 — 1.4 kB of
heap, allocated once at boot and never grown.

Two things keep that endpoint cheap, and both were lessons. The handler walks the
ring in place rather than copying it, so there is no second buffer: the ring and
the web handler both run on the Arduino loop task, which is what makes that safe.
And it writes the JSON straight to the socket in chunks instead of building a
document — six arrays of numbers is several hundred ArduinoJson slots, and
serialising that into a String doubles it again while the String grows, which is
a transient spike of tens of kilobytes on the same heap the firmware updater
needs a contiguous block from.

The dashboard's **Graphs** page draws four of them:

| Series | Why it is worth a line rather than a number |
|--------|---------------------------------------------|
| Battery voltage | 3.8 V is fine or nearly flat depending entirely on which way it is moving |
| Chip temperature | a speaker cooking in the sun, or a charge current heating the board, shows up as a trend |
| Free memory | only ever diagnostic as a slope — a line drifting down is a leak, and the difference between a mysterious reboot tonight and a known cause |
| Wi-Fi signal | below about −75 dBm a stream runs out of buffer before it runs out of bandwidth |

The stretches where something was playing are shaded behind the curve, which is
usually the whole explanation for a battery line that suddenly steepens.

**About that temperature.** It is the ESP32's internal sensor, and it measures
the *die*, not the room. On the original silicon it is an undocumented ROM
routine with a coarse step and a per-chip offset nobody calibrated, and it reads
ten to twenty degrees above ambient after the radio has been busy. It is genuinely
useful for what it is good at — spotting a trend — and it is not a thermometer.
The dashboard says so next to the graph, and no decision in this firmware is made
from it.

The ring is in RAM and not in flash on purpose: this is for looking at a running
speaker, and writing a sample to NVS every minute would wear the chip out
inside a year for a history nobody reads after a reboot. Total runtime and the
boot count *are* persisted, once every ten minutes.

```
graph                  the three series as sparklines, with the ranges
```

## What all of this costs

This is a chip with 320 kB of DRAM, no PSRAM, and a firmware updater that needs a
*contiguous* 45 kB block for its TLS handshake. Every feature above was built
against that, and two of them were built against it twice — the first versions
took their memory at boot and the visible symptom was **Check for updates**
failing with "not enough memory for a secure connection" on a speaker that had
never played a station.

What the whole bundle adds, measured from the linked image:

| | Cost |
|---|---|
| Static DRAM, all six modules | ~3 kB |
| Static DRAM, whole development firmware | 89,684 B |
| IRAM, whole development firmware | 130,419 / 131,072 B — **653 B spare** |
| Heap at boot (Wi-Fi modes) | 3.8 kB — the history ring and the station list |
| Heap at boot (Bluetooth mode) | 1.4 kB — the history ring only |
| Flash | +620 kB, of which 355 kB is the recorded speech |

Everything else is claimed when it is used and given back afterwards: the radio's
24 kB arena and ~30 kB decoder while a stream runs, the 10 kB decoder task from
the first station played, MQTT's 1.3 kB buffer while the broker is connected.

Three rules came out of getting this wrong the first time, and they are worth
keeping if you extend it:

- **Nothing large is allocated at boot for a feature that may never be used.** A
  24 kB buffer taken early does not merely cost 24 kB; it sits in the middle of
  the heap and splits the largest free block, which is the number the TLS
  handshake actually depends on.
- **Nothing large is built in RAM to be sent over HTTP.** `/api/telemetry` is
  streamed in chunks for exactly this reason.
- **Nothing goes into `/api/status` that the dashboard does not read.** It is
  fetched every two seconds for as long as anybody has the page open, so an
  unread field is heap churn thirty times a minute.

When something does go wrong, these two tell you where:

```
diag       reset reason, the heap trio (free / lowest ever / largest block),
           every task's stack high-water mark, what hardware was detected
station    the decoder task's remaining stack, whether the stream arena is
           held, and free / largest-block heap
```

The heap trio's third number is the one that matters for TLS: a heap that is
100 kB free in 8 kB pieces cannot start a handshake.

## Home Assistant, over MQTT

The dashboard in this firmware is complete, and it is also a thing you have to
open. Half of what a speaker in a house should do is conditional on something
else — turn the radio on when the bedroom alarm goes off, drop the volume when
the doorbell rings — and none of that can live in the speaker, because the
speaker does not know about the doorbell. So it publishes what it knows and
subscribes to what it can be told, and the automation lives where the rest of the
house's automation already lives.

Put your broker's address on the dashboard's **Home Assistant** page and the
speaker appears in Home Assistant within a second of connecting, complete, with
one device and these entities:

| Kind | Entities |
|------|----------|
| Controls | volume, mute, play/pause, next, previous, equaliser preset, radio station, the LED ring as a light with colour and effects, the sleep timer, one switch per alarm, standby |
| Readings | source, now playing, connected device, battery, voltage, charging, chip temperature, free memory, Wi-Fi signal, uptime, total runtime, radio status and buffer level |

Discovery is what makes that work. Home Assistant will happily talk to a device
publishing to hand-written topics, provided somebody writes about eighty lines of
YAML per entity and keeps it in step with the firmware — and nobody does that
twice. Publishing the discovery documents means the entity list follows the
firmware instead: add a radio station and the dropdown has one more option the
next time the speaker connects.

The standby switch turns off and cannot turn back on. That is the honest shape
for it — a chip in deep sleep is not listening to MQTT, and no amount of protocol
will change that — and the entity's own name says so.

MQTT is TCP, so this needs Wi-Fi; in Bluetooth mode the page says why. It runs
from `loop()` through the ordinary synchronous PubSubClient, alongside the web
server, deliberately rather than through an async client: the radio's decoder
task already wants a steady share of the network stack, and a second task doing
keepalives would be contention for a few hundred bytes every ten seconds.

```
mqtt                   connection state, message counts, discovery status
```

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

## Diagnostics: the `diag` command

The dashboard reports most of the speaker's state in more detail and more
legibly. It also needs a network the speaker may be failing to join, and it does
not exist at all in Bluetooth mode — which is exactly when something is worth
diagnosing. The serial port is there in every mode, from the first line of
`setup()`, whatever the radios are doing.

```sh
pio device monitor
# type:  diag
```

It costs nothing until it is typed: every number is either already being kept
for another reason or is one call into the IDF, so there is no background
collection to switch off in a release build. It prints about forty lines once
and stops.

| Section | What it answers |
|---------|-----------------|
| firmware, chip, flash, mac | which build is running, and whether the flash is configured to match the part fitted |
| reset reason | whether the last restart was a crash, a watchdog, a brownout, a deliberate reboot or a standby wake — all of which look identical from outside |
| uptime, clock | how long since that, and where the time came from |
| heap free / min / largest block | the three numbers that fail differently. The third one decides whether a TLS handshake or a Bluedroid bring-up can be allocated at all: a heap with 60 KB free in 2 KB pieces will refuse both while looking healthy |
| DMA-capable free | where the I2S descriptors live |
| task stacks | the smallest each task's free stack has ever been. Under ~400 bytes is worth raising; an overflow is a panic with a backtrace pointing at whatever function was unlucky |
| peripherals detected | OLED, DS3231 (with its oscillator state), ring, DFPlayer, battery — every one of which is optional and fails by being *quietly* absent |
| DFPlayer counters | frames sent/good/bad, module errors, offline events. These separate "noisy or swapped wiring" from "a clone that refuses commands" from "the module has gone away" |
| I2C errors | DS3231 transactions that did not complete. Should be 0 forever; a number that climbs means the shared bus is marginal, and the OLED is suffering equally and silently |
| audio peak dBFS | what the analyser is actually hearing. Silence sits at the −78 floor; anything anybody is listening to reads well above −70. This is the input every idle timer keys off, so it settles "why will the panel not blank" |
| power | whether saving is on and *why*, in a sentence |
| partitions | the live table, with the running slot marked, and how full it is |

A one-line version of the same thing is printed at every boot:

```
[boot] self-test: oled yes | rtc present, running | ring yes | dfplayer n/a in this mode | battery no cell / gauge off
```

That line exists because absent-tolerance has a cost: a peripheral that is
fitted but *not working* is otherwise indistinguishable from one that was never
fitted at all.

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
  frame_size`, with `dma_desc_num` left at 6. The 6 × 512 here produces six
  3,072-byte descriptors: 18,432 bytes and ~104 ms of DMA slack, which is
  already generous for A2DP jitter. **Do not raise the
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

## Troubleshooting internet radio

### "Low memory: N free, M block" instead of a station

The radio refuses to start a stream when free heap is below 70 kB or the largest
contiguous block is below 26 kB, and says so on the Radio page rather than
trying anyway. Those are not arbitrary: a stream needs the 24 kB arena, a ~30 kB
decoder and a socket, and the layers underneath allocate too.

It refuses because the alternative is worse. An allocation that fails inside the
IDF's socket layer does not return an error — `esp_vfs_select()` creates a
semaphore per call and **asserts** when that fails, which panics the chip. A
speaker that runs out of heap while opening a stream does not fail to play a
station, it reboots.

### "The internet radio is using the network" when checking for updates

Deliberate. There is room on this chip for **one** TLS session, not two. A
handshake against the Mozilla root bundle costs a 16 kB task stack and forty to
fifty kilobytes of mbedtls context, and an `https://` radio station wants the
same again.

When they collided, the second one did not get a clean error: mbedtls
allocation failures surface deep inside lwIP and the IDF socket layer, which
assert rather than unwind. What that looked like from outside was a panic in the
timer task with a corrupted heap behind it, on a speaker whose heap had been
perfectly healthy sixty seconds earlier.

So they take turns. The updater refuses while the radio has a stream open or is
opening one; the radio waits out its backoff while an update check is running;
and the boot-time update check waits for the radio to settle rather than racing
its autostart. Stop the radio, check for updates, start it again.

If you see this, the Graphs page has the numbers behind it: free memory, the
lowest it has ever been, and the largest free block. Things that make it worse:

- **"Always keep setup hotspot on"** in Settings. Running the access point and
  the station together costs a second network interface, a DHCP server and
  beacon buffers — tens of kilobytes, permanently.
- A firmware update check at the same time; TLS wants its own 45 kB block.
- Several browser tabs on the dashboard, each polling.

### It rebooted in a loop after I set a station to resume at boot

Fixed, and worth knowing what it was. Three things went wrong together:

1. Pressing play quietly armed "start playing at boot" — a setting with
   consequences at boot, turned on by a control that said nothing about boot.
2. Autostart fired from `setup()`, which is the worst moment to ask for 50 kB:
   the station is still associating, DHCP has not finished, mDNS and the web
   server have not started, and the setup hotspot has not been torn down yet.
3. The allocation failed inside the HTTP client and panicked, so the next boot
   did the same thing.

All three are addressed. Play no longer arms autostart — the switch on the Radio
page is the only thing that does. Autostart waits until the station has held an
address for twelve seconds. And an autostart sentinel is written to flash before
the attempt and cleared once audio is actually flowing, so a boot that finds it
still set knows the last one did not survive and **disarms autostart instead of
repeating it**. That is the same pattern the radio-mode sentinel uses, for the
same reason: a setting the owner can change from a web page must never be able
to make the web page unreachable.

If you are in a loop on older firmware, flash any build over USB — the ROM
bootloader is unaffected — and the sentinel clears it on the boot after.

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

## Manual hardware test checklist

Everything in this firmware that can be checked without a board already is: it
compiles with no warnings, `pio check` reports no high-severity findings in
`src/`, `scripts/test_pin_check.py` proves the pin assertions still assert, and
`scripts/test_settings_backup.py` proves a settings backup still round-trips
every stored preference.
None of that says anything about how the speaker *behaves*, and the list below
is what does. Work down it; the order is roughly worst-consequence-first.

Have `pio device monitor` open throughout and type `diag` at the end of each
group — most of the pass criteria below are one line of that report.

### 1. Boot and recovery (do these first)

- [ ] **Cold boot from USB.** `[boot] reset reason: power on`, then the
      `[boot] self-test:` line. The splash appears on the OLED within a second.
- [ ] **Ten reboots in a row** (`pio run -t upload`, or the dashboard's
      restart). No panic, no boot loop. `diag` → heap free within a few hundred
      bytes of the same number every time.
- [ ] **Every optional peripheral unplugged** — no OLED, no RTC, no ring, no
      DFPlayer, no battery. The speaker still pairs and plays. Self-test line
      says `no` / `not fitted` for each, and nothing else complains.
- [ ] **OLED only** (no RTC on the bus). Panel works; clock falls back to
      NVS/network and the info screen says so.
- [ ] **RTC only** (no OLED). No panel, no UI task, clock still comes from the
      DS3231. The BOOT button still does the headless factory reset.
- [ ] **OLED and RTC together.** Both on 0x3C and 0x68, no interference; `diag`
      → `I2C errors 0` after ten minutes.
- [ ] **Factory reset**: hold BOOT through the countdown, release. Settings,
      Wi-Fi and Bluetooth bonds are gone; the board **runs the firmware**, not
      the ROM bootloader. *(This is the strapping-pin case — if it ever comes
      back silent and unresponsive to the monitor, that is the failure.)*
- [ ] **Deliberate brownout**: run from a thin cable with the ring at full
      white. If it resets, `diag` should name it `BROWNOUT`, not a panic.

### 2. Standby and wake — the highest-risk path

- [ ] From the dashboard or the idle timer, enter standby. The **Goodbye**
      screen animates (moon rises, stars appear), then the panel goes dark.
- [ ] **Nothing on the Goodbye or Good morning screen runs off the edge.** Both
      draw their text into a 66 px column on the right, and both put text in it
      that does not fit — "Good morning" alone is 69 px, and the detail line
      carries the device name, which the owner chooses. The two lines must be
      clipped to the column and scroll if they overflow, and the moon and stars
      on the left must not be drawn over.
- [ ] **The ring is genuinely dark**, not frozen mid-effect. WS2812s latch: a
      ring still showing colour here means the blanking frame did not go out.
- [ ] The DFPlayer stops before standby, not after.
- [ ] **Wake with a long BOOT press and keep holding it.** The speaker must come
      back and run — the serial log shows `[power] waking`, then the normal boot
      banner. *A board that comes back silent with no banner is in the ROM
      download mode, which is the bug this path exists to avoid.*
- [ ] Wake with a brief brush of the button: it must **stay** asleep.
- [ ] After waking, `diag` → `reset reason ... (woke from standby)` and the
      OLED shows **Good morning** briefly.

### 3. Audio

- [ ] Pair a phone, play. Connect chime, then music.
- [ ] **Rapid start/stop**: play, pause, skip, seek, ten times quickly. No
      crackle, no dropout, no reset.
- [ ] **Rapid mode switching**: hold BOOT → confirm → repeat through all three
      modes. Each comes up clean; `diag` → heap min not falling boot on boot.
- [ ] **Long playback**: at least two hours continuous. Then `diag` → heap free
      and largest block should be within a few hundred bytes of where they were
      at the start. A slow fall is a leak.
- [ ] **Maximum volume** on loud material. The soft knee should limit, not
      square off — distortion that appears only on peaks means lowering
      `OUTPUT_GAIN` from 1.5.
- [ ] **Underrun check**: while playing, open the dashboard, load the Lighting
      page, run a Wi-Fi scan. Audio must not stutter. (There is no hardware
      underrun counter on this path; the ear is the instrument.)
- [ ] **LED animation during audio**: set the ring to Music sync and confirm it
      tracks the beat without affecting the sound.

### 4. DFPlayer

- [ ] **Absent**: DFPlayer mode with nothing on the UART. `diag` →
      `OFFLINE (check TX/RX)` and `framesGood 0`; the dashboard stays usable.
- [ ] **Present, genuine YX5200** and, if you have one, an **AA104/GD3200B
      clone**. `diag` → `framesGood` climbing, `framesBad 0`, `errors 0`.
- [ ] **TX and RX swapped deliberately** — confirm it reports offline rather
      than hanging.
- [ ] **SD card missing**: `media` shows no `sd`; the status LED shows the
      no-media pattern.
- [ ] **SD card empty**, **corrupt (non-FAT32)**, and **populated**. Only the
      last should play; the other two must not wedge the driver.
- [ ] **Card pulled while playing.** Recovers, reports it, does not reboot.
- [ ] **A computer plugged into the module's USB-B.** The dashboard says the
      card is mounted elsewhere; playback stops and resumes on unplug.
- [ ] **Folder and track limits**: a folder with ~3000 files, and track numbers
      near the top of the range. Numbering is 1-based and folders are 1–99.
- [ ] **Volume bounds**: 0 and 30 on the module scale, from both the dashboard
      and the console.

### 5. Battery and power

- [ ] **Meter against the gauge.** Measure the pack with a multimeter, compare
      to `bat` on the console. Then `bat calib <the meter reading>` and confirm
      the trim lands within a few tens of millivolts.
- [ ] Repeat at three states of charge (full, mid, low) — a trim that is right
      at one voltage and wrong at another means the divider ratio is wrong, not
      the trim.
- [ ] **With and without the 100 nF at GPIO34.** Note the difference; the
      firmware's discarded first conversion should make it small.
- [ ] **USB power vs battery power**, and the transition between them.
- [ ] **Low-battery threshold and hysteresis**: run the pack down past the
      threshold and confirm saving engages, then charge past the exit point and
      confirm it disengages *once*, not repeatedly.
- [ ] **Critical**: the status LED heartbeat pattern appears, and stops the
      moment a charger is connected.
- [ ] **No pack fitted, gauge enabled**: must report *not present*, not a
      critically flat cell.

### 6. Clock

- [ ] **RTC power loss**: pull the DS3231's coin cell, power-cycle, refit it.
      Boot should log `oscillator stopped` and *not* adopt the stale time.
- [ ] Set the time from the console or the dashboard; confirm `diag` →
      `present, running` and that the time survives a full power cut.
- [ ] **millis() wraparound.** Everything time-related here uses wrap-safe
      arithmetic, but 49.7 days is the only real proof. If you cannot run that
      long, the review-level confidence is: every comparison in the changed code
      is `(int32_t)(now - deadline) >= 0` or `now - then >= interval`, never
      `now > deadline`.

- [ ] **A Persian track title.** Play something with a Persian title, or set
      the Bluetooth name to one. The letters must be **joined**, not a row of
      separate shapes, and must read right to left. A Latin word or a number
      inside it must still read left to right.
- [ ] **A Persian name on the greeting screen.** Wake from standby with a
      Persian device name: it must stay inside the right-hand panel and scroll
      if it is too long, not run off the edge.
- [ ] **Latin is unchanged.** Compare the now-playing screen against the
      previous firmware with an ASCII title — it should be pixel-identical.

### 7. Persistence

- [ ] Change several settings, power-cycle, confirm they survive.
- [ ] **Settings backup round trip.** Download a backup with a passphrase,
      factory reset, then restore the file. Every card on the Settings page
      should come back as it was, the ring included, and the speaker should
      rejoin the same Wi-Fi network on its own. Sign back in with the password
      the *backup* carries. Time the download: the PBKDF2 pass should be around
      a second, and **nothing should glitch in whatever is playing**.
- [ ] **A wrong passphrase.** Restore the same file with one character changed.
      It must be refused, and *nothing* may change — check the Wi-Fi network and
      the dashboard password are still what they were, and that no restart
      happened.
- [ ] **A blank passphrase.** Download without one. The file must contain no
      `secrets` block and no credential anywhere in it — read it and confirm.
      Restoring it leaves the current passwords alone.
- [ ] **A file the restore should refuse.** Drop some other `.json` on the card:
      it must be rejected in the browser without a request being sent. Then
      `curl` a `{}` body at `/api/settings/restore` — 400, and nothing changes.
- [ ] **A trimmed backup.** Delete everything but the `leds` object from a
      backup and restore it. The ring comes back; nothing else moves.
- [ ] **Decrypt it off the device** with the Python snippet in the backup
      section. If that fails, the file format is not what it claims to be.
- [ ] **Power cycle during a settings write** — pull power repeatedly while
      saving from the dashboard. NVS should either keep the old value or take
      the new one; it must never fail to mount on the next boot.
- [ ] **OTA**: upload `firmware.bin` from the dashboard, confirm it writes to
      the *other* slot (`diag` → partitions, the `<-- running` marker moves).
- [ ] Power-cycle mid-OTA. The old slot must still boot.

### 8. Radios

- [ ] Bluetooth only mode: pair and play with Wi-Fi never initialised.
- [ ] DFPlayer + Wi-Fi: audio from the card while the dashboard is open.
- [ ] Setup access point: wrong credentials saved → AP appears after the grace
      period → new credentials fix it.

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
| [src/power.h](src/power.h) / [.cpp](src/power.cpp) | power saving: the three modes, the battery policy, and the four things it switches off |
| [src/audio_eq.h](src/audio_eq.h) / [.cpp](src/audio_eq.cpp) | the five-band equaliser: cookbook biquads, the presets, the automatic preamp |
| [src/net_radio.h](src/net_radio.h) / [.cpp](src/net_radio.cpp) | internet radio: the decoder task, the jitter buffer, ICY metadata, reconnection, the favourites |
| [src/voice.h](src/voice.h) / [.cpp](src/voice.cpp) | spoken announcements: the ADPCM decoder, the resampler, the ducking mixer, the queue |
| [src/voice_clips.h](src/voice_clips.h) | the clips themselves — generated, committed, never rebuilt by a build |
| [src/alarm_clock.h](src/alarm_clock.h) / [.cpp](src/alarm_clock.cpp) | alarms and the sleep timer: scheduling, the wake-up ramp, the fallback to the chime |
| [src/telemetry.h](src/telemetry.h) / [.cpp](src/telemetry.cpp) | the history ring behind the graphs, and the runtime counter |
| [src/home_assistant.h](src/home_assistant.h) / [.cpp](src/home_assistant.cpp) | MQTT and the Home Assistant discovery documents |
| [scripts/make_voice_clips.py](scripts/make_voice_clips.py) | speaks [scripts/voice_phrases.txt](scripts/voice_phrases.txt) and embeds it as IMA ADPCM — run by hand, not by the build |
| [src/ui_assets.h](src/ui_assets.h) | hand-drawn XBM icons |
| [src/pin_check.h](src/pin_check.h) | the whole pin map in one place, asserted at compile time — no code, no flash |
| [src/app_config.h](src/app_config.h) | the build-time switches — `SERIAL_LOG`, `CONSOLE_ENABLED`, `DIAGNOSTICS_ENABLED` — and the `LOGF`/`LOGLN`/`LOGP` macros every file logs through |
| [src/diagnostics.h](src/diagnostics.h) / [.cpp](src/diagnostics.cpp) | the `diag` console report: reset reason, heap, task stacks, what was detected, link counters, partitions |
| [src/memory_pressure.h](src/memory_pressure.h) / [.cpp](src/memory_pressure.cpp) | hysteretic heap/fragmentation pressure tracking and bounded optional-work policy |
| [src/runtime_events.h](src/runtime_events.h) / [.cpp](src/runtime_events.cpp) | checked RTC no-init lifecycle breadcrumb retained across resets |
| [src/stability_policy.h](src/stability_policy.h) | dependency-free production bounds for memory, timers, rings, URLs, UTF-8, EQ, backoff and PCM math |
| [scripts/test_pin_check.py](scripts/test_pin_check.py) | host-side test that the pin assertions actually assert — 21 cases, no board needed |
| [scripts/test_settings_backup.py](scripts/test_settings_backup.py) | host-side test that the settings backup and restore agree, key by key, and cover every stored preference |
| [scripts/test_stability_policy.py](scripts/test_stability_policy.py) | compiles and tests the actual production policy header with the Xtensa compiler |
| [scripts/test_audio_eq.py](scripts/test_audio_eq.py) | sweeps 1,000 production EQ designs across sample rates, bands and gain limits |
| [src/text_arabic.h](src/text_arabic.h) / [.cpp](src/text_arabic.cpp) | Arabic-script shaping and right-to-left ordering: four contextual shapes per letter, lam-alef ligatures, mixed Latin runs |
| [scripts/gen_arabic_tables.py](scripts/gen_arabic_tables.py) | derives the shaping tables from Unicode and checks the committed ones still match |
| [scripts/test_arabic_shaping.py](scripts/test_arabic_shaping.py) | host-side test of the shaping and ordering, plus every emitted glyph against the font's real glyph table |
| [scripts/font_bitmap.py](scripts/font_bitmap.py) | decodes u8g2 glyph bitmaps to draw text as ASCII art — the only way to see whether Persian came out looking like Persian |
| [scripts/font_coverage.py](scripts/font_coverage.py) | parses u8g2_fonts.c to report which codepoints a font actually contains |

The measured build, memory and concurrency review is recorded in
[docs/stability-memory-audit.md](docs/stability-memory-audit.md), including the
remaining hardware-only soak tests and their pass/fail criteria.

## Notes on the toolchain

- Audio comes from [ESP32-A2DP](https://github.com/pschatzmann/ESP32-A2DP)
  v1.8.11, driven through
  [arduino-audio-tools](https://github.com/pschatzmann/arduino-audio-tools)
  v1.2.5 via `I2SStream` — the output path the library documents. AudioTools is
  a hard dependency of current ESP32-A2DP, not an optional extra.
- The display uses [U8g2](https://github.com/olikraus/u8g2) in full-buffer mode
  (512 bytes for 128×32). It is the one library here that comes from the
  PlatformIO registry; the pschatzmann ones point at tagged GitHub commits.
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
  never uses — another 523 bytes. Together that leaves about 760 bytes of IRAM
  free, where the alternative was a build that did not link at all.

  If you ever build this for a WROVER **and enable PSRAM**, delete the
  `build_unflags` block: there the workaround is doing a real job.
