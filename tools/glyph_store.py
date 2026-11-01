#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Rasterise a CJK font into a glyph store: the bitmaps behind the DejaVu
fonts, for the Chinese, Japanese and Korean characters they lack
(docs/I18N.md, phase 2). lib/core/GlyphStore reads it.

The default makes lib/core/CjkGlyphs16.cpp: one 16 px store, 4 bits a pixel,
in the app's flash, from Noto Sans CJK Regular's Japanese face (its TTC/OTC,
version 2.004; SIL Open Font License 1.1). Every UI role draws CJK from it:
Small and Title at 16 px too, Bold and Title with the regular weight. A
store is one pixel size of one font; a second size, or a store the transfer
software writes to the card for one library's names, is another store in
the same format.

The characters (--set; each less what the four VLW fonts draw, DejaVu
winning, and what the face has no glyph for):
  pan     (the default) KS X 1001's 2,350 Hangul syllables and the Hangul
          Compatibility Jamo (U+3131-318E); JIS X 0208 whole (kanji levels
          1 and 2 and its other rows; the cells Windows' cp932 maps
          elsewhere too, ∥ U+2225); GB2312's level 1 hanzi (3,755); the
          Hiragana, Katakana and Katakana Phonetic Extensions blocks; CJK
          Symbols and Punctuation (U+3000-303F); the fullwidth signs TextFold
          doesn't fold to ASCII (｟ ｠, ￠-￦, ￨-￮)
  jis     the Hangul, JIS X 0208, the kana and the signs
  hangul  the Hangul and the signs
  pan-big5  pan and Big5's level 1 hanzi (5,401): what Traditional Chinese
          would add
The standards come from Python's own codecs (euc_kr, euc_jp, cp932, gb2312,
big5). Every set also takes the characters the UI's catalogues use
(--chars-from, i18n/ by default: every text of i18n/*.json, composed as
the device draws it), so a translation's Chinese or Japanese is drawn
whatever the set leaves out (a GB2312 level 2 hanzi in zh.json, say); the
generated file names the ones the set hadn't. Left out everywhere: fullwidth ASCII (U+FF01-FF5E), the minus sign
− (U+2212) and halfwidth katakana (U+FF61-FF9F), which TextFold folds (to
ASCII, which DejaVu draws; to the fullwidth kana), and a glyph whose ink
reaches more than 2 px past the em box (fits(): the vertical kana repeat
marks 〱 〲 and the ideographic tone marks, which only vertical text uses,
and the halfwidth bar ￨). The generated file names those.

One store draws a Han character in one regional style: --region picks the
TTC's face by its name (JP, KR, SC, TC or HK; the default JP suits Japanese
names, and every JIS X 0208 kanji is a JP glyph).

Rasterising: FreeType through Pillow, as tools/vlw_font.py (hinted), at
--size px to the em: the CJK em box is the font's OS/2 typo ascender and
descender (0.88 and 0.12 em: at 16 px 14 px above the baseline and 2 below),
so on DejaVu Sans 16's baseline (15 px above, 4 below, the 19 px Body row) a
CJK glyph sits inside the row. 8-bit coverage is quantised to 4 bits with
rounding ((v * 15 + 127) / 255) and cut to the glyph's tight box.

The store (version 1; all fields little-endian, every section 4-byte
aligned):
  header (32 B)  "GLYS", u16 version, u16 section count; u8 pixel size,
          u8 bits a pixel (4), u8 encoding (0 raw, 1 runs), u8 flags (0);
          i8 ascent, i8 descent (the em box above and below the baseline),
          i8 ink ascent, i8 ink descent (how far the ink reaches);
          u8 widest box, u8 tallest box, u16 0; u32 glyph count, u32 page
          count, u32 the store's size in bytes
  sections (12 B each: tag, u32 offset, u32 size), any order; a reader
          skips a tag it doesn't know:
    PAGE  u16 a page: the code points cp >> 8 the store has glyphs on,
          ascending (a binary search finds one)
    PMAP  48 B a page: its presence bitmap, 8 u32 (bit cp & 31 of word
          (cp & 255) >> 5); u32 its first glyph's index; u32 where its
          bitmaps start in BITS; 8 u8, the bits set in the words before
          each word. A glyph's index is its page's first, plus its word's
          u8, plus the bits set below its own in its word (glyphs are in
          code point order): a lookup counts the bits of one word
    METR  u32 a glyph: box width (bits 0-5), height (6-11), the box's left
          edge from the pen (12-18, signed) and its top above the baseline
          (19-25, signed), the advance (26-31)
    OFFS  u16 a glyph: its bitmap's offset from its page's start in BITS
          (so a page's bitmaps stay under 64 KB: build() checks)
    BITS  the bitmaps: width x height coverage values (0-15) a glyph, row
          by row, packed two to a byte, the first in the high nibble,
          starting on a byte. Raw (encoding 0), or as runs (1), a nibble
          each: 0 n is n + 1 zeros, 15 n is n + 1 fifteens (n 0-15), and
          1-14 is that value once; a run may cross rows.
The runs save 17.9 % of pan's bitmaps (measured: 1,252,173 B raw, 1,028,108 B
as runs; the store 1,322,364 B against 1,098,296 B) for a decoder of a few
lines, so the store uses them (--raw writes the other). The index (PAGE,
PMAP, METR, OFFS) is 6 B a glyph and 50 B a page: 70,188 B of pan's store.

Usage (the font is not in this repository: --font, or GLYPH_STORE_FONT):

    python tools/glyph_store.py --font NotoSansCJK-Regular.ttc
    python tools/glyph_store.py --font ... --check   # exit 1 if a generated file differs
    python tools/glyph_store.py --font ... --sizes   # each set's flash cost; writes nothing
    python tools/glyph_store.py --font ... --chars-from ''  # the set alone, not the catalogues' too
    python tools/glyph_store.py --fixture            # test_glyph_store's fixture store only (no font)

A translation with a character the store lacks fails the catalogue's check
(tools/gen_ui_strings.py names the code point) until this is run again
with the font, and the store committed with the catalogue.

The checked-in file was made with Pillow 12.2.0 (FreeType 2.14.3); another
FreeType can hint a pixel differently. Its header names the font file, its
version and its SHA-256, so --check proves the source. Needs Pillow.

The derived glyphs are a Modified Version of the font under the SIL Open Font
License 1.1: the generated file carries the font's copyright (read from its
name table) and stays under that licence (LICENSES/OFL-1.1.txt), and no
store, file or symbol is named after the font (its Reserved Font Name is
"Source"; "Noto" is a trademark): kCjkGlyphs16.
"""
import argparse
import glob
import hashlib
import json
import os
import re
import struct
import sys
import unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
NL = chr(10)

# ---- the characters ----


def dbcs(codec, leads, trails):
    """The code points a double-byte codec decodes the cells leads x trails to."""
    out = {}
    for a in leads:
        for b in trails:
            try:
                s = bytes([a, b]).decode(codec)
            except UnicodeDecodeError:
                continue
            if len(s) == 1:
                out[(a, b)] = ord(s)
    return out


def is_han(cp):
    return 0x3400 <= cp <= 0x4DBF or 0x4E00 <= cp <= 0x9FFF or 0xF900 <= cp <= 0xFAFF


def ks_hangul():
    """KS X 1001's 2,350 Hangul syllables (rows 16-40)."""
    return {cp for cp in dbcs("euc_kr", range(0xB0, 0xC9), range(0xA1, 0xFF)).values() if 0xAC00 <= cp <= 0xD7A3}


def jamo():
    """Hangul Compatibility Jamo, U+3131-318E (KS X 1001's row 4)."""
    return set(range(0x3131, 0x318F))


def sjis(a, b):
    """An EUC-JP cell's Shift_JIS bytes."""
    j1, j2 = a - 0x80, b - 0x80  # JIS X 0208's row and cell bytes, 0x21-0x7E
    s1 = (j1 + 1) // 2 + (0x70 if j1 <= 0x5E else 0xB0)
    if j1 % 2:
        s2 = j2 + 0x1F + (1 if j2 >= 0x60 else 0)
    else:
        s2 = j2 + 0x7E
    return bytes([s1, s2])


def jis0208():
    """JIS X 0208 whole (EUC-JP's two-byte cells), and the code points
    Windows' cp932 gives the same cells where it differs (∥ for ‖, ￢ for ¬)."""
    cells = dbcs("euc_jp", range(0xA1, 0xF5), range(0xA1, 0xFF))
    out = set(cells.values())
    for a, b in cells:
        try:
            s = sjis(a, b).decode("cp932")
        except UnicodeDecodeError:
            continue
        if len(s) == 1:
            out.add(ord(s))
    return out


def gb2312_level1():
    """GB2312's 3,755 level 1 hanzi (rows 16-55)."""
    return {cp for cp in dbcs("gb2312", range(0xB0, 0xD8), range(0xA1, 0xFF)).values() if is_han(cp)}


def big5_level1():
    """Big5's 5,401 level 1 hanzi (0xA440-0xC67E)."""
    out = set()
    for a in range(0xA4, 0xC7):
        trails = range(0x40, 0x7F) if a == 0xC6 else list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF))
        out |= {cp for cp in dbcs("big5", [a], trails).values() if is_han(cp)}
    return out


def kana():
    """The Hiragana, Katakana and Katakana Phonetic Extensions blocks."""
    return set(range(0x3040, 0x3100)) | set(range(0x31F0, 0x3200))


def signs():
    """CJK Symbols and Punctuation, and the fullwidth signs TextFold leaves
    (it folds U+FF01-FF5E to ASCII): ｟ ｠, ￠ ￡ ￢ ￣ ￤ ￥ ￦, ￨ and the
    halfwidth arrows and shapes."""
    return set(range(0x3000, 0x3040)) | {0xFF5F, 0xFF60} | set(range(0xFFE0, 0xFFE7)) | set(range(0xFFE8, 0xFFEF))


def folded(cp):
    """Code points TextFold folds before drawing, so no store needs them:
    fullwidth ASCII and JIS X 0208's minus sign − U+2212 (to ASCII, which
    DejaVu draws: a Latin name with − stays DejaVu's), and halfwidth katakana
    (to the fullwidth). The ideographic space U+3000, which TextFold folds to
    ' ' too, stays: a line breaks at it. test_glyph_store checks that no
    other glyph has an ASCII fold."""
    return 0xFF01 <= cp <= 0xFF5E or 0xFF61 <= cp <= 0xFF9F or cp == 0x2212


SETS = {
    "pan": lambda: ks_hangul() | jamo() | jis0208() | gb2312_level1() | kana() | signs(),
    "jis": lambda: ks_hangul() | jamo() | jis0208() | kana() | signs(),
    "hangul": lambda: ks_hangul() | jamo() | signs(),
    "pan-big5": lambda: ks_hangul() | jamo() | jis0208() | gb2312_level1() | kana() | signs() | big5_level1(),
}


def catalogue_chars(folder):
    """The code points the UI's texts use: every message of i18n/*.json (a
    plural's forms too) and each file's language name in itself, composed
    (NFC, as TextFit draws). Metadata ("@<id>", "@@note") is left out: the
    device never draws it. An empty set for no folder."""
    out = set()
    if not folder:
        return out
    for path in sorted(glob.glob(os.path.join(folder, "*.json"))):
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        texts = [data.get("@@autonym", "")]
        for key, value in data.items():
            if key.startswith("@"):
                continue
            texts += list(value.values()) if isinstance(value, dict) else [value]
        for t in texts:
            if isinstance(t, str):
                out |= {ord(ch) for ch in unicodedata.normalize("NFC", t) if ch >= " "}
    return out


def vlw_codepoints(path):
    """The code points the VLW fonts in `path` (tools/vlw_font.py's
    VlwFonts.cpp) draw: every font's, together."""
    with open(path, encoding="utf-8") as f:
        text = f.read()
    out = set()
    for m in re.finditer(r"const uint8_t \w+\[\d+\] = \{(.*?)\};", text, re.S):
        vlw = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", m.group(1)))
        for i in range(struct.unpack(">i", vlw[:4])[0]):  # the glyph count; 28 B of metrics a glyph
            out.add(struct.unpack(">i", vlw[24 + 28 * i:28 + 28 * i])[0])
    if not out:
        sys.exit(f"no VLW fonts in {path}")
    return out


# ---- the font file ----


class Face:
    """One face of a TrueType/OpenType font or collection: its names, cmap
    and em box, read from its tables (no fontTools)."""

    def __init__(self, data, offset):
        self.data = data
        self.tables = {}
        for i in range(struct.unpack(">H", data[offset + 4:offset + 6])[0]):
            tag, _, at, length = struct.unpack(">4sIII", data[offset + 12 + 16 * i:offset + 28 + 16 * i])
            self.tables[tag] = (at, length)

    def name(self, nid):
        """Name ID `nid` (Windows, English), or ""."""
        at = self.tables[b"name"][0]
        count, strings = struct.unpack(">HH", self.data[at + 2:at + 6])
        for j in range(count):
            pid, _, lang, n, length, off = struct.unpack(">6H", self.data[at + 6 + 12 * j:at + 18 + 12 * j])
            if pid == 3 and lang == 0x409 and n == nid:
                return self.data[at + strings + off:at + strings + off + length].decode("utf-16-be")
        return ""

    def em_box(self):
        """(units per em, OS/2 typo ascender, descender as a positive depth)."""
        head = self.tables[b"head"][0]
        upem = struct.unpack(">H", self.data[head + 18:head + 20])[0]
        os2 = self.tables[b"OS/2"][0]
        asc, desc = struct.unpack(">hh", self.data[os2 + 68:os2 + 72])
        return upem, asc, -desc

    def cmap(self):
        """The code points the face maps to a glyph (format 12, else 4)."""
        at = self.tables[b"cmap"][0]
        subs = {}
        for i in range(struct.unpack(">H", self.data[at + 2:at + 4])[0]):
            pid, eid, off = struct.unpack(">HHI", self.data[at + 4 + 8 * i:at + 12 + 8 * i])
            fmt = struct.unpack(">H", self.data[at + off:at + off + 2])[0]
            subs[(pid, eid, fmt)] = at + off
        d, cps = self.data, set()
        if (3, 10, 12) in subs:
            s = subs[(3, 10, 12)]
            for k in range(struct.unpack(">I", d[s + 12:s + 16])[0]):
                first, last, gid = struct.unpack(">III", d[s + 16 + 12 * k:s + 28 + 12 * k])
                cps.update(cp for cp in range(first, last + 1) if gid + cp - first)
            return cps
        s = subs[(3, 1, 4)]
        n = struct.unpack(">H", d[s + 6:s + 8])[0] // 2
        ends = struct.unpack(f">{n}H", d[s + 14:s + 14 + 2 * n])
        starts = struct.unpack(f">{n}H", d[s + 16 + 2 * n:s + 16 + 4 * n])
        deltas = struct.unpack(f">{n}h", d[s + 16 + 4 * n:s + 16 + 6 * n])
        ranges_at = s + 16 + 6 * n
        offsets = struct.unpack(f">{n}H", d[ranges_at:ranges_at + 2 * n])
        for k in range(n):
            for cp in range(starts[k], min(ends[k], 0xFFFE) + 1):
                if offsets[k] == 0:
                    g = (cp + deltas[k]) & 0xFFFF
                else:
                    off = ranges_at + 2 * k + offsets[k] + 2 * (cp - starts[k])
                    g = struct.unpack(">H", d[off:off + 2])[0]
                    g = (g + deltas[k]) & 0xFFFF if g else 0
                if g:
                    cps.add(cp)
        return cps


def faces(data):
    """Each face's offset: a collection's (ttcf), else the font's own."""
    if data[:4] == b"ttcf":
        n = struct.unpack(">I", data[8:12])[0]
        return list(struct.unpack(f">{n}I", data[12:12 + 4 * n]))
    return [0]


REGIONS = ("JP", "KR", "SC", "TC", "HK")


def pick_face(data, region):
    """(index, Face) of the proportional face whose family ends in " <region>"."""
    for i, off in enumerate(faces(data)):
        face = Face(data, off)
        family = face.name(1)
        if family.endswith(" " + region) and "Mono" not in family:
            return i, face
    sys.exit(f"no {region} face in the font")


# ---- rasterising ----


def quantise(v):
    """8-bit coverage to 4 bits, rounded."""
    return (v * 15 + 127) // 255


def trim(w, h, px):
    """The tight box around the nonzero values of a w x h raster:
    (left, top, width, height), or None for no ink."""
    rows = [px[r * w:(r + 1) * w] for r in range(h)]
    inked = [r for r in range(h) if any(rows[r])]
    if not inked:
        return None
    cols = [c for c in range(w) if any(rows[r][c] for r in inked)]
    return cols[0], inked[0], cols[-1] - cols[0] + 1, inked[-1] - inked[0] + 1


def rasterise(font, cp):
    """(w, h, x, y, advance, pixels) of a code point: the tight box, its left
    edge from the pen and its top above the baseline, and w x h values 0-15."""
    from PIL import Image, ImageDraw
    ch = chr(cp)
    advance = int(round(font.getlength(ch)))
    x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")
    w, h = x1 - x0, y1 - y0
    if w <= 0 or h <= 0:
        return 0, 0, 0, 0, advance, ()
    img = Image.new("L", (w, h), 0)
    ImageDraw.Draw(img).text((-x0, -y0), ch, font=font, fill=255, anchor="ls")
    px = [quantise(v) for v in img.tobytes()]
    box = trim(w, h, px)
    if box is None:
        return 0, 0, 0, 0, advance, ()
    left, top, bw, bh = box
    pixels = tuple(v for r in range(top, top + bh) for v in px[r * w + left:r * w + left + bw])
    return bw, bh, x0 + left, -(y0 + top), advance, pixels


def fits(g, ascent, descent):
    """Whether a glyph's ink stays within 2 px of the em box."""
    w, h, x, y, adv, px = g
    return h == 0 or (y <= ascent + 2 and h - y <= descent + 2)


# ---- the store ----

MAGIC = b"GLYS"
VERSION = 1
RAW, RUNS = 0, 1
HEADER = 32
PMAP_SIZE = 48


def runs(pixels):
    """Values 0-15 as runs (see the docstring), packed into bytes."""
    nib, i, n = [], 0, len(pixels)
    while i < n:
        v = pixels[i]
        if v in (0, 15):
            j = i
            while j < n and pixels[j] == v and j - i < 16:
                j += 1
            nib += [v, j - i - 1]
            i = j
        else:
            nib.append(v)
            i += 1
    return pack(nib)


def unruns(data, count):
    """runs()'s inverse: `count` values from `data`, and the bytes read."""
    out, at, hi = [], 0, True

    def nibble():
        nonlocal at, hi
        v = data[at] >> 4 if hi else data[at] & 15
        if not hi:
            at += 1
        hi = not hi
        return v

    while len(out) < count:
        v = nibble()
        if v in (0, 15):
            k = nibble() + 1
            if len(out) + k > count:
                raise ValueError("a run past the box")
            out += [v] * k
        else:
            out.append(v)
    return out, at + (0 if hi else 1)


def pack(values):
    """Values 0-15 two to a byte, the first in the high nibble."""
    if len(values) % 2:
        values = list(values) + [0]
    return bytes(values[i] << 4 | values[i + 1] for i in range(0, len(values), 2))


def unpack(data, count):
    return [data[i // 2] >> 4 if i % 2 == 0 else data[i // 2] & 15 for i in range(count)]


def metrics_word(w, h, x, y, adv):
    for v, lo, hi in ((w, 0, 63), (h, 0, 63), (x, -64, 63), (y, -64, 63), (adv, 0, 63)):
        if not lo <= v <= hi:
            raise ValueError(f"a metric out of range: {v}")
    return w | h << 6 | (x & 0x7F) << 12 | (y & 0x7F) << 19 | adv << 26


def signed7(v):
    return v - 128 if v & 0x40 else v


def build(glyphs, size, ascent, descent, encoding=RUNS):
    """The store's bytes for {code point: (w, h, x, y, advance, pixels)}."""
    cps = sorted(glyphs)
    on = {}
    for cp in cps:
        on.setdefault(cp >> 8, []).append(cp)
    pages = sorted(on)
    pmap, metr, offs, bits = [], [], [], bytearray()
    index = 0
    for page in pages:
        mine = on[page]
        words = [0] * 8
        for cp in mine:
            words[(cp & 255) >> 5] |= 1 << (cp & 31)
        base = len(bits)
        before = [sum(bin(words[k]).count("1") for k in range(w)) for w in range(8)]
        pmap.append(struct.pack("<8III8B", *words, index, base, *before))
        for cp in mine:
            w, h, x, y, adv, px = glyphs[cp]
            if len(px) != w * h:
                raise ValueError(f"U+{cp:04X}: {len(px)} values for {w} x {h}")
            metr.append(metrics_word(w, h, x, y, adv))
            if len(bits) - base > 0xFFFF:
                raise SystemExit(f"page U+{page:04X}xx's bitmaps pass 64 KB: OFFS can't reach U+{cp:04X}")
            offs.append(len(bits) - base)
            bits += runs(px) if encoding == RUNS else pack(px)
        index += len(mine)
    inked = [glyphs[cp] for cp in cps if glyphs[cp][1]]
    ink_up = max((g[3] for g in inked), default=0)
    ink_down = max((g[1] - g[3] for g in inked), default=0)
    widest = max((g[0] for g in inked), default=0)
    tallest = max((g[1] for g in inked), default=0)
    sections = [
        (b"PAGE", struct.pack(f"<{len(pages)}H", *pages)),
        (b"PMAP", b"".join(pmap)),
        (b"METR", struct.pack(f"<{len(metr)}I", *metr)),
        (b"OFFS", struct.pack(f"<{len(offs)}H", *offs)),
        (b"BITS", bytes(bits)),
    ]
    at = HEADER + 12 * len(sections)
    table, body = [], b""
    for tag, data in sections:
        table.append(struct.pack("<4sII", tag, at, len(data)))
        pad = (-len(data)) % 4
        body += data + bytes(pad)
        at += len(data) + pad
    header = struct.pack("<4sHHBBBBbbbbBBHIII", MAGIC, VERSION, len(sections), size, 4, encoding, 0, ascent,
                         descent, ink_up, ink_down, widest, tallest, 0, len(cps), len(pages), at)
    return header + b"".join(table) + body


def parse(store):
    """build()'s inverse: (header fields, {code point: glyph})."""
    (magic, version, nsec, size, bpp, encoding, _, ascent, descent, ink_up, ink_down, widest, tallest, _, count,
     npages, total) = struct.unpack("<4sHHBBBBbbbbBBHIII", store[:HEADER])
    if magic != MAGIC or version != VERSION or total != len(store):
        raise ValueError("not a store")
    sec = {}
    for i in range(nsec):
        tag, at, length = struct.unpack("<4sII", store[HEADER + 12 * i:HEADER + 12 * i + 12])
        sec[tag] = store[at:at + length]
    pages = struct.unpack(f"<{npages}H", sec[b"PAGE"])
    metr = struct.unpack(f"<{count}I", sec[b"METR"])
    offs = struct.unpack(f"<{count}H", sec[b"OFFS"])
    glyphs = {}
    for p, page in enumerate(pages):
        rec = struct.unpack("<8III8B", sec[b"PMAP"][PMAP_SIZE * p:PMAP_SIZE * (p + 1)])
        index, base = rec[8], rec[9]
        if list(rec[10:18]) != [sum(bin(rec[k]).count("1") for k in range(w)) for w in range(8)]:
            raise ValueError("a page's counts")
        for bit in range(256):
            if rec[bit >> 5] >> (bit & 31) & 1:
                m = metr[index]
                w, h, x, y, adv = m & 63, m >> 6 & 63, signed7(m >> 12 & 0x7F), signed7(m >> 19 & 0x7F), m >> 26
                data = sec[b"BITS"][base + offs[index]:]
                px = unruns(data, w * h)[0] if encoding == RUNS else unpack(data, w * h)
                glyphs[page << 8 | bit] = (w, h, x, y, adv, tuple(px))
                index += 1
    head = dict(size=size, bpp=bpp, encoding=encoding, ascent=ascent, descent=descent, ink_ascent=ink_up,
                ink_descent=ink_down, widest=widest, tallest=tallest, count=count, pages=npages,
                bits=len(sec[b"BITS"]))
    return head, glyphs


def digest(glyphs):
    """FNV-1a 64 of every glyph in code point order: the code point (4 bytes,
    little-endian), w, h, x, y, advance (a byte each), its w x h values.
    test_glyph_store recomputes it through GlyphStore's decoder."""
    h = 0xCBF29CE484222325

    def add(bs):
        nonlocal h
        for b in bs:
            h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF

    for cp in sorted(glyphs):
        w, gh, x, y, adv, px = glyphs[cp]
        add(struct.pack("<I", cp))
        add(bytes([w, gh, x & 0xFF, y & 0xFF, adv]))
        add(bytes(px))
    return h


# ---- the generated files ----


def c_words(name, store):
    """The store as a C++ array of little-endian u32 words (4-byte aligned,
    and fewer characters than bytes would take)."""
    words = struct.unpack(f"<{len(store) // 4}I", store)
    lines = ["    " + ",".join(f"0x{w:08x}" for w in words[i:i + 16]) + "," for i in range(0, len(words), 16)]
    return f"const uint32_t {name}[{len(words)}] = {{" + NL + NL.join(lines) + NL + "};" + NL


def wrap_list(prefix, items, width=100):
    lines, line = [], prefix
    for it in items:
        if len(line) + 1 + len(it) > width:
            lines.append(line)
            line = "//  "
        line += " " + it
    lines.append(line)
    return NL.join(lines) + NL


def wanted(name, vlw, extra=frozenset()):
    """A set's code points and `extra` (the catalogues'), less what TextFold
    folds and the VLW fonts draw."""
    return {cp for cp in SETS[name]() | set(extra) if not folded(cp)} - vlw


def draw(path, args, want):
    """The font's facts, and `want` drawn: ({cp: glyph}, the code points left
    out by fits(), those the face has no glyph for)."""
    import PIL
    from PIL import ImageFont, features
    with open(path, "rb") as f:
        data = f.read()
    index, face = pick_face(data, args.region)
    upem, asc, desc = face.em_box()
    ascent, descent = round(args.size * asc / upem), round(args.size * desc / upem)
    mapped = face.cmap()
    font = ImageFont.truetype(path, args.size, index=index)
    glyphs, outside = {}, []
    for cp in sorted(want & mapped):
        g = rasterise(font, cp)
        if fits(g, ascent, descent):
            glyphs[cp] = g
        else:
            outside.append(cp)
    facts = dict(file=os.path.basename(path), sha256=hashlib.sha256(data).hexdigest(), index=index,
                 family=face.name(1), version=face.name(5), copyright=face.name(0), licence_url=face.name(14),
                 ascent=ascent, descent=descent, freetype=features.version("freetype2"), pillow=PIL.__version__)
    return facts, glyphs, outside, sorted(want - mapped)


def generate(path, args):
    vlw = vlw_codepoints(args.vlw)
    extra = catalogue_chars(args.chars_from)
    beyond = sorted(wanted(args.set, vlw, extra) - wanted(args.set, vlw))  # what the catalogues add
    facts, glyphs, outside, missing = draw(path, args, wanted(args.set, vlw, extra))
    encoding = RAW if args.raw else RUNS
    store = build(glyphs, args.size, facts["ascent"], facts["descent"], encoding)
    head, back = parse(store)
    if back != glyphs:
        raise SystemExit("the store doesn't read back as it was written")
    name = f"kCjkGlyphs{args.size}"
    hangul = sum(1 for cp in glyphs if 0xAC00 <= cp <= 0xD7A3)
    han = sum(1 for cp in glyphs if is_han(cp))
    print(f"{name}: {len(glyphs)} glyphs ({han} Han, {hangul} Hangul) on {head['pages']} pages, {len(store)} bytes "
          f"(bitmaps {head['bits']}), ink {head['ink_ascent']} up / {head['ink_descent']} down")
    src = ["// SPDX-License-Identifier: OFL-1.1",
           f"// Generated by tools/glyph_store.py from {facts['file']} - do not edit.",
           f"//   font    {facts['file']}, face {facts['index']} ({facts['family']}), {facts['version']}",
           f"//   sha256  {facts['sha256']}",
           f"//   drawn   {args.size} px, 4 bits a pixel, by FreeType {facts['freetype']} (Pillow {facts['pillow']})",
           f"//   set     {args.set}, region {args.region}: {len(glyphs)} glyphs ({han} Han, {hangul} Hangul "
           f"syllables) on {head['pages']} pages",
           f"//   store   {len(store)} bytes; bitmaps {head['bits']} bytes, "
           f"{'raw' if encoding == RAW else 'as runs'}",
           f"//   em box  {facts['ascent']} px above the baseline, {facts['descent']} below; ink "
           f"{head['ink_ascent']} above, {head['ink_descent']} below; boxes up to {head['widest']} x "
           f"{head['tallest']} px"]
    text = NL.join(src) + NL
    if beyond:
        text += wrap_list("// The catalogues' (i18n/*.json) beyond the set:", [f"U+{cp:04X}" for cp in beyond])
    if outside:
        text += wrap_list("// Left out, their ink more than 2 px past the em box:", [f"U+{cp:04X}" for cp in outside])
    if missing:
        text += wrap_list("// Not in the face:", [f"U+{cp:04X}" for cp in missing])
    text += ("//" + NL +
             "// A CJK and Hangul glyph store (lib/core/GlyphStore reads it; the format is" + NL +
             "// in tools/glyph_store.py). Its glyphs are derived from the font above:" + NL +
             f"//   {facts['copyright']}" + NL +
             "// licensed under the SIL Open Font License, Version 1.1 (LICENSES/OFL-1.1.txt;" + NL +
             f"// {facts['licence_url']}). This data is a Modified Version of that Font" + NL +
             "// Software (a subset, rasterised to bitmaps): it is distributed under the" + NL +
             "// same licence, not the firmware's, and uses none of the font's Reserved" + NL +
             "// Font Names." + NL +
             f'#include "CjkGlyphs{args.size}.h"' + NL + NL +
             c_words(name, store) +
             f"const size_t {name}Size = {len(store)};" + NL +
             f"const uint64_t {name}Digest = 0x{digest(glyphs):016x}ull;" + NL)
    return text


# ---- the test fixture: a tiny store from hand-drawn glyphs, no font ----

# (code point, x, y, advance, rows): a row's characters are its values in
# hex, '.' for 0. Pages 0x30 and 0x31 meet between U+30FF and U+3100; U+3000
# is blank; U+4E00's zero and fifteen runs pass 16; U+1F600 and U+20000 are
# past the BMP.
FIXTURE = [
    (0x3000, 0, 0, 16, []),
    (0x3042, 1, 12, 16, ["..f..",
                         "fffff",
                         ".7f8.",
                         "a.f.a",
                         ".f9f."]),
    (0x30FF, 0, 10, 15, ["123456789abcdef"]),
    (0x3100, -2, 3, 0, ["f",
                        "8",
                        "1"]),
    (0x4E00, 0, 7, 16, ["..................",
                        "..................",
                        "ffffffffffffffffff",
                        "..................",
                        "1.................",
                        "..................",
                        "................2f"]),
    (0x4E01, 1, 14, 16, ["8"]),
    (0x1F600, 0, 13, 16, [".ff.",
                          "f..f",
                          ".ff."]),
    (0x20000, 2, 2, 12, ["e.e.e.e.e.e.e.e.e.e"]),
]
FIXTURE_SIZE, FIXTURE_ASCENT, FIXTURE_DESCENT = 16, 14, 2


def fixture_glyphs():
    out = {}
    for cp, x, y, adv, rows in FIXTURE:
        w = len(rows[0]) if rows else 0
        px = tuple(0 if c == "." else int(c, 16) for r in rows for c in r)
        out[cp] = (w, len(rows), x, y, adv, px)
    return out


def fixture_header():
    """test/test_glyph_store/fixture_store.h: the fixture as a store with
    runs and as a raw one, and each glyph as drawn."""
    glyphs = fixture_glyphs()
    parts = ["// SPDX-License-Identifier: GPL-3.0-or-later",
             "// Copyright (C) 2026 IrosTheBeggar",
             "// Generated by tools/glyph_store.py --fixture - do not edit.",
             "//",
             "// test_glyph_store's tiny stores, made from hand-drawn glyphs (FIXTURE in the",
             "// tool): the same glyphs with their bitmaps as runs and raw, and each glyph",
             "// as drawn, to compare the decoder's output with.",
             "#pragma once",
             "#include <cstddef>",
             "#include <cstdint>",
             "",
             "namespace fixture {",
             "",
             f"constexpr int kSize = {FIXTURE_SIZE}, kAscent = {FIXTURE_ASCENT}, kDescent = {FIXTURE_DESCENT};",
             ""]
    for label, enc in (("Runs", RUNS), ("Raw", RAW)):
        store = build(glyphs, FIXTURE_SIZE, FIXTURE_ASCENT, FIXTURE_DESCENT, enc)
        if parse(store)[1] != glyphs:
            raise SystemExit("the fixture doesn't read back")
        parts.append(c_words(f"k{label}", store).replace("const uint32_t", "constexpr uint32_t").rstrip(NL))
        parts.append(f"constexpr size_t k{label}Size = {len(store)};")
        parts.append("")
    parts += ["struct Drawn {",
              "  uint32_t cp;",
              "  int w, h, x, y, advance;",
              "  const char* rows;  // w x h values, row by row: '0'-'9', 'a'-'f'",
              "};",
              "constexpr Drawn kGlyphs[] = {"]
    for cp in sorted(glyphs):
        w, h, x, y, adv, px = glyphs[cp]
        vals = "".join(f"{v:x}" for v in px)
        parts.append(f'    {{0x{cp:X}, {w}, {h}, {x}, {y}, {adv}, "{vals}"}},')
    parts += ["};",
              f"constexpr size_t kCount = {len(glyphs)};",
              f"constexpr uint64_t kDigest = 0x{digest(glyphs):016x}ull;",
              "",
              "}  // namespace fixture",
              ""]
    return NL.join(parts)


# ---- sizes ----


def sizes(path, args):
    """Each set's store, its bitmaps as runs and raw, in bytes."""
    vlw = vlw_codepoints(args.vlw)
    sets = {name: wanted(name, vlw) for name in SETS}
    fullwidth = set(range(0xFF01, 0xFF5F))  # what pan would take with fullwidth ASCII kept
    facts, drawn, _, _ = draw(path, args, set().union(*sets.values()) | fullwidth)
    sets["pan+FF01"] = sets["pan"] | fullwidth
    print(f"{facts['family']} at {args.size} px, 4 bits a pixel (KB = 1,024 B; MB = 1,048,576 B):")
    for name, cps in sets.items():
        glyphs = {cp: drawn[cp] for cp in cps if cp in drawn}
        both = [build(glyphs, args.size, facts["ascent"], facts["descent"], enc) for enc in (RUNS, RAW)]
        bits = [parse(s)[0]["bits"] for s in both]
        print(f"  {name:9s} {len(glyphs):6,d} glyphs   runs {len(both[0]):9,d} B ({len(both[0]) / 1048576:.3f} MB)"
              f"   raw {len(both[1]):9,d} B ({len(both[1]) / 1048576:.3f} MB)   bitmaps {bits[0]:,d} / {bits[1]:,d}"
              f" B, runs save {100 - 100 * bits[0] / max(1, bits[1]):.1f} %")
    return 0


# ---- main ----


def check(path, text):
    try:
        with open(path, encoding="utf-8") as f:  # a CRLF checkout reads the same
            current = f.read()
    except OSError:
        current = None
    if current == text:
        return True
    print(f"glyph_store: out of date: {os.path.relpath(path, ROOT)}", file=sys.stderr)
    if current:
        for key in ("sha256", "drawn"):
            old = re.search(rf"^//   {key} .*$", current, re.M)
            new = re.search(rf"^//   {key} .*$", text, re.M)
            if old and new and old.group(0) != new.group(0):
                print(f"  it says: {old.group(0)[2:].strip()}{NL}  now:     {new.group(0)[2:].strip()}",
                      file=sys.stderr)
    return False


def write(path, text):
    with open(path, "w", encoding="utf-8", newline=NL) as f:
        f.write(text)
    print(f"wrote {os.path.relpath(path, ROOT)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--font", default=os.environ.get("GLYPH_STORE_FONT"),
                    help="the CJK font (TTC/OTC or OTF); default $GLYPH_STORE_FONT")
    ap.add_argument("--set", default="pan", choices=sorted(SETS))
    ap.add_argument("--region", default="JP", choices=REGIONS, help="the face, so the Han glyphs' style")
    ap.add_argument("--size", type=int, default=16, help="pixels to the em")
    ap.add_argument("--raw", action="store_true", help="bitmaps as they are, not as runs")
    ap.add_argument("--out", help="default lib/core/CjkGlyphs<size>.cpp")
    ap.add_argument("--vlw", default=os.path.join(ROOT, "src", "ui", "VlwFonts.cpp"),
                    help="the VLW fonts whose code points the store leaves to them")
    ap.add_argument("--chars-from", default=os.path.join(ROOT, "i18n"),
                    help="the UI's catalogues, whose characters the store takes too ('' for none)")
    ap.add_argument("--fixture-out", default=os.path.join(ROOT, "test", "test_glyph_store", "fixture_store.h"))
    ap.add_argument("--check", action="store_true", help="fail if a generated file isn't what this makes")
    ap.add_argument("--sizes", action="store_true", help="print each set's size; write nothing")
    ap.add_argument("--fixture", action="store_true", help="only the test fixture (no font needed)")
    args = ap.parse_args()
    out = args.out or os.path.join(ROOT, "lib", "core", f"CjkGlyphs{args.size}.cpp")
    fixture = fixture_header()
    if args.fixture:
        if args.check:
            return 0 if check(args.fixture_out, fixture) else 1
        write(args.fixture_out, fixture)
        return 0
    if not args.font:
        ap.error("--font is required (or set GLYPH_STORE_FONT)")
    if not os.path.exists(args.font):
        sys.exit(f"no font at {args.font}")
    if args.sizes:
        return sizes(args.font, args)
    text = generate(args.font, args)
    if args.check:
        ok = check(out, text)
        ok = check(args.fixture_out, fixture) and ok
        if ok:
            print("glyph_store: up to date")
        return 0 if ok else 1
    write(out, text)
    write(args.fixture_out, fixture)
    return 0


if __name__ == "__main__":
    sys.exit(main())
