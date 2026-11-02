#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""The waveform index, waves-<gen>.bin (MPWF v1, docs/WAVEFORM-INDEX.md): the
reference writer and reader, and the maker of its test vectors.

Every rule a second writer must reproduce byte for byte is here in its
plainest form, with the spec section it implements. card_fixtures.py
imports it for every MPWF fixture in test/fixtures/card, and test_mpwf.py
tests it. Standard library only.

    python tools/mpwf.py build TRACKS.json OUT.bin [--generation G] [--card-id X]
    python tools/mpwf.py check FILE.bin [--uses all|device]   exit 1 when absent; prints the reason
    python tools/mpwf.py dump FILE.bin [--slot R] [--hex] [--path REL | --hash H]
    python tools/mpwf.py vectors                              vectors.json's "mpwf" group
    python tools/mpwf.py size ROWS

`build` takes a plain map, {"<path>": {"lengthMs": 180000, "peaks": [...]}},
with --generation (default 1) and --card-id (default 0), or a description.

The description (TRACKS.json; WAVEFORM-INDEX.md 13.2) is the writer's whole
input, every value a writer would otherwise choose given:

    {"format": "MPWF", "file": "<golden name>.bin",
     "major": 1, "minor": 0,                          optional, these defaults
     "generation": 42, "cardId": "0x5EEDC0DE0A1B2C3D",
     "note": "...",
     "tracks": [{"path": "<relative to /music, as the card stores it>",
                 "hash": "0x...",                     instead of path: the hash itself
                 "lengthMs": 180000,                  the MPTG record's durationMs; 0 unknown
                 "peaks": [0, 255, ...],              the server's M peak bars, M >= 1, each 0-255
                 "note": "..."}]}

Tracks may be listed in any order. A hash two tracks share is left out.
"""
import argparse
import json
import math
import struct
import sys
import zlib
from collections import namedtuple
from pathlib import Path

# ---------------------------------------------------------------------------
# Constants (WAVEFORM-INDEX.md 3-4, 6)
# ---------------------------------------------------------------------------
MAGIC = b"MPWF"
MAJOR = 1
HEADER_BYTES = 224           # the whole header: its one directory entry ends at 256
REQUIRED = 1                 # bit 0 of a section's flags; readers ignore the other bits
WVSL = b"WVSL"
SLOT_BYTES = 256
BAR_COUNT = 240
FIRST_SLOT = 256             # WVSL's offset, so a slot never crosses a 512-byte sector
MAX_ROWS = 250000            # 64,000,256 bytes, under the transfer's 64 MiB file cap
INTERPOLATED = 4             # a lookup's first probes interpolate; the rest bisect
MAX_READS = 24               # a lookup's guard; a valid file takes at most 22
MAX_SECTIONS = 64
M64 = (1 << 64) - 1
# The type header: slotBytes, barCount (40-47), then zeros up to headerBytes.
TYPE_HEADER = struct.Struct("<II")
COMMON = struct.Struct("<4sHHIIIIQII")
DIR_ENTRY = struct.Struct("<4sIIIIIII")
# A slot: pathHash, lengthMs, bars, crc.
SLOT = struct.Struct("<QI%dsI" % BAR_COUNT)
assert SLOT.size == SLOT_BYTES
assert HEADER_BYTES + DIR_ENTRY.size == FIRST_SLOT


class WriterError(ValueError):
    """A description no writer may turn into a file (the writer refuses)."""


# ---------------------------------------------------------------------------
# Conventions (2)
# ---------------------------------------------------------------------------
def crc32(data, value=0):
    """CRC-32 (ISO-HDLC, zlib's): check value 0xCBF43926."""
    return zlib.crc32(bytes(data), value) & 0xFFFFFFFF


FNV_BASIS = 0xCBF29CE484222325
FNV_PRIME = 0x00000100000001B3


def fnv1a64(data, h=FNV_BASIS):
    for b in bytes(data):
        h = ((h ^ b) * FNV_PRIME) & M64
    return h


def path_hash(rel):
    """METADATA 2.3.3: FNV-1a 64 of "/music" + "/" + the path relative to it,
    as the card stores it (UTF-8)."""
    return fnv1a64(b"/music" + (b"/" + rel.encode("utf-8") if rel else b""))


def u64(v):
    return int(v, 0) if isinstance(v, str) else int(v)


def file_name(generation):
    """7.1: the name, the digits the entry's generation in lowercase hex."""
    return "waves-%08x.bin" % generation


def size(rows):
    """10: a file's exact length."""
    return FIRST_SLOT + SLOT_BYTES * rows


def lists_major(value, major):
    """7.4: a read.* value, a comma-separated list, lists `major`. Spaces in an
    item are ignored, and an item that isn't a number is skipped."""
    for item in value.split(","):
        item = item.replace(" ", "")
        if item.isascii() and item.isdigit() and int(item) == major:
            return True
    return False


def writes_waves(device_txt):
    """7.4: whether a writer writes MPWF for a card whose /.player/device.txt
    holds `device_txt` (None: no file): only when its read.mpwf lists 1. A text
    with no contract line isn't a device.txt, so reads as no file. No file, or
    no read.mpwf key, reads as none; a key given twice reads as its last."""
    if device_txt is None:
        return False
    contract, value = False, ""
    for line in device_txt.split("\n"):
        key, eq, rest = (line[:-1] if line.endswith("\r") else line).partition("=")
        if eq and key == "contract":
            contract = True
        elif eq and key == "read.mpwf":
            value = rest
    return contract and lists_major(value, MAJOR)


# ---------------------------------------------------------------------------
# The bars (5)
# ---------------------------------------------------------------------------
def bar_range(i, m):
    """Bar i's range [start, end) of M peaks."""
    start = i * m // BAR_COUNT
    return start, min(m, max(start + 1, (i + 1) * m // BAR_COUNT))


def bars_from_peaks(peaks):
    """The slot's 240 bars from the server's M peak bars (any M >= 1, each
    0-255): each bar the root mean square of its range, in f64, rounded half
    up. Linear, no stretch: a reader stretches for its own column count."""
    m = len(peaks)
    if m < 1:
        raise ValueError("no peaks")
    out = bytearray(BAR_COUNT)
    for i in range(BAR_COUNT):
        start, end = bar_range(i, m)
        s = 0
        for b in peaks[start:end]:
            s += b * b
        out[i] = math.floor(math.sqrt(s / (end - start)) + 0.5)
    return bytes(out)


# ---------------------------------------------------------------------------
# A slot (4.3)
# ---------------------------------------------------------------------------
Slot = namedtuple("Slot", "path_hash length_ms bars")


def slot_crc(slot, row):
    """CRC-32 over the 4 bytes of the row as u32 LE, then the slot's bytes
    before its CRC."""
    return crc32(bytes(slot[:SLOT_BYTES - 4]), crc32(struct.pack("<I", row)))


def encode_slot(row, h, length_ms, bars):
    buf = bytearray(SLOT.pack(h, length_ms, bars, 0))
    struct.pack_into("<I", buf, SLOT_BYTES - 4, slot_crc(buf, row))
    return bytes(buf)


# ---------------------------------------------------------------------------
# The lookup (6)
# ---------------------------------------------------------------------------
def probe_row(h, lo, hi, lo_h, hi_h, probes):
    """The row a lookup reads next. Its first four probes interpolate on the
    top 32 bits of (h - loH) / (hiH - loH), rounded to the nearest row over
    lo..hi; a span under 2^32 and every later probe bisect. hi - lo is below
    2^18 and the top 32 bits below 2^32, so 64 bits hold the product."""
    den = (hi_h - lo_h) >> 32
    if probes >= INTERPOLATED or den == 0:
        return lo + (hi - lo) // 2
    return lo + ((hi - lo) * ((h - lo_h) >> 32) + den // 2) // den


OK, HIT, MISS, DAMAGED, BROKEN = "ok", "hit", "miss", "damaged", "broken"
# A lookup's answer: its read, the row it stopped on (hit, damaged, broken),
# the slot (hit), the rows it read in order.
Found = namedtuple("Found", "read row slot probes")


# ---------------------------------------------------------------------------
# A description, ready to write (4.3)
# ---------------------------------------------------------------------------
class Prepared:
    """A file's values: the frame's and the rows in row order."""


def _track_hash(t):
    if ("path" in t) == ("hash" in t):
        raise WriterError("a track has a path or a hash: %r" % (t.get("path", t.get("hash"))))
    if "path" in t:
        if not isinstance(t["path"], str) or not t["path"]:
            raise WriterError("a track's path isn't a non-empty string")
        return path_hash(t["path"])
    h = u64(t["hash"])
    if not 0 <= h <= M64:
        raise WriterError("a hash out of range: %r" % t["hash"])
    return h


def _track_bars(t):
    peaks = t.get("peaks")
    if not isinstance(peaks, list) or not peaks:
        raise WriterError("a track with no peaks has no slot: %r" % (t.get("path", t.get("hash"))))
    if not all(isinstance(b, int) and not isinstance(b, bool) and 0 <= b <= 255 for b in peaks):
        raise WriterError("peaks are integers 0-255: %r" % (t.get("path", t.get("hash"))))
    return bars_from_peaks(peaks)


def prepare(desc):
    """Validates a description (the writer's refusals) and derives the rows:
    (pathHash, lengthMs, bars), strictly increasing by hash; a hash two
    tracks share is left out."""
    if desc.get("format", "MPWF") != "MPWF":
        raise WriterError("not an MPWF description")
    if int(desc.get("major", MAJOR)) != MAJOR:
        raise WriterError("this writer writes major %d" % MAJOR)
    p = Prepared()
    p.minor = int(desc.get("minor", 0))
    p.generation = int(desc["generation"])
    p.card_id = u64(desc.get("cardId", 0))
    if not (0 <= p.minor <= 0xFFFF and 0 <= p.generation <= 0xFFFFFFFF and 0 <= p.card_id <= M64):
        raise WriterError("a header value out of range")
    by_hash, paths = {}, set()
    for t in desc["tracks"]:
        h = _track_hash(t)
        if "path" in t:
            if t["path"] in paths:
                raise WriterError("the path %r twice" % t["path"])
            paths.add(t["path"])
        ms = t.get("lengthMs", 0)
        if not isinstance(ms, int) or isinstance(ms, bool) or not 0 <= ms <= 0xFFFFFFFF:
            raise WriterError("lengthMs out of range: %r" % (ms,))
        by_hash.setdefault(h, []).append((ms, _track_bars(t)))
    p.rows = sorted((h, v[0][0], v[0][1]) for h, v in by_hash.items() if len(v) == 1)
    if not 1 <= len(p.rows) <= MAX_ROWS:
        raise WriterError("rows must be 1-%d (no row: no file)" % MAX_ROWS)
    return p


# ---------------------------------------------------------------------------
# The container (3) and the file (4)
# ---------------------------------------------------------------------------
def frame(generation, card_id, type_header, sections, header_bytes, major=MAJOR, minor=0, magic=MAGIC):
    """3: sections [(type, flags, count, stride, bytes)] in directory order;
    the first at headerBytes + 32 x count, each next at the previous end
    rounded up to 8 (zeros between); the header CRC last. The type header
    is written from byte 40 and zero-padded to headerBytes (cut there when
    it is longer: a hardening file's short header)."""
    n = len(sections)
    at = header_bytes + 32 * n
    offsets = []
    for s in sections:
        at = (at + 7) & ~7
        offsets.append(at)
        at += len(s[4])
    out = bytearray(at)
    for s, o in zip(sections, offsets):
        out[o:o + len(s[4])] = s[4]
    for i, (s, o) in enumerate(zip(sections, offsets)):
        DIR_ENTRY.pack_into(out, header_bytes + 32 * i, s[0], s[1], o, len(s[4]), s[2], s[3], crc32(s[4]), 0)
    COMMON.pack_into(out, 0, magic, major, minor, header_bytes, n, at, generation, card_id, 0, 0)
    th = bytes(type_header)[:max(0, header_bytes - 40)]
    out[40:40 + len(th)] = th
    struct.pack_into("<I", out, 32, crc32(out[:header_bytes + 32 * n]))
    return bytes(out)


def wvsl_bytes(rows):
    """WVSL's whole: rows [(pathHash, lengthMs, bars)] in row order, each CRC
    seeded with its place."""
    return b"".join(encode_slot(r, h, ms, bars) for r, (h, ms, bars) in enumerate(rows))


def write(rows, generation, card_id=0, minor=0):
    """The file around rows [(pathHash, lengthMs, bars)] in row order."""
    return frame(generation, card_id, TYPE_HEADER.pack(SLOT_BYTES, BAR_COUNT),
                 [(WVSL, REQUIRED, len(rows), SLOT_BYTES, wvsl_bytes(rows))], HEADER_BYTES, minor=minor)


def build(desc):
    """A file from a description; the writer's own check of every slot before
    it hands the bytes over."""
    p = prepare(desc)
    data = write(p.rows, p.generation, p.card_id, p.minor)
    why = check(data, "all")
    if why != "ok":
        raise AssertionError("the writer's own file reads as absent: " + why)
    return data


# ---------------------------------------------------------------------------
# The reader (8)
# ---------------------------------------------------------------------------
class Section:
    def __init__(self, index, type_, flags, offset, nbytes, count, stride, crc):
        self.index, self.type, self.flags, self.offset = index, type_, flags, offset
        self.bytes, self.count, self.stride, self.crc = nbytes, count, stride, crc


class Reader:
    """Opens a file (8.1, 8.2): `why` is "ok" or the first check it fails.
    check() goes on (8.4); find() looks a hash up as the device does (6), and
    `reads` counts the slots read so far."""

    def __init__(self, data):
        self.b = bytes(data)
        self.section = None
        self.rows = 0
        self.reads = 0
        self.why = self._open()

    def _open(self):
        b, size_ = self.b, len(self.b)
        if size_ < 40:
            return "short"
        (magic, self.major, self.minor, self.header_bytes, self.section_count, self.file_bytes, self.generation,
         self.card_id, self.header_crc, _) = COMMON.unpack_from(b, 0)
        if magic != MAGIC:
            return "magic"
        if self.major != MAJOR:
            return "major"
        if self.header_bytes % 8 or self.header_bytes < HEADER_BYTES:
            return "headerBytes"
        if self.section_count > MAX_SECTIONS:
            return "sectionCount"
        dir_end = self.header_bytes + 32 * self.section_count
        if dir_end > size_:
            return "short"
        if self.file_bytes != size_:
            return "fileBytes"
        if crc32(b[:32] + bytes(4) + b[36:dir_end]) != self.header_crc:
            return "headerCrc"
        end, types = dir_end, []
        for i in range(self.section_count):
            t, flags, off, nb, count, stride, c, _ = DIR_ENTRY.unpack_from(b, self.header_bytes + 32 * i)
            if off % 8 or off < end or off - end >= 8 or off + nb > self.file_bytes or t in types:
                return "layout"
            types.append(t)
            end = off + nb
            if t != WVSL:
                if flags & REQUIRED:
                    return "unknownRequired"
                continue
            if stride < SLOT_BYTES or count * stride != nb:
                return "shape"
            self.section = Section(i, t, flags, off, nb, count, stride, c)
        if end != self.file_bytes:
            return "layout"
        if self.section is None:
            return "missing"
        # The header and WVSL (8.2).
        self.slot_bytes, self.bar_count = TYPE_HEADER.unpack_from(b, 40)
        if self.slot_bytes != SLOT_BYTES or self.bar_count != BAR_COUNT:
            return "enum"
        s = self.section
        if s.stride != SLOT_BYTES:
            return "shape"
        if not 1 <= s.count <= MAX_ROWS:
            return "enum"
        if s.offset != FIRST_SLOT:
            return "wvLayout"
        self.rows = s.count
        return "ok"

    def check(self, uses="all"):
        """8.4's order: "all" is every slot in row order (its CRC, then the
        sort rule), then WVSL's section CRC; "device" is the frame alone,
        its slots checked as a lookup reads them."""
        if self.why != "ok" or uses == "device":
            return self.why
        prev = None
        for r in range(self.rows):
            read_, why, slot = self.slot(r)
            if read_ != OK:
                return why
            if prev is not None and slot.path_hash <= prev:
                return "wvSlotOrder"
            prev = slot.path_hash
        s = self.section
        return "ok" if crc32(self.b[s.offset:s.offset + s.bytes]) == s.crc else "sectionCrc"

    def slot_raw(self, r):
        o = self.section.offset + r * SLOT_BYTES
        return self.b[o:o + SLOT_BYTES]

    def slot(self, r):
        """One slot read and checked (8.3): (OK, "ok", Slot), or (DAMAGED,
        "wvSlotCrc", None) when its CRC fails."""
        raw = self.slot_raw(r)
        self.reads += 1
        h, ms, bars, crc = SLOT.unpack(raw)
        if slot_crc(raw, r) != crc:
            return DAMAGED, "wvSlotCrc", None
        return OK, "ok", Slot(h, ms, bars)

    def find(self, h):
        """6: the row of path hash `h`, by the slots alone: a Found. A CRC that
        fails ends the lookup (DAMAGED: only this lookup misses); a hash
        outside the two already read around it breaks the sort rule (BROKEN:
        the file is absent from then on). The bounds 0 and 2^64 - 1 aren't
        slots: a slot may hold either hash."""
        lo, hi, lo_h, hi_h = 0, self.rows - 1, 0, M64
        probes = []
        while lo <= hi and len(probes) < MAX_READS:
            r = probe_row(h, lo, hi, lo_h, hi_h, len(probes))
            probes.append(r)
            read_, _, slot = self.slot(r)
            if read_ != OK:
                return Found(DAMAGED, r, None, probes)
            v = slot.path_hash
            if (lo > 0 and v <= lo_h) or (hi < self.rows - 1 and v >= hi_h):
                return Found(BROKEN, r, None, probes)
            if v == h:
                return Found(HIT, r, slot, probes)
            if v < h:
                lo, lo_h = r + 1, v
            else:
                hi, hi_h = r - 1, v
        return Found(MISS, None, None, probes)


def read(data):
    """A file opened (8.1-8.2): a Reader, its `why` "ok" or the reason."""
    return Reader(data)


def check(data, uses="all"):
    """8.4: "ok", or the first check the file fails (it is then absent)."""
    return Reader(data).check(uses)


def companion_matches(data, generation, file_bytes, header_crc, key=bytes(16)):
    """7.2: the file is the one the root's entry names: it opens, its
    generation, length and headerCrc are the entry's, the entry's key is all
    zero."""
    r = Reader(data)
    return (r.why == "ok" and r.generation == generation and r.file_bytes == file_bytes
            and r.header_crc == header_crc and bytes(key) == bytes(16))


def same_content(a, b):
    """Two files read the same (13.5): every slot's pathHash, lengthMs and
    bars, row by row. Returns None, or what differs."""
    ra, rb = Reader(a), Reader(b)
    if ra.why != "ok" or rb.why != "ok":
        return "not both present"
    if ra.rows != rb.rows:
        return "rows"
    for r in range(ra.rows):
        if ra.slot(r) != rb.slot(r):
            return "slot %d" % r
    return None


# ---------------------------------------------------------------------------
# When to write a new file (7.3)
# ---------------------------------------------------------------------------
NONE, SAME, NEW = "none", "same", "new"


def commit_outcome(device_txt, old, rows):
    """7.3's table, the first line that holds: NONE (no MPWF: the new root names
    none), SAME (the new root names the old file again) or NEW (a new file).
    `rows` are the rows the writer would write now, [(pathHash, lengthMs, bars)]
    in row order; `old` is the file the current root names, None when it names
    none or the file isn't valid (7.2). A fetch that stopped early isn't an
    input: it decides only what `rows` holds."""
    if not writes_waves(device_txt) or not rows:
        return NONE
    return SAME if old is not None and old[FIRST_SLOT:] == wvsl_bytes(rows) else NEW


def device_text(case):
    """The /.player/device.txt of a `deviceTxt` or `commit` vector (13.3): its
    `text`; None when it has no file; else its readMpwf under a contract line,
    with no such key when it says so."""
    if "text" in case:
        return case["text"]
    if case.get("noFile"):
        return None
    return "contract=1\n" + ("" if case.get("keyMissing") else "read.mpwf=%s\n" % case["readMpwf"])


def commit_rows(case, golden):
    """The rows of a `commit` vector (13.3): those of the golden file it names,
    less `drop`, with `lengthMs` {row: value} applied, then `add`, in row
    order."""
    rows = []
    if case.get("rows"):
        r = Reader(golden[case["rows"]])
        for i in range(r.rows):
            if i not in case.get("drop", []):
                s = r.slot(i)[2]
                rows.append((s.path_hash, case.get("lengthMs", {}).get(str(i), s.length_ms), s.bars))
    rows += [(u64(t["hash"]), t["lengthMs"], bars_from_peaks(t["peaks"])) for t in case.get("add", [])]
    return sorted(rows)


# ---------------------------------------------------------------------------
# Printing
# ---------------------------------------------------------------------------
def hexdump(b):
    """As `hexdump -C`: a line equal to the one before is folded into one '*';
    the length last."""
    out, prev, star = [], None, False
    for o in range(0, len(b), 16):
        line = bytes(b[o:o + 16])
        if line == prev:
            if not star:
                out.append("*")
                star = True
            continue
        star, prev = False, line
        hx = ["%02x" % x for x in line] + ["  "] * (16 - len(line))
        text = "".join(chr(x) if 0x20 <= x < 0x7F else "." for x in line)
        out.append("%08x  %s  %s  |%s|" % (o, " ".join(hx[:8]), " ".join(hx[8:]), text))
    out.append("%08x" % len(b))
    return "\n".join(out)


def _bars_line(bars):
    return "peak %d mean %d" % (max(bars), sum(bars) // len(bars))


def dump(data, slot=None, hex_=False, find=None, out=None):
    r = Reader(data)
    w = (out or sys.stdout).write
    if r.why in ("short", "magic"):
        w("absent: %s\n" % r.why)
        return
    w("MPWF %d.%d headerBytes %d sections %d fileBytes %d generation %d cardId 0x%016X headerCrc 0x%08X\n" % (
        r.major, r.minor, r.header_bytes, r.section_count, r.file_bytes, r.generation, r.card_id, r.header_crc))
    if r.why != "ok":
        w("absent: %s\n" % r.why)
        if hex_:
            w(hexdump(data) + "\n")
        return
    s = r.section
    w("slotBytes %d barCount %d rows %d\n" % (r.slot_bytes, r.bar_count, r.rows))
    w("WVSL entry %d offset %d bytes %d count %d stride %d crc 0x%08X\n" % (
        s.index, s.offset, s.bytes, s.count, s.stride, s.crc))
    w("check (all): %s\n" % r.check("all"))
    if slot is not None and not 0 <= slot < r.rows:
        w("no slot %d\n" % slot)
        return
    if find is not None:
        got = r.find(find)
        w("find 0x%016X: %s, reads %s\n" % (find, got.read, " ".join(str(p) for p in got.probes)))
        if got.read != HIT:
            return
        slot = got.row
    for i in range(r.rows) if slot is None else [slot]:
        raw = r.slot_raw(i)
        crc = struct.unpack_from("<I", raw, SLOT_BYTES - 4)[0]
        read_, why, one = r.slot(i)
        if read_ == OK:
            w("slot %d crc 0x%08X hash 0x%016X lengthMs %d %s\n" % (i, crc, one.path_hash, one.length_ms,
                                                                     _bars_line(one.bars)))
        else:
            w("slot %d crc 0x%08X: %s (%s)\n" % (i, crc, read_, why))
        if slot is not None and hex_:
            w(hexdump(raw) + "\n")
    if hex_ and slot is None:
        w(hexdump(data) + "\n")


# ---------------------------------------------------------------------------
# The test vectors (13): descriptions, goldens, hardening files, vectors.json
# ---------------------------------------------------------------------------
CARD_ID = "0x5EEDC0DE0A1B2C3D"
NFC_CAFE = "Café"
NFD_CAFE = "Café"
SERVER_BARS = 800


def _ramp():
    """Every peak by its place: 0 to 255 over M = 800."""
    return [j * 255 // (SERVER_BARS - 1) for j in range(SERVER_BARS)]


def _ties():
    """At M = 800 every third bar takes 4 peaks (10k + 6 to 10k + 9). One odd
    peak 2k + 1 there and zeros around it is a mean square of (k + 1/2)^2: an
    exact tie, which rounds up to k + 1."""
    peaks = [0] * SERVER_BARS
    for k in range(80):
        peaks[10 * k + 6] = 2 * k + 1
    return peaks


def _music():
    """A made-up track's shape: an intro, verses, choruses, a fade. Integers
    only, so every run makes the same list (the description holds it)."""
    peaks, x = [], 1
    for j in range(SERVER_BARS):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        if j < 80:
            level = j * 100 // 80
        elif j >= 640:
            level = (799 - j) * 100 // 160
        else:
            level = 100 if 320 <= j < 480 else 60
        peaks.append(level * (170 + (x >> 16) % 86) // 100)
    return peaks


G1_FILE = "waves-0000002a.bin"
G1_NOTE = ("The fixture card's waveform index (WAVEFORM-INDEX.md 13.4), the companion of manifest-waves.bin: seven of "
           "tags-0000002a.bin's records, each lengthMs that record's durationMs (0 for Artist/x.mp3, whose record has "
           "none). The card's other records have no slot: their tracks show a plain line. Listed out of order: the "
           "writer sorts by path hash.")


def g1():
    return {
        "format": "MPWF", "file": G1_FILE, "generation": 42, "cardId": CARD_ID, "note": G1_NOTE,
        "tracks": [
            {"path": "Artist/Album/02 - Other.mp3", "lengthMs": 181000, "peaks": _ties(),
             "note": "M = 800, every third bar an exact tie: bar 3k + 2 is k + 1, the others 0"},
            {"path": "Artist/Album/01 - Title.mp3", "lengthMs": 180000, "peaks": _ramp(),
             "note": "M = 800, a ramp: the server's own shape of input"},
            {"path": "A/x.mp3", "lengthMs": 186000, "peaks": [0], "note": "a silent track has a slot: all zero"},
            {"path": "Artist/x.mp3", "lengthMs": 0, "peaks": [255],
             "note": "a brick; length unknown: its record's durationMs is 0"},
            {"path": "Artist/Album/CD1/01 - Disc One.flac", "lengthMs": 183000, "peaks": _music(),
             "note": "M = 800, a made-up song's shape"},
            {"path": NFC_CAFE + "/Album/01 - Title.mp3", "lengthMs": 192000, "peaks": [10, 20],
             "note": "NFC: hashes as F1B24FC757494F2B. M = 2: 120 bars of each peak"},
            {"path": NFD_CAFE + "/Album/01 - Title.mp3", "lengthMs": 193000, "peaks": [30, 40, 50],
             "note": "NFD: another name, another hash (57017C0CC011BB1B). M = 3: 80 bars of each peak"},
        ]}


def g2():
    return {
        "format": "MPWF", "file": "waves-one.bin", "generation": 1, "cardId": CARD_ID,
        "note": "The smallest file, 512 bytes (WAVEFORM-INDEX.md 13.4): one row, every bar 0x40 (M = 1).",
        "tracks": [{"path": "Artist/Album/01 - Title.mp3", "lengthMs": 180000, "peaks": [0x40]}]}


SKEW_LOW, SKEW_HIGH = 40, 8
SKEW_ALONE, SKEW_SHARED = 0x4000000000000000, 0x8000000000000000


def g3():
    """Hashes that are nothing like uniform, so a lookup leaves its
    interpolation for bisection; the two extreme hashes; a hash two tracks
    share."""
    low = [{"hash": "0x%016X" % i, "lengthMs": i, "peaks": [i]} for i in range(SKEW_LOW)]
    high = [{"hash": "0x%016X" % (M64 - i), "lengthMs": 1000 + i, "peaks": [200 + i]} for i in range(SKEW_HIGH)]
    shared = {"hash": "0x%016X" % SKEW_SHARED, "lengthMs": 5, "peaks": [5], "note": "two tracks share this hash: "
              "both are left out, and the hash misses"}
    return {
        "format": "MPWF", "file": "waves-skew.bin", "generation": 9, "cardId": CARD_ID,
        "note": "A search test (WAVEFORM-INDEX.md 13.4), hashes given, not paths: 40 rows from hash 0 up, one alone at "
                "2^62, 8 rows up to 2^64 - 1. Hash 0 and hash 2^64 - 1 are valid in a slot: a reader's first bounds "
                "aren't slots. A lookup among the 40 creeps up one row a probe, four times, then bisects.",
        "tracks": low + [{"hash": "0x%016X" % SKEW_ALONE, "lengthMs": 500, "peaks": [100]}] + high +
                  [shared, dict(shared)]}


def fixture_libraries():
    """Every MPWF description of test/fixtures/card/libraries, in order."""
    return [g1(), g2(), g3()]


def fixture_goldens():
    """{file: bytes} for every description, each read back: every track by
    its own lookup, its bars and length the description's."""
    out = {}
    for d in fixture_libraries():
        data = build(d)
        r = Reader(data)
        for t in d["tracks"]:
            h = u64(t["hash"]) if "hash" in t else path_hash(t["path"])
            got = r.find(h)
            if h == SKEW_SHARED:
                if got.read != MISS:
                    raise AssertionError("%s: the shared hash reads %s" % (d["file"], got.read))
            elif got.read != HIT or got.slot.length_ms != t["lengthMs"] or (
                    got.slot.bars != bars_from_peaks(t["peaks"])):
                raise AssertionError("%s: %r reads back wrong" % (d["file"], t.get("path", t.get("hash"))))
        out[d["file"]] = data
    _check_g1(out[G1_FILE])
    _check_g2(out["waves-one.bin"])
    return out


def _check_g1(data):
    """13.4's typed facts about waves-0000002a.bin."""
    r = Reader(data)
    if (len(data), r.rows, r.generation) != (2048, 7, 42):
        raise AssertionError("G1 geometry")
    bars = {path: r.find(path_hash(path)).slot.bars for path in (
        "Artist/Album/02 - Other.mp3", "A/x.mp3", "Artist/x.mp3", NFC_CAFE + "/Album/01 - Title.mp3",
        NFD_CAFE + "/Album/01 - Title.mp3")}
    ties = [k // 3 + 1 if k % 3 == 2 else 0 for k in range(BAR_COUNT)]
    if list(bars["Artist/Album/02 - Other.mp3"]) != ties:
        raise AssertionError("G1 ties")
    if bars["A/x.mp3"] != bytes(BAR_COUNT) or bars["Artist/x.mp3"] != bytes([255]) * BAR_COUNT:
        raise AssertionError("G1 silence or brick")
    if bars[NFC_CAFE + "/Album/01 - Title.mp3"] != bytes([10]) * 120 + bytes([20]) * 120:
        raise AssertionError("G1 M = 2")
    if bars[NFD_CAFE + "/Album/01 - Title.mp3"] != bytes([30]) * 80 + bytes([40]) * 80 + bytes([50]) * 80:
        raise AssertionError("G1 M = 3")


def _check_g2(data):
    """13.4's typed bytes of waves-one.bin's slot."""
    want = (bytes.fromhex("D888017612B370DA") + bytes.fromhex("20BF0200") + bytes([0x40]) * BAR_COUNT)
    r = Reader(data)
    if len(data) != 512 or r.slot_raw(0)[:252] != want:
        raise AssertionError("G2's slot")


# --- Hardening (13.5) -------------------------------------------------------
class Parts:
    """A file taken apart for editing: the frame's fields, the type header
    (bytes 40 to headerBytes) and the sections in directory order."""

    def __init__(self, data):
        (_, self.major, self.minor, self.header_bytes, n, _, self.generation, self.card_id, _,
         _) = COMMON.unpack_from(data, 0)
        self.head = bytearray(data[40:self.header_bytes])
        self.sections = []
        for i in range(n):
            t, flags, off, nb, count, stride, _, _ = DIR_ENTRY.unpack_from(data, self.header_bytes + 32 * i)
            self.sections.append([t, flags, count, stride, bytearray(data[off:off + nb])])

    def wvsl(self):
        return next(s for s in self.sections if s[0] == WVSL)

    def put(self, fmt, off, value):
        """A type-header field at its file offset."""
        struct.pack_into(fmt, self.head, off - 40, value)

    def reseal_slot(self, r):
        data = self.wvsl()[4]
        o = r * SLOT_BYTES
        struct.pack_into("<I", data, o + SLOT_BYTES - 4, slot_crc(data[o:o + SLOT_BYTES], r))

    def build(self, header_bytes=None):
        """The file again, every CRC made right but the slots'."""
        hb = self.header_bytes if header_bytes is None else header_bytes
        return frame(self.generation, self.card_id, bytes(self.head), [tuple(s) for s in self.sections], hb,
                     self.major, self.minor)


def reseal_header(b):
    """The header's CRC made right after a raw edit of the frame."""
    hb, n = struct.unpack_from("<II", b, 8)
    struct.pack_into("<I", b, 32, 0)
    struct.pack_into("<I", b, 32, crc32(b[:hb + 32 * n]))
    return bytes(b)


def fixture_hardening(golden):
    """13.5's files: [(name, format, uses, expect, why, check, bytes, same,
    slot)], each made from waves-0000002a.bin by breaking one rule, every
    other CRC made right; each one's check() and, for a present file, its
    content against `same`, asserted here. `slot`, when given, is one lookup
    as the device makes it: the hash, what it reads, the slots it takes."""
    g1b = golden[G1_FILE]
    g = Reader(g1b)
    row_hash = [g.slot(i)[2].path_hash for i in range(g.rows)]
    out = []

    def add(name, why, text, data, uses="all", same=None, slot=None):
        expect = "present" if why == "ok" else "absent"
        got = check(data, uses)
        if got != why:
            raise AssertionError("%s: reads as %s, not %s" % (name, got, why))
        if same is not None:
            diff = same_content(data, golden[same])
            if diff:
                raise AssertionError("%s: not the same as %s (%s)" % (name, same, diff))
        info = None
        if slot is not None:
            r = Reader(data)
            found = r.find(slot[0])
            if r.why != "ok" or found.read != slot[1]:
                raise AssertionError("%s: the lookup reads %s, not %s" % (name, found.read, slot[1]))
            info = {"hash": "0x%016X" % slot[0], "read": found.read, "reads": len(found.probes)}
        out.append((name, "MPWF", uses, expect, why, text, data, same, info))

    def edit(fn, src=g1b, header_bytes=None):
        p = Parts(src)
        fn(p)
        return p.build(header_bytes)

    def raw(fn, src=g1b):
        b = bytearray(src)
        fn(b)
        return reseal_header(b)

    def frame_field(off, fmt, value):
        return raw(lambda b: struct.pack_into(fmt, b, off, value))

    # The frame (8.1).
    add("mpwf-short", "short", "39 bytes: below the common header's 40", g1b[:39])
    add("mpwf-short-directory", "short", "200 bytes: the directory's end, 256, is past the file", g1b[:200])
    add("mpwf-magic", "magic", "magic MPWX", frame_field(0, "<4s", b"MPWX"))
    add("mpwf-major-0", "major", "major 0: not a major", frame_field(4, "<H", 0))
    add("mpwf-major-2", "major", "major 2: a newer major", frame_field(4, "<H", 2))
    add("mpwf-header-bytes-short", "headerBytes", "headerBytes 216, below 224",
        edit(lambda p: None, header_bytes=216))
    add("mpwf-header-bytes-odd", "headerBytes", "headerBytes 228, not a multiple of 8",
        edit(lambda p: None, header_bytes=228))
    add("mpwf-section-count", "sectionCount", "sectionCount 65, above 64", frame_field(12, "<I", 65))
    add("mpwf-file-bytes", "fileBytes", "fileBytes 8 more than the file's length",
        frame_field(16, "<I", len(g1b) + 8))

    def wrong_crc(b):
        struct.pack_into("<I", b, 32, struct.unpack_from("<I", b, 32)[0] ^ 1)

    bad_crc = bytearray(g1b)
    wrong_crc(bad_crc)
    add("mpwf-header-crc", "headerCrc", "headerCrc wrong", bytes(bad_crc))

    def wrap_header_bytes():
        b = bytearray(g1b)
        struct.pack_into("<I", b, 8, 0xFFFFFFF8)
        struct.pack_into("<I", b, 32, 0)
        struct.pack_into("<I", b, 32, crc32(b[:(0xFFFFFFF8 + 32) & 0xFFFFFFFF]))
        return bytes(b)

    add("mpwf-header-bytes-wrap", "short", "headerBytes 0xFFFFFFF8: headerBytes + 32 x sectionCount is past the "
        "file (wrapped to 32 bits it is 24, and the header CRC is right for that)", wrap_header_bytes())

    # The directory (8.1).
    def moved_slots(offset, gap):
        b = bytearray(g1b[:256]) + bytes(gap) + bytearray(g1b[256:])
        struct.pack_into("<I", b, 16, len(b))
        struct.pack_into("<I", b, HEADER_BYTES + 8, offset)
        return reseal_header(b)

    add("mpwf-layout-gap", "layout", "WVSL at 264: 8 bytes past the directory's end, a gap of 8 or more",
        moved_slots(264, 8))
    add("mpwf-layout-odd-offset", "layout", "WVSL at 260: not a multiple of 8", moved_slots(260, 4))
    add("mpwf-layout-overlap", "layout", "WVSL at 248: inside the directory", moved_slots(248, 0))

    def trailing():
        b = bytearray(g1b) + bytes(8)
        struct.pack_into("<I", b, 16, len(b))
        return reseal_header(b)

    add("mpwf-layout-trailing", "layout", "8 bytes after WVSL: the last section doesn't end at fileBytes", trailing())

    def twice(p):
        p.sections.append(list(p.wvsl()))

    add("mpwf-wvsl-twice", "layout", "WVSL twice", edit(twice))

    def wrap_section_end():
        # WVZY (one slot's zeros) and an empty WVZZ, both optional, before
        # WVSL; then WVZZ claims 0xFFFFFF00 bytes. Its end wrapped to 32 bits
        # is WVZY's start, where this WVSL starts (one row more, its first
        # WVZY's zeros): a reader that wraps gets past the layout.
        p = Parts(g1b)
        p.sections[0:0] = [[b"WVZY", 0, 0, 0, bytearray(SLOT_BYTES)], [b"WVZZ", 0, 0, 0, bytearray()]]
        b = bytearray(p.build())
        hb = HEADER_BYTES
        zy, zz, sl = (hb + 32 * i for i in (0, 1, 2))
        at = struct.unpack_from("<I", b, zy + 8)[0]
        struct.pack_into("<I", b, zz + 12, 0xFFFFFF00)
        rows = (len(b) - at) // SLOT_BYTES
        struct.pack_into("<IIII", b, sl + 8, at, len(b) - at, rows, SLOT_BYTES)
        struct.pack_into("<I", b, sl + 24, crc32(b[at:]))
        return reseal_header(b)

    add("mpwf-section-end-wrap", "layout", "an optional section of 0xFFFFFF00 bytes before WVSL: its end is past the "
        "file (wrapped to 32 bits it is the start of WVSL, placed there)", wrap_section_end())

    def insert(flags, at):
        def fn(p):
            p.sections.insert(at, [b"WVZZ", flags, 0, 0, bytearray(b"a later minor's section")])
        return fn

    add("mpwf-unknown-required", "unknownRequired", "an unknown REQUIRED section, WVZZ, after WVSL",
        edit(insert(REQUIRED, 1)))
    add("mpwf-unknown-required-flags", "unknownRequired", "an unknown section, WVZZ, after WVSL, flags 3: bit 0 is "
        "REQUIRED, whatever the other bits say", edit(insert(3, 1)))

    def stride_short(p):
        s = p.wvsl()
        s[2], s[3] = s[2] * 2, SLOT_BYTES // 2

    add("mpwf-stride-short", "shape", "WVSL's stride 128, below 256 (count doubled, so bytes = count x stride)",
        edit(stride_short))

    def count_one_more(p):
        p.wvsl()[2] += 1

    add("mpwf-count-bytes", "shape", "WVSL's count 8, bytes for 7", edit(count_one_more))

    def wrap_count(p):
        p.wvsl()[2] += 1 << 24

    add("mpwf-array-bytes-wrap", "shape", "WVSL's count 7 + 2^24: count x stride isn't its 1,792 bytes (wrapped to "
        "32 bits it is)", edit(wrap_count))

    def no_wvsl(p):
        p.sections = [[b"WVZZ", 0, 0, 0, bytearray(b"a later minor's section")]]

    add("mpwf-missing-wvsl", "missing", "no WVSL, an optional section in its place", edit(no_wvsl))

    # The header and WVSL (8.2).
    def field(fmt, off, value):
        return lambda p: p.put(fmt, off, value)

    add("mpwf-slot-bytes", "enum", "slotBytes 512", edit(field("<I", 40, 512)))
    add("mpwf-bar-count", "enum", "barCount 120", edit(field("<I", 44, 120)))

    def slot_stride(p):
        s = p.wvsl()
        s[3] = 2 * SLOT_BYTES
        s[4] = bytearray(b"".join(bytes(s[4][SLOT_BYTES * i:SLOT_BYTES * (i + 1)]) + bytes(SLOT_BYTES)
                                  for i in range(s[2])))

    add("mpwf-slot-stride", "shape", "WVSL's stride 512: at least 256, but a later minor can't widen a slot",
        edit(slot_stride))

    def no_rows(p):
        s = p.wvsl()
        s[2], s[4] = 0, bytearray()

    add("mpwf-rows-zero", "enum", "WVSL's count 0: no row is no file", edit(no_rows))
    add("mpwf-misaligned", "wvLayout", "headerBytes 232: WVSL at 264, off a slot boundary",
        edit(lambda p: None, header_bytes=232))
    add("mpwf-header-bigger", "wvLayout", "headerBytes 480: WVSL at 512, on a slot boundary, but the slots start at "
        "256", edit(lambda p: None, header_bytes=480))
    add("mpwf-wvsl-not-first", "wvLayout", "an optional section before WVSL: WVSL at 312",
        edit(insert(0, 0)))
    add("mpwf-unknown-optional", "wvLayout", "an optional section after WVSL: the directory's second entry puts WVSL "
        "at 288, so a minor can add no section", edit(insert(0, 1)))
    add("mpwf-unknown-optional-flags", "wvLayout", "an optional section after WVSL, flags 2: bit 0 clear, so skipped, "
        "whatever the other bits say; the directory's second entry puts WVSL at 288", edit(insert(2, 1)))

    # Slots (8.3): each edit reseals the slot's CRC but the CRC's own fault.
    def slot_crc_wrong(p):
        p.wvsl()[4][2 * SLOT_BYTES + 252] ^= 0x01

    add("mpwf-slot-crc", "wvSlotCrc", "slot 2's CRC wrong", edit(slot_crc_wrong), slot=(row_hash[2], DAMAGED))

    def swap_slots(reseal):
        def fn(p):
            d = p.wvsl()[4]
            a, b = SLOT_BYTES, 2 * SLOT_BYTES
            d[a:a + SLOT_BYTES], d[b:b + SLOT_BYTES] = d[b:b + SLOT_BYTES], d[a:a + SLOT_BYTES]
            if reseal:
                p.reseal_slot(1)
                p.reseal_slot(2)
        return fn

    add("mpwf-slot-wrong-row", "wvSlotCrc", "slots 1 and 2 swapped, each CRC right for its own row",
        edit(swap_slots(False)), slot=(row_hash[1], DAMAGED))

    def flip(at, bit=0):
        def fn(p):
            p.wvsl()[4][3 * SLOT_BYTES + at] ^= 1 << bit
        return fn

    add("mpwf-flip-hash", "wvSlotCrc", "slot 3: one bit of pathHash flipped", edit(flip(5, 3)),
        slot=(row_hash[3], DAMAGED))
    add("mpwf-flip-length", "wvSlotCrc", "slot 3: one bit of lengthMs flipped", edit(flip(9, 1)),
        slot=(row_hash[3], DAMAGED))
    add("mpwf-flip-bar", "wvSlotCrc", "slot 3: one bit of bar 100 flipped", edit(flip(112, 7)),
        slot=(row_hash[3], DAMAGED))
    add("mpwf-flip-crc", "wvSlotCrc", "slot 3: one bit of the CRC flipped", edit(flip(253, 2)),
        slot=(row_hash[3], DAMAGED))

    unsorted = edit(swap_slots(True))

    def broken_lookup(data):
        """A hash whose lookup meets the broken sort rule: the first in a fixed
        scan, every row's hash and the points between them."""
        r = Reader(data)
        hs = sorted(r.slot(i)[2].path_hash for i in range(r.rows))
        cands = hs + [(a + b) // 2 for a, b in zip([0] + hs, hs + [M64])]
        for h in cands:
            if r.find(h).read == BROKEN:
                return h
        raise AssertionError("no lookup meets the broken sort rule")

    add("mpwf-slots-unsorted", "wvSlotOrder", "slots 1 and 2 swapped, their CRCs made right: hashes out of order",
        unsorted, slot=(broken_lookup(unsorted), BROKEN))

    def equal_hashes(p):
        d = p.wvsl()[4]
        d[3 * SLOT_BYTES:3 * SLOT_BYTES + 8] = d[2 * SLOT_BYTES:2 * SLOT_BYTES + 8]
        p.reseal_slot(3)

    equal = edit(equal_hashes)
    add("mpwf-slots-equal", "wvSlotOrder", "slots 2 and 3 hold one hash, their CRCs right", equal,
        slot=(broken_lookup(equal), BROKEN))

    def wrong_section_crc(b):
        struct.pack_into("<I", b, HEADER_BYTES + 24, struct.unpack_from("<I", b, HEADER_BYTES + 24)[0] ^ 1)

    add("mpwf-section-crc", "sectionCrc", "WVSL's directory CRC wrong: every slot passes, so a writer's bug",
        raw(wrong_section_crc))

    # Truncation: every length is in test_mpwf.py; these three are the kinds.
    add("mpwf-truncated", "fileBytes", "100 bytes cut, inside the last slot", g1b[:-100])

    def cut_and_say_so():
        b = bytearray(g1b[:-100])
        struct.pack_into("<I", b, 16, len(b))
        return reseal_header(b)

    add("mpwf-truncated-resealed", "layout", "100 bytes cut, fileBytes and the header CRC made to agree: WVSL runs "
        "past the end", cut_and_say_so())

    def one_row_less(p):
        s = p.wvsl()
        s[2] -= 1
        s[4] = s[4][:-SLOT_BYTES]

    add("mpwf-truncated-row", "ok", "the last slot cut, every field and CRC made to agree: a valid file of 6 rows. "
        "The root's entry tells them apart (fileBytes, headerCrc: 7.2); not the same as waves-0000002a.bin",
        edit(one_row_less), slot=(row_hash[6], MISS))

    # Present (9).
    def minor1(p):
        p.minor = 1
        for i in range(8, len(p.head)):
            p.head[i] = 0xA5

    add("mpwf-minor-1", "ok", "minor 1: header bytes 48-223 not zero, a later minor's fields; read the same",
        edit(minor1), same=G1_FILE)

    def reserved(b):
        struct.pack_into("<I", b, 36, 0x5A5A5A5A)
        struct.pack_into("<I", b, HEADER_BYTES + 28, 0x5A5A5A5A)
        b[100] = 0x5A

    add("mpwf-reserved-set", "ok", "minor 0 with the reserved fields and a padding byte set: not checked (the header "
        "CRC made right)", raw(reserved), same=G1_FILE)

    def optional_flag(p):
        p.wvsl()[1] = 0

    add("mpwf-wvsl-not-required", "ok", "WVSL's REQUIRED bit clear: a reader that knows the type doesn't look",
        edit(optional_flag), same=G1_FILE)
    add("mpwf-uses-device-slot-crc", "ok", "mpwf-slot-crc read by the device: the frame is good; the slot fails when "
        "read", edit(slot_crc_wrong), uses="device", slot=(row_hash[2], DAMAGED))
    add("mpwf-uses-device-unsorted", "ok", "mpwf-slots-unsorted read by the device: the frame is good; this lookup "
        "meets the sort rule", unsorted, uses="device", slot=(broken_lookup(unsorted), BROKEN))
    add("mpwf-uses-device-section-crc", "ok", "mpwf-section-crc read by the device, which doesn't check it: a lookup "
        "hits", raw(wrong_section_crc), uses="device", slot=(row_hash[4], HIT))
    return out


# --- vectors.json "mpwf" (13.3) -----------------------------------------------
def splitmix64(state):
    """The next (state, value) of SplitMix64: the scale cases' uniform hashes."""
    state = (state + 0x9E3779B97F4A7C15) & M64
    z = state
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
    return state, z ^ (z >> 31)


SCALE_ROWS, SCALE_MISSES = 20000, 5000
SCALE_PATH = "Generated Artist %03d/Sample Album %d/%d - Sample Title.mp3"
SCALE_MISS_PATH = "Hand Copied/%05d.flac"


def scale_hashes():
    """The scale cases (13.3): ("uniform": SplitMix64 from 0, the rows then the
    misses; "paths": 400 artists, 5 albums, 10 tracks, the misses in another
    folder): {name: (rows' hashes sorted, misses' hashes)}."""
    state, uniform = 0, []
    for _ in range(SCALE_ROWS + SCALE_MISSES):
        state, h = splitmix64(state)
        uniform.append(h)
    paths = [path_hash(SCALE_PATH % (a, b, t)) for a in range(400) for b in range(5) for t in range(10)]
    misses = [path_hash(SCALE_MISS_PATH % i) for i in range(SCALE_MISSES)]
    return {"uniform": (sorted(uniform[:SCALE_ROWS]), uniform[SCALE_ROWS:]), "paths": (sorted(paths), misses)}


def scale_case(rows, misses):
    """Every row and miss looked up in a file of those rows: the reads."""
    r = Reader(write([(h, 0, bytes(BAR_COUNT)) for h in rows], 1))
    hits = [r.find(h) for h in rows]
    gone = [r.find(h) for h in misses]
    if any(f.read != HIT for f in hits) or any(f.read != MISS for f in gone):
        raise AssertionError("a scale lookup reads wrong")
    return {"rows": len(rows), "hitReads": sum(len(f.probes) for f in hits),
            "hitMax": max(len(f.probes) for f in hits), "misses": len(misses),
            "missReads": sum(len(f.probes) for f in gone), "missMax": max(len(f.probes) for f in gone)}


def _hex(b):
    return bytes(b).hex()


def _bar_case(note, peaks, bars):
    """A case of the bars rule, typed from the rule and checked here."""
    if list(bars_from_peaks(peaks)) != list(bars):
        raise AssertionError("bars: " + note)
    return {"m": len(peaks), "peaks": _hex(peaks), "bars": _hex(bars), "note": note}


def _sparse_case(note, m, peaks, bars):
    """The same for a long list: {index: value}, the others 0."""
    full = [0] * m
    for i, b in peaks.items():
        full[i] = b
    want = [0] * BAR_COUNT
    for i, b in bars.items():
        want[i] = b
    if list(bars_from_peaks(full)) != want:
        raise AssertionError("bars: " + note)
    return {"m": m, "peaksAt": {str(i): b for i, b in peaks.items()}, "barsAt": {str(i): b for i, b in bars.items()},
            "note": note}


def _bar_cases():
    p7 = [0, 50, 100, 150, 200, 250, 255]
    p239 = [j % 256 for j in range(239)]
    p240 = [255 - j for j in range(240)]
    p241 = [j for j in range(241)]
    p480 = [3, 4] * 240       # a mean square of 12.5: sqrt 3.54 reads 4
    cases = [
        _bar_case("M = 1: every bar the one peak", [200], [200] * BAR_COUNT),
        _bar_case("M = 2: each peak 120 bars", [10, 20], [10] * 120 + [20] * 120),
        _bar_case("M = 3: each peak 80 bars", [1, 2, 3], [1] * 80 + [2] * 80 + [3] * 80),
        _bar_case("M = 7: each peak 34 or 35 bars, none averaged", p7,
                  [0] * 35 + [50] * 34 + [100] * 34 + [150] * 35 + [200] * 34 + [250] * 34 + [255] * 34),
        _bar_case("M = 239: bars 0 and 1 both peak 0, then one each", p239, [0] + p239),
        _bar_case("M = 240: the bars are the peaks", p240, p240),
        _bar_case("M = 241: the last bar takes peaks 239 and 240: RMS 239.5005 reads 240", p241,
                  list(range(239)) + [240]),
        _bar_case("M = 480: two peaks a bar", p480, [4] * BAR_COUNT),
        _sparse_case("M = 800: a width-3 bar: 200 of 3 peaks is sqrt(40000 / 3) = 115.47", 800, {0: 200}, {0: 115}),
        _sparse_case("M = 800, a tie: sqrt(1 / 4) = 0.5 rounds up to 1", 800, {6: 1}, {2: 1}),
        _sparse_case("M = 800, a tie: sqrt(9 / 4) = 1.5 rounds up to 2", 800, {6: 3}, {2: 2}),
        _sparse_case("M = 800, a tie: sqrt(25 / 4) = 2.5 rounds up to 3; half to even gives 2", 800, {6: 5}, {2: 3}),
        _sparse_case("M = 800, a tie: sqrt(64009 / 4) = 126.5 rounds up to 127; half to even gives 126", 800, {6: 253},
                     {2: 127}),
        _sparse_case("M = 800, a tie from three peaks: 2, 2 and 1 are sqrt(9 / 4) = 1.5", 800, {6: 2, 7: 2, 8: 1},
                     {2: 2}),
        _sparse_case("M = 800, just under a tie: 2 and 2 are sqrt(8 / 4) = 1.41", 800, {6: 2, 7: 2}, {2: 1}),
        _sparse_case("M = 800, under the first step: 1 of 3 peaks is sqrt(1 / 3) = 0.58, of 4 a tie at 0.5", 800,
                     {0: 1, 6: 1}, {0: 1, 2: 1}),
        _sparse_case("M = 1000: 25 peaks over 6 bars: bar 5 takes 5", 1000, {24: 10}, {5: 4}),
        _bar_case("all 255: 255, the largest bar", [255] * 800, [255] * BAR_COUNT),
    ]
    return cases


def _ranges():
    """Bar i's [start, end), typed and checked."""
    typed = {800: [(0, 0, 3), (1, 3, 6), (2, 6, 10), (3, 10, 13), (119, 396, 400), (120, 400, 403), (239, 796, 800)],
             1: [(0, 0, 1), (239, 0, 1)],
             7: [(0, 0, 1), (34, 0, 1), (35, 1, 2), (239, 6, 7)],
             239: [(0, 0, 1), (1, 0, 1), (2, 1, 2), (239, 238, 239)],
             241: [(0, 0, 1), (238, 238, 239), (239, 239, 241)],
             1000: [(0, 0, 4), (5, 20, 25), (6, 25, 29), (239, 995, 1000)]}
    out = []
    for m, cases in typed.items():
        for i, start, end in cases:
            if bar_range(i, m) != (start, end):
                raise AssertionError("range %d of %d" % (i, m))
        out.append({"m": m, "cases": [{"i": i, "start": s, "end": e} for i, s, e in cases]})
    return out


def _pick_cases():
    """probe_row's arithmetic: each typed from the rule and checked."""
    top, half = M64, 1 << 63
    cases = [
        (half, 0, 6, 0, top, 0, 3, "the middle of the first bounds: 3 by rounding"),
        (top // 7 * 2, 0, 6, 0, top, 0, 2, "2/7 of the bounds: 1.71 rounds to 2, where a floor gives 1"),
        (0, 0, 249999, 0, top, 0, 0, "h = loH: num is 0, so lo"),
        (top, 0, 249999, 0, top, 0, 249999, "h = hiH: num = den, so hi, in 64 bits at the largest product"),
        (top - 1, 0, 249999, 0, top, 1, 249999, "nearly so: the top 32 bits still equal"),
        ((1 << 32) - 1, 5, 20, 0, top, 0, 5, "h under 2^32 above loH: num is 0, so lo"),
        (1000 + (1 << 31), 10, 20, 1000, 1000 + (1 << 32) - 1, 0, 15, "a span under 2^32: den is 0, so it bisects"),
        (1000, 7, 7, 1000, 1001, 2, 7, "one row, den 0"),
        (half, 0, 249999, 0, top, 3, 125000, "probe 3 still interpolates"),
        (half, 0, 249999, 0, top, 4, 124999, "probe 4 bisects: (hi - lo) / 2 rounds down"),
        (top, 3, 6, 0, top, 23, 4, "any later probe bisects, whatever h says"),
    ]
    out = []
    for h, lo, hi, lo_h, hi_h, probes, row, note in cases:
        if probe_row(h, lo, hi, lo_h, hi_h, probes) != row:
            raise AssertionError("pick: " + note)
        out.append({"h": "0x%016X" % h, "lo": lo, "hi": hi, "loH": "0x%016X" % lo_h, "hiH": "0x%016X" % hi_h,
                    "probes": probes, "row": row, "note": note})
    return out


def _search_case(r, h, note=None):
    f = r.find(h)
    e = {"hash": "0x%016X" % h, "read": f.read, "probes": f.probes}
    if f.read == HIT:
        e["row"] = f.row
    if note:
        e["note"] = note
    return e


def _commit_cases(golden):
    """7.3's table: each outcome typed from the rule and checked."""
    one = "waves-one.bin"
    extra = {"hash": "0x0000000000000001", "lengthMs": 1000, "peaks": [9]}
    cases = [
        ("2", G1_FILE, G1_FILE, {}, NONE, "read.mpwf doesn't list 1: no MPWF, whatever there is to write"),
        ("1", None, None, {}, NONE, "no file, and no row to write"),
        ("1", one, None, {}, NONE, "the old file's only track left the card and no other has peaks: no row to write, "
                                   "so no MPWF, not a file of 0 rows; the clean-up deletes the old file"),
        ("1", None, G1_FILE, {}, NEW, "a card's first waves"),
        ("1", G1_FILE, G1_FILE, {}, SAME, "nothing changed; or a fetch stopped early, gained nothing, and no old row "
                                          "changed: either way the bytes are equal"),
        ("1", G1_FILE, G1_FILE, {"drop": [6]}, NEW, "row 6's track left the card; a fetch stopped early and gained "
                                                    "nothing: the bytes differ, so a new file leaves that row out"),
        ("1", G1_FILE, G1_FILE, {"add": [extra]}, NEW, "a row gained: every row after it moves"),
        ("1", G1_FILE, G1_FILE, {"lengthMs": {"3": 1000}}, NEW, "one row's lengthMs: no CRC tells these bytes from the "
                                                                "old file's (4.2), so compare them"),
    ]
    out = []
    for read_mpwf, old, rows, change, outcome, note in cases:
        e = dict({"readMpwf": read_mpwf, "old": old, "rows": rows}, **change)
        e.update(outcome=outcome, note=note)
        if commit_outcome(device_text(e), golden.get(old), commit_rows(e, golden)) != outcome:
            raise AssertionError("commit: " + note)
        out.append(e)
    return out


def _head(rows):
    """A file's first 256 bytes for `rows`, with no slots behind them: a
    fixture can't hold the 64 MB of the row limit. Its section CRC is 0."""
    b = bytearray(FIRST_SLOT)
    DIR_ENTRY.pack_into(b, HEADER_BYTES, WVSL, REQUIRED, FIRST_SLOT, SLOT_BYTES * rows, rows, SLOT_BYTES, 0, 0)
    COMMON.pack_into(b, 0, MAGIC, MAJOR, 0, HEADER_BYTES, 1, size(rows), 1, u64(CARD_ID), 0, 0)
    TYPE_HEADER.pack_into(b, 40, SLOT_BYTES, BAR_COUNT)
    struct.pack_into("<I", b, 32, crc32(b))
    return bytes(b)


def fixture_vectors(golden):
    """The "mpwf" group of vectors.json, each value recomputed here."""
    v = {"note": "WAVEFORM-INDEX.md 13.3. 'who': both unless said. Byte strings are lowercase hex; integers in hex "
                 "strings are 0x-prefixed."}
    v["names"] = [{"generation": g, "name": file_name(g)} for g in (1, 42, 0xFFFFFFFF)]
    if [e["name"] for e in v["names"]] != ["waves-00000001.bin", "waves-0000002a.bin", "waves-ffffffff.bin"]:
        raise AssertionError("names")
    v["sizes"] = [{"rows": n, "fileBytes": size(n)} for n in (1, 7, 10000, 20000, 50000, MAX_ROWS)]
    if [e["fileBytes"] for e in v["sizes"]] != [512, 2048, 2560256, 5120256, 12800256, 64000256]:
        raise AssertionError("sizes")
    v["barRanges"] = _ranges()
    v["bars"] = _bar_cases()
    r = Reader(golden["waves-one.bin"])
    raw = r.slot_raw(0)
    v["slot"] = {"file": "waves-one.bin", "row": 0, "path": "Artist/Album/01 - Title.mp3",
                 "pathHash": "0xDA70B312760188D8", "lengthMs": 180000, "barsAll": "0x40", "bytes": raw.hex(),
                 "crc": "0x%08X" % struct.unpack_from("<I", raw, 252)[0], "unseeded": "0x%08X" % crc32(raw[:252]),
                 "note": "CRC-32 over the row as u32 LE, then the slot's first 252 bytes; 'unseeded' is the CRC of "
                         "those bytes alone"}
    g = Reader(golden[G1_FILE])
    raw = g.slot_raw(3)
    v["slotCrc"] = {"file": G1_FILE, "row": 3, "crc": "0x%08X" % struct.unpack_from("<I", raw, 252)[0],
                    "unseeded": "0x%08X" % crc32(raw[:252])}
    one, seven = Reader(golden["waves-one.bin"]).section.crc, g.section.crc
    blank = Reader(write([(i + 1, 0, bytes(BAR_COUNT)) for i in range(7)], 1)).section.crc
    if blank != seven:
        raise AssertionError("WVSL's CRC should depend on its row count alone")
    v["sectionCrc"] = [{"rows": 1, "file": "waves-one.bin", "crc": "0x%08X" % one},
                       {"rows": 7, "file": G1_FILE, "crc": "0x%08X" % seven,
                        "note": "any 7 slots with good CRCs have this section CRC, whatever they hold: 4.2"}]
    v["pick"] = _pick_cases()
    g_row = [g.slot(i)[2].path_hash for i in range(g.rows)]
    gaps = [(a + b) // 2 for a, b in zip(g_row, g_row[1:])]
    skew = Reader(golden["waves-skew.bin"])
    v["search"] = {
        "note": "each lookup's read, its row on a hit, and the rows it reads in order",
        "files": {
            G1_FILE: [_search_case(g, h) for h in g_row] + [
                _search_case(g, gaps[2], "a miss between rows 2 and 3"), _search_case(g, 0, "below every row: hi "
                                                                                         "reaches -1"),
                _search_case(g, M64, "above every row")],
            "waves-one.bin": [_search_case(r, path_hash("Artist/Album/01 - Title.mp3")), _search_case(r, 0),
                              _search_case(r, M64)],
            "waves-skew.bin": [
                _search_case(skew, 0, "hash 0 in row 0: the first bound 0 isn't a slot"),
                _search_case(skew, M64, "hash 2^64 - 1 in the last row: nor is the first bound 2^64 - 1"),
                _search_case(skew, 3, "creeping up: 3 probes, then the hit"),
                _search_case(skew, 20, "four probes creep from row 0, then bisection"),
                _search_case(skew, SKEW_ALONE, "the row alone: interpolation is far off"),
                _search_case(skew, M64 - 5, "creeping down from the last row"),
                _search_case(skew, SKEW_SHARED, "the hash two tracks shared: left out, a miss"),
                _search_case(skew, 1 << 40, "no such row")]},
    }
    if [len(e["probes"]) for e in v["search"]["files"]["waves-skew.bin"][:2]] != [1, 1]:
        raise AssertionError("the extreme hashes read at once")
    hashes = scale_hashes()
    v["scale"] = {
        "note": "every row looked up and misses too, in a file of these hashes (the bars don't matter): the reads "
                "summed, and the most any lookup took. uniform: SplitMix64 from state 0, the rows' hashes first, "
                "then the misses'. paths: the path hash of pattern % (artist, album, track), artist 0-399, album "
                "0-4, track 0-9; the misses', of missPattern % i, i 0-4999. MEASURED by the reference: the same "
                "search reads exactly these.",
        "splitmix64": ["0xE220A8397B1DCDAF", "0x6E789E6AA1B965F4", "0x06C45D188009454F"],
        "pattern": SCALE_PATH, "missPattern": SCALE_MISS_PATH,
        "uniform": scale_case(*hashes["uniform"]), "paths": scale_case(*hashes["paths"])}
    s, first = 0, []
    for _ in range(3):
        s, h = splitmix64(s)
        first.append("0x%016X" % h)
    if first != v["scale"]["splitmix64"]:
        raise AssertionError("splitmix64")
    v["rowLimit"] = [{"rows": n, "fileBytes": size(n), "head": _hex(_head(n)), "why": why,
                      "note": "uses device: a source of fileBytes whose first 256 bytes are head; no slot is read"}
                     for n, why in ((MAX_ROWS, "ok"), (MAX_ROWS + 1, "enum"))]
    v["deviceTxt"] = {"who": "software", "note": "a writer writes MPWF only when /.player/device.txt's read.mpwf lists "
                                                 "1; no file, or no read.mpwf key, reads as none. readMpwf is the "
                                                 "value under a contract line; text is the file's whole text",
                      "cases": [{"readMpwf": "", "writes": False}, {"readMpwf": "1", "writes": True},
                                {"readMpwf": "2", "writes": False}, {"readMpwf": "1,2", "writes": True},
                                {"readMpwf": " 1 , 2", "writes": True}, {"readMpwf": "01", "writes": True},
                                {"readMpwf": "x1", "writes": False}, {"readMpwf": "\t1", "writes": False},
                                {"noFile": True, "writes": False}, {"keyMissing": True, "writes": False},
                                {"text": "read.mpwf=1\n", "writes": False,
                                 "note": "no contract line: not a device.txt, so no file"},
                                {"text": "read.mpwf=1\ncontract=1\n", "writes": True,
                                 "note": "the contract line may stand anywhere"},
                                {"text": "contract=2\nread.mpwf=1\n", "writes": True,
                                 "note": "whatever the contract number says"},
                                {"text": "contract=1\nread.mpwf=2\nread.mpwf=1\n", "writes": True,
                                 "note": "a key given twice reads as its last"},
                                {"text": "contract=1\nread.mpwf=1\nread.mpwf=2\n", "writes": False,
                                 "note": "a key given twice reads as its last"},
                                {"text": "contract=1\r\nread.mpwf=1\r\n", "writes": True, "note": "CRLF is read"}]}
    v["commit"] = {"who": "software", "note": "7.3's table, the first line that holds. readMpwf is the value under a "
                                              "contract line; old is the golden file the current root names, null for "
                                              "none; rows, the golden file whose rows the writer starts from (null: "
                                              "none), less drop (row numbers), with lengthMs ({row: value}) applied, "
                                              "then add ({hash, lengthMs, peaks}), sorted by hash. outcome: none, same "
                                              "or new",
                   "cases": _commit_cases(golden)}
    return v


# ---------------------------------------------------------------------------
# The command line
# ---------------------------------------------------------------------------
def _load_tracks(path, generation, card_id):
    """A description, or the plain map a writer's own fetch would make."""
    d = json.loads(Path(path).read_text(encoding="utf-8"))
    if d.get("format") == "MPWF":
        return d
    return {"format": "MPWF", "generation": generation, "cardId": card_id,
            "tracks": [dict(v, path=k) for k, v in d.items()]}


def main(argv=None):
    ap = argparse.ArgumentParser(description="The waveform index (MPWF v1, docs/WAVEFORM-INDEX.md): the reference "
                                             "writer and reader.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="a file from {path: {lengthMs, peaks}} or a description")
    b.add_argument("tracks")
    b.add_argument("out")
    b.add_argument("--generation", type=int, default=1, help="a plain map's generation")
    b.add_argument("--card-id", default="0", help="a plain map's card id")
    c = sub.add_parser("check", help="validate a file as a reader does (8)")
    c.add_argument("file")
    c.add_argument("--uses", choices=("all", "device"), default="all")
    d = sub.add_parser("dump", help="what a file holds")
    d.add_argument("file")
    d.add_argument("--slot", type=int)
    d.add_argument("--hex", action="store_true")
    f = d.add_mutually_exclusive_group()
    f.add_argument("--path", help="look a path up, relative to /music")
    f.add_argument("--hash", help="look a path hash up")
    sub.add_parser("vectors", help="vectors.json's mpwf group, recomputed")
    z = sub.add_parser("size", help="a file's length")
    z.add_argument("rows", type=int)
    a = ap.parse_args(argv)
    try:
        if a.cmd == "build":
            data = build(_load_tracks(a.tracks, a.generation, a.card_id))
            Path(a.out).write_bytes(data)
            print("%s: %d bytes, %d rows, headerCrc 0x%08X" % (a.out, len(data), (len(data) - FIRST_SLOT) // SLOT_BYTES,
                                                                struct.unpack_from("<I", data, 32)[0]))
        elif a.cmd == "check":
            why = check(Path(a.file).read_bytes(), a.uses)
            print("%s: %s" % (a.file, "present" if why == "ok" else "absent (%s)" % why))
            return 0 if why == "ok" else 1
        elif a.cmd == "dump":
            h = path_hash(a.path) if a.path else u64(a.hash) if a.hash else None
            dump(Path(a.file).read_bytes(), a.slot, a.hex, h)
        elif a.cmd == "vectors":
            sys.stdout.write(json.dumps(fixture_vectors(fixture_goldens()), indent=1, ensure_ascii=False) + "\n")
        elif a.cmd == "size":
            n = size(a.rows)
            print("%d bytes (%.2f MB)" % (n, n / 1e6))
    except WriterError as e:
        print("refused: %s" % e, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
