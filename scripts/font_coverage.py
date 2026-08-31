"""Report which codepoints a u8g2 font actually contains.

Format, from u8g2_font_get_glyph_data() in u8g2_font.c:
  23-byte header; bytes 21..22 are start_pos_unicode (big endian).
  At that offset: a lookup table of 4-byte entries (offset_delta, max_encoding).
  Glyph records are (2-byte BE encoding, 1-byte jump, data...), encoding 0 ends.

The arrays are C string literals whose *data* contains literal ';' and '"'
characters, so the declaration has to be scanned rather than regex-matched.
"""
import re
import sys

import os
SRC = os.environ.get('U8G2_FONTS',
    r'c:/Projects/esp32-s3-blue-spk/.pio/libdeps/esp32dev/U8g2/src/clib/u8g2_fonts.c')
HEADER = 23

text = open(SRC, encoding='utf-8', errors='replace').read()

SIMPLE = {'n': 10, 't': 9, 'r': 13, '\\': 92, '"': 34, "'": 39, 'a': 7,
          'b': 8, 'f': 12, 'v': 11, '?': 63}


def extract(name):
    """The font's bytes, and the length its declaration claims."""
    decl = re.search(r'u8g2_font_' + re.escape(name) + r'\[(\d+)\][^=]*=', text)
    if not decl:
        return None, None
    declared = int(decl.group(1))
    i = decl.end()
    out = bytearray()
    while i < len(text):
        c = text[i]
        if c == ';':
            break
        if c != '"':
            i += 1
            continue
        # Inside a string literal until the next unescaped quote.
        i += 1
        while i < len(text) and text[i] != '"':
            if text[i] != '\\':
                out.append(ord(text[i]) & 0xFF)
                i += 1
                continue
            n = text[i + 1]
            if n == 'x':
                j, h = i + 2, ''
                while j < len(text) and text[j] in '0123456789abcdefABCDEF' and len(h) < 2:
                    h += text[j]
                    j += 1
                out.append(int(h, 16))
                i = j
            elif n in '01234567':
                j, o = i + 1, ''
                while j < len(text) and text[j] in '01234567' and len(o) < 3:
                    o += text[j]
                    j += 1
                out.append(int(o, 8) & 0xFF)
                i = j
            else:
                out.append(SIMPLE.get(n, ord(n)))
                i += 2
        i += 1
    return bytes(out), declared


def coverage(data):
    start_unicode = (data[21] << 8) | data[22]
    ascii_cps = set()
    p = HEADER
    while p + 1 < len(data) and data[p + 1] != 0:
        ascii_cps.add(data[p])
        p += data[p + 1]
    uni = set()
    if start_unicode:
        table = HEADER + start_unicode
        if table + 4 <= len(data):
            off = (data[table] << 8) | data[table + 1]
            p = table + off
            while p + 2 < len(data):
                e = (data[p] << 8) | data[p + 1]
                jump = data[p + 2]
                if e == 0 or jump == 0:
                    break
                uni.add(e)
                p += jump
    return ascii_cps, uni


BLOCKS = [
    ("Arabic U+0600-06FF", 0x0600, 0x06FF),
    ("Arabic Supplement", 0x0750, 0x077F),
    ("Presentation Forms-A", 0xFB50, 0xFDFF),
    ("Presentation Forms-B", 0xFE70, 0xFEFF),
]

FARSI = {0x067E: 'pe', 0x0686: 'che', 0x0698: 'zhe', 0x06AF: 'gaf',
         0x06A9: 'keheh', 0x06CC: 'farsi yeh'}

for name in sys.argv[1:]:
    data, declared = extract(name)
    if data is None:
        print('%-24s NOT FOUND' % name)
        continue
    status = 'ok' if len(data) + 1 == declared else 'PARSE MISMATCH'
    print('=' * 74)
    print('%s  %d bytes (declared %d) %s' % (name, len(data), declared, status))
    if len(data) + 1 != declared:
        continue
    ascii_cps, uni = coverage(data)
    desc = data[14] - 256 if data[14] > 127 else data[14]
    print('  glyphs: %d ascii + %d unicode   height=%d ascent=%d descent=%d'
          % (len(ascii_cps), len(uni), data[10], data[13], desc))
    for label, lo, hi in BLOCKS:
        n = sum(1 for c in uni if lo <= c <= hi)
        if n:
            print('  %-22s %4d glyphs' % (label, n))
    if uni:
        arabic = sorted(c for c in uni if 0x0600 <= c <= 0x06FF)
        forms = sorted(c for c in uni if 0xFE70 <= c <= 0xFEFF)
        if arabic:
            print('  base Arabic range: U+%04X..U+%04X' % (arabic[0], arabic[-1]))
        if forms:
            print('  presentation forms: U+%04X..U+%04X' % (forms[0], forms[-1]))
        miss = [(c, n) for c, n in sorted(FARSI.items()) if c not in uni]
        print('  Persian-specific letters missing: %s'
              % (', '.join('U+%04X %s' % (c, n) for c, n in miss) or 'none'))
        pd = [c for c in range(0x06F0, 0x06FA) if c not in uni]
        print('  Persian digits U+06F0-9: %s'
              % ('all present' if not pd else '%d of 10 missing' % len(pd)))
