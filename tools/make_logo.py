#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""The mStream logo for the boot screen (lib/core/LogoArt.h, LogoArt.cpp),
from tools/art/mstream-logo.svg.

    python tools/make_logo.py                # regenerate
    python tools/make_logo.py --check        # exit 1 if the checked-in files are stale
    python tools/make_logo.py --png out.png  # also the decoded logo on the boot screen's colours

The SVG is mStream's own logo (webapp/assets/img/mstream-logo.svg in
https://github.com/IrosTheBeggar/mStream, GPL-3.0, the same author): an "m"
of three bars (two light blue, a navy one between them with a notch) and
"stream" in navy, flat colours on nothing. It is rendered here with Pillow
alone: the paths' curves are flattened to polygons, each part's shapes are
filled 8x8 supersampled, and averaged down to the logo's size. Every pixel
is then a part (0 nothing, 1 the word, 2 the outer bars, 3 the middle bar:
the part that covers most of it) and a coverage, 0-32. The firmware picks
each part's colour when it draws (lib/core/BootLayout.h), and blends the
edges into the background then: the dark screen, or a light panel.

The pixels are run-length coded a row at a time (lib/core/RleImage.h reads
them), one byte a token, a run never crossing a row's end:

    00nnnnnn    n+1 pixels of nothing (1-64)
    pp0nnnnn    n+1 pixels of part pp (1-3) at full coverage (1-32)
    pp1aaaaa    one pixel of part pp at coverage a+1 (1-31, of 32)

A 240 px logo is about 2.2 KB this way (raw RGB565 would be 22 KB, and tied to
one background). The arrays are const, in flash. Needs Pillow.
"""
import argparse
import math
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "tools" / "art" / "mstream-logo.svg"
OUT_H = ROOT / "lib" / "core" / "LogoArt.h"
OUT_CPP = ROOT / "lib" / "core" / "LogoArt.cpp"

WIDTH = 240      # the logo's width on the screen, px (its ink, plus a pixel each side)
SS = 8           # supersampling, each way
LEVELS = 32      # coverage steps
CURVE_STEPS = 24  # line segments per cubic curve (at 8x)

# The SVG's classes, by fill: which part each is.
PARTS = {
    "#264679": 1,  # "stream"
    "#6684B2": 2,  # the outer bars of the "m"
    "#26477B": 3,  # its middle bar
}
PART_NAMES = {1: "the word \"stream\"", 2: "the outer bars", 3: "the middle bar"}

SVG_NS = "{http://www.w3.org/2000/svg}"
TOKEN = re.compile(r"[MmLlHhVvCcSsZz]|[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?")


# ---- the SVG ----

def path_polygons(d):
    """The closed polygons of an SVG path's subpaths (M, L, H, V, C, S, Z,
    absolute and relative: what the logo uses)."""
    toks = TOKEN.findall(d)
    polys, cur = [], []
    x = y = 0.0
    start = (0.0, 0.0)
    last_ctrl = None
    cmd = None
    i = 0

    def num():
        nonlocal i
        v = float(toks[i])
        i += 1
        return v

    def cubic(p0, p1, p2, p3):
        for k in range(1, CURVE_STEPS + 1):
            t = k / CURVE_STEPS
            u = 1 - t
            cur.append((u * u * u * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t * t * t * p3[0],
                        u * u * u * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t * t * t * p3[1]))

    while i < len(toks):
        if toks[i].isalpha():
            cmd = toks[i]
            i += 1
            if cmd in "Zz":
                if cur:
                    polys.append(cur)
                cur = []
                x, y = start
                last_ctrl = None
                continue
        rel = cmd.islower()
        c = cmd.upper()
        ox, oy = (x, y) if rel else (0.0, 0.0)
        if c == "M":
            if cur:
                polys.append(cur)
            x, y = ox + num(), oy + num()
            start = (x, y)
            cur = [(x, y)]
            cmd = "l" if rel else "L"  # later pairs are lines
            last_ctrl = None
        elif c == "L":
            x, y = ox + num(), oy + num()
            cur.append((x, y))
            last_ctrl = None
        elif c == "H":
            x = (x if rel else 0.0) + num()
            cur.append((x, y))
            last_ctrl = None
        elif c == "V":
            y = (y if rel else 0.0) + num()
            cur.append((x, y))
            last_ctrl = None
        elif c == "C":
            p1 = (ox + num(), oy + num())
            p2 = (ox + num(), oy + num())
            p3 = (ox + num(), oy + num())
            cubic((x, y), p1, p2, p3)
            x, y = p3
            last_ctrl = p2
        elif c == "S":
            p1 = (2 * x - last_ctrl[0], 2 * y - last_ctrl[1]) if last_ctrl else (x, y)
            p2 = (ox + num(), oy + num())
            p3 = (ox + num(), oy + num())
            cubic((x, y), p1, p2, p3)
            x, y = p3
            last_ctrl = p2
        else:
            raise ValueError("unsupported path command " + cmd)
    if cur:
        polys.append(cur)
    return polys


def load_shapes(svg):
    """[(part, [polygon, ...]), ...] in document order, and the viewBox."""
    root = ET.parse(svg).getroot()
    style = "".join(e.text or "" for e in root.iter(SVG_NS + "style"))
    fills = {m.group(1): m.group(2).upper() for m in re.finditer(r"\.(\w+)\{[^}]*fill:(#[0-9A-Fa-f]{6})", style)}
    shapes = []
    for e in root.iter():
        tag = e.tag.replace(SVG_NS, "")
        if tag not in ("path", "polygon"):
            continue
        fill = fills[e.get("class")]
        part = {k.upper(): v for k, v in PARTS.items()}[fill]
        if tag == "path":
            polys = path_polygons(e.get("d"))
        else:
            nums = [float(v) for v in TOKEN.findall(e.get("points"))]
            polys = [list(zip(nums[0::2], nums[1::2]))]
        shapes.append((part, polys))
    vb = [float(v) for v in root.get("viewBox").split()]
    return shapes, vb


# ---- rendering ----

def render(width=WIDTH):
    """The logo at `width` px: (w, h, cover) where cover[part] is an 'L'
    image of that part's coverage (0-255)."""
    shapes, _ = load_shapes(SRC)
    xs = [p[0] for _, polys in shapes for poly in polys for p in poly]
    ys = [p[1] for _, polys in shapes for poly in polys for p in poly]
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    # The ink spans width - 2 px: a pixel of room each side for its edges
    # (less a subpixel: Pillow fills a polygon's far edge too).
    scale = ((width - 2) * SS - 1) / ((x1 - x0) * SS)
    h = math.ceil(((y1 - y0) * scale * SS + 1) / SS) + 2
    cover = {}
    for part in sorted(set(PARTS.values())):
        mask = Image.new("L", (width * SS, h * SS), 0)
        for p, polys in shapes:
            if p != part:
                continue
            for poly in polys:
                one = Image.new("L", mask.size, 0)
                pts = [((px - x0) * scale * SS + SS, (py - y0) * scale * SS + SS) for px, py in poly]
                ImageDraw.Draw(one).polygon(pts, fill=255)
                mask = ImageChops.logical_xor(mask.convert("1"), one.convert("1")).convert("L")  # even-odd
        cover[part] = mask.reduce(SS)
    return width, h, cover


def classify(w, h, cover):
    """Rows of (part, level): the part covering most of the pixel, and the
    pixel's whole coverage in 0..LEVELS."""
    px = {p: img.load() for p, img in cover.items()}
    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            best, total = 0, 0
            for p in px:
                v = px[p][x, y]
                total += v
                if v > (px[best][x, y] if best else 0):
                    best = p
            level = min(LEVELS, (total * LEVELS + 127) // 255)
            row.append((best, level) if level else (0, 0))
        rows.append(row)
    return rows


# ---- the code ----

def encode(rows):
    out = []
    for row in rows:
        x = 0
        w = len(row)
        while x < w:
            part, level = row[x]
            if part == 0:
                n = 1
                while x + n < w and row[x + n][0] == 0 and n < 64:
                    n += 1
                out.append(n - 1)
            elif level == LEVELS:
                n = 1
                while x + n < w and row[x + n] == (part, LEVELS) and n < 32:
                    n += 1
                out.append(part << 6 | (n - 1))
            else:
                n = 1
                out.append(part << 6 | 0x20 | (level - 1))
            x += n
    return out


def decode(data, w, h):
    """The inverse of encode(), as lib/core/RleImage does it (for --png and
    the check below)."""
    rows, i = [], 0
    for _ in range(h):
        row = []
        while len(row) < w:
            b = data[i]
            i += 1
            part = b >> 6
            if part == 0:
                row += [(0, 0)] * ((b & 0x3F) + 1)
            elif b & 0x20:
                row.append((part, (b & 0x1F) + 1))
            else:
                row += [(part, LEVELS)] * ((b & 0x1F) + 1)
        assert len(row) == w, "a run crossed a row's end"
        rows.append(row)
    assert i == len(data)
    return rows


def stats(rows):
    """What the host test checks the decoder against: each part's pixel count
    and its coverage summed (levels)."""
    count = {1: 0, 2: 0, 3: 0}
    level = {1: 0, 2: 0, 3: 0}
    for row in rows:
        for p, l in row:
            if p:
                count[p] += 1
                level[p] += l
    return count, level


def generate(width=WIDTH):
    w, h, cover = render(width)
    rows = classify(w, h, cover)
    data = encode(rows)
    assert decode(data, w, h) == rows
    count, level = stats(rows)
    header = f"""// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar
// Generated by tools/make_logo.py from tools/art/mstream-logo.svg (mStream's
// logo, GPL-3.0). Do not edit: run `python tools/make_logo.py`.
//
// The mStream logo for the boot screen, {w} x {h} px: run-length coded rows of
// parts and coverage (lib/core/RleImage.h reads them; the code is described
// in the tool). The part's colours are the boot screen's (BootLayout.h).
#pragma once
#include <cstdint>

namespace logo {{

constexpr int kW = {w}, kH = {h};
constexpr int kParts = 3;  // 1 {PART_NAMES[1]}, 2 {PART_NAMES[2]}, 3 {PART_NAMES[3]}
constexpr int kLevels = {LEVELS};  // a pixel's coverage, 1..kLevels
extern const uint8_t kData[];
constexpr uint32_t kSize = {len(data)};

// For the host test: each part's pixels, and their coverage summed.
constexpr uint32_t kPixels[kParts + 1] = {{0, {count[1]}, {count[2]}, {count[3]}}};
constexpr uint32_t kCoverage[kParts + 1] = {{0, {level[1]}, {level[2]}, {level[3]}}};

}}  // namespace logo
"""
    lines = []
    for k in range(0, len(data), 20):
        lines.append("    " + " ".join(f"0x{b:02x}," for b in data[k:k + 20]))
    cpp = f"""// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar
// Generated by tools/make_logo.py from tools/art/mstream-logo.svg. Do not edit.
#include "LogoArt.h"

namespace logo {{

const uint8_t kData[kSize] = {{
{chr(10).join(lines)}
}};

}}  // namespace logo
"""
    return header, cpp, (w, h, rows, len(data))


def to_png(rows, w, h, colours, bg, path):
    img = Image.new("RGB", (w, h), bg)
    px = img.load()
    for y, row in enumerate(rows):
        for x, (p, l) in enumerate(row):
            if p:
                c = colours[p]
                px[x, y] = tuple(bg[k] + (c[k] - bg[k]) * l // LEVELS for k in range(3))
    img.save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true", help="exit 1 if the checked-in files are stale")
    ap.add_argument("--png", help="also write the decoded logo here, in the boot screen's colours")
    args = ap.parse_args()
    header, cpp, (w, h, rows, size) = generate()
    if args.check:
        stale = [p for p, text in ((OUT_H, header), (OUT_CPP, cpp))
                 if not p.exists() or p.read_text(encoding="utf-8") != text]
        for p in stale:
            print(f"stale: {p.relative_to(ROOT)} (run python tools/make_logo.py)")
        return 1 if stale else 0
    OUT_H.write_text(header, encoding="utf-8", newline="\n")
    OUT_CPP.write_text(cpp, encoding="utf-8", newline="\n")
    print(f"logo {w} x {h} px: {size} B of runs (RGB565 would be {w * h * 2} B)")
    if args.png:
        # The boot screen's colours (lib/core/BootLayout.h).
        src = (ROOT / "lib" / "core" / "BootLayout.h").read_text(encoding="utf-8")
        rgb = {int(m.group(1)): int(m.group(2), 16)
               for m in re.finditer(r"kLogoPart(\d)\s*=\s*0x([0-9A-Fa-f]{6})", src)}
        bg = int(re.search(r"kLogoBg\s*=\s*0x([0-9A-Fa-f]{6})", src).group(1), 16)
        split = lambda v: ((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)  # noqa: E731
        to_png(rows, w, h, {p: split(v) for p, v in rgb.items()}, split(bg), args.png)
        print("wrote", args.png)
    return 0


if __name__ == "__main__":
    sys.exit(main())
