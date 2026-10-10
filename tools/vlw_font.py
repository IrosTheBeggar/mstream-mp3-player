#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Rasterise TrueType fonts into VLW smooth fonts (M5GFX loadFont) as C++ arrays.

The UI draws its text in anti-aliased VLW fonts from flash (docs/UI-SPIKE.md,
"Fonts": DejaVu Sans looked best by a clear margin). A VLW font here covers
what names in Latin, Greek and Cyrillic scripts need (RANGES; docs/I18N.md,
phase 1): ASCII, Latin-1, Latin Extended-A, Romanian's and Pinyin's letters,
Greek, Cyrillic and the punctuation tag editors use. A character outside it
is folded by lib/core TextFit (to ASCII, or '?').

Every glyph is fitted to the font's line: M5GFX's VLWfont::loadFont makes the
line height the tallest glyph's ascent plus the deepest one's descent (of
ASCII and everything above U+00FF), so one tall glyph would move every row
of the UI. A glyph that reaches above the font's ascent or below its descent
(a capital with two marks, Ǘ; a breve on a capital, Ă) loses rows from its
marks and the gaps around them, never from its letter (fit_to_line(),
squeeze()). The generated file names each font's fitted glyphs.

VLW (Processing's format, as M5GFX reads it; all fields big-endian int32):
  header:   glyph count, version (11), font size, 0, ascent, descent
  metrics:  per glyph, in code point order: code point, height, width,
            x advance, dY (top above the baseline), dX (left offset), 0
  bitmaps:  per glyph, width x height bytes of 8-bit coverage

Usage (the default makes src/ui/VlwFonts.cpp: DejaVu Sans 16 and 13 px and
DejaVu Sans Bold 16 and 22 px; their licence, the Bitstream Vera fonts licence
for Bitstream's glyphs and the Arev fonts licence for Arev's, plus
public-domain DejaVu changes, allows embedding: see LICENSES/DejaVu-Fonts.txt,
and keep the notice in the generated file). The checked-in file comes from
DejaVu 2.37
(https://github.com/dejavu-fonts/dejavu-fonts/releases/tag/version_2_37) and
Pillow 12.2 (FreeType 2.14; another FreeType can hint a pixel differently);
the generated header names each font's version, read from the TTF:

    python tools/vlw_font.py
    python tools/vlw_font.py --check     # exit 1 if src/ui/VlwFonts.cpp differs
    python tools/vlw_font.py --fonts "C:/Windows/Fonts/DejaVuSans.ttf:Sans:16,13;C:/Windows/Fonts/DejaVuSans-Bold.ttf:SansBold:16,22"

Needs Pillow.
"""
import argparse
import os
import struct
import sys
import unicodedata

from PIL import Image, ImageDraw, ImageFont

# The characters each font gets, those the TTF has (its cmap). docs/I18N.md
# phase 1 has each block's cost and the ones left out (Latin Extended-B's
# other letters and Extended Additional, Vietnamese's: TextFold folds them
# to their base letters; Cyrillic Supplement, Armenian, Georgian).
RANGES = [
    (0x0020, 0x007E),  # ASCII
    (0x00A0, 0x00FF),  # Latin-1
    (0x0100, 0x017F),  # Latin Extended-A: Polish, Czech, Turkish, the Baltic languages...
    (0x01CD, 0x01DC),  # Pinyin's vowels with a caron, and ü's four tones
    (0x0218, 0x021B),  # Romanian's Ș ș Ț ț (with a comma below)
    (0x0370, 0x03E1),  # Greek: its letters, tonos and dialytika, symbols and archaic letters,
    (0x03F0, 0x03FF),  # not the Coptic letters (U+03E2-03EF)
    (0x0400, 0x0482),  # Cyrillic: every letter, not Church Slavonic's
    (0x048A, 0x04FF),  # combining marks (U+0483-0489: TextFit drops them)
    (0x2010, 0x2015), (0x2018, 0x201F), (0x2020, 0x2022), (0x2026, 0x2026),  # punctuation
    (0x2030, 0x2030), (0x2032, 0x2033), (0x2039, 0x203A),
    (0x20AC, 0x20AC), (0x2122, 0x2122),  # € ™
]


def codepoints():
    cps = set()
    for a, b in RANGES:
        cps.update(range(a, b + 1))
    return sorted(cps)


def table(data, want):
    """A TrueType table's offset, or None."""
    for i in range(struct.unpack(">H", data[4:6])[0]):
        tag, _, off, _ = struct.unpack(">4sIII", data[12 + 16 * i:28 + 16 * i])
        if tag == want:
            return off
    return None


def cmap(path):
    """The BMP code points the font maps to a glyph (its Windows Unicode cmap, format 4).
    Pillow draws a code point the font lacks as .notdef's box, so it can't tell."""
    with open(path, "rb") as f:
        data = f.read()
    off = table(data, b"cmap")
    cps = set()
    for i in range(struct.unpack(">H", data[off + 2:off + 4])[0]):
        pid, eid, sub = struct.unpack(">HHI", data[off + 4 + 8 * i:off + 12 + 8 * i])
        s = off + sub
        if pid != 3 or eid != 1 or struct.unpack(">H", data[s:s + 2])[0] != 4:
            continue
        n = struct.unpack(">H", data[s + 6:s + 8])[0] // 2
        ends = struct.unpack(f">{n}H", data[s + 14:s + 14 + 2 * n])
        starts = struct.unpack(f">{n}H", data[s + 16 + 2 * n:s + 16 + 4 * n])
        deltas = struct.unpack(f">{n}h", data[s + 16 + 4 * n:s + 16 + 6 * n])
        ranges_at = s + 16 + 6 * n
        offsets = struct.unpack(f">{n}H", data[ranges_at:ranges_at + 2 * n])
        for k in range(n):
            for cp in range(starts[k], min(ends[k], 0xFFFE) + 1):
                if offsets[k] == 0:
                    g = (cp + deltas[k]) & 0xFFFF
                else:
                    at = ranges_at + 2 * k + offsets[k] + 2 * (cp - starts[k])
                    g = struct.unpack(">H", data[at:at + 2])[0]
                    g = (g + deltas[k]) & 0xFFFF if g else 0
                if g:
                    cps.add(cp)
    return cps


def glyph(font, cp):
    """(height, width, x advance, dY, dX, bitmap) of a code point, as FreeType draws it."""
    ch = chr(cp)
    advance = int(round(font.getlength(ch)))
    if cp == 0x20 or cp == 0xA0:
        return 0, 0, advance, 0, 0, b""
    x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")
    w, h = max(0, x1 - x0), max(0, y1 - y0)
    if w == 0 or h == 0:
        return 0, 0, advance, 0, 0, b""
    img = Image.new("L", (w, h), 0)
    ImageDraw.Draw(img).text((-x0, -y0), ch, font=font, fill=255, anchor="ls")
    return h, w, advance, -y0, x0, img.tobytes()


def base_extent(font, cp):
    """How far the letter under a glyph's marks reaches above and below the
    baseline: its canonical decomposition's first character's glyph (Ǘ: U,
    ΐ: ι, ΅: ¨), else H's height and the baseline."""
    base = unicodedata.normalize("NFD", chr(cp))[0]
    if base == chr(cp) or unicodedata.combining(base):
        base = "H"
    h, _, _, dy, _, _ = glyph(font, ord(base))
    return (dy, h - dy) if base != "H" else (dy, 0)


def squeeze(rows, n, zone, marks):
    """`rows` with `n` fewer among its first `zone` (from the glyph's edge to
    its letter: its `marks` marks), each step the first of these:
      1. a blank row at the edge;
      2. a second blank row between the marks and the letter;
      3. a row of a mark 3 rows tall or more: in a run of rows with ink
         taller than 2 rows a mark (the glyph's marks spread over its runs:
         Ǘ's acute touching its diaeresis is one run of two marks), the
         tallest run's two rows most alike merge, the innermost of equals
         (Ǖ's diaeresis before its macron);
      4. a blank row between two marks;
      5. the blank row left between the marks and the letter;
      6. any run's two rows most alike merge.
    So the marks stay apart from the letter, and each keeps two rows (an
    acute isn't a grave), as long as there is room. A merge keeps the darker
    pixel of the two rows, so no stroke is lost."""
    rows = [bytearray(r) for r in rows]

    def blank(i):
        return not any(rows[i])

    def alike(i):  # rows i and i + 1: their shared ink over their joint ink
        both = sum(max(a, b) for a, b in zip(rows[i], rows[i + 1]))
        return sum(min(a, b) for a, b in zip(rows[i], rows[i + 1])) / both

    def runs():  # the runs of rows with ink, [start, end)
        out, i = [], 0
        while i < zone:
            if blank(i):
                i += 1
                continue
            j = i
            while j < zone and not blank(j):
                j += 1
            out.append((i, j))
            i = j
        return out

    def merge(tall):  # in the runs at least `tall` rows: the tallest, the most alike, the innermost
        best = None
        for a, b in runs():
            if b - a < max(2, tall):
                continue
            for i in range(a, b - 1):
                key = (b - a if tall else 0, alike(i), i)
                if best is None or key > best[0]:
                    best = (key, i)
        if best is None:
            return None
        i = best[1]
        rows[i] = bytearray(max(a, b) for a, b in zip(rows[i], rows[i + 1]))
        return i + 1

    for _ in range(n):
        gap = zone  # the blank rows next to the letter: [gap, zone)
        while gap > 0 and blank(gap - 1):
            gap -= 1
        per_run = -(-marks // max(1, len(runs())))
        steps = (
            lambda: 0 if gap > 0 and blank(0) else None,
            lambda: gap if zone - gap >= 2 else None,
            lambda: merge(2 * per_run + 1),
            lambda: next((i for i in range(gap) if blank(i)), None),
            lambda: gap if zone - gap == 1 else None,
            lambda: merge(0),
        )
        drop = next((d for d in (step() for step in steps) if d is not None), None)
        if drop is None:
            raise ValueError(f"no row left to take among {zone}")
        del rows[drop]
        zone -= 1
    return rows


def fit_to_line(font, cp, g, ascent, descent):
    """The glyph `g` within the font's ascent and descent: rows taken from above
    (below) its letter. Returns the glyph and whether it changed."""
    h, w, adv, dy, dx, bits = g
    up, down = dy - ascent, (h - dy) - descent
    if h == 0 or (up <= 0 and down <= 0):
        return g, False
    top, bottom = base_extent(font, cp)
    marks = max(1, sum(1 for c in unicodedata.normalize("NFD", chr(cp)) if unicodedata.combining(c)))
    rows = [bits[r * w:(r + 1) * w] for r in range(h)]
    try:
        if up > 0:
            rows = squeeze(rows, up, dy - top, marks)
            dy -= up
        if down > 0:
            rows = squeeze(rows[::-1], down, len(rows) - dy - bottom, marks)[::-1]
    except ValueError as e:
        raise SystemExit(f"U+{cp:04X} doesn't fit the line ({e}): leave it out of RANGES") from e
    return (len(rows), w, adv, dy, dx, b"".join(bytes(r) for r in rows)), True


def line_height(glyphs, ascent, descent):
    """M5GFX 0.2.30's VLWfont::loadFont: the ascent and descent grown by the
    glyphs above U+00FF and the ASCII ones (lgfx_fonts.inl)."""
    up, down = ascent, descent
    for cp, (h, _, _, dy, _, _) in glyphs:
        if cp > 0xFF or (0x20 < cp < 0xA0 and cp != 0x7F):
            up, down = max(up, dy), max(down, h - dy)
    return up + down


def vlw(font_path, size, mapped):
    """The VLW font, its glyph count and the code points fitted to the line."""
    font = ImageFont.truetype(font_path, size)
    ascent, descent = font.getmetrics()
    cps = [cp for cp in codepoints()
           if cp in mapped and (cp in (0x20, 0xA0) or font.getmask(chr(cp)).getbbox())]
    glyphs, fitted = [], []
    for cp in cps:
        g, changed = fit_to_line(font, cp, glyph(font, cp), ascent, descent)
        glyphs.append((cp, g))
        if changed:
            fitted.append(cp)
    assert line_height(glyphs, ascent, descent) == ascent + descent
    header = struct.pack(">6i", len(cps), 11, size, 0, ascent, descent)
    metrics, bitmaps = [], []
    for cp, (h, w, adv, dy, dx, bits) in glyphs:
        metrics.append(struct.pack(">7i", cp, h, w, adv, dy, dx, 0))
        bitmaps.append(bits)
    return header + b"".join(metrics) + b"".join(bitmaps), len(cps), fitted


def c_array(name, data):
    lines = []
    for i in range(0, len(data), 20):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 20]) + ",")
    return (f"const uint8_t {name}[{len(data)}] = {{\n" + "\n".join(lines) + "\n};\n"
            f"const size_t {name}Size = {len(data)};\n")


def font_version(path):
    """The TTF's version string (its name table's ID 5, e.g. "Version 2.37"), or "?"."""
    with open(path, "rb") as f:
        data = f.read()
    off = table(data, b"name")
    if off is None:
        return "?"
    count, strings = struct.unpack(">HH", data[off + 2:off + 6])
    for j in range(count):
        pid, _, _, nid, length, at = struct.unpack(">6H", data[off + 6 + 12 * j:off + 18 + 12 * j])
        if nid == 5 and pid == 3:  # Windows platform: UTF-16BE
            return data[off + strings + at:off + strings + at + length].decode("utf-16-be")
    return "?"


DEFAULT_FONTS = ("C:/Windows/Fonts/DejaVuSans.ttf:Sans:16,13;"
                 "C:/Windows/Fonts/DejaVuSans-Bold.ttf:SansBold:16,22")


NOTICE = """// VLW smooth fonts (M5GFX loadFont) for the UI: ASCII, Latin-1, Latin
// Extended-A, Romanian's and Pinyin's letters, Greek, Cyrillic, General
// Punctuation's common marks. A glyph taller than the line has rows taken
// from between its marks and its letter (tools/vlw_font.py, fit_to_line()).
// Glyphs rasterised from DejaVu Sans: DejaVu changes are in the public domain; the
// Bitstream Vera glyphs they build on are (c) 2003 Bitstream, Inc., under the
// Bitstream Vera Fonts licence, and the glyphs imported from Arev are (c) 2006
// Tavmjong Bah, under the Arev Fonts licence (embedding permitted; their notices
// must accompany redistributed copies: LICENSES/DejaVu-Fonts.txt, and
// https://dejavu-fonts.github.io/License.html).
"""
NL = chr(10)


def generate(fonts):
    parts, sources = [], []
    for spec in fonts.split(";"):
        ttf, name, sizes = spec.rsplit(":", 2)
        if not os.path.exists(ttf):
            sys.exit(f"no font at {ttf}")
        sources.append(f"{os.path.basename(ttf)} ({font_version(ttf)})")
        mapped = cmap(ttf)
        for size in (int(s) for s in sizes.split(",")):
            data, n, fitted = vlw(ttf, size, mapped)
            cname = f"kVlw{name}{size}"
            note = f"// {os.path.basename(ttf)} {size} px: {n} glyphs, {len(data)} bytes" + NL
            if fitted:
                note += "// Fitted to the line: " + " ".join(f"U+{cp:04X}" for cp in fitted) + NL
            parts.append(note + c_array(cname, data))
            print(f"{cname}: {n} glyphs, {len(data)} bytes, {len(fitted)} fitted to the line")
    # The data is the fonts' (their licence); the few lines of code around it are ours.
    return ("// SPDX-License-Identifier: Bitstream-Vera" + NL +
            "// Generated by tools/vlw_font.py from " + ", ".join(sources) + " - do not edit." + NL + NOTICE +
            '#include "ui/VlwFonts.h"' + NL + NL + NL.join(parts))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fonts", default=DEFAULT_FONTS,
                    help="semicolon-separated ttf:name:sizes, e.g. DejaVuSans.ttf:Sans:16,13 -> kVlwSans16, kVlwSans13")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "src", "ui", "VlwFonts.cpp"))
    ap.add_argument("--check", action="store_true", help="fail if --out isn't what these fonts make")
    args = ap.parse_args()
    src = generate(args.fonts)
    if args.check:
        try:
            with open(args.out, encoding="utf-8") as f:  # a CRLF checkout reads the same
                current = f.read()
        except OSError:
            current = None
        if current != src:
            print(f"vlw_font: out of date: {os.path.normpath(args.out)}", file=sys.stderr)
            return 1
        print("vlw_font: up to date")
        return 0
    with open(args.out, "w", encoding="utf-8", newline=NL) as f:
        f.write(src)
    print(f"wrote {os.path.normpath(args.out)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
