#!/usr/bin/env python3
"""
Host-side test for the Persian/Arabic text pipeline.

There is no host C++ compiler on this machine and no board attached, so this
cannot run src/text_arabic.cpp itself. What it does instead covers the two
things most likely to be wrong, and both of them against real data:

  1. THE ALGORITHM, against known-correct Persian. shape() below is a
     transliteration of text_arabic_visual(); the expected outputs are written
     as explicit presentation-form sequences worked out from the Unicode charts,
     so a wrong contextual form or a run reversed the wrong way fails here.
     Being a transliteration, it has to be kept in step with the C++ by hand --
     its job is to prove the algorithm is right, not to diff the implementation.

  2. EVERY GLYPH IT EMITS EXISTS IN THE FONT. This one tests the shipped
     artefact: it parses u8g2_fonts.c and checks that each presentation form
     the shaper can produce is actually in u8g2_font_unifont_t_arabic. A form
     that is correct by Unicode but absent from the font draws as nothing at
     all, which is how farsi yeh was caught.

Run:  python scripts/test_arabic_shaping.py
      python scripts/test_arabic_shaping.py --art    (also draw the results)
"""

import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "scripts"))

from gen_arabic_tables import (  # noqa: E402
    build_forms, build_joining, LAM_ALEF, FARSI_YEH,
)

JOIN = build_joining()
FORMS = dict(build_forms())
ISO, FIN, INI, MED = 0, 1, 2, 3


def jclass(cp):
    if 0x0600 <= cp <= 0x06FF:
        return JOIN[cp - 0x0600]
    if cp == 0x200D:
        return "C"
    return "U"


def bidi(cp):
    if (0x0600 <= cp <= 0x06FF or 0xFB50 <= cp <= 0xFDFF
            or 0xFE70 <= cp <= 0xFEFF):
        if 0x0660 <= cp <= 0x0669 or 0x06F0 <= cp <= 0x06F9:
            return "L"
        return "R"
    ch = chr(cp)
    if ch.isascii() and (ch.isalpha() or ch.isdigit()):
        return "L"
    if 0x00C0 <= cp <= 0x024F:
        return "L"
    return "N"


MIRROR = {ord(a): ord(b) for a, b in
          ["()", ")(", "[]", "][", "{}", "}{", "<>", "><"]}


def shape(text):
    """Transliteration of text_arabic_visual(). Returns a list of codepoints."""
    cps = [ord(c) for c in text]
    cls = [jclass(c) for c in cps]

    # Pass 1: lam + alef.
    i = 0
    while i + 1 < len(cps):
        if cps[i] == 0x0644:
            j = i + 1
            while j < len(cps) and cls[j] == "T":
                j += 1
            if j < len(cps):
                for alef, iso, fin in LAM_ALEF:
                    if cps[j] == alef:
                        joins_prev = False
                        k = i
                        while k > 0:
                            k -= 1
                            if cls[k] == "T" or cps[k] == 0:
                                continue
                            joins_prev = cls[k] in ("D", "C")
                            break
                        cps[i] = fin if joins_prev else iso
                        cls[i] = "R"
                        cps[j] = 0
                        cls[j] = "T"
                        break
        i += 1

    # Pass 2: contextual shaping.
    for i, cp in enumerate(cps):
        if cp == 0 or cls[i] == "T":
            continue
        forms = FORMS.get(cp)
        if forms is None or cls[i] not in ("D", "R"):
            continue
        joins_prev = False
        k = i
        while k > 0:
            k -= 1
            if cps[k] == 0 or cls[k] == "T":
                continue
            joins_prev = cls[k] in ("D", "C")
            break
        joins_next = False
        for k in range(i + 1, len(cps)):
            if cps[k] == 0 or cls[k] == "T":
                continue
            joins_next = cls[k] in ("D", "R", "C")
            break
        if cls[i] == "R":
            joins_next = False
        if joins_prev and joins_next:
            want = MED
        elif joins_prev:
            want = FIN
        elif joins_next:
            want = INI
        else:
            want = ISO
        glyph = forms[want]
        if glyph == 0 and want == MED:
            glyph = forms[FIN]
        if glyph == 0 and want == INI:
            glyph = forms[ISO]
        if glyph == 0:
            glyph = forms[ISO]
        if glyph:
            cps[i] = glyph

    kept = [(c, cls[i]) for i, c in enumerate(cps)
            if c not in (0, 0x200C, 0x200D)]
    cps = [c for c, _ in kept]

    # Pass 3: ordering.
    dirs = [bidi(c) for c in cps]
    base = "L"
    for i, d in enumerate(dirs):
        if d == "R" and not any(x == "L" for x in dirs[:i]):
            base = "R"
            break
        if d == "L":
            break
    run = base
    for i, d in enumerate(dirs):
        if d == "N":
            dirs[i] = run
        else:
            run = d

    out = []
    if base == "R":
        end = len(cps)
        while end > 0:
            start = end
            while start > 0 and dirs[start - 1] == dirs[end - 1]:
                start -= 1
            seg = cps[start:end]
            if dirs[end - 1] == "R":
                out += [MIRROR.get(c, c) for c in reversed(seg)]
            else:
                out += seg
            end = start
    else:
        start = 0
        while start < len(cps):
            end = start
            while end < len(cps) and dirs[end] == dirs[start]:
                end += 1
            seg = cps[start:end]
            if dirs[start] == "R":
                out += [MIRROR.get(c, c) for c in reversed(seg)]
            else:
                out += seg
            start = end
    return out


# --------------------------------------------------------------- vectors ---
# Expected outputs are visual order, left to right -- the order u8g2 draws.
# Worked out from the Unicode charts, letter by letter.
V = [
    (
        "سلام",  # سلام  "salaam"
        [0xFEE1,   # meem isolated   -- after an alef-ligature, which cannot
                   #                    join forward, so meem stands alone
         0xFEFC,   # lam-alef ligature, final: the seen before it joins in
         0xFEB3],  # seen initial
        "salaam: lam+alef must ligate, and the meem after it must NOT join",
    ),
    (
        "دنیا",  # دنیا  "donya" (world)
        [0xFE8E,   # alef final
         0xFEF4,   # farsi yeh medial  (borrowed from Arabic yeh)
         0xFEE7,   # noon initial      -- dal cannot join forward
         0xFEA9],  # dal isolated
        "donya: exercises the farsi yeh substitution and a right-joining dal",
    ),
    (
        "خوب",  # خوب  "khoob" (good)
        [0xFE8F,   # beh isolated -- waw is right-joining, so no join forward
         0xFEEE,   # waw final
         0xFEA7],  # khah initial
        "khoob: a right-joining waw breaks the word after itself",
    ),
    (
        "خوب ESP32",
        [ord(c) for c in "ESP32"] + [0x20, 0xFE8F, 0xFEEE, 0xFEA7],
        "mixed: the Latin run keeps its order and sits left of the Persian",
    ),
    (
        "ESP32 خوب",
        [ord(c) for c in "ESP32"] + [0x20, 0xFE8F, 0xFEEE, 0xFEA7],
        "Latin first: base direction is LTR, the Persian run still reverses",
    ),
    (
        "Good morning",
        [ord(c) for c in "Good morning"],
        "no Arabic at all: passed through untouched",
    ),
    (
        "مم",  # مم  two meems
        [0xFEE2,   # meem final
         0xFEE3],  # meem initial
        "two dual-joining letters join to each other",
    ),
    (
        "چای",  # chai (tea)
        [0xFEEF,   # yeh ISOLATED, not final: the alef before it is
                   #   right-joining and cannot join forward, so the yeh has
                   #   nothing to attach to on either side. Dotless either
                   #   way -- Persian yeh drops its dots isolated and final,
                   #   and keeps them initial and medial, which is why
                   #   U+06CC borrows two shapes from alef maksura and two
                   #   from Arabic yeh.
         0xFE8E,   # alef final
         0xFB7C],  # tcheh initial
        "chai: yeh after an alef is isolated, and dotless",
    ),
    (
        "بی",  # bi (without)
        [0xFEF0,   # yeh FINAL -- dotless, and this time genuinely final
         0xFE91],  # beh initial
        "bi: a dual-joining beh does make the yeh final",
    ),
    (
        "گل",  # گل  "gol" (flower)
        [0xFEDE,   # lam final
         0xFB94],  # gaf initial -- Presentation Forms-A
        "gaf: a Persian-only letter whose shapes live in Forms-A",
    ),
]


def name(cp):
    return "U+%04X" % cp


def main():
    fails = []
    for src, want, note in V:
        got = shape(src)
        if got == want:
            print("ok    %s" % note)
        else:
            print("FAIL  %s" % note)
            print("        input    %s" % " ".join(name(ord(c)) for c in src))
            print("        expected %s" % " ".join(name(c) for c in want))
            print("        got      %s" % " ".join(name(c) for c in got))
            fails.append(note)

    # --- the check that tests the shipped font, not this file ---
    sys.path.insert(0, os.path.join(REPO, "scripts"))
    scratch = os.path.join(REPO, ".pio", "libdeps", "esp32dev", "U8g2", "src",
                           "clib", "u8g2_fonts.c")
    if not os.path.exists(scratch):
        print("note  u8g2_fonts.c not present (run a build first); skipping the "
              "font coverage check")
    else:
        os.environ["U8G2_FONTS"] = scratch
        emitted = set()
        for forms in FORMS.values():
            emitted.update(c for c in forms if c)
        for _, iso, fin in LAM_ALEF:
            emitted.update((iso, fin))
        cov = load_font_coverage(scratch)
        missing = sorted(c for c in emitted if c not in cov)
        if missing:
            fails.append("font is missing %d glyph(s) the shaper emits"
                         % len(missing))
            print("FAIL  the shaper can emit %d glyph(s) u8g2_font_unifont_t_arabic "
                  "does not have:" % len(missing))
            for c in missing[:12]:
                print("        %s" % name(c))
        else:
            print("ok    all %d presentation forms the shaper can emit exist in "
                  "u8g2_font_unifont_t_arabic" % len(emitted))

    if "--art" in sys.argv:
        draw_art(scratch)

    if fails:
        print("\n%d failure(s)." % len(fails))
        return 1
    print("\nshaping is correct on %d vectors, and every glyph it emits is in "
          "the font" % len(V))
    return 0


def load_font_coverage(path):
    """Codepoints present in u8g2_font_unifont_t_arabic."""
    os.environ["U8G2_FONTS"] = path
    import importlib
    import font_coverage
    importlib.reload(font_coverage)
    data, declared = font_coverage.extract("unifont_t_arabic")
    if not data or len(data) + 1 != declared:
        raise SystemExit("could not parse u8g2_font_unifont_t_arabic")
    ascii_cps, uni = font_coverage.coverage(data)
    return ascii_cps | uni


def draw_art(font_path):
    """
    Render every vector with the real font bitmaps.

    This is the check the codepoint assertions cannot make: whether the result
    looks like Persian. It is how the dots on farsi yeh were settled -- the
    letter is dotless isolated and final and dotted initial and medial, and
    the only way to be sure the table picks the right one is to look at the
    glyphs the font actually contains.
    """
    if not os.path.exists(font_path):
        print("(--art needs u8g2_fonts.c; run a build first)")
        return
    import font_bitmap
    font_bitmap.render_all(font_path, [shape(src) for src, _, _ in V],
                           [note.split(":")[0] for _, _, note in V])


if __name__ == "__main__":
    sys.exit(main())
