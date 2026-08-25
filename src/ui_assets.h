/*
 * ui_assets.h -- the icons, hand-drawn.
 *
 * These are XBM bitmaps for u8g2's drawXBMP(). They are drawn by hand rather
 * than taken from an icon font for two reasons: U8g2's icon fonts have no
 * Bluetooth glyph at all, and a font big enough to hold one costs several
 * kilobytes of flash where these cost 8 to 32 bytes each.
 *
 * XBM puts the *leftmost* pixel in the *least* significant bit of each byte,
 * which makes the numbers unreadable as pictures. So each row below is written
 * the way it looks -- leftmost pixel first, as the high bit -- and xr() flips it
 * at compile time. The comment beside each row is therefore also the row.
 */

#pragma once

#include <stdint.h>

/// Reverses one byte, so icon rows can be written left-to-right in source.
static constexpr uint8_t xr(uint8_t v) {
  return (uint8_t)(((v & 0x01) << 7) | ((v & 0x02) << 5) | ((v & 0x04) << 3) |
                   ((v & 0x08) << 1) | ((v & 0x10) >> 1) | ((v & 0x20) >> 3) |
                   ((v & 0x40) >> 5) | ((v & 0x80) >> 7));
}

// ------------------------------------------------------------- 8x8 icons -----

/// The Bluetooth rune.
static const uint8_t ICON_BT[8] = {
    xr(0b00010000),  // ...#....
    xr(0b00011000),  // ...##...
    xr(0b01010100),  // .#.#.#..
    xr(0b00111000),  // ..###...
    xr(0b00111000),  // ..###...
    xr(0b01010100),  // .#.#.#..
    xr(0b00011000),  // ...##...
    xr(0b00010000),  // ...#....
};

/// Eighth note -- the "now playing" marker.
static const uint8_t ICON_NOTE[8] = {
    xr(0b00001111),  // ....####
    xr(0b00001001),  // ....#..#
    xr(0b00001001),  // ....#..#
    xr(0b00001000),  // ....#...
    xr(0b00001000),  // ....#...
    xr(0b00111000),  // ..###...
    xr(0b01111100),  // .#####..
    xr(0b00111000),  // ..###...
};

static const uint8_t ICON_PLAY[8] = {
    xr(0b01000000),  // .#......
    xr(0b01100000),  // .##.....
    xr(0b01110000),  // .###....
    xr(0b01111000),  // .####...
    xr(0b01111000),  // .####...
    xr(0b01110000),  // .###....
    xr(0b01100000),  // .##.....
    xr(0b01000000),  // .#......
};

static const uint8_t ICON_PAUSE[8] = {
    xr(0b01101100),  // .##.##..
    xr(0b01101100),  // .##.##..
    xr(0b01101100),  // .##.##..
    xr(0b01101100),  // .##.##..
    xr(0b01101100),  // .##.##..
    xr(0b01101100),  // .##.##..
    xr(0b01101100),  // .##.##..
    xr(0b01101100),  // .##.##..
};

static const uint8_t ICON_STOP[8] = {
    xr(0b00000000),  // ........
    xr(0b01111110),  // .######.
    xr(0b01111110),  // .######.
    xr(0b01111110),  // .######.
    xr(0b01111110),  // .######.
    xr(0b01111110),  // .######.
    xr(0b01111110),  // .######.
    xr(0b00000000),  // ........
};

static const uint8_t ICON_CLOCK[8] = {
    xr(0b00111100),  // ..####..
    xr(0b01000010),  // .#....#.
    xr(0b10010001),  // #..#...#
    xr(0b10010001),  // #..#...#
    xr(0b10011101),  // #..###.#
    xr(0b10000001),  // #......#
    xr(0b01000010),  // .#....#.
    xr(0b00111100),  // ..####..
};

static const uint8_t ICON_PHONE[8] = {
    xr(0b01111110),  // .######.
    xr(0b01000010),  // .#....#.
    xr(0b01000010),  // .#....#.
    xr(0b01000010),  // .#....#.
    xr(0b01000010),  // .#....#.
    xr(0b01000010),  // .#....#.
    xr(0b01011010),  // .#.##.#.
    xr(0b01111110),  // .######.
};

/// Wi-Fi radio waves, used by provisioning and dashboard status overlays.
static const uint8_t ICON_WIFI[8] = {
    xr(0b01111110),
    xr(0b10000001),
    xr(0b00111100),
    xr(0b01000010),
    xr(0b00011000),
    xr(0b00100100),
    xr(0b00000000),
    xr(0b00011000),
};

/// Down arrow into a tray: firmware download / install.
static const uint8_t ICON_UPDATE[8] = {
    xr(0b00011000),
    xr(0b00011000),
    xr(0b00011000),
    xr(0b01011010),
    xr(0b00111100),
    xr(0b00011000),
    xr(0b00000000),
    xr(0b01111110),
};

static const uint8_t ICON_OK[8] = {
    xr(0b00000000),
    xr(0b00000001),
    xr(0b00000011),
    xr(0b01000110),
    xr(0b01101100),
    xr(0b00111000),
    xr(0b00010000),
    xr(0b00000000),
};

static const uint8_t ICON_ERROR[8] = {
    xr(0b01000010),
    xr(0b00100100),
    xr(0b00011000),
    xr(0b00011000),
    xr(0b00011000),
    xr(0b00100100),
    xr(0b01000010),
    xr(0b00000000),
};

// ------------------------------------------------------------ 16x8 icons -----
// Two bytes per row: the first byte is columns 0-7, the second 8-15.

/// Speaker with three arcs -- the volume popup.
static const uint8_t ICON_SPEAKER[16] = {
    xr(0b00000100), xr(0b00000000),  // .....#..........
    xr(0b00001100), xr(0b00001000),  // ....##......#...
    xr(0b00011100), xr(0b00101000),  // ...###....#.#...
    xr(0b01111100), xr(0b10101000),  // .#####..#.#.#...
    xr(0b01111100), xr(0b10101000),  // .#####..#.#.#...
    xr(0b00011100), xr(0b00101000),  // ...###....#.#...
    xr(0b00001100), xr(0b00001000),  // ....##......#...
    xr(0b00000100), xr(0b00000000),  // .....#..........
};

/// Same speaker, muted.
static const uint8_t ICON_MUTE[16] = {
    xr(0b00000100), xr(0b00000000),  // .....#..........
    xr(0b00001100), xr(0b00000000),  // ....##..........
    xr(0b00011100), xr(0b10001000),  // ...###..#...#...
    xr(0b01111100), xr(0b01010000),  // .#####...#.#....
    xr(0b01111100), xr(0b00100000),  // .#####....#.....
    xr(0b00011100), xr(0b01010000),  // ...###...#.#....
    xr(0b00001100), xr(0b10001000),  // ....##..#...#...
    xr(0b00000100), xr(0b00000000),  // .....#..........
};

// ----------------------------------------------------------- 16x16 icons -----

/// The big Bluetooth rune for the pairing screen, with the beacon rings drawn
/// around it in code so they can pulse.
static const uint8_t ICON_BT_BIG[32] = {
    xr(0b00000001), xr(0b10000000),  // .......##.......
    xr(0b00000001), xr(0b11000000),  // .......###......
    xr(0b00000001), xr(0b01100000),  // .......#.##.....
    xr(0b01000001), xr(0b00110000),  // .#.....#..##....
    xr(0b00100001), xr(0b00011000),  // ..#....#...##...
    xr(0b00010001), xr(0b00110000),  // ...#...#..##....
    xr(0b00001001), xr(0b01100000),  // ....#..#.##.....
    xr(0b00000101), xr(0b11000000),  // .....#.###......
    xr(0b00000101), xr(0b11000000),  // .....#.###......
    xr(0b00001001), xr(0b01100000),  // ....#..#.##.....
    xr(0b00010001), xr(0b00110000),  // ...#...#..##....
    xr(0b00100001), xr(0b00011000),  // ..#....#...##...
    xr(0b01000001), xr(0b00110000),  // .#.....#..##....
    xr(0b00000001), xr(0b01100000),  // .......#.##.....
    xr(0b00000001), xr(0b11000000),  // .......###......
    xr(0b00000001), xr(0b10000000),  // .......##.......
};
