/*
 * diagnostics.h -- one command that answers "what is this speaker actually
 * doing", over the serial console.
 *
 * The dashboard already reports most of this, in more detail and more
 * legibly -- but the dashboard exists in two of the three radio modes, needs a
 * network the speaker may be failing to join, and is precisely what is missing
 * when something has gone wrong enough to be worth diagnosing. Bluetooth mode
 * has no web server at all. The serial port is there in every mode, from the
 * first line of setup(), whatever the radios are doing.
 *
 * Cost: nothing until it is typed. Every number below is either already being
 * kept for another reason or is a single call into the IDF, so there is no
 * collection to run in the background and nothing to switch off in a release
 * build. It prints about forty lines and then stops -- it is not a monitor, and
 * nothing here is rate-limited because nothing here repeats.
 *
 * What it is for, in rough order of how often each answers the question:
 *
 *   the reset reason      a panic, a watchdog, a brownout and a deliberate
 *                         reboot all look identical from outside: the speaker
 *                         came back. They need completely different fixes.
 *   the heap trio         free, the lowest it has ever been, and the largest
 *                         single block. The third one is the one that matters
 *                         for TLS and for Bluedroid: a heap that is 60 KB free
 *                         in 2 KB pieces cannot start either.
 *   stack high water      how close each task has come to overflowing. A task
 *                         that overflows takes the chip down with no warning,
 *                         and the margin is invisible until it is gone.
 *   what was detected     the OLED, the RTC, the ring, the module and the
 *                         gauge are all optional, all absent-tolerant, and all
 *                         therefore capable of being silently missing.
 *   the link counters     whether the DFPlayer and the I2C bus are healthy, as
 *                         numbers rather than as an impression.
 */

#pragma once

#include "app_config.h"

#if DIAGNOSTICS_ENABLED
/// Handles the "diag" console command. Returns false if the line was something
/// else, so the caller can try its own commands.
bool diagnostics_command(const char *line);
#else
/*
 * A release build has no diag report. The whole of diagnostics.cpp is compiled
 * to nothing, which is the ~15 kB of format strings that make the report
 * readable -- everything it prints is either already on the dashboard or is a
 * question you cannot ask a unit in somebody's kitchen anyway.
 */
inline bool diagnostics_command(const char *) { return false; }
#endif
