#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""The UI's icons as 1-bit bitmaps in flash (src/ui/IconData.cpp).

Each icon is drawn here at its real size with Pillow on a 1-bit canvas (no
anti-aliasing: every pixel is on or off, as the LCD shows it), or from ASCII
art, and written as M5GFX drawBitmap() data: rows of whole bytes, the most
significant bit first. The UI draws them in any colour (the section's
accent, the Bluetooth state's colour).

    python tools/ui_icons.py            # writes src/ui/IconData.cpp
    python tools/ui_icons.py --preview  # also prints each icon as ASCII

The Library icon is a record half out of its sleeve, deliberately nothing
like Now Playing's EQ bars (the design review: at 1x the old book stack and
the bars read alike). Needs Pillow.
"""
import argparse
import os

from PIL import Image, ImageDraw


def canvas(w, h):
    img = Image.new("1", (w, h), 0)
    return img, ImageDraw.Draw(img)


def from_art(rows):
    h, w = len(rows), len(rows[0])
    img, _ = canvas(w, h)
    for y, row in enumerate(rows):
        assert len(row) == w, (row, w)
        for x, c in enumerate(row):
            if c == "#":
                img.putpixel((x, y), 1)
    return img


def library():
    # A record sleeve (left) with the disc pulled half out to the right.
    img, d = canvas(26, 18)
    d.ellipse((8, 0, 25, 17), outline=1, width=2)      # the disc
    d.ellipse((15, 7, 19, 11), fill=1)                  # its label
    d.rectangle((0, 0, 13, 17), fill=0)                 # the sleeve hides its left half
    d.rectangle((0, 0, 13, 17), outline=1, width=2)
    d.line((4, 12, 9, 12), fill=1)                      # a sticker on the sleeve
    return img


def queue():
    # A play triangle and three list lines.
    img, d = canvas(24, 18)
    d.polygon([(0, 0), (0, 8), (6, 4)], fill=1)
    d.rectangle((10, 3, 23, 4), fill=1)
    d.rectangle((0, 9, 23, 10), fill=1)
    d.rectangle((0, 15, 23, 16), fill=1)
    return img


CRAB = [
    "..##..............##..",
    ".#..#............#..#.",
    ".#.##............##.#.",
    "..##..............##..",
    "...#...#......#...#...",
    "...##..#......#..##...",
    "....##############....",
    "...################...",
    "..######.######.####..",
    "..######.######.####..",
    "...################...",
    "..#.#.##########.#.#..",
    ".#..#..#......#..#..#.",
    "#...#...........#....#",
]


def headphones():
    img, d = canvas(22, 18)
    d.arc((1, 0, 20, 21), 180, 360, fill=1, width=2)    # the band
    d.rounded_rectangle((1, 9, 6, 17), 2, fill=1)       # the cups
    d.rounded_rectangle((15, 9, 20, 17), 2, fill=1)
    return img


def speaker():
    img, d = canvas(22, 18)
    d.rectangle((1, 6, 5, 11), fill=1)
    d.polygon([(5, 6), (11, 1), (11, 16), (5, 11)], fill=1)
    d.arc((8, 4, 16, 13), 300, 60, fill=1, width=2)
    d.arc((9, 0, 21, 17), 300, 60, fill=1, width=2)
    return img


def note():
    img, d = canvas(16, 18)
    d.ellipse((1, 11, 7, 17), fill=1)
    d.rectangle((6, 1, 7, 14), fill=1)
    d.polygon([(7, 1), (14, 4), (14, 7), (7, 5)], fill=1)
    return img


def play():
    img, d = canvas(16, 18)
    d.polygon([(2, 1), (2, 16), (15, 8)], fill=1)
    return img


def pause():
    img, d = canvas(16, 18)
    d.rectangle((2, 1, 6, 16), fill=1)
    d.rectangle((9, 1, 13, 16), fill=1)
    return img


def next_track():
    img, d = canvas(18, 16)
    d.polygon([(1, 1), (1, 14), (12, 7)], fill=1)
    d.rectangle((13, 1, 15, 14), fill=1)
    return img


def prev_track():
    return next_track().transpose(Image.FLIP_LEFT_RIGHT)


def more():
    img, d = canvas(18, 4)
    for x in (1, 7, 13):
        d.rectangle((x, 0, x + 3, 3), fill=1)
    return img


def chevron_right():
    return from_art([
        "##....",
        "###...",
        ".###..",
        "..###.",
        "...###",
        "...###",
        "..###.",
        ".###..",
        "###...",
        "##....",
    ])


def chevron_left():
    return chevron_right().transpose(Image.FLIP_LEFT_RIGHT)


def check():
    return from_art([
        "..........##",
        ".........###",
        "........###.",
        "##.....###..",
        "###...###...",
        ".###.###....",
        "..#####.....",
        "...###......",
    ])


def cross():
    img, d = canvas(12, 12)
    d.line((0, 0, 11, 11), fill=1, width=2)
    d.line((0, 11, 11, 0), fill=1, width=2)
    return img


def plus():
    img, d = canvas(12, 12)
    d.rectangle((5, 0, 6, 11), fill=1)
    d.rectangle((0, 5, 11, 6), fill=1)
    return img


def minus():
    img, d = canvas(12, 12)
    d.rectangle((0, 5, 11, 6), fill=1)
    return img


def folder():
    # The Folders explorer's folder (drawn amber).
    img, d = canvas(22, 18)
    d.polygon([(0, 2), (7, 2), (9, 4), (21, 4), (21, 17), (0, 17)], fill=1)
    d.line((1, 7, 20, 7), fill=0)                       # the flap's edge
    return img


def audio_file():
    # A page with a folded corner and a note: an audio file.
    img, d = canvas(16, 20)
    d.polygon([(1, 0), (10, 0), (15, 5), (15, 19), (1, 19)], outline=1)
    d.line((10, 0, 10, 5), fill=1)
    d.line((10, 5, 15, 5), fill=1)
    d.ellipse((4, 13, 8, 16), fill=1)                  # the note
    d.rectangle((7, 7, 8, 15), fill=1)
    d.line((8, 7, 11, 9), fill=1)
    return img


def trash():
    # Remove (the Queue's edit bar).
    img, d = canvas(16, 18)
    d.rectangle((5, 0, 10, 1), fill=1)                  # the handle
    d.rectangle((0, 2, 15, 3), fill=1)                  # the lid
    d.polygon([(2, 5), (13, 5), (12, 17), (3, 17)], outline=1)
    d.line((2, 5, 3, 17), fill=1)
    d.line((13, 5, 12, 17), fill=1)
    for x in (6, 9):
        d.line((x, 8, x, 14), fill=1)
    return img


def undo():
    # Undo (the toast's compact button): an arrow curling back to the left.
    return from_art([
        "....#...........",
        "...##...........",
        "..###########...",
        ".#############..",
        "..###.......###.",
        "...##........##.",
        "....#........##.",
        ".............##.",
        "............###.",
        "......#########.",
        "......########..",
        "................",
    ])


def sd_card():
    # The no-card state: a microSD card with its contacts.
    img, d = canvas(24, 30)
    d.polygon([(0, 0), (17, 0), (23, 6), (23, 29), (0, 29)], outline=1)
    d.polygon([(1, 1), (16, 1), (22, 7), (22, 28), (1, 28)], outline=1)
    for x in (6, 10, 14, 18):
        d.rectangle((x, 4, x + 1, 9), fill=1)
    return img


def shuffle():
    # Shuffle all: two crossing arrows.
    img, d = canvas(20, 16)
    d.line((0, 3, 5, 3), fill=1, width=2)
    d.line((5, 3, 13, 12), fill=1, width=2)
    d.line((13, 12, 17, 12), fill=1, width=2)
    d.line((0, 12, 5, 12), fill=1, width=2)
    d.line((5, 12, 13, 3), fill=1, width=2)
    d.line((13, 3, 17, 3), fill=1, width=2)
    d.polygon([(16, 0), (19, 3), (16, 6)], fill=1)
    d.polygon([(16, 9), (19, 12), (16, 15)], fill=1)
    return img


def jack():
    # Line out: a 3.5 mm plug.
    img, d = canvas(22, 18)
    d.rectangle((0, 7, 5, 10), fill=1)                  # the cable
    d.rounded_rectangle((6, 5, 12, 12), 1, fill=1)      # the body
    d.rectangle((13, 7, 20, 10), fill=1)                # the tip
    d.line((16, 7, 16, 10), fill=0)                     # its rings
    d.line((18, 7, 18, 10), fill=0)
    return img


def warn():
    # A track that couldn't be played: "!" in a triangle.
    return from_art([
        ".......##.......",
        "......####......",
        "......####......",
        ".....##..##.....",
        ".....##..##.....",
        "....##.##.##....",
        "....##.##.##....",
        "...##..##..##...",
        "...##..##..##...",
        "..##...##...##..",
        "..##........##..",
        ".##.....##...##.",
        ".##..........##.",
        "################",
    ])


def gear():
    # Settings rows (touch calibration).
    img, d = canvas(18, 18)
    d.ellipse((3, 3, 14, 14), outline=1, width=2)
    d.ellipse((7, 7, 10, 10), fill=1)
    for x0, y0, x1, y1 in ((8, 0, 9, 3), (8, 14, 9, 17), (0, 8, 3, 9), (14, 8, 17, 9)):
        d.rectangle((x0, y0, x1, y1), fill=1)
    for (x, y) in ((2, 2), (13, 2), (2, 13), (13, 13)):
        d.rectangle((x, y, x + 2, y + 2), fill=1)
    return img


def info():
    # About: "i" in a circle.
    img, d = canvas(18, 18)
    d.ellipse((0, 0, 17, 17), outline=1, width=2)
    d.rectangle((8, 4, 9, 5), fill=1)
    d.rectangle((8, 7, 9, 13), fill=1)
    return img


def vibrate():
    # Haptics: a phone with buzz marks.
    img, d = canvas(20, 18)
    d.rounded_rectangle((6, 0, 13, 17), 2, outline=1)
    for x in (2, 17):
        d.line((x, 5, x, 12), fill=1)
    for x in (0, 19):
        d.line((x, 7, x, 10), fill=1)
    return img


# Now Playing's shuffle and repeat indicator (docs/QUEUE-MODES.md section
# 4.2): small enough to sit under the "..." dots (18 x 4), 1 px strokes.
# kShuffle (20 x 16, Shuffle all's) is too heavy there.
SHUFFLE_SMALL = [
    "...........#..",
    "...........##.",
    "####.....#####",
    "....#...#..##.",
    ".....#.#...#..",
    "......#.......",
    ".....#.#...#..",
    "....#...#..##.",
    "####.....#####",
    "...........##.",
    "...........#..",
]

REPEAT_SMALL = [
    ".........#....",
    ".........##...",
    ".###########..",
    ".#.......##...",
    ".#.......#..#.",
    ".#..........#.",
    ".#..#.......#.",
    "...##.......#.",
    "..###########.",
    "...##.........",
    "....#.........",
]

# The loop with a "1" in it: Repeat One.
REPEAT_ONE_SMALL = [
    ".........#....",
    ".........##...",
    ".###########..",
    ".#.....#.##...",
    ".#....##.#..#.",
    ".#.....#....#.",
    ".#..#..#....#.",
    "...##.###...#.",
    "..###########.",
    "...##.........",
    "....#.........",
]


ICONS = [
    ("Library", library),
    ("Queue", queue),
    ("Crab", lambda: from_art(CRAB)),
    ("Headphones", headphones),
    ("Speaker", speaker),
    ("Note", note),
    ("Play", play),
    ("Pause", pause),
    ("Next", next_track),
    ("Prev", prev_track),
    ("More", more),
    ("ChevronRight", chevron_right),
    ("ChevronLeft", chevron_left),
    ("Check", check),
    ("Cross", cross),
    ("Plus", plus),
    ("Minus", minus),
    ("Folder", folder),
    ("File", audio_file),
    ("Trash", trash),
    ("Undo", undo),
    ("SdCard", sd_card),
    ("Shuffle", shuffle),
    ("Jack", jack),
    ("Warn", warn),
    ("Gear", gear),
    ("Info", info),
    ("Vibrate", vibrate),
    ("ShuffleSmall", lambda: from_art(SHUFFLE_SMALL)),
    ("RepeatSmall", lambda: from_art(REPEAT_SMALL)),
    ("RepeatOneSmall", lambda: from_art(REPEAT_ONE_SMALL)),
]


def bits(img):
    w, h = img.size
    out = []
    for y in range(h):
        for bx in range(0, w, 8):
            b = 0
            for k in range(8):
                x = bx + k
                if x < w and img.getpixel((x, y)):
                    b |= 0x80 >> k
            out.append(b)
    return out


def preview(name, img):
    w, h = img.size
    print(f"{name} {w}x{h}")
    for y in range(h):
        print("  " + "".join("#" if img.getpixel((x, y)) else "." for x in range(w)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "src", "ui", "IconData.cpp"))
    ap.add_argument("--preview", action="store_true")
    args = ap.parse_args()
    nl = chr(10)
    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "// Copyright (C) 2026 IrosTheBeggar",
        "// Generated by tools/ui_icons.py - do not edit.",
        "// The UI's 1-bit icons (M5GFX drawBitmap: rows of whole bytes, MSB first).",
        '#include "ui/Icons.h"',
        "",
        "namespace icons {",
        "",
    ]
    for name, make in ICONS:
        img = make()
        if args.preview:
            preview(name, img)
        w, h = img.size
        data = bits(img)
        body = ", ".join(f"0x{b:02x}" for b in data)
        lines.append(f"static const uint8_t k{name}Bits[] = {{{body}}};")
        lines.append(f"const Icon k{name} = {{k{name}Bits, {w}, {h}}};")
        lines.append("")
    lines.append("}  // namespace icons")
    with open(args.out, "w", encoding="utf-8", newline=nl) as f:
        f.write(nl.join(lines) + nl)
    print(f"wrote {os.path.normpath(args.out)}")


if __name__ == "__main__":
    main()
