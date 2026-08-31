#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Arabic-script text, made drawable.
 *
 * u8g2 draws a UTF-8 string one glyph at a time, left to right, exactly as the
 * bytes arrive. That is all a Latin script needs and none of what Persian
 * needs, because two things happen between "what the phone sent" and "what
 * belongs on the panel":
 *
 *   Shaping.   Arabic script is cursive. Every letter has up to four shapes --
 *              isolated, initial, medial, final -- and which one is correct
 *              depends on whether the letters either side of it join. Unicode
 *              stores the letters in one block and the shapes in another, so
 *              handing the letters straight to a font draws them all in
 *              isolated form: legible only in the way that S O M E T H I N G
 *              L I K E T H I S is legible. Some pairs, lam followed by alef,
 *              must combine into a single glyph and are simply wrong apart.
 *
 *   Ordering.  Persian reads right to left. u8g2 draws left to right. So the
 *              string has to be handed over reversed -- but only the
 *              right-to-left parts of it: a Latin word or a number embedded in
 *              a Persian title still reads left to right, and reversing those
 *              too would turn "ESP32" into "23PSE".
 *
 * text_arabic_visual() does both, and its output is an ordinary UTF-8 string
 * that any font carrying the presentation forms can draw with no further help.
 *
 * What this is not: it is not a full implementation of UAX #9, the bidirectional
 * algorithm. There are no explicit embedding controls, no bracket-pair
 * resolution, and neutral runs simply inherit the direction of what precedes
 * them. That is a deliberate limit -- the whole of UAX #9 is a large amount of
 * code to arbitrate cases that do not arise in a track title on a 128x32 panel,
 * and getting the common case right in a page of code is the better trade. A
 * string of Persian, a string of Latin, and either one with the other embedded
 * in it all come out correct.
 */

/// True if the string contains a codepoint from the Arabic block (U+0600..06FF),
/// which is the test for "this needs shaping, and a font that can draw it".
bool text_has_arabic(const char *s);

/*
 * Shapes and reorders a logical-order UTF-8 string into visual order.
 *
 * Safe on any input: a string with no Arabic in it is copied through unchanged,
 * so callers do not have to test first. Always NUL-terminates, and never writes
 * more than `cap` bytes. Returns dst, so it can be used in an expression.
 *
 * Truncation is by codepoint, never mid-sequence: a string too long for the
 * buffer loses whole characters off the end rather than emitting a half-encoded
 * one that would draw as a replacement box.
 */
char *text_arabic_visual(const char *src, char *dst, size_t cap);

/*
 * How large a destination buffer has to be for a source of `src_bytes`.
 *
 * Shaping grows the string: a Persian letter is two bytes as a codepoint
 * (U+06xx) and three as a presentation form (U+FBxx / U+FExx). So the worst
 * case is half again, plus the terminator. Lam-alef shrinks two letters into
 * one glyph and only ever helps.
 */
constexpr size_t text_arabic_cap(size_t src_bytes) {
  return src_bytes + src_bytes / 2 + 4;
}
