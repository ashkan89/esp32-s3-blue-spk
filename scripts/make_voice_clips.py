"""Render the spoken announcements and embed them in the firmware.

    python scripts/make_voice_clips.py

Reads scripts/voice_phrases.txt, speaks each line with the Windows speech
engine, and writes src/voice_clips.h. Run it by hand when the phrase list
changes and commit the result: the build itself never calls this, so a firmware
build works on a machine with no speech engine and produces the same bytes on
every machine that has one.

Why the clips are generated rather than recorded. The alternative is a set of
WAV files in the repository, which is the same thing minus the ability to change
a wording without a microphone -- and the announcements need to be editable,
because half the point of the device-name clips is that the owner adds their
own. Anything the speech engine can say, this can embed.

Why IMA ADPCM. Speech at 16 kHz is 32 kB per second as 16-bit PCM, and the whole
announcement set would be most of a megabyte. ADPCM is exactly 4:1, decodes in
about a dozen integer operations per sample, and on band-limited speech through
a small driver it is not distinguishable from the original. The decoder is
twenty lines in voice.cpp.

Requires Windows (System.Speech). Everything else is the Python standard
library, deliberately: this is a tool, not a dependency.
"""

import argparse
import array
import io
import os
import re
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

PROJECT = Path(__file__).resolve().parent.parent
PHRASE_FILE = PROJECT / "scripts" / "voice_phrases.txt"
OUTPUT = PROJECT / "src" / "voice_clips.h"

# 16 kHz is the standard telephony rate and it is the right one here. Speech
# carries no useful energy above 8 kHz, the speaker is a small full-range
# driver, and halving the rate halves the flash. The firmware resamples to
# whatever the I2S channel is running at.
RATE = 16000

# Peak level the clips are normalised to, as a fraction of full scale. Short of
# 1.0 so that mixing an announcement into music that is already near full scale
# has somewhere to go before the limiter has to work.
PEAK = 0.80

# Silence trimming. The speech engine leaves a good fraction of a second of
# nothing at each end of every clip, and paying flash for it is bad enough
# before considering that the announcements then sound sluggish.
TRIM_DB = -45.0
TRIM_KEEP_MS = 30

# A short fade at each end. Without it the clip starts on a non-zero sample and
# the transition into it clicks -- the same reason the melodies in main.cpp fade.
FADE_MS = 8

# The phrase list shipped when scripts/voice_phrases.txt does not exist yet.
#
# The ids are what the firmware refers to, so they are an interface: renaming
# one means editing voice.h to match. Adding a line, though, costs nothing but
# the flash the clip takes -- which is why the device-name section at the bottom
# is a list of examples rather than a fixed set. That section is the answer to
# "announce which phone connected": the module cannot synthesise a name it has
# never seen, but it can play a clip somebody made for it, and the dashboard
# maps a Bluetooth address to one.
DEFAULT_PHRASES = """\
# One clip per line:  id | the words to speak
#
# Lines starting with # are ignored. After editing, run
#   python scripts/make_voice_clips.py
# and rebuild. Ids used by the firmware are marked [fixed] and should keep their
# names; everything else is free for you to change, remove or add to.

# -- power and mode ---------------------------------------------------- [fixed]
boot              | Speaker ready.
goodbye           | Powering down. Goodbye.
mode_bluetooth    | Bluetooth mode.
mode_wifi         | Wi-Fi mode.
mode_dfplayer     | Memory card mode.

# -- network ----------------------------------------------------------- [fixed]
wifi_connected    | Wi-Fi connected.
wifi_lost         | Wi-Fi connection lost.
wifi_setup        | Setup hotspot is open. Connect to it to configure me.

# -- bluetooth --------------------------------------------------------- [fixed]
bt_connected      | Device connected.
bt_disconnected   | Device disconnected.
bt_ready          | Ready to pair.

# -- battery ----------------------------------------------------------- [fixed]
battery_low       | Battery low. Please connect the charger.
battery_critical  | Battery critically low. Shutting down soon.
battery_charging  | Charging.
battery_full      | Battery fully charged.

# -- internet radio ---------------------------------------------------- [fixed]
radio_connecting  | Connecting to the station.
radio_playing     | Station connected.
radio_failed      | Could not reach the station.
radio_buffering   | Buffering.

# -- alarm and sleep --------------------------------------------------- [fixed]
alarm             | Good morning. Your alarm is going off.
alarm_snooze      | Snoozing.
alarm_set         | Alarm set.
sleep_timer       | Sleep timer started. Good night.
sleep_ending      | Sleep timer finished.

# -- device names -------------------------------------------------------------
# These are examples. Add one line per phone or laptop you want announced by
# name, then map the Bluetooth address to it on the dashboard's Sound page.
dev_phone         | Phone connected.
dev_laptop        | Laptop connected.
dev_tablet        | Tablet connected.
dev_tv            | Television connected.
"""


def load_phrases(path):
    """Parses the phrase file into an ordered list of (id, text)."""
    if not path.exists():
        path.write_text(DEFAULT_PHRASES, encoding="utf-8")
        print("created %s with the default phrases" % path)
    phrases = []
    seen = set()
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "|" not in line:
            raise SystemExit("%s:%d: expected 'id | text'" % (path.name, lineno))
        ident, text = (part.strip() for part in line.split("|", 1))
        if not re.fullmatch(r"[a-z][a-z0-9_]*", ident):
            raise SystemExit(
                "%s:%d: '%s' is not a usable id (lowercase, digits, underscore)"
                % (path.name, lineno, ident)
            )
        if ident in seen:
            raise SystemExit("%s:%d: duplicate id '%s'" % (path.name, lineno, ident))
        if not text:
            raise SystemExit("%s:%d: '%s' has nothing to say" % (path.name, lineno, ident))
        seen.add(ident)
        phrases.append((ident, text))
    if not phrases:
        raise SystemExit("%s has no phrases in it" % path)
    return phrases


def speak_all(phrases, voice, rate, out_dir):
    """Renders every phrase to a WAV in one speech-engine session.

    One session for the whole list rather than one per phrase: starting the
    synthesizer costs about a second, and doing that forty times is the
    difference between a script that is annoying to run and one that is not.
    """
    script_lines = [
        "$ErrorActionPreference = 'Stop'",
        "Add-Type -AssemblyName System.Speech",
        "$s = New-Object System.Speech.Synthesis.SpeechSynthesizer",
        "$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo("
        "%d, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, "
        "[System.Speech.AudioFormat.AudioChannel]::Mono)" % RATE,
        "$s.SelectVoice(%s)" % ps_quote(voice),
        "$s.Rate = %d" % rate,
    ]
    for ident, text in phrases:
        target = out_dir / ("%s.wav" % ident)
        script_lines.append("$s.SetOutputToWaveFile(%s, $fmt)" % ps_quote(str(target)))
        script_lines.append("$s.Speak(%s)" % ps_quote(text))
    script_lines.append("$s.SetOutputToNull()")
    script_lines.append("$s.Dispose()")

    result = subprocess.run(
        ["powershell", "-NoProfile", "-NonInteractive", "-Command", "\n".join(script_lines)],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise SystemExit("the speech engine failed:\n%s" % (result.stderr or result.stdout))


def ps_quote(value):
    """A PowerShell single-quoted literal, which escapes only the quote itself."""
    return "'%s'" % value.replace("'", "''")


def list_voices():
    result = subprocess.run(
        [
            "powershell",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            "Add-Type -AssemblyName System.Speech; "
            "(New-Object System.Speech.Synthesis.SpeechSynthesizer)."
            "GetInstalledVoices() | ForEach-Object { $_.VoiceInfo.Name }",
        ],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return []
    return [line.strip() for line in result.stdout.splitlines() if line.strip()]


def read_wav(path):
    with wave.open(str(path), "rb") as handle:
        if handle.getnchannels() != 1 or handle.getsampwidth() != 2:
            raise SystemExit("%s is not 16-bit mono" % path.name)
        if handle.getframerate() != RATE:
            raise SystemExit("%s is %d Hz, expected %d" % (path.name, handle.getframerate(), RATE))
        samples = array.array("h")
        samples.frombytes(handle.readframes(handle.getnframes()))
    if sys.byteorder == "big":
        samples.byteswap()
    return samples


def trim_and_level(samples):
    """Cuts the leading and trailing silence, normalises, and fades the ends."""
    if not samples:
        return samples
    threshold = int(32767 * (10.0 ** (TRIM_DB / 20.0)))
    first, last = 0, len(samples) - 1
    while first < len(samples) and abs(samples[first]) < threshold:
        first += 1
    while last > first and abs(samples[last]) < threshold:
        last -= 1
    if first > last:  # the engine produced nothing audible at all
        return array.array("h")

    keep = int(RATE * TRIM_KEEP_MS / 1000)
    first = max(0, first - keep)
    last = min(len(samples) - 1, last + keep)
    out = samples[first : last + 1]

    peak = max(abs(value) for value in out) or 1
    gain = (32767.0 * PEAK) / peak
    for i in range(len(out)):
        out[i] = clamp16(int(out[i] * gain))

    fade = min(int(RATE * FADE_MS / 1000), len(out) // 2)
    for i in range(fade):
        scale = i / fade
        out[i] = int(out[i] * scale)
        out[len(out) - 1 - i] = int(out[len(out) - 1 - i] * scale)
    return out


def clamp16(value):
    return -32768 if value < -32768 else (32767 if value > 32767 else value)


# ------------------------------------------------------------- IMA ADPCM ----
# The reference tables, straight from the IMA specification. The encoder below
# is the standard one, run as a single continuous stream rather than in blocks:
# the firmware only ever plays a clip from its start, so the per-block headers
# that make seeking possible would be pure overhead.
STEP_TABLE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
    45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209,
    230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876,
    963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749,
    3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767,
]
INDEX_TABLE = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


def adpcm_encode(samples):
    """Returns (packed bytes, sample count). Two samples per byte, low nibble first."""
    predictor = 0
    index = 0
    out = bytearray()
    pending = None

    for sample in samples:
        step = STEP_TABLE[index]
        diff = sample - predictor
        code = 0
        if diff < 0:
            code = 8
            diff = -diff
        # The three magnitude bits, most significant first.
        delta = step >> 3
        if diff >= step:
            code |= 4
            diff -= step
            delta += step
        step >>= 1
        if diff >= step:
            code |= 2
            diff -= step
            delta += step
        step >>= 1
        if diff >= step:
            code |= 1
            delta += step

        predictor = clamp16(predictor - delta if code & 8 else predictor + delta)
        index = min(88, max(0, index + INDEX_TABLE[code]))

        if pending is None:
            pending = code
        else:
            out.append(pending | (code << 4))
            pending = None

    if pending is not None:
        out.append(pending)
    return bytes(out), len(samples)


def c_identifier(ident):
    return "VOICE_" + ident.upper()


def emit(phrases, encoded, voice, rate):
    lines = []
    add = lines.append
    add("// Generated by scripts/make_voice_clips.py -- do not edit by hand.")
    add("//")
    add("// Edit scripts/voice_phrases.txt and re-run the script instead. The")
    add("// build does not regenerate this file, so it is committed as it is.")
    add("//")
    add("// Voice: %s, rate setting %+d, %d Hz IMA ADPCM." % (voice, rate, RATE))
    add("#pragma once")
    add("")
    add('#include <Arduino.h>')
    add("")
    add("/// The rate every clip was rendered at. voice.cpp resamples from here to")
    add("/// whatever the I2S channel is running.")
    add("static const uint32_t VOICE_CLIP_RATE = %d;" % RATE)
    add("")

    total = 0
    for ident, text in phrases:
        payload, count = encoded[ident]
        total += len(payload)
        add("// \"%s\" -- %.2f s" % (text.replace('"', "'"), count / float(RATE)))
        add("static const uint8_t VOICE_DATA_%s[] PROGMEM = {" % ident.upper())
        for offset in range(0, len(payload), 16):
            chunk = payload[offset : offset + 16]
            add("    " + ", ".join("0x%02x" % byte for byte in chunk) + ",")
        add("};")
        add("")

    add("/// One entry per phrase, in the order of the VoiceClipId enum below.")
    add("struct VoiceClipDef {")
    add("  const char *id;      ///< the phrase file's name for it")
    add("  const char *text;    ///< what it says, for the dashboard's list")
    add("  const uint8_t *data; ///< IMA ADPCM nibbles, low nibble first")
    add("  uint32_t samples;    ///< decoded length, in samples at VOICE_CLIP_RATE")
    add("};")
    add("")
    add("enum VoiceClipId : uint8_t {")
    for ident, _ in phrases:
        add("  %s," % c_identifier(ident))
    add("  VOICE_CLIP_COUNT")
    add("};")
    add("")
    add("static const VoiceClipDef VOICE_CLIPS[VOICE_CLIP_COUNT] = {")
    for ident, text in phrases:
        _, count = encoded[ident]
        add('    {"%s", "%s", VOICE_DATA_%s, %d},'
            % (ident, text.replace('"', "'").replace("\\", ""), ident.upper(), count))
    add("};")
    add("")

    OUTPUT.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    seconds = sum(count for _, count in encoded.values()) / float(RATE)
    print("wrote %s: %d clips, %.1f s of speech, %.1f kB of flash"
          % (OUTPUT.relative_to(PROJECT), len(phrases), seconds, total / 1024.0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--voice", default=None,
                        help="speech engine voice name (default: the first female "
                             "voice installed, which carries better through a small driver)")
    parser.add_argument("--rate", type=int, default=-1,
                        help="speaking rate, -10 to 10 (default -1: announcements "
                             "read slightly slow are easier to catch across a room)")
    parser.add_argument("--list-voices", action="store_true",
                        help="print the installed voices and exit")
    args = parser.parse_args()

    if os.name != "nt":
        raise SystemExit("this script needs the Windows speech engine; "
                         "src/voice_clips.h is committed, so a build does not")

    voices = list_voices()
    if args.list_voices:
        print("\n".join(voices) if voices else "no voices found")
        return
    if not voices:
        raise SystemExit("no speech voices are installed")

    voice = args.voice
    if voice is None:
        female = [name for name in voices if "Zira" in name or "Hazel" in name]
        voice = (female or voices)[0]
    if voice not in voices:
        raise SystemExit("no such voice: %s\ninstalled: %s" % (voice, ", ".join(voices)))

    phrases = load_phrases(PHRASE_FILE)
    print("speaking %d phrases as %s..." % (len(phrases), voice))

    with tempfile.TemporaryDirectory() as tmp:
        out_dir = Path(tmp)
        speak_all(phrases, voice, args.rate, out_dir)
        encoded = {}
        for ident, text in phrases:
            path = out_dir / ("%s.wav" % ident)
            if not path.exists():
                raise SystemExit("the speech engine produced nothing for '%s'" % ident)
            samples = trim_and_level(read_wav(path))
            if not samples:
                raise SystemExit("'%s' came out silent" % ident)
            encoded[ident] = adpcm_encode(samples)

    emit(phrases, encoded, voice, args.rate)


if __name__ == "__main__":
    main()
