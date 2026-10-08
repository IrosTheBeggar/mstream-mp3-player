# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Tests for tools/dancepack.py (the dance pack compiler), from its contract:
docs/DANCE-SPRITES.md, Appendices A-C.

    python -m unittest discover -s tools -p "test_dancepack.py"

Written from the doc, not from dancepack.py. The suite carries its own PNG
encoder, its own MPDN reader and reference versions of Appendix B's integer
algorithms, each checked first against the doc's worked examples
(HelperSelfTest), so a misreading in the compiler shows up as a difference
rather than being confirmed by the compiler itself. Throwaway dancers live
in temporary folders; the command line runs through main(argv), and through
a subprocess where the process itself matters (exit status, cwd, --check).
"""
import collections
import contextlib
import functools
import io
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from fractions import Fraction
from pathlib import Path
from unittest import mock

TOOLS = Path(__file__).resolve().parent
REPO = TOOLS.parent
DANCEPACK = TOOLS / "dancepack.py"
TEST_DANCER = "tools/art/dancers/test"
TESTPACK_H = "test/support/dance/TestPack.h"
VECTOR_HEADERS = (
    "test/support/dance/EligibilityVectors.h",
    "test/support/dance/FoldVectors.h",
    "test/support/dance/BudgetVectors.h",
)
TEXTFOLD_CPP = "lib/core/TextFold.cpp"
VLWFONTS_CPP = "src/ui/VlwFonts.cpp"

sys.path.insert(0, str(TOOLS))
try:
    import dancepack as dp  # noqa: E402
    DP_ERROR = None
except Exception as exc:  # noqa: BLE001 - reported by every test that needs it
    dp = None
    DP_ERROR = exc


# ---------------------------------------------------------------------------
# PNG: a small encoder for throwaway sheets, and a reference decoder that
# checks the encoder.

PNG_SIG = b"\x89PNG\r\n\x1a\n"
CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
VALID_DEPTHS = {0: (1, 2, 4, 8, 16), 2: (8, 16), 3: (1, 2, 4, 8), 4: (8, 16), 6: (8, 16)}
ADAM7 = ((0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2))


def png_chunk(tag, data):
    tag = tag.encode("ascii")
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def pack_samples(samples, depth):
    if depth == 8:
        return bytes(samples)
    if depth == 16:
        return b"".join(struct.pack(">H", s) for s in samples)
    out, acc, bits = bytearray(), 0, 0
    for s in samples:
        assert 0 <= s < 1 << depth, s
        acc, bits = acc << depth | s, bits + depth
        if bits == 8:
            out.append(acc)
            acc, bits = 0, 0
    if bits:
        out.append(acc << (8 - bits))
    return bytes(out)


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def filter_line(ftype, line, prev, bpp):
    if ftype == 0:
        return b"\x00" + line
    out = bytearray([ftype])
    for i, x in enumerate(line):
        a = line[i - bpp] if i >= bpp else 0
        b = prev[i] if prev else 0
        c = prev[i - bpp] if prev and i >= bpp else 0
        out.append((x - (0, a, b, (a + b) // 2, paeth(a, b, c))[ftype]) & 0xFF)
    return bytes(out)


def scanlines(rows, depth):
    def flat(px):
        return (px,) if isinstance(px, int) else tuple(px)
    return [pack_samples([s for px in row for s in flat(px)], depth) for row in rows]


def png_raw(pixels, ctype, depth, interlace=False, filters=(0,)):
    """The filtered, uncompressed image data (the inflated IDAT stream)."""
    bpp = max(1, CHANNELS[ctype] * depth // 8)
    if interlace:
        images = [[row[x0::dx] for row in pixels[y0::dy]] for x0, y0, dx, dy in ADAM7]
    else:
        images = [pixels]
    raw = bytearray()
    for image in images:
        if not image or not image[0]:
            continue
        prev = None
        for y, line in enumerate(scanlines(image, depth)):
            raw += filter_line(filters[y % len(filters)], line, prev, bpp)
            prev = line
    return bytes(raw)


def make_png(pixels, ctype=6, depth=8, palette=None, trns=None, interlace=False, filters=(0,),
             extra=(), idat_parts=1, raw=None, ihdr=None):
    """A PNG of pixels[y][x], each an int (types 0 and 3) or a tuple of samples."""
    h = len(pixels)
    w = len(pixels[0]) if h else 0
    if raw is None:
        raw = png_raw(pixels, ctype, depth, interlace, filters)
    z = zlib.compress(raw, 9)
    if ihdr is None:
        ihdr = struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, 1 if interlace else 0)
    out = PNG_SIG + png_chunk("IHDR", ihdr)
    if palette is not None:
        out += png_chunk("PLTE", bytes(c for rgb in palette for c in rgb))
    if trns is not None:
        out += png_chunk("tRNS", trns)
    for tag, data in extra:
        out += png_chunk(tag, data)
    step = max(1, -(-len(z) // idat_parts))
    for i in range(0, len(z), step):
        out += png_chunk("IDAT", z[i:i + step])
    return out + png_chunk("IEND", b"")


def ref_decode_png(data):
    """((w, h, depth, type), {tag: [bodies]}, rows of sample tuples); not interlaced."""
    assert data[:8] == PNG_SIG
    pos, chunks = 8, collections.OrderedDict()
    while pos < len(data):
        n, tag = struct.unpack_from(">I4s", data, pos)
        body = data[pos + 8:pos + 8 + n]
        assert struct.unpack_from(">I", data, pos + 8 + n)[0] == zlib.crc32(tag + body) & 0xFFFFFFFF, tag
        chunks.setdefault(tag.decode("ascii"), []).append(body)
        pos += 12 + n
    w, h, depth, ctype, comp, filt, inter = struct.unpack(">IIBBBBB", chunks["IHDR"][0])
    assert (comp, filt, inter) == (0, 0, 0)
    raw = zlib.decompress(b"".join(chunks["IDAT"]))
    nch = CHANNELS[ctype]
    bpp, stride = max(1, nch * depth // 8), (w * nch * depth + 7) // 8
    rows, prev, pos = [], bytes(stride), 0
    for _ in range(h):
        ftype, line = raw[pos], bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            c = prev[i - bpp] if i >= bpp else 0
            line[i] = (line[i] + (0, a, prev[i], (a + prev[i]) // 2, paeth(a, prev[i], c))[ftype]) & 0xFF
        prev = bytes(line)
        if depth == 16:
            s = [line[i] << 8 | line[i + 1] for i in range(0, stride, 2)]
        elif depth == 8:
            s = list(line)
        else:
            s = [b >> (8 - depth - k) & ((1 << depth) - 1) for b in line for k in range(0, 8, depth)]
        rows.append([tuple(s[x * nch:(x + 1) * nch]) for x in range(w)])
    assert pos == len(raw)
    return (w, h, depth, ctype), chunks, rows


def lcg(seed):
    x = seed
    while True:
        x = (1103515245 * x + 12345) & 0x7FFFFFFF
        yield x >> 8


# ---------------------------------------------------------------------------
# Sheets, cells and colours.

def cell_xy(name):
    """'B4' -> (1, 3): the column and row from 0."""
    m = re.match(r"([A-Za-z]+)([0-9]+)$", name)
    col = 0
    for ch in m.group(1).upper():
        col = col * 26 + ord(ch) - 64
    return col - 1, int(m.group(2)) - 1


def cell_name(col, row):
    s, n = "", col + 1
    while n:
        n, r = divmod(n - 1, 26)
        s = chr(65 + r) + s
    return s + str(row + 1)


def ref_rgb565(r, g, b):
    return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3


class Sheet:
    """cols x rows cells of cw x ch; a pixel is None (transparent) or (r, g, b[, a])."""

    def __init__(self, cols, rows, cw, ch):
        self.cols, self.rows, self.cw, self.ch = cols, rows, cw, ch
        self.px = [[None] * (cols * cw) for _ in range(rows * ch)]

    def put(self, cell, x, y, colour):
        c, r = cell_xy(cell)
        assert 0 <= x < self.cw and 0 <= y < self.ch, (cell, x, y)
        self.px[r * self.ch + y][c * self.cw + x] = colour

    def fill(self, cell, x, y, w, h, colour):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                self.put(cell, xx, yy, colour)

    def get(self, cell, x, y):
        c, r = cell_xy(cell)
        return self.px[r * self.ch + y][c * self.cw + x]

    def rgba(self):
        return [[(0, 0, 0, 0) if p is None else tuple(p) + (255,) * (4 - len(p)) for p in row]
                for row in self.px]

    def png(self, **kw):
        return make_png(self.rgba(), 6, 8, **kw)

    def keyed_png(self, key=(255, 0, 255)):
        """Colour type 2, with `key` wherever the sheet is transparent."""
        return make_png([[key if p is None else tuple(p[:3]) for p in row] for row in self.px], 2, 8)

    def indexed_png(self, depth=8):
        """Colour type 3, palette entry 0 transparent through tRNS."""
        colours = []
        for row in self.px:
            for p in row:
                if p is not None and tuple(p[:3]) not in colours:
                    colours.append(tuple(p[:3]))
        pal = [(0, 0, 0)] + colours
        rows = [[0 if p is None else pal.index(tuple(p[:3]), 1) for p in row] for row in self.px]
        return make_png(rows, 3, depth, palette=pal, trns=b"\x00")

    def grid(self, cell, index):
        """The cell's rows of palette indices; `index` maps RGB565 to an index."""
        c, r = cell_xy(cell)
        out = []
        for y in range(self.ch):
            row = bytearray()
            for p in self.px[r * self.ch + y][c * self.cw:(c + 1) * self.cw]:
                opaque = p is not None and (len(p) == 3 or p[3] >= 128)
                row.append(index[ref_rgb565(*p[:3])] if opaque else 0)
            out.append(bytes(row))
        return out


W, R, G, B, Y = (255, 255, 255), (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0)
NEAR_W = (254, 255, 255)   # RGB565 0xFFFF, as white


def figure(sheet, cell, head_x=3, head=W, belt=None, arms=None):
    """An 8x8 figure: a 3-wide head from head_x, body and red feet on the cell's
    bottom row. With head_x 3 it is symmetric about column 4, the default anchor
    (8 // 2), which is the axis A.14 and B.4 mirror about."""
    sheet.fill(cell, head_x, 0, 3, 2, head)
    sheet.fill(cell, 3, 2, 3, 4, W)
    if belt:
        sheet.fill(cell, 3, 4, 3, 1, belt)
    if arms:
        sheet.put(cell, 1, 3, arms)
        sheet.put(cell, 7, 3, arms)
    sheet.fill(cell, 2, 6, 5, 2, R)


def ref_mirror(grid, anchor_x):
    """A.14: a cell grid mirrored about the anchor column (x -> 2*anchor_x - x),
    or None when an opaque pixel would land outside the cell."""
    out = []
    for row in grid:
        new = bytearray(len(row))
        for x, v in enumerate(row):
            if v:
                m = 2 * anchor_x - x
                if not 0 <= m < len(row):
                    return None
                new[m] = v
        out.append(bytes(new))
    return out


def ref_store(frames, grid, anchor_x):
    """A.14's dedup of one cell against the frames stored so far (a list it
    appends to): (frame, mirrored)."""
    grid = [bytes(row) for row in grid]
    if grid in frames:
        return frames.index(grid), False
    mirror = ref_mirror(grid, anchor_x)
    if mirror is not None and mirror in frames:
        return frames.index(mirror), True
    frames.append(grid)
    return len(frames) - 1, False


def drawn_pixels(grid, anchor_x, mirrored):
    """What B.4 and A.10 draw of a cell grid, as {(x from the anchor, y): index}:
    a frame drawn mirrored is reflected about the anchor column."""
    return {((anchor_x - x) if mirrored else (x - anchor_x), y): v
            for y, row in enumerate(grid) for x, v in enumerate(row) if v}


def base_sheet():
    """4x3 cells of 8x8, anchor 4,8 by default. Row 1: stand (symmetric about the
    anchor column), lean, the lean's mirror image about the anchor column, a copy
    of A1. Row 2: arms (near-white, merging with white), a green belt, a blue
    head, a yellow head. Row 3 is blank."""
    s = Sheet(4, 3, 8, 8)
    figure(s, "A1")
    figure(s, "B1", head_x=4)
    figure(s, "C1", head_x=2)
    figure(s, "D1")
    figure(s, "A2", arms=NEAR_W)
    figure(s, "B2", belt=G)
    figure(s, "C2", head=B)
    figure(s, "D2", head=Y)
    return s


@functools.lru_cache(maxsize=None)
def base_png():
    return base_sheet().png()


# ---------------------------------------------------------------------------
# MPDN: an independent reader (Appendix A) and writer for hand-made packs.

TAGS = ("META", "GEOM", "PALS", "THMB", "FTAB", "STRS", "MOVS", "STEP", "FDAT")
RStep = collections.namedtuple("RStep", "frame mirrored dur dx dy ease go")
RGeom = collections.namedtuple("RGeom", "scale faces cell_w cell_h anchor_x anchor_y shadow_w frame_count rest_frame")
MOVS_RECORD = "<HBBBBBBBBBbBBBBBBBBHBBHBBI"
MOVS_FIELDS = ("name", "kind", "beats", "pace_lo", "pace_hi", "weight10", "when", "flags", "repeat", "needs",
               "travel", "start_at", "travel_ease", "bounce", "sway", "from_key", "to_key", "from_key_m",
               "to_key_m", "genre_first", "genre_count", "step_count", "step_first", "song_lo", "song_hi",
               "reserved")
RMove = collections.namedtuple("RMove", MOVS_FIELDS + ("name_text", "genres", "steps"))
# A hand-built move for the reference algorithms (dancepack.Move uses the same names).
M = collections.namedtuple("M", "kind beats pace_lo pace_hi flags travel start_at travel_ease bounce sway steps")


def read_cstr(buf, pos):
    n = buf[pos]
    raw = bytes(buf[pos + 1:pos + 1 + n])
    assert buf[pos + 1 + n] == 0 and 0 not in raw, "string at %d" % pos
    return raw.decode("utf-8"), pos + n + 2


def ref_box(grid):
    """The tightest box around a grid's non-zero indices, or (0, 0, 0, 0)."""
    ys = [y for y, row in enumerate(grid) if any(row)]
    if not ys:
        return (0, 0, 0, 0)
    xs = [x for row in grid for x, v in enumerate(row) if v]
    return (min(xs), ys[0], max(xs) - min(xs) + 1, ys[-1] - ys[0] + 1)


def ref_encode_frame(grid, box):
    """A.10's canonical rows."""
    x0, y0, w, h = box
    out = bytearray()
    for y in range(y0, y0 + h):
        row = grid[y][x0:x0 + w]
        opaque = [i for i, v in enumerate(row) if v]
        if not opaque:
            out += b"\x00\x00"
            continue
        i, last, tokens = opaque[0], opaque[-1], []
        while i <= last:
            j = i
            while j <= last and row[j] == row[i]:
                j += 1
            run = j - i
            while run:
                n = min(16, run)
                tokens.append((n - 1) << 4 | row[i])
                run -= n
            i = j
        out += bytes([opaque[0], len(tokens)]) + bytes(tokens)
    return bytes(out)


def ref_thumbnail(grid, box):
    """A.8: 800 bytes, 40 rows of 20, the left pixel in the high nibble."""
    x0, y0, w, h = box
    px = [[0] * 40 for _ in range(40)]
    big = max(w, h)
    if big <= 40:
        m = 40 // big
        tw, th = w * m, h * m

        def value(i, j):
            return grid[y0 + j // m][x0 + i // m]
    else:
        s = (big + 39) // 40
        tw, th = (w + s - 1) // s, (h + s - 1) // s

        def value(i, j):
            counts = collections.Counter(
                grid[y0 + y][x0 + x]
                for y in range(j * s, min(j * s + s, h)) for x in range(i * s, min(i * s + s, w)))
            counts.pop(0, None)
            if not counts:
                return 0
            top = max(counts.values())
            return min(k for k, v in counts.items() if v == top)
    ox, oy = (40 - tw) // 2, (40 - th) // 2
    for j in range(th):
        for i in range(tw):
            px[oy + j][ox + i] = value(i, j)
    return bytes(row[2 * k] << 4 | row[2 * k + 1] for row in px for k in range(20))


def ref_fnv(data):
    h = 0x811C9DC5
    for byte in data:
        h = (h ^ byte) * 0x01000193 & 0xFFFFFFFF
    return h


def ref_frame_hash(grid):
    return ref_fnv(b"".join(bytes(row) for row in grid))


class Mpdn:
    """An MPDN v1 pack read as Appendix A says, independently of dancepack.

    canonical=True also checks what the compiler must write (A.4, A.10, A.14):
    the chunk order, the padding, frame data back to back in canonical rows."""

    def __init__(self, data, canonical=True):
        self.data = data = bytes(data)
        assert len(data) >= 32
        (magic, self.format, self.min_reader, self.size, self.crc, self.count,
         self.flags) = struct.unpack_from("<4sHHIIHH", data, 0)
        assert magic == b"MPDN", magic
        assert self.size == len(data), (self.size, len(data))
        assert self.crc == zlib.crc32(data[16:]) & 0xFFFFFFFF, "CRC"
        self.dir = []
        for i in range(self.count):
            tag, off, size = struct.unpack_from("<4sII", data, 32 + 12 * i)
            self.dir.append((tag.decode("ascii"), off, size))
        if canonical:
            assert (self.format, self.min_reader, self.flags) == (1, 1, 0)
            assert data[20:32] == bytes(12)
            assert tuple(t for t, _, _ in self.dir) == TAGS, self.dir
            pos = 32 + 12 * self.count
            for tag, off, size in self.dir:
                assert off == (pos + 3) & ~3, (tag, off, pos)
                assert data[pos:off] == bytes(off - pos), tag + " padding"
                pos = off + size
            assert len(data) == (pos + 3) & ~3 and data[pos:] == bytes(len(data) - pos), "end padding"
        self.chunk = {tag: data[off:off + size] for tag, off, size in self.dir}
        self._meta()
        self._geom()
        pals = self.chunk["PALS"]
        assert len(pals) == 96
        self.palette = [tuple(pals[3 * i:3 * i + 3]) for i in range(16)]
        self.idle_palette = [tuple(pals[48 + 3 * i:48 + 3 * i + 3]) for i in range(16)]
        self.thumb = self.chunk["THMB"]
        assert len(self.thumb) == 800
        self._frames(canonical)
        self._strs()
        self._moves()

    def _meta(self):
        buf, pos, out = self.chunk["META"], 0, []
        for _ in range(6):
            s, pos = read_cstr(buf, pos)
            out.append(s)
        self.id, self.name, self.author, self.licence, self.url, self.version = out
        self.declared_moves = buf[pos]

    def _geom(self):
        g = self.chunk["GEOM"]
        assert len(g) == 12
        v = struct.unpack("<BBBBBBBBHH", g)
        assert v[7] == 0
        self.geom = RGeom(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[8], v[9])

    def _frames(self, canonical):
        ftab, fdat, g = self.chunk["FTAB"], self.chunk["FDAT"], self.geom
        assert len(ftab) == 12 * g.frame_count
        self.boxes, self.spans, self.grids = [], [], []
        running = 0
        for i in range(g.frame_count):
            x0, y0, w, h, off, ln, res = struct.unpack_from("<BBBBIHH", ftab, 12 * i)
            assert res == 0
            box = (x0, y0, w, h)
            self.boxes.append(box)
            self.spans.append((off, ln))
            grid = [bytearray(g.cell_w) for _ in range(g.cell_h)]
            p = off
            for r in range(h):
                skip, n = fdat[p], fdat[p + 1]
                p += 2
                x = skip
                for _ in range(n):
                    tok = fdat[p]
                    p += 1
                    run, idx = (tok >> 4) + 1, tok & 15
                    assert x + run <= w, "frame %d row %d" % (i, r)
                    for k in range(run):
                        grid[y0 + r][x0 + x + k] = idx
                    x += run
            assert p == off + ln, "frame %d length" % i
            grid = [bytes(row) for row in grid]
            self.grids.append(grid)
            if canonical:
                assert box == ref_box(grid), "frame %d box" % i
                if w == 0:
                    assert (off, ln) == (0, 0)
                else:
                    assert off == running, "frame %d offset" % i
                    assert fdat[off:off + ln] == ref_encode_frame(grid, box), "frame %d rows" % i
                    running += ln
        if canonical:
            assert len(fdat) == running

    def frame_data(self, i):
        off, ln = self.spans[i]
        return self.chunk["FDAT"][off:off + ln]

    def _strs(self):
        buf = self.chunk["STRS"]
        self.pose_count = buf[0]
        assert buf[1:4] == b"\x00\x00\x00"
        self.strings, pos = collections.OrderedDict(), 4
        while pos < len(buf):
            s, nxt = read_cstr(buf, pos)
            self.strings[pos] = s
            pos = nxt
        assert pos == len(buf)
        self.pose_keys = list(self.strings.values())[:self.pose_count]

    def string_run(self, off, count):
        out, pos = [], off
        for _ in range(count):
            s, pos = read_cstr(self.chunk["STRS"], pos)
            out.append(s)
        return out

    def _moves(self):
        buf, step = self.chunk["MOVS"], self.chunk["STEP"]
        count, rec_len = buf[0], buf[1]
        assert buf[2:4] == b"\x00\x00" and rec_len == 32
        assert len(buf) == 4 + count * rec_len
        assert len(step) % 8 == 0
        self.steps = [RStep(f & 0x7FFF, bool(f & 0x8000), d, dx, dy, e, bool(fl & 1))
                      for f, d, dx, dy, e, fl in struct.iter_unpack("<HHbbBB", step)]
        self.raw_steps = list(struct.iter_unpack("<HHbbBB", step))
        self.moves = []
        for i in range(count):
            v = struct.unpack_from(MOVS_RECORD, buf, 4 + rec_len * i)
            rec = dict(zip(MOVS_FIELDS, v))
            name, _ = read_cstr(self.chunk["STRS"], rec["name"])
            genres = self.string_run(rec["genre_first"], rec["genre_count"]) if rec["genre_count"] else []
            steps = tuple(self.steps[rec["step_first"]:rec["step_first"] + rec["step_count"]])
            self.moves.append(RMove(*(v + (name, genres, steps))))

    def move(self, name):
        return next(m for m in self.moves if m.name_text == name)


def with_crc(data):
    data = bytearray(data)
    struct.pack_into("<I", data, 12, zlib.crc32(bytes(data[16:])) & 0xFFFFFFFF)
    return bytes(data)


def patch(data, offset, fmt, *values):
    """A copy with struct values written at offset and the CRC recomputed."""
    d = bytearray(data)
    struct.pack_into(fmt, d, offset, *values)
    return with_crc(d)


def assemble_mpdn(chunks):
    """Header and directory (A.2-A.4) around [(tag, bytes)], in that order."""
    pos = 32 + 12 * len(chunks)
    body, entries = bytearray(), []
    for tag, blob in chunks:
        off = (pos + 3) & ~3
        body += bytes(off - pos) + blob
        entries.append(struct.pack("<4sII", tag.encode("ascii"), off, len(blob)))
        pos = off + len(blob)
    end = (pos + 3) & ~3
    body += bytes(end - pos)
    tail = struct.pack("<HH", len(chunks), 0) + bytes(12) + b"".join(entries) + bytes(body)
    return b"MPDN" + struct.pack("<HHII", 1, 1, end, zlib.crc32(tail) & 0xFFFFFFFF) + tail


A17_INI = """[dancer]
name    = Dot
author  = Ana
license = CC0-1.0
cell    = 4x4

[move hop]
frames = A1
beats  = 1
bpm    = any
bounce = 2
"""

A17_DUMP = """
0000: 4d 50 44 4e 01 00 01 00 84 04 00 00 85 e2 f1 5b
0010: 09 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0020: 4d 45 54 41 8c 00 00 00 1d 00 00 00 47 45 4f 4d
0030: ac 00 00 00 0c 00 00 00 50 41 4c 53 b8 00 00 00
0040: 60 00 00 00 54 48 4d 42 18 01 00 00 20 03 00 00
0050: 46 54 41 42 38 04 00 00 0c 00 00 00 53 54 52 53
0060: 44 04 00 00 09 00 00 00 4d 4f 56 53 50 04 00 00
0070: 24 00 00 00 53 54 45 50 74 04 00 00 08 00 00 00
0080: 46 44 41 54 7c 04 00 00 06 00 00 00
"""

# Offsets inside A.17's pack (A.17's table): MOVS's first record and STEP.
A17_MOV = 1108
A17_STEP = 1140
A17_FTAB = 1080
A17_FDAT = 1148
A17_STRS = 1092


def parse_dump(text):
    return bytes.fromhex("".join(line.split(":", 1)[1] for line in text.strip().splitlines()))


@functools.lru_cache(maxsize=None)
def a17_pack():
    """A.17's pack, built from the doc's chunk table (not by dancepack)."""
    h = bytes.fromhex
    pals = bytearray(96)
    pals[3:6] = h("ff0000")
    pals[51:54] = h("741b1b")
    return assemble_mpdn([
        ("META", h("03 64 6f 74 00 03 44 6f 74 00 03 41 6e 61 00 07 43 43 30 2d 31 2e 30 00 00 00 00 00 01")),
        ("GEOM", h("01 00 04 04 02 04 00 00 01 00 00 00")),
        ("PALS", bytes(pals)),
        ("THMB", b"\x11" * 800),
        ("FTAB", h("01 02 02 02 00 00 00 00 06 00 00 00")),
        ("STRS", h("00 00 00 00 03 68 6f 70 00")),
        ("MOVS", h("01 20 00 00") + h("04 00 00 01 00 00 0a 00 00 00 00 00 00 00 02 00"
                                      "00 00 00 00 00 00 00 01 00 00 00 00 00 00 00 00")),
        ("STEP", h("00 00 0c 00 00 00 00 00")),
        ("FDAT", h("00 01 11 00 01 11")),
    ])


def a17_sheet():
    s = Sheet(1, 1, 4, 4)
    s.fill("A1", 1, 2, 2, 2, R)
    return s


# ---------------------------------------------------------------------------
# Appendix B, written again from the doc: the reference the tests compare with.

SPOT_X = (64, 160, 256)
FPS_PERIOD = ((24, 42), (30, 33))
START_AT_SPOTS = {0: (-1, 0, 1), 1: (0,), 2: (-1, 1), 3: (-1,), 4: (1,)}


def ref_rhu(a, b):
    return (2 * a + b) // (2 * b)


def ref_ease(code, f):
    if code == 0:
        return 65536 if f == 65536 else 0      # B.2: hold reaches 65536 only at f = 65536
    if code == 1:
        return f
    if code == 2:
        return f * f >> 16
    if code == 3:
        return 65536 - ((65536 - f) * (65536 - f) >> 16)
    return f * f * (196608 - 2 * f) >> 32


def ref_hop(phi):
    if phi < 7864:
        return 0
    a = (phi - 7864) * 65536 // 57672
    return 4 * a * (65536 - a) >> 16


def ref_sway(b, phi):
    return (-1 if b % 2 == 0 else 1) * (65536 - 2 * ref_ease(4, phi))


def ref_idle_colour(rgb):
    r, g, b = rgb
    y = (77 * r + 150 * g + 29 * b + 128) >> 8
    return tuple((7 * ((c + y + 1) >> 1) + 5) // 10 for c in rgb)


def ref_near_background(rgb):
    return abs(rgb[0] - 8) < 16 and abs(rgb[1] - 12) < 16 and abs(rgb[2] - 16) < 16


def ref_beats_text(t):
    w, r = divmod(t, 12)
    if t == 12:
        return "1 beat"
    if r == 0:
        return "%d beats" % w
    fr = Fraction(r, 12)
    if w == 0:
        return "%d/%d beat" % (fr.numerator, fr.denominator)
    return "%d %d/%d beats" % (w, fr.numerator, fr.denominator)


def ref_eligible_k(t10, lo, hi, prev_k, beats, travel=0):
    if lo == 0 and hi == 0:
        return 0
    for k in ([prev_k] if prev_k is not None else []) + [0, -1, 1]:
        if k == -1 and beats > 4:
            continue
        if k == 1 and t10 > 900:
            continue
        if travel and k == 1 and beats < 4:      # section 8's D = beats * 2^-k >= 2
            continue
        x = t10 * 2 ** (k + 1) if k >= 0 else t10
        if 20 * lo <= x <= 20 * hi:
            return k
    return None


def ref_pace_holds(t10, k, lo, hi):
    if lo == 0 and hi == 0:
        return True
    if k == 1 and t10 > 900:              # B.7: the 3 % never takes a pace past 180
        return False
    x = t10 * 2 ** (k + 1) if k >= 0 else t10
    return 1940 * lo <= 100 * x <= 2060 * hi


def ref_reach(lo, hi, beats, travel=0):
    ranged = not (lo == 0 and hi == 0)
    octaves = []
    if not (travel and beats < 2):
        octaves.append(0)
    if ranged and beats <= 4:
        octaves.append(-1)
    if ranged and (not travel or beats >= 4):
        octaves.append(1)
    if not octaves:                       # an unranged travel move under 2 beats (B.7)
        return None
    bands = {-1: (36, 88), 0: (72, 176), 1: (144, 180)}
    a = min(bands[k][0] for k in octaves)
    b = max(bands[k][1] for k in octaves)
    if not ranged:
        return (a, b)
    return (max(lo, a), min(hi, b)) if max(lo, a) <= min(hi, b) else None


def ref_area(r):
    return 0 if r is None else max(0, r[2] - r[0]) * max(0, r[3] - r[1])


def ref_union(a, b):
    if a is None:
        return b
    if b is None:
        return a
    return (min(a[0], b[0]), min(a[1], b[1]), max(a[2], b[2]), max(a[3], b[3]))


def ref_dirty(prev, cur):
    if prev is None:
        return ref_area(cur)
    u = ref_union(prev, cur)
    if 10 * ref_area(u) > 13 * (ref_area(prev) + ref_area(cur)):
        return ref_area(prev) + ref_area(cur)
    return ref_area(u)


def ref_frame_at(g, boxes, mv, pos16, mirrored, spot, e16=65536):
    """B.4: (frame, drawn mirrored, rootX, rootY, box, shadow, rect); rects or None."""
    assert pos16 >= 0, "B.4's precondition"
    steps = mv.steps
    n = len(steps)
    starts, t = [], 0
    for s in steps:
        starts.append(t)
        t += s.dur
    total = t
    loop = pos16 // (total * 65536)
    q = pos16 - loop * total * 65536
    i = max(k for k in range(n) if starts[k] * 65536 <= q)
    st, nx = steps[i], steps[(i + 1) % n]
    f = (q - starts[i] * 65536) // st.dur
    drawn = bool(st.mirrored) != bool(mirrored)
    e = ref_ease(st.ease, f)
    kx = st.dx * 65536 + (nx.dx - st.dx) * e
    ky = st.dy * 65536 + (nx.dy - st.dy) * e
    if mv.kind == 0:
        b, phi = pos16 // 786432, (pos16 % 786432) // 12
        by, sx = -mv.bounce * ref_hop(phi), mv.sway * ref_sway(b, phi)
    else:
        by = sx = 0
    sgn = -1 if mirrored else 1
    x16, y16 = sgn * (kx + sx), ky + by
    if mv.flags & 32:
        X, Y = g.scale * ref_rhu(e16 * x16, 1 << 32), g.scale * ref_rhu(e16 * y16, 1 << 32)
    else:
        X, Y = ref_rhu(g.scale * e16 * x16, 1 << 32), ref_rhu(g.scale * e16 * y16, 1 << 32)
    start_x, travel_px = SPOT_X[spot + 1], 0
    if mv.kind == 0 and mv.travel:
        end_x = SPOT_X[spot + mv.travel * sgn + 1]
        go = [k for k in range(n) if steps[k].go] or list(range(n))
        gt = sum(steps[k].dur for k in go)
        if loop >= 1:
            p = 65536
        else:
            gq = sum(steps[k].dur * 65536 for k in go if k < i)
            if i in go:
                gq += q - starts[i] * 65536
            p = gq // gt
        travel_px = ref_rhu((end_x - start_x) * ref_ease(mv.travel_ease, p), 65536)
    rx, ry = start_x + travel_px + X, 153 + Y
    x0, y0, w, h = boxes[st.frame]
    box = None
    if w > 0:
        left = rx + ((g.anchor_x - x0 - w + 1) if drawn else (x0 - g.anchor_x)) * g.scale
        top = ry + (y0 - g.anchor_y) * g.scale
        right, bottom = left + w * g.scale, top + h * g.scale
        cx = -left if left < 0 else (320 - right if right > 320 else 0)
        cy = -top if top < 0 else (164 - bottom if bottom > 164 else 0)
        rx, ry = rx + cx, ry + cy
        box = (left + cx, top + cy, right + cx, bottom + cy)
    shadow = None
    sw = g.shadow_w - 2 * max(0, -Y)
    if sw > 0:
        sx0 = rx - sw // 2
        sx0, sx1 = max(sx0, 0), min(sx0 + sw, 320)
        if sx1 > sx0:
            shadow = (sx0, 152, sx1, 155)
    return (st.frame, drawn, rx, ry, box, shadow, ref_union(box, shadow))


def ref_extent(g, boxes, mv, mirrored):
    sgn = -1 if mirrored else 1
    los, his = [], []
    n = len(mv.steps)
    for i, st in enumerate(mv.steps):
        x0, y0, w, h = boxes[st.frame]
        if w == 0:
            continue
        m = bool(st.mirrored) != bool(mirrored)
        left = ((g.anchor_x - x0 - w + 1) if m else (x0 - g.anchor_x)) * g.scale
        right = left + w * g.scale
        a, c = sgn * st.dx * g.scale, sgn * mv.steps[(i + 1) % n].dx * g.scale
        los.append(left + min(a, c, 0) - mv.sway * g.scale)
        his.append(right + max(a, c, 0) + mv.sway * g.scale)
    return (min(los), max(his)) if los else None


def ref_fits(g, boxes, mv, mirrored, spot):
    e = ref_extent(g, boxes, mv, mirrored)
    return e is None or (SPOT_X[spot + 1] + e[0] >= 0 and SPOT_X[spot + 1] + e[1] <= 320)


def ref_start_spots(g, boxes, mv, mirrored):
    t = mv.travel * (-1 if mirrored else 1)
    allowed = START_AT_SPOTS[mv.start_at]
    if mirrored and mv.start_at in (3, 4):
        allowed = tuple(-s for s in allowed)
    return [s for s in (-1, 0, 1)
            if -1 <= s + t <= 1 and s in allowed
            and ref_fits(g, boxes, mv, mirrored, s) and ref_fits(g, boxes, mv, mirrored, s + t)]


def ref_pos16(mv, n, period, pace):
    return n * period * pace * 65536 // 5000 if mv.kind == 0 else n * period * 65536 // 10


def ref_trace(g, boxes, mv, fps, pace, mirrored, spot):
    """B.6's one case: [(n, pos16, pose, dirty or None)] for n = 0..N."""
    period = dict(FPS_PERIOD)[fps]
    total = sum(s.dur for s in mv.steps) * 65536
    last = 1
    while ref_pos16(mv, last, period, pace) < total:
        last += 1
    out, prev = [], None
    for n in range(last + 1):
        pos = ref_pos16(mv, n, period, pace)
        pose = ref_frame_at(g, boxes, mv, pos, mirrored, spot)
        out.append((n, pos, pose, None if n == 0 else ref_dirty(prev, pose[6])))
        prev = pose[6]
    return out


def ref_paces(mv):
    if mv.kind == 1:
        return [0]
    r = ref_reach(mv.pace_lo, mv.pace_hi, mv.beats, mv.travel)
    return sorted({r[0], (r[0] + r[1]) // 2, r[1]})


def ref_case_spots(g, boxes, mv, mirrored):
    if mv.kind == 0 and mv.travel:
        return ref_start_spots(g, boxes, mv, mirrored)
    return [0]


def ref_budget(g, boxes, mv):
    """B.6: ([(fps, pace, mirrored, spot, N, median, peak)], median, peak, light)."""
    cases = []
    for fps, _ in FPS_PERIOD:
        for pace in ref_paces(mv):
            for mirrored in ((False, True) if mv.flags & 1 else (False,)):
                for spot in ref_case_spots(g, boxes, mv, mirrored):
                    tr = ref_trace(g, boxes, mv, fps, pace, mirrored, spot)
                    ds = sorted(d for _, _, _, d in tr[1:])
                    cases.append((fps, pace, mirrored, spot, len(ds), ds[(len(ds) - 1) // 2], ds[-1]))
    peak = max(c[6] for c in cases)
    light = "green" if peak <= 16000 else "amber" if peak <= 20000 else "red"
    return cases, max(c[5] for c in cases), peak, light


def ref_fold(tag, table):
    s = "".join(ch if ord(ch) < 0x80 else table.get(ord(ch), "?") for ch in tag)
    s = s.lower().replace("&", " and ").replace("+", " and ")
    words = re.findall(r"[a-z0-9]+", s)
    return "".join("and" if w == "n" and 0 < i < len(words) - 1 else w for i, w in enumerate(words))


def c_unescape(s):
    return re.sub(r'\\(0|"|\\)', lambda m: {"0": "\0", '"': '"', "\\": "\\"}[m.group(1)], s)


def ref_textfold_table(src):
    """C.2's TEXTFOLD, rebuilt from TextFold.cpp's text."""
    def switch(fn):
        body = re.search(r"const char\* " + fn + r"\(uint32_t cp\) \{(.*?)\n\}", src, re.S).group(1)
        out, pending = {}, []
        for m in re.finditer(r'case (0x[0-9A-Fa-f]+):|return "((?:[^"\\]|\\.)*)";|return nullptr;', body):
            if m.group(1):
                pending.append(int(m.group(1), 16))
            else:
                if m.group(2) is not None:
                    for cp in pending:
                        out.setdefault(cp, c_unescape(m.group(2)))
                pending = []
        return out

    def literal(name):
        i = src.index(name + "[")
        text = re.sub(r"//[^\n]*", "", src[i:src.index(";", i)])
        return "".join(c_unescape(s) for s in re.findall(r'"((?:[^"\\]|\\.)*)"', text))

    table = {}
    for fn in ("punctuation", "latin1Multi", "symbols"):
        for cp, rep in switch(fn).items():
            table.setdefault(cp, rep)
    for base, chars in ((0xC0, literal("kLatin1")), (0x100, literal("kLatinExtA"))):
        for i, c in enumerate(chars):
            if c != "\0":
                table.setdefault(base + i, c if c.isalpha() and ord(c) < 0x80 else "?")
    return {cp: rep for cp, rep in table.items() if cp >= 0x80 and rep != "?"}


def ref_vlw_code_points(src):
    sets = []
    for m in re.finditer(r"const uint8_t kVlw\w*\[(\d+)\]\s*=\s*\{(.*?)\};", src, re.S):
        vals = bytes(int(t, 16) if t[:2] in ("0x", "0X") else int(t)
                     for t in re.findall(r"0[xX][0-9a-fA-F]+|\d+", m.group(2)))
        assert len(vals) == int(m.group(1))
        n = struct.unpack_from(">I", vals, 0)[0]
        sets.append({struct.unpack_from(">I", vals, 24 + 28 * i)[0] for i in range(n)})
    return set.intersection(*sets)


def to_ranges(points):
    out = []
    for cp in sorted(points):
        if out and out[-1][1] == cp - 1:
            out[-1][1] = cp
        else:
            out.append([cp, cp])
    return [tuple(r) for r in out]


def vlw_array(name, code_points):
    """A VLW-shaped C array: the glyph count, then a 28-byte record per glyph."""
    blob = bytearray(struct.pack(">IIIIII", len(code_points), 11, 16, 0, 15, 4))
    for cp in code_points:
        blob += struct.pack(">IIIIiII", cp, 8, 6, 7, 8, 0, 0)
    lines = [", ".join("0x%02x" % b for b in blob[i:i + 20]) for i in range(0, len(blob), 20)]
    return "const uint8_t %s[%d] = {\n    %s,\n};\n" % (name, len(blob), ",\n    ".join(lines))


@functools.lru_cache(maxsize=None)
def repo_text(rel):
    return (REPO / rel).read_text(encoding="utf-8")


@functools.lru_cache(maxsize=None)
def ref_textfold():
    return ref_textfold_table(repo_text(TEXTFOLD_CPP))


@functools.lru_cache(maxsize=None)
def ref_font_coverage():
    return frozenset(ref_vlw_code_points(repo_text(VLWFONTS_CPP)))


# ---------------------------------------------------------------------------
# The doc's tables.

# B.2 at f = 0, 1/4, 1/2, 3/4, 1.
EASE_TABLE = {
    0: (0, 0, 0, 0, 65536),
    1: (0, 16384, 32768, 49152, 65536),
    2: (0, 4096, 16384, 36864, 65536),
    3: (0, 28672, 49152, 61440, 65536),
    4: (0, 10240, 32768, 55296, 65536),
}
EASE_F = (0, 16384, 32768, 49152, 65536)
HOP_TABLE = ((0, 0), (7863, 0), (7864, 0), (7865, 3), (7866, 7), (16384, 33003), (32768, 64317),
             (36700, 65536), (49152, 53317), (65535, 7))
SWAY_TABLE = (((0, 0), -65536), ((0, 32768), 0), ((0, 65535), 65534), ((1, 0), 65536))

# B.7: (T10, lo, hi, prevK, beats) -> k.
ELIGIBLE_TABLE = (
    ((1200, 90, 130, None, 4), 0),
    ((870, 150, 180, None, 1), 1),
    ((870, 150, 180, None, 8), 1),
    ((1740, 36, 60, None, 4), None),
    ((1200, 36, 200, 1, 2), 0),
    ((1200, 36, 200, -1, 2), -1),
    ((950, 170, 180, None, 2), None),
    ((900, 170, 180, None, 2), 1),
    ((1760, 0, 0, 1, 2), 0),
    ((1200, 60, 100, None, 4), -1),
    ((1200, 60, 100, None, 8), None),
    # (T10, lo, hi, prevK, beats, travel): a travel move needs D = beats * 2^-k >= 2
    ((800, 72, 180, 1, 2, 1), 0),       # prevK +1 would give D = 1; 0 fits
    ((720, 100, 150, None, 2, 1), None),  # only double time fits, and 2 beats can't travel there
    ((720, 100, 150, None, 4, 1), 1),   # 4 beats: D = 2 at +1
)
# More cases worked out by hand at the edges.
ELIGIBLE_EDGES = (
    ((720, 36, 60, None, 4), -1),      # 36 exactly, half time
    ((880, 88, 88, None, 1), 0),       # x(0) = 1760 = 20*88
    ((870, 88, 88, None, 1), None),    # 87, 43.5 and 174 all miss 88
    ((901, 170, 180, None, 2), None),  # double time needs T10 <= 900
    ((870, 36, 200, 1, 2), 1),         # the previous k fits first: 174
    ((1200, 36, 200, -1, 8), 0),       # prevK -1 can't apply to 8 beats; then 0
    ((1200, 36, 200, 0, 2), 0),
    ((1200, 0, 0, -1, 8), 0),          # unranged: always 0
    ((1760, 177, 200, None, 1), None),
    ((1760, 88, 88, None, 4), -1),     # 176 / 2
    ((1760, 88, 88, None, 8), None),
)
REACH_TABLE = (
    ((0, 0, 1, 0), (72, 176)),
    ((0, 0, 8, 0), (72, 176)),
    ((0, 0, 1, 1), None),               # no octave allowed: A.15 drops it as travel 1 instead
    ((40, 80, 1, 1), (40, 80)),         # ranged, travel under 2 beats: half time only (36-88)
    ((90, 130, 4, 0), (90, 130)),
    ((36, 60, 4, 0), (36, 60)),
    ((36, 60, 8, 0), None),
    ((185, 200, 2, 0), None),
    ((178, 200, 2, 1), None),
    ((100, 200, 4, 1), (100, 180)),
    ((40, 200, 8, 0), (72, 180)),
)
# (T10, k, lo, hi) -> pace_holds, by hand from 1940*lo <= 100*x(k) <= 2060*hi.
PACE_HOLDS_TABLE = (
    ((1200, 0, 90, 130), True),
    ((1200, 0, 123, 130), True),     # 120 >= 0.97 * 123 = 119.31
    ((1200, 0, 124, 130), False),    # 120 < 0.97 * 124 = 120.28
    ((1200, 0, 60, 117), True),      # 120 <= 1.03 * 117 = 120.51
    ((1200, 0, 60, 116), False),     # 120 > 1.03 * 116 = 119.48
    ((1200, -1, 60, 100), True),     # 60
    ((900, 1, 170, 180), True),      # 180
    ((1200, 1, 0, 0), True),         # unranged
    ((970, 0, 100, 100), True),      # 97 = 0.97 * 100 exactly: the bounds are inclusive
    ((969, 0, 100, 100), False),
    ((1030, 0, 100, 100), True),     # 103 = 1.03 * 100 exactly
    ((1031, 0, 100, 100), False),
    # double time holds only while T10 <= 900, as eligible_k: never past 180
    ((901, 1, 170, 180), False),     # 180.2, inside 1.03 * 180 but past 180
    ((927, 1, 170, 180), False),     # 185.4 = 1.03 * 180 exactly
    ((900, 1, 177, 200), True),      # 180
    ((1030, 1, 177, 200), False),    # 206 = 1.03 * 200, the old tolerance's top
    ((901, 0, 170, 180), False),     # 90.1 at k = 0: below 0.97 * 170
)

FOLD_TABLE = (
    ("Hip Hop", "hiphop", 0x5DE39FA5),
    ("hip-hop", "hiphop", 0x5DE39FA5),
    ("HIP_HOP", "hiphop", 0x5DE39FA5),
    ("Drum & Bass", "drumandbass", 0xE6318A55),
    ("drum n bass", "drumandbass", 0xE6318A55),
    ("Drum ’n’ Bass", "drumandbass", 0xE6318A55),
    ("Drum&n&Bass", "drumandandandbass", 0xEBEFBAA3),   # & is " and ", spaces included: n stands alone
    ("R&B", "randb", 0x1BDE7F5C),
    ("r+b", "randb", 0x1BDE7F5C),
    ("d&b", "dandb", 0xDD173CB2),
    ("rock'n'roll", "rockandroll", 0x4666B1EE),
    ("n", "n", 0xEB0C3431),
    ("a n", "an", 0x4124F2E6),
    ("n n n", "nandn", 0x66E2517C),
    ("&", "and", 0x0F29C2A6),
    ("C++", "candand", 0xF49E7244),
    ("Électro", "electro", 0xEED54F8F),
    ("Straße", "strasse", 0x0581A214),
    ("Æther", "aether", 0x886A2654),
    ("Œuvre", "oeuvre", 0xA67EB213),
    ("Łódź", "lodz", 0xD5FC07C6),
    ("Ñandú", "nandu", 0x61E2499D),
    ("½ step", "12step", 0x8E61F028),
    ("©", "c", 0xE60C2C52),
    ("Synth–Pop", "synthpop", 0x87CDEC94),
    ("K-Pop", "kpop", 0x9076A259),
    ("90s", "90s", 0xA0CA74AD),
    ("Música Popular Brasileira", "musicapopularbrasileira", 0xC9B7E57E),
    ("日本", "", None),
    ("  ", "", None),
    ("", "", None),
)

ELIG_T10 = (720, 800, 870, 880, 900, 901, 957, 1200, 1600, 1740, 1760)
ELIG_RANGES = ((0, 0), (36, 60), (36, 200), (60, 100), (88, 88), (90, 130), (100, 160), (110, 180),
               (144, 180), (150, 200), (170, 180), (177, 200))
ELIG_PREV = (None, -1, 0, 1)
ELIG_BEATS = (1, 2, 4, 8)
ELIG_TRAVEL = (0, 1)        # travel 1 only with 2 beats or more (C.8)
PACE_T10 = (720, 800, 870, 880, 900, 901, 957, 969, 970, 1030, 1031, 1200, 1600, 1740, 1760)
PACE_RANGES = ELIG_RANGES + ((100, 100),)


def elig_grid():
    """C.8's eligibility grid, in its nested order: (T10, lo, hi, prevK, beats, travel)."""
    return [(t10, lo, hi, prev, beats, travel) for t10 in ELIG_T10 for lo, hi in ELIG_RANGES for prev in ELIG_PREV
            for beats in ELIG_BEATS for travel in (ELIG_TRAVEL if beats >= 2 else (0,))]

ALLOWLIST = ("GPL-3.0-or-later", "GPL-2.0-or-later", "CC0-1.0", "CC-BY-4.0", "CC-BY-SA-4.0", "MIT",
             "Apache-2.0")

# B.4's example move, and its table.
B4_GEOM = RGeom(2, 0, 8, 8, 4, 8, 10, 1, 0)
B4_BOXES = [(2, 2, 4, 6)]
B4_MOVE = M(0, 2, 0, 0, 0, 1, 0, 4, 2, 1,
            (RStep(0, False, 12, 0, 0, 3, False), RStep(0, True, 12, 3, -2, 2, True)))
B4_TABLE = {  # tick: (unmirrored from spot -1, mirrored from spot +1)
    0: ((0, 0, 62, 153, (58, 141, 66, 153), (57, 152, 67, 155)),
        (0, 1, 258, 153, (256, 141, 264, 153), (253, 152, 263, 155))),
    3: ((0, 0, 65, 149, (61, 137, 69, 149), (64, 152, 66, 155)),
        (0, 1, 255, 149, (253, 137, 261, 149), (254, 152, 256, 155))),
    6: ((0, 0, 69, 146, (65, 134, 73, 146), None),
        (0, 1, 252, 146, (250, 134, 258, 146), None)),
    12: ((0, 1, 72, 149, (70, 137, 78, 149), (71, 152, 73, 155)),
         (0, 0, 248, 149, (244, 137, 252, 149), (247, 152, 249, 155))),
    15: ((0, 1, 86, 147, (84, 135, 92, 147), None),
         (0, 0, 234, 147, (230, 135, 238, 147), None)),
    18: ((0, 1, 117, 146, (115, 134, 123, 146), None),
         (0, 0, 204, 146, (200, 134, 208, 146), None)),
    24: ((0, 0, 158, 153, (154, 141, 162, 153), (153, 152, 163, 155)),
         (0, 1, 162, 153, (160, 141, 168, 153), (157, 152, 167, 155))),
}
# B.4's clamp example.
CLAMP_GEOM = RGeom(2, 0, 64, 76, 32, 72, 40, 1, 0)
CLAMP_BOXES = [(0, 10, 64, 62)]
CLAMP_MOVE = M(0, 1, 0, 0, 0, 0, 0, 0, 0, 0, (RStep(0, False, 12, -8, 0, 0, False),))
# A.17's move.
DOT_GEOM = RGeom(1, 0, 4, 4, 2, 4, 0, 1, 0)
DOT_BOXES = [(1, 2, 2, 2)]
DOT_MOVE = M(0, 1, 0, 0, 0, 0, 0, 0, 2, 0, (RStep(0, False, 12, 0, 0, 0, False),))
# A hand-worked idle case on A.17's frame: 200 ms at x 0, then 300 ms at x +1.
# 30 fps: 3.3 units a frame, N = 16, step 2 from n = 7: dirty 4 x6, 6, 4 x8, 6.
# 24 fps: 4.2 units a frame, N = 12, step 2 from n = 5 (21.0): 4 x4, 6, 4 x6, 6.
IDLE_MOVE = M(1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
              (RStep(0, False, 20, 0, 0, 0, False), RStep(0, False, 30, 1, 0, 0, False)))
IDLE_DIRTY = {30: [4] * 6 + [6] + [4] * 8 + [6], 24: [4] * 4 + [6] + [4] * 6 + [6]}


# ---------------------------------------------------------------------------
# Writing dancers, running the compiler.

DANCER_KEYS = ("name = Robo", "author = Ana", "license = MIT", "sheet = sheet.png", "cell = 8x8")
BASE_MOVE = ("[move base]", ("frames = A1", "beats = 1", "bpm = any"))


def dancer_keys(changes=None, add=()):
    """DANCER_KEYS with values changed (None drops the key) and lines added."""
    changes = changes or {}
    out = []
    for line in DANCER_KEYS:
        key = line.split("=")[0].strip()
        if key not in changes:
            out.append(line)
        elif changes[key] is not None:
            out.append("%s = %s" % (key, changes[key]))
    return out + list(add)


def ini_text(dancer=DANCER_KEYS, moves=(BASE_MOVE,)):
    lines = ["[dancer]"] + list(dancer)
    for header, body in moves:
        lines += ["", header] + list(body)
    return "\n".join(lines) + "\n"


def ini_line(text, section, key=None, nth=1):
    """The 1-based line of a section header, or of a key inside that section."""
    lines = text.split("\n")
    starts = [i for i, l in enumerate(lines) if l.strip() == section]
    start = starts[nth - 1]
    if key is None:
        return start + 1
    for i in range(start + 1, len(lines)):
        s = lines[i].strip()
        if s.startswith("["):
            break
        if re.match(re.escape(key) + r"\s*[=:]", s, re.I):
            return i + 1
    raise AssertionError("no %s in %s" % (key, section))


def lines_of(x):
    return x.splitlines() if isinstance(x, str) else list(x)


def write_lf(path, text):
    with open(str(path), "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def run_main(argv):
    """dancepack.main(argv) in this process: (exit code, stdout, stderr)."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        try:
            code = dp.main(argv)
        except SystemExit as e:
            code = e.code if isinstance(e.code, int) else (0 if e.code is None else 2)
    return code, out.getvalue(), err.getvalue()


def run_cli(args, cwd, script=DANCEPACK):
    env = dict(os.environ, PYTHONIOENCODING="utf-8")
    p = subprocess.run([sys.executable, str(script)] + list(args), cwd=str(cwd), env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=300)
    return p.returncode, p.stdout.decode("utf-8"), p.stderr.decode("utf-8")


def dp_geom(g, boxes):
    return dp.Geom(scale=g.scale, faces=g.faces, cell_w=g.cell_w, cell_h=g.cell_h, anchor_x=g.anchor_x,
                   anchor_y=g.anchor_y, shadow_w=g.shadow_w, frame_count=len(boxes), rest_frame=0)


def dp_boxes(boxes):
    return tuple(dp.Box(*b) for b in boxes)


def dp_move(m, name="m"):
    return dp.Move(name=name, kind=m.kind, beats=m.beats, pace_lo=m.pace_lo, pace_hi=m.pace_hi, weight10=10,
                   when=3 if m.kind else 0, flags=m.flags, repeat=0, needs=0, travel=m.travel,
                   start_at=m.start_at, travel_ease=m.travel_ease, bounce=m.bounce, sway=m.sway,
                   from_key=None, to_key=None, from_key_m=None, to_key_m=None, genres=(), song_lo=0, song_hi=0,
                   steps=tuple(dp.Step(frame=s.frame, mirrored=bool(s.mirrored), dur=s.dur, dx=s.dx, dy=s.dy,
                                       ease=s.ease, go=bool(s.go)) for s in m.steps))


def rect_or_none(r):
    return None if r is None else tuple(r)


def pose_tuple(p):
    return (p.frame, bool(p.mirrored), p.root_x, p.root_y, rect_or_none(p.box), rect_or_none(p.shadow),
            rect_or_none(p.rect))


class DpCase(unittest.TestCase):
    """A test that needs dancepack, with a temporary folder."""
    maxDiff = None

    def setUp(self):
        if dp is None:
            self.fail("tools/dancepack.py doesn't import: %r" % (DP_ERROR,))
        self.tmp = Path(tempfile.mkdtemp(prefix="dancepack-test-"))
        self.addCleanup(shutil.rmtree, str(self.tmp), True)

    def dancer(self, ini, png=None, name="robo", raw=None, sheet="sheet.png", parent=None):
        d = (parent or self.tmp) / name
        d.mkdir(parents=True, exist_ok=True)
        (d / "dancer.ini").write_bytes(raw if raw is not None else ini.encode("utf-8"))
        if png is not None:
            (d / sheet).parent.mkdir(parents=True, exist_ok=True)
            (d / sheet).write_bytes(png)
        return d

    def compile(self, ini, png=None, strict=False, **kw):
        return dp.compile_dir(str(self.dancer(ini, base_png() if png is None else png, **kw)), strict=strict)

    def messages(self, result):
        return list(result.messages)

    def assertLine(self, result, line, ci=False):
        msgs = self.messages(result)
        if ci:
            self.assertIn(line.lower(), [m.lower() for m in msgs], "\n" + "\n".join(msgs))
        else:
            self.assertIn(line, msgs, "\n" + "\n".join(msgs))

    def assertNoLine(self, result, pattern):
        hits = [m for m in self.messages(result) if re.search(pattern, m)]
        self.assertEqual(hits, [], "\n" + "\n".join(self.messages(result)))

    def assertPackError(self, result, line, ci=False):
        self.assertLine(result, line, ci)
        self.assertFalse(result.ok)
        self.assertIsNone(result.data)

    def assertSkip(self, result, ini, name, message, key=None, section=None):
        line = ini_line(ini, section or "[move %s]" % name, key)
        self.assertLine(result, 'dancer.ini:%d: move "%s": %s (skipped)' % (line, name, message))
        self.assertEqual(list(result.skipped), [name])
        self.assertTrue(result.ok)
        self.assertNotIn(name, result.budgets)

    def assertWarning(self, result, ini, section, key, message, ci=False):
        line = ini_line(ini, section, key)
        self.assertLine(result, "dancer.ini:%d: warning: %s" % (line, message), ci)


# ===========================================================================
# The suite's own tools, against the doc.

class HelperSelfTest(unittest.TestCase):
    """The PNG encoder, the MPDN reader and the reference algorithms, checked
    against each other and against every worked example in the doc."""

    def sample(self, ctype, depth, w=5, h=6, seed=1):
        rnd = lcg(seed)
        top = (1 << depth) - 1
        if ctype == 3:
            top = min(top, 15)
        return [[next(rnd) % (top + 1) if CHANNELS[ctype] == 1
                 else tuple(next(rnd) % (top + 1) for _ in range(CHANNELS[ctype])) for _ in range(w)]
                for _ in range(h)]

    def test_png_encoder_round_trips(self):
        for ctype, depths in VALID_DEPTHS.items():
            for depth in depths:
                for filters in ((0,), (1,), (2,), (3,), (4,), (0, 1, 2, 3, 4)):
                    with self.subTest(ctype=ctype, depth=depth, filters=filters):
                        px = self.sample(ctype, depth)
                        pal = [(i, 2 * i, 3 * i) for i in range(16)] if ctype == 3 else None
                        hdr, chunks, rows = ref_decode_png(make_png(px, ctype, depth, palette=pal,
                                                                    filters=filters, idat_parts=3))
                        self.assertEqual(hdr, (5, 6, depth, ctype))
                        self.assertEqual(rows, [[(p,) if isinstance(p, int) else p for p in r] for r in px])

    def test_interlaced_stream_is_seven_passes(self):
        px = self.sample(6, 8, w=10, h=9)
        raw = zlib.decompress(b"".join(ref_decode_png_chunks(make_png(px, 6, 8, interlace=True))["IDAT"]))
        expect = 0
        for x0, y0, dx, dy in ADAM7:
            sw, sh = len(range(x0, 10, dx)), len(range(y0, 9, dy))
            if sw and sh:
                expect += sh * (1 + 4 * sw)
        self.assertEqual(len(raw), expect)

    def test_cell_names(self):
        self.assertEqual(cell_xy("B4"), (1, 3))
        self.assertEqual(cell_xy("aa1"), (26, 0))
        self.assertEqual(cell_name(26, 0), "AA1")
        self.assertEqual(cell_name(31, 9), "AF10")
        self.assertEqual(cell_name(25, 0), "Z1")

    def test_a17_pack_is_the_doc_dump(self):
        data = a17_pack()
        self.assertEqual(len(data), 1156)
        self.assertEqual(struct.unpack_from("<I", data, 12)[0], 0x5BF1E285)
        self.assertEqual(data[:140], parse_dump(A17_DUMP))

    def test_mpdn_reader_on_a17(self):
        p = Mpdn(a17_pack())
        self.assertEqual((p.id, p.name, p.author, p.licence, p.url, p.version, p.declared_moves),
                         ("dot", "Dot", "Ana", "CC0-1.0", "", "", 1))
        self.assertEqual(p.geom, RGeom(1, 0, 4, 4, 2, 4, 0, 1, 0))
        self.assertEqual(p.boxes, [(1, 2, 2, 2)])
        self.assertEqual(p.grids[0], [b"\0\0\0\0", b"\0\0\0\0", b"\0\1\1\0", b"\0\1\1\0"])
        self.assertEqual(ref_frame_hash(p.grids[0]), 0x18857B21)
        self.assertEqual(ref_thumbnail(p.grids[0], p.boxes[0]), p.thumb)
        self.assertEqual(p.palette[1], (255, 0, 0))
        self.assertEqual(p.idle_palette[1], ref_idle_colour((255, 0, 0)))
        self.assertEqual(p.pose_keys, [])
        mv = p.moves[0]
        self.assertEqual((mv.name_text, mv.kind, mv.beats, mv.weight10, mv.bounce, mv.step_count),
                         ("hop", 0, 1, 10, 2, 1))
        self.assertEqual(mv.steps, (RStep(0, False, 12, 0, 0, 0, False),))

    def test_reference_eases(self):
        for code, row in EASE_TABLE.items():
            self.assertEqual(tuple(ref_ease(code, f) for f in EASE_F), row, code)
        for phi, hop in HOP_TABLE:
            self.assertEqual(ref_hop(phi), hop, phi)
        for (b, phi), v in SWAY_TABLE:
            self.assertEqual(ref_sway(b, phi), v, (b, phi))

    def test_reference_colours(self):
        self.assertEqual(ref_rgb565(0x08, 0x0C, 0x10), 0x0862)
        self.assertEqual(ref_rgb565(0xF0, 0xF0, 0xF8), 0xF79F)
        self.assertEqual(ref_rgb565(0xF4, 0x72, 0xB6), 0xF396)
        for rgb, idle in (((0xFF, 0, 0), (0x74, 0x1B, 0x1B)), ((0xF0, 0xF0, 0xF8), (0xA9, 0xA9, 0xAC)),
                          ((0xA8, 0xAC, 0xC0), (0x78, 0x79, 0x80)), ((0xF4, 0x72, 0xB6), (0x8E, 0x61, 0x78)),
                          ((0xFF, 0xFF, 0xFF), (0xB3, 0xB3, 0xB3))):
            self.assertEqual(ref_idle_colour(rgb), idle, rgb)
        self.assertTrue(ref_near_background((0x10, 0x14, 0x18)))
        self.assertFalse(ref_near_background((0x18, 0x1C, 0x20)))

    def test_reference_fnv_and_folds(self):
        self.assertEqual(ref_fnv(b""), 0x811C9DC5)
        self.assertEqual(ref_fnv(b"a"), 0xE40C292C)
        table = ref_textfold()
        for tag, key, h in FOLD_TABLE:
            self.assertEqual(ref_fold(tag, table), key, tag)
            if h is not None:
                self.assertEqual(ref_fnv(key.encode()), h, tag)

    def test_reference_textfold_table(self):
        # B.8's rule, checked by its structure rather than a count, so a new
        # fold in TextFold.cpp needs only TEXTFOLD updated (--check says so).
        table = ref_textfold()
        self.assertEqual(table[0xE9], "e")
        self.assertEqual(table[0xDF], "ss")
        self.assertEqual(table[0xC6], "AE")
        self.assertNotIn(0xBF, table)        # its replacement is "?"
        self.assertTrue(all(cp >= 0x80 for cp in table))
        self.assertNotIn("?", table.values())

    def test_reference_vlw_coverage(self):
        self.assertTrue(set(range(0x20, 0x7F)) <= ref_font_coverage())
        self.assertTrue(all(0x20 <= cp for cp in ref_font_coverage()))
        self.assertIn(0xEB, ref_font_coverage())     # the test dancer's "Tëst"
        src = vlw_array("kVlwOne", [32, 33, 34, 65, 66, 233]) + vlw_array("kVlwTwo", [32, 33, 34, 65, 67, 233])
        self.assertEqual(to_ranges(ref_vlw_code_points(src)), [(32, 34), (65, 65), (233, 233)])

    def test_reference_pace(self):
        for args, k in ELIGIBLE_TABLE + ELIGIBLE_EDGES:
            self.assertEqual(ref_eligible_k(*args), k, args)
        for args, r in REACH_TABLE:
            self.assertEqual(ref_reach(*args), r, args)
        for args, v in PACE_HOLDS_TABLE:
            self.assertEqual(ref_pace_holds(*args), v, args)

    def test_reference_frame_at_b4(self):
        for tick, (plain, mirrored) in B4_TABLE.items():
            for mir, spot, row in ((False, -1, plain), (True, 1, mirrored)):
                got = ref_frame_at(B4_GEOM, B4_BOXES, B4_MOVE, tick * 65536, mir, spot)
                self.assertEqual(got[:6], (row[0], bool(row[1])) + row[2:], (tick, mir))
                self.assertEqual(got[6], ref_union(row[4], row[5]))
        got = ref_frame_at(B4_GEOM, B4_BOXES, B4_MOVE, 15 * 65536, False, -1, e16=47186)
        self.assertEqual(got[2:6], (84, 149, (82, 137, 90, 149), (83, 152, 85, 155)))

    def test_reference_clamp(self):
        got = ref_frame_at(CLAMP_GEOM, CLAMP_BOXES, CLAMP_MOVE, 0, False, -1)
        self.assertEqual(got[2:6], (64, 153, (0, 29, 128, 153), (44, 152, 84, 155)))
        got = ref_frame_at(CLAMP_GEOM, CLAMP_BOXES, CLAMP_MOVE, 0, True, -1)
        self.assertEqual(got[2:5], (80, 153, (18, 29, 146, 153)))

    def test_reference_dirty_rule(self):
        tr = {t: ref_frame_at(B4_GEOM, B4_BOXES, B4_MOVE, t * 65536, False, -1)[6] for t in (0, 6, 15, 24)}
        self.assertEqual((ref_area(tr[6]), ref_area(tr[15]), ref_area(ref_union(tr[6], tr[15]))), (96, 96, 351))
        self.assertEqual(ref_dirty(tr[6], tr[15]), 192)
        self.assertEqual((ref_area(tr[0]), ref_area(ref_union(tr[0], tr[24]))), (140, 1484))
        self.assertEqual(ref_dirty(tr[0], tr[24]), 280)

    def test_reference_budget_b6(self):
        cases, median, peak, light = ref_budget(B4_GEOM, B4_BOXES, B4_MOVE)
        self.assertEqual([c[:4] for c in cases],
                         [(fps, pace, False, spot) for fps in (24, 30) for pace in (72, 124, 176)
                          for spot in (-1, 0)])
        by = {c[:4]: c[4:] for c in cases}
        self.assertEqual(by[(30, 124, False, -1)], (30, 160, 266))
        self.assertEqual(by[(24, 124, False, -1)][1:], (176, 304))
        self.assertEqual((median, peak, light), (192, 304, "green"))
        self.assertEqual(max(by[(24, 176, False, s)][1] for s in (-1, 0)), 192)

    def test_reference_budget_a17_and_idle(self):
        cases, median, peak, light = ref_budget(DOT_GEOM, DOT_BOXES, DOT_MOVE)
        self.assertEqual([c[:4] for c in cases], [(f, p, False, 0) for f in (24, 30) for p in (72, 124, 176)])
        self.assertEqual({c[5:] for c in cases}, {(4, 6)})
        self.assertEqual((median, peak, light), (4, 6, "green"))
        cases, median, peak, light = ref_budget(DOT_GEOM, DOT_BOXES, IDLE_MOVE)
        self.assertEqual(cases, [(24, 0, False, 0, 12, 4, 6), (30, 0, False, 0, 16, 4, 6)])
        for fps in (24, 30):
            self.assertEqual([d for _, _, _, d in ref_trace(DOT_GEOM, DOT_BOXES, IDLE_MOVE, fps, 0, False, 0)[1:]],
                             IDLE_DIRTY[fps])

    def test_reference_extent(self):
        self.assertEqual(ref_extent(B4_GEOM, B4_BOXES, B4_MOVE, False), (-6, 14))
        self.assertEqual(ref_start_spots(B4_GEOM, B4_BOXES, B4_MOVE, False), [-1, 0])

    def test_reference_encoders(self):
        grid = [bytes([5] + [0] * 39), bytes([0, 0] + [3] * 37 + [4])]
        self.assertEqual(ref_box(grid), (0, 0, 40, 2))
        self.assertEqual(ref_encode_frame(grid, (0, 0, 40, 2)), bytes.fromhex("00 01 05 02 04 f3 f3 43 04"))
        self.assertEqual(ref_beats_text(45), "3 3/4 beats")
        self.assertEqual([ref_beats_text(t) for t in (3, 12, 24, 4, 18)],
                         ["1/4 beat", "1 beat", "2 beats", "1/3 beat", "1 1/2 beats"])


def ref_decode_png_chunks(data):
    pos, chunks = 8, collections.OrderedDict()
    while pos < len(data):
        n, tag = struct.unpack_from(">I4s", data, pos)
        chunks.setdefault(tag.decode("ascii"), []).append(data[pos + 8:pos + 8 + n])
        pos += 12 + n
    return chunks


def png_chunk_list(data):
    """[(tag, body)] of a PNG, in order."""
    pos, out = 8, []
    while pos < len(data):
        n, tag = struct.unpack_from(">I4s", data, pos)
        out.append((tag.decode("ascii"), data[pos + 8:pos + 8 + n]))
        pos += 12 + n
    return out


def png_join(chunks):
    return PNG_SIG + b"".join(png_chunk(t, b) for t, b in chunks)


# ===========================================================================
# The API: constants and the shared algorithms (Appendix B, C.9).

class ConstantsTest(DpCase):
    def test_constants(self):
        self.assertEqual((dp.FORMAT, dp.MIN_READER, dp.MAX_PACK), (1, 1, 131072))
        self.assertEqual((dp.STAGE_W, dp.STAGE_H, dp.GROUND_Y), (320, 164, 153))
        self.assertEqual(tuple(dp.SPOTS), (64, 160, 256))
        self.assertEqual(tuple(dp.EASES), ("hold", "linear", "in", "out", "inout"))
        self.assertEqual(tuple(dp.LICENCE_ALLOWLIST), ALLOWLIST)
        self.assertTrue(issubclass(dp.DanceError, ValueError))
        self.assertEqual(Path(dp.ROOT).resolve(), REPO)

    def test_registry(self):
        builtins = [tuple(b) for b in dp.BUILTINS]
        self.assertIn((TEST_DANCER, TESTPACK_H), builtins)
        for src, out in builtins:
            self.assertNotIn("\\", src + out)
            self.assertTrue((REPO / src / "dancer.ini").is_file(), src)

    def test_records(self):
        self.assertEqual(dp.Geom._fields, ("scale", "faces", "cell_w", "cell_h", "anchor_x", "anchor_y",
                                           "shadow_w", "frame_count", "rest_frame"))
        self.assertEqual(dp.Box._fields, ("x0", "y0", "w", "h"))
        self.assertEqual(dp.Step._fields, ("frame", "mirrored", "dur", "dx", "dy", "ease", "go"))
        self.assertEqual(dp.Move._fields, (
            "name", "kind", "beats", "pace_lo", "pace_hi", "weight10", "when", "flags", "repeat", "needs",
            "travel", "start_at", "travel_ease", "bounce", "sway", "from_key", "to_key", "from_key_m",
            "to_key_m", "genres", "song_lo", "song_hi", "steps"))
        self.assertEqual(dp.Pack._fields, (
            "size", "crc", "id", "name", "author", "licence", "url", "version", "declared_moves", "geom",
            "palette", "idle_palette", "thumb", "boxes", "grids", "pose_keys", "moves", "ignored"))
        self.assertEqual(dp.Pose._fields, ("frame", "mirrored", "root_x", "root_y", "box", "shadow", "rect"))
        self.assertEqual(dp.Case._fields, ("fps", "pace", "mirrored", "spot", "frames", "median", "peak"))
        self.assertEqual(dp.Budget._fields, ("cases", "median", "peak", "light"))
        self.assertEqual(dp.Result._fields, ("ok", "data", "pack", "messages", "report", "skipped", "budgets"))


class SharedAlgorithmsTest(DpCase):
    def test_fnv_and_crc(self):
        self.assertEqual(dp.fnv1a32(b""), 0x811C9DC5)
        self.assertEqual(dp.fnv1a32(b"a"), 0xE40C292C)
        self.assertEqual(dp.fnv1a32(b"hiphop"), 0x5DE39FA5)
        self.assertEqual(dp.crc32(b"123456789"), 0xCBF43926)
        self.assertEqual(dp.crc32(a17_pack()[16:]), 0x5BF1E285)

    def test_colours(self):
        self.assertEqual(dp.rgb565(0x08, 0x0C, 0x10), 0x0862)
        self.assertEqual(dp.rgb565(0xF0, 0xF0, 0xF8), 0xF79F)
        self.assertEqual(dp.rgb565(0xFF, 0xFF, 0xFF), 0xFFFF)
        self.assertEqual(dp.rgb565(0xFE, 0x03, 0x07), 0xF800)    # truncation, not rounding
        for rgb, idle in (((0xFF, 0, 0), (0x74, 0x1B, 0x1B)), ((0xF0, 0xF0, 0xF8), (0xA9, 0xA9, 0xAC)),
                          ((0xA8, 0xAC, 0xC0), (0x78, 0x79, 0x80)), ((0xF4, 0x72, 0xB6), (0x8E, 0x61, 0x78)),
                          ((0xFF, 0xFF, 0xFF), (0xB3, 0xB3, 0xB3)), ((0, 0, 0), (0, 0, 0))):
            self.assertEqual(tuple(dp.idle_colour(rgb)), idle, rgb)
        for rgb, near in (((0x10, 0x14, 0x18), True), ((0x18, 0x1C, 0x20), False), ((8, 12, 16), True),
                          ((23, 27, 31), True), ((24, 12, 16), False), ((8, 12, 0), False), ((8, 12, 31), True)):
            self.assertEqual(bool(dp.near_background(rgb)), near, rgb)

    def test_pose_twin(self):
        for key, twin in (("lean-left", "lean-right"), ("right", "left"), ("left", "right"), ("stand", "stand"),
                          ("leftish", "leftish"), ("left-right", "right-left"), ("a-left-b", "a-right-b"),
                          ("low", "low")):
            self.assertEqual(dp.pose_twin(key), twin, key)

    def test_beats_text(self):
        for t, text in ((3, "1/4 beat"), (4, "1/3 beat"), (6, "1/2 beat"), (1, "1/12 beat"), (12, "1 beat"),
                        (24, "2 beats"), (96, "8 beats"), (45, "3 3/4 beats"), (18, "1 1/2 beats"),
                        (13, "1 1/12 beats"), (16, "1 1/3 beats")):
            self.assertEqual(dp.beats_text(t), text, t)

    def test_parse_beat_time(self):
        # B.1's table.
        for text, ticks, snapped in (("1/4", 3, False), (".25", 3, False), ("1/3", 4, False), ("3/4", 9, False),
                                     ("2", 24, False), ("0.33", 4, True), ("0.26", 3, True), ("0.5", 6, False),
                                     ("1", 12, False), ("8", 96, False), ("4/12", 4, False)):
            got = dp.parse_beat_time(text)
            self.assertEqual((got[0], bool(got[1])), (ticks, snapped), text)
        for text in ("0.3", "1/5", "0.125"):
            with self.assertRaises(dp.DanceError) as cm:
                dp.parse_beat_time(text)
            self.assertEqual(cm.exception.args[0],
                             "timing %s is off the 1/12-beat grid by more than 1/48 beat" % text)
        for text in ("1e2", "-1/4", "+1", "1s", "half", "1/0", "1/2/3", "", "1."):
            with self.assertRaises(dp.DanceError, msg=text):
                dp.parse_beat_time(text)
        with self.assertRaises(dp.DanceError) as cm:
            dp.parse_beat_time("half")
        self.assertEqual(cm.exception.args[0], 'timing "half" isn\'t a number of beats, like 1/4, 0.5 or 2')

    def test_parse_idle_time(self):
        for text, units in (("2s", 200), ("1.5s", 150), ("400ms", 40), (".5s", 50), ("10ms", 1),
                            ("12s", 1200), ("0.01s", 1), ("2S", 200), ("400MS", 40), ("400Ms", 40)):  # B.1's note
            self.assertEqual(dp.parse_idle_time(text), units, text)
        with self.assertRaises(dp.DanceError) as cm:
            dp.parse_idle_time("0.405s")
        self.assertEqual(cm.exception.args[0], "timing 0.405s isn't a whole number of 10 ms")
        with self.assertRaises(dp.DanceError) as cm:
            dp.parse_idle_time("2")
        self.assertEqual(cm.exception.args[0], 'timing "2" needs s or ms, like 2s or 400ms')
        for text in ("2 s", "-2s", "2sec", "1/2s", "15ms"):
            with self.assertRaises(dp.DanceError, msg=text):
                dp.parse_idle_time(text)

    def even_split(self, n, beats):
        try:
            return dp.even_split(n, beats)
        except TypeError:   # C.9 names the argument `frames`: a count or the frames themselves
            return dp.even_split(["A1"] * n, beats)

    def test_even_split(self):
        self.assertEqual(list(self.even_split(8, 2)), [3] * 8)
        self.assertEqual(list(self.even_split(3, 4)), [16] * 3)
        self.assertEqual(list(self.even_split(1, 8)), [96])
        self.assertEqual(list(self.even_split(32, 8)), [3] * 32)
        with self.assertRaises(dp.DanceError) as cm:
            self.even_split(5, 1)
        self.assertEqual(cm.exception.args[0], "5 frames can't share 1 beat evenly on the 1/12 grid: give timing =")
        with self.assertRaises(dp.DanceError) as cm:
            self.even_split(5, 4)
        self.assertEqual(cm.exception.args[0],
                         "5 frames can't share 4 beats evenly on the 1/12 grid: give timing =")

    def test_eases_hop_sway(self):
        for code, row in EASE_TABLE.items():
            self.assertEqual(tuple(dp.ease_q16(code, f) for f in EASE_F), row, dp.EASES[code])
        for phi, hop in HOP_TABLE:
            self.assertEqual(dp.hop_q16(phi), hop, phi)
        for (b, phi), v in SWAY_TABLE:
            self.assertEqual(dp.sway_q16(b, phi), v, (b, phi))
        for f in range(0, 65537, 4099):
            for code in range(5):
                self.assertEqual(dp.ease_q16(code, f), ref_ease(code, f), (code, f))
        for phi in range(0, 65536, 997):
            self.assertEqual(dp.hop_q16(phi), ref_hop(phi), phi)
            self.assertEqual(dp.sway_q16(3, phi), ref_sway(3, phi), phi)

    def test_eligible_k(self):
        for args, k in ELIGIBLE_TABLE + ELIGIBLE_EDGES:
            self.assertEqual(dp.eligible_k(*args), k, args)
        for args in elig_grid():
            self.assertEqual(dp.eligible_k(*args), ref_eligible_k(*args), args)
        # C.9: travel defaults to 0
        self.assertEqual(dp.eligible_k(720, 100, 150, None, 2), 1)

    def test_eligible_k_agrees_with_reach(self):
        # F4: the octaves eligible_k can return are the ones reach() allows.
        for t10 in range(720, 1761, 10):
            for lo, hi in ELIG_RANGES[1:]:
                for beats in (2, 4, 8):
                    for travel in (0, 1):
                        k = dp.eligible_k(t10, lo, hi, None, beats, travel)
                        if k is not None:
                            r = dp.reach(lo, hi, beats, travel)
                            self.assertIsNotNone(r)
                            self.assertTrue(r[0] * 20 <= t10 * 2 ** (k + 1) <= r[1] * 20, (t10, lo, hi, beats, travel))

    def test_pace_holds(self):
        for args, v in PACE_HOLDS_TABLE:
            self.assertEqual(bool(dp.pace_holds(*args)), v, args)
        for t10 in PACE_T10:
            for k in (-1, 0, 1):
                for lo, hi in PACE_RANGES:
                    self.assertEqual(bool(dp.pace_holds(t10, k, lo, hi)), ref_pace_holds(t10, k, lo, hi))

    def test_reach(self):
        for (lo, hi, beats, travel), r in REACH_TABLE:
            got = dp.reach(lo, hi, beats, travel)
            self.assertEqual(None if got is None else tuple(got), r, (lo, hi, beats, travel))
        self.assertEqual(tuple(dp.reach(0, 0, 2)), (72, 176))
        # B.7: no octave allowed (an unranged travel move under 2 beats) reaches nothing
        self.assertIsNone(dp.reach(0, 0, 1, 1))

    def b4(self, move=B4_MOVE):
        return dp_geom(B4_GEOM, B4_BOXES), dp_boxes(B4_BOXES), dp_move(move)

    def test_frame_at_b4(self):
        geom, boxes, move = self.b4()
        for tick, (plain, mirrored) in B4_TABLE.items():
            for mir, spot, row in ((False, -1, plain), (True, 1, mirrored)):
                got = pose_tuple(dp.frame_at(geom, boxes, move, tick * 65536, mir, spot))
                want = (row[0], bool(row[1])) + row[2:] + (ref_union(row[4], row[5]),)
                self.assertEqual(got, want, (tick, mir))
        got = pose_tuple(dp.frame_at(geom, boxes, move, 15 * 65536, False, -1, 47186))
        self.assertEqual(got[2:6], (84, 149, (82, 137, 90, 149), (83, 152, 85, 155)))
        # Every pos16 of one play and a bit, both ways, against the reference.
        for pos in range(0, 30 * 65536, 4321):
            for mir, spot in ((False, -1), (False, 0), (True, 1), (True, 0)):
                self.assertEqual(pose_tuple(dp.frame_at(geom, boxes, move, pos, mir, spot)),
                                 ref_frame_at(B4_GEOM, B4_BOXES, B4_MOVE, pos, mir, spot), (pos, mir, spot))
        # B.4 step 8's p is floored: at g = 33270 into the go step, rounding would give 73
        self.assertEqual(dp.frame_at(geom, boxes, move, 12 * 65536 + 33270, False, -1).root_x, 72)

    def test_frame_at_needs_a_position(self):
        # B.4's precondition: pos16 >= 0 (the Choreographer clamps u at 0, section 3.5)
        geom, boxes, move = self.b4()
        for pos in (-1, -3277, -6554):
            with self.assertRaises(ValueError):
                dp.frame_at(geom, boxes, move, pos, False, -1)

    def test_one_beat_sway(self):
        # B.3: b counts move beats since the run started, so a 1-beat move's
        # sway swings on through its replays (+1 at tick 12, the second play).
        one = M(0, 1, 0, 0, 0, 0, 0, 0, 0, 2, (RStep(0, False, 12, 0, 0, 0, False),))
        geom, boxes = dp_geom(B4_GEOM, B4_BOXES), dp_boxes(B4_BOXES)
        got = [dp.frame_at(geom, boxes, dp_move(one), t * 65536, False, 0).root_x for t in (0, 6, 12, 18, 24)]
        self.assertEqual(got, [156, 160, 164, 160, 156])
        self.assertEqual(dp.sway_q16(1, 0), 65536)
        bouncy = one._replace(bounce=3)
        for pos in range(0, 3 * 12 * 65536, 3001):
            self.assertEqual(pose_tuple(dp.frame_at(geom, boxes, dp_move(bouncy), pos, False, 0)),
                             ref_frame_at(B4_GEOM, B4_BOXES, bouncy, pos, False, 0), pos)
        self.assertEqual(self.cases(dp.budget(geom, boxes, dp_move(bouncy))), ref_budget(B4_GEOM, B4_BOXES, bouncy)[0])

    def test_held_travel_lands(self):
        # B.2: hold is 65536 at f = 65536, so a held travel lands at the end of
        # the play (B.4 step 8) and the budget sees the jump (B.6).
        self.assertEqual((dp.ease_q16(0, 65535), dp.ease_q16(0, 65536), dp.ease_q16("hold", 65536)), (0, 65536, 65536))
        held = B4_MOVE._replace(travel_ease=0)
        geom, boxes, move = self.b4(held)
        self.assertEqual([dp.frame_at(geom, boxes, move, t * 65536, False, -1).root_x for t in (0, 12, 23, 24, 30)],
                         [62, 72, 63, 158, 165])
        b = dp.budget(geom, boxes, move)
        self.assertEqual(self.cases(b), ref_budget(B4_GEOM, B4_BOXES, held)[0])
        self.assertEqual((b.median, b.peak), (160, 290))

    def test_odd_shadow(self):
        # B.5: x0 = rootX - sw // 2, so an odd shadow's extra column is on the right.
        g = B4_GEOM._replace(shadow_w=11)
        mv = DOT_MOVE._replace(bounce=0)
        got = pose_tuple(dp.frame_at(dp_geom(g, B4_BOXES), dp_boxes(B4_BOXES), dp_move(mv), 0, False, 0))
        self.assertEqual(got[5], (155, 152, 166, 155))
        self.assertEqual(got, ref_frame_at(g, B4_BOXES, mv, 0, False, 0))

    def test_frame_at_snap(self):
        move = B4_MOVE._replace(flags=32)
        geom, boxes, dmove = self.b4(move)
        # tick 3: x16 = 40960 is 0.625 native px: 1, then x2 (2 without snap's rounding of 1.25 -> 1).
        self.assertEqual(dp.frame_at(geom, boxes, dmove, 3 * 65536, False, -1).root_x, 66)
        for pos in range(0, 24 * 65536, 7777):
            self.assertEqual(pose_tuple(dp.frame_at(geom, boxes, dmove, pos, False, -1)),
                             ref_frame_at(B4_GEOM, B4_BOXES, move, pos, False, -1), pos)

    def test_frame_at_clamp(self):
        geom, boxes, move = dp_geom(CLAMP_GEOM, CLAMP_BOXES), dp_boxes(CLAMP_BOXES), dp_move(CLAMP_MOVE)
        got = pose_tuple(dp.frame_at(geom, boxes, move, 0, False, -1))
        self.assertEqual(got[2:6], (64, 153, (0, 29, 128, 153), (44, 152, 84, 155)))
        got = pose_tuple(dp.frame_at(geom, boxes, move, 0, True, -1))
        self.assertEqual(got[2:5], (80, 153, (18, 29, 146, 153)))

    def test_frame_at_vertical_clamp_and_shadow_clip(self):
        # B.4 step 11 in y, and B.5's shadow (hop from Y before the clamp; clipped to x 0-320), by hand.
        g = RGeom(2, 0, 64, 76, 32, 72, 40, 2, 0)
        boxes = [(16, 0, 32, 72), (0, 0, 0, 0)]
        geom, bx = dp_geom(g, boxes), dp_boxes(boxes)

        def pose(frame, dx, dy, spot):
            mv = M(0, 1, 0, 0, 0, 0, 0, 0, 0, 0, (RStep(frame, False, 12, dx, dy, 0, False),))
            got = pose_tuple(dp.frame_at(geom, bx, dp_move(mv), 0, False, spot))
            self.assertEqual(got, ref_frame_at(g, boxes, mv, 0, False, spot))
            return got
        # key 0,-24: Y = -48, root y 105 puts the top at -39: pushed down 39; a 48 px hop leaves no shadow
        self.assertEqual(pose(0, 0, -24, 0), (0, False, 160, 144, (128, 0, 192, 144), None, (128, 0, 192, 144)))
        # key 0,+10: Y = 20, the bottom at 173: pushed up 9; Y > 0 is no hop, so the shadow is whole
        self.assertEqual(pose(0, 0, 10, 0), (0, False, 160, 164, (128, 20, 192, 164), (140, 152, 180, 155),
                                             (128, 20, 192, 164)))
        # a blank frame isn't clamped, but its shadow is clipped to the stage
        self.assertEqual(pose(1, -24, 0, -1), (1, False, 16, 153, None, (0, 152, 36, 155), (0, 152, 36, 155)))
        self.assertEqual(pose(1, 24, 0, 1), (1, False, 304, 153, None, (284, 152, 320, 155), (284, 152, 320, 155)))

    def test_budget_drops_a_repeated_pace(self):
        # B.6: the paces rlo, (rlo + rhi) // 2 and rhi, a repeat dropped.
        g, bx = dp_geom(DOT_GEOM, DOT_BOXES), dp_boxes(DOT_BOXES)
        for lo, hi, paces in ((100, 100, [100]), (100, 101, [100, 101]), (100, 102, [100, 101, 102])):
            mv = DOT_MOVE._replace(pace_lo=lo, pace_hi=hi)
            got = [(c.fps, c.pace) for c in dp.budget(g, bx, dp_move(mv)).cases]
            self.assertEqual(got, [(f, p) for f in (24, 30) for p in paces], (lo, hi))
            self.assertEqual([c[:2] for c in ref_budget(DOT_GEOM, DOT_BOXES, mv)[0]], got)

    def test_frame_at_blank_and_idle(self):
        geom = dp_geom(DOT_GEOM, [(1, 2, 2, 2), (0, 0, 0, 0)])
        boxes = dp_boxes([(1, 2, 2, 2), (0, 0, 0, 0)])
        blank = M(0, 1, 0, 0, 0, 0, 0, 0, 0, 0, (RStep(1, False, 12, -9, 0, 0, False),))
        pose = dp.frame_at(geom, boxes, dp_move(blank), 0, False, -1)
        self.assertEqual((pose.frame, pose.box, pose.shadow, pose.rect), (1, None, None, None))
        self.assertEqual((pose.root_x, pose.root_y), (64 - 9, 153))      # no box, so no clamp
        # idle moves: pos16 in 10 ms units, no bounce or sway, e16 ignored (always 65536)
        idle = dp_move(IDLE_MOVE._replace(bounce=5, sway=5))
        g, b = dp_geom(DOT_GEOM, DOT_BOXES), dp_boxes(DOT_BOXES)
        self.assertEqual(dp.frame_at(g, b, idle, 25 * 65536, False, 0).root_x, 161)
        self.assertEqual(dp.frame_at(g, b, idle, 25 * 65536, False, 0).root_y, 153)
        self.assertEqual(dp.frame_at(g, b, idle, 55 * 65536, False, 0).root_x, 160)    # wrapped

    def test_dirty_px(self):
        self.assertEqual(dp.dirty_px((65, 134, 73, 146), (84, 135, 92, 147)), 192)
        self.assertEqual(dp.dirty_px((57, 141, 67, 155), (153, 141, 163, 155)), 280)
        self.assertEqual(dp.dirty_px(None, (0, 0, 10, 10)), 100)
        a = (0, 0, 10, 10)
        self.assertEqual(dp.dirty_px(a, (2, 0, 12, 10)), 120)      # overlapping: the union
        self.assertEqual(dp.dirty_px(a, (16, 0, 26, 10)), 260)     # 10*260 = 13*200 exactly: still one rect
        self.assertEqual(dp.dirty_px(a, (17, 0, 27, 10)), 200)     # 2700 > 2600: two rects
        self.assertEqual(dp.dirty_px(a, (6, 6, 16, 16)), 256)
        self.assertEqual(dp.dirty_px(a, (7, 7, 17, 17)), 200)      # the union is 289

    def test_extent_and_spots(self):
        geom, boxes, move = self.b4()
        self.assertEqual(tuple(dp.extent(geom, boxes, move, False)), (-6, 14))
        self.assertEqual(tuple(dp.extent(geom, boxes, move, True)),
                         ref_extent(B4_GEOM, B4_BOXES, B4_MOVE, True))
        self.assertEqual(list(dp.start_spots(geom, boxes, move, False)), [-1, 0])
        self.assertEqual(list(dp.start_spots(geom, boxes, move, True)), [0, 1])
        self.assertTrue(dp.fits(geom, boxes, move, False, -1))
        # one step keyed at dx: lo = L - 2|dx| - 2 (sway 1 at scale 2) = -4 - 58 - 2 = -64 at dx -29,
        # so spot -1 (x 64) just fits; at dx -30 it doesn't
        wide = dp_move(B4_MOVE._replace(steps=(RStep(0, False, 24, -29, 0, 0, False),)))
        self.assertEqual(tuple(dp.extent(geom, boxes, wide, False)), (-64, 6))
        self.assertTrue(dp.fits(geom, boxes, wide, False, -1))
        wider = dp_move(B4_MOVE._replace(steps=(RStep(0, False, 24, -30, 0, 0, False),)))
        self.assertFalse(dp.fits(geom, boxes, wider, False, -1))
        self.assertEqual(list(dp.start_spots(geom, boxes, wider, False)), [0])
        for start_at, mirrored, spots in ((1, False, [0]), (2, False, [-1]), (3, False, [-1]), (4, False, []),
                                          (3, True, [1]), (4, True, []), (2, True, [1])):
            mv = B4_MOVE._replace(start_at=start_at)
            self.assertEqual(list(dp.start_spots(geom, boxes, dp_move(mv), mirrored)), spots, (start_at, mirrored))
            self.assertEqual(ref_start_spots(B4_GEOM, B4_BOXES, mv, mirrored), spots, (start_at, mirrored))
        blank = dp_move(B4_MOVE._replace(steps=(RStep(1, False, 24, 0, 0, 0, False),)))
        two = dp_boxes(B4_BOXES + [(0, 0, 0, 0)])
        self.assertIsNone(dp.extent(geom, two, blank, False))

    def test_extent_wraps_to_the_first_key(self):
        # B.11: j = (i + 1) mod n, so the last step's c is the first step's key.
        g = RGeom(2, 0, 16, 8, 8, 8, 0, 2, 0)
        boxes = [(7, 0, 2, 8), (0, 0, 16, 8)]
        mv = M(0, 2, 0, 0, 0, 0, 0, 0, 0, 0,
               (RStep(0, False, 12, -10, 0, 1, False), RStep(1, False, 12, 0, 0, 1, False)))
        geom, bx, move = dp_geom(g, boxes), dp_boxes(boxes), dp_move(mv)
        self.assertEqual(tuple(dp.extent(geom, bx, move, False)), (-36, 16))
        self.assertEqual(tuple(dp.extent(geom, bx, move, True)), (-14, 38))
        self.assertEqual(ref_extent(g, boxes, mv, False), (-36, 16))
        lefts = [dp.frame_at(geom, bx, move, p, False, 0).box[0] - 160 for p in range(0, 24 * 65536, 997)]
        self.assertEqual(min(lefts), -36)      # the bound is reached: the extent is tight here

    def cases(self, budget):
        return [(c.fps, c.pace, bool(c.mirrored), c.spot, c.frames, c.median, c.peak) for c in budget.cases]

    def test_budget(self):
        geom, boxes, move = self.b4()
        b = dp.budget(geom, boxes, move)
        self.assertEqual(self.cases(b), ref_budget(B4_GEOM, B4_BOXES, B4_MOVE)[0])
        by = {c[:4]: c[4:] for c in self.cases(b)}
        self.assertEqual(by[(30, 124, False, -1)], (30, 160, 266))
        self.assertEqual(by[(24, 124, False, -1)][1:], (176, 304))
        self.assertEqual((b.median, b.peak, b.light), (192, 304, "green"))
        b = dp.budget(dp_geom(DOT_GEOM, DOT_BOXES), dp_boxes(DOT_BOXES), dp_move(DOT_MOVE))
        self.assertEqual(self.cases(b), ref_budget(DOT_GEOM, DOT_BOXES, DOT_MOVE)[0])
        self.assertEqual([c[:4] + c[5:] for c in self.cases(b)],
                         [(f, p, False, 0, 4, 6) for f in (24, 30) for p in (72, 124, 176)])
        self.assertEqual((b.median, b.peak, b.light), (4, 6, "green"))

    def test_budget_by_hand(self):
        # IDLE_MOVE's numbers are worked out by hand above the table.
        g, bx, mv = dp_geom(DOT_GEOM, DOT_BOXES), dp_boxes(DOT_BOXES), dp_move(IDLE_MOVE)
        b = dp.budget(g, bx, mv)
        self.assertEqual(self.cases(b), [(24, 0, False, 0, 12, 4, 6), (30, 0, False, 0, 16, 4, 6)])
        self.assertEqual((b.median, b.peak, b.light), (4, 6, "green"))
        for fps in (24, 30):
            tr = dp.trace(g, bx, mv, fps, 0, False, 0)
            self.assertEqual([n for n, _, _, _ in tr], list(range(len(IDLE_DIRTY[fps]) + 1)))
            self.assertIsNone(tr[0][3])
            self.assertEqual([d for _, _, _, d in tr[1:]], IDLE_DIRTY[fps])
            period = dict(FPS_PERIOD)[fps]
            self.assertEqual([p for _, p, _, _ in tr], [n * period * 65536 // 10 for n in range(len(tr))])
        tr = dp.trace(g, bx, mv, 30, 0, False, 0)
        self.assertEqual([t[2].root_x for t in tr], [160] * 7 + [161] * 9 + [160])

    def test_budget_n_on_the_boundary(self):
        # B.6: N is the first n >= 1 with pos16(n) >= L*65536. 330 ms + 330 ms at
        # 30 fps: pos16(20) = 20*33*65536 // 10 = 66*65536 exactly, so N = 20
        # (step 2 from n = 10, 33.0); at 24 fps 4.2 units a frame, N = 16 (67.2),
        # step 2 from n = 8 (33.6).
        g, bx = dp_geom(DOT_GEOM, DOT_BOXES), dp_boxes(DOT_BOXES)
        mv = M(1, 0, 0, 0, 0, 0, 0, 0, 0, 0, (RStep(0, False, 33, 0, 0, 0, False), RStep(0, False, 33, 1, 0, 0, False)))
        b = dp.budget(g, bx, dp_move(mv))
        self.assertEqual(self.cases(b), [(24, 0, False, 0, 16, 4, 6), (30, 0, False, 0, 20, 4, 6)])
        self.assertEqual(self.cases(b), ref_budget(DOT_GEOM, DOT_BOXES, mv)[0])
        self.assertEqual([d for _, _, _, d in dp.trace(g, bx, dp_move(mv), 30, 0, False, 0)[1:]],
                         [4] * 9 + [6] + [4] * 9 + [6])

    def test_lights(self):
        # One static frame: every dirty count is the box's area (160 px wide at scale 4).
        geom = dp_geom(RGeom(4, 0, 40, 40, 20, 38, 0, 1, 0), [(0, 0, 40, 40)])
        mv = dp_move(M(0, 1, 0, 0, 0, 0, 0, 0, 0, 0, (RStep(0, False, 12, 0, 0, 0, False),)))
        for box, peak, light in (((0, 0, 40, 40), 25600, "red"), ((0, 0, 40, 32), 20480, "red"),
                                 ((0, 8, 40, 30), 19200, "amber"), ((0, 0, 40, 31), 19840, "amber"),
                                 ((0, 0, 40, 25), 16000, "green"), ((0, 0, 40, 26), 16640, "amber")):
            b = dp.budget(geom, dp_boxes([box]), mv)
            self.assertEqual((b.median, b.peak, b.light), (peak, peak, light), box)

    def test_trace_matches_reference(self):
        geom, boxes, move = self.b4()
        for fps in (24, 30):
            for spot in (-1, 0):
                got = dp.trace(geom, boxes, move, fps, 124, False, spot)
                want = ref_trace(B4_GEOM, B4_BOXES, B4_MOVE, fps, 124, False, spot)
                self.assertEqual([(n, p, pose_tuple(q), d) for n, p, q, d in got], want, (fps, spot))

    def test_encode_frame(self):
        dot = [b"\0\0\0\0", b"\0\0\0\0", b"\0\1\1\0", b"\0\1\1\0"]
        self.assertEqual(bytes(dp.encode_frame(dot, dp.Box(1, 2, 2, 2))), bytes.fromhex("00 01 11 00 01 11"))
        grid = [bytes([5] + [0] * 39), bytes([0, 0] + [3] * 37 + [4])]
        self.assertEqual(bytes(dp.encode_frame(grid, dp.Box(0, 0, 40, 2))),
                         bytes.fromhex("00 01 05 02 04 f3 f3 43 04"))
        gap = [bytes([1, 0, 0, 2, 0]), bytes(5), bytes([0, 0, 0, 0, 9])]
        self.assertEqual(bytes(dp.encode_frame(gap, dp.Box(0, 0, 5, 3))),
                         bytes.fromhex("00 03 01 10 02  00 00  04 01 09"))
        run16 = [bytes([7] * 16 + [8] * 17)]
        self.assertEqual(bytes(dp.encode_frame(run16, dp.Box(0, 0, 33, 1))), bytes.fromhex("00 03 f7 f8 08"))
        # A.10: a gap is split into 16 px tokens from its left too
        gap20 = [bytes([1] + [0] * 20 + [2])]
        self.assertEqual(bytes(dp.encode_frame(gap20, dp.Box(0, 0, 22, 1))), bytes.fromhex("00 04 01 f0 30 02"))
        self.assertEqual(ref_encode_frame(gap20, (0, 0, 22, 1)), bytes.fromhex("00 04 01 f0 30 02"))

    def test_thumbnail_and_hash(self):
        dot = [b"\0\0\0\0", b"\0\0\0\0", b"\0\1\1\0", b"\0\1\1\0"]
        self.assertEqual(bytes(dp.thumbnail(dot, dp.Box(1, 2, 2, 2))), b"\x11" * 800)
        self.assertEqual(dp.frame_hash(dot), 0x18857B21)
        rnd = lcg(3)
        grid = [bytes(next(rnd) % 15 + 1 if 2 <= x < 15 and 1 <= y < 8 else 0 for x in range(16)) for y in range(9)]
        box = ref_box(grid)
        self.assertEqual(box, (2, 1, 13, 7))
        self.assertEqual(bytes(dp.thumbnail(grid, dp.Box(*box))), ref_thumbnail(grid, box))   # m = 3
        big = [bytes((x * 7 + y * 3) % 6 if 3 <= x < 60 else 0 for x in range(64)) for y in range(70)]
        box = ref_box(big)
        self.assertEqual(bytes(dp.thumbnail(big, dp.Box(*box))), ref_thumbnail(big, box))     # s = 2
        self.assertEqual(dp.frame_hash(grid), ref_frame_hash(grid))
        # A.8's s at exact multiples of 40: (80 + 39) // 40 = 2, (120 + 39) // 40 = 3
        rnd = lcg(9)
        for w, h in ((80, 80), (80, 30), (120, 50)):
            g = [bytes(next(rnd) % 4 for _ in range(w)) for _ in range(h)]
            self.assertEqual(bytes(dp.thumbnail(g, dp.Box(0, 0, w, h))), ref_thumbnail(g, (0, 0, w, h)), (w, h))

    def test_genre_folding(self):
        for tag, key, h in FOLD_TABLE:
            self.assertEqual(dp.fold_genre(tag), key, tag)
            self.assertEqual(dp.genre_key(tag), h, tag)
        self.assertEqual(dp.textfold_full("Électro").lower(), "electro")
        self.assertEqual(dp.textfold_full("Straße").lower(), "strasse")
        self.assertEqual(dp.textfold_full("日本"), "??")
        self.assertEqual(dp.textfold_full("plain ASCII & co"), "plain ASCII & co")
        self.assertEqual(dp.textfold_full("¿"), "?")


class TablesTest(DpCase):
    """TEXTFOLD and FONT_COVERAGE, against the C++ sources they mirror (C.2)."""

    def test_textfold(self):
        table = {int(k): v for k, v in dp.TEXTFOLD.items()}
        self.assertEqual(table, ref_textfold())
        self.assertEqual({int(k): v for k, v in dp.parse_textfold_cpp(repo_text(TEXTFOLD_CPP)).items()},
                         ref_textfold())

    def test_font_coverage(self):
        want = to_ranges(ref_font_coverage())
        self.assertEqual([tuple(r) for r in dp.FONT_COVERAGE], want)
        self.assertEqual([tuple(r) for r in dp.parse_vlw_coverage(repo_text(VLWFONTS_CPP))], want)
        src = vlw_array("kVlwOne", [32, 33, 34, 65, 66, 233]) + vlw_array("kVlwTwo", [32, 33, 34, 65, 67, 233])
        self.assertEqual([tuple(r) for r in dp.parse_vlw_coverage(src)], [(32, 34), (65, 65), (233, 233)])


# ===========================================================================
# Reading PNGs (C.4).

class PngReadTest(DpCase):
    def read(self, data):
        w, h, rows = dp.read_png(data)
        return w, h, [[tuple(p) for p in row] for row in rows]

    def test_grey_every_depth(self):
        for depth in (1, 2, 4, 8):
            top = (1 << depth) - 1
            px = [[(x + y) % (top + 1) for x in range(5)] for y in range(3)]
            w, h, rows = self.read(make_png(px, 0, depth, filters=(0, 1, 2, 3, 4)))
            self.assertEqual((w, h), (5, 3))
            want = [[(v * 255 // top,) * 3 + (255,) for v in row] for row in px]
            self.assertEqual(rows, want, depth)
        px = [[0x1234, 0xFF00, 0x00FF, 0xFFFF]]
        self.assertEqual(self.read(make_png(px, 0, 16))[2], [[(0x12,) * 3 + (255,), (255,) * 4, (0, 0, 0, 255),
                                                              (255,) * 4]])
        self.assertEqual(self.read(make_png([[0, 1, 2, 3]], 0, 2))[2][0],
                         [(0, 0, 0, 255), (85, 85, 85, 255), (170, 170, 170, 255), (255, 255, 255, 255)])

    def test_grey_trns_compares_raw_samples(self):
        rows = self.read(make_png([[0, 1, 2, 3]], 0, 2, trns=struct.pack(">H", 2)))[2]
        self.assertEqual([p[3] for p in rows[0]], [255, 255, 0, 255])
        rows = self.read(make_png([[0x00FF, 0x0000, 0xFFFF, 0x00FE]], 0, 16, trns=struct.pack(">H", 0x00FF)))[2]
        self.assertEqual(rows[0], [(0, 0, 0, 0), (0, 0, 0, 255), (255, 255, 255, 255), (0, 0, 0, 255)])

    def test_rgb_and_trns(self):
        px = [[(10, 20, 30), (0, 0, 0), (255, 0, 255)]]
        rows = self.read(make_png(px, 2, 8, trns=struct.pack(">HHH", 0, 0, 0), filters=(4,)))[2]
        self.assertEqual(rows[0], [(10, 20, 30, 255), (0, 0, 0, 0), (255, 0, 255, 255)])
        px = [[(0x0100, 0, 0), (0x01FF, 0, 0), (0xABCD, 0x1200, 0xFFFF)]]
        rows = self.read(make_png(px, 2, 16, trns=struct.pack(">HHH", 0x0100, 0, 0)))[2]
        self.assertEqual(rows[0], [(1, 0, 0, 0), (1, 0, 0, 255), (0xAB, 0x12, 0xFF, 255)])

    def test_indexed(self):
        pal = [(1, 2, 3), (4, 5, 6), (7, 8, 9), (250, 251, 252)]
        for depth in (1, 2, 4, 8):
            n = min(4, 1 << depth)
            px = [[(x * 3 + y) % n for x in range(7)] for y in range(2)]
            rows = self.read(make_png(px, 3, depth, palette=pal[:n], trns=b"\x00\x80", filters=(2, 3)))[2]
            alpha = (0, 128, 255, 255)
            self.assertEqual(rows, [[pal[i] + (alpha[i],) for i in row] for row in px], depth)

    def test_alpha_types(self):
        px = [[(200, 10), (7, 255), (9, 127)]]
        self.assertEqual(self.read(make_png(px, 4, 8))[2][0],
                         [(200, 200, 200, 10), (7, 7, 7, 255), (9, 9, 9, 127)])
        px = [[(0xC800, 0x8000), (0x0700, 0x7FFF)]]
        self.assertEqual(self.read(make_png(px, 4, 16))[2][0], [(200, 200, 200, 128), (7, 7, 7, 127)])
        px = [[(1, 2, 3, 4), (250, 251, 252, 253)]]
        self.assertEqual(self.read(make_png(px, 6, 8, filters=(1,)))[2][0], [(1, 2, 3, 4), (250, 251, 252, 253)])
        px = [[(0x0102, 0x0304, 0x0506, 0x0708), (0xFFFF, 0, 0x8000, 0x80FF)]]
        self.assertEqual(self.read(make_png(px, 6, 16))[2][0], [(1, 3, 5, 7), (255, 0, 128, 128)])

    def test_filters_and_chunks(self):
        rnd = lcg(11)
        px = [[tuple(next(rnd) % 256 for _ in range(4)) for _ in range(9)] for _ in range(7)]
        want = self.read(make_png(px, 6, 8))
        for filters in ((1,), (2,), (3,), (4,), (0, 1, 2, 3, 4)):
            got = self.read(make_png(px, 6, 8, filters=filters, idat_parts=4,
                                     extra=(("tEXt", b"Comment\x00hi"), ("gAMA", struct.pack(">I", 45455)))))
            self.assertEqual(got, want, filters)

    def test_rejections(self):
        good = make_png([[(1, 2, 3, 255)] * 2] * 2)
        chunks = png_chunk_list(good)
        bad = [
            b"GIF89a" + bytes(40),
            good[:30] + good[31:],
            png_join([c for c in chunks if c[0] != "IDAT"]),
            make_png([[(1, 2, 3, 255)] * 2] * 2, interlace=True),
            make_png([[0, 1]], 3, 8),                                    # no PLTE
            make_png([[1]], 2, 4, ihdr=struct.pack(">IIBBBBB", 1, 1, 4, 2, 0, 0, 0), raw=b"\0\0\0"),
            png_join([chunks[0], ("IDAT", b"not zlib"), ("IEND", b"")]),
            make_png([[(1, 2, 3, 255)] * 2] * 2, raw=b"\x05" + bytes(8) + b"\x00" + bytes(8)),
            make_png([[(1, 2, 3, 255)] * 2] * 2, extra=(("ZZZZ", b""),)),
        ]
        for i, data in enumerate(bad):
            with self.assertRaises(dp.DanceError, msg=str(i)):
                dp.read_png(data)
        with self.assertRaises(dp.DanceError) as cm:
            dp.read_png(make_png([[(1, 2, 3, 255)] * 2] * 2, interlace=True))
        self.assertEqual(cm.exception.args[0], "save without interlacing")


class SheetErrorTest(DpCase):
    """`<sheet>: error:` messages, through compile_dir (C.6)."""

    def sheet_error(self, png, message, ini=None, sheet="sheet.png"):
        r = self.compile(ini or ini_text(), png)
        self.assertPackError(r, "%s: error: %s" % (sheet, message))

    def test_damaged(self):
        good = base_png()
        chunks = png_chunk_list(good)
        idat = next(i for i, c in enumerate(chunks) if c[0] == "IDAT")

        def without(tag):
            return png_join([c for c in chunks if c[0] != tag])

        bad_crc = bytearray(good)
        pos = good.index(b"IDAT")
        n = struct.unpack_from(">I", good, pos - 4)[0]
        bad_crc[pos + 4 + n] ^= 0xFF
        raw = png_raw(base_sheet().rgba(), 6, 8)
        cases = [
            (b"GIF89a" + bytes(64), "not a PNG"),
            (bytes(bad_crc), "damaged PNG (bad CRC in IDAT)"),
            (good[:pos + 4 + n // 2], "damaged PNG (truncated)"),
            (png_join(chunks[1:]), "damaged PNG (no IHDR)"),
            (without("IDAT"), "damaged PNG (no IDAT)"),
            (png_join([chunks[0]] + [("IDAT", b"not zlib data")] + chunks[idat + 1:]), "damaged PNG (bad zlib data)"),
            (make_png(base_sheet().rgba(), raw=b"\x07" + raw[1:]), "damaged PNG (bad filter 7)"),
            (make_png(base_sheet().rgba(), extra=(("ZZZZ", b"x"),)), "damaged PNG (unknown critical chunk ZZZZ)"),
            (make_png(base_sheet().rgba(), raw=raw + b"\x00"), "damaged PNG (more than %d bytes of pixels)" % len(raw)),
            (make_png(base_sheet().rgba(), raw=raw[:-1]), "damaged PNG (%d bytes of pixels, not %d)" % (
                len(raw) - 1, len(raw))),
            (png_join([("IHDR", chunks[0][1][:12])] + chunks[1:]), "damaged PNG (bad IHDR)"),
        ]
        rows = [[0] * 32] * 24
        cases.append((make_png(rows, 3, 8), "damaged PNG (no PLTE)"))
        # C.6's bad PLTE: not 1-256 entries of 3 bytes, or a pixel indexing past it
        pal = [(0, 0, 0), (255, 255, 255)]
        short = [(t, b"\0\0\0\xff\xff" if t == "PLTE" else b) for t, b in png_chunk_list(make_png(rows, 3, 8, palette=pal))]
        cases.append((png_join(short), "damaged PNG (bad PLTE)"))
        cases.append((make_png(rows, 3, 8, palette=[(0, 0, 0)] * 257), "damaged PNG (bad PLTE)"))
        cases.append((make_png([[0] * 31 + [2]] + [[0] * 32] * 23, 3, 8, palette=pal), "damaged PNG (bad PLTE)"))
        for data, message in cases:
            with self.subTest(message):
                self.sheet_error(data, message)

    def test_inflate_is_bounded(self):
        # C.4: the reader inflates at most one byte more than the image needs,
        # so a small file of zeros can't take gigabytes of memory.
        z, parts = zlib.compressobj(9), []
        chunk = bytes(1 << 20)
        for _ in range(16):
            parts.append(z.compress(chunk))
        parts.append(z.flush())
        ihdr = struct.pack(">IIBBBBB", 64, 64, 8, 6, 0, 0, 0)
        bomb = PNG_SIG + png_chunk("IHDR", ihdr) + png_chunk("IDAT", b"".join(parts)) + png_chunk("IEND", b"")
        self.assertLess(len(bomb), 100000)
        want = 64 * (64 * 4 + 1)
        import tracemalloc
        tracemalloc.start()
        try:
            with self.assertRaises(dp.DanceError) as cm:
                dp.read_png(bomb)
            peak = tracemalloc.get_traced_memory()[1]
        finally:
            tracemalloc.stop()
        self.assertEqual(cm.exception.args[0], "damaged PNG (more than %d bytes of pixels)" % want)
        self.assertLess(peak, 4 << 20)       # the 16 MiB of zeros are never inflated

    def test_types_and_sizes(self):
        for ctype, depth in ((5, 8), (2, 4), (3, 16), (0, 3), (4, 4), (6, 2)):
            ihdr = struct.pack(">IIBBBBB", 32, 24, depth, ctype, 0, 0, 0)
            with self.subTest((ctype, depth)):
                self.sheet_error(make_png([[0]], 0, 8, ihdr=ihdr, raw=b"\0" * 10),
                                 "colour type %d at %d bits isn't valid PNG" % (ctype, depth))
        self.sheet_error(base_sheet().png(interlace=True), "save without interlacing")
        wide = Sheet(129, 1, 8, 8)
        figure(wide, "A1")
        self.sheet_error(wide.png(), "1032x8: at most 1024x1024")
        odd = Sheet(1, 1, 20, 8)
        figure(odd, "A1")
        self.sheet_error(odd.png(), "20x8 isn't a whole number of 8x8 cells")

    def test_sheet_in_a_subfolder(self):
        ini = ini_text(dancer_keys({"sheet": "art/robo.png"}))
        d = self.dancer(ini, base_sheet().keyed_png(), sheet="art/robo.png")
        r = dp.compile_dir(str(d))
        self.assertPackError(r, "art/robo.png: error: every cell is fully opaque: export with transparency, or set "
                                "key = #ff00ff (the top-left pixel of A1)")

    def test_fully_opaque(self):
        # C.6: the message names the first used cell's top-left colour as the likely key
        self.sheet_error(base_sheet().keyed_png(),
                         "every cell is fully opaque: export with transparency, or set key = #ff00ff (the top-left "
                         "pixel of A1)")
        white = base_sheet().keyed_png(key=(255, 255, 255))
        self.sheet_error(white, "every cell is fully opaque: export with transparency, or set key = #ffffff (the "
                                "top-left pixel of A1)")
        # the same sheet with the key set builds, and equals the RGBA build
        r = self.compile(ini_text(dancer_keys(add=["key = #FF00ff"])), base_sheet().keyed_png(),
                         parent=self.tmp / "keyed")
        self.assertTrue(r.ok, r.messages)
        self.assertEqual(bytes(r.data), bytes(self.compile(ini_text(), parent=self.tmp / "rgba").data))

    def test_too_many_colours(self):
        s = Sheet(2, 1, 8, 8)
        cols = [(16 * i, 200, 100) for i in range(16)]
        s.fill("A1", 0, 0, 8, 2, cols[0])
        s.fill("A1", 0, 7, 8, 1, cols[1])
        for i in range(7):
            s.put("B1", i, 0, cols[2 + i])
            s.put("B1", i, 1, cols[9 + i])
        ini = ini_text(moves=[("[move base]", ["frames = A1 B1", "beats = 2", "bpm = any"])])
        # C.6's <list>: the 15 most used (a tie in scan order), then the rest summed up
        listing = ", ".join(["#00c864 16 px", "#10c864 8 px in A1"] +
                            ["#%02xc864 1 px in B1" % (16 * i) for i in range(2, 15)])
        hint = " (smoothing or anti-aliasing in the export adds colours: export at 100 % with no smoothing)"
        r = self.compile(ini, s.png())
        self.assertPackError(r, "sheet.png: error: 16 colours: at most 15. By pixel count: " + listing +
                             ", and 1 more colour of at most 1 px, in B1" + hint)

    def test_too_many_colours_summed_up(self):
        # A smoothed export: 200 colours, the merge notes left out, the rare ones summed up.
        s = Sheet(2, 1, 16, 16)
        s.fill("A1", 4, 4, 8, 12, W)
        for i in range(200):
            s.put("B1", i % 16, i // 16, (i, 255 - i, 128))
        ini = ini_text(dancer_keys({"cell": "16x16"}), [("[move base]", ["frames = A1 B1", "beats = 2", "bpm = any"])])
        r = self.compile(ini, s.png())
        self.assertFalse(r.ok)
        self.assertNoLine(r, "note: ")
        errors = [m for m in self.messages(r) if m.startswith("sheet.png: error: ")]
        self.assertEqual(len(errors), 1, r.messages)
        m = re.match(r"sheet\.png: error: (\d+) colours: at most 15\. By pixel count: (.*), and (\d+) more colours "
                     r"of at most (\d+) px each, in B1 \(smoothing or anti-aliasing in the export adds colours: "
                     r"export at 100 % with no smoothing\)$", errors[0])
        self.assertTrue(m, errors[0])
        n = int(m.group(1))
        self.assertEqual(len(m.group(2).split(", ")), 15)
        self.assertEqual(int(m.group(3)), n - 15)
        self.assertGreater(n, 15)
        self.assertTrue(m.group(2).startswith("#ffffff 96 px, "), m.group(2))

    def test_colour_not_in_palette(self):
        ini = ini_text(dancer_keys(add=["palette = #ffffff"]),
                       [("[move base]", ["frames = A1 B1", "beats = 2", "bpm = any"])])
        r = self.compile(ini)
        self.assertPackError(r, "sheet.png: error: colour #ff0000 (in A1, B1) isn't in palette =")

    def test_cells_used(self):
        s = Sheet(32, 10, 1, 1)
        s.put("A1", 0, 0, W)
        moves = [("[move m%d]" % r, ["frames = " + " ".join(cell_name(c, r) for c in range(32)), "beats = 8",
                                     "bpm = any"]) for r in range(9)]
        r = self.compile(ini_text(dancer_keys({"cell": "1x1"}), moves), s.png())
        self.assertPackError(r, "dancer.ini: error: 288 cells used: at most 256")

    def test_rest(self):
        ini = ini_text(dancer_keys(add=["rest = E1"]))
        self.assertPackError(self.compile(ini), "dancer.ini:%d: error: rest E1 isn't a cell of the 4x3 sheet"
                             % ini_line(ini, "[dancer]", "rest"))
        ini = ini_text(dancer_keys(add=["rest = A3"]))
        self.assertPackError(self.compile(ini), "dancer.ini:%d: error: the rest frame A3 is empty"
                             % ini_line(ini, "[dancer]", "rest"))


# ===========================================================================
# A.17, end to end.

class WorkedPackTest(DpCase):
    def build_dot(self, png=None, strict=False):
        return dp.compile_dir(str(self.dancer(A17_INI, png or a17_sheet().png(), name="dot")), strict=strict)

    def test_bytes_exactly(self):
        r = self.build_dot()
        self.assertTrue(r.ok, r.messages)
        self.assertEqual(bytes(r.data), a17_pack())
        self.assertEqual(list(r.skipped), [])
        self.assertLine(r, 'dancer.ini:7: move "hop": 1 beat, pace any; green: median 4 px, peak 6 of 16,000 px')
        self.assertLine(r, "dot: 1 move (0 of 1 skipped), 1 frame, 1,156 bytes")
        b = r.budgets["hop"]
        self.assertEqual((b.median, b.peak, b.light), (4, 6, "green"))
        self.assertEqual([(c.fps, c.pace, bool(c.mirrored), c.spot, c.median, c.peak) for c in b.cases],
                         [(f, p, False, 0, 4, 6) for f in (24, 30) for p in (72, 124, 176)])
        self.assertEqual(self.build_dot(strict=True).data, r.data)    # CC0-1.0 is on the allowlist

    def test_any_png_form(self):
        s = a17_sheet()
        red = [[(255, 0, 0) if p else (0, 0, 0) for p in row] for row in s.px]
        forms = [
            make_png(red, 2, 8, trns=struct.pack(">HHH", 0, 0, 0)),
            make_png([[1 if p else 0 for p in row] for row in s.px], 3, 1, palette=[(9, 9, 9), (255, 0, 0)],
                     trns=b"\x00"),
            make_png([[tuple(c * 257 for c in px) for px in row] for row in s.rgba()], 6, 16),
            s.png(filters=(4,), idat_parts=3, extra=(("tEXt", b"k\x00v"),)),
        ]
        for i, png in enumerate(forms):
            with self.subTest(i):
                self.assertEqual(bytes(self.build_dot(png).data), a17_pack())

    def test_read_pack(self):
        p = dp.read_pack(a17_pack())
        self.assertEqual((p.size, p.crc, p.id, p.name, p.author, p.licence, p.url, p.version, p.declared_moves),
                         (1156, 0x5BF1E285, "dot", "Dot", "Ana", "CC0-1.0", "", "", 1))
        self.assertEqual(tuple(p.geom), (1, 0, 4, 4, 2, 4, 0, 1, 0))
        self.assertEqual([tuple(c) for c in p.palette], [(0, 0, 0), (255, 0, 0)] + [(0, 0, 0)] * 14)
        self.assertEqual([tuple(c) for c in p.idle_palette], [(0, 0, 0), (0x74, 0x1B, 0x1B)] + [(0, 0, 0)] * 14)
        self.assertEqual(bytes(p.thumb), b"\x11" * 800)
        self.assertEqual([tuple(b) for b in p.boxes], [(1, 2, 2, 2)])
        self.assertEqual([[bytes(row) for row in g] for g in p.grids],
                         [[b"\0\0\0\0", b"\0\0\0\0", b"\0\1\1\0", b"\0\1\1\0"]])
        self.assertEqual(list(p.pose_keys), [])
        self.assertEqual(list(p.ignored), [])
        mv = p.moves[0]
        self.assertEqual((mv.name, mv.kind, mv.beats, mv.pace_lo, mv.pace_hi, mv.weight10, mv.when, mv.flags,
                          mv.repeat, mv.needs, mv.travel, mv.start_at, mv.travel_ease, mv.bounce, mv.sway),
                         ("hop", 0, 1, 0, 0, 10, 0, 0, 0, 0, 0, 0, 0, 2, 0))
        self.assertEqual((mv.from_key, mv.to_key, mv.from_key_m, mv.to_key_m), (None, None, None, None))
        self.assertEqual((tuple(mv.genres), mv.song_lo, mv.song_hi), ((), 0, 0))
        self.assertEqual([tuple(s) for s in mv.steps], [(0, False, 12, 0, 0, 0, False)])
        r = self.build_dot()
        self.assertEqual((r.pack.id, r.pack.size, r.pack.crc), ("dot", 1156, 0x5BF1E285))

    def test_info(self):
        f = self.tmp / "dot.dnc"
        f.write_bytes(a17_pack())
        code, out, err = run_main(["info", str(f)])
        self.assertEqual(code, 0, out + err)
        lines = out.splitlines()
        self.assertEqual(lines[0], "%s: MPDN v1 (reader 1), 1,156 bytes, CRC ok" % str(f).replace("\\", "/"))
        for word in ("dot", "Dot", "Ana", "CC0-1.0", "hop", "green") + TAGS:
            self.assertIn(word, out)
        bad = self.tmp / "bad.dnc"
        bad.write_bytes(a17_pack()[:-4] + b"\x01\x02\x03\x04")
        code, out, _ = run_main(["info", str(bad)])
        self.assertEqual(code, 1)
        self.assertIn("%s: error: bad checksum" % str(bad).replace("\\", "/"), out.splitlines())
        code, out, _ = run_main(["info", str(self.tmp / "none.dnc")])
        self.assertEqual(code, 1)
        self.assertIn("%s: error: no such file" % str(self.tmp / "none.dnc").replace("\\", "/"), out.splitlines())
        dropped = self.tmp / "drop.dnc"
        dropped.write_bytes(patch(a17_pack(), A17_MOV + 3, "B", 3))
        code, out, _ = run_main(["info", str(dropped)])
        self.assertEqual(code, 0)
        self.assertIn("ignored: 3 beats", out)
        self.assertIn('  move 0 "hop": 1 beat, pace any, 1 step; green: median 4 px, peak 6 of 16,000 px',
                      run_main(["info", str(f)])[1].splitlines())
        # C.6: a folder isn't a pack
        code, out, _ = run_main(["info", str(self.tmp)])
        self.assertEqual(code, 1)
        self.assertEqual(out.splitlines(), ["%s: error: a folder, not a .dnc pack" % str(self.tmp).replace("\\", "/")])


# ===========================================================================
# read_pack: A.15's rejections and drops on patched copies of A.17's pack.

def a17_with(**chunks):
    """A.17's pack reassembled with some chunks replaced (or extra ones added)."""
    data = a17_pack()
    p = Mpdn(data)
    out = [(tag, chunks.pop(tag, p.chunk[tag])) for tag in TAGS]
    return assemble_mpdn(out + sorted(chunks.items()))


class ReadPackTest(DpCase):
    def reason(self, data):
        with self.assertRaises(dp.DanceError) as cm:
            dp.read_pack(data)
        return cm.exception.args[0]

    def test_rejections(self):
        d = a17_pack()
        cases = [
            ("too short", d[:31]),
            ("not an MPDN pack", b"MPDX" + d[4:]),
            ("needs a newer player", patch(d, 6, "<H", 2)),
            ("size 1160 isn't the file's 1156", patch(d, 8, "<I", 1160)),
            ("bad checksum", d[:12] + b"\0\0\0\0" + d[16:]),
            ("bad chunk count 0", patch(d, 16, "<H", 0)),
            ("bad chunk count 17", patch(d, 16, "<H", 17)),
            ("chunk META: misaligned", patch(d, 36, "<I", 141)),
            ("chunk FDAT: out of bounds", patch(d, 136, "<I", 100)),
            ("chunk GEOM twice", patch(d, 116, "4s", b"GEOM")),
            ("chunks META and GEOM overlap", patch(d, 48, "<I", 168)),
            ("no STEP chunk", patch(d, 116, "4s", b"STEX")),
            ("chunk META: out of bounds", patch(d, 40, "<I", 1017)),        # 140 + 1017 runs past 1156
            ("META: bad id", patch(d, 141, "3s", b"DOT")),
            ("META: bad name", patch(d, 145, "B", 0)),
            ("META: bad count", patch(d, 40, "<I", 28)),                    # the chunk ends before the count
            ("GEOM: too short", patch(d, 52, "<I", 11)),
            ("GEOM: bad scale", patch(d, 172, "B", 5)),
            ("GEOM: bad faces", patch(d, 173, "B", 3)),
            ("GEOM: bad cellW", patch(d, 174, "B", 0)),
            ("GEOM: bad cellH", patch(d, 175, "B", 161)),
            ("GEOM: bad anchorX", patch(d, 176, "B", 4)),
            ("GEOM: bad anchorY", patch(d, 177, "B", 0)),
            ("GEOM: bad shadowW", patch(d, 178, "B", 161)),
            ("GEOM: bad frame count", patch(d, 180, "<H", 0)),
            ("GEOM: bad rest frame", patch(d, 182, "<H", 1)),
            ("PALS: 95 bytes, not 96", patch(d, 64, "<I", 95)),
            ("THMB: 799 bytes, not 800", patch(d, 76, "<I", 799)),
            ("FTAB: 11 bytes for 1 frames", patch(d, 88, "<I", 11)),
            ("frame 0: bad box", patch(d, A17_FTAB, "B", 3)),
            ("frame 0: data out of bounds", patch(d, A17_FTAB + 8, "<H", 7)),
            ("frame 0: bad data", patch(d, A17_FDAT + 2, "B", 0x21)),
            ("frame 0: bad data", patch(d, A17_FTAB + 8, "<H", 5)),          # the last token past len
            ("the rest frame is empty", patch(d, A17_FTAB, "<BBBBIHH", 0, 0, 0, 0, 0, 0, 0)),
            ("STRS: too short", patch(d, 100, "<I", 3)),
            ("MOVS: too short", patch(d, 112, "<I", 3)),
            ("MOVS: record length 31", patch(d, 1105, "B", 31)),
            ("STEP: 7 bytes", patch(d, 124, "<I", 7)),
            # A.3's bounds in an order that can't wrap in 32-bit arithmetic: off <= total first
            ("chunk STEP: out of bounds", patch(d, 120, "<II", 0xFFFFFFF0, 0)),
            ("chunk STEP: out of bounds", patch(d, 120, "<II", 0xFFFFFFF0, 0x20)),
            ("chunk META: out of bounds", patch(d, 36, "<I", 32)),               # inside the directory
            # A.9's likewise: an FTAB off near 2^32 whose 32-bit sum with len wraps to 2
            ("frame 0: data out of bounds", patch(d, A17_FTAB + 4, "<IH", 0xFFFFFFFC, 6)),
            # A.10: skip + the runs <= w on every row, a row with no tokens too
            ("frame 0: bad data", a17_with(FDAT=bytes.fromhex("09 00 00 01 11"),
                                           FTAB=bytes.fromhex("01 02 02 02 00 00 00 00 05 00 00 00"))),
            # A.15's note: GEOM in table order; anchorY's two stage limits read as anchorY
            ("GEOM: bad anchorY", patch(d, 175, "B", 15)),                        # 11 rows under the ground
            ("GEOM: bad anchorY", patch(patch(patch(d, 172, "B", 4), 175, "B", 40), 177, "B", 39)),   # 156 > 153
            # the overlap names the earlier directory entry, then the later; missing tags in A.4's order
            ("chunks STRS and MOVS overlap", patch(d, 96, "<I", 1104)),
            ("no META chunk", patch(patch(d, 32, "4s", b"XXXX"), 44, "4s", b"YYYY")),
            ("no GEOM chunk", patch(patch(d, 44, "4s", b"XXXX"), 116, "4s", b"YYYY")),
            # frame indices count from 0, as FTAB does
            ("frame 1: bad box", a17_with(GEOM=bytes.fromhex("01 00 04 04 02 04 00 00 02 00 00 00"),
                                          FTAB=bytes.fromhex("01 02 02 02 00 00 00 00 06 00 00 00"
                                                             "03 00 02 01 00 00 00 00 00 00 00 00"))),
            # A.6: cellW and cellH are 1 .. 160 // scale, so 0 is its own field's failure
            ("GEOM: bad cellH", patch(d, 175, "B", 0)),
            # A.12: the MOVS head in order: the count before the records' size
            ("MOVS: 65 moves", patch(d, 1104, "B", 65)),                         # one record, not 65
            ("MOVS: too short", patch(d, 1104, "B", 2)),                         # 4 + 2 * 32 > 36
            # A.1: a string ends in a NUL: the id's replaced by "x"
            ("META: bad id", patch(d, 144, "B", 0x78)),
        ]
        hop = Mpdn(d).chunk["MOVS"][4:]
        cases.append(("MOVS: 65 moves", a17_with(MOVS=bytes([65, 32, 0, 0]) + hop * 65)))
        for want, data in cases:
            with self.subTest(want):
                self.assertEqual(self.reason(data), want)
        # P = 2: "hop" reads as key 1, key 2 runs past the end (A.15's note: ids from 1)
        self.assertEqual(self.reason(patch(d, A17_STRS, "B", 2)), "STRS: bad pose key 2")
        big = a17_with(XTRA=bytes(131072))
        self.assertEqual(self.reason(big), "too big (%d bytes)" % len(big))      # plain numbers (A.15)

    def test_order(self):
        d = a17_pack()
        self.assertEqual(self.reason(patch(d, 6, "<HI", 2, 1160)), "needs a newer player")
        two = patch(patch(d, 172, "B", 5), 173, "B", 3)
        self.assertEqual(self.reason(two), "GEOM: bad scale")

    def test_accepted(self):
        d = a17_pack()
        self.assertEqual(list(dp.read_pack(d).ignored), [])
        p = dp.read_pack(d[:12] + b"\0\0\0\0" + d[16:], check_crc=False)
        self.assertEqual(p.id, "dot")
        for extra in ({"XTRA": b"abc"}, {"STAG": b"x" * 8, "SPOT": b""}):
            self.assertEqual(dp.read_pack(a17_with(**extra)).id, "dot")
        # a zero-size chunk overlaps nothing, even inside another chunk
        data = a17_with(ZERO=b"")
        entry = 32 + 12 * 9
        self.assertEqual(data[entry:entry + 4], b"ZERO")
        ftab = dict((t, o) for t, o, _ in Mpdn(data, canonical=False).dir)["FTAB"]
        self.assertEqual(dp.read_pack(patch(data, entry + 4, "<I", ftab + 4)).id, "dot")
        # format, flags and reserved bytes are ignored; a longer GEOM, META bytes past the count too
        self.assertEqual(dp.read_pack(patch(d, 4, "<H", 7)).id, "dot")
        self.assertEqual(dp.read_pack(patch(d, 18, "<HI", 0xFFFF, 0xFFFFFFFF)).id, "dot")
        p = Mpdn(d)
        self.assertEqual(dp.read_pack(a17_with(GEOM=p.chunk["GEOM"] + b"more", META=p.chunk["META"] + b"!")).id, "dot")
        # a longer MOVS record: the reader takes the first 32 B
        movs = bytes([1, 40, 0, 0]) + p.chunk["MOVS"][4:] + b"\xff" * 8
        self.assertEqual(list(dp.read_pack(a17_with(MOVS=movs)).ignored), [])
        # genre runs: a valid string referenced from MOVS (the name, "hop") is fine
        self.assertEqual(list(dp.read_pack(patch(d, A17_MOV + 20, "<HB", 4, 1)).ignored), [])
        # the rest frame's bit 15 is masked off (A.6)
        self.assertEqual(list(dp.read_pack(patch(d, 182, "<H", 0x8000)).ignored), [])
        # A.15: an idle move's genre test reads genreCount only; genreFirst isn't read
        idle = d
        for off, fmt, val in ((A17_MOV + 2, "B", 1), (A17_MOV + 3, "B", 0), (A17_MOV + 14, "B", 0),
                              (A17_MOV + 7, "B", 3), (A17_STEP + 2, "<H", 60)):
            idle = patch(idle, off, fmt, val)
        self.assertEqual(list(dp.read_pack(idle).ignored), [])
        self.assertEqual(list(dp.read_pack(patch(idle, A17_MOV + 20, "<H", 4)).ignored), [])
        # A.15's note: every reserved byte or bit is ignored
        for off, fmt, val in ((179, "B", 5), (A17_FTAB + 10, "<H", 0xFFFF), (A17_STRS + 1, "3s", b"\x01\x02\x03"),
                              (1106, "<H", 0xFFFF), (A17_MOV + 28, "<I", 0xFFFFFFFF), (A17_MOV + 8, "B", 0xC0),
                              (A17_STEP + 7, "B", 0xFE)):
            with self.subTest(off=off):
                self.assertEqual(list(dp.read_pack(patch(d, off, fmt, val)).ignored), [])

    def test_accepted_token_sequences(self):
        # A.10: the dry run accepts any tokens within the rules, and A.9 asks no
        # tight box: a leading gap token, a trailing one, a run split in two, a
        # loose box, and a row whose skip is w with no tokens all draw A.17's dot.
        dot = [b"\0\0\0\0", b"\0\0\0\0", b"\0\1\1\0", b"\0\1\1\0"]
        for box, rows in (((0, 2, 4, 2), "00 02 00 11  00 02 00 11"),       # leading gap, loose box
                          ((1, 2, 3, 2), "00 02 11 00  00 02 11 00"),       # trailing gap
                          ((1, 2, 2, 2), "00 02 01 01  00 02 01 01")):      # 2 px split 1 + 1
            data = bytes.fromhex(rows)
            ftab = bytes(box) + struct.pack("<IHH", 0, len(data), 0)
            with self.subTest(rows):
                p = dp.read_pack(a17_with(FTAB=ftab, FDAT=data))
                self.assertEqual(list(p.ignored), [])
                self.assertEqual([bytes(r) for r in p.grids[0]], dot)
        data = bytes.fromhex("02 00 00 01 11")                              # skip = w, n = 0: a blank row
        p = dp.read_pack(a17_with(FTAB=bytes((1, 2, 2, 2)) + struct.pack("<IHH", 0, 5, 0), FDAT=data))
        self.assertEqual([bytes(r) for r in p.grids[0]], [b"\0\0\0\0", b"\0\0\0\0", b"\0\0\0\0", b"\0\1\1\0"])

    def test_drops(self):
        d, mv, st = a17_pack(), A17_MOV, A17_STEP
        idle = [(mv + 2, "B", 1), (mv + 3, "B", 0), (mv + 14, "B", 0)]
        cases = [
            ("bad name", [(mv, "<H", 3)]),
            ("bad name", [(mv, "<H", 1090)]),
            ("bad kind", [(mv + 2, "B", 2)]),
            ("bad kind", [(mv + 2, "B", 2), (mv + 3, "B", 3)]),       # the first check wins
            ("bad steps", [(mv + 23, "B", 0)]),
            ("bad steps", [(mv + 24, "<H", 1)]),
            ("bad steps", [(mv + 23, "B", 33)]),
            ("frame 1 out of range", [(st, "<H", 1)]),
            ("3 beats", [(mv + 3, "B", 3)]),
            ("step 1: 2 ticks", [(st + 2, "<H", 2)]),
            ("13 of 12 ticks", [(st + 2, "<H", 13)]),
            ("step 1: 100 ms", idle + [(st + 2, "<H", 10), (mv + 7, "B", 3)]),
            ("step 1: 5010 ms", idle + [(st + 2, "<H", 501), (mv + 7, "B", 3)]),     # plain numbers (A.15)
            ("400 ms in all", idle + [(st + 2, "<H", 40), (mv + 7, "B", 3)]),
            ("beat fields on a no-BPM move", [(mv + 2, "B", 1), (st + 2, "<H", 60), (mv + 7, "B", 3)]),
            ("bad when", idle + [(st + 2, "<H", 60)]),
            ("bad when", idle + [(st + 2, "<H", 60), (mv + 7, "B", 4)]),
            ("bad when", [(mv + 7, "B", 1)]),
            ("pace 30-100", [(mv + 4, "BB", 30, 100)]),
            ("pace 0-100", [(mv + 4, "BB", 0, 100)]),
            ("pace 120-100", [(mv + 4, "BB", 120, 100)]),
            ("pace 36-201", [(mv + 4, "BB", 36, 201)]),
            ("range 185-200 unreachable", [(mv + 4, "BB", 185, 200)]),
            ("weight 0", [(mv + 6, "B", 0)]),
            ("weight 101", [(mv + 6, "B", 101)]),
            ("repeat 9", [(mv + 9, "B", 9)]),
            ("repeat 4", [(mv + 3, "B", 8), (st + 2, "<H", 96), (mv + 9, "B", 4)]),
            ("bounce 17", [(mv + 14, "B", 17)]),
            ("sway 17", [(mv + 15, "B", 17)]),
            ("travel 1", [(mv + 11, "b", 1)]),
            ("travel 3", [(mv + 3, "B", 2), (st + 2, "<H", 24), (mv + 11, "b", 3)]),
            ("travel -3", [(mv + 3, "B", 2), (st + 2, "<H", 24), (mv + 11, "b", -3)]),
            ("bad start_at", [(mv + 12, "B", 5)]),
            ("bad ease", [(mv + 13, "B", 5)]),
            ("bad ease", [(st + 6, "B", 5)]),
            ("bad place", [(mv + 8, "B", 12)]),
            ("offset 49,0", [(st + 4, "b", 49)]),
            ("offset 0,-49", [(st + 5, "b", -49)]),
            ("bad pose key", [(mv + 16, "B", 1)]),
            ("bad pose key", [(mv + 19, "B", 1)]),
            ("bad genre", [(mv + 22, "B", 9)]),
            ("bad genre", [(mv + 20, "<HB", 0, 1)]),
            ("bad genre", [(mv + 20, "<HB", 4, 2)]),                      # the run's second tag is past the end
            ("song_bpm needs track BPM data: skipped in this version", [(mv + 10, "B", 1), (mv + 26, "BB", 160, 180)]),
            ("needs 0x02", [(mv + 10, "B", 2)]),
            ("needs 0xA0", [(mv + 10, "B", 0xA0)]),                       # uppercase hex digits (C.6's note)
            # A.15: each beat field on a no-BPM move
            ("beat fields on a no-BPM move", idle + [(st + 2, "<H", 60), (mv + 7, "B", 3), (mv + 8, "B", 4)]),
            ("beat fields on a no-BPM move", idle + [(st + 2, "<H", 60), (mv + 7, "B", 3), (mv + 8, "B", 2)]),
            ("beat fields on a no-BPM move", idle + [(st + 2, "<H", 60), (mv + 7, "B", 3), (mv + 20, "<HB", 4, 1)]),
            ("beat fields on a no-BPM move", idle + [(st + 2, "<H", 60), (mv + 7, "B", 3), (mv + 11, "b", 1)]),
            ("beat fields on a no-BPM move", idle + [(st + 2, "<H", 60), (mv + 7, "B", 3), (mv + 15, "B", 1)]),
            ("beat fields on a no-BPM move", idle + [(st + 2, "<H", 60), (mv + 7, "B", 3), (mv + 9, "B", 1)]),
            ("beat fields on a no-BPM move", idle + [(st + 2, "<H", 60), (mv + 7, "B", 3), (mv + 4, "BB", 90, 100)]),
            # A.11: o + 2 + n <= size: the name's NUL in STRS's padding is past its end
            ("bad name", [(100, "<I", 8)]),
            # A.1/A.11's order: o + 2 <= size before the length byte at o is read. STRS is 9 B,
            # so o = 8 and o = 9 (where a string after "hop" would start) and o far past the
            # pack are all bad without reading past STRS (10.3's accessor checks a port).
            ("bad name", [(mv, "<H", 8)]),
            ("bad name", [(mv, "<H", 9)]),
            ("bad name", [(mv, "<H", 0xFFFF)]),
            ("bad genre", [(mv + 20, "<HB", 9, 1)]),
            ("bad genre", [(mv + 20, "<HB", 0xFFFF, 1)]),
        ]
        for want, edits in cases:
            data = d
            for off, fmt, *vals in edits:
                data = patch(data, off, fmt, *vals)
            with self.subTest(want, edits=edits):
                p = dp.read_pack(data)
                self.assertEqual([tuple(x) for x in p.ignored], [(0, want)])

    def test_drops_rebuilt(self):
        # Drops that need a chunk to grow, so the pack is reassembled (a17_with).
        p = Mpdn(a17_pack())
        movs = bytearray(p.chunk["MOVS"])
        # A.11: a tag that is a valid string but folds to nothing is a bad genre; "ab" is fine
        for tag, want in ((b"?!", [(0, "bad genre")]), (b"ab", [])):
            strs = p.chunk["STRS"] + bytes((len(tag),)) + tag + b"\0"
            m = bytearray(movs)
            struct.pack_into("<HB", m, 4 + 20, 9, 1)
            with self.subTest(tag):
                self.assertEqual([tuple(x) for x in dp.read_pack(a17_with(STRS=strs, MOVS=bytes(m))).ignored], want)
        # A.15: an idle move's steps sum to at most 12,000 ms: 400 + 400 + 401 units is 12,010
        m = bytearray(movs)
        struct.pack_into("<BBBB", m, 4 + 2, 1, 0, 0, 0)       # kind idle, beats 0, no pace
        struct.pack_into("<B", m, 4 + 7, 3)                    # when any
        struct.pack_into("<B", m, 4 + 14, 0)                   # bounce 0
        struct.pack_into("<B", m, 4 + 23, 3)                   # 3 steps
        for durs, want in (((400, 400, 401), [(0, "12010 ms in all")]), ((400, 400, 400), [])):
            step = b"".join(struct.pack("<HHbbBB", 0, d, 0, 0, 0, 0) for d in durs)
            with self.subTest(durs):
                self.assertEqual([tuple(x) for x in dp.read_pack(a17_with(MOVS=bytes(m), STEP=step)).ignored], want)

    def test_strings_read_inside_strs(self):
        # A.1's order (o + 2 <= size before the length byte at o is read), with STRS
        # moved to the end of the file and grown to 12 B ("hop" at 4, "x" at 9), so a
        # byte past STRS is past the data too: reading one would raise IndexError.
        p = Mpdn(a17_pack())
        parts = dict((t, p.chunk[t]) for t in TAGS)
        parts["STRS"] += b"\x01x\x00"
        order = [t for t in TAGS if t != "STRS"] + ["STRS"]
        for name, genre, want in ((4, (0, 0), []), (9, (0, 0), []), (12, (0, 0), [(0, "bad name")]),
                                  (0xFFFF, (0, 0), [(0, "bad name")]), (4, (9, 1), []),
                                  (4, (9, 2), [(0, "bad genre")]), (4, (12, 1), [(0, "bad genre")])):
            m = bytearray(parts["MOVS"])
            struct.pack_into("<H", m, 4, name)
            struct.pack_into("<HB", m, 4 + 20, *genre)
            data = assemble_mpdn([(t, bytes(m) if t == "MOVS" else parts[t]) for t in order])
            tag, off, size = struct.unpack_from("<4sII", data, 32 + 12 * 8)
            self.assertEqual((tag, off + size, size), (b"STRS", len(data), 12))
            with self.subTest(name=name, genre=genre):
                self.assertEqual([tuple(x) for x in dp.read_pack(data).ignored], want)

    def test_card_budget_and_spots(self):
        # A red move kept by heavy = yes loads; without the flag the reader drops it.
        s = Sheet(2, 1, 40, 40)
        s.fill("A1", 0, 0, 40, 40, R)
        s.fill("B1", 18, 30, 4, 8, W)
        keys = dancer_keys({"cell": "40x40"}, add=["scale = 4", "anchor = 20,38", "rest = B1"])
        ini = ini_text(keys, [("[move held]", ["frames = A1 A1", "beats = 2", "bpm = any", "heavy = yes"]),
                              ("[move walk]", ["frames = B1 B1", "beats = 2", "bpm = any", "travel = 2"])])
        r = self.compile(ini, s.png())
        self.assertTrue(r.ok, r.messages)
        p = Mpdn(r.data)
        self.assertEqual(list(dp.read_pack(r.data).ignored), [])
        rec = 4 + p.dir[TAGS.index("MOVS")][1]
        flags = p.moves[0].flags
        self.assertEqual(flags & 16, 16)
        got = dp.read_pack(patch(r.data, rec + 8, "B", flags & ~16)).ignored
        self.assertEqual(len(got), 1)
        self.assertEqual(got[0][0], 0)
        self.assertEqual(got[0][1], "budget red (25600 px)")
        # walk travels 2 from -1; start_at centre leaves it no spot
        got = dp.read_pack(patch(r.data, rec + 32 + 12, "B", 1)).ignored
        self.assertEqual([tuple(x) for x in got], [(1, "travels from no spot")])

    def test_spots_only_in_orientations_it_plays(self):
        # B.11, A.15: start spots count only in an orientation the move plays in. The test
        # dancer's walk (MOVS 6) without its mirror flag, from the centre, keyed 10 native
        # px right on every step: unmirrored its extent is (-24, 68), so 256 + 68 > 320 and
        # there is no spot; mirrored it is (-64, 28) with a spot at 0, but it never plays
        # mirrored, so the reader drops it. With the flag back it loads.
        data = testpack_result().data
        p = Mpdn(data)
        walk = [m.name_text for m in p.moves].index("walk")
        self.assertEqual(walk, 6)
        m = p.moves[walk]
        rec = p.dir[TAGS.index("MOVS")][1] + 4 + 32 * walk
        step = p.dir[TAGS.index("STEP")][1]
        self.assertEqual(m.flags & 1, 1)
        d = patch(data, rec + 8, "B", m.flags & ~1)
        d = patch(d, rec + 12, "B", 1)
        for k in range(m.step_count):
            d = patch(d, step + 8 * (m.step_first + k) + 4, "b", 10)
        q = dp.read_pack(d)
        mv = q.moves[walk]
        self.assertEqual((dp.extent(q.geom, q.boxes, mv, False), dp.extent(q.geom, q.boxes, mv, True)),
                         ((-24, 68), (-64, 28)))
        self.assertEqual((dp.start_spots(q.geom, q.boxes, mv, False), dp.start_spots(q.geom, q.boxes, mv, True)),
                         ([], [0]))
        self.assertEqual([tuple(x) for x in q.ignored if x[0] == walk], [(walk, "travels from no spot")])
        q = dp.read_pack(patch(d, rec + 8, "B", m.flags))
        self.assertEqual([tuple(x) for x in q.ignored if x[0] == walk], [])

    def test_robustness(self):
        d = a17_pack()
        for n in range(0, len(d), 4):
            data = d[:n] if n < 32 else patch(d[:n], 8, "<I", n)
            with self.assertRaises(dp.DanceError, msg=str(n)):
                dp.read_pack(data)
        # Every single-byte flip with the CRC recomputed: a Pack or a DanceError, nothing else.
        skip = set(range(184 + 6, 184 + 96)) | set(range(280 + 4, 1080))   # PALS and THMB: nothing to check
        for i in range(16, len(d)):
            if i in skip:
                continue
            data = bytearray(d)
            data[i] ^= 0xFF if i % 2 else 0x01
            try:
                dp.read_pack(with_crc(data))
            except dp.DanceError:
                pass


# ===========================================================================
# dancer.ini (C.3) and the pack errors of C.6.

class ManifestErrorTest(DpCase):
    def at(self, ini, section, key, message):
        return "dancer.ini:%d: error: %s" % (ini_line(ini, section, key), message)

    def test_folder(self):
        d = self.tmp / "robo"
        d.mkdir()
        r = dp.compile_dir(str(d))
        self.assertFalse(r.ok)
        line = [m for m in self.messages(r) if m.endswith(": error: no dancer.ini")]
        self.assertEqual(len(line), 1, r.messages)
        self.assertNotIn("\\", line[0])
        self.assertIn("robo", line[0])
        # each in its own parent: Windows folder names ignore case
        for i, name in enumerate(("Robo", "-robo", "a" * 24, "ro_bo", "robö")):
            with self.subTest(name):
                d = self.dancer(ini_text(), base_png(), name=name, parent=self.tmp / ("bad%d" % i))
                r = dp.compile_dir(str(d))
                want = (': error: the folder name "%s" can\'t be an id: use 1-23 of a-z, 0-9 and -, starting '
                        'with a letter or digit' % name)
                self.assertTrue(any(m.endswith(want) for m in self.messages(r)), r.messages)
                self.assertFalse(r.ok)
        for i, name in enumerate(("a" * 23, "9lives", "a-b-")):
            d = self.dancer(ini_text(), base_png(), name=name, parent=self.tmp / ("good%d" % i))
            r = dp.compile_dir(str(d))
            self.assertTrue(r.ok, name)
            self.assertEqual(Mpdn(r.data).id, name)

    def test_read_failures(self):
        # C.6: a file that is there but can't be read gives the operating system's
        # reason. read_bytes is made to fail for one file name (permissions can't be
        # relied on: an administrator, or root in Docker, reads anything).
        real = Path.read_bytes

        def failing(name):
            def read_bytes(path):
                if path.name == name:
                    raise PermissionError(13, "Permission denied")
                return real(path)
            return read_bytes

        d = self.dancer(ini_text(), base_png())
        with mock.patch.object(Path, "read_bytes", failing("dancer.ini")):
            self.assertPackError(dp.compile_dir(str(d)), "dancer.ini: error: can't read it (Permission denied)")
        with mock.patch.object(Path, "read_bytes", failing("sheet.png")):
            self.assertPackError(dp.compile_dir(str(d)), "sheet.png: error: can't read it (Permission denied)")
        self.assertTrue(dp.compile_dir(str(d)).ok)
        f = self.tmp / "dot.dnc"
        f.write_bytes(a17_pack())
        with mock.patch.object(Path, "read_bytes", failing("dot.dnc")):
            code, out, _ = run_main(["info", str(f)])
        self.assertEqual(code, 1)
        self.assertEqual(out.splitlines(), ["%s: error: can't read it (Permission denied)" % str(f).replace("\\", "/")])

    def test_parser_errors(self):
        good = ini_text()
        lines = good.split("\n")
        cases = []
        raw = good.encode("utf-8").replace(b"author = Ana", b"author = An\xff")
        cases.append(("utf8", None, raw, "dancer.ini:3: error: not UTF-8: save it as UTF-8"))
        ini = good + "\n[move base]\nframes = A1\nbeats = 1\nbpm = any\n"
        cases.append(("dup-section", ini, None,
                      "dancer.ini:%d: error: duplicate section [move base]" % ini_line(ini, "[move base]", nth=2)))
        ini = ini_text(dancer_keys(add=["name = Other"]))
        cases.append(("dup-key", ini, None, "dancer.ini:%d: error: duplicate key \"name\" in [dancer]"
                      % (ini.split("\n").index("name = Other") + 1)))
        ini = ini_text(moves=[("[move base]", ["frames = A1", "beats = 1", "bpm = any", "beats = 2"])])
        cases.append(("dup-key-move", ini, None, "dancer.ini:%d: error: duplicate key \"beats\" in [move base]"
                      % (len(ini.split("\n")) - 1)))
        ini = "name = Robo\n" + good
        cases.append(("outside", ini, None, 'dancer.ini:1: error: "name = Robo" is outside any section'))
        ini = ini_text(dancer_keys(add=["just words"]))
        cases.append(("not-kv", ini, None, 'dancer.ini:%d: error: can\'t read "just words": not key = value'
                      % (ini.split("\n").index("just words") + 1)))
        ini = ini_text(moves=[BASE_MOVE, ("[move]", ["frames = A1", "beats = 1", "bpm = any"])])
        cases.append(("no-name", ini, None,
                      "dancer.ini:%d: error: a move needs a name: [move <name>]" % ini_line(ini, "[move]")))
        ini = ini_text(moves=[BASE_MOVE, ("[MOVE base]", ["frames = B1", "beats = 1", "bpm = any"])])
        cases.append(("dup-move", ini, None,
                      'dancer.ini:%d: error: duplicate move "base"' % ini_line(ini, "[MOVE base]")))
        ini = "\n".join(lines[len(DANCER_KEYS) + 2:])
        cases.append(("no-dancer", ini, None, "dancer.ini: error: no [dancer] section"))
        for label, ini, raw, line in cases:
            with self.subTest(label):
                d = self.dancer(ini, base_png(), name="robo", raw=raw)
                self.assertPackError(dp.compile_dir(str(d)), line)

    def test_required_and_typos(self):
        for key in ("name", "author", "license", "cell"):
            with self.subTest(key):
                ini = ini_text(dancer_keys({key: None}))
                self.assertPackError(self.compile(ini), "dancer.ini:1: error: [dancer] needs %s =" % key)
        # C.3: a ratio of exactly 0.75 is an error too ("rist" against "rest")
        for typo, known in (("nmae", "name"), ("scael", "scale"), ("autor", "author"), ("licence", "license"),
                            ("shadw", "shadow"), ("palete", "palette"), ("rist", "rest")):
            with self.subTest(typo):
                ini = ini_text(dancer_keys(add=["%s = 1" % typo]))
                self.assertPackError(self.compile(ini), self.at(
                    ini, "[dancer]", typo, 'unknown key "%s" in [dancer]: did you mean %s?' % (typo, known)))
        # C.3: a ratio from 0.6 up to 0.75 is only a hint: the key is ignored with a warning
        for word, known in (("speed", "sheet"), ("genres", "rest")):
            with self.subTest(word):
                ini = ini_text(dancer_keys(add=["%s = 2" % word]))
                r = self.compile(ini, name="hint-" + word)
                self.assertTrue(r.ok, r.messages)
                self.assertWarning(r, ini, "[dancer]", word, 'unknown key "%s" in [dancer] ignored (did you mean %s?)'
                                   % (word, known))

    def test_values(self):
        ctl = "Ro\x07bo"
        cases = [
            ({"format": "2"}, "format", "format 2 is newer than this dancepack.py knows (1): update dancepack.py"),
            ({"format": "0"}, "format", "format must be 1"),
            ({"name": ""}, "name", "name is empty"),
            ({"author": ""}, "author", "author is empty"),
            ({"name": "n" * 25}, "name", "name is 25 bytes: at most 24"),
            ({"name": "é" * 13}, "name", "name is 26 bytes: at most 24"),
            ({"author": "a" * 41}, "author", "author is 41 bytes: at most 40"),
            ({"license": "L" * 41}, "license", "license is 41 bytes: at most 40"),
            ({"url": "u" * 81}, "url", "url is 81 bytes: at most 80"),
            ({"version": "v" * 17}, "version", "version is 17 bytes: at most 16"),
            ({"name": ctl}, "name", "name has a control character"),
            ({"author": "A\u009bna"}, "author", "author has a control character"),     # C1 (CSI)
            ({"url": "https://x.org/\x7f"}, "url", "url has a control character"),
            ({"sheet": "../sheet.png"}, "sheet", "sheet must be a path inside the dancer's folder"),
            ({"sheet": "missing.png"}, "sheet", "no sheet at missing.png"),
            ({"cell": "8by8"}, "cell", "cell must be WxH, like 64x76"),
            ({"scale": "5"}, "scale", "scale must be 1, 2, 3 or 4"),
            ({"scale": "0"}, "scale", "scale must be 1, 2, 3 or 4"),
            ({"anchor": "4"}, "anchor", "anchor must be x,y"),
            ({"anchor": "8,8"}, "anchor", "anchor 8,8 is outside the 8x8 cell"),
            ({"anchor": "4,0"}, "anchor", "anchor 4,0 is outside the 8x8 cell"),
            ({"anchor": "4,9"}, "anchor", "anchor 4,9 is outside the 8x8 cell"),
            ({"shadow": "161"}, "shadow", "shadow must be 0-160 (screen px)"),
            ({"faces": "up"}, "faces", "faces must be none, right or left"),
            ({"key": "magenta"}, "key", "key must be #rrggbb"),
            ({"key": "#ff00f"}, "key", "key must be #rrggbb"),
            ({"palette": "#ff0000 red"}, "palette", 'palette: "red" isn\'t #rrggbb'),
            ({"palette": "#ffffff #ff0000", "idle_palette": "#808080 grey"}, "idle_palette",
             'idle_palette: "grey" isn\'t #rrggbb'),
            ({"palette": "#ffffff #ff0000", "idle_palette": "#808080"}, "idle_palette",
             "idle_palette has 1 colour; the palette has 2"),
            ({"palette": "#ffffff #ff0000 #00ff00", "idle_palette": "#808080 #101010"}, "idle_palette",
             "idle_palette has 2 colours; the palette has 3"),
            ({"sheet": "art\\sheet.png"}, "sheet", "sheet must be a path inside the dancer's folder: use / between "
                                                   "folders"),
            ({"sheet": "sheet.png."}, "sheet", 'sheet: "sheet.png." ends in a dot or a space, which Windows drops: '
                                               "rename it"),
            ({"sheet": "SHEET.PNG"}, "sheet",
             "no sheet at SHEET.PNG (the file is sheet.png: names are case-sensitive)"),
            ({"scale": "1234567890"}, "scale", "scale must be 1, 2, 3 or 4"),     # more than 9 digits
            ({"palette": " ".join("#%02x%02x%02x" % (8 * i, 255, 255 - 8 * i) for i in range(16))}, "palette",
             "palette has 16 colours: at most 15"),
        ]
        for changes, key, message in cases:
            with self.subTest(changes=changes):
                keys = dancer_keys({k: v for k, v in changes.items() if k in ("name", "author", "license",
                                                                               "sheet", "cell")},
                                   add=["%s = %s" % (k, v) for k, v in changes.items()
                                        if k not in ("name", "author", "license", "sheet", "cell")])
                ini = ini_text(keys)
                self.assertPackError(self.compile(ini), self.at(ini, "[dancer]", key, message))
        ini = ini_text(dancer_keys(add=["palette = #ff0000 #fe0000"]))
        self.assertPackError(self.compile(ini), self.at(
            ini, "[dancer]", "palette", "palette lists #ff0000 and #fe0000, both RGB565 0xF800"))

    def test_geometry(self):
        s = Sheet(1, 1, 41, 8)
        s.fill("A1", 18, 0, 4, 8, W)
        ini = ini_text(dancer_keys({"cell": "41x8"}, add=["scale = 4"]))
        self.assertPackError(self.compile(ini, s.png()), self.at(
            ini, "[dancer]", "cell", "cell 41x8 at scale 4 is 164x32 on screen: at most 160x160"))
        s = Sheet(1, 1, 40, 40)
        s.fill("A1", 18, 20, 4, 19, W)
        ini = ini_text(dancer_keys({"cell": "40x40"}, add=["scale = 4", "anchor = 20,39"]))
        self.assertPackError(self.compile(ini, s.png()), self.at(
            ini, "[dancer]", "anchor", "anchor y 39 at scale 4 puts the cell's top 156 px above the ground: at most 153"))
        ini = ini_text(dancer_keys({"cell": "40x40"}, add=["scale = 4"]))     # the default anchor: 20,40
        self.assertPackError(self.compile(ini, s.png()), "dancer.ini:1: error: anchor y 40 at scale 4 puts the "
                             "cell's top 160 px above the ground: at most 153")
        s = Sheet(1, 1, 8, 16)
        s.fill("A1", 3, 4, 2, 6, W)
        ini = ini_text(dancer_keys({"cell": "8x16"}, add=["scale = 2", "anchor = 4,10"]))
        self.assertPackError(self.compile(ini, s.png()), self.at(
            ini, "[dancer]", "anchor", "the cell reaches 12 screen px below the ground: at most 10"))
        ini = ini_text(dancer_keys({"cell": "8x16"}, add=["scale = 2", "anchor = 4,11"]))
        self.assertTrue(self.compile(ini, s.png()).ok)                        # 10 px exactly

    def test_pack_level_limits(self):
        ini = ini_text(moves=[("[move m%d]" % i, BASE_MOVE[1]) for i in range(65)])
        self.assertPackError(self.compile(ini), "error: 65 moves: at most 64")
        self.assertTrue(self.compile(ini_text(moves=[("[move m%d]" % i, BASE_MOVE[1]) for i in range(64)]),
                                     name="sixty4").ok)
        moves = [("[move m%d]" % i, list(BASE_MOVE[1]) + ["from = f%d-left" % i, "to = t%d-left" % i])
                 for i in range(64)]
        self.assertPackError(self.compile(ini_text(moves=moves), name="poses"), "error: 256 pose keys: at most 255")

    def test_pack_too_big(self):
        cols = [R, G, B, Y]
        s = Sheet(11, 10, 40, 40)
        s.fill("A1", 18, 30, 4, 10, W)
        rnd = lcg(7)
        names = []
        for k in range(1, 109):
            c, r = k % 11, k // 11
            for y in range(40):
                row = s.px[r * 40 + y]
                for x in range(40):
                    # the LCG's high bits: its low ones repeat every 2^(k+1) draws, which
                    # would make most cells identical (and A.14 stores those once)
                    row[c * 40 + x] = cols[next(rnd) >> 13 & 3]
            names.append(cell_name(c, r))
        index = {ref_rgb565(*c): i + 1 for i, c in enumerate([W] + cols)}
        frames = []
        for n in ["A1"] + names:
            ref_store(frames, s.grid(n, index), 20)
        self.assertEqual(len(frames), 109)      # no two cells merge, mirrored or not
        fdat = sum(len(ref_encode_frame(g, ref_box(g))) for g in frames)
        self.assertGreater(fdat, 131072)
        moves = [("[move m%d]" % i, ["frames = " + " ".join(names[32 * i:32 * i + 32]), "beats = 8", "bpm = any"])
                 for i in range(3)]
        moves.append(("[move m3]", ["frames = " + " ".join(names[96:]), "beats = 4", "bpm = any"]))
        r = self.compile(ini_text(dancer_keys({"cell": "40x40"}, add=["rest = A1"]), moves), s.png())
        self.assertFalse(r.ok)
        hits = [re.match(r"^error: the pack is ([0-9,]+) bytes: at most 131,072$", m) for m in self.messages(r)]
        hits = [h for h in hits if h]
        self.assertEqual(len(hits), 1, r.messages)
        n = int(hits[0].group(1).replace(",", ""))
        self.assertGreater(n, fdat)
        self.assertEqual(hits[0].group(1), "{:,}".format(n))


class ManifestRulesTest(DpCase):
    """What C.3 accepts: comments, cases, BOM and line ends, lists, x- keys."""

    def test_long_numbers(self):
        # C.3: any leading zeros, then at most 9 digits; never Python's int-string
        # limit, so every version reads the same (5,000 zeros is still 2).
        zeros = "0" * 5000
        ini = ini_text(dancer_keys(add=["scale = %s2" % zeros]),
                       [("[move base]", ["frames = A%s1" % zeros, "beats = %s1" % zeros, "bpm = %s90-130" % zeros,
                                         "timing = %s1/%s1" % (zeros, zeros)]),
                        ("[move idle]", ["frames = A1 B1", "bpm = none", "timing = %s1s 1.5s" % zeros])])
        r = self.compile(ini)
        self.assertTrue(r.ok, r.messages[:3])
        self.assertEqual(list(r.skipped), [])
        p = Mpdn(r.data)
        self.assertEqual(p.geom.scale, 2)
        self.assertEqual((p.moves[0].beats, p.moves[0].pace_lo, p.moves[0].pace_hi), (1, 90, 130))
        self.assertEqual([s.dur for s in p.moves[1].steps], [100, 150])
        nines = "9" * 5000
        for timing, bpm in (("%sms 1s" % nines, "none"), ("1/%s 1" % nines, "any"), ("0.1234567891 1", "any")):
            with self.subTest(timing[-12:]):
                body = ["frames = A1 B1", "bpm = %s" % bpm, "timing = " + timing] + (["beats = 1"] if bpm == "any"
                                                                                      else [])
                ini = ini_text(moves=[BASE_MOVE, ("[move m]", body)])
                r = self.compile(ini, name="long")
                self.assertSkip(r, ini, "m", 'timing "%s" has a number of more than 9 digits' % timing.split()[0],
                                "timing")
        for text in ("%sms" % nines, "0.0000000001s"):
            with self.assertRaises(dp.DanceError):
                dp.parse_idle_time(text)
        with self.assertRaises(dp.DanceError):
            dp.parse_beat_time(nines)
        self.assertEqual(dp.parse_beat_time(zeros + "1/" + zeros + "4"), (3, False))

    def test_long_cell_names(self):
        # A sheet has at most 1024 columns (AMJ) and rows: a longer name is no
        # cell, rejected at once rather than converted (it used to take minutes).
        import time
        ini = ini_text(moves=[BASE_MOVE, ("[move m]", ["frames = %s1" % ("Z" * 300000), "beats = 1", "bpm = any"])])
        t0 = time.time()
        r = self.compile(ini)
        self.assertLess(time.time() - t0, 20)
        self.assertEqual(list(r.skipped), ["m"])
        ini = ini_text(dancer_keys(add=["rest = AAAA1"]))
        self.assertPackError(self.compile(ini, name="rest4"), "dancer.ini:%d: error: rest AAAA1 isn't a cell of the "
                             "4x3 sheet" % ini_line(ini, "[dancer]", "rest"))

    def test_section_headers(self):
        # C.3: a header runs to the line's last ], on every Python version (3.8
        # and 3.9's configparser stopped at the first).
        ini = ini_text(moves=[BASE_MOVE, ("[move hop]]", ["frames = A1 B1", "beats = 1", "bpm = any"])])
        r = self.compile(ini)
        self.assertSkip(r, ini, "hop]", "its name must be 1-23 of a-z, 0-9 and -, starting with a letter or digit",
                        None, "[move hop]]")
        self.assertEqual(Mpdn(r.data).declared_moves, 1)

    def test_section_near_misses(self):
        # C.3: a header that nearly says [move ...] skips that move; nearly [dancer] is an error.
        for header in ("[moves wave]", "[mvoe wave]", "[move:wave]", "[move-wave]", "[Moves wave]"):
            with self.subTest(header):
                ini = ini_text(moves=[BASE_MOVE, (header, ["frames = A1 B1", "beats = 2", "bpm = any"])])
                r = self.compile(ini, name="near")
                self.assertLine(r, 'dancer.ini:%d: move "wave": unknown section %s: did you mean [move wave]? '
                                   '(skipped)' % (ini_line(ini, header), header))
                self.assertEqual(list(r.skipped), ["wave"])
                self.assertEqual([m.name_text for m in Mpdn(r.data).moves], ["base"])
                self.assertLine(r, "near: 1 move (1 of 2 skipped), 1 frame, %s bytes" % "{:,}".format(len(r.data)))
                self.assertPackError(self.compile(ini, strict=True, name="near-s"), "error: --strict: 1 move skipped")
        ini = ini_text(moves=[BASE_MOVE, ("[dancr]", ["x = 1"])])
        self.assertPackError(self.compile(ini, name="dancr"), "dancer.ini:%d: error: unknown section [dancr]: did you "
                             "mean [dancer]?" % ini_line(ini, "[dancr]"))
        for header in ("[moves]", "[move2]", "[move:]"):
            with self.subTest(header):
                ini = ini_text(moves=[BASE_MOVE, (header, ["frames = A1"])])
                self.assertPackError(self.compile(ini, name="no-name"), "dancer.ini:%d: error: unknown section %s: "
                                     "did you mean [move <name>]?" % (ini_line(ini, header), header))
        # the separator rule comes first: a one-letter name after it (whose first word
        # also scores 0.8 against "move") skips that move, as a longer name does
        for header, name in (("[move-1]", "1"), ("[move:a]", "a"), ("[move_x]", "x"), ("[move- wave]", "wave"),
                             ("[moves x]", "x")):
            with self.subTest(header):
                ini = ini_text(moves=[BASE_MOVE, (header, ["frames = A1 B1", "beats = 2", "bpm = any"])])
                r = self.compile(ini, name="short")
                self.assertTrue(r.ok, r.messages)
                self.assertLine(r, 'dancer.ini:%d: move "%s": unknown section %s: did you mean [move %s]? '
                                   '(skipped)' % (ini_line(ini, header), name, header, name))
                self.assertEqual(list(r.skipped), [name])

    def test_near_miss_cells_are_used(self):
        # C.4: a near-miss header's cells are used cells, like any skipped move's, so the
        # palette doesn't depend on how a move was skipped: A1 white, B1 red.
        s = Sheet(2, 1, 8, 8)
        s.fill("A1", 2, 6, 4, 2, W)
        s.fill("B1", 2, 6, 4, 2, R)
        for header, body in (("[moves wave]", ["frames = B1", "beats = 2", "bpm = any"]),     # a near miss
                             ("[move wave]", ["frames = B1", "beats = 2", "bmp = any"]),      # a typo key
                             ("[move wave]", ["frames = B1", "beats = 3", "bpm = any"])):     # a bad value
            with self.subTest(header=header, body=body):
                ini = ini_text(moves=[BASE_MOVE, (header, body)])
                r = self.compile(ini, s.png(), name="cells")
                self.assertTrue(r.ok, r.messages)
                self.assertEqual(list(r.skipped), ["wave"])
                self.assertEqual(Mpdn(r.data).palette[:4], [(0, 0, 0), W, R, (0, 0, 0)])

    def test_hash_after_a_value(self):
        # C.3: # starts a comment only at the start of a line; after a value it is
        # part of it, with a warning (a comment after a value starts with ;).
        ini = ini_text(dancer_keys({"name": "Robo  # my robot", "license": "MIT # free"},
                                   add=["url = https://x.org/#top"]),
                       [("[move base]", ["frames = A1", "beats = 1", "bpm = any", "genre = funk  # for funk"]),
                        ("[move hashy]", ["frames = A1", "beats = 1", "bpm = any", "genre = Robo #2"])])
        r = self.compile(ini)
        self.assertTrue(r.ok, r.messages)
        tail = "is part of the value; a comment after a value starts with ;"
        self.assertWarning(r, ini, "[dancer]", "name", 'name: "# my robot" %s' % tail)
        self.assertWarning(r, ini, "[dancer]", "license", 'license: "# free" %s' % tail)
        self.assertWarning(r, ini, "[move base]", "genre", 'genre: "# for funk" %s' % tail)
        self.assertNoLine(r, r"url: |genre: \"#2")       # # with no space before or after it
        p = Mpdn(r.data)
        self.assertEqual((p.name, p.licence), ("Robo  # my robot", "MIT # free"))
        ini = ini_text(dancer_keys(add=["scale = 2   # double size"]))
        r = self.compile(ini, name="scale")
        self.assertWarning(r, ini, "[dancer]", "scale", 'scale: "# double size" %s' % tail)
        self.assertPackError(r, "dancer.ini:%d: error: scale must be 1, 2, 3 or 4" % ini_line(ini, "[dancer]", "scale"))

    def test_wrapped_palette(self):
        # C.3: an indented line starting with # is a comment, even inside a value,
        # so a palette wrapped onto it loses those colours: an error at that line.
        for key in ("palette", "idle_palette"):
            with self.subTest(key):
                extra = ["palette = #ffffff #ff0000"] if key == "idle_palette" else []
                ini = ini_text(dancer_keys(add=extra + ["%s = #ffffff" % key, "          #ff0000 ; red"]))
                line = ini.split("\n").index("          #ff0000 ; red") + 1
                self.assertPackError(self.compile(ini, name="wrap-" + key.replace("_", "-")), (
                    "dancer.ini:%d: error: an indented line starting with # is a comment, not more colours: keep %s "
                    "on one line" % (line, key)))
        # a comment that isn't colours, or isn't indented, loses nothing
        for note in ("# a note", "    # a wrapped note"):
            ini = ini_text(dancer_keys(add=["palette = #ffffff #ff0000", note]))
            self.assertTrue(self.compile(ini, name="note").ok, note)

    def test_inline_comments_and_hash(self):
        ini = ini_text(dancer_keys({"license": "MIT ; the licence", "name": "Robo   ; a name"},
                                   add=["# a whole-line comment", "; another", "palette = #ffffff #ff0000 ; two",
                                        "url = https://example.org/#top"]))
        r = self.compile(ini)
        self.assertTrue(r.ok, r.messages)
        p = Mpdn(r.data)
        self.assertEqual((p.name, p.licence, p.url), ("Robo", "MIT", "https://example.org/#top"))
        self.assertEqual(p.palette[1:3], [W, R])

    def test_palette_order_and_rgb565_match(self):
        s = Sheet(1, 1, 8, 8)
        s.fill("A1", 3, 0, 2, 4, (0xFE, 0x01, 0x02))    # matches #ff0000 by RGB565
        s.fill("A1", 3, 4, 2, 4, G)
        ini = ini_text(dancer_keys(add=["palette = #00ff00 #FF0000"]))
        r = self.compile(ini, s.png())
        self.assertTrue(r.ok, r.messages)
        p = Mpdn(r.data)
        self.assertEqual(p.palette[:3], [(0, 0, 0), G, R])           # the listed order, the listed RGB888
        self.assertEqual(p.grids[0][0][3], 2)
        self.assertEqual(p.grids[0][7][3], 1)
        ini = ini_text(dancer_keys(add=["palette = #00ff00 #ff0000", "idle_palette = #102030 #405060"]))
        p = Mpdn(self.compile(ini, s.png(), name="idle").data)
        self.assertEqual(p.idle_palette[:4], [(0, 0, 0), (0x10, 0x20, 0x30), (0x40, 0x50, 0x60), (0, 0, 0)])

    def test_bom_and_line_ends(self):
        ini = robo_ini()
        want = self.compile(ini, parent=self.tmp / "lf")
        for label, raw in (("bom", b"\xef\xbb\xbf" + ini.encode()), ("crlf", ini.replace("\n", "\r\n").encode()),
                           ("cr", ini.replace("\n", "\r").encode()),
                           ("bom-crlf", b"\xef\xbb\xbf" + ini.replace("\n", "\r\n").encode())):
            d = self.dancer(None, base_png(), name="robo", raw=raw, parent=self.tmp / label)
            r = dp.compile_dir(str(d))
            self.assertTrue(r.ok, (label, r.messages))
            self.assertEqual(bytes(r.data), bytes(want.data), label)
            self.assertEqual(list(r.messages), list(want.messages), label)      # the same line numbers

    def test_words_cases_and_syntax(self):
        moves = [("[Move base]", ["Frames = a1 B1", "BEATS = 2", "bpm = 90 - 130", "mirror = On", "snap = TRUE",
                                  "heavy = 0", "ease = InOut Hold", "offset = 0,0 +1,-2", "place = START",
                                  "x-anything = whatever"]),
                 ("[move idle]", ["frames = A1 B1", "bpm = NONE", "timing = 1S 500Ms", "when = Quiet"]),
                 ("[move trip]", ["frames = A1", "  B1 C1", "  D1", "beats = 4", "bpm = Any", "travel = +1",
                                  "go = 1, 3-4", "start_at = Side", "travel_ease = LINEAR"]),
                 ("[move tagged]", ["frames = A1", "beats = 1", "bpm = any", "genre = hip hop , R&B,Disco "])]
        ini = ini_text(list(DANCER_KEYS) + ["faces = Right", "x-note = ignored"], moves)
        ini = ini.replace("[dancer]", "[DANCER]").replace("cell = 8x8", "cell: 8x8")
        r = self.compile(ini)
        self.assertTrue(r.ok, r.messages)
        self.assertEqual(list(r.skipped), [])
        self.assertNoLine(r, r"x-(note|anything)")
        p = Mpdn(r.data)
        self.assertEqual(p.geom.faces, 1)
        base = p.move("base")
        self.assertEqual((base.pace_lo, base.pace_hi, base.flags), (90, 130, 1 | 4 | 32))
        self.assertEqual([(s.dx, s.dy, s.ease) for s in base.steps], [(0, 0, 4), (1, -2, 0)])
        idle = p.move("idle")
        self.assertEqual((idle.kind, idle.when, [s.dur for s in idle.steps]), (1, 1, [100, 50]))
        trip = p.move("trip")
        self.assertEqual((trip.travel, trip.start_at, trip.travel_ease, len(trip.steps)), (1, 2, 1, 4))
        self.assertEqual([s.go for s in trip.steps], [True, False, True, True])
        self.assertEqual(p.move("tagged").genres, ["hip hop", "R&B", "Disco"])

    def test_unknown_keys_and_sections(self):
        ini = ini_text(dancer_keys(add=["colour = red", "x-made-with = paint"]),
                       [("[move base]", list(BASE_MOVE[1]) + ["notes = hi", "x-todo = more"]),
                        ("[extras]", ["thing = 1"])])
        r = self.compile(ini)
        self.assertTrue(r.ok, r.messages)
        self.assertWarning(r, ini, "[dancer]", "colour", 'unknown key "colour" in [dancer] ignored')
        self.assertWarning(r, ini, "[move base]", "notes", 'unknown key "notes" in [move base] ignored')
        self.assertLine(r, "dancer.ini:%d: warning: unknown section [extras] ignored" % ini_line(ini, "[extras]"))
        self.assertNoLine(r, r"x-(made-with|todo)")
        self.assertEqual(list(r.skipped), [])
        # a weak match (ratio 0.6-0.75) keeps the move and hints: a display name isn't "frames"
        ini = ini_text(moves=[("[move base]", list(BASE_MOVE[1]) + ["name = Base", "time = 1"])])
        r = self.compile(ini, name="hints")
        self.assertEqual(list(r.skipped), [])
        self.assertWarning(r, ini, "[move base]", "name", 'unknown key "name" in [move base] ignored (did you mean '
                                                          'frames?)')
        self.assertWarning(r, ini, "[move base]", "time", 'unknown key "time" in [move base] ignored (did you mean '
                                                          'timing?)')

    def test_weights(self):
        for text, w10 in (("0.1", 1), ("2.5", 25), ("10", 100), ("3", 30), ("1.0", 10)):
            ini = ini_text(moves=[("[move base]", list(BASE_MOVE[1]) + ["weight = " + text])])
            r = self.compile(ini, name="w" + text.replace(".", "-"))
            self.assertEqual(Mpdn(r.data).moves[0].weight10, w10, text)


# ===========================================================================
# Move skips: every C.6 message, each with its line.

SKIPS = (
    ("[move Bad_Name]", ("frames = A1", "beats = 1", "bpm = any"), None,
     "its name must be 1-23 of a-z, 0-9 and -, starting with a letter or digit"),
    ("[move -x]", ("frames = A1", "beats = 1", "bpm = any"), None,
     "its name must be 1-23 of a-z, 0-9 and -, starting with a letter or digit"),
    ("[move %s]" % ("a" * 24), ("frames = A1", "beats = 1", "bpm = any"), None,
     "its name must be 1-23 of a-z, 0-9 and -, starting with a letter or digit"),
    ("[move m]", ("frames = A1", "beats = 1", "bmp = any"), "bmp", 'unknown key "bmp": did you mean bpm?'),
    ("[move m]", ("frames = A1", "beats = 1", "bpm = any", "timming = 1"), "timming",
     'unknown key "timming": did you mean timing?'),
    ("[move m]", ("frames = A1", "beats = 1", "bpm = any", "mirrror = yes"), "mirrror",
     'unknown key "mirrror": did you mean mirror?'),
    # C.3: a ratio of exactly 0.75 skips too ("easy" against "ease")
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "easy = inout"), "easy",
     'unknown key "easy": did you mean ease?'),
    ("[move m]", ("frames = A1", "beats = 1"), None, "needs bpm = (a range like 90-130, any, or none)"),
    ("[move m]", ("frames = A1", "beats = 1", "bpm = fast"), "bpm", "bpm must be lo-hi, any or none"),
    ("[move m]", ("frames = A1 A1", "bpm = none", "timing = 1s 1s", "bounce = 2"), "bounce",
     "bounce applies to beat moves only"),
    ("[move m]", ("frames = A1 A1", "bpm = none", "timing = 1s 1s", "genre = disco"), "genre",
     "genre applies to beat moves only"),
    ("[move m]", ("frames = A1", "beats = 1", "bpm = any", "when = quiet"), "when",
     "when applies to no-BPM moves only"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "travel_ease = out"), "travel_ease",
     "travel_ease needs travel ="),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "travel = 0", "start_at = side"), "start_at",
     "start_at needs travel ="),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "go = 1"), "go", "go needs travel ="),
    ("[move m]", ("beats = 1", "bpm = any"), None, "give row = or frames ="),
    ("[move m]", ("row = 1", "frames = A1", "beats = 1", "bpm = any"), "row", "give row = or frames =, not both"),
    ("[move m]", ("row = 9", "beats = 1", "bpm = any"), "row", "row 9 isn't a row of the 4x3 sheet"),
    ("[move m]", ("row = 3", "beats = 1", "bpm = any"), "row", "row 3 is empty"),
    ("[move m]", ("frames = A1 E1", "beats = 1", "bpm = any"), "frames",
     'frames: "E1" isn\'t a cell of the 4x3 sheet'),
    ("[move m]", ("frames = A1 A4", "beats = 1", "bpm = any"), "frames",
     'frames: "A4" isn\'t a cell of the 4x3 sheet'),
    ("[move m]", ("frames = " + " ".join(["A1"] * 33), "beats = 8", "bpm = any"), "frames",
     "33 frames: at most 32 steps"),
    ("[move m]", ("frames = A1", "bpm = any"), None, "needs beats = 1, 2, 4 or 8"),
    ("[move m]", ("frames = A1", "beats = 3", "bpm = any"), "beats", "beats must be 1, 2, 4 or 8"),
    ("[move m]", ("frames = A1", "beats = 16", "bpm = any"), "beats", "beats must be 1, 2, 4 or 8"),
    ("[move m]", ("frames = A1", "bpm = none"), None, "a no-BPM move needs timing = (s or ms)"),
    ("[move m]", ("frames = A1 B1", "beats = 1", "bpm = any", "timing = 1/4 1/4 1/2"), "timing",
     "timing has 3 values for 2 steps: give one per step"),
    ("[move m]", ("frames = A1 B1", "beats = 1", "bpm = any", "timing = 1"), "timing",
     "timing has 1 value for 2 steps: give one per step"),
    ("[move m]", ("frames = A1 B1", "beats = 1", "bpm = any", "timing = 1/2 half"), "timing",
     'timing "half" isn\'t a number of beats, like 1/4, 0.5 or 2'),
    ("[move m]", ("frames = A1 B1", "beats = 1", "bpm = any", "timing = 1/2 0.5s"), "timing",
     'timing "0.5s" isn\'t a number of beats, like 1/4, 0.5 or 2'),
    ("[move m]", ("frames = A1 B1", "beats = 1", "bpm = any", "timing = 0.3 0.7"), "timing",
     "timing 0.3 is off the 1/12-beat grid by more than 1/48 beat"),
    ("[move m]", ("frames = A1 B1", "bpm = none", "timing = 2 2"), "timing",
     'timing "2" needs s or ms, like 2s or 400ms'),
    ("[move m]", ("frames = A1 B1", "bpm = none", "timing = 0.405s 1s"), "timing",
     "timing 0.405s isn't a whole number of 10 ms"),
    ("[move m]", ("frames = A1 B1 C1 D1 A1", "beats = 1", "bpm = any"), "frames",
     "5 frames can't share 1 beat evenly on the 1/12 grid: give timing ="),
    ("[move m]", ("row = 1", "beats = 8", "bpm = any", "timing = 1 1 1"), "timing",
     "timing has 3 values for 4 steps: give one per step"),
    ("[move m]", ("frames = A1 B1 C1", "beats = 1", "bpm = any", "timing = 1/2 1/6 1/3"), "timing",
     "step 2 lasts 2 ticks: at least 3 (1/4 beat)"),
    ("[move m]", ("frames = A1 B1 C1 D1 A1 B1", "beats = 1", "bpm = any"), "frames",
     "step 1 lasts 2 ticks: at least 3 (1/4 beat)"),
    ("[move m]", ("frames = A1 B1 C1 D1", "beats = 4", "bpm = any", "timing = 1 1 1 3/4"), "timing",
     "steps last 45 ticks (3 3/4 beats); a 4-beat move needs 48"),
    ("[move m]", ("frames = A1 B1", "beats = 1", "bpm = any", "timing = 1/2 1"), "timing",
     "steps last 18 ticks (1 1/2 beats); a 1-beat move needs 12"),
    ("[move m]", ("frames = A1 B1", "bpm = none", "timing = 100ms 1s"), "timing",
     "step 1 lasts 100 ms: 200-5,000 ms"),
    ("[move m]", ("frames = A1 B1", "bpm = none", "timing = 200ms 200ms"), "timing",
     "the steps last 400 ms in all: 500-12,000 ms"),
    # milliseconds aren't px or bytes, so they print plain even from 1,000 (C.5)
    ("[move m]", ("frames = A1 B1", "bpm = none", "timing = 5.01s 1s"), "timing",
     "step 1 lasts 5010 ms: 200-5,000 ms"),
    ("[move m]", ("frames = A1 B1 C1", "bpm = none", "timing = 5s 5s 2.01s"), "timing",
     "the steps last 12010 ms in all: 500-12,000 ms"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "offset = 0,0 1,0 2,0"), "offset",
     "offset has 3 values for 2 steps: give one per step"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "offset = 0,0 a,b"), "offset",
     'offset "a,b" isn\'t x,y'),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "offset = 0,0 1.5,2"), "offset",
     'offset "1.5,2" isn\'t x,y'),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "offset = 0,0 49,0"), "offset",
     "offset 49,0 moves 49 screen px: at most 48"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "offset = 0,0 0,-49"), "offset",
     "offset 0,-49 moves 49 screen px: at most 48"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "ease = bounce"), "ease",
     'ease "bounce" isn\'t hold, linear, in, out or inout'),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "travel = 1", "travel_ease = fast"), "travel_ease",
     'ease "fast" isn\'t hold, linear, in, out or inout'),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "ease = in out in"), "ease",
     "ease has 3 values for 2 steps: give one, or one per step"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "mirror = maybe"), "mirror",
     "mirror must be yes or no"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "snap = sometimes"), "snap",
     "snap must be yes or no"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "place = middle"), "place",
     "place must be start or end"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "weight = 0.25"), "weight",
     "weight must be 0.1-10 in steps of 0.1"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "weight = 0"), "weight",
     "weight must be 0.1-10 in steps of 0.1"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "weight = 10.1"), "weight",
     "weight must be 0.1-10 in steps of 0.1"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "from = stand_up"), "from",
     'from "stand_up": pose keys are 1-23 of a-z, 0-9 and -'),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "to = " + "p" * 24), "to",
     'to "%s": pose keys are 1-23 of a-z, 0-9 and -' % ("p" * 24)),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "from = %s-left" % ("a" * 18)), "from",
     'from "%s-left": its mirror twin "%s-right" is 24 bytes: at most 23' % ("a" * 18, "a" * 18)),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "to = left-left-left-left-a"), "to",
     'to "left-left-left-left-a": its mirror twin "right-right-right-right-a" is 25 bytes: at most 23'),
    ("[move m]", ("frames = A1, B1", "beats = 2", "bpm = any"), "frames",
     'frames: "A1," isn\'t a cell of the 4x3 sheet: separate cells with spaces'),
    ("[move a] [x]", ("frames = A1", "beats = 1", "bpm = any"), None,
     "its name must be 1-23 of a-z, 0-9 and -, starting with a letter or digit"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "repeat = 9"), "repeat", "repeat must be 0-8"),
    ("[move m]", ("frames = A1 B1 C1 D1", "beats = 4", "bpm = any", "repeat = 5"), "repeat",
     "repeat 5 x 4 beats is 20 beats: at most 16"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "sway = 17"), "sway", "sway must be 0-16 (native px)"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "bounce = 17"), "bounce",
     "bounce must be 0-16 (native px)"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "bounce = -1"), "bounce",
     "bounce must be 0-16 (native px)"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "travel = 3"), "travel",
     "travel must be -2 to 2 (spots)"),
    ("[move m]", ("frames = A1", "beats = 1", "bpm = any", "travel = 1"), "travel",
     "a travel move needs at least 2 beats"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "travel = 1", "start_at = center"), "start_at",
     "start_at must be any, centre, side, left or right: did you mean centre?"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "travel = 1", "start_at = middle"), "start_at",
     "start_at must be any, centre, side, left or right"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "travel = 1", "go = 3"), "go",
     'go: "3" isn\'t a step (1-2) or a range of them'),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "genre = disco, , funk"), "genre", "empty genre tag"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "genre = a, b, c, d, e, f, g, h, i"), "genre",
     "more than 8 genre tags (9)"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "genre = " + "x" * 64), "genre",
     'genre tag "%s" is 64 bytes: at most 63' % ("x" * 64)),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "genre = a\x01b"), "genre",
     'genre tag "a\\x01b" has a control character'),            # C.5: printed as \xNN
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "genre = rock, 日本"), "genre",
     'genre tag "日本" folds to nothing: use letters or digits Full folding can spell'),
    ("[move m]", ("frames = A1 B1", "bpm = none", "timing = 1s 1s", "when = never"), "when",
     "when must be quiet, listening or any"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "song_bpm = 0-300"), "song_bpm",
     "song_bpm must be lo-hi, 1-255"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = 30-100"), "bpm", "bpm 30-100: each bound must be 36-200"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = 36-201"), "bpm", "bpm 36-201: each bound must be 36-200"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = 130-90"), "bpm", "bpm 130-90: lo is above hi"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = 185-200"), "bpm",
     "range 185-200 can't be reached: the fastest pace is 180"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = 178-200", "travel = 1"), "bpm",
     "range 178-200 can't be reached: the fastest pace is 176"),
    ("[move m]", ("frames = A1 B1", "beats = 8", "bpm = 36-60"), "bpm",
     "range 36-60 can't be reached: the slowest pace is 72"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "travel = 2", "start_at = centre"), "travel",
     "can't travel from any spot: too wide there, or start_at rules every spot out"),
    ("[move m]", ("frames = A1 B1", "beats = 2", "bpm = any", "mirror = yes", "travel = -2", "start_at = left"),
     "travel", "can't travel from any spot: too wide there, or start_at rules every spot out"),
)


class MoveSkipTest(DpCase):
    def test_every_skip(self):
        for header, body, key, message in SKIPS:
            name = header[len("[move "):-1]
            with self.subTest(message, header=header, body=body):
                ini = ini_text(moves=[BASE_MOVE, (header, body)])
                r = self.compile(ini)
                self.assertSkip(r, ini, name, message, key, header)
                self.assertEqual(Mpdn(r.data).declared_moves, 1)

    def test_freeze_once(self):
        ini = ini_text(moves=[("[move base]", list(BASE_MOVE[1]) + ["freeze = yes"]),
                              ("[move m]", ["frames = A1 B1", "beats = 2", "bpm = any", "freeze = yes"])])
        self.assertSkip(self.compile(ini), ini, "m", 'freeze is already set on move "base"', "freeze")

    def test_typo_in_a_move_skips_only_it(self):
        ini = ini_text(moves=[BASE_MOVE, ("[move m]", ["fames = A1", "beats = 1", "bpm = any"]),
                              ("[move n]", ["frames = B1", "beats = 1", "bpm = any"])])
        r = self.compile(ini)
        self.assertLine(r, 'dancer.ini:%d: move "m": unknown key "fames": did you mean frames? (skipped)'
                        % ini_line(ini, "[move m]", "fames"))
        self.assertEqual(list(r.skipped), ["m"])
        self.assertEqual([m.name_text for m in Mpdn(r.data).moves], ["base", "n"])

    def test_offset_limit_scales(self):
        # The limit is 48 screen px (C.6), so 24 native px at scale 2.
        keys = dancer_keys(add=["scale = 2"])
        ok = ini_text(keys, [BASE_MOVE, ("[move m]", ["frames = A1 B1", "beats = 2", "bpm = any",
                                                      "offset = 0,0 -24,24"])])
        self.assertEqual(list(self.compile(ok).skipped), [])
        bad = ini_text(keys, [BASE_MOVE, ("[move m]", ["frames = A1 B1", "beats = 2", "bpm = any",
                                                       "offset = 0,0 0,25"])])
        self.assertSkip(self.compile(bad, name="bad"), bad, "m", "offset 0,25 moves 50 screen px: at most 48",
                        "offset")

    def test_default_section(self):
        # C.3's note: [DEFAULT] is an unknown section, not defaults lent to every other one.
        ini = "[DEFAULT]\nbpm = any\nscale = 3\n\n" + ini_text(moves=[("[move m]", ["frames = A1", "beats = 1"])])
        r = self.compile(ini)
        self.assertLine(r, "dancer.ini:1: warning: unknown section [DEFAULT] ignored")
        self.assertSkip(r, ini, "m", "needs bpm = (a range like 90-130, any, or none)")
        self.assertEqual(Mpdn(r.data).geom.scale, 1)

    def test_only_the_first_failure(self):
        ini = ini_text(moves=[BASE_MOVE, ("[move m]", ["frames = A1", "beats = 3", "bpm = 30-20", "sway = 99"])])
        r = self.compile(ini)
        self.assertSkip(r, ini, "m", "beats must be 1, 2, 4 or 8", "beats")
        self.assertEqual([m for m in self.messages(r) if '"m"' in m], [
            'dancer.ini:%d: move "m": beats must be 1, 2, 4 or 8 (skipped)' % ini_line(ini, "[move m]", "beats")])


# ===========================================================================
# Sheets: colours, alpha, key, palettes; and the warnings and notes of C.6.

class WarningTest(DpCase):
    def test_partial_alpha(self):
        s = base_sheet()
        s.put("A1", 0, 0, W + (200,))
        s.put("A1", 7, 0, W + (127,))
        s.put("B1", 0, 0, W + (128,))
        s.put("C1", 0, 0, W + (50,))      # an unused cell: not counted
        ini = ini_text(moves=[("[move base]", ["frames = A1 B1", "beats = 2", "bpm = any"])])
        r = self.compile(ini, s.png())
        self.assertTrue(r.ok, r.messages)
        self.assertLine(r, "sheet.png: warning: 3 px with partial alpha in A1, B1: alpha 128-254 counts as "
                           "opaque, 1-127 as transparent")
        p = Mpdn(r.data)
        self.assertEqual((p.grids[0][0][0], p.grids[0][0][7]), (1, 0))

    def test_merges_and_background(self):
        s = base_sheet()
        s.fill("A1", 2, 6, 5, 2, (0x10, 0x14, 0x18))     # A1's feet
        ini = ini_text(moves=[("[move base]", ["frames = A1 A2", "beats = 2", "bpm = any"])])
        r = self.compile(ini, s.png())
        self.assertTrue(r.ok, r.messages)
        self.assertLine(r, "sheet.png: warning: colour 2 #101418 is close to the background #080c10: it will vanish")
        self.assertLine(r, "dancer.ini:1: warning: idle colour 2 #0d0e0f is close to the background #080c10: it "
                           "will vanish while idle")
        self.assertLine(r, "sheet.png: note: #feffff merges with #ffffff (both RGB565 0xFFFF)")
        p = Mpdn(r.data)
        self.assertEqual(p.palette[1], W)
        self.assertEqual(p.idle_palette[2], (0x0D, 0x0E, 0x0F))
        ini = ini_text(dancer_keys(add=["palette = #ffffff #ff0000 #0000ff", "idle_palette = #808080 #101418 #000080"]))
        r = self.compile(ini, name="given")
        self.assertWarning(r, ini, "[dancer]", "idle_palette",
                           "idle colour 2 #101418 is close to the background #080c10: it will vanish while idle")
        self.assertWarning(r, ini, "[dancer]", "palette", "palette colour #0000ff isn't used")

    def test_fonts(self):
        ini = ini_text(dancer_keys({"name": "Robo 日本", "author": "Ana ☃"},
                                   add=["version = 1\U0001F600"]))
        for cp in (0x65E5, 0x672C, 0x2603, 0x1F600):
            self.assertNotIn(cp, ref_font_coverage())
        r = self.compile(ini)
        self.assertTrue(r.ok, r.messages)
        lack = 'the device fonts lack will show as a look-alike or "?"'
        self.assertWarning(r, ini, "[dancer]", "name", "name: 2 characters %s: U+65E5, U+672C" % lack)
        self.assertWarning(r, ini, "[dancer]", "author", "author: 1 character %s: U+2603" % lack)
        self.assertWarning(r, ini, "[dancer]", "version", "version: 1 character %s: U+1F600" % lack)
        r = self.compile(ini_text(dancer_keys({"name": "Tëst"})), name="covered")
        self.assertNoLine(r, "fonts lack")

    def test_timing_warnings(self):
        ini = ini_text(moves=[BASE_MOVE,
                              ("[move snap]", ["frames = A1 B1", "beats = 1", "bpm = any", "timing = 0.26 3/4"]),
                              ("[move third]", ["frames = A1 B1 C1", "beats = 1", "bpm = any",
                                                "timing = 0.33 1/3 1/3"]),
                              ("[move hit]", ["frames = A1 B1 C1", "beats = 2", "bpm = any", "timing = 1/2 1 1/2"]),
                              ("[move split]", ["frames = A1 b1 C1", "beats = 2", "bpm = any"])])
        r = self.compile(ini)
        self.assertTrue(r.ok, r.messages)
        self.assertWarning(r, ini, "[move snap]", "timing", 'move "snap": timing 0.26 snapped to 1/4 beat')
        self.assertWarning(r, ini, "[move third]", "timing", 'move "third": timing 0.33 snapped to 1/3 beat')
        hold = "no step starts on it; fine for a hold, otherwise set timing = so one does"
        self.assertWarning(r, ini, "[move hit]", "timing", 'move "hit": beat 2 lands inside step 2 (B1): ' + hold)
        self.assertWarning(r, ini, "[move split]", None, 'move "split": beat 2 lands inside step 2 (b1): ' + hold)
        self.assertEqual([s.dur for s in Mpdn(r.data).move("snap").steps], [3, 9])
        self.assertNoLine(r, '"base".*lands inside')

    def test_blank_steps(self):
        # C.6's note: a blank step makes the dancer disappear; the echo table marks it.
        ini = ini_text(moves=[BASE_MOVE, ("[move gap]", ["frames = A1 A3 B1 a3", "beats = 2", "bpm = any"]),
                              ("[move rest]", ["frames = A1 A3", "bpm = none", "timing = 1s 300ms"])])
        r = self.compile(ini)
        self.assertTrue(r.ok, r.messages)
        self.assertLine(r, 'dancer.ini:%d: note: move "gap": step 2 (A3) is blank: the dancer disappears for 1/2 beat'
                        % ini_line(ini, "[move gap]", "frames"))
        self.assertLine(r, 'dancer.ini:%d: note: move "gap": step 4 (a3) is blank: the dancer disappears for 1/2 beat'
                        % ini_line(ini, "[move gap]", "frames"))
        self.assertLine(r, 'dancer.ini:%d: note: move "rest": step 2 (A3) is blank: the dancer disappears for 300 ms'
                        % ini_line(ini, "[move rest]", "frames"))
        rep = lines_of(r.report)
        head = 'dancer.ini:%d: move "gap"' % ini_line(ini, "[move gap]")
        i = next(k for k, l in enumerate(rep) if l.startswith(head))
        self.assertEqual([row.split()[:3] for row in rep[i + 2:i + 6]],
                         [["1", "A1", "6"], ["2", "A3", "(blank)"], ["3", "B1", "6"], ["4", "a3", "(blank)"]])

    def test_rest_frame_on_the_ground(self):
        # C.6: the rest frame's lowest drawn row should be anchor y - 1.
        s = Sheet(1, 1, 8, 10)
        figure(s, "A1")                       # rows 0-7 of a 10-row cell, the default anchor 4,10
        r = self.compile(ini_text(dancer_keys({"cell": "8x10"})), s.png())
        self.assertTrue(r.ok, r.messages)
        self.assertLine(r, "dancer.ini:1: warning: the rest frame A1's feet end on row 7, 2 native px above the "
                           "ground row (anchor y 10): set anchor = 4,8 to stand it on the ground")
        ini = ini_text(dancer_keys(add=["anchor = 4,6"]))
        r = self.compile(ini, name="below")
        self.assertWarning(r, ini, "[dancer]", "anchor", "the rest frame A1's feet end on row 7, 2 native px below "
                                                         "the ground row (anchor y 6): set anchor = 4,8 to stand it "
                                                         "on the ground")
        s = Sheet(1, 1, 8, 12)
        figure(s, "A1")                       # at scale 4, anchor 4,8 would put 16 px under the ground
        r = self.compile(ini_text(dancer_keys({"cell": "8x12"}, add=["scale = 4"])), s.png(), name="lower")
        self.assertLine(r, "dancer.ini:1: warning: the rest frame A1's feet end on row 7, 4 native px above the "
                           "ground row (anchor y 12): draw it 4 native px lower")
        self.assertNoLine(self.compile(ini_text(), name="ground"), "rest frame")

    def test_runs_on_into_the_next_cell(self):
        # C.6: a cell width that cuts the drawings in two.
        s = Sheet(4, 1, 8, 8)
        for left in ("A1", "C1"):
            s.fill(left, 3, 2, 5, 6, W)
        for right in ("B1", "D1"):
            s.fill(right, 0, 2, 4, 6, R)
        ini = ini_text(moves=[("[move m]", ["frames = A1 B1 C1 D1", "beats = 2", "bpm = any"])])
        r = self.compile(ini, s.png())
        self.assertTrue(r.ok, r.messages)
        self.assertLine(r, "sheet.png: warning: the drawing in A1 runs on into B1: is cell = 8x8 right?")
        self.assertNoLine(self.compile(robo_ini(), name="robo2"), "runs on")
        self.assertNoLine(testpack_result(), "runs on")

    def test_feet_drift_rules(self):
        # C.6: a step lifted by its key (dy != 0) isn't standing, so section 4.5's
        # point doesn't warn; a glide (an ease other than hold) carries the feet
        # to the next key by the step's end, so drawing the feet against the
        # offsets can't hide an offset sway.
        ini = ini_text(moves=[BASE_MOVE, ("[move point]", ["frames = A1 A1 A1 A1", "beats = 4", "bpm = any",
                                                           "timing = 1/2 1/2 1 2", "offset = 0,0 2,-2 4,-3 2,-2",
                                                           "ease = inout"])])
        self.assertNoLine(self.compile(ini), "feet drift")
        s = Sheet(2, 1, 16, 8)
        s.fill("A1", 4, 0, 5, 8, W)           # feet at x 4-8
        s.fill("B1", 8, 0, 5, 8, W)           # x 8-12: drawn 4 px right, against the -2,+2 offsets
        for ease, drift in (("hold", None), ("linear", 8)):
            ini = ini_text(dancer_keys({"cell": "16x8"}), [("[move sway]", ["frames = A1 B1", "bpm = none",
                                                                              "timing = 2s 2s", "offset = 2,0 -2,0",
                                                                              "ease = " + ease])])
            r = self.compile(ini, s.png(), name="sway-" + ease)
            if drift is None:
                self.assertNoLine(r, "feet drift")
            else:
                self.assertWarning(r, ini, "[move sway]", None, 'move "sway": the feet drift %d native px across its '
                                                                 'standing frames' % drift)

    def test_section_4_5_example(self):
        # The doc's own manifest (section 4.5), over art drawn in place: only the
        # point's hit and the head-nod's genre note, no feet drift (point's lifted
        # steps don't stand; sway draws its motion, with no offsets).
        doc = (REPO / "docs" / "DANCE-SPRITES.md").read_text(encoding="utf-8").replace("\r\n", "\n")
        m = re.search(r"```ini\n(\[dancer\]\nname    = Stick\n.*?)```", doc, re.S)
        self.assertTrue(m, "section 4.5's example")
        ini = m.group(1)
        s = Sheet(8, 12, 64, 76)
        cells = ["%s%d" % (c, r) for r in (1, 7) for c in "ABCDEFGH"] + ["A4", "B4", "C4", "A9", "B9", "C9"] + \
                ["%s12" % c for c in "ABCDE"]
        for cell in cells:
            s.fill(cell, 28, 20, 9, 8, W)          # the head, its top at stage row 49
            s.fill(cell, 31, 28, 3, 28, W)
            s.fill(cell, 26, 56, 3, 16, R)         # the feet end on row 71: anchor y 72
            s.fill(cell, 36, 56, 3, 16, R)
        r = self.compile(ini, s.png(), name="stick")
        self.assertTrue(r.ok, r.messages)
        self.assertEqual(list(r.skipped), [])
        line = functools.partial(ini_line, ini)
        got = [m for m in self.messages(r) if re.match(r"^(dancer\.ini:\d+: |sheet\.png: )?(warning|note): ", m)]
        self.assertEqual(sorted(got), sorted([
            'dancer.ini:%d: warning: move "point": beat 4 lands inside step 4 (B4): no step starts on it; fine for '
            'a hold, otherwise set timing = so one does' % line("[move point]", "timing"),
            'dancer.ini:%d: note: move "head-nod": genre tags keep it dormant until tracks carry genres'
            % line("[move head-nod]", "genre")]))
        self.assertEqual(len(Mpdn(r.data).move("sway").steps), 8)

    def test_control_characters_are_escaped(self):
        # C.5: echoed text prints C0, DEL and C1 controls as \xNN.
        ini = ini_text(moves=[BASE_MOVE, ("[\x1b]0;owned\x07]", ["x = 1"]),
                              ("[move m]", ["frames = A1\x1b[31m", "beats = 1", "bpm = any"])])
        r = self.compile(ini)
        self.assertTrue(r.ok, r.messages)
        self.assertLine(r, "dancer.ini:%d: warning: unknown section [\\x1b]0;owned\\x07] ignored"
                        % ini_line(ini, "[\x1b]0;owned\x07]"))
        self.assertLine(r, 'dancer.ini:%d: move "m": frames: "A1\\x1b[31m" isn\'t a cell of the 4x3 sheet (skipped)'
                        % ini_line(ini, "[move m]", "frames"))
        for line in lines_of(r.report):
            self.assertFalse(re.search("[\x00-\x1f\x7f-\x9f]", line), repr(line))

    def test_key_warnings(self):
        ini = ini_text(dancer_keys(add=["faces = right"]),
                       [BASE_MOVE,
                        ("[move jump]", ["frames = A1 B1", "beats = 2", "bpm = any", "offset = 1,0 0,0"]),
                        ("[move drift]", ["frames = A1 A1", "beats = 2", "bpm = any", "offset = 0,0 2,0"]),
                        ("[move steady]", ["frames = A1 A1", "beats = 2", "bpm = any", "offset = 0,0 1,0"]),
                        ("[move back]", ["frames = A1 B1", "beats = 2", "bpm = any", "travel = -1"]),
                        ("[move ahead]", ["frames = A1 B1", "beats = 2", "bpm = any", "travel = 1"]),
                        ("[move future]", ["frames = A1", "beats = 1", "bpm = any", "song_bpm = 100-120"]),
                        ("[move tagged]", ["frames = A1", "beats = 1", "bpm = any", "genre = disco"])])
        r = self.compile(ini)
        self.assertTrue(r.ok, r.messages)
        self.assertWarning(r, ini, "[move jump]", "offset", 'move "jump": the first key is 1,0, not 0,0: the '
                                                            'dancer jumps when another move follows')
        self.assertWarning(r, ini, "[move drift]", None, 'move "drift": the feet drift 2 native px across its '
                                                         'standing frames')
        self.assertNoLine(r, '"(steady|jump)": the feet drift')
        self.assertWarning(r, ini, "[move back]", "travel", 'move "back": travels left while the art faces right')
        self.assertNoLine(r, '"ahead": travels')
        self.assertWarning(r, ini, "[move future]", "song_bpm",
                           'move "future": song_bpm needs track BPM data: skipped in this version')
        self.assertLine(r, 'dancer.ini:%d: note: move "tagged": genre tags keep it dormant until tracks carry '
                           'genres' % ini_line(ini, "[move tagged]", "genre"))
        self.assertEqual(list(r.skipped), [])
        future = Mpdn(r.data).move("future")
        self.assertEqual((future.needs, future.song_lo, future.song_hi), (1, 100, 120))
        self.assertIn("future", r.budgets)

    def test_faces_left(self):
        ini = ini_text(dancer_keys(add=["faces = left"]),
                       [BASE_MOVE, ("[move ahead]", ["frames = A1 B1", "beats = 2", "bpm = any", "travel = 1"]),
                        ("[move back]", ["frames = A1 B1", "beats = 2", "bpm = any", "travel = -1"])])
        r = self.compile(ini)
        self.assertTrue(r.ok, r.messages)
        self.assertWarning(r, ini, "[move ahead]", "travel", 'move "ahead": travels right while the art faces left')
        self.assertNoLine(r, '"back": travels')
        self.assertEqual(Mpdn(r.data).geom.faces, 2)

    def big_sheet(self):
        s = Sheet(3, 1, 40, 40)
        s.fill("A1", 0, 0, 40, 40, R)
        s.fill("B1", 0, 8, 40, 30, B)
        s.fill("C1", 18, 30, 4, 8, W)
        return s

    def test_budget_lights(self):
        keys = dancer_keys({"cell": "40x40"}, add=["scale = 4", "anchor = 20,38", "rest = C1"])
        ini = ini_text(keys, [("[move block]", ["frames = A1 A1", "beats = 2", "bpm = any"]),
                              ("[move held]", ["frames = A1 A1", "beats = 2", "bpm = any", "heavy = yes"]),
                              ("[move wide]", ["frames = B1", "beats = 1", "bpm = any"])])
        r = self.compile(ini, self.big_sheet().png())
        self.assertTrue(r.ok, r.messages)
        self.assertSkip(r, ini, "block", "budget red: peak 25,600 px is over 20,000; set heavy = yes to keep it")
        self.assertLine(r, 'dancer.ini:%d: move "held": 2 beats, pace any; red (heavy): median 25,600 px, peak '
                           '25,600 px, over 20,000' % ini_line(ini, "[move held]"))
        self.assertWarning(r, ini, "[move held]", "heavy",
                           'move "held": budget red (peak 25,600 px), kept because heavy = yes')
        self.assertLine(r, 'dancer.ini:%d: move "wide": 1 beat, pace any; amber: median 19,200 px, peak 19,200 of '
                           '20,000 px' % ini_line(ini, "[move wide]"))
        self.assertLine(r, "sheet.png: warning: frame B1: standing, it reaches stage row 33, under the toast "
                           "(rows 0-35)")
        self.assertEqual({k: v.light for k, v in r.budgets.items()}, {"held": "red", "wide": "amber"})
        p = Mpdn(r.data)
        self.assertEqual([m.name_text for m in p.moves], ["held", "wide"])
        self.assertEqual(p.geom.frame_count, 3)     # C1, A1 (held; block is skipped), B1

    def test_runs(self):
        s = Sheet(2, 1, 40, 40)
        for y in range(16):
            for x in range(0, 40, 2):
                s.put("A1", x, y, W)
        s.fill("B1", 18, 30, 4, 10, R)
        ini = ini_text(dancer_keys({"cell": "40x40"}, add=["rest = B1"]),
                       [("[move m]", ["frames = A1 B1", "beats = 2", "bpm = any"])])
        r = self.compile(ini, s.png())
        self.assertTrue(r.ok, r.messages)
        self.assertLine(r, "sheet.png: warning: frame A1: 320 runs, over 300: slow to draw")
        self.assertNoLine(r, "frame B1: .* runs")

    def test_coverage_warnings(self):
        r = self.compile(ini_text(moves=()))
        self.assertTrue(r.ok, r.messages)
        self.assertLine(r, "warning: no moves: only the engine's auto-bounce and auto-idle will play")
        gap = "warning: no beat move at dance tempo %s: auto-bounce plays there"
        r = self.compile(ini_text(moves=[("[move m]", ["frames = A1", "beats = 4", "bpm = 90-130"])]), name="one")
        self.assertLine(r, gap % "72-89")
        self.assertLine(r, gap % "131-176")
        moves = [("[move a]", ["frames = A1", "beats = 4", "bpm = 90-130"]),
                 ("[move b]", ["frames = A1", "beats = 4", "bpm = 132-176"])]
        r = self.compile(ini_text(moves=moves), name="two")
        # 72-88 by b at double time, 90-130 by a, 132-176 by b: 89 and 131 alone are uncovered
        self.assertEqual([t for t in range(72, 177)
                          if all(ref_eligible_k(10 * t, lo, hi, None, 4) is None for lo, hi in ((90, 130), (132, 176)))],
                         [89, 131])
        self.assertEqual([m for m in self.messages(r) if "no beat move" in m], [gap % "89", gap % "131"])
        for label, extra in (("tag", "genre = disco"), ("needs", "song_bpm = 100-120")):
            r = self.compile(ini_text(moves=[("[move m]", ["frames = A1", "beats = 1", "bpm = any", extra])]),
                             name=label)
            self.assertLine(r, gap % "72-176")
        r = self.compile(ini_text(), name="base")
        self.assertNoLine(r, "no beat move")
        # B.7: a travel move of 2 beats can't dance at double time (D = 1), so it covers 140-176 only
        r = self.compile(ini_text(moves=[("[move glide]", ["frames = A1 B1", "beats = 2", "bpm = 140-180",
                                                           "travel = 1"])]), name="glide")
        self.assertEqual([m for m in self.messages(r) if "no beat move" in m], [gap % "72-139"])


# ===========================================================================
# The whole layout of a hand-written dancer (A.5-A.14).

ROBO_DANCER = ("name = Robo", "author = Ana", "license = MIT ; the licence", "sheet = sheet.png", "cell = 8x8",
               "scale = 2", "shadow = 12", "url = https://example.org/robo", "version = 1.2",
               "x-note = ignored")
ROBO_MOVES = (
    ("[move step]", ("frames = B1 C1 D1 B2", "beats = 2", "timing = 1/2 1/2 .5 0.5",
                     "offset = 0,0 0,-1 0,-2 0,-1", "ease = linear in out inout", "mirror = yes", "place = start",
                     "weight = 0.5", "repeat = 2", "from = stand", "to = lean-left", "snap = yes", "bounce = 1",
                     "sway = 2", "bpm = 90-130", "genre = Disco, Funk")),
    ("[move trip]", ("row = 1", "beats = 4", "bpm = any", "travel = -1", "travel_ease = out", "start_at = right",
                     "go = 2-3", "heavy = yes", "from = lean-left", "to = stand")),
    ("[move idle]", ("frames = A2 D2", "bpm = none", "timing = 500ms 300ms", "when = listening", "mirror = yes",
                     "offset = 0,0 0,1")),
    ("[move future]", ("frames = A1", "beats = 1", "bpm = any", "song_bpm = 100-120")),
    ("[move broken]", ("frames = C2", "beats = 1", "bmp = any")),
)


def robo_ini():
    return ini_text(ROBO_DANCER, ROBO_MOVES)


class LayoutTest(DpCase):
    def build(self):
        ini = robo_ini()
        r = self.compile(ini)
        self.assertTrue(r.ok, r.messages)
        return ini, r, Mpdn(r.data)

    def test_genre_runs_shared(self):
        # A.11 step 3: a run equal to an earlier one isn't written again; the same
        # tags in another order are another run.
        moves = [("[move a]", ["frames = A1", "beats = 1", "bpm = any", "genre = disco, funk"]),
                 ("[move b]", ["frames = A1", "beats = 1", "bpm = any", "genre = disco, funk"]),
                 ("[move c]", ["frames = A1", "beats = 1", "bpm = any", "genre = funk, disco"])]
        p = Mpdn(self.compile(ini_text(moves=moves)).data)
        a, b, c = p.move("a"), p.move("b"), p.move("c")
        self.assertEqual(a.genre_first, b.genre_first)
        self.assertNotEqual(c.genre_first, a.genre_first)
        self.assertEqual((a.genres, b.genres, c.genres), (["disco", "funk"],) * 2 + (["funk", "disco"],))
        self.assertEqual(list(p.strings.values()), ["a", "b", "c", "disco", "funk", "funk", "disco"])
        self.assertEqual(a.genre_first, [o for o, t in p.strings.items() if t == "disco"][0])

    def test_pose_key_order(self):
        # A.11 step 1: from, to, then the twin of from, then the twin of to
        moves = [("[move a]", ["frames = A1", "beats = 1", "bpm = any", "from = lean-left", "to = stand"])]
        p = Mpdn(self.compile(ini_text(moves=moves)).data)
        self.assertEqual(p.pose_keys, ["lean-left", "stand", "lean-right"])
        m = p.moves[0]
        self.assertEqual((m.from_key, m.to_key, m.from_key_m, m.to_key_m), (1, 2, 3, 2))

    def test_travel_ease_default(self):
        # A.12, C.3: travelEase is 4 (inout) for a travel move that doesn't set it, 0 without travel
        moves = [("[move w]", ["frames = A1 B1", "beats = 2", "bpm = any", "travel = 1"]),
                 ("[move s]", ["frames = A1 B1", "beats = 2", "bpm = any"])]
        p = Mpdn(self.compile(ini_text(moves=moves)).data)
        self.assertEqual((p.move("w").travel_ease, p.move("s").travel_ease), (4, 0))

    def test_meta_geom_pals(self):
        ini, r, p = self.build()
        self.assertEqual((p.id, p.name, p.author, p.licence, p.url, p.version, p.declared_moves),
                         ("robo", "Robo", "Ana", "MIT", "https://example.org/robo", "1.2", 4))
        self.assertEqual(p.geom, RGeom(2, 0, 8, 8, 4, 8, 12, 5, 0))
        # first appearance over the used cells (C2 too, though only the skipped move uses it), merged
        dance = [(0, 0, 0), W, R, G, B, Y] + [(0, 0, 0)] * 10
        self.assertEqual(p.palette, dance)
        self.assertEqual(p.idle_palette, [(0, 0, 0)] + [ref_idle_colour(c) for c in dance[1:6]] + [(0, 0, 0)] * 10)

    def test_frames(self):
        ini, r, p = self.build()
        s = base_sheet()
        index = {ref_rgb565(*c): i for i, c in enumerate(p.palette) if i}
        cells = ["A1", "B1", "B2", "A2", "D2"]
        self.assertEqual(p.grids, [s.grid(c, index) for c in cells])
        self.assertEqual(p.boxes, [ref_box(g) for g in p.grids])
        self.assertEqual(p.thumb, ref_thumbnail(p.grids[0], p.boxes[0]))
        frames = [(st.frame, st.mirrored) for st in p.steps]
        self.assertEqual(frames, [(1, False), (1, True), (0, False), (2, False),       # step: B1 C1 D1 B2
                                  (0, False), (1, False), (1, True), (0, False),       # trip: row 1
                                  (3, False), (4, False),                              # idle: A2 D2
                                  (0, False)])                                         # future: A1

    def test_strings_and_moves(self):
        ini, r, p = self.build()
        self.assertEqual(p.pose_keys, ["stand", "lean-left", "lean-right"])
        self.assertEqual(list(p.strings.items()), [
            (4, "stand"), (11, "lean-left"), (22, "lean-right"), (34, "step"), (40, "trip"), (46, "idle"),
            (52, "future"), (60, "Disco"), (67, "Funk")])
        rec = {m.name_text: m for m in p.moves}
        self.assertEqual([m.name_text for m in p.moves], ["step", "trip", "idle", "future"])
        cols = MOVS_FIELDS[:-1]
        want = {
            "step": (34, 0, 2, 90, 130, 5, 0, 1 | 4 | 32, 2, 0, 0, 0, 0, 1, 2, 1, 2, 1, 3, 60, 2, 4, 0, 0, 0),
            "trip": (40, 0, 4, 0, 0, 10, 0, 16, 0, 0, -1, 4, 3, 0, 0, 2, 1, 3, 1, 0, 0, 4, 4, 0, 0),
            "idle": (46, 1, 0, 0, 0, 10, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 8, 0, 0),
            "future": (52, 0, 1, 0, 0, 10, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 10, 100, 120),
        }
        for name, values in want.items():
            self.assertEqual(tuple(getattr(rec[name], c) for c in cols), values, name)
            self.assertEqual(rec[name].reserved, 0)
        self.assertEqual(rec["step"].genres, ["Disco", "Funk"])
        self.assertEqual(p.raw_steps, [
            (1, 6, 0, 0, 1, 0), (0x8001, 6, 0, -1, 2, 0), (0, 6, 0, -2, 3, 0), (2, 6, 0, -1, 4, 0),
            (0, 12, 0, 0, 0, 0), (1, 12, 0, 0, 0, 1), (0x8001, 12, 0, 0, 0, 1), (0, 12, 0, 0, 0, 0),
            (3, 50, 0, 0, 0, 0), (4, 30, 0, 1, 0, 0),
            (0, 12, 0, 0, 0, 0)])

    def test_messages(self):
        ini, r, p = self.build()
        line = functools.partial(ini_line, ini)
        status = {}
        for m in p.moves:
            _, med, peak, light = ref_budget(p.geom, p.boxes, m)
            limit = {"green": "16,000", "amber": "20,000"}.get(light)
            peak_text = ("peak {:,} of %s px" % limit if limit else "peak {:,} px, over 20,000").format(peak)
            status[m.name_text] = "%s: median %s px, %s" % (light if limit else "red (heavy)", "{:,}".format(med),
                                                            peak_text)
            b = r.budgets[m.name_text]
            self.assertEqual((b.median, b.peak, b.light), (med, peak, light), m.name_text)
            self.assertEqual([(c.fps, c.pace, bool(c.mirrored), c.spot, c.frames, c.median, c.peak) for c in b.cases],
                             ref_budget(p.geom, p.boxes, m)[0], m.name_text)
        expect = [
            "sheet.png: note: #feffff merges with #ffffff (both RGB565 0xFFFF)",
            'dancer.ini:%d: move "step": 2 beats, pace 90-130; %s' % (line("[move step]"), status["step"]),
            'dancer.ini:%d: note: move "step": genre tags keep it dormant until tracks carry genres'
            % line("[move step]", "genre"),
            'dancer.ini:%d: move "trip": 4 beats, pace any; %s' % (line("[move trip]"), status["trip"]),
            'dancer.ini:%d: move "idle": no BPM, when listening, 800 ms; %s' % (line("[move idle]"), status["idle"]),
            'dancer.ini:%d: move "future": 1 beat, pace any; %s' % (line("[move future]"), status["future"]),
            'dancer.ini:%d: warning: move "future": song_bpm needs track BPM data: skipped in this version'
            % line("[move future]", "song_bpm"),
            'dancer.ini:%d: move "broken": unknown key "bmp": did you mean bpm? (skipped)'
            % line("[move broken]", "bmp"),
        ]
        msgs = self.messages(r)          # C.6's note: #rrggbb lowercase, 0x digits uppercase
        positions = []
        for e in expect:
            self.assertIn(e, msgs, "\n" + "\n".join(msgs))
            positions.append(msgs.index(e))
        self.assertEqual(positions, sorted(positions), "C.5's order")
        summary = [m for m in msgs if m.startswith("robo: ")]
        self.assertEqual(len(summary), 1, msgs)
        # C.5: <n> counts the moves the pack holds, then the skipped of the moves written (5)
        self.assertEqual(summary[0], "robo: 4 moves (1 of 5 skipped), 5 frames, %s bytes" % "{:,}".format(len(r.data)))
        self.assertGreater(msgs.index(summary[0]), positions[-1])
        self.assertEqual(list(r.skipped), ["broken"])
        self.assertEqual(sorted(r.budgets), ["future", "idle", "step", "trip"])
        self.assertFalse(any("warning" in m for m in msgs if "x-note" in m))

    def test_read_pack_agrees(self):
        ini, r, p = self.build()
        q = dp.read_pack(r.data)
        self.assertEqual(list(q.pose_keys), p.pose_keys)
        self.assertEqual([m.name for m in q.moves], ["step", "trip", "idle", "future"])
        step = q.moves[0]
        self.assertEqual((step.from_key, step.to_key, step.from_key_m, step.to_key_m),
                         ("stand", "lean-left", "stand", "lean-right"))
        self.assertEqual(tuple(step.genres), ("Disco", "Funk"))
        self.assertEqual([tuple(s) for s in q.moves[1].steps],
                         [(0, False, 12, 0, 0, 0, False), (1, False, 12, 0, 0, 0, True),
                          (1, True, 12, 0, 0, 0, True), (0, False, 12, 0, 0, 0, False)])
        self.assertEqual([tuple(x) for x in q.ignored],
                         [(3, "song_bpm needs track BPM data: skipped in this version")])
        self.assertEqual([[bytes(row) for row in g] for g in q.grids], p.grids)


class MirrorMergeTest(DpCase):
    """A.14: which cells are stored once, mirrored about the anchor column."""

    def test_anchor_column(self):
        s = Sheet(4, 3, 8, 8)                       # the default anchor: 4,8
        figure(s, "A1")
        figure(s, "B1", head_x=4)                   # a lean
        figure(s, "C1", head_x=2)                   # B1 mirrored about the anchor column (x -> 8 - x)
        for y in range(8):                          # D1: B1 mirrored about the cell's centre (x -> 7 - x)
            for x in range(8):
                s.put("D1", 7 - x, y, s.get("B1", x, y))
        figure(s, "A2")
        s.put("A2", 0, 3, W)                        # A1 plus a pixel whose mirror image (x 8) leaves the cell
        ini = ini_text(moves=[("[move lean]", ["frames = B1 C1 D1 A2", "beats = 2", "bpm = any"]),
                              ("[move gap]", ["frames = A1 A3 B3 A1", "beats = 2", "bpm = any"])])
        r = self.compile(ini, s.png())
        self.assertTrue(r.ok, r.messages)
        p = Mpdn(r.data)
        cells = ("B1", "C1", "D1", "A2", "A1", "A3", "B3", "A1")
        self.assertEqual([(st.frame, st.mirrored) for st in p.steps],
                         [(1, False), (1, True), (2, False), (3, False),     # C1 is B1 drawn mirrored; D1 isn't
                          (0, False), (4, False), (4, False), (0, False)])   # every blank cell is one frame
        index = {ref_rgb565(*c): i for i, c in enumerate(p.palette) if i}
        self.assertEqual(p.grids, [s.grid(c, index) for c in ("A1", "B1", "D1", "A2", "A3")])
        # Merging never changes what is drawn (B.4 mirrors about the anchor too) ...
        for c, st in zip(cells, p.steps):
            self.assertEqual(drawn_pixels(p.grids[st.frame], 4, st.mirrored), drawn_pixels(s.grid(c, index), 4, False),
                             c)
        # ... so the compiler's budget, from the cells as drawn, is the device's, from the stored frames.
        q = dp.read_pack(r.data)
        self.assertEqual(list(q.ignored), [])
        for m in q.moves:
            self.assertEqual(dp.budget(q.geom, q.boxes, m), r.budgets[m.name], m.name)


class ThumbnailTest(DpCase):
    def test_downscaled_rest_frame(self):
        s = Sheet(1, 1, 60, 60)
        cols = [R, G, B, Y, (0, 255, 255)]
        for y in range(60):
            for x in range(5, 55):
                s.put("A1", x, y, cols[(x * 7 + y * 3) % 5])
        for x, y, c in ((5, 0, G), (6, 0, R), (5, 1, R), (6, 1, G), (7, 0, B), (8, 0, None), (7, 1, None),
                        (8, 1, None)):
            s.put("A1", x, y, c)
        ini = ini_text(dancer_keys({"cell": "60x60"}, add=["scale = 2",
                                                         "palette = #ff0000 #00ff00 #0000ff #ffff00 #00ffff"]))
        r = self.compile(ini, s.png())
        self.assertTrue(r.ok, r.messages)
        p = Mpdn(r.data)
        self.assertEqual(p.boxes[0], (5, 0, 50, 60))
        self.assertEqual(p.thumb, ref_thumbnail(p.grids[0], p.boxes[0]))
        # s = 2, 25x30 at (7, 5): the tie block gives index 1, the lone blue pixel index 3
        self.assertEqual((p.thumb[5 * 20 + 3] & 15, p.thumb[5 * 20 + 4] >> 4), (1, 3))
        self.assertEqual(p.thumb[5 * 20 + 3] >> 4, 0)

    def test_upscaled_rest_frame(self):
        s = Sheet(1, 1, 16, 8)
        rnd = lcg(5)
        for y in range(1, 8):
            for x in range(2, 15):
                s.put("A1", x, y, [R, G, B][next(rnd) % 3])
        r = self.compile(ini_text(dancer_keys({"cell": "16x8"})), s.png())
        p = Mpdn(r.data)
        self.assertEqual(p.boxes[0], (2, 1, 13, 7))
        self.assertEqual(p.thumb, ref_thumbnail(p.grids[0], p.boxes[0]))
        self.assertEqual(p.thumb[:9 * 20], bytes(180))         # 39x21 at m = 3, from row 9


# ===========================================================================
# What check prints: status lines, echo tables, the coverage report (C.5).

class ReportTest(DpCase):
    def report(self, result):
        return lines_of(result.report)

    def test_coverage_report(self):
        moves = [BASE_MOVE,
                 ("[move slow]", ["frames = A1 B1", "beats = 4", "bpm = 36-60"]),
                 ("[move nod]", ["frames = A1", "beats = 1", "bpm = 70-100", "genre = hip hop"]),
                 ("[move fast]", ["frames = A1", "beats = 1", "bpm = 150-180"]),
                 ("[move fut]", ["frames = A1", "beats = 1", "bpm = any", "song_bpm = 100-120"]),
                 ("[move breathe]", ["frames = A1 B1", "bpm = none", "timing = 1s 1s", "when = quiet"]),
                 ("[move look]", ["frames = A1 B1", "bpm = none", "timing = 1s 1s", "when = listening"]),
                 ("[move sway]", ["frames = A1 B1", "bpm = none", "timing = 1s 1s"])]
        r = self.compile(ini_text(moves=moves))
        self.assertTrue(r.ok, r.messages)
        rep = self.report(r)
        i = next(k for k, l in enumerate(rep) if l.startswith("coverage:"))
        self.assertEqual(rep[i], "coverage: pace at dance tempo T (h = half time, d = double time, . = not eligible)")
        rows = [
            ("T", "72 80 90 100 110 120 130 140 150 160 176"),
            ("base", "72 80 90 100 110 120 130 140 150 160 176"),
            ("slow", "36h 40h 45h 50h 55h 60h . . . . ."),
            ("nod*", "72 80 90 100 . . . 70h 75h 80h 88h"),
            ("fast", ". 160d 180d . . . . . 150 160 176"),
        ]
        for k, (label, cells) in enumerate(rows):
            line = rep[i + 1 + k]
            self.assertTrue(line.startswith("  " + label + " "), line)
            self.assertEqual(line.split(), [label] + cells.split())
            tail = line[-55:]
            self.assertEqual([tail[5 * c:5 * c + 5] for c in range(11)], [c.rjust(5) for c in cells.split()])
        self.assertEqual(rep[i + 6], "  * genre-tagged: dormant until tracks carry genres")
        self.assertEqual(rep[i + 7], "idles: quiet: breathe, sway; listening: look, sway")
        self.assertEqual(rep[i + 8], "edges: every move fits at both edge spots")
        # computed by the reference too
        for label, lo, hi, beats in (("slow", 36, 60, 4), ("nod", 70, 100, 1), ("fast", 150, 180, 1)):
            cells = []
            for t in (72, 80, 90, 100, 110, 120, 130, 140, 150, 160, 176):
                k = ref_eligible_k(10 * t, lo, hi, None, beats)
                cells.append("." if k is None else "%d%s" % (t * 2 ** k if k >= 0 else t // 2,
                                                             {0: "", -1: "h", 1: "d"}[k]))
            self.assertEqual(" ".join(cells), dict(rows)[label + ("*" if label == "nod" else "")])
        build = [l for l in self.messages(r)]
        self.assertFalse(any(l.startswith(("coverage:", "idles:", "edges:")) for l in build))

    def test_coverage_knows_travel(self):
        # B.7: eligible_k skips double time for a travel move under 4 beats, so
        # the report doesn't show 9.3's chasse at 144d and 150d.
        moves = [("[move chasse]", ["frames = A1 B1", "beats = 2", "bpm = 100-150", "travel = 1"]),
                 ("[move grape]", ["frames = A1 B1 A1 B1", "beats = 4", "bpm = 90-160", "travel = 1"])]
        r = self.compile(ini_text(moves=moves))
        self.assertTrue(r.ok, r.messages)
        rep = self.report(r)
        tempos = (72, 80, 90, 100, 110, 120, 130, 140, 150, 160, 176)
        for name, lo, hi, beats in (("chasse", 100, 150, 2), ("grape", 90, 160, 4)):
            line = next(l for l in rep if l.startswith("  %s " % name))
            want = []
            for t in tempos:
                k = ref_eligible_k(10 * t, lo, hi, None, beats, 1)
                pace = t * 2 ** k if k is not None and k >= 0 else t // 2
                want.append("." if k is None else "%d%s" % (pace, {0: "", -1: "h", 1: "d"}[k]))
            self.assertEqual(line.split()[1:], want, name)
        self.assertEqual(next(l for l in rep if l.startswith("  chasse ")).split()[1:3], [".", "."])
        self.assertEqual(next(l for l in rep if l.startswith("  grape ")).split()[1:3], ["144d", "160d"])

    def test_idle_lines(self):
        quiet = [BASE_MOVE, ("[move b]", ["frames = A1 B1", "bpm = none", "timing = 1s 1s", "when = quiet"])]
        r = self.compile(ini_text(moves=quiet))
        self.assertIn("idles: quiet: b; listening: (the quiet idles)", self.report(r))
        listen = [BASE_MOVE, ("[move l]", ["frames = A1 B1", "bpm = none", "timing = 1s 1s", "when = listening"])]
        r = self.compile(ini_text(moves=listen), name="listen")
        self.assertIn("idles: quiet: (the listening idles); listening: l", self.report(r))
        r = self.compile(ini_text(), name="none")
        self.assertIn("idles: quiet: auto-idle; listening: auto-idle", self.report(r))

    def test_edges(self):
        s = Sheet(3, 1, 40, 8)
        s.fill("A1", 12, 0, 16, 8, W)
        s.fill("B1", 0, 0, 40, 8, R)
        s.fill("C1", 18, 0, 4, 8, B)
        moves = [("[move shove]", ["frames = A1 A1", "beats = 2", "bpm = any", "offset = 0,0 -12,0"]),
                 ("[move shove2]", ["frames = A1 A1", "beats = 2", "bpm = any", "offset = 0,0 -12,0",
                                    "mirror = yes"]),
                 ("[move wide]", ["frames = B1", "beats = 1", "bpm = any"]),
                 ("[move walk]", ["frames = C1 C1", "beats = 2", "bpm = any", "travel = 1", "mirror = yes"]),
                 ("[move fits]", ["frames = C1", "beats = 1", "bpm = any"])]
        ini = ini_text(dancer_keys({"cell": "40x8"}, add=["scale = 4"]), moves)
        r = self.compile(ini, s.png())
        self.assertTrue(r.ok, r.messages)
        rep = self.report(r)
        i = rep.index("edges:")
        self.assertEqual(rep[i + 1:i + 5], ["  shove: clamped at spot -1", "  shove2: clamped at both edge spots",
                                            "  wide: clamped at both edge spots",
                                            "  walk: travels from -1 and 0; mirrored from 0 and +1"])
        self.assertWarning(r, ini, "[move shove]", None,
                           'move "shove": the feet drift 12 native px across its standing frames')
        p = Mpdn(r.data)
        self.assertEqual(ref_extent(p.geom, p.boxes, p.move("shove"), False), (-80, 32))

    def test_echo_tables(self):
        moves = [("[move walk]", ["frames = a1 B1 A1 B1", "beats = 4", "bpm = any", "travel = 1", "go = 2-3",
                                  "offset = 0,0 1,0 0,0 -1,0", "ease = linear"]),
                 ("[move idle]", ["frames = A1 B1", "bpm = none", "timing = 500ms 300ms"])]
        ini = ini_text(moves=moves)
        r = self.compile(ini)
        rep = self.report(r)
        i = next(k for k, l in enumerate(rep) if l.startswith('dancer.ini:%d: move "walk": 4 beats, pace any; '
                                                               % ini_line(ini, "[move walk]")))
        table = rep[i + 1:i + 6]
        for row in table:
            self.assertTrue(row.startswith("    "), row)
        self.assertEqual([row.split() for row in table], [
            ["step", "cell", "ticks", "offset", "ease"],
            ["1", "a1", "12", "0,0", "linear"],
            ["2", "B1", "12", "1,0", "linear", "go"],
            ["3", "A1", "12", "0,0", "linear", "go"],
            ["4", "B1", "12", "-1,0", "linear"]])
        i = next(k for k, l in enumerate(rep) if '"idle": no BPM, when any, 800 ms; ' in l)
        self.assertEqual([row.split() for row in rep[i + 1:i + 4]], [
            ["step", "cell", "ms", "offset", "ease"], ["1", "A1", "500", "0,0", "hold"],
            ["2", "B1", "300", "0,0", "hold"]])
        self.assertFalse(any(l.startswith("    ") for l in self.messages(r)))

    def test_status_lines(self):
        # A1's box is 5x8 at scale 1 and nothing moves: every frame dirties 40 px.
        moves = [BASE_MOVE, ("[move rest]", ["frames = A1 A1", "bpm = none", "timing = 300ms 300ms"])]
        ini = ini_text(moves=moves)
        r = self.compile(ini)
        self.assertLine(r, 'dancer.ini:%d: move "base": 1 beat, pace any; green: median 40 px, peak 40 of 16,000 px'
                        % ini_line(ini, "[move base]"))
        self.assertLine(r, 'dancer.ini:%d: move "rest": no BPM, when any, 600 ms; green: median 40 px, peak 40 of '
                           '16,000 px' % ini_line(ini, "[move rest]"))
        self.assertLine(r, "robo: 2 moves (0 of 2 skipped), 1 frame, %s bytes" % "{:,}".format(len(r.data)))


# ===========================================================================
# --strict (C.7) and the command line's exit codes (C.2).

class StrictTest(DpCase):
    def test_licences(self):
        for licence in ALLOWLIST:
            ini = ini_text(dancer_keys({"license": licence}))
            self.assertTrue(self.compile(ini, strict=True, name="l-" + licence.lower().replace(".", "")).ok, licence)
        for licence in ("Proprietary", "mit", "GPL-3.0", "CC-BY-NC-4.0"):
            with self.subTest(licence):
                ini = ini_text(dancer_keys({"license": licence}))
                name = "x-" + re.sub(r"[^a-z0-9]", "", licence.lower())
                self.assertTrue(self.compile(ini, name=name).ok)
                self.assertPackError(self.compile(ini, strict=True, name=name + "s"), (
                    'dancer.ini:4: error: license "%s" isn\'t on the built-in allowlist: GPL-3.0-or-later, '
                    'GPL-2.0-or-later, CC0-1.0, CC-BY-4.0, CC-BY-SA-4.0, MIT, Apache-2.0' % licence))

    def test_skips(self):
        one = ini_text(moves=[BASE_MOVE, ("[move m]", ["frames = A1", "beats = 3", "bpm = any"])])
        self.assertTrue(self.compile(one).ok)
        self.assertPackError(self.compile(one, strict=True, name="s1"), "error: --strict: 1 move skipped")
        two = ini_text(moves=[BASE_MOVE, ("[move m]", ["frames = A1", "beats = 3", "bpm = any"]),
                              ("[move n]", ["frames = A1", "beats = 1"])])
        self.assertPackError(self.compile(two, strict=True, name="s2"), "error: --strict: 2 moves skipped")
        kept = ini_text(moves=[BASE_MOVE, ("[move f]", ["frames = A1", "beats = 1", "bpm = any",
                                                        "song_bpm = 90-100"])])
        self.assertTrue(self.compile(kept, strict=True, name="s3").ok)

    def test_main_exit_codes(self):
        good = self.dancer(ini_text(), base_png(), name="good")
        broken = self.dancer(ini_text(dancer_keys({"cell": "8by8"})), base_png(), name="broken")
        skip = self.dancer(ini_text(moves=[BASE_MOVE, ("[move m]", ["frames = A1", "beats = 3", "bpm = any"])]),
                           base_png(), name="skip")
        out = self.tmp / "good.dnc"
        for argv, code in ((["check", str(good)], 0), (["check", str(good), "--strict"], 0),
                           (["check", str(broken)], 1), (["check", str(skip)], 0), (["check", str(skip), "--strict"], 1),
                           (["build", str(skip), "-o", str(self.tmp / "skip.dnc"), "--strict"], 1),
                           (["build", str(good), "-o", str(out)], 0), (["info", str(out)], 0),
                           (["check", str(self.tmp / "nowhere")], 1), (["info", str(self.tmp / "none.dnc")], 1),
                           (["frobnicate"], 2), (["builtin", str(good)], 2),
                           (["builtin", "--out", str(self.tmp / "Good.h")], 2),
                           (["builtin", str(good), "--out", str(self.tmp / "bad-name.h")], 2),
                           (["builtin", str(good), "--out", str(self.tmp / "9lives.cpp")], 2),
                           (["check"], 2)):
            with self.subTest(argv=argv):
                got, stdout, stderr = run_main(argv)
                self.assertEqual(got, code, stdout + stderr)
        self.assertFalse((self.tmp / "skip.dnc").exists())
        self.assertEqual(out.read_bytes()[:4], b"MPDN")
        # C.2: -o creates its folder; an output that can't be written is an error line, exit 1
        deep = self.tmp / "card" / "dancers" / "good.dnc"
        self.assertEqual(run_main(["build", str(good), "-o", str(deep)])[0], 0)
        self.assertEqual(deep.read_bytes(), out.read_bytes())
        for target in (self.tmp / "card", good / "dancer.ini" / "x.dnc"):
            code, stdout, stderr = run_main(["build", str(good), "-o", str(target)])
            self.assertEqual((code, stderr), (1, ""), stdout)
            self.assertTrue(stdout.splitlines()[-1].startswith("%s: error: can't write it (" % str(target).replace(
                "\\", "/")), stdout)
        (self.tmp / "Taken.h").mkdir()
        code, stdout, _ = run_main(["builtin", str(good), "--out", str(self.tmp / "Taken.h")])
        self.assertEqual(code, 1)
        self.assertTrue(stdout.splitlines()[-1].startswith("%s: error: can't write it (" % str(
            self.tmp / "Taken.h").replace("\\", "/")), stdout)
        code, stdout, stderr = run_main(["build", str(broken), "-o", str(self.tmp / "broken.dnc")])
        self.assertEqual(code, 1)
        self.assertFalse((self.tmp / "broken.dnc").exists())
        self.assertIn("dancer.ini:6: error: cell must be WxH, like 64x76", stdout.splitlines())
        self.assertEqual(stderr, "")

    def test_builtin_header(self):
        d = self.dancer(ini_text(), base_png(), name="good")
        dest = self.tmp / "RoboPack.h"
        code, out, err = run_main(["builtin", str(d), "--out", str(dest)])
        self.assertEqual(code, 0, out + err)
        raw = dest.read_bytes()
        self.assertNotIn(b"\r", raw)
        text = raw.decode("utf-8")
        self.assertTrue(text.endswith("\n"))
        head = text.split("\n")[:4]
        self.assertEqual(head[0], "// SPDX-License-Identifier: MIT")
        self.assertEqual(head[1], "// Copyright (C) Ana")
        self.assertTrue(head[2].startswith("// Generated by tools/dancepack.py from "), head[2])
        self.assertTrue(head[2].endswith(". Do not edit:"), head[2])
        self.assertEqual(head[3], "// change the source and run `python tools/dancepack.py builtin`.")
        self.assertIn("#pragma once", text)
        self.assertIn("namespace dance {", text)
        data = bytes(dp.compile_dir(str(d)).data)
        self.assertRegex(text, r"alignas\(4\) static const uint8_t kRoboPack\[%d\] = \{" % len(data))
        self.assertEqual(c_bytes(text, "kRoboPack"), data)
        self.assertIn("static const size_t kRoboPackSize = %d;" % len(data), text)
        p = Mpdn(data)
        self.assertEqual(c_ints(text, "kRoboPackFrameHash"), [ref_frame_hash(g) for g in p.grids])
        self.assertIn("static const int kRoboPackFrameCount = %d;" % p.geom.frame_count, text)
        for line in c_body(text, "kRoboPack").strip().splitlines():
            self.assertLessEqual(len(re.findall(r"0x[0-9A-F]{2}\b", line)), 16, line)
        dest = self.tmp / "RoboPack.cpp"
        self.assertEqual(run_main(["builtin", str(d), "--out", str(dest)])[0], 0)
        text = dest.read_text(encoding="utf-8")
        self.assertIn("extern const uint8_t kRoboPack[", text)
        self.assertRegex(text, r"alignas\(4\) const uint8_t kRoboPack\[%d\] = \{" % len(data))
        self.assertEqual(c_bytes(text, "kRoboPack"), data)
        bad = self.dancer(ini_text(dancer_keys({"license": "Proprietary"})), base_png(), name="closed")
        self.assertEqual(run_main(["builtin", str(bad), "--out", str(self.tmp / "Closed.h")])[0], 1)

    def test_compile_dir_never_raises(self):
        # C.9: compile_dir returns a Result; a pack its own reader rejects (a
        # compiler bug) is a pack error, not an exception.
        real = dp.read_pack

        def broken(data, check_crc=True):
            raise dp.DanceError("boom")
        dp.read_pack = broken
        try:
            r = self.compile(ini_text())
        finally:
            dp.read_pack = real
        self.assertPackError(r, "error: internal: the pack doesn't read back (boom): please report it")


# ===========================================================================
# Determinism (A.1).

class DeterminismTest(DpCase):
    def test_same_bytes(self):
        ini = robo_ini()
        first = bytes(self.compile(ini, parent=self.tmp / "a").data)
        self.assertEqual(bytes(self.compile(ini, parent=self.tmp / "b" / "deeper").data), first)
        self.assertEqual(bytes(self.compile(ini, parent=self.tmp / "a").data), first)
        crlf = self.dancer(None, base_png(), raw=ini.replace("\n", "\r\n").encode(), parent=self.tmp / "c")
        self.assertEqual(bytes(dp.compile_dir(str(crlf)).data), first)
        s = base_sheet()
        forms = {
            "indexed4": s.indexed_png(4),
            "indexed8": s.indexed_png(8),
            "rgba16": make_png([[tuple(c * 257 for c in px) for px in row] for row in s.rgba()], 6, 16),
            "filtered": s.png(filters=(0, 1, 2, 3, 4), idat_parts=5, extra=(("tIME", bytes(7)),)),
        }
        for label, png in forms.items():
            self.assertEqual(bytes(self.compile(ini, png, parent=self.tmp / label).data), first, label)
        keyed = ini_text(ROBO_DANCER + ("key = #ff00ff",), ROBO_MOVES)
        self.assertEqual(bytes(self.compile(keyed, s.keyed_png(), parent=self.tmp / "keyed").data), first)
        trailing = self.dancer(None, base_png(), raw=ini.encode(), parent=self.tmp / "d")
        self.assertEqual(bytes(dp.compile_dir(str(trailing) + os.sep).data), first)
        other = Mpdn(self.compile(ini, name="robo2").data)
        self.assertEqual(other.id, "robo2")

    def test_messages_repeat(self):
        ini = robo_ini()
        a = self.compile(ini, parent=self.tmp / "a")
        b = self.compile(ini, parent=self.tmp / "b")
        self.assertEqual(list(a.messages), list(b.messages))
        self.assertEqual(lines_of(a.report), lines_of(b.report))


# ===========================================================================
# The test dancer (C.10), the generated files (C.8) and CI's --check.

TEST_CELLS = (
    ("bounce", ("A1", "B1", "C1", "D1")), ("pulse", ("A1", "B1", "D1", "B1")),
    ("step-touch", ("A2", "B2", "A2", "B2")), ("point", ("A3", "B3", "C3", "B3")), ("slow", ("A4", "B4", "C4")),
    ("fast", ("A5", "B5", "C5")), ("walk", ("A6", "B6", "C6", "D6")), ("nod", ("A7", "B7", "A7")),
    ("future", ("A1", "B1")), ("big", ("A8", "A8")), ("heavy", ("A8", "A8")), ("sway", ("A9", "B9")),
    ("breathe", ("A10", "B10", "C10")), ("look", ("A11", "B11", "A11")),
)
TEST_FIELDS = {   # C.10's keys as MOVS and STEP hold them; anything unnamed is its default
    "bounce": dict(beats=2, flags=2, bounce=3, durs=[6] * 4),
    "pulse": dict(beats=1, flags=32, sway=1, durs=[3] * 4, keys=[(0, 0), (0, -1), (0, -2), (0, -1)], eases=[3] * 4),
    "step-touch": dict(beats=2, flags=1, sway=2, poses=(1, 1, 1, 1), keys=[(0, 0), (-1, 0), (0, 0), (1, 0)],
                       eases=[1, 3, 2, 0], durs=[6] * 4),
    "point": dict(beats=4, durs=[6, 6, 12, 24], keys=[(0, 0), (2, -2), (4, -3), (2, -2)], eases=[4] * 4,
                  pace=(90, 130), flags=5, weight10=3, repeat=2, poses=(1, 2, 1, 3)),
    "slow": dict(beats=4, durs=[16] * 3, pace=(36, 60), weight10=25),
    "fast": dict(beats=1, durs=[3, 6, 3], pace=(150, 180), flags=8, poses=(4, 1, 4, 1)),
    "walk": dict(beats=4, durs=[12] * 4, pace=(90, 140), flags=1, travel=1, travel_ease=1, start_at=2,
                 go=[False, True, True, False]),
    "nod": dict(beats=1, durs=[4] * 3, pace=(70, 100), genres=["hip hop", "R&B", "drum n bass"]),
    "future": dict(beats=2, durs=[12, 12], needs=1, song=(160, 180)),
    "big": dict(beats=2, durs=[12, 12], pace=(100, 160), keys=[(-4, 0), (4, 0)]),
    "heavy": dict(beats=2, durs=[12, 12], flags=16, keys=[(-12, 0), (12, 0)]),
    "sway": dict(kind=1, when=3, durs=[200, 200], keys=[(-2, 0), (2, 0)], eases=[4, 4]),
    "breathe": dict(kind=1, when=1, durs=[110] * 3),
    "look": dict(kind=1, when=2, durs=[50, 100, 150], flags=1),
}


@functools.lru_cache(maxsize=None)
def testpack_result():
    return dp.compile_dir(str(REPO / TEST_DANCER), strict=True)


@functools.lru_cache(maxsize=None)
def generated():
    return dict((p, t) for p, t in dp.generated_files())


def c_strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r'//[^\n]*', "", text)


def c_body(text, name):
    m = re.search(re.escape(name) + r"\[[^\]]*\]\s*=\s*\{(.*?)\};", c_strip_comments(text), re.S)
    assert m, name
    return m.group(1)


def c_bytes(text, name):
    return bytes(int(t, 16) for t in re.findall(r"0[xX]([0-9a-fA-F]{2})\b", c_body(text, name)))


def c_ints(text, name):
    return [int(t, 16) if t[:2] in ("0x", "0X") else int(t)
            for t in re.findall(r"0[xX][0-9a-fA-F]+|\d+", c_body(text, name))]


def c_string_bytes(lit):
    body, out, i = lit[1:-1], bytearray(), 0
    while i < len(body):
        if body[i] == "\\":
            m = re.match(r"[0-7]{1,3}", body[i + 1:])
            if m:
                out.append(int(m.group(0), 8))
                i += 1 + len(m.group(0))
            else:
                out += body[i + 1].encode("utf-8")
                i += 2
        else:
            out += body[i].encode("utf-8")
            i += 1
    return bytes(out)


def c_structs(text, name):
    """Each `{...}` of an array of structs, as a list of values."""
    out = []
    for entry in re.findall(r"\{([^{}]*)\}", c_body(text, name)):
        vals = []
        for tok in re.findall(r'"(?:[^"\\]|\\.)*"|-?0[xX][0-9a-fA-F]+[uU]?|-?\d+|kNoK|true|false', entry):
            if tok.startswith('"'):
                vals.append(c_string_bytes(tok))
            elif tok == "kNoK":
                vals.append(127)
            elif tok in ("true", "false"):
                vals.append(int(tok == "true"))
            else:
                vals.append(int(tok.rstrip("uU"), 0) if "x" in tok.lower() else int(tok))
        out.append(vals)
    return out


class TestDancerTest(DpCase):
    def setUp(self):
        super().setUp()
        self.r = testpack_result()
        self.assertTrue(self.r.ok, "\n".join(self.r.messages))
        self.p = Mpdn(self.r.data)
        self.ini = (REPO / TEST_DANCER / "dancer.ini").read_text(encoding="utf-8-sig").replace("\r\n", "\n")

    def test_header_fields(self):
        p = self.p
        self.assertEqual((p.id, p.name, p.author, p.licence), ("test", "Tëst", "IrosTheBeggar",
                                                               "GPL-3.0-or-later"))
        self.assertTrue(p.url and p.version)
        self.assertEqual(p.geom[:7], (4, 1, 24, 32, 12, 30, 35))         # an odd shadow (B.5's sw // 2)
        self.assertEqual(p.declared_moves, 14)
        self.assertTrue(all(c != (0, 0, 0) for c in p.palette[1:5]) and p.palette[5:] == [(0, 0, 0)] * 11)
        self.assertEqual(p.pose_keys, ["stand", "left", "right", "low"])
        self.assertEqual(list(self.r.skipped), [])
        self.assertNoLine(self.r, "x-note")
        hdr, chunks, _ = ref_decode_png((REPO / TEST_DANCER / "sheet.png").read_bytes())
        self.assertEqual(hdr, (96, 352, 8, 2))
        self.assertNotIn("tRNS", chunks)

    def test_moves(self):
        p = self.p
        self.assertEqual([m.name_text for m in p.moves], [n for n, _ in TEST_CELLS])
        for name, f in TEST_FIELDS.items():
            m = p.move(name)
            n = len(m.steps)
            with self.subTest(name):
                self.assertEqual(m.kind, f.get("kind", 0))
                self.assertEqual(m.beats, f.get("beats", 0))
                self.assertEqual((m.pace_lo, m.pace_hi), f.get("pace", (0, 0)))
                self.assertEqual(m.weight10, f.get("weight10", 10))
                self.assertEqual(m.when, f.get("when", 0))
                self.assertEqual(m.flags, f.get("flags", 0))
                self.assertEqual(m.repeat, f.get("repeat", 0))
                self.assertEqual(m.needs, f.get("needs", 0))
                self.assertEqual((m.song_lo, m.song_hi), f.get("song", (0, 0)))
                self.assertEqual((m.travel, m.start_at, m.travel_ease),
                                 (f.get("travel", 0), f.get("start_at", 0), f.get("travel_ease", 0)))
                self.assertEqual((m.bounce, m.sway), (f.get("bounce", 0), f.get("sway", 0)))
                self.assertEqual((m.from_key, m.to_key, m.from_key_m, m.to_key_m), f.get("poses", (0, 0, 0, 0)))
                self.assertEqual(m.genres, f.get("genres", []))
                self.assertEqual([s.dur for s in m.steps], f["durs"])
                self.assertEqual([(s.dx, s.dy) for s in m.steps], f.get("keys", [(0, 0)] * n))
                self.assertEqual([s.ease for s in m.steps], f.get("eases", [0] * n))
                self.assertEqual([s.go for s in m.steps], f.get("go", [False] * n))

    def test_cells_and_frames(self):
        p = self.p
        bounce, touch, slow = p.move("bounce"), p.move("step-touch"), p.move("slow")
        self.assertEqual((bounce.steps[0].frame, bounce.steps[0].mirrored), (0, False))      # A1 is the rest
        self.assertEqual((bounce.steps[2].frame, bounce.steps[2].mirrored), (0, False))      # C1 = A1
        self.assertEqual((touch.steps[1].frame, touch.steps[1].mirrored), (touch.steps[0].frame, True))  # B2
        self.assertEqual(p.boxes[slow.steps[1].frame], (0, 0, 0, 0))                          # B4
        self.assertEqual(p.boxes[p.move("big").steps[0].frame], (0, 0, 24, 32))              # A8
        # A.14 again, from the sheet's own pixels: mirror images about the anchor column (12).
        _, _, rows = ref_decode_png((REPO / TEST_DANCER / "sheet.png").read_bytes())
        index = {ref_rgb565(*c): i for i, c in enumerate(p.palette) if 0 < i < 5}

        def grid(cell):
            c, r = cell_xy(cell)
            return [bytes(0 if px == (255, 0, 255) else index[ref_rgb565(*px)]
                          for px in rows[r * 32 + y][c * 24:(c + 1) * 24]) for y in range(32)]

        frames, refs = [], []
        ref_store(frames, grid("A1"), 12)
        for _, cells in TEST_CELLS:
            refs.extend(ref_store(frames, grid(c), 12) for c in cells)
        self.assertEqual(p.grids, frames)
        self.assertEqual([(s.frame, s.mirrored) for s in p.steps], refs)
        self.assertEqual(p.thumb, ref_thumbnail(p.grids[0], p.boxes[0]))
        # C.10's note: B2 is the only cell stored as a mirrored reference, and merging
        # never changes what is drawn (B.4 mirrors about the anchor too).
        cells = [c for _, cs in TEST_CELLS for c in cs]
        self.assertEqual([c for c, s in zip(cells, p.steps) if s.mirrored], ["B2", "B2"])
        for c, s in zip(cells, p.steps):
            self.assertEqual(drawn_pixels(p.grids[s.frame], 12, s.mirrored), drawn_pixels(grid(c), 12, False), c)

    def test_budgets_and_warnings(self):
        r, p = self.r, self.p
        lights = {name: b.light for name, b in r.budgets.items()}
        self.assertEqual(lights, dict((n, "amber" if n == "big" else "red" if n == "heavy" else "green")
                                      for n, _ in TEST_CELLS))
        for m in p.moves:
            cases, med, peak, light = ref_budget(p.geom, p.boxes, m)
            b = r.budgets[m.name_text]
            self.assertEqual((b.median, b.peak, b.light), (med, peak, light), m.name_text)
            self.assertEqual([(c.fps, c.pace, bool(c.mirrored), c.spot, c.frames, c.median, c.peak)
                              for c in b.cases], cases, m.name_text)
        line = functools.partial(ini_line, self.ini)
        heavy_peak = r.budgets["heavy"].peak
        for want in (
                'dancer.ini:%d: warning: move "pulse": timing 0.26 snapped to 1/4 beat' % line("[move pulse]", "timing"),
                'dancer.ini:%d: warning: move "future": song_bpm needs track BPM data: skipped in this version'
                % line("[move future]", "song_bpm"),
                'dancer.ini:%d: warning: move "heavy": budget red (peak %s px), kept because heavy = yes'
                % (line("[move heavy]", "heavy"), "{:,}".format(heavy_peak)),
                'dancer.ini:%d: note: move "nod": genre tags keep it dormant until tracks carry genres'
                % line("[move nod]", "genre"),
                'dancer.ini:%d: warning: move "big": the first key is -4,0, not 0,0: the dancer jumps when another '
                'move follows' % line("[move big]", "offset"),
                'dancer.ini:%d: warning: move "heavy": the first key is -12,0, not 0,0: the dancer jumps when '
                'another move follows' % line("[move heavy]", "offset")):
            self.assertLine(r, want)
        rep = lines_of(r.report)
        i = rep.index("edges:")
        self.assertIn("  heavy: clamped at both edge spots", rep[i + 1:])
        self.assertIn("  walk: travels from -1; mirrored from +1", rep[i + 1:])
        self.assertNotIn("big", " ".join(l.split(":")[0] for l in rep[i + 1:] if l.startswith("  ")))

    def test_vector_boundaries(self):
        # C.10: breathe's 3.3 s ends exactly on a 30 fps frame (pos16(100) =
        # 330*65536), so kBudgetCases pins B.6's N >= rule; pulse is a 1-beat
        # move with sway (B.3's b since the run started).
        cases = {(c.fps, c.pace): c for c in self.r.budgets["breathe"].cases}
        self.assertEqual(cases[(30, 0)].frames, 100)
        self.assertEqual(100 * 33 * 65536 // 10, 330 * 65536)
        self.assertEqual((self.p.move("pulse").beats, self.p.move("pulse").sway), (1, 1))
        self.assertEqual(self.p.geom.shadow_w % 2, 1)

    def test_every_warning_and_note(self):
        # C.10 and its note: the expected warnings and notes, and no others (the art keeps them away).
        line = functools.partial(ini_line, self.ini)
        jumps = "not 0,0: the dancer jumps when another move follows"
        hold = "no step starts on it; fine for a hold, otherwise set timing = so one does"
        want = [
            'dancer.ini:%d: warning: move "pulse": timing 0.26 snapped to 1/4 beat' % line("[move pulse]", "timing"),
            'dancer.ini:%d: warning: move "point": beat 4 lands inside step 4 (B3): %s'
            % (line("[move point]", "timing"), hold),
            'dancer.ini:%d: warning: move "slow": beat 2 lands inside step 1 (A4): %s' % (line("[move slow]"), hold),
            'dancer.ini:%d: warning: move "slow": beat 3 lands inside step 2 (B4): %s' % (line("[move slow]"), hold),
            'dancer.ini:%d: warning: move "slow": beat 4 lands inside step 3 (C4): %s' % (line("[move slow]"), hold),
            'dancer.ini:%d: note: move "slow": step 2 (B4) is blank: the dancer disappears for 1 1/3 beats'
            % line("[move slow]", "row"),
            'dancer.ini:%d: warning: move "sway": the feet drift 4 native px across its standing frames'
            % line("[move sway]"),
            'dancer.ini:%d: note: move "nod": genre tags keep it dormant until tracks carry genres'
            % line("[move nod]", "genre"),
            'dancer.ini:%d: warning: move "future": song_bpm needs track BPM data: skipped in this version'
            % line("[move future]", "song_bpm"),
            'dancer.ini:%d: warning: move "big": the first key is -4,0, %s' % (line("[move big]", "offset"), jumps),
            'dancer.ini:%d: warning: move "heavy": the first key is -12,0, %s' % (line("[move heavy]", "offset"), jumps),
            'dancer.ini:%d: warning: move "heavy": budget red (peak %s px), kept because heavy = yes'
            % (line("[move heavy]", "heavy"), "{:,}".format(self.r.budgets["heavy"].peak)),
        ]
        got = [m for m in self.r.messages if re.match(r"^(dancer\.ini:\d+: |sheet\.png: )?(warning|note): ", m)]
        self.assertEqual(sorted(got), sorted(want))
        # milliseconds print plain (C.5), px with commas
        self.assertTrue(any(m.startswith('dancer.ini:%d: move "sway": no BPM, when any, 4000 ms; green: median '
                                         % line("[move sway]")) for m in self.r.messages), self.r.messages)

    def test_report_is_the_docs_example(self):
        # C.5 shows a few of the test dancer's coverage rows, its idles line and its edges.
        rep = lines_of(self.r.report)
        example = [
            "coverage: pace at dance tempo T (h = half time, d = double time, . = not eligible)",
            "  T              72   80   90  100  110  120  130  140  150  160  176",
            "  bounce         72   80   90  100  110  120  130  140  150  160  176",
            "  slow          36h  40h  45h  50h  55h  60h    .    .    .    .    .",
            "  nod*           72   80   90  100    .    .    .  70h  75h  80h  88h",
            "  * genre-tagged: dormant until tracks carry genres",
            "idles: quiet: sway, breathe; listening: sway, look",
        ]
        for l in example:
            self.assertIn(l, rep)
        i = rep.index("edges:")
        self.assertEqual(rep[i + 1:i + 3], ["  walk: travels from -1; mirrored from +1",      # MOVS order
                                            "  heavy: clamped at both edge spots"])


class GeneratedFilesTest(DpCase):
    def test_paths_and_headers(self):
        files = generated()
        for path in (TESTPACK_H,) + VECTOR_HEADERS:
            self.assertIn(path, files)
        for path, text in files.items():
            with self.subTest(path):
                self.assertNotIn("\r", text)
                self.assertTrue(text.endswith("\n"))
                head = text.split("\n")[:4]
                self.assertTrue(head[0].startswith("// SPDX-License-Identifier: "), head[0])
                self.assertTrue(head[1].startswith("// Copyright (C) "), head[1])
                self.assertTrue(head[2].startswith("// Generated by tools/dancepack.py from "), head[2])
                self.assertTrue(head[2].endswith(". Do not edit:"), head[2])
                self.assertEqual(head[3], "// change the source and run `python tools/dancepack.py builtin`.")
                if path.endswith(".h"):
                    self.assertIn("#pragma once", text)
                self.assertEqual(text, (REPO / path).read_text(encoding="utf-8"),
                                 "%s is stale: run python tools/dancepack.py builtin" % path)
        for path in VECTOR_HEADERS:
            head = files[path].split("\n")[:3]
            self.assertEqual(head[0], "// SPDX-License-Identifier: GPL-3.0-or-later")
            self.assertEqual(head[1], "// Copyright (C) 2026 IrosTheBeggar")
            source = "tools/art/dancers/test" if "Budget" in path else "its own tables"
            self.assertEqual(head[2], "// Generated by tools/dancepack.py from %s. Do not edit:" % source)
            self.assertIn("namespace dancevec", files[path])
        head = files[TESTPACK_H].split("\n")[:3]
        self.assertEqual(head, ["// SPDX-License-Identifier: GPL-3.0-or-later", "// Copyright (C) IrosTheBeggar",
                                "// Generated by tools/dancepack.py from tools/art/dancers/test. Do not edit:"])

    def test_testpack(self):
        text = generated()[TESTPACK_H]
        data = bytes(testpack_result().data)
        self.assertEqual(c_bytes(text, "kTestPack"), data)
        self.assertRegex(text, r"alignas\(4\) static const uint8_t kTestPack\[%d\] = \{" % len(data))
        self.assertIn("static const size_t kTestPackSize = %d;" % len(data), text)
        p = Mpdn(data)
        self.assertEqual(c_ints(text, "kTestPackFrameHash"), [ref_frame_hash(g) for g in p.grids])
        self.assertIn("static const int kTestPackFrameCount = %d;" % p.geom.frame_count, text)

    def test_eligibility_vectors(self):
        text = generated()["test/support/dance/EligibilityVectors.h"]
        self.assertRegex(text, r"constexpr int8_t kNoK = 127;")
        want = []
        for t10, lo, hi, prev, beats, travel in elig_grid():
            k = ref_eligible_k(t10, lo, hi, prev, beats, travel)
            want.append([t10, lo, hi, 127 if prev is None else prev, beats, travel, 127 if k is None else k])
        self.assertEqual(len(want), 3696)
        got = c_structs(text, "kEligibility")
        self.assertEqual(len(got), len(want))
        self.assertTrue(got == want, next((g, w) for g, w in zip(got, want) if g != w) if got != want else "")
        self.assertIn("static const int kEligibilityCount = 3696;", text)    # C.8: <array>Count
        want = [[t10, k, lo, hi, int(ref_pace_holds(t10, k, lo, hi))]
                for t10 in PACE_T10 for k in (-1, 0, 1) for lo, hi in PACE_RANGES]
        self.assertEqual(c_structs(text, "kPaceHolds"), want)
        self.assertIn("static const int kPaceHoldsCount = %d;" % len(want), text)

    def test_fold_vectors(self):
        text = generated()["test/support/dance/FoldVectors.h"]
        got = c_structs(text, "kFold")
        self.assertEqual(got, [[tag.encode("utf-8"), key.encode("ascii"), h or 0] for tag, key, h in FOLD_TABLE])
        self.assertRegex(text, r'"\\303\\211lectro"')            # bytes >= 0x80 as octal escapes
        self.assertIn("static const int kFoldCount = %d;" % len(FOLD_TABLE), text)

    def test_budget_vectors(self):
        text = generated()["test/support/dance/BudgetVectors.h"]
        p = Mpdn(testpack_result().data)
        cases, moves, traces, frames = [], [], [], []
        for i, m in enumerate(p.moves):
            cs, med, peak, light = ref_budget(p.geom, p.boxes, m)
            cases += [[i, fps, pace, int(mir), spot, n, cmed, cpeak] for fps, pace, mir, spot, n, cmed, cpeak in cs]
            moves.append([i, med, peak, {"green": 0, "amber": 1, "red": 2}[light]])
            pace = 0 if m.kind else (lambda r: (r[0] + r[1]) // 2)(ref_reach(m.pace_lo, m.pace_hi, m.beats, m.travel))
            first = next(c for c in cs if c[0] == 30 and c[1] == pace)
            tr = ref_trace(p.geom, p.boxes, m, 30, pace, first[2], first[3])
            traces.append([i, 30, pace, int(first[2]), first[3], len(frames), len(tr)])
            for n, pos, pose, d in tr:
                rect = pose[6] or (0, 0, 0, 0)
                frames.append([n, pos, pose[0] | (0x8000 if pose[1] else 0), pose[2], pose[3]] + list(rect) + [d or 0])
        self.assertEqual(c_structs(text, "kBudgetCases"), cases)
        self.assertEqual(c_structs(text, "kMoveBudgets"), moves)
        self.assertEqual(c_structs(text, "kTraceCases"), traces)
        self.assertEqual(c_structs(text, "kTrace"), frames)
        # C.8's note: each array's count is its name plus Count
        for name, n in (("kBudgetCases", len(cases)), ("kMoveBudgets", len(moves)), ("kTraceCases", len(traces)),
                        ("kTrace", len(frames))):
            self.assertIn("static const int %sCount = %d;" % (name, n), text)
        self.assertEqual(len(moves), 14)     # every MOVS record, future (needs bit) included


class CliTest(DpCase):
    """The command line as a process: cwd, stdout, exit status."""

    def test_build_and_check(self):
        self.dancer(A17_INI, a17_sheet().png(), name="dot")
        code, out, err = run_cli(["build", "dot"], self.tmp)
        self.assertEqual((code, err), (0, ""), out)
        lines = out.splitlines()
        self.assertEqual(lines[-2:], ["dot: 1 move (0 of 1 skipped), 1 frame, 1,156 bytes", "wrote dot.dnc"])
        self.assertIn('dancer.ini:7: move "hop": 1 beat, pace any; green: median 4 px, peak 6 of 16,000 px', lines)
        self.assertNotIn("coverage:", out)
        self.assertEqual((self.tmp / "dot.dnc").read_bytes(), a17_pack())
        (self.tmp / "out").mkdir()
        code, out, err = run_cli(["build", "./dot/", "-o", os.path.join("out", "x.dnc")], self.tmp)
        self.assertEqual(code, 0, out + err)
        self.assertEqual(out.splitlines()[-1], "wrote out/x.dnc")
        self.assertEqual((self.tmp / "out" / "x.dnc").read_bytes(), a17_pack())
        before = sorted(p.name for p in self.tmp.rglob("*"))
        code, out, err = run_cli(["check", "dot"], self.tmp)
        self.assertEqual((code, err), (0, ""))
        self.assertEqual(sorted(p.name for p in self.tmp.rglob("*")), before)     # check writes nothing (C.2)
        self.assertIn("coverage: pace at dance tempo T (h = half time, d = double time, . = not eligible)",
                      out.splitlines())
        self.assertEqual(out.splitlines()[-1], "dot: 1 move (0 of 1 skipped), 1 frame, 1,156 bytes")
        code, out, err = run_cli(["info", "dot.dnc"], self.tmp)
        self.assertEqual(code, 0)
        self.assertEqual(out.splitlines()[0], "dot.dnc: MPDN v1 (reader 1), 1,156 bytes, CRC ok")
        code, out, err = run_cli(["build", "nowhere"], self.tmp)
        self.assertEqual(code, 1)
        code, out, err = run_cli(["check"], self.tmp)
        self.assertEqual((code, out), (2, ""))
        self.assertIn("usage", err.lower())

    def test_repository_check(self):
        code, out, err = run_cli(["--check"], REPO)
        self.assertEqual((code, out, err), (0, "", ""))

    def test_outside_the_repository(self):
        # C.2: dancepack.py downloaded alone checks and builds dancers; --check and
        # builtin (no DIR) say they need the repository, a usage error (exit 2).
        script = self.tmp / "alone" / "tools" / "dancepack.py"
        script.parent.mkdir(parents=True)
        shutil.copyfile(str(DANCEPACK), str(script))
        for args in (["--check"], ["builtin"]):
            code, out, err = run_cli(args, self.tmp, script)
            self.assertEqual((code, out), (2, ""), args)
            self.assertIn("--check and builtin work only in the mstream-mp3-player repository; to check a dancer "
                          "use: dancepack.py check DIR", err)
        self.dancer(A17_INI, a17_sheet().png(), name="dot")
        self.assertEqual(run_cli(["check", "dot"], self.tmp, script)[0], 0)
        code, out, err = run_cli(["builtin", "dot", "--out", "DotPack.h"], self.tmp, script)
        self.assertEqual(code, 0, out + err)

    def repo_copy(self):
        """What --check reads (C.2), copied with dancepack.py into a temporary tree."""
        root = self.tmp / "repo"
        paths = [TEXTFOLD_CPP, VLWFONTS_CPP] + list(VECTOR_HEADERS)
        for src, out in dp.BUILTINS:
            shutil.copytree(str(REPO / src), str(root / src))
            paths.append(out)
        for rel in paths:
            (root / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(str(REPO / rel), str(root / rel))
        script = root / "tools" / "dancepack.py"
        script.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(str(DANCEPACK), str(script))
        return root, script

    def test_check_compares_the_sheet_pixels(self):
        # C.1's note: builtin renders sheet.txt to sheet.png, and --check compares pixels, not bytes.
        root, script = self.repo_copy()
        png = root / TEST_DANCER / "sheet.png"
        hdr, _, rows = ref_decode_png(png.read_bytes())
        self.assertEqual(hdr, (96, 352, 8, 2))
        png.write_bytes(make_png([[p + (255,) for p in row] for row in rows], 6, 8, filters=(4,)))
        self.assertEqual(run_cli(["--check"], root, script)[:2], (0, ""))
        rows[10 * 32 + 5][3 * 24 + 5] = (1, 2, 3)      # in D11, a cell no move uses: the pack doesn't change
        png.write_bytes(make_png(rows, 2, 8))
        code, out, err = run_cli(["--check"], root, script)
        self.assertEqual((code, out.splitlines()),
                         (1, ["stale: %s/sheet.png (run python tools/dancepack.py builtin)" % TEST_DANCER]), err)
        code, out, err = run_cli(["builtin"], root, script)
        self.assertEqual((code, out.splitlines()), (0, ["wrote %s/sheet.png" % TEST_DANCER]), err)
        self.assertEqual(run_cli(["--check"], root, script)[:2], (0, ""))
        self.assertEqual(ref_decode_png(png.read_bytes())[0], (96, 352, 8, 2))

    def test_check_in_a_copy(self):
        root, script = self.repo_copy()
        self.assertEqual(run_cli(["--check"], root, script)[:2], (0, ""))
        fold = root / "test/support/dance/FoldVectors.h"
        fold.write_bytes(fold.read_bytes() + b"// edited\n")
        elig = root / "test/support/dance/EligibilityVectors.h"
        elig.unlink()
        code, out, err = run_cli(["--check"], root, script)
        self.assertEqual(code, 1)
        hint = " (run python tools/dancepack.py builtin)"
        self.assertEqual(sorted(out.splitlines()), sorted(["stale: test/support/dance/EligibilityVectors.h" + hint,
                                                           "stale: test/support/dance/FoldVectors.h" + hint]))
        code, out, err = run_cli(["builtin"], root, script)
        self.assertEqual(code, 0, out + err)
        self.assertEqual(sorted(out.splitlines()), ["wrote test/support/dance/EligibilityVectors.h",
                                                    "wrote test/support/dance/FoldVectors.h"])
        self.assertNotIn(b"\r", elig.read_bytes())
        self.assertEqual(elig.read_text(encoding="utf-8"), generated()["test/support/dance/EligibilityVectors.h"])
        # A CRLF checkout (core.autocrlf) isn't stale; the embedded tables are checked against their sources.
        pack = root / TESTPACK_H
        pack.write_bytes(pack.read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
        tf = root / TEXTFOLD_CPP
        text = tf.read_text(encoding="utf-8")
        # the first `case` group with a non-empty replacement (one label or several); --check
        # names the lowest code point that differs, the group's smallest label
        m = re.search(r'((?:case 0x[0-9A-Fa-f]+:\s*)+)return "([^"\\]+)";', text)
        labels = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]+)", m.group(1))]
        write_lf(tf, text[:m.start(2)] + "Q" + text[m.end(2):])
        cov = sorted(ref_font_coverage() - {0x41})
        write_lf(root / VLWFONTS_CPP, vlw_array("kVlwSans16", cov) + vlw_array("kVlwSans13", cov))
        code, out, err = run_cli(["--check"], root, script)
        self.assertEqual(code, 1)
        self.assertEqual(sorted(out.splitlines()), sorted([
            "stale: tools/dancepack.py (TEXTFOLD differs from lib/core/TextFold.cpp at U+%04X)" % min(labels),
            "stale: tools/dancepack.py (FONT_COVERAGE differs from src/ui/VlwFonts.cpp at U+0041)"]))


if __name__ == "__main__":
    unittest.main()
