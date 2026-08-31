#!/usr/bin/env python3
"""
Draws text with a u8g2 font's real bitmaps, as ASCII art.

The shaping test can prove the codepoints are right without proving the result
looks like Persian. This renders the actual glyph bitmaps out of u8g2_fonts.c,
so the output can be compared against the script by eye once -- which is the
only check that catches a table that is self-consistent and still wrong.

Glyph format, from u8g2_font_decode_glyph()/u8g2_font_decode_len():
  after the per-glyph header bits (width, height, x, y, delta_x) the bitmap is
  run-length coded -- a run of `bits_per_0` background pixels then a run of
  `bits_per_1` foreground pixels, repeated while a following single bit is set,
  wrapping at the glyph width. The bitmap's top-left sits at
  (pen_x + x, baseline - height - y).
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import font_coverage  # noqa: E402

HEADER = 23


class Bits:
    def __init__(self, data, byte_pos):
        self.data = data
        self.pos = byte_pos * 8

    def u(self, n):
        v = 0
        for i in range(n):
            v |= ((self.data[self.pos >> 3] >> (self.pos & 7)) & 1) << i
            self.pos += 1
        return v

    def s(self, n):
        # Biased, not two's complement -- see u8g2_font_decode_get_signed_bits.
        return self.u(n) - (1 << (n - 1)) if n else 0


class Font:
    def __init__(self, data):
        self.d = data
        self.bits_0 = data[2]
        self.bits_1 = data[3]
        self.bpw, self.bph = data[4], data[5]
        self.bpx, self.bpy, self.bpd = data[6], data[7], data[8]
        self.ascent = data[13]
        self.descent = data[14] - 256 if data[14] > 127 else data[14]
        self.glyphs = {}
        p = HEADER
        while p + 1 < len(data) and data[p + 1] != 0:
            self.glyphs[data[p]] = p + 2
            p += data[p + 1]
        start = (data[21] << 8) | data[22]
        if start:
            table = HEADER + start
            off = (data[table] << 8) | data[table + 1]
            p = table + off
            while p + 2 < len(data):
                e = (data[p] << 8) | data[p + 1]
                jump = data[p + 2]
                if e == 0 or jump == 0:
                    break
                self.glyphs[e] = p + 3
                p += jump

    def glyph(self, cp):
        """(advance, width, height, x, y, [(lx,ly)...set pixels])."""
        at = self.glyphs.get(cp)
        if at is None:
            return None
        b = Bits(self.d, at)
        w = b.u(self.bpw)
        h = b.u(self.bph)
        x = b.s(self.bpx)
        y = b.s(self.bpy)
        d = b.s(self.bpd)
        pix = []
        if w > 0:
            lx = ly = 0
            while ly < h:
                a = b.u(self.bits_0)
                c = b.u(self.bits_1)
                while True:
                    for run, on in ((a, False), (c, True)):
                        cnt = run
                        while cnt > 0:
                            rem = w - lx
                            cur = min(cnt, rem)
                            if on:
                                for i in range(cur):
                                    pix.append((lx + i, ly))
                            lx += cur
                            cnt -= cur
                            if lx >= w:
                                lx = 0
                                ly += 1
                    if b.u(1) == 0:
                        break
                if ly >= h:
                    break
        return d, w, h, x, y, pix


def render(font, cps, pad=1):
    """A list of rows of '#'/'.' for one line of text."""
    cells = []
    pen = 0
    for cp in cps:
        g = font.glyph(cp)
        if g is None:
            pen += 4
            continue
        d, w, h, gx, gy, pix = g
        for lx, ly in pix:
            cells.append((pen + gx + lx, -h - gy + ly))
        pen += d
    if not cells:
        return [], 0
    xs = [c[0] for c in cells]
    ys = [c[1] for c in cells]
    x0, x1 = min(xs) - pad, max(xs) + pad
    y0, y1 = min(ys) - pad, max(ys) + pad
    on = set(cells)
    rows = []
    for y in range(y0, y1 + 1):
        rows.append("".join("#" if (x, y) in on else "."
                            for x in range(x0, x1 + 1)))
    return rows, pen


def render_all(font_path, lines, labels=None):
    os.environ["U8G2_FONTS"] = font_path
    import importlib
    importlib.reload(font_coverage)
    data, declared = font_coverage.extract("unifont_t_arabic")
    if not data or len(data) + 1 != declared:
        raise SystemExit("could not parse u8g2_font_unifont_t_arabic")
    font = Font(data)
    print("\nRendered with u8g2_font_unifont_t_arabic "
          "(ascent %d, descent %d).\nRead left to right, as the panel draws it."
          % (font.ascent, font.descent))
    for i, cps in enumerate(lines):
        rows, adv = render(font, cps)
        label = labels[i] if labels else ""
        print("\n--- %s  (%d px wide, %s of the 128 px panel) ---"
              % (label, adv, "fits" if adv <= 128 else "TOO WIDE"))
        for r in rows:
            print("   " + r)


if __name__ == "__main__":
    path = os.environ.get(
        "U8G2_FONTS",
        os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                     ".pio", "libdeps", "esp32dev", "U8g2", "src", "clib",
                     "u8g2_fonts.c"))
    render_all(path, [[ord(c) for c in "Hi"]], ["ascii sanity check"])
