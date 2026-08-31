#!/usr/bin/env python3
"""
Generates -- and checks -- the shaping tables in src/text_arabic.cpp.

Arabic script is cursive: every letter has up to four contextual shapes, and
which one you draw depends on whether its neighbours join to it. Unicode keeps
the shapes in separate blocks from the letters, so a renderer needs two tables:
a joining class per letter, and a letter -> four-shapes mapping.

Typing those out by hand means 140-odd hex constants, and a single transposed
digit is a wrong glyph in one context that nobody notices for a year. So they
are derived here instead:

  * The Presentation Forms-B block (U+FE80..FEF4) is laid out in letter order,
    two or four shapes each. Walking the base letters in that order reproduces
    the whole block, and the walk is checked against known anchors below.
  * The six Persian-specific letters live in Presentation Forms-A and are
    irregular, so they are listed -- six lines, each verifiable against the
    Unicode charts.
  * Joining classes come from ArabicShaping.txt, transcribed as ranges.

Run with no arguments to check the committed C++ still matches:
    python scripts/gen_arabic_tables.py
Run with --emit to print the tables for pasting into src/text_arabic.cpp:
    python scripts/gen_arabic_tables.py --emit
"""

import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CPP = os.path.join(REPO, "src", "text_arabic.cpp")

# --------------------------------------------------------------- joining ---
# From ArabicShaping.txt. U = non-joining, R = right-joining (joins only to the
# letter before it), D = dual-joining, C = join-causing (tatweel), T =
# transparent (a mark that does not interrupt a join).
#
# "Right-joining" is the confusing one: it means the letter accepts a join on
# its right-hand side, which in a right-to-left script is the side facing the
# *previous* letter. So R letters have isolated and final shapes only, and the
# letter after them always starts a new shape -- which is why "دنیا" breaks
# after the dal.
JOINING = [
    (0x0600, 0x0605, "U"), (0x0606, 0x0608, "U"), (0x0609, 0x060B, "U"),
    (0x060C, 0x060F, "U"),
    (0x0610, 0x061A, "T"),
    (0x061B, 0x061F, "U"),
    (0x0620, 0x0620, "D"),
    (0x0621, 0x0621, "U"),                        # hamza
    (0x0622, 0x0625, "R"),                        # alef variants, waw hamza
    (0x0626, 0x0626, "D"),                        # yeh hamza
    (0x0627, 0x0627, "R"),                        # alef
    (0x0628, 0x0628, "D"),                        # beh
    (0x0629, 0x0629, "R"),                        # teh marbuta
    (0x062A, 0x062E, "D"),                        # teh..khah
    (0x062F, 0x0632, "R"),                        # dal, thal, reh, zain
    (0x0633, 0x063F, "D"),                        # seen..ghain + extensions
    (0x0640, 0x0640, "C"),                        # tatweel
    (0x0641, 0x0647, "D"),                        # feh..heh
    (0x0648, 0x0649, "R"),                        # waw, alef maksura
    (0x064A, 0x064A, "D"),                        # yeh
    (0x064B, 0x065F, "T"),                        # harakat
    (0x0660, 0x066D, "U"),                        # Arabic-Indic digits, signs
    (0x066E, 0x066E, "D"), (0x066F, 0x066F, "R"),
    (0x0670, 0x0670, "T"),
    (0x0671, 0x0673, "R"), (0x0674, 0x0675, "U"), (0x0676, 0x0677, "R"),
    (0x0678, 0x0687, "D"),                        # includes peh, tcheh
    (0x0688, 0x0699, "R"),                        # includes zheh
    (0x069A, 0x06BF, "D"),
    (0x06C0, 0x06C0, "R"),
    (0x06C1, 0x06C2, "D"),
    (0x06C3, 0x06CB, "R"),
    (0x06CC, 0x06CC, "D"),                        # farsi yeh
    (0x06CD, 0x06CD, "R"),
    (0x06CE, 0x06CE, "D"),
    (0x06CF, 0x06CF, "R"),
    (0x06D0, 0x06D1, "D"),
    (0x06D2, 0x06D3, "R"),                        # yeh barree
    (0x06D4, 0x06D4, "U"),
    (0x06D5, 0x06D5, "R"),
    (0x06D6, 0x06DC, "T"),
    (0x06DD, 0x06DE, "U"),
    (0x06DF, 0x06E4, "T"),
    (0x06E5, 0x06E6, "U"),
    (0x06E7, 0x06E8, "T"),
    (0x06E9, 0x06E9, "U"),
    (0x06EA, 0x06ED, "T"),
    (0x06EE, 0x06EF, "R"),
    (0x06F0, 0x06F9, "U"),                        # Persian digits
    (0x06FA, 0x06FC, "D"),
    (0x06FD, 0x06FE, "U"),
    (0x06FF, 0x06FF, "D"),
]

# ------------------------------------------------------ presentation forms ---
# The Presentation Forms-B block starts at U+FE80 and runs in this order, with
# this many shapes each (2 = isolated+final, 4 = +initial+medial).
FEB_ORDER = [
    (0x0621, 1), (0x0622, 2), (0x0623, 2), (0x0624, 2), (0x0625, 2),
    (0x0626, 4), (0x0627, 2), (0x0628, 4), (0x0629, 2), (0x062A, 4),
    (0x062B, 4), (0x062C, 4), (0x062D, 4), (0x062E, 4), (0x062F, 2),
    (0x0630, 2), (0x0631, 2), (0x0632, 2), (0x0633, 4), (0x0634, 4),
    (0x0635, 4), (0x0636, 4), (0x0637, 4), (0x0638, 4), (0x0639, 4),
    (0x063A, 4), (0x0641, 4), (0x0642, 4), (0x0643, 4), (0x0644, 4),
    (0x0645, 4), (0x0646, 4), (0x0647, 4), (0x0648, 2), (0x0649, 2),
    (0x064A, 4),
]

# Anchors from the Unicode charts. If the walk above is right these must land
# exactly here, which is what makes the derivation trustworthy.
ANCHORS = {
    0x0621: 0xFE80, 0x0627: 0xFE8D, 0x0628: 0xFE8F, 0x062A: 0xFE95,
    0x062F: 0xFEA9, 0x0631: 0xFEAD, 0x0633: 0xFEB1, 0x0639: 0xFEC9,
    0x0641: 0xFED1, 0x0644: 0xFEDD, 0x0645: 0xFEE1, 0x0646: 0xFEE5,
    0x0647: 0xFEE9, 0x0648: 0xFEED, 0x0649: 0xFEEF, 0x064A: 0xFEF1,
}

# Persian letters, whose shapes are in Presentation Forms-A and are not
# contiguous. (base, isolated, final, initial, medial); 0 = no such shape.
FEA_PERSIAN = [
    (0x067E, 0xFB56, 0xFB57, 0xFB58, 0xFB59),  # peh
    (0x0686, 0xFB7A, 0xFB7B, 0xFB7C, 0xFB7D),  # tcheh
    (0x0698, 0xFB8A, 0xFB8B, 0x0000, 0x0000),  # zheh (right-joining)
    (0x06A9, 0xFB8E, 0xFB8F, 0xFB90, 0xFB91),  # keheh
    (0x06AF, 0xFB92, 0xFB93, 0xFB94, 0xFB95),  # gaf
]

# Farsi yeh is the exception that matters.
#
# Its own shapes are U+FBFC..FBFF, and *none of the three u8g2 Arabic fonts
# ships them* -- verified by parsing u8g2_fonts.c. Rendering it unshaped would
# leave the commonest letter in Persian permanently disconnected, so it borrows
# shapes that do exist:
#
#   isolated/final  <- alef maksura U+FEEF/FEF0, which is a dotless yeh: this
#                      is not a compromise, it is the correct Persian shape.
#   initial/medial  <- Arabic yeh U+FEF3/FEF4, whose joined shapes carry no
#                      dots either, so they are identical to Persian.
FARSI_YEH = (0x06CC, 0xFEEF, 0xFEF0, 0xFEF3, 0xFEF4)

# Lam followed by an alef must combine; the pair is never drawn apart.
LAM_ALEF = [
    (0x0622, 0xFEF5, 0xFEF6),
    (0x0623, 0xFEF7, 0xFEF8),
    (0x0625, 0xFEF9, 0xFEFA),
    (0x0627, 0xFEFB, 0xFEFC),
]


def build_forms():
    """[(base, iso, fin, ini, med)] for every letter with shapes."""
    forms = {}
    cp = 0xFE80
    for base, count in FEB_ORDER:
        if base in ANCHORS and ANCHORS[base] != cp:
            raise SystemExit("Presentation Forms-B walk is wrong: U+%04X landed "
                             "at U+%04X, charts say U+%04X"
                             % (base, cp, ANCHORS[base]))
        if count == 1:
            forms[base] = (cp, 0, 0, 0)
        elif count == 2:
            forms[base] = (cp, cp + 1, 0, 0)
        else:
            forms[base] = (cp, cp + 1, cp + 2, cp + 3)
        cp += count
    if cp - 1 != 0xFEF4:
        raise SystemExit("Presentation Forms-B walk ended at U+%04X, expected "
                         "U+FEF4" % (cp - 1))
    for base, iso, fin, ini, med in FEA_PERSIAN:
        forms[base] = (iso, fin, ini, med)
    forms[FARSI_YEH[0]] = FARSI_YEH[1:]
    return sorted(forms.items())


def build_joining():
    """A 256-entry class per codepoint in U+0600..U+06FF."""
    table = ["U"] * 256
    for lo, hi, cls in JOINING:
        for c in range(lo, hi + 1):
            table[c - 0x0600] = cls
    return table


CLASS_ENUM = {"U": "JOIN_NONE", "R": "JOIN_RIGHT", "D": "JOIN_DUAL",
              "C": "JOIN_CAUSING", "T": "JOIN_TRANSPARENT"}


def emit():
    forms = build_forms()
    joining = build_joining()
    out = []
    out.append("// GENERATED by scripts/gen_arabic_tables.py -- do not edit by hand.")
    out.append("// Run that script to check this still matches Unicode.")
    out.append("")
    out.append("/// Joining class of every codepoint in the Arabic block, indexed")
    out.append("/// by (cp - 0x0600). See gen_arabic_tables.py for the source data.")
    out.append("static const uint8_t JOINING_CLASS[256] = {")
    for row in range(0, 256, 8):
        cells = ", ".join("%-16s" % CLASS_ENUM[joining[row + i]] for i in range(8))
        out.append("    %s  // U+%04X" % (cells.rstrip() + ",", 0x0600 + row))
    out.append("};")
    out.append("")
    out.append("/// base letter -> {isolated, final, initial, medial}; 0 where the")
    out.append("/// letter has no such shape. Sorted, so lookup can bisect.")
    out.append("static const ArabicForms FORMS[] = {")
    for base, (iso, fin, ini, med) in forms:
        out.append("    {0x%04X, {0x%04X, 0x%04X, 0x%04X, 0x%04X}},"
                   % (base, iso, fin, ini, med))
    out.append("};")
    out.append("")
    out.append("/// lam + alef, which must always combine: {alef, isolated, final}.")
    out.append("static const uint16_t LAM_ALEF[][3] = {")
    for alef, iso, fin in LAM_ALEF:
        out.append("    {0x%04X, 0x%04X, 0x%04X}," % (alef, iso, fin))
    out.append("};")
    return "\n".join(out)


def parse_cpp():
    """The tables as they exist in the committed C++."""
    if not os.path.exists(CPP):
        return None, None, None
    src = open(CPP, encoding="utf-8").read()
    m = re.search(r"JOINING_CLASS\[256\]\s*=\s*\{(.*?)\};", src, re.S)
    joining = None
    if m:
        names = re.findall(r"JOIN_[A-Z]+", m.group(1))
        inv = {v: k for k, v in CLASS_ENUM.items()}
        joining = [inv[n] for n in names]
    m = re.search(r"FORMS\[\]\s*=\s*\{(.*?)\};", src, re.S)
    forms = None
    if m:
        forms = []
        for row in re.finditer(
                r"\{0x([0-9A-Fa-f]{4}),\s*\{0x([0-9A-Fa-f]{4}),\s*0x([0-9A-Fa-f]{4}),"
                r"\s*0x([0-9A-Fa-f]{4}),\s*0x([0-9A-Fa-f]{4})\}\}", m.group(1)):
            g = [int(x, 16) for x in row.groups()]
            forms.append((g[0], tuple(g[1:])))
    m = re.search(r"LAM_ALEF\[\]\[3\]\s*=\s*\{(.*?)\};", src, re.S)
    lam = None
    if m:
        lam = [tuple(int(x, 16) for x in row)
               for row in re.findall(r"\{0x([0-9A-Fa-f]{4}),\s*0x([0-9A-Fa-f]{4}),"
                                     r"\s*0x([0-9A-Fa-f]{4})\}", m.group(1))]
    return joining, forms, lam


def main():
    if "--emit" in sys.argv:
        print(emit())
        return 0

    want_join = build_joining()
    want_forms = build_forms()
    got_join, got_forms, got_lam = parse_cpp()
    problems = []

    if got_join is None:
        problems.append("JOINING_CLASS[256] not found in src/text_arabic.cpp")
    elif len(got_join) != 256:
        problems.append("JOINING_CLASS has %d entries, expected 256" % len(got_join))
    elif got_join != want_join:
        diff = [i for i in range(256) if got_join[i] != want_join[i]]
        problems.append("joining classes differ at %d codepoint(s): %s"
                        % (len(diff), ", ".join(
                            "U+%04X (C++ says %s, Unicode says %s)"
                            % (0x600 + i, got_join[i], want_join[i])
                            for i in diff[:6])))
    else:
        print("ok    joining classes match Unicode for all 256 codepoints")

    if got_forms is None:
        problems.append("FORMS[] not found in src/text_arabic.cpp")
    elif got_forms != want_forms:
        gd, wd = dict(got_forms), dict(want_forms)
        for base in sorted(set(gd) | set(wd)):
            if gd.get(base) != wd.get(base):
                problems.append("U+%04X shapes: C++ %s, derived %s"
                                % (base, gd.get(base), wd.get(base)))
        problems.append("%d letters have shape tables" % len(got_forms))
    else:
        print("ok    presentation forms match for all %d letters" % len(want_forms))
        if not any(base == 0x06CC for base, _ in got_forms):
            problems.append("farsi yeh U+06CC has no shapes, so the commonest "
                            "letter in Persian will not join")
        else:
            print("ok    farsi yeh U+06CC borrows the dotless shapes")

    if got_lam != LAM_ALEF:
        problems.append("LAM_ALEF differs: C++ %s, expected %s" % (got_lam, LAM_ALEF))
    else:
        print("ok    all %d lam-alef ligatures present" % len(LAM_ALEF))

    if problems:
        print("\n%d problem(s):\n" % len(problems))
        for p in problems:
            print("  " + p)
        print("\nRegenerate with: python scripts/gen_arabic_tables.py --emit")
        return 1
    print("\nthe shaping tables in src/text_arabic.cpp match Unicode")
    return 0


if __name__ == "__main__":
    sys.exit(main())
