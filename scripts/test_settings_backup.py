#!/usr/bin/env python3
"""
Host-side test for the settings backup format in src/management.cpp.

A settings backup has one failure mode worth guarding, and it is silent. Add a
setting to the firmware, remember it in handleSettingsBackup(), forget it in
handleSettingsRestore(), and everything still compiles, the file still
downloads, the restore still reports success -- and that one setting quietly
does not come back. Nobody finds out until somebody restores a speaker and
spends an afternoon wondering why its ring is the wrong colour.

So this checks what keeps the format honest, by parsing the source rather than
by running it -- no board, no linker, under a second:

  1. every key the backup writes, the restore reads, and vice versa;
  2. every key the save functions put in NVS has a named home in the backup,
     except the ones this test is told to expect to be missing;
  3. the keys deliberately left out are exactly the documented two;
  4. all four credentials are in the encrypted envelope;
  5. none of them is also written into the readable half of the file; and
  6. the envelope's parameters are the documented ones, and the key derivation
     it documents really is standard PBKDF2-HMAC-SHA256.

The last two are a different kind of guard: not "does it round-trip" but "is it
still encrypted". Moving a secret back into the clear-text settings object would
round-trip perfectly and pass every other check here.

Run:  python scripts/test_settings_backup.py
Exit: 0 if the format is symmetric and complete, 1 otherwise.
"""

import hashlib
import hmac
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(REPO, "src", "management.cpp")

# The ArduinoJson locals the two handlers build the document out of. Keeping the
# list explicit is what stops this test from quietly matching nothing at all if
# somebody renames them: an unknown name means no keys are found, and a run that
# finds no keys fails below rather than passing vacuously.
CONTAINERS = ("s", "df", "bat", "oled", "pwr", "led", "clk", "sec")

# The nested objects and arrays themselves -- structure, not settings.
SECTIONS = {
    ("s", "dfplayer"),
    ("s", "battery"),
    ("s", "display"),
    ("s", "power"),
    ("s", "leds"),
    ("s", "clock"),
    ("s", "eq"),
    ("s", "voice"),
    ("s", "mqtt"),
    ("s", "radio"),
    ("s", "alarms"),
}

# NVS keys that are stored but deliberately absent from a backup, and why. See
# the block comment above handleSettingsBackup() for the reasoning; this list
# existing is what turns "we left it out on purpose" into something a future
# change cannot silently disagree with.
EXCLUDED = {
    "radioMode": "a file taken in Bluetooth mode would restore a speaker with no dashboard",
    "bootFail": "the boot sentinel's strike count describes a boot, not a preference",
}

# Settings that live in another module's NVS namespace, so they never appear in
# a prefs.put*() call in management.cpp. They are still settings, and still have
# to be in the file.
#
#   clk.*      soft_clock.cpp's "clock" namespace
#   s.radio    net_radio.cpp's "radio" namespace -- the station list
#   s.alarms   alarm_clock.cpp's "alarm" namespace
EXTERNAL_KEYS = {
    ("clk", "use24h"),
    ("clk", "autoSync"),
    ("clk", "offsetMinutes"),
    ("clk", "zone"),
    ("s", "radio"),
    ("s", "alarms"),
}

# Kept under its old name for the check that the clock, specifically, is carried.
CLOCK_KEYS = {pair for pair in EXTERNAL_KEYS if pair[0] == "clk"}

# The functions that put a setting into NVS. Anything writing a prefs key from
# outside these is, by definition, storing something the backup does not know
# about -- which is what the EXCLUDED list below is for.
SAVE_FUNCTIONS = ("saveSettings", "saveLedSettings", "saveAudioSettings")

# Every NVS key this firmware stores, and where a backup file carries it.
#
# The count this used to be -- "the backup has as many keys as NVS does" -- was
# a good enough proxy while every setting was one value. It stopped being one
# when the equaliser, the announcements and the MQTT client each arrived as a
# struct: one NVS blob, several fields in the file. Naming the destination is
# both stricter and clearer, and it means adding a setting fails this test with
# the name of the setting rather than with an off-by-one.
#
# The three composite entries point at a SECTIONS object rather than at a leaf,
# because that is genuinely where they live: s["eq"] is the whole EqConfig.
NVS_HOME = {
    "ssid": ("s", "ssid"),
    "wifiPass": ("sec", "wifiPassword"),
    "hostname": ("s", "hostname"),
    "apPass": ("sec", "apPassword"),
    "adminPass": ("sec", "adminPassword"),
    "deviceName": ("s", "deviceName"),
    "apAlways": ("s", "apAlways"),
    "ghRepo": ("s", "githubRepo"),
    "ghAsset": ("s", "githubAsset"),
    "ghToken": ("sec", "githubToken"),

    "dfSrc": ("df", "source"),
    "dfVol": ("df", "volume"),
    "dfEq": ("df", "eq"),
    "dfLoop": ("df", "loop"),
    "dfLoopF": ("df", "loopFolder"),
    "dfAuto": ("df", "autoplay"),

    "batOn": ("bat", "enabled"),
    "batDiv": ("bat", "divider"),
    "batCal": ("bat", "calibration"),
    "batFull": ("bat", "full"),
    "batEmpty": ("bat", "empty"),
    "batCells": ("bat", "cells"),
    "batLow": ("bat", "low"),
    "batCrit": ("bat", "critical"),

    "uiBlank": ("oled", "blankMode"),
    "uiBlankS": ("oled", "blankAfterSeconds"),

    "pwrMode": ("pwr", "mode"),
    "pwrPct": ("pwr", "threshold"),
    "slpMode": ("pwr", "sleepMode"),
    "slpS": ("pwr", "sleepAfterSeconds"),

    "ledOn": ("led", "enabled"),
    "ledFx": ("led", "effect"),
    "ledBri": ("led", "brightness"),
    "ledSpd": ("led", "speed"),
    "ledRct": ("led", "reactivity"),
    "ledCol": ("led", "color"),
    "ledCol2": ("led", "color2"),
    "ledIdle": ("led", "idleOff"),
    "ledIdleS": ("led", "idleAfterSeconds"),

    # Composite: one NVS blob, an object of fields in the file.
    "eq": ("s", "eq"),
    "voice": ("s", "voice"),
    "haCfg": ("s", "mqtt"),

    "voiceDev": ("s", "voiceDevices"),
}

# The four values that travel in the AES-256-GCM envelope rather than in clear
# text. They are settings like any other -- they must round-trip -- but they are
# assigned into their own document, so the check below makes sure they are still
# in the file at all rather than having quietly been dropped when the envelope
# was added.
SECRET_KEYS = {
    ("sec", "wifiPassword"),
    ("sec", "apPassword"),
    ("sec", "adminPassword"),
    ("sec", "githubToken"),
}


def function_body(source, name):
    """The text of one free function, from its signature to the next one."""
    start = source.index("void %s() {" % name)
    end = source.index("\nvoid ", start + 10)
    return source[start:end]


def keys_assigned(text):
    """Keys written as obj["key"] = ... -- what the backup puts in the file."""
    pattern = r"\b(%s)\[\"([A-Za-z0-9]+)\"\]\s*=" % "|".join(CONTAINERS)
    return {m.groups() for m in re.finditer(pattern, text)}


def keys_referenced(text):
    """Keys mentioned at all -- what the restore looks for in the file."""
    pattern = r"\b(%s)\[\"([A-Za-z0-9]+)\"\]" % "|".join(CONTAINERS)
    return {m.groups() for m in re.finditer(pattern, text)}



# The envelope's parameters, as documented in the README and in the block comment
# above handleSettingsBackup(). Checked against the constants in the source so
# the three cannot drift apart -- a salt quietly shortened to 8 bytes, or an
# iteration count dropped a zero, is exactly the kind of change that breaks
# nothing visible.
CRYPTO_CONSTANTS = {
    "SETTINGS_BACKUP_ITERATIONS": 50000,
    "SETTINGS_BACKUP_SALT_LEN": 16,
    "SETTINGS_BACKUP_IV_LEN": 12,
    "SETTINGS_BACKUP_TAG_LEN": 16,
}


def derive_like_firmware(passphrase, salt, iterations):
    """
    A transliteration of deriveBackupKey() in src/management.cpp.

    The firmware writes PBKDF2 out by hand rather than calling
    mbedtls_pkcs5_pbkdf2_hmac(), so that it can yield to the scheduler between
    rounds -- fifty thousand HMACs with no break stalls the audio path. Writing
    a standard construction out by hand is also how a subtle mistake gets in:
    the wrong counter width, an XOR against the wrong buffer, a loop that runs
    one round short. Any of those still produce a stable, self-consistent key,
    so the firmware would encrypt and decrypt its own backups perfectly and
    nothing else in the world could read them.

    This mirrors the C++ line for line and is compared with hashlib below. It
    has to be kept in step with the C++ by hand: its job is to prove the
    algorithm is standard, not to diff the implementation.
    """
    u = hmac.new(passphrase, salt + bytes([0, 0, 0, 1]), hashlib.sha256).digest()
    block = bytearray(u)
    for _ in range(1, iterations):
        u = hmac.new(passphrase, u, hashlib.sha256).digest()
        for j in range(32):
            block[j] ^= u[j]
    return bytes(block)


def check_crypto(source, failures):
    """The envelope parameters, and whether the KDF is really PBKDF2."""
    for name, expected in sorted(CRYPTO_CONSTANTS.items()):
        match = re.search(r"\b%s\s*=\s*(\d+)" % name, source)
        if not match:
            failures.append("%s is gone from management.cpp" % name)
            print("FAIL  %s not found" % name)
        elif int(match.group(1)) != expected:
            failures.append("%s is %s, documented as %d"
                            % (name, match.group(1), expected))
            print("FAIL  %s = %s, expected %d" % (name, match.group(1), expected))
        else:
            print("ok    %s = %d" % (name, expected))

    # AES-256, and an authenticated mode. A silent move to CBC would lose the
    # tag, and with it the only thing that makes a wrong passphrase safe.
    if "mbedtls_gcm_auth_decrypt" not in source or "MBEDTLS_GCM_ENCRYPT" not in source:
        failures.append("the envelope no longer uses AES-GCM, so a wrong "
                        "passphrase would no longer be detected")
        print("FAIL  AES-GCM is not in use")
    elif not re.search(r"mbedtls_gcm_setkey\([^;]*?,\s*256\s*\)", source, re.S):
        failures.append("AES key size is not 256 bits")
        print("FAIL  AES is not keyed at 256 bits")
    else:
        print("ok    AES-256-GCM, authenticated both ways")

    # And the part that actually matters for interoperability.
    iterations = CRYPTO_CONSTANTS["SETTINGS_BACKUP_ITERATIONS"]
    cases = [
        (b"correct horse battery staple", bytes(range(16)), 1),
        (b"correct horse battery staple", bytes(range(16)), 2),
        (b"hunter22", b"\x00" * 16, 1000),
        (os.urandom(20), os.urandom(16), 4096),
        (b"", os.urandom(16), 17),
    ]
    bad = 0
    for passphrase, salt, rounds in cases:
        mine = derive_like_firmware(passphrase, salt, rounds)
        reference = hashlib.pbkdf2_hmac("sha256", passphrase, salt, rounds, dklen=32)
        if mine != reference:
            bad += 1
    if bad:
        failures.append("the key derivation is not standard "
                        "PBKDF2-HMAC-SHA256 (%d of %d cases differ from "
                        "hashlib) -- backups would be readable by nothing but "
                        "this firmware" % (bad, len(cases)))
        print("FAIL  key derivation differs from hashlib.pbkdf2_hmac")
    else:
        print("ok    key derivation is standard PBKDF2-HMAC-SHA256 (%d cases vs "
              "hashlib)" % len(cases))

    # A published SHA-256 vector, so a broken hashlib cannot make the above pass
    # by agreeing with an equally broken transliteration.
    vector = hashlib.pbkdf2_hmac("sha256", b"password", b"salt", 1, dklen=32).hex()
    if not vector.startswith("120fb6cffcf8b32c"):
        failures.append("hashlib disagrees with the published PBKDF2-SHA256 "
                        "vector, so the comparison above proves nothing")
        print("FAIL  reference implementation is itself wrong")
    else:
        print("ok    reference matches the published PBKDF2-SHA256 vector")

    # Iteration count is a judgement call, not a constant to assert blindly, but
    # a *low* one is worth saying out loud.
    if iterations < 10000:
        print("note  %d PBKDF2 iterations is low even for a microcontroller"
              % iterations)

def main():
    source = open(SOURCE, encoding="utf-8").read()
    failures = []

    written = keys_assigned(function_body(source, "handleSettingsBackup")) - SECTIONS
    read = keys_referenced(function_body(source, "handleSettingsRestore")) - SECTIONS

    # A run that parsed nothing is a broken test, not a passing one.
    if not written or not read:
        print("FAIL  parsed no keys at all -- did the handlers or their locals "
              "get renamed?")
        return 1

    def show(pairs):
        return ", ".join(
            "%s.%s" % (obj, key) if obj != "s" else key
            for obj, key in sorted(pairs)
        )

    # 1. Symmetry, both directions.
    orphans = written - read
    if orphans:
        failures.append("backed up but never restored: " + show(orphans))
        print("FAIL  %d key(s) are written to the file and never read back"
              % len(orphans))
    else:
        print("ok    every backed-up key is restored (%d)" % len(written))

    invented = read - written
    if invented:
        failures.append("restored but never backed up: " + show(invented))
        print("FAIL  %d key(s) are read from a file that never contains them"
              % len(invented))
    else:
        print("ok    every restored key is backed up (%d)" % len(read))

    # 2. Completeness against what actually reaches NVS.
    persisted = set()
    for name in SAVE_FUNCTIONS:
        body = function_body(source, name)
        persisted |= set(re.findall(r"prefs\.put\w+\(\"([A-Za-z0-9]+)\"", body))

    # Every stored key has to be accounted for: either it has a home in the file
    # or it is on the documented exclusion list. An NVS key in neither is a
    # setting somebody added and nobody arranged to restore.
    homeless = persisted - set(NVS_HOME) - set(EXCLUDED)
    if homeless:
        failures.append("stored in NVS with no home in the backup: "
                        + ", ".join(sorted(homeless)))
        print("FAIL  %d stored setting(s) have no place in a backup file"
              % len(homeless))
    else:
        print("ok    all %d stored settings have a named home in the backup"
              % len(persisted))

    # And the table cannot go stale in the other direction either.
    ghosts = set(NVS_HOME) - persisted
    if ghosts:
        failures.append("this test names a home for keys NVS no longer stores: "
                        + ", ".join(sorted(ghosts)))
        print("FAIL  %d stale entr(y/ies) in NVS_HOME" % len(ghosts))
    else:
        print("ok    the NVS_HOME table matches what is stored")

    # The destinations themselves have to be in the file. A leaf shows up in
    # `written`; a composite is a SECTIONS object, which `written` excludes by
    # construction, so both are checked against the union.
    backed_up = len(written)
    in_file = written | SECTIONS
    missing = {home for home in NVS_HOME.values() if home not in in_file}
    if missing:
        failures.append("named a home that the backup does not write: "
                        + show(missing))
        print("FAIL  %d setting(s) name a backup key that is never written"
              % len(missing))
    else:
        print("ok    every home the table names is actually written")

    absent = EXTERNAL_KEYS - in_file
    if absent:
        failures.append("settings from another module's namespace are missing "
                        "from the backup: " + show(absent))
        print("FAIL  %d external setting(s) are not in the file" % len(absent))
    else:
        print("ok    the other modules' settings are in the backup (%d)"
              % len(EXTERNAL_KEYS))

    if not CLOCK_KEYS <= written:
        failures.append("clock preferences missing from the backup: "
                        + show(CLOCK_KEYS - written))
        print("FAIL  the clock namespace is not backed up")
    else:
        print("ok    the clock namespace is backed up")

    if not SECRET_KEYS <= written:
        failures.append("credentials missing from the backup envelope: "
                        + show(SECRET_KEYS - written))
        print("FAIL  the encrypted envelope does not carry all four secrets")
    else:
        print("ok    all four secrets are in the encrypted envelope")

    # The secrets must never be written into the readable half of the file. This
    # is the check that would catch somebody "simplifying" the envelope away.
    plaintext = {key for obj, key in written
                 if obj == "s" and (("sec", key) in SECRET_KEYS)}
    if plaintext:
        failures.append("written to the file in clear text: "
                        + ", ".join(sorted(plaintext)))
        print("FAIL  %d secret(s) are backed up unencrypted" % len(plaintext))
    else:
        print("ok    no secret is written in clear text")

    check_crypto(source, failures)

    # 3. The deliberate omissions are still exactly the documented ones.
    everything = set(re.findall(r"prefs\.put\w+\(\"([A-Za-z0-9]+)\"", source))
    left_out = everything - persisted
    if left_out != set(EXCLUDED):
        surprise = left_out - set(EXCLUDED)
        gone = set(EXCLUDED) - left_out
        if surprise:
            failures.append(
                "stored outside saveSettings() and not in the backup, with no "
                "documented reason: " + ", ".join(sorted(surprise)))
            print("FAIL  %d undocumented key(s) escape the backup"
                  % len(surprise))
        if gone:
            # Not a defect -- but this test is now asserting something stale.
            failures.append(
                "no longer written outside saveSettings(), so the exclusion note "
                "in this test is stale: " + ", ".join(sorted(gone)))
            print("FAIL  stale exclusion(s) in this test")
    else:
        print("ok    the %d deliberate omissions are the documented ones (%s)"
              % (len(EXCLUDED), ", ".join(sorted(EXCLUDED))))

    if failures:
        print("\n%d problem(s) with the backup format:\n" % len(failures))
        for line in failures:
            print("  " + line)
        print("\nhandleSettingsBackup() and handleSettingsRestore() in "
              "src/management.cpp are meant to be edited together.")
        return 1

    print("\nthe settings backup format is symmetric and complete: %d keys, "
          "%d stored settings, %d from other namespaces"
          % (backed_up, len(persisted), len(EXTERNAL_KEYS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
