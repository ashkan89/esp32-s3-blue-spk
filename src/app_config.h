#pragma once

#ifndef APP_NAME
#define APP_NAME "esp32-blue-spk"
#endif

// Keep this in sync with GitHub release tags. A leading "v" on the release is
// ignored when the dashboard compares versions.
#ifndef FW_VERSION
#define FW_VERSION "2.8.0"
#endif

// Can be overridden with build flags, for example:
//   -DDEFAULT_GITHUB_REPO=\"owner/esp32-blue-spk\"
#ifndef DEFAULT_GITHUB_REPO
#define DEFAULT_GITHUB_REPO "ashkan89/esp32-blue-spk"
#endif

#ifndef DEFAULT_GITHUB_ASSET
#define DEFAULT_GITHUB_ASSET "*.bin"
#endif

#ifndef MANAGEMENT_ENABLED
#define MANAGEMENT_ENABLED 1
#endif

/*
 * What a release build leaves out.
 *
 * The development build talks: it narrates the boot, names every state change,
 * and offers a console that can drive every subsystem by hand. All of that is
 * how this firmware was made debuggable, and none of it belongs on a unit
 * sitting in somebody's kitchen -- where the UART is a pad on a board, the
 * running commentary is a few tens of kilobytes of format strings, and the
 * console is an unauthenticated control channel for anybody who can reach two
 * pins.
 *
 * So `pio run -e release` sets all three to 0 and the compiler removes them.
 * Not "does not print at runtime" -- removes: the strings never reach the
 * image, and the command handlers lose their only caller and are dropped by
 * --gc-sections along with everything only they referenced.
 *
 *   SERIAL_LOG           the running commentary. LOGF/LOGLN/LOGP below.
 *   CONSOLE_ENABLED      the interactive console in main.cpp, and with it every
 *                        *_command() handler in the firmware. ui_command() is
 *                        the exception: the dashboard's display controls go
 *                        through it, so it survives either way.
 *   DIAGNOSTICS_ENABLED  the `diag` report.
 *
 * The BOOT button still cycles radio modes and still runs the factory reset
 * with no console at all, so a release unit is never locked out of a mode it
 * cannot reach the dashboard from.
 */
#ifndef SERIAL_LOG
#define SERIAL_LOG 1
#endif

#ifndef CONSOLE_ENABLED
#define CONSOLE_ENABLED 1
#endif

#ifndef DIAGNOSTICS_ENABLED
#define DIAGNOSTICS_ENABLED 1
#endif

/// True when anything at all wants the UART, which is what decides whether
/// setup() opens it.
#define SERIAL_PORT_USED (SERIAL_LOG || CONSOLE_ENABLED || DIAGNOSTICS_ENABLED)

/*
 * The logging macros every .cpp in this project uses instead of Serial.
 *
 * With SERIAL_LOG on they are exactly Serial.printf/println/print, so nothing
 * about the development build changed. With it off they expand to nothing at
 * all: no call, no arguments evaluated, and -- the point of the exercise -- no
 * format string in the image.
 *
 * That last part is why this is a macro rather than an empty inline function.
 * An inline that ignores its arguments still forces the literals to be emitted;
 * a macro that never mentions them means the compiler never sees them.
 *
 * Nothing in this firmware logs from an expression whose value is used, or
 * with an argument that has a side effect -- both were checked before the
 * switch existed, and both would be silently broken by turning it off.
 */
#if SERIAL_LOG
#include <Arduino.h>
#define LOGF(...) Serial.printf(__VA_ARGS__)
#define LOGLN(...) Serial.println(__VA_ARGS__)
#define LOGP(...) Serial.print(__VA_ARGS__)
#define LOGFLUSH() Serial.flush()
#else
#define LOGF(...) ((void)0)
#define LOGLN(...) ((void)0)
#define LOGP(...) ((void)0)
#define LOGFLUSH() ((void)0)
#endif
