#!/usr/bin/env python3
"""Rasterise TrueType fonts into VLW smooth fonts (M5GFX loadFont) as C++ arrays.

The UI draws its text in anti-aliased VLW fonts from flash (docs/UI-SPIKE.md,
"Fonts": DejaVu Sans looked best by a clear margin and has every character the
library's names use). A VLW font here covers only what a Western library
needs: ASCII, Latin-1, Latin Extended-A's common letters and General
Punctuation; a character outside it is folded to ASCII (lib/core TextFit).

VLW (Processing's format, as M5GFX reads it; all fields big-endian int32):
  header:   glyph count, version (11), font size, 0, ascent, descent
  metrics:  per glyph, in code point order: code point, height, width,
            x advance, dY (top above the baseline), dX (left offset), 0
  bitmaps:  per glyph, width x height bytes of 8-bit coverage

Usage (the default makes src/ui/VlwFonts.cpp: DejaVu Sans 16 and 13 px and
DejaVu Sans Bold 16 and 22 px; their licence, the Bitstream Vera fonts licence
plus public-domain DejaVu changes, allows embedding: see
LICENSES/DejaVu-Fonts.txt, and keep the notice in the generated file):

    python tools/vlw_font.py
    python tools/vlw_font.py --fonts "C:/Windows/Fonts/DejaVuSans.ttf:Sans:16,13;C:/Windows/Fonts/DejaVuSans-Bold.ttf:SansBold:16,22"

Needs Pillow.
"""
import argparse
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

# ASCII, Latin-1, the Latin Extended-A letters that turn up in names, and the
# punctuation tag editors use.
RANGES = [
    (0x20, 0x7E),
    (0xA0, 0xFF),
    (0x0131, 0x0131), (0x0141, 0x0142), (0x0152, 0x0153), (0x0160, 0x0161),
    (0x0178, 0x0178), (0x017D, 0x017E), (0x0150, 0x0151), (0x0170, 0x0171),
    (0x0106, 0x0107), (0x010C, 0x010D), (0x011A, 0x011B), (0x0158, 0x0159),
    (0x015A, 0x015B), (0x0179, 0x017C), (0x0104, 0x0105), (0x0118, 0x0119),
    (0x0143, 0x0144), (0x0147, 0x0148), (0x0164, 0x0165), (0x016E, 0x016F),
    (0x2010, 0x2015), (0x2018, 0x201F), (0x2020, 0x2022), (0x2026, 0x2026),
    (0x2030, 0x2030), (0x2032, 0x2033), (0x2039, 0x203A), (0x20AC, 0x20AC),
    (0x2122, 0x2122),
]


def codepoints():
    cps = set()
    for a, b in RANGES:
        cps.update(range(a, b + 1))
    return sorted(cps)


def glyph(font, cp):
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


def vlw(font_path, size):
    font = ImageFont.truetype(font_path, size)
    ascent, descent = font.getmetrics()
    cps = [cp for cp in codepoints() if font.getmask(chr(cp)).getbbox() or cp in (0x20, 0xA0)]
    header = struct.pack(">6i", len(cps), 11, size, 0, ascent, descent)
    metrics, bitmaps = [], []
    for cp in cps:
        h, w, adv, dy, dx, bits = glyph(font, cp)
        metrics.append(struct.pack(">7i", cp, h, w, adv, dy, dx, 0))
        bitmaps.append(bits)
    return header + b"".join(metrics) + b"".join(bitmaps), len(cps)


def c_array(name, data):
    lines = []
    for i in range(0, len(data), 20):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 20]) + ",")
    return (f"const uint8_t {name}[{len(data)}] = {{\n" + "\n".join(lines) + "\n};\n"
            f"const size_t {name}Size = {len(data)};\n")


DEFAULT_FONTS = ("C:/Windows/Fonts/DejaVuSans.ttf:Sans:16,13;"
                 "C:/Windows/Fonts/DejaVuSans-Bold.ttf:SansBold:16,22")


NOTICE = """// VLW smooth fonts (M5GFX loadFont) for the UI: ASCII, Latin-1, common Latin
// Extended-A letters, General Punctuation.
// Glyphs rasterised from DejaVu Sans: DejaVu changes are in the public domain; the
// Bitstream Vera glyphs they build on are (c) 2003 Bitstream, Inc., under the
// Bitstream Vera Fonts licence (embedding permitted; its notice must accompany
// redistributed copies: LICENSES/DejaVu-Fonts.txt, and
// https://dejavu-fonts.github.io/License.html).
"""
NL = chr(10)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fonts", default=DEFAULT_FONTS,
                    help="semicolon-separated ttf:name:sizes, e.g. DejaVuSans.ttf:Sans:16,13 -> kVlwSans16, kVlwSans13")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "src", "ui", "VlwFonts.cpp"))
    args = ap.parse_args()
    parts, sources = [], []
    for spec in args.fonts.split(";"):
        ttf, name, sizes = spec.rsplit(":", 2)
        if not os.path.exists(ttf):
            sys.exit(f"no font at {ttf}")
        sources.append(os.path.basename(ttf))
        for size in (int(s) for s in sizes.split(",")):
            data, n = vlw(ttf, size)
            cname = f"kVlw{name}{size}"
            parts.append(f"// {os.path.basename(ttf)} {size} px: {n} glyphs, {len(data)} bytes" + NL + c_array(cname, data))
            print(f"{cname}: {n} glyphs, {len(data)} bytes")
    src = ("// Generated by tools/vlw_font.py from " + ", ".join(sources) + " - do not edit." + NL + NOTICE +
           '#include "ui/VlwFonts.h"' + NL + NL + NL.join(parts))
    with open(args.out, "w", encoding="utf-8", newline=NL) as f:
        f.write(src)
    print(f"wrote {os.path.normpath(args.out)}")


if __name__ == "__main__":
    main()
