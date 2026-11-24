#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Listening stats (docs/LISTENING-STATS.md): the reference.

Every rule the firmware's stats must reproduce is here in its plainest
form, with the plan's section it implements: what counts (4.1), the fold
of a slot into the aggregate and each slot once (5.6), the merge of two
per-track rows (5.2), `/.player/stats.bin` and its base file byte for
byte (5.2, 5.3), the boot (5.4-5.6: the read, the fold above MARK, the
save, the trim, the seq rule), the sessions' save moments (5.5, 5.14:
the records and their save, a library update's fence, the orderly end, a
held tmp, a boot overflow's tries, behind), the shelves' rankers (4.3)
and the windows (4.1, 4.2), and what S3's visit reads besides the lists
(9.7's S3a: the totals, the 7 days from DAYS and their span, the u40
played set). It keeps the whole aggregate in RAM: the
firmware (S1's rework) keeps only the records and merges the card's rows
at each save, which writes the same bytes given the same save moments.
lib/core's StatsFold, StatsFile and StatsRank (S1, S2) are tested
against what it makes: card_fixtures.py imports it for the fixtures in
test/fixtures/card/stats, and test_statsref.py tests it. Standard library
only; it reads the play log with tools/playlog.py and writes the card
container's frame with tools/mpwf.py's writer.

    python tools/statsref.py dump STATS_BIN [BASE]   the stats as the device reads them, as JSON
                                       (BASE: by default the stats-<gen>.bin beside STATS_BIN)
    python tools/statsref.py boot DIR [--now MS] [--acks DEV:EPOCH:SEQ,...] [--commit-time S]
                                       a boot over a copy of a card's /.player (nothing written):
                                       what it reads, folds, saves and trims, and the lists
    python tools/statsref.py seed CARD [...]          a test card's play log, and with --base its
                                       stats (seed -h; S1-S3's device passes)

A slot is playlog.decode()'s dict. Times: a slot's startedAt is Unix ms
(UTC); a row's firstAt and lastAt are 4.1's times, seconds since
2026-01-01T00:00:00Z plus 1; hours and days count from 2026-01-01, day 0.
"""
import argparse
import json
import os
import random
import re
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mpwf  # noqa: E402  (the card container's frame: METADATA 2.4)
import playlog  # noqa: E402  (the play log: METADATA 2.20)

# ---------------------------------------------------------------------------
# What counts (4.1)
# ---------------------------------------------------------------------------
MIN_MS = 1767225600000                  # 2026-01-01T00:00:00Z (wallclock::kMinMs)
MAX_MS = 4102444799999                  # 2099-12-31T23:59:59.999Z (wallclock::kMaxMs)
HOUR_MS = 3600000
DAY_MS = 86400000
LINE_MS = 30000
YOU, SHUFFLE, AUTODJ = 0, 1, 2          # a source as HEAD's order and TRKS's flags keep it
SKIPPED = 2                             # the slot's outcome 2 (METADATA 2.20.3)
NO_FIRST = 0xFFFFFFFF                   # firstAt of a row with skips only
UNSET_DAY = 0xFFFF                      # HEAD's sinceDay, not set yet
M8, M16, M24, M32, M64 = (1 << 8) - 1, (1 << 16) - 1, (1 << 24) - 1, (1 << 32) - 1, (1 << 64) - 1


def counts(s) -> bool:
    """A slot that counts: a valid play slot (decode()'s "ok", kind 1) with
    an epoch, a seq and a deviceId (METADATA 2.20.6, item 2). Once per
    (deviceId, epoch, seq): the fold's business (Session.scan)."""
    return s["kind"] == playlog.KIND_PLAY and playlog.usable(s)


def is_play(s) -> bool:
    """playedMs at or over the line, min(30 s, durationMs / 2), 30 s when
    durationMs is 0; the half not rounded (mStream's decideCounted:
    playedMs >= 30,000, or playedMs >= durationMs x 0.5), so in integers
    2 x playedMs >= min(60,000, durationMs). Whatever the outcome."""
    d = s["durationMs"]
    return 2 * s["playedMs"] >= (min(2 * LINE_MS, d) if d else 2 * LINE_MS)


def is_skip(s) -> bool:
    """Outcome 2 (skipped) under the line. Completed or stopped under the
    line is neither; an outcome of 0 or above 3 reads as stopped."""
    return s["outcome"] == SKIPPED and not is_play(s)


def source(s) -> int:
    """3 Auto DJ, 2 shuffle, anything else (1 manual, 0 not said, above 3)
    "you"."""
    return {3: AUTODJ, 2: SHUFFLE}.get(s["source"], YOU)


def believable(ms) -> bool:
    return ms is not None and MIN_MS <= ms <= MAX_MS


def dated(s) -> bool:
    """startedAt from 2026-01-01 to 2099-12-31 UTC; else undated (0 while
    the clock was unset, or another device's clock)."""
    return believable(s["startedAt"])


def hour_of(ms) -> int:
    return (ms - MIN_MS) // HOUR_MS


def day_of(ms) -> int:
    return (ms - MIN_MS) // DAY_MS


def time_of(ms) -> int:
    """A time in a row: seconds since 2026-01-01T00:00:00Z, plus 1 (0 is
    none)."""
    return (ms - MIN_MS) // 1000 + 1


def day_of_time(t) -> int:
    """A row's time (time_of's) back to its UTC day."""
    return (t - 1) // 86400


def today_of(now_ms):
    """The board's clock now as a UTC day; None while it is unset or not
    believed."""
    return day_of(now_ms) if believable(now_ms) else None


def sat(v, top):
    return min(v, top)


# ---------------------------------------------------------------------------
# A track's row (5.2: TRKS's, TRKD's) and the merge
# ---------------------------------------------------------------------------
class Row:
    """plays, skips (u16, saturating); firstAt (the earliest dated counted
    play's time; 0 counted plays, none dated; NO_FIRST none: skips only);
    lastAt (the latest dated counted play's time; 0 none); lastOrd (the
    fold round of the latest counted play, u24); src (the source of the play
    firstAt keeps: YOU, SHUFFLE, AUTODJ)."""
    __slots__ = ("plays", "skips", "firstAt", "lastAt", "lastOrd", "src")

    def __init__(self, plays=0, skips=0, firstAt=NO_FIRST, lastAt=0, lastOrd=0, src=YOU):
        self.plays, self.skips, self.firstAt, self.lastAt, self.lastOrd, self.src = (
            plays, skips, firstAt, lastAt, lastOrd, src)

    def tuple(self):
        return (self.plays, self.skips, self.firstAt, self.lastAt, self.lastOrd, self.src)

    def __eq__(self, other):
        return isinstance(other, Row) and self.tuple() == other.tuple()

    def __repr__(self):
        return "Row%r" % (self.tuple(),)


def first_key(t, src):
    """firstAt's order: a dated time before an undated play (0) before none
    (NO_FIRST); on equal times the smaller source code."""
    return ({0: 1 << 32, NO_FIRST: (1 << 32) + 1}.get(t, t), src)


def merge(a: Row, b: Row) -> Row:
    """a (+) b, a the older (the base before the delta; a row before a
    slot). Every rule is order-free, so any order of the same slots gives
    the same row: plays and skips summed, saturating; firstAt the earlier
    dated time (0 only when neither side has one; NO_FIRST loses to both),
    its source going with it, the smaller source on equal times; lastAt and
    lastOrd the larger."""
    first, src = min(first_key(a.firstAt, a.src), first_key(b.firstAt, b.src))
    first = {1 << 32: 0, (1 << 32) + 1: NO_FIRST}.get(first, first)
    return Row(sat(a.plays + b.plays, M16), sat(a.skips + b.skips, M16), first, max(a.lastAt, b.lastAt),
               max(a.lastOrd, b.lastOrd), src)


def slot_row(s, rnd) -> Row:
    """A slot as the row of that one slot: a play is plays 1, firstAt and
    lastAt its time (firstAt 0 and lastAt 0 when undated), lastOrd its round,
    its source; a skip is skips 1, firstAt NO_FIRST, lastAt 0, lastOrd 0.
    None for a slot that is neither."""
    if is_play(s):
        t = time_of(s["startedAt"]) if dated(s) else 0
        return Row(1, 0, t, t, sat(rnd, M24), source(s))
    if is_skip(s):
        return Row(0, 1, NO_FIRST, 0, 0, YOU)
    return None


# ---------------------------------------------------------------------------
# The aggregate (5.2) and the fold of one slot (5.6)
# ---------------------------------------------------------------------------
SOURCES = ("you", "shuffle", "autodj")


def new_head():
    return {"plays": 0, "skips": 0, "listenedMs": 0, "slots": 0,
            "you": [0, 0, 0], "shuffle": [0, 0, 0], "autodj": [0, 0, 0],  # plays, skips, listenedMs
            "undated": 0, "sinceDay": UNSET_DAY, "flags": 0, "round": 0,
            "baseGen": 0, "baseRows": 0, "baseCrc": 0, "twice": 0, "gap": 0}


FLAG_SINCE_COMMIT = 1                   # HEAD's flags bit 0: sinceDay came from the sync's commitTime
FLAG_BASE_LOST = 2                      # bit 1: a base was lost (5.4)


class Stats:
    """What stats.bin holds, and its base's rows.

    head: HEAD's fields (new_head()). mark: {(deviceId bytes, epoch): the
    highest seq folded}. hour: {hour: [plays, listenedMs]}. days: {day:
    [plays, skips, autodjPlays, autodjSkips, listenedMs, shufflePlays,
    shuffleSkips]}. rcnt: {(day, pathHash): plays}. trkd: {pathHash: Row},
    the rows changed since the base. base: {pathHash: Row}, the base file's
    rows. generation: stats.bin's (the save count; 0 while none was
    saved)."""

    def __init__(self):
        self.head = new_head()
        self.mark = {}
        self.hour = {}
        self.days = {}
        self.rcnt = {}
        self.trkd = {}
        self.base = {}
        self.generation = 0

    def fold(self, s, rnd):
        """One slot that counts, at fold round rnd (5.6), into every
        section: HEAD always; HOUR, DAYS and RCNT when dated; TRKD for a
        play or a skip; MARK."""
        h = self.head
        ms, src, play, skip = s["playedMs"], SOURCES[source(s)], is_play(s), is_skip(s)
        h["slots"] = sat(h["slots"] + 1, M64)
        h["listenedMs"] = sat(h["listenedMs"] + ms, M64)
        h[src][2] = sat(h[src][2] + ms, M64)
        if play:
            h["plays"] = sat(h["plays"] + 1, M64)
            h[src][0] = sat(h[src][0] + 1, M32)
            if not dated(s):
                h["undated"] = sat(h["undated"] + 1, M32)
        if skip:
            h["skips"] = sat(h["skips"] + 1, M64)
            h[src][1] = sat(h[src][1] + 1, M32)
        if dated(s):
            hr = self.hour.setdefault(hour_of(s["startedAt"]), [0, 0])
            hr[0] = sat(hr[0] + play, M8)
            hr[1] = sat(hr[1] + ms, M32)
            day = day_of(s["startedAt"])
            d = self.days.setdefault(day, [0, 0, 0, 0, 0, 0, 0])
            d[0] = sat(d[0] + play, M16)
            d[1] = sat(d[1] + skip, M16)
            if src == "autodj":
                d[2] = sat(d[2] + play, M8)
                d[3] = sat(d[3] + skip, M8)
            if src == "shuffle":
                d[5] = sat(d[5] + play, M8)
                d[6] = sat(d[6] + skip, M8)
            d[4] = sat(d[4] + ms, M32)
            if play:
                k = (day, s["pathHash"])
                self.rcnt[k] = sat(self.rcnt.get(k, 0) + 1, M16)
        r = slot_row(s, rnd)
        if r is not None:
            old = self.trkd.get(s["pathHash"])
            self.trkd[s["pathHash"]] = r if old is None else merge(old, r)
        k = (playlog.device_bytes(s["deviceId"]), s["epoch"])
        self.mark[k] = max(self.mark.get(k, 0), s["seq"])

    def track(self, h):
        """A track's whole row, base (+) delta; None when it has none."""
        a, b = self.base.get(h), self.trkd.get(h)
        return b if a is None else a if b is None else merge(a, b)

    def tracks(self):
        """Every track's whole row: {pathHash: Row}."""
        return {h: self.track(h) for h in set(self.base) | set(self.trkd)}

    def covered(self, s) -> bool:
        """5.5's trim: a play slot at or under MARK for its (deviceId,
        epoch). A slot whose pair has no MARK row is covered only at seq 0,
        as ACKS's acknowledged() reads it."""
        return s["seq"] <= self.mark.get((playlog.device_bytes(s["deviceId"]), s["epoch"]), 0)


# ---------------------------------------------------------------------------
# Retention at a save (5.2)
# ---------------------------------------------------------------------------
HOUR_ROWS = 840                         # 35 days of every hour
DAYS_ROWS = 400
RCNT_DAYS = 32
RCNT_ROWS = 4096


def retain(st: Stats, today):
    """HOUR and DAYS keep their newest rows (840, 400), never pruned by
    date. RCNT keeps 32 days counted back from the earlier of today and the
    newest dated day folded (DAYS's newest), rows newer than that kept;
    nothing by date while the clock is unset or not believed (today None);
    then past 4,096 rows whole days go, oldest first, until 4,096 or fewer
    are left (one day of more than 4,096 tracks goes whole too). MARK and
    TRKD are never pruned."""
    for table, cap in ((st.hour, HOUR_ROWS), (st.days, DAYS_ROWS)):
        for k in sorted(table)[:-cap]:
            del table[k]
    if today is not None and st.days:
        ref = min(today, max(st.days))
        for k in [k for k in st.rcnt if k[0] <= ref - RCNT_DAYS]:
            del st.rcnt[k]
    while len(st.rcnt) > RCNT_ROWS:
        oldest = min(k[0] for k in st.rcnt)
        for k in [k for k in st.rcnt if k[0] == oldest]:
            del st.rcnt[k]


# ---------------------------------------------------------------------------
# The files (5.2, 5.3): a card container (METADATA 2.4)
# ---------------------------------------------------------------------------
MAGIC = b"MPST"
MAJOR, MINOR = 1, 0
HEADER_BYTES = 40                       # the common header alone: no type header
REQUIRED = 1                            # a section's flags bit 0 (CardContainer.h kSectionRequired)
HEAD, MARK, HOUR, DAYS, RCNT, TRKD, TRKS = (b"HEAD", b"MARK", b"HOUR", b"DAYS", b"RCNT", b"TRKD", b"TRKS")
HEAD_BYTES = 128
STATS_SECTIONS = ((HEAD, 0), (MARK, 24), (HOUR, 8), (DAYS, 16), (RCNT, 12), (TRKD, 24))  # (type, stride; 0 a blob)
BASE_SECTIONS = ((TRKS, 24),)
STATS_BIN, STATS_TMP, STATS_BAD = "stats.bin", "stats.tmp", "stats.bad"
MAX_SECTIONS = 64

HEAD_LAYOUT = struct.Struct("<QQQQ" + "IIQ" * 3 + "IHHIIIIII16x")
MARK_ROW = struct.Struct("<6sHQII")     # deviceId, reserved, epoch, seq, reserved
HOUR_ROW = struct.Struct("<II")         # hour u24 | plays u8 << 24, listenedMs
DAYS_ROW = struct.Struct("<HHHBBIBB2x")
RCNT_ROW = struct.Struct("<HHQ")
TRKS_ROW = struct.Struct("<QHHIII")     # ..., lastOrd u24 | flags u8 << 24
COMMON = struct.Struct("<4sHHIIIIQII")
DIR_ENTRY = struct.Struct("<4sIIIIIII")
assert HEAD_LAYOUT.size == HEAD_BYTES and DAYS_ROW.size == 16 and RCNT_ROW.size == 12 and TRKS_ROW.size == 24


def base_name(gen) -> str:
    """5.3: stats-<gen>.bin, the generation in 8 lowercase hex digits."""
    return "stats-%08x.bin" % gen


def crc32(b) -> int:
    return zlib.crc32(bytes(b)) & M32


def frame(generation, sections) -> bytes:
    """METADATA 2.4's frame as lib/core/CardContainer's ContainerWriter lays
    it out, by tools/mpwf.py's writer of it: the 40-byte header (magic MPST,
    major 1, minor 0, headerBytes 40, cardId 0: the stats are the card's
    because they are on it), the directory, then the sections [(type,
    stride, bytes)] in order, each REQUIRED, each at the previous one's end
    rounded up to 8, the header's CRC last. (card_fixtures.build_container
    makes the same bytes: test_statsref checks it.)"""
    return mpwf.frame(generation, 0, b"", [(t, REQUIRED, len(d) // stride if stride else 0, stride, d)
                                           for t, stride, d in sections], HEADER_BYTES, MAJOR, MINOR, MAGIC)


def header_crc(data) -> int:
    return struct.unpack_from("<I", data, 32)[0]


def head_bytes(h) -> bytes:
    return HEAD_LAYOUT.pack(h["plays"], h["skips"], h["listenedMs"], h["slots"],
                            *[v for name in SOURCES for v in h[name]],
                            h["undated"], h["sinceDay"], h["flags"], h["round"], h["baseGen"], h["baseRows"],
                            h["baseCrc"], h["twice"], h["gap"])


def rows_bytes(rows) -> bytes:
    """TRKS's (TRKD's) rows, strictly increasing by hash."""
    return b"".join(TRKS_ROW.pack(h, r.plays, r.skips, r.firstAt, r.lastAt, r.lastOrd | r.src << 24)
                    for h, r in sorted(rows.items()))


def write_stats(st: Stats) -> bytes:
    """stats.bin, generation st.generation: HEAD, MARK, HOUR, DAYS, RCNT,
    TRKD, every one REQUIRED, all little-endian, each array strictly
    increasing by its key."""
    mark = b"".join(MARK_ROW.pack(d, 0, e, s, 0) for (d, e), s in sorted(st.mark.items()))
    hour = b"".join(HOUR_ROW.pack(k | v[0] << 24, v[1]) for k, v in sorted(st.hour.items()))
    days = b"".join(DAYS_ROW.pack(k, v[0], v[1], v[2], v[3], v[4], v[5], v[6]) for k, v in sorted(st.days.items()))
    rcnt = b"".join(RCNT_ROW.pack(d, n, h) for (d, h), n in sorted(st.rcnt.items()))
    return frame(st.generation, [(HEAD, 0, head_bytes(st.head)), (MARK, 24, mark), (HOUR, 8, hour),
                                 (DAYS, 16, days), (RCNT, 12, rcnt), (TRKD, 24, rows_bytes(st.trkd))])


def write_base(rows, gen) -> bytes:
    """stats-<gen>.bin (5.3): the same frame, generation gen, one REQUIRED
    section TRKS of the rows, strictly increasing by hash."""
    return frame(gen, [(TRKS, 24, rows_bytes(rows))])


class Bad(Exception):
    """A file that fails a check: why is CardContainer's Why, by name."""

    def __init__(self, why):
        super().__init__(why)
        self.why = why


def open_frame(b, known):
    """Container::open's checks, in its order (lib/core/CardContainer.cpp),
    then each known section's CRC (SectionCrc). known: ((type, stride),
    ...), every one required. {type: (stride, bytes)}, or Bad."""
    if len(b) < HEADER_BYTES:
        raise Bad("Short")
    magic, major, _minor, hb, n, fb, gen, _card, hcrc, _ = COMMON.unpack_from(b, 0)
    if magic != MAGIC:
        raise Bad("Magic")
    if major != MAJOR:
        raise Bad("Major")
    if hb % 8 or hb < HEADER_BYTES:
        raise Bad("HeaderBytes")
    if n > MAX_SECTIONS:
        raise Bad("SectionCount")
    dir_end = hb + DIR_ENTRY.size * n
    if dir_end > len(b):
        raise Bad("Short")
    if fb != len(b):
        raise Bad("FileBytes")
    if crc32(b[:32] + bytes(4) + b[36:dir_end]) != hcrc:
        raise Bad("HeaderCrc")
    strides = dict(known)
    end, seen, found = dir_end, [], {}
    for i in range(n):
        t, flags, off, nb, count, stride, crc, _ = DIR_ENTRY.unpack_from(b, hb + DIR_ENTRY.size * i)
        if off % 8 or off < end or off - end >= 8 or off + nb > fb or t in seen:
            raise Bad("Layout")
        seen.append(t)
        end = off + nb
        if t not in strides:
            if flags & REQUIRED:
                raise Bad("UnknownRequired")
            continue  # a newer minor's OPTIONAL section: skipped (5.12)
        if strides[t] == 0:
            if count or stride:
                raise Bad("Shape")
        elif stride < strides[t] or count * stride != nb:
            raise Bad("Shape")
        found[t] = (stride, b[off:off + nb], crc)
    if end != fb:
        raise Bad("Layout")
    if any(t not in found for t, _ in known):
        raise Bad("Missing")
    for t, (_, data, crc) in found.items():
        if crc32(data) != crc:
            raise Bad("SectionCrc")
    return gen, {t: (stride, data) for t, (stride, data, _) in found.items()}


def rows_of(stride, data, layout):
    """An array's rows by their first-minor prefix (a newer minor's longer
    rows keep this minor's fields first: 5.12)."""
    return [layout.unpack_from(data, i) for i in range(0, len(data), stride)]


def increasing(keys):
    if any(keys[i] >= keys[i + 1] for i in range(len(keys) - 1)):
        raise Bad("Order")


def read_rows(stride, data):
    rows = {}
    for h, p, s, f, last, w in rows_of(stride, data, TRKS_ROW):
        rows[h] = Row(p, s, f, last, w & M24, w >> 24 & 3)
    increasing([r[0] for r in rows_of(stride, data, TRKS_ROW)])
    return rows


def read_stats(b) -> Stats:
    """stats.bin as 5.4 reads it, or Bad: its frame, each section's CRC,
    HEAD at least 128 bytes (Shape) and every array's rows in order
    (Order). The base isn't read here (load())."""
    gen, secs = open_frame(b, STATS_SECTIONS)
    if len(secs[HEAD][1]) < HEAD_BYTES:
        raise Bad("Shape")
    st = Stats()
    st.generation = gen
    v = HEAD_LAYOUT.unpack_from(secs[HEAD][1], 0)
    h = st.head
    h["plays"], h["skips"], h["listenedMs"], h["slots"] = v[0:4]
    for i, name in enumerate(SOURCES):
        h[name] = list(v[4 + 3 * i:7 + 3 * i])
    (h["undated"], h["sinceDay"], h["flags"], h["round"], h["baseGen"], h["baseRows"], h["baseCrc"], h["twice"],
     h["gap"]) = v[13:]
    marks = rows_of(*secs[MARK], MARK_ROW)
    increasing([(d, e) for d, _, e, _, _ in marks])
    st.mark = {(d, e): s for d, _, e, s, _ in marks}
    hours = rows_of(*secs[HOUR], HOUR_ROW)
    increasing([w & M24 for w, _ in hours])
    st.hour = {w & M24: [w >> 24, ms] for w, ms in hours}
    days = rows_of(*secs[DAYS], DAYS_ROW)
    increasing([r[0] for r in days])
    st.days = {r[0]: list(r[1:]) for r in days}
    rc = rows_of(*secs[RCNT], RCNT_ROW)
    increasing([(d, hh) for d, _, hh in rc])
    st.rcnt = {(d, hh): n for d, n, hh in rc}
    st.trkd = read_rows(*secs[TRKD])
    return st


def read_base(b, gen=None):
    """A base file's rows ({pathHash: Row}), or Bad; with gen, its
    generation must be gen ("Counts" otherwise)."""
    g, secs = open_frame(b, BASE_SECTIONS)
    if gen is not None and g != gen:
        raise Bad("Counts")
    return read_rows(*secs[TRKS])


def verdict(why) -> str:
    """5.4's verdict on a stats.bin's bytes: "loaded", "newer" (Major,
    UnknownRequired: stats off, the file left alone) or "corrupt" (any other
    check: renamed stats.bad, counting starts again). "unread" and "absent"
    are the card's answers, not the bytes'."""
    return "loaded" if why is None else "newer" if why in ("Major", "UnknownRequired") else "corrupt"


# ---------------------------------------------------------------------------
# A boot (5.4-5.6)
# ---------------------------------------------------------------------------
COMPACT_BYTES = 32 * 1024
COMPACT_FORCED = 96 * 1024
ROW_BYTES = 24
LOG_NAMES = playlog.NAMES               # the scan's order: plays.jnl, plays.1.jnl, then plays.2-9.jnl
# The records (5.14, S1's rework): the slots that count folded since the
# card's copy (or a held tmp) took them, all RAM keeps between saves. 40
# since the resident set moved to the device's internal RAM (5.14, "Moved
# to internal RAM": 2,048 B at most), the design's 96 before.
RECORDS = 40                            # the records a session holds; past them a boot's fold overflows
SAVE_AT = 24                            # the records' save is due from this many
BOOT_TRIES = 4                          # a boot overflow's save: the boot's and three more (tagstore::kBootWaitsMs)


def compaction_due(delta_rows, base_rows, playing, safe_point) -> bool:
    """5.3, at a boot's save: while nothing plays, when the delta is over
    32 KB or over a quarter of the base (any delta over no base); forced
    past 96 KB at a safe point (nothing playing, or the heard track with 20 s
    or more left and no seek in the last 2 s)."""
    d = delta_rows * ROW_BYTES
    if d > COMPACT_FORCED and (safe_point or not playing):
        return True
    return not playing and (d > COMPACT_BYTES or 4 * d > base_rows * ROW_BYTES)


def since_day(st: Stats, first_play_day, today, commit_time):
    """4.1: set once, at the first save that knows a date: the day of the
    earliest dated play folded, but not after today when today is known (a
    foreign slot dated 2099 is believable: 5.2); else today; else the
    root's commitTime's day (Unix seconds), flagged; else still unset."""
    h = st.head
    if h["sinceDay"] != UNSET_DAY:
        return
    if first_play_day is not None and today is not None:
        first_play_day = min(first_play_day, today)
    if first_play_day is not None:
        h["sinceDay"] = first_play_day
    elif today is not None:
        h["sinceDay"] = today
    elif commit_time is not None and believable(commit_time * 1000):
        h["sinceDay"] = day_of(commit_time * 1000)
        h["flags"] |= FLAG_SINCE_COMMIT


class Session:
    """One boot of a device that keeps stats, over a card's /.player files
    ({name: bytes}; changed in place as the device would change them) and
    its root's ACKS ([(deviceId hex, epoch, seq)]).

    The read (5.4) on construction; scan() is the boot's fold; save() the
    commit (5.5); trim() the log's trim; live() a play written this
    session. `read` says what the read found: "loaded", "absent",
    "corrupt" (renamed stats.bad, then absent), "newer" (stats off: no
    fold, no save, no trim), "unread" (the base it names doesn't read
    whole: shorter than its header, or the header stats.bin names over
    rows that don't read; a card fault, so stats off as for a read the
    card failed).

    unread: {log name: the slots the scan read of it before the card
    failed a read, or None when its open failed}: 5.6's partial scan. The
    scan reads that much, folds it, and goes on; no save runs this session
    (the boot's, a rotation's, Hs); the trim keeps those files and follows
    the MARK read at the boot.

    once: a remount's read (5.4): one read, so a stats.bin that fails its
    checks is "unread" (stats off), never renamed stats.bad on one read.

    The records (5.14): `records` counts the slots that count folded since
    the card's copy (or a held tmp) took them, the boot's fold's and the
    live ones'; a save that lands, or is held, empties them. A live slot
    with the records full (RECORDS) isn't folded, and the session is
    `behind` its log from then on: no record, no save (the next boot folds
    the log above the card's MARK, which the trim keeps), and the records
    it held count for nothing (0). The read settles
    a stats.tmp first, as tagstore::settle() does (METADATA 2.12.6): beside
    stats.bin it is a leftover, removed; alone, promoted when it reads
    whole (a newer major's too), else removed."""

    def __init__(self, files, acks=(), commit_time=None, unread=None, once=False):
        self.files = files
        self.acks = list(acks)
        self.commit_time = commit_time
        self.unread = dict(unread or {})
        self.once = once
        self.partial = False            # a log file not read whole (5.6): no save this session
        self.keys = []                  # (deviceId bytes, epoch, seq) of each slot the boot's fold took, in order
        self.removed = []
        self.read, self.why = self._load()
        self.on = self.read in ("loaded", "absent", "corrupt")
        self.mark_at_boot = dict(self.stats.mark)
        self.round = self.stats.head["round"]
        self.used = None                # the highest round used this session
        self.first_play_day = None      # the earliest dated play folded this session (sinceDay)
        self.new = False                # folded, or MARK raised, since the card's copy
        self.saved_mark = dict(self.stats.mark)  # the MARK on the card
        self.folded = self.twice = self.gap = 0
        self.compacted = False
        self.wrote = False              # stats.bin written this session
        self.records = 0                # slots folded since the card's copy (or a held tmp) took them (5.14)
        self.behind = False             # RAM lacks a slot the log holds above the card's MARK: no record, no save

    def _settle(self):
        """stats.tmp at the read (tagstore::settle(), the cut-rename rule
        with StatsCheck): beside stats.bin a leftover (the two never share
        a chain here), removed; alone, renamed stats.bin when it reads whole
        (its frame and every section's CRC; a newer major's is whole too),
        else removed (torn)."""
        tmp = self.files.get(STATS_TMP)
        if tmp is None:
            return
        whole = STATS_BIN not in self.files
        if whole:
            try:
                open_frame(tmp, STATS_SECTIONS)
            except Bad as e:
                whole = verdict(e.why) == "newer"
        if whole:
            self.files[STATS_BIN] = self.files.pop(STATS_TMP)
        else:
            del self.files[STATS_TMP]

    def _load(self):
        self.stats = Stats()
        self.card_base = 0              # the base the card's stats.bin names, whole or lost (0: none)
        self._settle()
        data = self.files.get(STATS_BIN)
        if data is None:
            return "absent", None
        try:
            st = read_stats(data)
        except Bad as e:
            if verdict(e.why) == "newer":
                return "newer", e.why
            if self.once:
                return "unread", e.why
            self.files.pop(STATS_BAD, None)
            self.files[STATS_BAD] = self.files.pop(STATS_BIN)
            return "corrupt", e.why
        h = st.head
        self.card_base = h["baseGen"]
        if h["baseGen"]:
            name = base_name(h["baseGen"])
            b = self.files.get(name)
            if b is not None and len(b) < HEADER_BYTES:
                # Its header doesn't read whole: a card fault, never a lost
                # base (5.4, step 4): stats off.
                return "unread", "Short"
            if b is None or header_crc(b) != h["baseCrc"]:
                # Gone, or another file: the rows before it are lost, the
                # totals stay; no stats-<n>.bin is removed on this boot.
                h["flags"] |= FLAG_BASE_LOST
                h["baseGen"] = h["baseRows"] = h["baseCrc"] = 0
            else:
                try:
                    st.base = read_base(b, h["baseGen"])
                except Bad as e:
                    # Its header the one named, its rows unreadable: a card
                    # fault, never a lost base (5.4): stats off.
                    return "unread", e.why
                if len(st.base) != h["baseRows"]:
                    return "unread", "Counts"
                for g in (h["baseGen"] - 1, h["baseGen"] + 1):
                    if g > 0 and base_name(g) in self.files:
                        del self.files[base_name(g)]
                        self.removed.append(base_name(g))
        self.stats = st
        return "loaded", None

    # The boot's fold.
    def scan(self):
        """Every log file in the scan's order (plays.jnl, plays.1.jnl,
        plays.2-9.jnl), each to its cap: each slot that counts, above the
        MARK read at the boot for its (deviceId, epoch), folded once at
        round HEAD.round + 1 from every deviceId (O2). Each slot once: a
        key (deviceId, epoch, seq) already read in this scan is skipped,
        the first read kept (a conflict too), and counted in HEAD's twice
        when another file gave it. Drop notes and slots that don't count are
        skipped. Then the gap: for each ACKS row above MARK, the seqs it
        covers that no valid play slot of the log holds (a firmware without
        stats trimmed them), counted in HEAD's gap and covered by MARK from
        now on, so a later boot never counts them again. A partial scan
        counts no gap: the slots it didn't read may hold those seqs (and
        nothing it folds is saved)."""
        if not self.on:
            return
        st, mark = self.stats, self.mark_at_boot
        rnd = sat(self.round + 1, M24)
        seen, held = {}, {}
        for name in LOG_NAMES:
            if name not in self.files:
                continue
            data = self.files[name]
            if name in self.unread:
                self.partial = True
                data = data[:(self.unread[name] or 0) * playlog.SLOT]
            for _, why, s, _ in playlog.read_file(data):
                if why != "ok" or s["kind"] != playlog.KIND_PLAY:
                    continue
                dev = playlog.device_bytes(s["deviceId"])
                held.setdefault((dev, s["epoch"]), set()).add(s["seq"])
                if not counts(s) or s["seq"] <= mark.get((dev, s["epoch"]), 0):
                    continue
                k = (dev, s["epoch"], s["seq"])
                if k in seen:
                    if seen[k] != name:
                        self.twice += 1
                    continue
                seen[k] = name
                self.keys.append(k)
                self.fold(s, rnd)
        for d, e, a in () if self.partial else self.acks:
            k = (playlog.device_bytes(d), playlog.u64(e))
            m = mark.get(k, 0)
            if a > m:
                self.gap += sum(1 for q in range(m + 1, a + 1) if q not in held.get(k, ()))
                st.mark[k] = max(st.mark.get(k, 0), a)
        h = st.head
        h["twice"] = sat(h["twice"] + self.twice, M32)
        h["gap"] = sat(h["gap"] + self.gap, M32)
        self.new = self.new or st.mark != mark

    def fold(self, s, rnd):
        """One slot that counts into the stats, and what the save needs of
        it: the round used, the earliest dated play (sinceDay)."""
        self.stats.fold(s, rnd)
        self.folded += 1
        self.records += 1
        self.used = rnd if self.used is None else max(self.used, rnd)
        self.new = True
        if is_play(s) and dated(s):
            d = day_of(s["startedAt"])
            self.first_play_day = d if self.first_play_day is None else min(self.first_play_day, d)

    def live(self, s):
        """A play written this session (Outbox::done(), Written only): folded
        at round HEAD.round + 2, in RAM only, a record (5.14). Not while the
        session is behind; with the records full (RECORDS: a save asked
        failed or was refused, or waits) the slot isn't folded and the
        session is behind from then on. True when folded."""
        if not self.on or not counts(s) or self.behind:
            return False
        if self.records >= RECORDS:
            self.fall_behind()
            return False
        self.fold(s, sat(self.round + 2, M24))
        return True

    def fall_behind(self):
        """The session lacks a slot the log holds above the card's MARK:
        behind for the rest of it (5.14). No save takes the records any
        more (the next boot folds the log again): none are kept."""
        self.behind = True
        self.records = 0

    def next_seq(self, nvs_seq, device_id, epoch):
        """The seq rule (5.6, METADATA 2.20.4) for a device that keeps stats,
        after the boot's scan: max(NVS's, the log's, ACKS's and MARK's for
        this device and epoch) + 1 (playlog.boot_seq, given this session's
        MARK). The log's files as they are: a trim takes only slots MARK
        and ACKS cover, so before it or after, the same."""
        mark = self.stats.mark.get((playlog.device_bytes(device_id), playlog.u64(epoch)), 0)
        return playlog.boot_seq(nvs_seq, playlog.valid_slots({n: b for n, b in self.files.items() if n in LOG_NAMES}),
                                device_id, epoch, self.acks, mark)

    # The save.
    def save(self, now=None, why="boot", playing=False, safe_point=True, fails=False, held=False):
        """5.5: True when the card's stats.bin is now this session's (written,
        or nothing new to write); False when skipped or failed (fails: a full
        or failing card; every save of a session whose scan was partial). A
        boot's save may compact (5.3); no other does (a rotation's, the
        records', Hs). Writes stats.bin (generation + 1, retention applied,
        sinceDay set once), a new base when compacting, and removes the old
        base. The new base's name is one stats.bin doesn't name: a stale
        file there (a stats.bin deleted or corrupt, or a base lost, left it)
        is replaced. A compaction whose name the card's stats.bin names
        (after a lost base 1, baseGen starts again at 0: its next is 1)
        waits for the next boot: a cut after its create would leave a base
        shorter than its header under the name stats.bin names, which every
        boot reads as unread (5.3). A save that writes empties the records.

        held: replace() fails with the tmp written whole (5.14's Held): the
        bytes go to stats.tmp, and stats.bin, the old base, the card's MARK
        (saved_mark) and the base it names (card_base) stay the ones before;
        RAM takes the plan as at a landing (HEAD, the generation, the
        records emptied), so the next save merges only the newer records
        onto the held copy once it is promoted. False (the card's stats.bin
        doesn't hold them)."""
        if not self.on or fails or self.partial:
            return False
        if not self.new:
            return True
        st, h = self.stats, self.stats.head
        today = today_of(now)
        if self.used is not None:
            h["round"] = max(h["round"], self.used)
        since_day(st, self.first_play_day, today, self.commit_time)
        retain(st, today)
        old_base = base_name(h["baseGen"]) if h["baseGen"] else None
        self.compacted = (why == "boot" and compaction_due(len(st.trkd), h["baseRows"], playing, safe_point)
                          and sat(h["baseGen"] + 1, M32) != self.card_base)
        if self.compacted:
            st.base = st.tracks()
            st.trkd = {}
            h["baseGen"] = sat(h["baseGen"] + 1, M32)
            base = write_base(st.base, h["baseGen"])
            h["baseRows"], h["baseCrc"] = len(st.base), header_crc(base)
            self.files[base_name(h["baseGen"])] = base
        st.generation = sat(st.generation + 1, M32)
        self.new = False
        self.records = 0
        if held:
            self.files[STATS_TMP] = write_stats(st)
            return False
        self.files[STATS_BIN] = write_stats(st)
        if self.compacted and old_base and old_base in self.files:
            del self.files[old_base]
            self.removed.append(old_base)
        self.saved_mark = dict(st.mark)
        self.card_base = h["baseGen"]
        self.wrote = True
        return True

    # The trim.
    def trim(self, saved):
        """2.20.2's trim with 5.5's rule: with ACKS, a log file goes when
        today's rule holds (playlog.trimmable: every play acknowledged, at
        least one, a play last, nothing it can't judge) and every play slot
        in it is at or under the card's MARK: the saved one after a save
        that succeeded, else the one read at the boot. Stats off (newer):
        nothing. A file the scan didn't read whole (unread) is kept, as
        today's scan keeps one it couldn't judge. The order: plays.1.jnl to
        plays.9.jnl, then plays.jnl. The names trimmed."""
        if not self.on or not self.acks:
            return []
        cover = Stats()
        cover.mark = self.saved_mark if saved else self.mark_at_boot
        out = []
        for name in LOG_NAMES[1:] + LOG_NAMES[:1]:
            data = self.files.get(name)
            if data is None or name in self.unread or not playlog.trimmable(data, self.acks):
                continue
            plays = [s for _, why, s, _ in playlog.read_file(data) if why == "ok" and s["kind"] == playlog.KIND_PLAY]
            if all(cover.covered(s) for s in plays):
                del self.files[name]
                out.append(name)
        return out


def boot(files, acks=(), now=None, commit_time=None, playing=False, safe_point=True, save_fails=False, unread=None):
    """A whole boot over a copy of a card's /.player (files: {name: bytes};
    a new dict comes back in the result): the read, the scan's fold, the
    boot's save (one try: a session's boot tries an overflow's again,
    _session_boot), the trim. now: the board's clock at the save (Unix ms, or
    None while unset). unread: the scan's card faults (Session's). The
    result: {"files", "stats", "read", "why", "folded", "twice", "gap",
    "partial", "saved" (stats.bin written), "compacted", "removed",
    "trimmed"}."""
    files = dict(files)
    se = Session(files, acks, commit_time, unread)
    se.scan()
    ok = se.save(now, "boot", playing, safe_point, save_fails)
    trimmed = se.trim(ok)
    return {"files": files, "stats": se.stats, "session": se, "read": se.read, "why": se.why, "folded": se.folded,
            "twice": se.twice, "gap": se.gap, "partial": se.partial, "saved": se.wrote,
            "compacted": se.compacted, "removed": se.removed, "trimmed": trimmed}


def load(files):
    """The stats a card's /.player holds (stats.bin and the base it names),
    read as a boot reads them, nothing changed: a Stats, or None when there
    is none, or it can't be read."""
    se = Session(dict(files))
    return se.stats if se.read == "loaded" else None


# ---------------------------------------------------------------------------
# The windows (4.1, 4.2, 7.6) and the shelves' rankers (4.3)
# ---------------------------------------------------------------------------
CAPS = {"mostPlayed": 50, "newFinds": 50, "oldFavourites": 200, "recentlyPlayed": 200}
PAST_CAP = 16                           # the rankers keep 16 rows past each cap, for orphans (6.6)
OLD_FAVOURITE = 5                       # plays at least


def window(today, days):
    """The days of a window, (first, last): today and the days - 1 before;
    None while the clock is unset. days None: All time, (None, None)."""
    if days is None:
        return None, None
    return None if today is None else (today - days + 1, today)


def counting_days(st: Stats, today):
    """today - sinceDay + 1, at least 1; None while either is unknown."""
    s = st.head["sinceDay"]
    return None if today is None or s == UNSET_DAY else max(1, today - s + 1)


def windows_offered(days_counted):
    """Most played's tray (7.6): no tray under 7 counting days or with no
    dates; 7 days and All time from 7; 7 days, 30 days and All time from
    30. The windows' lengths, None for All time."""
    if days_counted is None or days_counted < 7:
        return []
    return [7, None] if days_counted < 30 else [7, 30, None]


def most_played(st: Stats, today, days=None, rows=None):
    """[(pathHash, plays in the window)], at least 1: plays, most first; then
    the latest play (RCNT's day in the window, or lastAt for All time),
    newest first; then the hash. The cap and 16 more. None for a window
    while the clock is unset."""
    cap = CAPS["mostPlayed"] + PAST_CAP
    if days is None:
        rows = st.tracks() if rows is None else rows
        items = sorted((-r.plays, -r.lastAt, h) for h, r in rows.items() if r.plays >= 1)
        return [(h, -p) for p, _, h in items[:cap]]
    w = window(today, days)
    if w is None:
        return None
    plays, latest = {}, {}
    for (day, h), n in st.rcnt.items():
        if w[0] <= day <= w[1]:
            plays[h] = plays.get(h, 0) + n
            latest[h] = max(latest.get(h, day), day)
    items = sorted((-n, -latest[h], h) for h, n in plays.items())
    return [(h, -p) for p, _, h in items[:cap]]


def dated_time(t) -> bool:
    return t not in (0, NO_FIRST)


def new_finds(st: Stats, today, rows=None, capped=True):
    """[(pathHash, plays, firstAt, its source)]: firstAt dated and inside the
    30-day window. Plays, most first; then firstAt, newest first; then the
    hash. None while the clock is unset."""
    w = window(today, 30)
    if w is None:
        return None
    rows = st.tracks() if rows is None else rows
    items = sorted((-r.plays, -r.firstAt, h, r.src) for h, r in rows.items()
                   if dated_time(r.firstAt) and w[0] <= day_of_time(r.firstAt) <= w[1])
    items = items[:CAPS["newFinds"] + PAST_CAP] if capped else items
    return [(h, -p, -f, s) for p, f, h, s in items]


def old_favourites(st: Stats, today, rows=None, capped=True):
    """[(pathHash, plays, lastAt)]: plays at least 5, lastAt dated and before
    the 30-day window. Plays; then lastAt, newest first; then the hash. None
    while the clock is unset."""
    w = window(today, 30)
    if w is None:
        return None
    rows = st.tracks() if rows is None else rows
    items = sorted((-r.plays, -r.lastAt, h) for h, r in rows.items()
                   if r.plays >= OLD_FAVOURITE and r.lastAt != 0 and day_of_time(r.lastAt) < w[0])
    items = items[:CAPS["oldFavourites"] + PAST_CAP] if capped else items
    return [(h, -p, -t) for p, t, h in items]


def live_round_of(st: Stats):
    """The round a session that boots on this card gives its live plays
    (Fold::liveRound(): HEAD's round as it loads it, + 2, saturating). No
    row of the card has it: a save writes HEAD's round at least each row's
    lastOrd."""
    return sat(st.head["round"] + 2, M24)


def recently_played(st: Stats, rows=None, live_round=None, dated=True):
    """[(pathHash, plays, lastAt, lastOrd)]: plays at least 1. Dated (the
    clock set): lastAt, newest first; then the rows with no dated play by
    lastOrd, newest first; then the hash. With the clock unset (dated False;
    live_round the ranking session's Fold::liveRound(), None or 0: none)
    this session's rows first: lastOrd the live round, whatever lastAt (an
    undated play leaves lastAt, 5.9; the owner's call, 2026-11-24: 4.3's
    "This session", then "Earlier"). Then, within each part, lastOrd,
    newest first (every play sets it, dated or not: with no time, the one
    order every row has, so the last no-clock session's never-dated plays
    lead Earlier, not fall past 216 dated rows); then lastAt, newest first
    (no dated play last); then the hash."""
    rows = st.tracks() if rows is None else rows

    def key(h, r):
        if dated:
            return (1, (0, -r.lastAt) if r.lastAt else (1, -r.lastOrd), h)
        session = bool(live_round) and r.lastOrd == live_round
        return (0 if session else 1, (-r.lastOrd, -r.lastAt), h)

    items = sorted(key(h, r) for h, r in rows.items() if r.plays >= 1)
    items = items[:CAPS["recentlyPlayed"] + PAST_CAP]
    return [(h, rows[h].plays, rows[h].lastAt, rows[h].lastOrd) for _, _, h in items]


def not_played(st: Stats, albums, rows=None):
    """Not played yet: the albums [(name, [pathHash...])], in the library's
    A-Z order, with no track played (a track only skipped is not played)."""
    rows = st.tracks() if rows is None else rows
    return [name for name, hashes in albums if not any(h in rows and rows[h].plays >= 1 for h in hashes)]


def lists(st: Stats, today, albums=None, live_round=None):
    """Every shelf's list (4.3) as the rankers give it: hashes, no names.
    With the clock unset (today None) Recently played puts the ranking
    session's rows first, then the rest by lastOrd (recently_played()):
    live_round its Fold::liveRound(), by default a session that boots on
    the card (live_round_of(): no row is its)."""
    rows = st.tracks()
    live = None if today is not None else live_round_of(st) if live_round is None else live_round
    out = {"mostPlayed": {str(d or "all"): most_played(st, today, d, rows) for d in (7, 30, None)},
           "newFinds": new_finds(st, today, rows), "oldFavourites": old_favourites(st, today, rows),
           "recentlyPlayed": recently_played(st, rows, live, dated=today is not None)}
    if albums is not None:
        out["notPlayed"] = not_played(st, albums, rows)
    return out


def week(st: Stats, today):
    """The last 7 days (4.2): each UTC day's time listened and plays from
    HOUR, oldest first; their sums; the top day by time listened (a tie
    goes to the later day; None with nothing listened); Auto DJ's plays and
    skips from DAYS. None while the clock is unset."""
    w = window(today, 7)
    if w is None:
        return None
    by_day = {d: [0, 0] for d in range(w[0], w[1] + 1)}
    for hr, (plays, ms) in st.hour.items():
        d = hr // 24
        if d in by_day:
            by_day[d][0] += ms
            by_day[d][1] += plays
    days = [(d, by_day[d][0], by_day[d][1]) for d in sorted(by_day)]
    top = max(days, key=lambda x: (x[1], x[0]))
    adj = [0, 0]
    for d, row in st.days.items():
        if w[0] <= d <= w[1]:
            adj[0] += row[2]
            adj[1] += row[3]
    return {"days": days, "listenedMs": sum(x[1] for x in days), "plays": sum(x[2] for x in days),
            "topDay": top[0] if top[1] else None, "autodj": adj}


def view(st: Stats, today, albums=None):
    """The hub's numbers (4.2): the lifetime time listened and sinceDay,
    counting days, the windows offered, the 7 days, the counts behind the
    hub's lines (New finds, Old favourites, Not played yet)."""
    rows = st.tracks()
    nf, of = new_finds(st, today, rows, False), old_favourites(st, today, rows, False)
    out = {"listenedMs": st.head["listenedMs"], "plays": st.head["plays"], "sinceDay": st.head["sinceDay"],
           "countingDays": counting_days(st, today), "windows": windows_offered(counting_days(st, today)),
           "week": week(st, today), "newFinds": None if nf is None else len(nf),
           "oldFavourites": None if of is None else len(of)}
    if albums is not None:
        out["notPlayed"] = len(not_played(st, albums, rows))
    return out


# ---------------------------------------------------------------------------
# What the visit's ask reads besides the lists (9.7's S3a, V1): the totals,
# the 7 days and their span, the u40 played set
# ---------------------------------------------------------------------------
def totals(st: Stats, today, rows=None):
    """Every list's offers before its cap (Ranker::total(); hub rows 3-4
    count New finds' and Old favourites', 4.2 as decided): the rows its rule
    takes. Most played's windows: the hashes with a play in the window
    (RCNT's); All time and Recently played: the tracks with a play. None for
    a list that needs the clock while it is unset. Same shape as lists()."""
    rows = st.tracks() if rows is None else rows

    def in_window(days):
        w = window(today, days)
        return None if w is None else len({h for (d, h), n in st.rcnt.items() if n and w[0] <= d <= w[1]})

    nf, of = new_finds(st, today, rows, False), old_favourites(st, today, rows, False)
    played = sum(1 for r in rows.values() if r.plays >= 1)
    return {"mostPlayed": {"7": in_window(7), "30": in_window(30), "all": played},
            "newFinds": None if nf is None else len(nf), "oldFavourites": None if of is None else len(of),
            "recentlyPlayed": played}


def days7(st: Stats, today):
    """The last 7 days from DAYS (4.2 as decided: hub row 1 and Week by UTC
    day, HOUR not read), oldest first: each day's [listenedMs, plays, Auto
    DJ's plays, Auto DJ's skips], a day with no row all 0; None while the
    clock is unset. A play counts whole on the day it began (4.1), one that
    runs past a UTC midnight too. And the span, clock or not: DAYS' first
    and newest day (None: no row) and spanDays, newest - first + 1 (0 with
    no row: 3.1's datedSpanDays, O5); RCNT's oldest day (rcntFirstDay, None:
    no row), which a capped RCNT moves inside the 30 days (7.6's
    stats.days_n)."""
    w = window(today, 7)
    days = None
    if w is not None:
        days = []
        for d in range(w[0], w[1] + 1):
            r = st.days.get(d)
            days.append([0, 0, 0, 0] if r is None else [r[4], r[0], r[2], r[3]])
    first = min(st.days) if st.days else None
    newest = max(st.days) if st.days else None
    return {"days": days, "firstDay": first, "newestDay": newest, "spanDays": newest - first + 1 if st.days else 0,
            "rcntFirstDay": min(d for d, _ in st.rcnt) if st.rcnt else None}


def played40(st: Stats, rows=None):
    """Not played yet's played set (call 3, "5-byte fingerprints", 4.3): the
    top 40 bits of each played hash, in the hashes' ascending order (their
    top bits ascend with them: no sort). Two played hashes that share their
    top 40 bits give two equal keys."""
    rows = st.tracks() if rows is None else rows
    return [h >> 24 for h in sorted(h for h, r in rows.items() if r.plays >= 1)]


def played40_bytes(keys) -> bytes:
    """The u40 set as the rank's region holds it (statsrank::Played40): 5 B
    a key, little-endian, in order."""
    return b"".join(k.to_bytes(5, "little") for k in keys)


# ---------------------------------------------------------------------------
# JSON, for the fixtures and the command line
# ---------------------------------------------------------------------------
def hx(h) -> str:
    return "0x%016x" % h


def stats_json(st: Stats, rows=False):
    """The stats as JSON: HEAD, MARK, and each section's rows (rows=True) or
    their counts."""
    out = {"generation": st.generation, "head": dict(st.head),
           "mark": [[d.hex(), "0x%016x" % e, s] for (d, e), s in sorted(st.mark.items())]}
    if rows:
        out.update(hour=[[k, v[0], v[1]] for k, v in sorted(st.hour.items())],
                   days=[[k] + v for k, v in sorted(st.days.items())],
                   rcnt=[[d, hx(h), n] for (d, h), n in sorted(st.rcnt.items())],
                   trkd=[[hx(h)] + list(r.tuple()) for h, r in sorted(st.trkd.items())],
                   base=[[hx(h)] + list(r.tuple()) for h, r in sorted(st.base.items())])
    else:
        out["rows"] = {"hour": len(st.hour), "days": len(st.days), "rcnt": len(st.rcnt), "trkd": len(st.trkd),
                       "base": len(st.base)}
    return out


def lists_json(ls):
    def conv(v):
        if v is None:
            return None
        return [[hx(x[0])] + list(x[1:]) if isinstance(x, tuple) else x for x in v]
    out = {k: conv(v) for k, v in ls.items() if k != "mostPlayed"}
    out["mostPlayed"] = {k: conv(v) for k, v in ls["mostPlayed"].items()}
    return out


# ---------------------------------------------------------------------------
# The fixtures (test/fixtures/card/stats): made-up names, made-up devices
# ---------------------------------------------------------------------------
DEV_A, DEV_B, DEV_C = "00005e005301", "00005e005302", "00005e005303"   # RFC 7042's documentation MACs
EPOCH_A, EPOCH_A2, EPOCH_B, EPOCH_C = 0x1122334455667788, 0x2233445566778899, 0x0a0b0c0d0e0f1011, 0x5a5b5c5d5e5f6061
path_hash = mpwf.path_hash             # METADATA 2.3.3: FNV-1a 64 of "/music/" and the path below it

# The library behind the fixtures' Not played yet: albums in A-Z order
# (lib/core/LibraryIndex albumsAZ()), each its tracks' paths under /music.
LIBRARY = [
    ("Ash Meridian", "Copper Season", ["01 Copper Season.mp3", "02 Brine.mp3", "03 Low Tide.mp3"]),
    ("Halvard Moss", "Quiet Engine", ["01 Quiet Engine.flac", "02 Signal Fires.flac"]),
    ("Ilse Varga", "Paper Comets", ["01 Paper Comets.mp3", "02 Orbit Room.mp3", "03 Glass Ladder.mp3"]),
    ("Lumen Ferry", "Night Ticket", ["01 Night Ticket.mp3", "02 Slow Lantern.mp3", "03 Harbour Lights.mp3"]),
    ("Mira Okafor", "Tidewater Rooms", ["01 Tidewater.opus", "02 Salt Stairs.opus"]),
    ("Sefa Ilmar", "Glass Harbour", ["01 Glass Harbour.mp3", "02 Weather Vane.mp3", "03 North Pier.mp3"]),
]


def library_albums():
    """[(album, [pathHash...])] in A-Z order, and {name: (path, hash)} of
    every track by its file's title."""
    albums, tracks = [], {}
    for artist, album, files in sorted(LIBRARY, key=lambda a: a[1].casefold()):
        hs = []
        for f in files:
            rel = "%s/%s/%s" % (artist, album, f)
            h = path_hash(rel)
            hs.append(h)
            tracks[f.split(" ", 1)[1].rsplit(".", 1)[0]] = (rel, h)
        albums.append((album, hs))
    return albums, tracks


ALBUMS, TRACKS = library_albums()


def ms_of(iso) -> int:
    """'2026-11-08T21:40:00' (UTC) as Unix ms."""
    import datetime
    t = datetime.datetime.strptime(iso, "%Y-%m-%dT%H:%M:%S").replace(tzinfo=datetime.timezone.utc)
    return int(t.timestamp()) * 1000


def slot(dev, epoch, seq, track, played, duration, outcome=1, src=1, at=None, boot=1, up=0, **kw):
    """A play slot of the fixture library (track: a title of TRACKS, or a
    path hash), dated at `at` (ISO UTC, or None: undated, clock 0)."""
    h = TRACKS[track][1] if isinstance(track, str) else track
    s = {"kind": playlog.KIND_PLAY, "deviceId": dev, "epoch": epoch, "seq": seq, "outcome": outcome,
         "source": src, "clock": 1 if at else 0, "startedAt": ms_of(at) if at else 0, "bootSeq": boot,
         "uptimeMs": up or seq * 1000, "playedMs": played, "durationMs": duration, "pathHash": h,
         "size": 4000000 + (h & 0xFFFFF), "fatTime": 0x5B684A00, "generation": 1}
    s.update(kw)
    return s


def log(*slots) -> bytes:
    return b"".join(s if isinstance(s, (bytes, bytearray)) else playlog.encode(s) for s in slots)


def earlier(slots, now=None, compact=True):
    """The card's stats.bin (and base) as an earlier boot of the stats
    firmware left them after folding `slots`: {name: bytes}."""
    files = {}
    se = Session(files)
    for s in slots:
        if counts(s):
            se.fold(s, 1)
    se.save(now, "boot", playing=not compact)
    return files


DAY = "2026-11-%02dT%s"


def scenario_worked():
    """5.13, the worked example."""
    a = lambda seq, *args, **kw: slot(DEV_A, EPOCH_A, seq, *args, **kw)  # noqa: E731
    old = [a(q, ["Night Ticket", "Brine", "Glass Harbour", "Paper Comets"][q % 4], 200000, 200000,
             at=DAY % (1 + q // 8, "%02d:%02d:00" % (8 + q % 8, q % 60))) for q in range(1, 30)]
    p1 = [a(q, ["Night Ticket", "Brine", "Glass Harbour", "Paper Comets"][q % 4], 200000, 200000,
            at=DAY % (5 + (q - 30) // 6, "%02d:15:00" % (9 + (q - 30) % 6))) for q in range(30, 42)]
    p1 += [a(42, "Weather Vane", 120000, 200000, outcome=1, src=3, at="2026-11-08T21:40:00"),
           a(43, "Orbit Room", 12000, 240000, outcome=2, src=1, at="2026-11-08T21:44:00"),
           a(44, "Orbit Room", 25000, 40000, outcome=3, src=1, at="2026-11-08T21:45:00"),
           a(45, "Low Tide", 8000, 0, outcome=3, src=1, at="2026-11-08T21:46:00")]
    p0 = [a(46, "Weather Vane", 200000, 200000, src=1, at="2026-11-09T08:00:00"),
          a(47, "Salt Stairs", 180000, 180000, src=2, at="2026-11-09T08:04:00"),
          a(48, "Night Ticket", 210000, 210000, src=1, at="2026-11-09T08:08:00")]
    before = earlier(old + p1[:12], now=ms_of("2026-11-07T23:00:00"))
    before.update({"plays.1.jnl": log(*p1), "plays.jnl": log(*p0)})
    acks = [(DEV_A, EPOCH_A, 45)]
    now = ms_of("2026-11-10T07:30:00")
    return {"name": "worked-example",
            "note": "LISTENING-STATS.md 5.13: device A, epoch E; stats.bin's MARK (A, E) 41; plays.1.jnl seqs 30-45, "
                    "plays.jnl 46-48; ACKS (A, E) 45. 42 is X (Weather Vane) played 120 s of 200 s by Auto DJ at "
                    "2026-11-08T21:40Z (day 311, hour 7,485, time 26,948,401); 43 Y (Orbit Room) skipped at 12 s of "
                    "240 s; 44 Y stopped at 25 s of 40 s, a play (line 20 s); 45 durationMs 0, 8 s: neither; 46-48 "
                    "three plays. The first boot plays on through its save (no compaction): it folds 42-48, MARK "
                    "48, and trims plays.1.jnl (under MARK and ACKS); plays.jnl stays (over ACKS). The second finds "
                    "nothing above 48: no save, nothing trimmed.",
            "device": [DEV_A, EPOCH_A], "files": before,
            "boots": [dict(acks=acks, now=now, playing=True, safePoint=False),
                      dict(acks=acks, now=now + DAY_MS, playing=False, safePoint=True)]}


def scenario_rules():
    """4.1 on every kind of slot, from a fresh start (no stats.bin)."""
    a = lambda seq, *args, **kw: slot(DEV_A, EPOCH_A, seq, *args, **kw)  # noqa: E731
    t = lambda h: "2026-11-09T%02d:00:00" % h  # noqa: E731
    p1 = [a(1, "Copper Season", 20000, 40001, outcome=3, at=t(8)),       # half 20,000.5: under the line
          a(2, "Copper Season", 20001, 40001, outcome=3, at=t(8)),       # at the line: a play
          a(3, "Brine", 30000, 0, outcome=3, src=0, at=t(9)),            # durationMs 0: the line 30 s; source 0 is you
          a(4, "Brine", 29999, 0, outcome=2, src=4, at=t(9)),            # a skip; source 4 is you
          a(5, "Low Tide", 10000, 200000, outcome=1, at=t(10)),          # completed under the line: neither
          a(6, "Low Tide", 10000, 200000, outcome=0, at=t(10)),          # outcome 0 reads as stopped: neither
          a(7, "Low Tide", 10000, 200000, outcome=4, at=t(11)),          # outcome 4: neither
          a(8, "Quiet Engine", 31000, 100000, outcome=2, src=2, at=t(11)),  # skipped over the line: a play
          a(9, "Quiet Engine", 5000, 100000, outcome=2, src=3, at=t(12)),   # Auto DJ's skip
          a(10, "Signal Fires", 40000, 300000, src=3, at=None),          # undated (the clock unset)
          a(11, "Signal Fires", 40000, 300000, src=1, startedAt=ms_of("2025-12-31T23:59:59"), clock=2)]  # before 2026
    note = {"kind": playlog.KIND_DROP, "deviceId": DEV_A, "epoch": EPOCH_A, "clock": 1,
            "startedAt": ms_of(t(12)), "bootSeq": 1, "uptimeMs": 5, "dropped": 3}
    damaged = bytearray(playlog.encode(a(12, "Paper Comets", 200000, 200000, at=t(13))))
    damaged[50] ^= 0xFF                                                   # its CRC fails: skipped
    unusable = slot(DEV_A, 0, 1, "Paper Comets", 200000, 200000, at=t(13))  # epoch 0: no slot that counts
    newer = bytearray(playlog.encode(slot(DEV_C, EPOCH_C, 1, "Orbit Room", 200000, 200000, at=t(15))))
    newer[4] = 2                                                          # a newer major's slot
    p0 = [a(13, "Paper Comets", 200000, 200000, at=t(14)),
          slot(DEV_B, EPOCH_B, 1, "Orbit Room", 200000, 200000, startedAt=ms_of("2099-06-01T10:00:00"), clock=2),
          unusable, bytes(newer), a(14, "Glass Ladder", 200000, 200000, at=t(15))]
    files = {"plays.1.jnl": log(*p1, note, bytes(damaged), a(12, "Paper Comets", 200000, 200000, at=t(13))),
             "plays.jnl": log(*p0)}
    return {"name": "rules",
            "note": "4.1 on each kind of slot, no stats.bin before (a fresh start: every slot folded). plays.1.jnl: "
                    "the line at an odd durationMs (20,000 of 40,001 ms is under it, 20,001 a play), durationMs 0 "
                    "(30 s), a skip, completed and stopped under the line (neither), outcomes 0 and 4 (stopped), "
                    "sources 0 and 4 (you), a skip over the line (a play), Auto DJ's skip, an undated play and one "
                    "dated 2025 (undated), a drop note, then seq 12 damaged and again whole; its last slot a play, "
                    "ACKS covering it: trimmed. plays.jnl: device B's play dated 2099 (dated), a slot with epoch 0 "
                    "(not counted, never acknowledged) and device C's newer-major slot (not read): kept.",
            "device": [DEV_A, EPOCH_A], "files": files,
            "boots": [dict(acks=[(DEV_A, EPOCH_A, 14), (DEV_B, EPOCH_B, 1)], now=ms_of("2026-11-10T09:00:00"),
                           playing=False, safePoint=True)]}


def first_at_slots():
    a = lambda seq, *args, **kw: slot(DEV_A, EPOCH_A, seq, *args, **kw)  # noqa: E731
    b = lambda seq, *args, **kw: slot(DEV_B, EPOCH_B, seq, *args, **kw)  # noqa: E731
    xa = [a(1, "Harbour Lights", 200000, 200000, src=3, at=None),                     # undated, Auto DJ
          a(2, "Harbour Lights", 200000, 200000, src=2, at="2026-11-03T10:00:00"),    # dated, shuffle
          a(3, "Tidewater", 200000, 200000, src=3, at=None),                          # undated, Auto DJ
          a(4, "North Pier", 9000, 200000, outcome=2, at="2026-11-04T10:00:00")]      # a skip
    xb = [b(1, "Harbour Lights", 200000, 200000, src=1, at="2026-11-02T10:00:00"),    # earlier, you
          b(2, "Tidewater", 200000, 200000, src=2, at=None),                          # undated, shuffle
          b(3, "North Pier", 200000, 200000, src=1, at="2026-11-05T10:00:00")]        # a play after the skip
    return xa, xb


def scenario_first_at(order):
    xa, xb = first_at_slots()
    # Each file in seq order, as a device writes it (5.6's ranges rely on
    # it); the two orders swap the files, and the scan reads plays.jnl first.
    files = {"plays.1.jnl": log(*xa), "plays.jnl": log(*xb)} if order == "a" else \
        {"plays.1.jnl": log(*xb), "plays.jnl": log(*xa)}
    return {"name": "first-at-" + order,
            "note": "5.2's merge is order-free: the same seven slots of two devices, read %s (the scan reads "
                    "plays.jnl first). Harbour Lights: B's dated play is earlier than A's (firstAt B's, source "
                    "you); Tidewater: two undated plays, Auto DJ and shuffle (firstAt 0, the smaller source: "
                    "shuffle); North Pier: a skip, then a play (firstAt the play's). first-at-a and first-at-b "
                    "must give the same stats.bin." % ("B's then A's" if order == "a" else "A's then B's"),
            "device": [DEV_A, EPOCH_A], "files": files,
            "boots": [dict(acks=[], now=ms_of("2026-11-10T09:00:00"), playing=True, safePoint=False)]}


def scenario_two_devices():
    """O2: two devices' slots in one log, each slot once, each MARK row."""
    a = lambda seq, *args, **kw: slot(DEV_A, EPOCH_A, seq, *args, **kw)  # noqa: E731
    b = lambda seq, *args, **kw: slot(DEV_B, EPOCH_B, seq, *args, **kw)  # noqa: E731
    at = lambda d, h: "2026-11-%02dT%02d:00:00" % (d, h)  # noqa: E731
    before = earlier([a(q, "Quiet Engine", 200000, 200000, at=at(1, 8 + q)) for q in range(1, 4)],
                     now=ms_of(at(1, 20)))
    p1 = [a(1, "Quiet Engine", 200000, 200000, at=at(1, 9)), b(1, "Glass Harbour", 200000, 200000, at=at(2, 9)),
          a(2, "Quiet Engine", 200000, 200000, at=at(1, 10)), b(2, "Glass Harbour", 200000, 200000, at=at(2, 10)),
          a(3, "Quiet Engine", 200000, 200000, at=at(1, 11)), b(3, "Weather Vane", 200000, 200000, at=at(2, 11)),
          a(4, "Signal Fires", 200000, 200000, at=at(3, 9)), a(5, "Signal Fires", 200000, 200000, at=at(3, 10))]
    p0 = [a(6, "Slow Lantern", 200000, 200000, at=at(6, 9)), b(4, "Slow Lantern", 200000, 200000, at=at(6, 10)),
          a(7, "Slow Lantern", 200000, 200000, at=at(7, 9)), b(5, "Brine", 200000, 200000, at=at(7, 10)),
          a(8, "Night Ticket", 200000, 200000, at=at(8, 9))]
    before.update({"plays.1.jnl": log(*p1), "plays.jnl": log(*p0)})
    return {"name": "two-devices",
            "note": "Every player's plays on the card count (O2). stats.bin holds A's 1-3 (MARK (A) 3, no row for "
                    "B); plays.1.jnl holds A 1-5 and B 1-3, plays.jnl A 6-8 and B 4-5; ACKS (A) 8, (B) 3. The boot "
                    "folds A 4-8 and B 1-5: MARK (A) 8, (B) 5. plays.1.jnl is under ACKS and the saved MARK: "
                    "trimmed; plays.jnl holds B 4-5 over ACKS: kept.",
            "device": [DEV_A, EPOCH_A], "files": before,
            "boots": [dict(acks=[(DEV_A, EPOCH_A, 8), (DEV_B, EPOCH_B, 3)], now=ms_of("2026-11-10T09:00:00"),
                           playing=True, safePoint=False)]}


def scenario_twice():
    """Each slot once (5.6): the same slots in two files; a conflict."""
    a = lambda seq, *args, **kw: slot(DEV_A, EPOCH_A, seq, *args, **kw)  # noqa: E731
    at = lambda h: "2026-11-09T%02d:00:00" % h  # noqa: E731
    first = [a(q, ["Copper Season", "Brine", "Low Tide"][q % 3], 200000, 200000, at=at(6 + q)) for q in range(1, 5)]
    later = [a(5, "Night Ticket", 200000, 200000, at=at(12)),
             a(6, "Night Ticket", 200000, 200000, at=at(13))]
    conflict = a(6, "Orbit Room", 60000, 200000, at=at(13))         # seq 6 again, other bytes (a rolled-back NVS)
    files = {"plays.jnl": log(*later), "plays.1.jnl": log(*first), "plays.2.jnl": log(*(first[2:] + [conflict]))}
    return {"name": "twice",
            "note": "Each slot once: plays.2.jnl (a later firmware's longer rotation) holds seqs 3-4 again, as "
                    "plays.1.jnl does, and a seq 6 whose bytes differ from plays.jnl's (a conflict). The scan reads "
                    "plays.jnl, plays.1.jnl, then plays.2.jnl: each key's first read is kept, and the three met "
                    "again from another file count in HEAD's twice. No ACKS: nothing trimmed.",
            "device": [DEV_A, EPOCH_A], "files": files,
            "boots": [dict(acks=[], now=ms_of("2026-11-10T09:00:00"), playing=True, safePoint=False)]}


def scenario_new_epoch():
    a = lambda seq, *args, **kw: slot(DEV_A, EPOCH_A, seq, *args, **kw)  # noqa: E731
    files = {"plays.jnl": log(*[a(q, "Paper Comets", 200000, 200000, at="2026-11-08T%02d:00:00" % (9 + q))
                                for q in range(1, 4)])}
    return {"name": "new-epoch",
            "note": "The device boots with a new epoch (NVS erased: its epoch is device[1]) and the root has no "
                    "ACKS: 0.14.0's scan reads nothing then (PlayLog.cpp startScan); with stats the scan reads, "
                    "and the old epoch's slots fold once under their own MARK row.",
            "device": [DEV_A, EPOCH_A2], "files": files,
            "boots": [dict(acks=[], now=ms_of("2026-11-10T09:00:00"), playing=True, safePoint=False)]}


def scenario_retention():
    """5.2's retention: HOUR's 840 rows, DAYS's 400, RCNT's 32 days."""
    a = lambda seq, *args, **kw: slot(DEV_A, EPOCH_A, seq, *args, **kw)  # noqa: E731
    names = sorted(TRACKS)
    slots, q, today = [], 0, 452                    # today 2027-03-29
    for d in range(today - 449, today + 1):         # 450 days, two hours each
        for hh in (8, 20):
            q += 1
            slots.append(a(q, names[q % len(names)], 200000, 200000, startedAt=MIN_MS + d * DAY_MS + hh * HOUR_MS,
                           clock=1))
    q += 1
    slots.append(a(q, "Quiet Engine", 200000, 200000, startedAt=MIN_MS + (today + 1) * DAY_MS + 9 * HOUR_MS,
                   clock=1))                        # tomorrow, by a fast clock
    foreign = slot(DEV_C, EPOCH_C, 1, "Glass Ladder", 200000, 200000, startedAt=ms_of("2099-06-01T10:00:00"), clock=2)
    files = {"plays.jnl": log(*(slots + [foreign]))}
    return {"name": "retention",
            "note": "5.2's retention at a save, today day 452 (2027-03-29): two plays a day for 450 days (900 "
                    "hours), one tomorrow by a fast clock, and device C's play dated 2099. HOUR keeps its newest "
                    "840 rows and DAYS its newest 400, 2099's and tomorrow's among them: rows by count, never by "
                    "date. RCNT keeps the 32 days back from the earlier of today and the newest dated day (2099's): "
                    "days 421-452, and tomorrow's and 2099's rows, newer than that. TRKD and HEAD keep everything.",
            "device": [DEV_A, EPOCH_A], "files": files,
            "boots": [dict(acks=[], now=MIN_MS + today * DAY_MS + 21 * HOUR_MS, playing=True, safePoint=False)]}


def scenario_rcnt_cap():
    """5.2: RCNT past 4,096 rows loses whole days, oldest first."""
    a = lambda seq, *args, **kw: slot(DEV_A, EPOCH_A, seq, *args, **kw)  # noqa: E731
    hashes = [path_hash("Bulk/Track %03d.mp3" % i) for i in range(129)]
    slots, q = [], 0
    for d in range(32):
        for i, h in enumerate(hashes):
            q += 1
            slots.append(a(q, h, 31000, 62000, startedAt=MIN_MS + (288 + d) * DAY_MS + i * 40000, clock=1))
    files = {"plays.jnl": log(*slots)}
    return {"name": "rcnt-cap",
            "note": "32 days (288-319) of the same 129 tracks (made-up paths outside the fixture library: orphans "
                    "to the shelves, kept by the rankers) played once a day: 4,128 RCNT rows, over the 4,096 cap, "
                    "so day 288 goes whole and 3,999 rows stay; the 30-day window then holds 30 days still.",
            "device": [DEV_A, EPOCH_A], "files": files,
            "boots": [dict(acks=[], now=MIN_MS + 319 * DAY_MS + 23 * HOUR_MS, playing=True, safePoint=False)]}


def scenario_no_clock():
    """No dates: sinceDay from the root's commitTime, nothing pruned by date."""
    a = lambda seq, *args, **kw: slot(DEV_A, EPOCH_A, seq, *args, **kw)  # noqa: E731
    b = lambda seq, *args, **kw: slot(DEV_B, EPOCH_B, seq, *args, **kw)  # noqa: E731
    p0 = [a(q, ["Night Ticket", "Slow Lantern"][q % 2], 200000, 200000, at=None) for q in range(1, 4)]
    p1 = [b(1, "Brine", 200000, 200000, at="2026-08-01T10:00:00"), b(2, "Brine", 200000, 200000,
                                                                     at="2026-08-02T10:00:00")]
    return [{"name": "no-clock",
             "note": "The clock never set: three undated plays, now unset at the save. sinceDay comes from the "
                     "root's commitTime (flags bit 0); the shelves that need dates have no list, Most played's "
                     "All time and Recently played (by lastOrd) do.",
             "device": [DEV_A, EPOCH_A], "files": {"plays.jnl": log(*p0)},
             "boots": [dict(acks=[], now=None, commitTime=ms_of("2026-11-01T12:00:00") // 1000, playing=True,
                            safePoint=False)]},
            {"name": "no-clock-dated",
             "note": "The board's clock unset, but device B's plays are dated, 100 days back: sinceDay is their "
                     "earliest day (not flagged), and with no today nothing is pruned by date, so their RCNT rows "
                     "stay.",
             "device": [DEV_A, EPOCH_A], "files": {"plays.jnl": log(*p0), "plays.1.jnl": log(*p1)},
             "boots": [dict(acks=[], now=None, commitTime=ms_of("2026-11-01T12:00:00") // 1000, playing=True,
                            safePoint=False)]}]


def scenario_compaction():
    """5.3: the delta over a quarter of the base, compacted while nothing
    plays; then the same with a track playing: no compaction."""
    a = lambda seq, *args, **kw: slot(DEV_A, EPOCH_A, seq, *args, **kw)  # noqa: E731
    names = sorted(TRACKS)
    old = [a(q, names[q % 6], 200000, 200000, at="2026-10-%02dT10:00:00" % (1 + q)) for q in range(1, 13)]
    before = earlier(old, now=ms_of("2026-10-20T10:00:00"))
    new = [a(q, names[q % len(names)], 200000, 200000, at="2026-11-%02dT10:00:00" % (q - 12)) for q in range(13, 17)]
    files = dict(before, **{"plays.jnl": log(*(old + new))})
    return [{"name": "compaction",
             "note": "A base of 6 rows (stats-00000001.bin) and a delta of 4 new rows from the log: over a quarter "
                     "of the base, and nothing playing, so the boot's save compacts: stats-00000002.bin holds base "
                     "(+) delta, stats.bin names it with an empty TRKD, and the old base goes. ACKS covers the log, "
                     "which the new MARK covers too: trimmed.",
             "device": [DEV_A, EPOCH_A], "files": files,
             "boots": [dict(acks=[(DEV_A, EPOCH_A, 16)], now=ms_of("2026-11-10T09:00:00"), playing=False,
                            safePoint=True)]},
            {"name": "compaction-playing",
             "note": "As compaction, with a track playing at the save: the delta (96 B) is under 96 KB, so no "
                     "compaction; TRKD holds the 4 rows and the base stays.",
             "device": [DEV_A, EPOCH_A], "files": dict(files),
             "boots": [dict(acks=[(DEV_A, EPOCH_A, 16)], now=ms_of("2026-11-10T09:00:00"), playing=True,
                            safePoint=True)]}]


def scenario_gap():
    """A firmware without stats trimmed acknowledged files between."""
    a = lambda seq, *args, **kw: slot(DEV_A, EPOCH_A, seq, *args, **kw)  # noqa: E731
    at = lambda d, h: "2026-11-%02dT%02d:00:00" % (d, h)  # noqa: E731
    before = earlier([a(q, "Copper Season", 200000, 200000, at=at(1, q + 8)) for q in range(1, 11)],
                     now=ms_of(at(1, 22)))
    p0 = [a(q, "Glass Ladder", 200000, 200000, at=at(8, (q - 41) // 4 + 8)) for q in range(41, 61)]
    before["plays.jnl"] = log(*p0)
    return {"name": "gap",
            "note": "stats.bin's MARK (A) is 10; 0.14.0 then ran, the card was synced (ACKS (A) 50) and 0.14.0 "
                    "trimmed the files holding 11-40 without folding them. The log now holds 41-60. The boot folds "
                    "41-60, counts seqs 11-40 in HEAD's gap (30: plays lost to the stats), and MARK covers them. "
                    "The second boot finds nothing new: no save, no second count of the gap.",
            "device": [DEV_A, EPOCH_A], "files": before,
            "boots": [dict(acks=[(DEV_A, EPOCH_A, 50)], now=ms_of("2026-11-10T09:00:00"), playing=True,
                           safePoint=False),
                      dict(acks=[(DEV_A, EPOCH_A, 50)], now=ms_of("2026-11-11T09:00:00"), playing=True,
                           safePoint=False)]}


def scenario_save_fails():
    """5.5: a save that fails still trims what the card's MARK covers."""
    a = lambda seq, *args, **kw: slot(DEV_A, EPOCH_A, seq, *args, **kw)  # noqa: E731
    at = lambda d, h: "2026-11-%02dT%02d:00:00" % (d, h)  # noqa: E731
    p1 = [a(q, "Tidewater", 200000, 200000, at=at(2, 8 + q)) for q in range(1, 5)]
    p0 = [a(q, "Salt Stairs", 200000, 200000, at=at(3, 8 + q)) for q in range(5, 9)]
    before = earlier(p1, now=ms_of(at(2, 20)))
    before.update({"plays.1.jnl": log(*p1), "plays.jnl": log(*p0)})
    return {"name": "save-fails",
            "note": "stats.bin covers plays.1.jnl (MARK (A) 4); plays.jnl holds 5-8; ACKS (A) 8 covers both. The "
                    "card is full: the boot's save fails, stats.bin stays as it was, and the trim follows the MARK "
                    "read at the boot: plays.1.jnl goes, plays.jnl stays for the next boot to fold.",
            "device": [DEV_A, EPOCH_A], "files": before,
            "boots": [dict(acks=[(DEV_A, EPOCH_A, 8)], now=ms_of("2026-11-10T09:00:00"), playing=False,
                           safePoint=True, saveFails=True)]}


def resealed(b, major=None, minor=None) -> bytes:
    """A file's frame with another major or minor, its header's CRC made
    again (the sections' CRCs hold)."""
    b = bytearray(b)
    if major is not None:
        struct.pack_into("<H", b, 4, major)
    if minor is not None:
        struct.pack_into("<H", b, 6, minor)
    hb, n = struct.unpack_from("<II", b, 8)
    struct.pack_into("<I", b, 32, 0)
    struct.pack_into("<I", b, 32, crc32(b[:hb + 32 * n]))
    return bytes(b)


def newer_minor(st: Stats, minor=3, more=8) -> bytes:
    """stats.bin as a later minor of major 1 may write it (5.12): HEAD 32
    bytes longer, every array's rows `more` bytes longer (this minor's
    fields first), and an OPTIONAL section this reader doesn't know."""
    _, secs = open_frame(write_stats(st), STATS_SECTIONS)
    out = []
    for t, stride in STATS_SECTIONS:
        data = secs[t][1]
        if stride == 0:
            out.append((t, REQUIRED, 0, 0, data + b"\xee" * 32))
        else:
            rows = [data[i:i + stride] + b"\xee" * more for i in range(0, len(data), stride)]
            out.append((t, REQUIRED, len(rows), stride + more, b"".join(rows)))
    out.append((b"XTRA", 0, 2, 4, bytes(8)))
    return mpwf.frame(st.generation, 0, b"", out, HEADER_BYTES, MAJOR, minor, MAGIC)


def reads_slots():
    """The card the read's scenarios share: stats.bin and its base after
    A's seqs 1-6 (MARK (A) 6, base stats-00000001.bin of 3 rows), the log
    holding 1-6 (plays.1.jnl) and 7-10 (plays.jnl), ACKS (A) 10."""
    a = lambda seq, *args, **kw: slot(DEV_A, EPOCH_A, seq, *args, **kw)  # noqa: E731
    at = lambda d, h: "2026-11-%02dT%02d:00:00" % (d, h)  # noqa: E731
    old = [a(q, ["Copper Season", "Brine", "Night Ticket"][q % 3], 200000, 200000, at=at(2, 8 + q))
           for q in range(1, 7)]
    new = [a(q, ["Paper Comets", "Brine"][q % 2], 200000, 200000, at=at(9, q)) for q in range(7, 11)]
    return old, {"plays.1.jnl": log(*old), "plays.jnl": log(*new)}, [(DEV_A, EPOCH_A, 10)], ms_of(at(2, 20))


def scenario_reads():
    """5.4's every verdict, the bases, and 5.12's newer minor."""
    old, logs, acks, then = reads_slots()
    before = earlier(old, now=then)
    now = ms_of("2026-11-10T09:00:00")
    boot1 = lambda playing=True: [dict(acks=acks, now=now, playing=playing, safePoint=not playing)]  # noqa: E731
    dev = [DEV_A, EPOCH_A]
    out = []
    newer = dict(before, **logs)
    newer["stats.bin"] = resealed(before["stats.bin"], major=2)
    out.append({"name": "newer",
                "note": "stats.bin of a newer major (2; its frame whole). The stats are off this session: nothing "
                        "folded, no save, and no trim although ACKS covers the log; every file is left as it is.",
                "device": dev, "files": newer, "boots": boot1(False)})
    corrupt = dict(before, **logs)
    corrupt["stats.bin"] = before["stats.bin"][:-1]
    corrupt["stats.bad"] = before["stats.bin"][:64]
    out.append({"name": "corrupt",
                "note": "stats.bin one byte short (FileBytes), and an older stats.bad. stats.bin is renamed "
                        "stats.bad over the older one, and counting starts again from the log: every slot in it "
                        "folded. The stale stats-00000001.bin it named is named by nothing now: nothing playing, "
                        "the save compacts and writes the new base under that name, over it (5.3).",
                "device": dev, "files": corrupt, "boots": boot1(False)})
    lost = dict(before, **logs)
    del lost["stats-00000001.bin"]
    lost["stats-00000002.bin"] = write_base({}, 2)
    out.append({"name": "base-lost",
                "note": "The base stats.bin names (stats-00000001.bin) isn't there; a stats-00000002.bin (a cut "
                        "compaction's) is. The base is lost: flags bit 1, baseGen 0, its rows gone, the totals kept; "
                        "the fold goes on, and no stats-<n>.bin is removed on this boot. A track plays: no "
                        "compaction.",
                "device": dev, "files": lost, "boots": boot1(True)})
    other = dict(before, **logs)
    other["stats-00000001.bin"] = write_base({path_hash("Elsewhere/01 Other.mp3"): Row(1, 0, 0, 0, 1, YOU)}, 1)
    later = slot(DEV_A, EPOCH_A, 11, "Paper Comets", 200000, 200000, at="2026-11-10T20:00:00")
    out.append({"name": "base-other",
                "note": "Another file under the base's name: its header whole, its CRC not stats.bin's baseCrc. "
                        "The base is lost (flags bit 1, the totals kept). Boot 1: nothing plays, but the "
                        "compaction's name would be stats-00000001.bin again, the one stats.bin names, and a cut "
                        "after its create would leave a base shorter than its header there, unread at every boot "
                        "(5.3): so no compaction (TRKD keeps the rows), nothing removed. Boot 2, a play later: its "
                        "stats.bin names no base, so the save compacts, and the new base, stats-00000001.bin, "
                        "replaces that file; nothing is removed.",
                "device": dev, "files": other,
                "boots": boot1(False) + [dict(acks=acks, now=now + DAY_MS, playing=False, safePoint=True,
                                              add={"plays.jnl": log(later)})]})
    unread = dict(before, **logs)
    b = bytearray(before["stats-00000001.bin"])
    b[-1] ^= 1
    unread["stats-00000001.bin"] = bytes(b)
    out.append({"name": "base-unread",
                "note": "The base's header is the one stats.bin names, but its last row fails its CRC: a card "
                        "fault, never a lost base. The stats are off this session (unread): nothing folded, no "
                        "save, no trim; every file left as it is.",
                "device": dev, "files": unread, "boots": boot1(False)})
    short = dict(before, **logs)
    short["stats-00000001.bin"] = before["stats-00000001.bin"][:HEADER_BYTES - 1]
    out.append({"name": "base-short",
                "note": "The base cut to 39 bytes: its header doesn't read whole, so the card failed it (unread), "
                        "as at 41 bytes (Short) or any length (FileBytes): never a lost base.",
                "device": dev, "files": short, "boots": boot1(False)})
    near = dict(before, **logs)
    near["stats-00000002.bin"] = write_base({}, 2)
    out.append({"name": "base-neighbours",
                "note": "A stats-00000002.bin (a compaction cut before stats.bin named it) beside a stats.bin naming "
                        "stats-00000001.bin, which checks whole: the neighbour is removed. A track plays: no "
                        "compaction.",
                "device": dev, "files": near, "boots": boot1(True)})
    nm = earlier(old, now=then, compact=False)
    nm["stats.bin"] = newer_minor(load(nm))
    nm.update(logs)
    out.append({"name": "newer-minor",
                "note": "stats.bin of minor 3 (5.12): HEAD 32 bytes longer, every array's rows 8 bytes longer, and "
                        "an OPTIONAL section XTRA. It reads (loaded) by its known sections and row prefixes; the "
                        "save writes minor 0, without the additions. A track plays: no compaction.",
                "device": dev, "files": nm, "boots": boot1(True)})
    return out


def scenario_clock_ahead():
    """5.2's retention: a clock a year ahead at one save, then corrected."""
    a = lambda seq, *args, **kw: slot(DEV_A, EPOCH_A, seq, *args, **kw)  # noqa: E731
    t = 312                                                             # 2026-11-09
    at = lambda day, h: MIN_MS + day * DAY_MS + h * HOUR_MS  # noqa: E731
    names = sorted(TRACKS)
    hist = [a(q, names[q % len(names)], 200000, 200000, startedAt=at(t - 40 + q, 10), clock=1) for q in range(1, 41)]
    before = earlier(hist, now=at(t, 20))
    ahead = a(41, "Quiet Engine", 200000, 200000, startedAt=at(t + 365, 10), clock=1)
    fixed = a(42, "Signal Fires", 200000, 200000, startedAt=at(t + 1, 10), clock=1)
    before["plays.jnl"] = log(ahead)
    return {"name": "clock-ahead",
            "note": "stats.bin holds 40 days of plays to day 312 (2026-11-09; RCNT its last 32 days). Boot 1: the "
                    "board's clock a year ahead (day 677), and a play dated by it. RCNT keeps the 32 days back from "
                    "677: only that play's row. Boot 2: the clock corrected (day 313) and a new play (plays.jnl "
                    "gains it before the boot): RCNT keeps 313's row and 677's (newer than the reference). HOUR, "
                    "DAYS, TRKD and HEAD keep every row and count at both: the cost is RCNT's month, never a "
                    "lifetime figure, an hour or a day. sinceDay stays day 273.",
            "device": [DEV_A, EPOCH_A], "files": before,
            "boots": [dict(acks=[], now=at(t + 365, 12), playing=True, safePoint=False),
                      dict(acks=[], now=at(t + 1, 12), playing=True, safePoint=False,
                           add={"plays.jnl": log(ahead, fixed)})]}


def scenario_partial():
    """5.6's partial scan: no save; the trim by the boot's MARK; the next
    whole boot folds every slot once."""
    a = lambda seq, *args, **kw: slot(DEV_A, EPOCH_A, seq, *args, **kw)  # noqa: E731
    at = lambda d, h: "2026-11-%02dT%02d:00:00" % (d, h)  # noqa: E731
    s = [None] + [a(q, ["Harbour Lights", "Slow Lantern", "Orbit Room"][q % 3], 200000, 200000,
                    at=at(3 + q // 4, 8 + q % 4)) for q in range(1, 15)]
    now = ms_of("2026-11-10T09:00:00")
    whole = dict(acks=[(DEV_A, EPOCH_A, 11)], now=now + DAY_MS, playing=True, safePoint=False)
    opened = earlier(s[1:5], now=ms_of(at(4, 20)))
    opened.update({"plays.1.jnl": log(*s[1:9]), "plays.jnl": log(*s[9:12])})
    midway = dict(opened)
    tail = earlier(s[1:9], now=ms_of(at(5, 20)))
    tail.update({"plays.1.jnl": log(*s[3:9]), "plays.jnl": log(*s[9:15])})
    return [{"name": "partial-open",
             "note": "stats.bin's MARK (A) 4; plays.1.jnl holds 1-8, plays.jnl 9-11; ACKS (A) 11. Boot 1: the "
                     "card fails plays.1.jnl's open (unread, null: no slot read). The scan goes on: 9-11 folded, "
                     "but the scan was partial, so no save; the trim keeps plays.1.jnl (unread) and follows the "
                     "MARK read at the boot (4), so plays.jnl stays. Boot 2 reads both whole: 5-11 folded, each "
                     "once; saved; both trimmed.",
             "device": [DEV_A, EPOCH_A], "files": opened,
             "boots": [dict(whole, now=now, unread={"plays.1.jnl": None}), whole]},
            {"name": "partial-read",
             "note": "As partial-open, but the card fails a read midway through plays.1.jnl: 6 slots read (1-6, "
                     "so 5-6 folded), 7-8 not. 9-11 folded too; no save; nothing trimmed. Boot 2 folds 5-11 once.",
             "device": [DEV_A, EPOCH_A], "files": midway,
             "boots": [dict(whole, now=now, unread={"plays.1.jnl": 6}), whole]},
            {"name": "partial-tail",
             "note": "stats.bin's MARK (A) 8; plays.1.jnl holds 3-8, plays.jnl 9-14; ACKS (A) 14. Boot 1: a read "
                     "fault after plays.jnl's third slot (9-11 folded, 12-14 not): no save; the trim follows the "
                     "MARK read at the boot, under which all of plays.1.jnl is: it goes (ACKS covers it), and "
                     "plays.jnl (unread) stays. Boot 2 folds 9-14 once, saves, and trims plays.jnl.",
             "device": [DEV_A, EPOCH_A], "files": tail,
             "boots": [dict(acks=[(DEV_A, EPOCH_A, 14)], now=now, playing=True, safePoint=False,
                            unread={"plays.jnl": 3}),
                       dict(acks=[(DEV_A, EPOCH_A, 14)], now=now + DAY_MS, playing=True, safePoint=False)]}]


def scenarios():
    out = [scenario_worked(), scenario_rules(), scenario_first_at("a"), scenario_first_at("b"),
           scenario_two_devices(), scenario_twice(), scenario_new_epoch(), scenario_retention(), scenario_rcnt_cap()]
    out += scenario_no_clock() + scenario_compaction()
    out += [scenario_gap(), scenario_save_fails()]
    out += scenario_reads() + [scenario_clock_ahead()] + scenario_partial()
    return out


def acks_json(acks):
    return [[d, "0x%016x" % playlog.u64(e), s] for d, e, s in acks]


def run_scenario(sc):
    """Each boot of a scenario in turn: (the index entry, {path: bytes})."""
    out, entry = {}, {"name": sc["name"], "note": sc["note"],
                      "device": [sc["device"][0], "0x%016x" % sc["device"][1]], "boots": []}
    files = dict(sc["files"])
    where = {}                                      # each file's folder: a file stored once, where it first appears

    def stored(fs, folder):
        for n, b in sorted(fs.items()):
            if n not in where or where[n][1] != b:
                where[n] = (folder, b)
                out["stats/%s/%s" % (folder, n)] = b
        return {n: where[n][0] for n in sorted(fs)}

    card_in = stored(files, sc["name"] + "/0")
    for i, bt in enumerate(sc["boots"], 1):
        if bt.get("add"):
            # Files changed between boots (a play the device wrote): this
            # boot's in, stored in <i>-in.
            files = dict(files, **bt["add"])
            card_in = stored(files, "%s/%d-in" % (sc["name"], i))
        r = boot(files, bt["acks"], bt["now"], bt.get("commitTime"), bt["playing"], bt["safePoint"],
                 bt.get("saveFails", False), bt.get("unread"))
        today = today_of(bt["now"])
        card_out = stored(r["files"], "%s/%d" % (sc["name"], i))
        st = load(r["files"])
        e = {"in": card_in, "added": sorted(bt.get("add", {})),
             "acks": acks_json(bt["acks"]), "now": bt["now"], "today": today, "commitTime": bt.get("commitTime"),
             "playing": bt["playing"], "safePoint": bt["safePoint"], "saveFails": bt.get("saveFails", False),
             "unread": bt.get("unread", {}),
             "out": card_out,
             "read": r["read"], "why": r["why"], "folded": r["folded"], "twice": r["twice"], "gap": r["gap"],
             "partial": r["partial"], "saved": r["saved"],
             "compacted": r["compacted"], "removed": r["removed"], "trimmed": r["trimmed"]}
        if st is not None:
            e["stats"] = stats_json(st)
            e["lists"] = lists_json(lists(st, today, ALBUMS))
            e["view"] = view(st, today, ALBUMS)
        entry["boots"].append(e)
        files, card_in = r["files"], card_out
    return entry, out


# ---------------------------------------------------------------------------
# The sessions (test/fixtures/card/stats/sessions.json): players' boots and
# plays in turn on one card, the play log written as the firmware's journal
# writes it (tools/playlog.py's Device: the rotation under a cap, its drop
# note, the twins) and the stats kept as 5.5-5.8 and 5.14 say (Session):
# the boot's fold, save and trim (a boot overflow's save tried four times);
# the live fold of each play written into the records; the records' save
# once they number SAVE_AT (waiting for a library update's fence, at RECORDS whatever
# the fence, refused while the orderly end flushes); the rotation's save
# before plays.1.jnl leaves the log's names; Hs; the end's flush (a
# rotation's save only with 1 s of its budget left); the card's copy checked
# first (another player's stats.bin refuses every later save of a session; a
# section damaged under it leaves it behind); a tmp held by a replace() that
# failed, promoted by the next save; a 0.14.0 boot (no stats: today's trim);
# NVS put back or erased. test_stats_store runs each step through the
# firmware's code and compares the card's /.player after it, byte for byte.
# ---------------------------------------------------------------------------
class _Card:
    """The card the session's players share: /.player's files ({name:
    bytes}) and each one's first cluster (the cut-rename rule's chains)."""

    def __init__(self, files):
        self.files = dict(files)
        self.chains = {}
        self.chain = 0


class _Player(playlog.Device):
    """A player: playlog's Device over the shared card, and its session's
    stats (None: none this boot, or a 0.14.0 boot)."""

    def __init__(self, card, device_id, epochs, cap):
        self.card = card
        super().__init__(device_id, epochs, cap)
        self.files = card.files
        self.chains = card.chains
        self.se = None
        self.acks = []
        self.seen = None          # the card's stats.bin this session stands on (read, or last written)
        self.changed = False      # ... changed under it: every later save of the session refused
        self.appended = False     # a slot asked for since the boot (a remount begins no scan again)
        self.fenced = False       # a library update's fence is up ("fence" to "unfence"): the records' save waits
        self.held = None          # stats.tmp as a save whose replace() failed wrote it (5.14's Held), till promoted
        self.tried = 0            # the records when their save last didn't land (0: none since; StatsGlue's recordsTried_)

    @property
    def chain(self):
        return self.card.chain

    @chain.setter
    def chain(self, v):
        self.card.chain = v


def _header(b):
    """A stats.bin's header as the store compares copies (5.5): its
    generation and header CRC; None for no file."""
    return None if b is None else (struct.unpack_from("<I", b, 20)[0], header_crc(b))


def _promote(p):
    """Store::promote() (5.14's Held): the tmp a save held takes stats.bin's
    place before the next save goes on (stats.bin removed, the tmp renamed:
    tagstore::replace()). None when stats.bin is the held copy now (promoted;
    or the tmp gone with stats.bin the held copy's header, another's
    settle() having taken it), and the card's MARK is the held copy's;
    else the refusal: "behind" (the tmp gone with stats.bin the copy before,
    or a tmp whose header isn't the one written: RAM no longer holds the
    slots the held copy took, and the log above the card's MARK keeps them
    for the next boot) or "changed"."""
    files, held, se = p.card.files, p.held, p.se
    p.held = None
    tmp = files.get(STATS_TMP)
    if tmp is not None and _header(tmp) == _header(held):
        files[STATS_BIN] = files.pop(STATS_TMP)
    elif tmp is not None or files.get(STATS_BIN) == p.seen:
        se.fall_behind()
        return "behind"
    elif _header(files.get(STATS_BIN)) != _header(held):
        p.changed = True
        return "changed"
    p.seen = files[STATS_BIN]
    st = read_stats(p.seen)
    se.saved_mark = dict(st.mark)
    se.card_base = st.head["baseGen"]
    return None


SAVED = ("wrote", "nothing")            # the outcomes after which the card's stats.bin holds every slot folded


def _save(p, why, st):
    """A save of player p's session (_ask's), and the records' save's
    retry rule (StatsGlue's recordsTried_): one that didn't land, failed or
    refused, is due again only after a later record (_records_due); any
    save that writes, has nothing new or is held clears it."""
    out = _ask(p, why, st)
    if out in SAVED or out == "held":
        p.tried = 0
    elif why == "records" and p.se is not None:
        p.tried = p.se.records
    return out


def _ask(p, why, st):
    """A save of player p's session (why: "boot", "records", "rotation",
    "hs"), its outcome: "wrote" (stats.bin written), "nothing" (nothing new:
    nothing written; the card's stats.bin holds it all), "held" (5.14's
    Held: the tmp written whole, replace() failed: st's `replaceFails`),
    "failed" (a step failed: st's `saveFails`, a full card), or a refusal,
    in this order: "off" (no stats this session), "ending" (the boot's or
    the records' in the orderly end's flush), "budget" (a rotation's there
    with under 1 s of its budget `left`), "fenced" (Hs while a library
    update's fence is up), "partial" (a partial scan), "behind" (the session
    lacks a slot the log holds above the card's MARK), "changed" (the card's
    stats.bin isn't the copy the session stands on: refused for good this
    session). Then the store: a held tmp promoted first (_promote); nothing
    new; the card's copy checked (another header: "changed"; the same
    header over other bytes, a section damaged under the session: "behind"
    for good, the next boot's read Corrupt); the write. Only a boot's save
    compacts."""
    se = p.se
    if se is None or not se.on:
        return "off"
    if st.get("ending") and why in ("boot", "records"):
        return "ending"
    if st.get("ending") and why == "rotation" and st.get("left", 0) < 1000:
        return "budget"
    if why == "hs" and p.fenced:
        return "fenced"
    if se.partial:
        return "partial"
    if se.behind:
        return "behind"
    if p.changed:
        return "changed"
    if p.held is not None:
        refused = _promote(p)
        if refused:
            return refused
    if not se.new:
        return "nothing"
    cur = p.card.files.get(STATS_BIN)
    if cur != p.seen:
        if cur is not None and _header(cur) == _header(p.seen):
            se.fall_behind()
            return "behind"
        p.changed = True
        return "changed"
    if st.get("saveFails"):
        return "failed"
    held = bool(st.get("replaceFails"))
    se.save(st.get("now"), why, st.get("playing", False), st.get("safePoint", True), held=held)
    if held:
        p.held = p.card.files[STATS_TMP]
        return "held"
    p.seen = p.card.files.get(STATS_BIN)
    return "wrote"


def _records_due(p):
    """The records' save is due (5.14): the session's records number SAVE_AT
    or more, the boot's fold's or the live ones', and it isn't behind; one
    that didn't land is due again only once a later record came (the
    records past the count it was tried at: StatsGlue's recordsDue())."""
    se = p.se
    return (se is not None and se.on and not se.behind and se.records >= SAVE_AT and
            not (p.tried and se.records <= p.tried))


def _session_boot(p, st):
    """A boot of player p: NVS (a new epoch when erased), the cut-rename
    rule, the stats' read and the boot's fold, the boot rules (the seq with
    ACKS's and MARK's), the boot's save (none after a partial scan), the
    trim: with stats, a file every play of which the card's MARK covers
    (after a save that landed, the one it wrote). stats false: a 0.14.0
    boot, today's trim. A boot whose fold passes the records (RECORDS: the
    overflow's chunks) and whose save doesn't land tries it again after
    the trim, BOOT_TRIES in all while it fails; none landing (or held), or
    a partial scan's, leaves the session behind. saveFails: True, every try
    fails; a number n, the first n."""
    acks = list(st.get("acks", []))
    unread = dict(st.get("unread") or {})
    p.down = p.blocked = False
    p.acks, p.appended, p.changed = acks, False, False
    p.fenced, p.held, p.tried = False, None, 0
    if p.nvs is None:
        p.nvs = {"epoch": p.epochs.pop(0), "seq": 0, "boots": 0}
    p.settle()
    stats = st.get("stats", True)
    se = Session(p.card.files, acks, st.get("commitTime"), unread) if stats else None
    p.se = se
    p.seen = p.card.files.get(STATS_BIN)
    if se is not None:
        se.scan()
    # The boot rules over the slots the scan read (a partial scan's, as far
    # as it read).
    logs = {}
    for n, b in p.card.files.items():
        if n in LOG_NAMES:
            logs[n] = b if n not in unread else b"" if unread[n] is None else b[:unread[n] * playlog.SLOT]
    slots = playlog.valid_slots(logs)
    mark = se.stats.mark.get((playlog.device_bytes(p.device_id), p.nvs["epoch"]), 0) if se and se.on else 0
    p.nvs["boots"] = playlog.boot_count(p.nvs["boots"], slots, p.device_id, p.nvs["epoch"])
    p.nvs["seq"] = playlog.boot_seq(p.nvs["seq"], slots, p.device_id, p.nvs["epoch"], acks, mark) - 1
    fails = st.get("saveFails", False)
    failing = BOOT_TRIES if fails is True else int(fails)
    outcome, tries = None, 0

    def once():
        nonlocal tries
        tries += 1
        return _save(p, "boot", dict(st, saveFails=tries <= failing))

    if se is not None and not se.partial:
        outcome = once()
    saved = outcome in SAVED
    # The trim, plays.1.jnl to plays.9.jnl, then plays.jnl.
    trimmed = []
    cover = None
    if se is not None:
        cover = Stats()
        cover.mark = se.saved_mark if saved else se.mark_at_boot
    for name in LOG_NAMES[1:] + LOG_NAMES[:1]:
        data = p.card.files.get(name)
        if not acks or data is None or name in unread or not playlog.trimmable(data, acks):
            continue
        if se is not None:
            plays = [s for _, why, s, _ in playlog.read_file(data) if why == "ok" and s["kind"] == playlog.KIND_PLAY]
            if not se.on or not all(cover.covered(s) for s in plays):
                continue
        if p._shared(name):
            continue
        p._delete(name, "acknowledged")
        trimmed.append(name)
    facts = {}
    if se is not None:
        if se.on and se.records > RECORDS and outcome not in ("wrote", "held"):
            # The overflow: tried again at later passes, the boot's waits
            # apart, while it fails; given back when one lands, else the
            # session is behind.
            while outcome == "failed" and tries < BOOT_TRIES:
                outcome = once()
            if outcome not in ("wrote", "held"):
                se.fall_behind()
        facts = {"read": se.read, "why": se.why, "folded": se.folded, "twice": se.twice, "gap": se.gap,
                 "partial": se.partial, "saved": se.wrote, "compacted": se.compacted, "removed": list(se.removed),
                 "bootSave": outcome, "tries": tries, "held": outcome == "held"}
    facts.update({"stats": stats, "trimmed": trimmed})
    return facts


def _session_play(p, st):
    """One play of player p, written: the records' save first when it is due
    and no fence is up (refused in the end's flush; one that doesn't land is
    due again only after a later record: _records_due); the rotation's save when the append
    would rotate over a plays.1.jnl holding a play the card's MARK doesn't
    cover (none after a partial scan; none without stats); then the slot
    (playlog's Device: the rotation, the drop note), then the live fold
    into the records. A slot that brings the records to RECORDS while the
    save waits for a fence: the save then, whatever the fence."""
    se, files = p.se, p.card.files
    rec = _save(p, "records", st) if _records_due(p) and not p.fenced else None
    rot = None
    cur = files.get("plays.jnl", b"")
    if se is not None and len(cur) // playlog.SLOT + 1 > p.cap and "plays.1.jnl" in files and not se.partial:
        cover = Stats()
        cover.mark = se.saved_mark
        plays = [s for _, why, s, _ in playlog.read_file(files["plays.1.jnl"])
                 if why == "ok" and s["kind"] == playlog.KIND_PLAY]
        if any(not cover.covered(s) for s in plays) or not se.on:
            rot = _save(p, "rotation", st)
    p.clock = 1 if st["at"] else 0
    slot = p.play(st["hash"], st["size"], st["fatTime"], st["generation"], st["played"], st["duration"],
                  st["outcome"], st["src"], st["up"], started_at=st["at"], acks=p.acks)
    p.appended = True
    if slot is not None and se is not None:
        se.live(slot)
        if p.fenced and se.records >= RECORDS and _records_due(p):
            rec = _save(p, "records", st)
    return {"save": None if rot is None else rot in SAVED, "rotationSave": rot, "recordsSave": rec,
            "seq": None if slot is None else slot["seq"]}


PLAY_FIELDS = ("hash", "played", "duration", "outcome", "src", "at", "up", "size", "fatTime", "generation", "now")
EACH_FIELDS = ("seq", "save", "rotationSave", "recordsSave", "records")


class _Inline(list):
    """A list sessions.json writes on one line: a batch's columns."""


def _session_plays(p, st):
    """A batch: plays of player p one after the other (each as a "play"
    step, its fields from the columns `plays`, the batch's saveFails on
    each), the card stored once after the last. Each play's facts in the
    columns `each` (its seq, its rotation's save and outcome, the records'
    save's outcome, the records after it)."""
    cols = st["plays"]
    each = {k: [] for k in EACH_FIELDS}
    for i in range(len(cols["hash"])):
        one = {k: cols[k][i] for k in PLAY_FIELDS}
        if st.get("saveFails"):
            one["saveFails"] = True
        f = _session_play(p, one)
        f["records"] = p.se.records if p.se else 0
        for k in EACH_FIELDS:
            each[k].append(f[k])
    return {"each": {k: _Inline(v) for k, v in each.items()}}


def _damage(card, name, section):
    """A section damaged under the session: its first byte flipped (XOR
    0xFF), the header and the directory (its CRC) as they were."""
    b = bytearray(card.files[name])
    hb, n = struct.unpack_from("<II", b, 8)
    for i in range(n):
        t, _, off, nb = struct.unpack_from("<4sIII", b, hb + DIR_ENTRY.size * i)
        if t == section.encode("ascii"):
            assert nb > 0, "an empty section can't be damaged"
            b[off] ^= 0xFF
            card.files[name] = bytes(b)
            return
    raise ValueError(section)


def _link(card, a, b):
    """b on a's chain: a rename cut between its two writes left it (or a
    disk check's survivor)."""
    card.files[b] = card.files[a]
    if a not in card.chains:
        card.chain += 1
        card.chains[a] = card.chain
    card.chains[b] = card.chains[a]


def session_play(track, played=200000, duration=200000, outcome=1, src=1, at=None, up=1000, **kw):
    """A play step's own fields (the journal fills seq, epoch, deviceId,
    bootSeq): track a title of TRACKS or a path hash; at ISO UTC or None
    (the clock unset: undated); up the board's monotonic ms at its start.
    Outcomes 1-3 and plays of 1 s or more only (the Tracker's), a completed
    one played to its end."""
    h = TRACKS[track][1] if isinstance(track, str) else track
    st = {"do": "play", "hash": h, "played": played, "duration": duration, "outcome": outcome, "src": src,
          "at": ms_of(at) if at else 0, "up": up, "size": 4000000 + (h & 0xFFFFF), "fatTime": 0x5B684A00,
          "generation": 1}
    st.update(kw)
    return st


def _plays(player, tracks, first_up, at_day, **kw):
    """A run of plays of `tracks` (titles), one a minute from 08:00 on
    2026-11-<at_day> (at_day None: undated)."""
    out = []
    for i, t in enumerate(tracks):
        at = None if at_day is None else "2026-11-%02dT08:%02d:00" % (at_day, i)
        out.append(dict(session_play(t, at=at, up=first_up + 60000 * i, **kw), player=player))
    return out


def _boot(player, day, hour=9, **kw):
    st = {"do": "boot", "player": player, "now": ms_of("2026-11-%02dT%02d:00:00" % (day, hour)), "playing": True,
          "safePoint": False}
    st.update(kw)
    return st


NAMES = ["Night Ticket", "Brine", "Glass Harbour", "Paper Comets", "Slow Lantern", "Low Tide", "Orbit Room",
         "Quiet Engine", "Signal Fires"]


def session_specs():
    """The sessions: each a card's /.player to start from, its players
    (device, the epochs a fresh NVS draws, NVS as it starts: None erased),
    the log's cap, its steps, and the plays the stats lose (`lost`: the
    undercount 5.5 and 5.14 name, asserted)."""
    A = lambda epochs=(EPOCH_A,), nvs=None: {"device": DEV_A, "epochs": list(epochs), "nvs": nvs}  # noqa: E731
    B = lambda: {"device": DEV_B, "epochs": [EPOCH_B], "nvs": None}  # noqa: E731
    names = NAMES
    out = []
    # A rotation over a plays.1.jnl no save covers: the boot's save failed
    # (a full card), and the rotation's saves first (nothing lost), or fails
    # too (1-4 lost: an undercount of 4). Also over a plays.1.jnl a twin's
    # chain holds (renamed to a twin, not deleted: the save runs before).
    for twin in (False, True):
        for fails in (False, True):
            steps = [_boot("A", 1)] + _plays("A", names[:8], 1000, 2)
            steps += [_boot("A", 3, saveFails=True)]
            if twin:
                steps += [{"do": "link", "a": "plays.1.jnl", "b": "plays.xl1"}]
            steps += [dict(session_play("Signal Fires", at="2026-11-03T12:00:00", up=900000, player="A",
                                        now=ms_of("2026-11-03T12:05:00"), saveFails=fails))]
            steps += [_boot("A", 4)]
            out.append({"name": "rotation-%s%s" % ("twin-" if twin else "", "fails" if fails else "saves"),
                        "note": "The boot's save fails (a full card): plays.1.jnl (1-4) is covered by no MARK. The "
                                "next play rotates%s; the save runs first and %s. The next boot folds what the log "
                                "still holds above MARK: %s." % (
                                    " over a plays.1.jnl a twin's chain holds (renamed to a twin, not deleted)"
                                    if twin else "", "fails too" if fails else "lands (MARK 8)",
                                    "5-9; 1-4 are lost to the stats (5.5's undercount, 4)" if fails else "9; none lost"),
                        "cap": 4, "files": {}, "players": {"A": A()}, "steps": steps, "lost": 4 if fails else 0})
    # The end's flush: a rotation over plays.1.jnl (1-4, this session's,
    # no save covering them) with 3 s of the budget left, or 0.5 s (the
    # played file's question took 2.5 s): saved, or 1-4 lost.
    for left in (3000, 500):
        steps = [_boot("A", 1)] + _plays("A", names[:8], 1000, 2)
        steps += [dict(session_play("Signal Fires", at="2026-11-02T12:00:00", up=900000, player="A",
                                    now=ms_of("2026-11-02T12:05:00"), ending=True, left=left))]
        steps += [_boot("A", 3)]
        out.append({"name": "flush-in-time" if left >= 1000 else "flush-late",
                    "note": "The orderly end's flush writes the ninth play, which rotates over plays.1.jnl (1-4, "
                            "saved by nothing). %s" % (
                                "3 s of the budget left: the rotation's save runs first; none lost." if left >= 1000
                                else "0.5 s left: no save, and 1-4 are lost to the stats (4)."),
                    "cap": 4, "files": {}, "players": {"A": A()}, "steps": steps, "lost": 0 if left >= 1000 else 4})
    # A second player between two saves of the first.
    steps = [_boot("A", 1)] + _plays("A", names[:3], 1000, 2) + [_boot("A", 3)]
    steps += _plays("A", names[3:4], 1000, 3)
    steps += [_boot("B", 4, playing=False, safePoint=True)] + _plays("B", names[4:5], 1000, 4)
    steps += [{"do": "remount", "player": "A"}] + _plays("A", names[5:6], 2000000, 4)
    steps += [{"do": "hs", "player": "A", "now": ms_of("2026-11-04T09:30:00")}, _boot("A", 5)]
    out.append({"name": "second-player",
                "note": "A saves 1-3 (stats.bin generation 1) and plays 4. The card goes into B, which folds A's 4, "
                        "compacts (no base yet, nothing playing) and saves (generation 2), then plays B 1. Back in "
                        "A (a remount, its session going on), A plays 5: Hs is refused, the card's stats.bin no "
                        "longer the copy A's session stands on (5.5). A's next boot reads B's stats.bin and folds "
                        "B 1 and A 5: every slot once.",
                "cap": 4, "files": {}, "players": {"A": A(), "B": B()}, "steps": steps, "lost": 0})
    # NVS put back after a trim (C5's seq rule).
    acks4 = [(DEV_A, EPOCH_A, 4)]
    steps = [_boot("A", 1)] + _plays("A", names[:4], 1000, 2) + [_boot("A", 3, acks=acks4)]
    steps += [{"do": "nvs", "player": "A", "seq": 1}, _boot("A", 4, acks=acks4)]
    steps += _plays("A", names[4:5], 1000, 4) + [_boot("A", 5, acks=acks4)]
    out.append({"name": "nvs-back",
                "note": "A plays 1-4; a sync's ACKS (4); the boot folds, saves and trims the log empty. NVS is "
                        "then put back to seq 1 (a settings backup): the seq rule takes ACKS's and MARK's 4, so the "
                        "next play is 5 (not 2, which ACKS acknowledges and MARK covers: never sent, never "
                        "folded); the next boot folds it.",
                "cap": 4, "files": {}, "players": {"A": A()}, "steps": steps, "lost": 0})
    # Two players alternating.
    acks_ab = [(DEV_A, EPOCH_A, 3), (DEV_B, EPOCH_B, 2)]
    steps = [_boot("A", 1)] + _plays("A", names[:2], 1000, 1) + [_boot("B", 2)] + _plays("B", names[2:4], 1000, 2)
    steps += [_boot("A", 3)] + _plays("A", names[4:5], 1000, 3)
    steps += [_boot("B", 4, acks=acks_ab)] + _plays("B", names[5:6], 1000, 4) + [_boot("A", 5, acks=acks_ab)]
    out.append({"name": "alternating",
                "note": "A and B take the card in turns, each boot folding the other's plays, a sync's ACKS "
                        "between (A 3, B 2) trimming what the saved MARK covers: each slot once.",
                "cap": 4, "files": {}, "players": {"A": A(), "B": B()}, "steps": steps, "lost": 0})
    # A 0.14.0 boot's trim between.
    acks6 = [(DEV_A, EPOCH_A, 6)]
    steps = [_boot("A", 1)] + _plays("A", names[:3], 1000, 1) + [_boot("A", 2)]
    steps += _plays("A", names[3:6], 1000, 2)
    steps += [_boot("A", 3, acks=acks6, stats=False), _boot("A", 4, acks=acks6)]
    out.append({"name": "old-firmware",
                "note": "A saves 1-3, plays 4-6 (live, unsaved), then a sync's ACKS (6) and a boot of 0.14.0, "
                        "which trims both files without folding them. The next stats boot finds ACKS above MARK "
                        "with 4-6 in no file: HEAD's gap 3, MARK 6 (5.8's undercount, counted).",
                "cap": 4, "files": {}, "players": {"A": A()}, "steps": steps, "lost": 3})
    # A partial scan, a live play, Hs refused; then a whole boot.
    tail = scenario_partial()[2]
    acks14 = [(DEV_A, EPOCH_A, 14)]
    steps = [_boot("A", 10, acks=acks14, unread={"plays.jnl": 3})]
    steps += [dict(session_play("Night Ticket", at="2026-11-10T10:00:00", up=5000, player="A",
                                now=ms_of("2026-11-10T10:05:00")))]
    steps += [{"do": "hs", "player": "A", "now": ms_of("2026-11-10T10:06:00")}, _boot("A", 11, acks=acks14)]
    out.append({"name": "partial-hs",
                "note": "partial-tail's card: the boot's scan fails a read after plays.jnl's third slot (9-11 "
                        "folded, 12-14 not): no save; A then plays 15 and Hs is refused (a partial scan: no save "
                        "this session). The next boot reads it all: 12-15 folded once, saved.",
                "cap": playlog.FILE_SLOTS, "files": tail["files"],
                "players": {"A": A(nvs=[EPOCH_A, 14, 1])}, "steps": steps, "lost": 0})
    # firstAt through a rotation's save (the rows a boot's fold gives, but
    # lastOrd, compared by order: test_stats_store).
    xa = [("Harbour Lights", 3, None), ("Harbour Lights", 2, "2026-11-03T10:00:00"), ("Tidewater", 3, None)]
    xb = [("Harbour Lights", 1, "2026-11-02T10:00:00"), ("Tidewater", 2, None),
          ("North Pier", 1, "2026-11-05T10:00:00")]
    steps = [_boot("B", 6)]
    steps += [dict(session_play(t, src=s, at=at, up=1000 + 60000 * i), player="B") for i, (t, s, at) in enumerate(xb)]
    steps += [_boot("A", 7)]
    steps += [dict(session_play(t, src=s, at=at, up=1000 + 60000 * i), player="A") for i, (t, s, at) in enumerate(xa)]
    steps += [dict(session_play("North Pier", played=9000, outcome=2, at="2026-11-04T10:00:00", up=200000),
                   player="A")]
    steps += _plays("A", ["Salt Stairs", "Brine"], 300000, 7) + [_boot("A", 8)]
    out.append({"name": "first-at-live",
                "note": "first-at-a's seven slots, B's folded at A's boot and A's live (an undated Auto DJ play, "
                        "then a dated shuffle one, of Harbour Lights; an undated Auto DJ play of Tidewater; a skip "
                        "of North Pier), saved by a rotation's save (the log's cap 4: plays.1.jnl holds A's first, "
                        "which no save covers). Their rows are first-at-a's but lastOrd: the live plays' round is "
                        "HEAD.round + 2, the boot's + 1. The next boot folds the last play.",
                "cap": 4, "files": {}, "players": {"A": A(), "B": B()}, "steps": steps, "lost": 0})
    # A rotation after a partial scan: no save this session, so plays.1.jnl
    # leaves the log unsaved (5.5's undercount, 4).
    opened = scenario_partial()[0]
    steps = [_boot("A", 10, acks=[(DEV_A, EPOCH_A, 11)], unread={"plays.1.jnl": None})]
    steps += _plays("A", names[:2], 5000, 10) + [_boot("A", 11, acks=[(DEV_A, EPOCH_A, 11)])]
    out.append({"name": "partial-rotation",
                "note": "partial-open's card (the stats' MARK 4; plays.1.jnl 1-8, its open failed at this boot; "
                        "plays.jnl 9-11): no save this session. The second play rotates over plays.1.jnl, 5-8 "
                        "unsaved: no save is asked (a partial scan), and 5-8 are lost to the stats (4).",
                "cap": 4, "files": opened["files"], "players": {"A": A(nvs=[EPOCH_A, 11, 1])}, "steps": steps,
                "lost": 4})
    # The log deleted by hand (a PC), NVS put back: MARK alone holds the
    # seq (C5): the next play is 5, not 2 (under MARK: never folded).
    steps = [_boot("A", 1)] + _plays("A", names[:4], 1000, 1) + [_boot("A", 2)]
    steps += [{"do": "delete", "name": "plays.jnl"}, {"do": "nvs", "player": "A", "seq": 1}, _boot("A", 3)]
    steps += _plays("A", names[4:5], 1000, 3) + [_boot("A", 4)]
    out.append({"name": "log-deleted",
                "note": "A plays 1-4 and saves them (MARK 4, no sync). plays.jnl is then deleted on a PC and NVS "
                        "put back to seq 1: with no log and no ACKS, the seq rule's MARK keeps the next play at 5 "
                        "(2 would be under MARK: never folded).",
                "cap": 4, "files": {}, "players": {"A": A()}, "steps": steps, "lost": 0})
    # A new epoch with no ACKS, in the middle of a session's history.
    steps = [_boot("A", 1)] + _plays("A", names[:3], 1000, 1)
    steps += [{"do": "erase", "player": "A"}, _boot("A", 2)] + _plays("A", names[3:4], 1000, 2) + [_boot("A", 3)]
    out.append({"name": "new-epoch-live",
                "note": "A plays 1-3, then its NVS is erased: the next boot draws a new epoch, and with no ACKS "
                        "its scan still reads (0.14.0's didn't): 1-3 fold once under the old epoch's MARK row, and "
                        "the new epoch's 1 under its own at the boot after.",
                "cap": 4, "files": {}, "players": {"A": A(epochs=(EPOCH_A, EPOCH_A2))}, "steps": steps,
                "lost": 0})
    out += _records_sessions(A)
    return out


def _run(n, first, day, first_up=1000, skips=()):
    """n plays (session_play's), the i-th of the session's (from `first`,
    0-based) of NAMES[i % 9] at 08:00 + i minutes on 2026-11-<day>, `now`
    (the board's clock at a save it brings) 5 minutes after its start, its
    uptime first_up + 1 minute a play; those at the run's indexes in
    `skips` skipped at 9 s of 200 s (a skip: a record too)."""
    out, t0 = [], ms_of("2026-11-%02dT08:00:00" % day)
    for k in range(n):
        i = first + k
        st = session_play(NAMES[i % len(NAMES)], up=first_up + 60000 * k,
                          **(dict(played=9000, outcome=2) if k in skips else {}))
        st.update(at=t0 + 60000 * i, now=t0 + 60000 * i + 300000)
        out.append(st)
    return out


def _batch(player, plays, **kw):
    """plays (session_play's) as one "plays" step, its fields in columns."""
    st = {"do": "plays", "player": player, "plays": {k: [p[k] for p in plays] for k in PLAY_FIELDS}}
    st.update(kw)
    return st


def _one(player, play, **kw):
    """One of _run's plays as a "play" step."""
    return dict(play, player=player, **kw)


def _history(n, cap, day):
    """A card's /.player after A (EPOCH_A) wrote n plays with 0.14.0 (no
    stats) and never synced: its log (playlog's Device at `cap`, one play a
    minute from 06:00 on 2026-11-<day>) and A's NVS then ([epoch, seq,
    boots])."""
    d = playlog.Device(DEV_A, [EPOCH_A], cap)
    d.boot(clock=1)
    t0 = ms_of("2026-11-%02dT06:00:00" % day)
    for i in range(n):
        h = TRACKS[NAMES[i % len(NAMES)]][1]
        d.play(h, 4000000 + (h & 0xFFFFF), 0x5B684A00, 1, 200000, 200000, 1, 1, 1000 + 60000 * i,
               started_at=t0 + 60000 * i)
    return dict(d.files), [EPOCH_A, d.nvs["seq"], d.nvs["boots"]]


def _records_sessions(A):
    """5.14's records (S1's rework, R2; their counts SAVE_AT and RECORDS
    since the move to internal RAM): the records' save's one trigger and
    its waits, Held and its promotion, the boot overflow's tries, the new
    causes of behind, a section damaged under the session, and a library
    update that moves no stats byte."""
    out = []
    hs = lambda day, hour=12: {"do": "hs", "player": "A", "now": ms_of("2026-11-%02dT%02d:00:00" % (day, hour))}  # noqa
    S, R = SAVE_AT, RECORDS
    run = _run(S + 6, 0, 2)
    steps = [_boot("A", 1), _batch("A", run[:S - 1]), _one("A", run[S - 1]), _one("A", run[S]),
             _batch("A", run[S + 1:]), _boot("A", 3)]
    out.append({"name": "records-save",
                "note": "A plays %d: the %dth brings the records to %d, so the records' save runs at the next step, "
                        "before the %dth's slot, holding 1-%d (stats.bin generation 1); %d-%d are records again, "
                        "and the next boot folds those 6." % (S + 6, S, S, S + 1, S, S + 1, S + 6),
                "cap": playlog.FILE_SLOTS, "files": {}, "players": {"A": A()}, "steps": steps, "lost": 0})
    run = _run(R + 2, 0, 2)
    steps = [_boot("A", 1), _batch("A", run[:S]), _batch("A", run[S:R], saveFails=True),
             _one("A", run[R], saveFails=True), _one("A", run[R + 1]), hs(2), _boot("A", 3)]
    out.append({"name": "records-save-fails",
                "note": "A plays %d; the card is full from then on: the records' save is tried before each later "
                        "play's slot and fails (%d-%d). %d brings the records to %d; at %d the save fails again and "
                        "the slot has no room: not folded, and the session is behind its log (5.14): no record "
                        "(%d neither), no save (Hs refused). The next boot folds every one once: %d." % (
                            S, S + 1, R + 1, R, R, R + 1, R + 2, R + 2),
                "cap": playlog.FILE_SLOTS, "files": {}, "players": {"A": A()}, "steps": steps, "lost": 0})
    run = _run(S + 3, 0, 2)
    steps = [_boot("A", 1), _batch("A", run[:S - 1]), {"do": "fence", "player": "A"}, _batch("A", run[S - 1:S + 2]),
             hs(2, 10), {"do": "unfence", "player": "A"}, _one("A", run[S + 2]), _boot("A", 3)]
    out.append({"name": "records-save-fenced",
                "note": "A plays %d; a library update's fence goes up, and %d-%d are written under it: the records' "
                        "save, due from the %dth, waits for the fence, and Hs is refused under it. It comes down, and "
                        "the save runs at the next step, before %d's slot, holding %d. The next boot folds %d." % (
                            S - 1, S, S + 2, S, S + 3, S + 2, S + 3),
                "cap": playlog.FILE_SLOTS, "files": {}, "players": {"A": A()}, "steps": steps, "lost": 0})
    run = _run(R + 1, 0, 2, skips=(S + 4, S + 12))
    steps = [_boot("A", 1), _batch("A", run[:S]), {"do": "fence", "player": "A"}, _batch("A", run[S:]),
             {"do": "unfence", "player": "A"}, hs(2, 11), _boot("A", 3)]
    out.append({"name": "records-save-fenced-backlog",
                "note": "A plays %d (the records' save due), and a library update's fence goes up before it runs: "
                        "the Tracker's backlog and the plays that end under the fence, %d-%d (two of them, %d and "
                        "%d, skips), are written under it. %d brings the records to %d with the save still waiting: "
                        "it runs then, whatever the fence, holding 1-%d; %d is a record. Nothing behind; Hs saves "
                        "%d after the fence; the next boot folds nothing." % (
                            S, S + 1, R + 1, S + 5, S + 13, R, R, R, R + 1, R + 1),
                "cap": playlog.FILE_SLOTS, "files": {}, "players": {"A": A()}, "steps": steps, "lost": 0})
    run = _run(R + 1, 0, 2)
    steps = [_boot("A", 1), _batch("A", run[:S]), {"do": "fence", "player": "A"}, _batch("A", run[S:R - 1]),
             _one("A", run[R - 1], saveFails=True), {"do": "unfence", "player": "A"},
             {"do": "hs", "player": "A", "now": run[R - 1]["now"], "saveFails": True}, _one("A", run[R]),
             _boot("A", 3)]
    out.append({"name": "records-save-fenced-fails",
                "note": "A plays %d (the records' save due), and a library update's fence goes up before it runs: "
                        "%d-%d are written under it. %d brings the records to %d: the save runs then, whatever the "
                        "fence, and fails (a full card). The fence comes down. Hs, the card still full, fails, and "
                        "no records' save comes before it: one that didn't land is due again only after a later "
                        "record, and none came. %d, the card mended: still none due, so its slot finds the records "
                        "full: not folded, and the session is behind. The next boot folds 1-%d once." % (
                            S, S + 1, R, R, R, R + 1, R + 1),
                "cap": playlog.FILE_SLOTS, "files": {}, "players": {"A": A()}, "steps": steps, "lost": 0})
    run = _run(S + 1, 0, 2)
    steps = [_boot("A", 1), _batch("A", run[:S - 1]), _one("A", run[S - 1], ending=True, left=3000),
             _one("A", run[S], ending=True, left=3000), _boot("A", 3)]
    out.append({"name": "records-save-ending",
                "note": "A plays %d; the orderly end's flush writes the %dth, then the %dth: the records' save, due "
                        "from the %dth, is refused while the end flushes (its plays only take records). Nothing is "
                        "saved; the next boot folds them once: %d." % (S - 1, S, S + 1, S, S + 1),
                "cap": playlog.FILE_SLOTS, "files": {}, "players": {"A": A()}, "steps": steps, "lost": 0})
    n = S + 6  # (more than SAVE_AT, within RECORDS: no overflow)
    files, nvs = _history(n, playlog.FILE_SLOTS, 1)
    run = _run(1, n, 2)
    steps = [_boot("A", 2, saveFails=True), _one("A", run[0]), _boot("A", 3)]
    out.append({"name": "boot-unsaved-records",
                "note": "A's 0.14.0 left %d plays unsynced. The first stats boot folds them (%d records, no "
                        "overflow) and its save fails; the trim keeps the log. The records' save is due: it runs "
                        "before the first live slot (%d's), holding the %d. The next boot folds %d." % (
                            n, n, n + 1, n, n + 1),
                "cap": playlog.FILE_SLOTS, "files": files, "players": {"A": A(nvs=nvs)}, "steps": steps, "lost": 0})
    # Held: a save whose replace() fails (stats.bin's remove: it stays the
    # copy before), the tmp written whole and kept.
    pre, run = _run(2, 0, 2), _run(S + 3, 2, 4)
    for lost_tmp in (False, True):
        steps = [_boot("A", 1), _batch("A", pre), _boot("A", 3), _batch("A", run[:S]),
                 _one("A", run[S], replaceFails=True)]
        if lost_tmp:
            steps += [_one("A", run[S + 1]), {"do": "delete", "name": STATS_TMP}, hs(4, 10), _one("A", run[S + 2])]
        else:
            steps += [_batch("A", run[S + 1:]), hs(4)]
        steps += [_boot("A", 5)]
        out.append({"name": "held-lost" if lost_tmp else "records-save-held",
                    "note": "A saves 1-2 at a boot (generation 1, MARK 2), then plays 3-%d; the records' save, before "
                            "%d's slot, writes stats.tmp whole (generation 2) but replace() fails (stats.bin's remove: "
                            "it stays generation 1): held. The records are emptied, the card's MARK stays 2. %s" % (
                                S + 2, S + 3,
                                "%d is written; the tmp is then removed (a PC, or another player's settle()). Hs "
                                "finds no tmp and stats.bin the copy before: the session is behind (Hs refused, %d "
                                "not folded). The next boot folds 3-%d above MARK 2, once." % (S + 4, S + 5, S + 5)
                                if lost_tmp else
                                "%d-%d are written; Hs promotes the tmp (generation 2) and merges only %d-%d onto it "
                                "(generation 3). No play counted twice; the next boot folds nothing." % (
                                    S + 4, S + 5, S + 3, S + 5)),
                    "cap": playlog.FILE_SLOTS, "files": {}, "players": {"A": A()}, "steps": steps, "lost": 0})
    comp = scenario_compaction()[0]
    run = _run(2, 16, 11)
    steps = [_boot("A", 10, playing=False, safePoint=True, replaceFails=True), _batch("A", run), hs(11),
             _boot("A", 12)]
    out.append({"name": "boot-compaction-held",
                "note": "compaction's card (stats.bin's MARK 12 over base stats-00000001.bin of 6 rows; the log "
                        "1-16). The boot folds 13-16 and its save compacts: stats-00000002.bin written (base (+) "
                        "delta), then stats.tmp naming it, whose replace() fails: held (stats.bin still names base "
                        "1, which stays). A plays 17-18; Hs promotes the tmp and merges them: stats.bin names base 2, "
                        "its TRKD 17-18's rows, the old delta in the base. The next boot removes base 1 (named by "
                        "nothing, beside the named one) and folds nothing.",
                "cap": playlog.FILE_SLOTS, "files": comp["files"], "players": {"A": A(nvs=[EPOCH_A, 16, 1])},
                "steps": steps, "lost": 0})
    steps = [_boot("A", 1)] + _plays("A", NAMES[:8], 1000, 2)
    steps += [dict(hs(2), replaceFails=True), hs(2, 13)]
    steps += [dict(session_play(NAMES[8], at="2026-11-02T14:00:00", up=900000), player="A"), _boot("A", 3)]
    out.append({"name": "held-promoted-covers",
                "note": "The log's cap 4. A plays 1-8 (plays.1.jnl 1-4, plays.jnl 5-8; no stats.bin yet). Hs writes "
                        "stats.tmp whole, but its rename fails: held (no stats.bin; the card's MARK none). The next "
                        "Hs promotes it (stats.bin: 1-8, MARK 8) and has nothing more to write. 9 rotates over "
                        "plays.1.jnl, which the promoted copy's MARK covers: no save asked. The next boot folds 9.",
                "cap": 4, "files": {}, "players": {"A": A()}, "steps": steps, "lost": 0})
    run = _run(129, 0, 2)
    steps = [_boot("A", 1), _batch("A", run[:S - 1]), _one("A", run[S - 1]), _batch("A", run[S:R], saveFails=True),
             _one("A", run[R], saveFails=True), _batch("A", run[R + 1:128]), _one("A", run[128]), _boot("A", 3)]
    out.append({"name": "records-behind-rotation",
                "note": "The log's cap 64. A plays %d; the card is full for %d-%d: each records' save fails, and %d "
                        "finds the records full: the session is behind. The card mends; %d-128 aren't folded (no "
                        "record, no save). 129 rotates over plays.1.jnl (1-64, which no save covers): the rotation's "
                        "save is refused (behind), and 1-64 are lost to the stats (5.14's hole, 64). The next boot "
                        "folds 65-129." % (S, S + 1, R + 1, R + 1, R + 2),
                "cap": 64, "files": {}, "players": {"A": A()}, "steps": steps, "lost": 64})
    run = _run(R + 3, 0, 2)
    steps = [_boot("A", 1), _batch("A", run[:R + 1], saveFails=True), _one("A", run[R + 1]), _one("A", run[R + 2]),
             _boot("A", 3)]
    out.append({"name": "records-fail-rotations",
                "note": "The log's cap 8. The card is full from A's first play to its %dth: each rotation's save "
                        "fails (plays.1.jnl's plays, which no save covers, leave the log and are held only in the "
                        "records), and from %d each records' save fails too. %d finds the records full: the session "
                        "is behind, and the records go. The card mends; %d-%d aren't folded. The next boot folds "
                        "what the log still holds: every play the rotations took out before behind is lost to the "
                        "stats (5.14's hole: a rotation before behind loses plays too)." % (
                            R + 1, S + 1, R + 1, R + 2, R + 3),
                "cap": 8, "files": {}, "players": {"A": A()}, "steps": steps, "lost": 30})
    files, nvs = _history(300, playlog.FILE_SLOTS, 1)
    run = _run(1, 300, 2)
    for name, fails, note in (
            ("boot-overflow", 0, "its save lands at the first try: the overflow given back, nothing behind. A plays "
                                 "301, Hs saves it, and the next boot folds nothing."),
            ("boot-overflow-retry", 1, "its save fails once and lands at the second try (the boot's waits apart): "
                                       "nothing behind. A plays 301, Hs saves it, and the next boot folds nothing."),
            ("boot-overflow-unsaved", 4, "its save fails at all four tries: the overflow given back, the session "
                                         "behind (301 not folded, Hs refused). The next boot folds them once: 301.")):
        steps = [_boot("A", 2, saveFails=fails), _one("A", run[0]), hs(2, 14), _boot("A", 3)]
        out.append({"name": name,
                    "note": "A's 0.14.0 left 300 plays unsynced. The first stats boot folds them, past the records "
                            "(%d): overflow chunks; %s" % (R, note),
                    "cap": playlog.FILE_SLOTS, "files": files, "players": {"A": A(nvs=nvs)}, "steps": steps,
                    "lost": 0})
    steps = [_boot("A", 2, unread={"plays.jnl": 200}), _one("A", run[0]), hs(2, 14), _boot("A", 3)]
    out.append({"name": "boot-overflow-partial",
                "note": "The same 300 plays; the card fails a read after the scan's 200th slot: a partial scan, "
                        "past the records (%d), so its overflow goes at once (no save is asked, none tried again) "
                        "and the session is behind (301 not folded; Hs refused: the partial scan). The next boot "
                        "reads it all: 301 folded once." % R,
                "cap": playlog.FILE_SLOTS, "files": files, "players": {"A": A(nvs=nvs)}, "steps": steps, "lost": 0})
    files, nvs = _history(256, 128, 1)
    run = _run(1, 256, 2)
    for fails in (BOOT_TRIES, 1):
        unsaved = fails >= BOOT_TRIES
        steps = [_boot("A", 2, saveFails=fails), _one("A", run[0]), _boot("A", 3)]
        out.append({"name": "boot-overflow-full-rotation" + ("" if unsaved else "-retry"),
                    "note": "A full log (the cap 128: plays.1.jnl 1-128, plays.jnl 129-256), A's 0.14.0's, unsynced. "
                            "The first stats boot folds the 256 (overflow). %s" % (
                                "Its save fails at all four tries: behind. The first play (257) rotates over "
                                "plays.1.jnl, which no save covers: the rotation's save is refused, and 1-128 are "
                                "lost to the stats (5.14's hole, 128). The next boot folds 129-257." if unsaved
                                else "Its save fails once and lands at the second try (MARK 256). The first play "
                                     "(257) rotates over plays.1.jnl, which MARK covers: no save asked, none lost. "
                                     "The next boot folds 257."),
                    "cap": 128, "files": files, "players": {"A": A(nvs=nvs)}, "steps": steps,
                    "lost": 128 if unsaved else 0})
    steps = [_boot("A", 1)] + _plays("A", NAMES[:8], 1000, 2) + [_boot("A", 3, acks=[(DEV_A, EPOCH_A, 4)])]
    steps += _plays("A", NAMES[8:9], 1000, 4) + [{"do": "damage", "name": STATS_BIN, "section": "TRKD"}, hs(4)]
    steps += [dict(session_play(NAMES[0], at="2026-11-04T13:00:00", up=2000000), player="A"), _boot("A", 5)]
    out.append({"name": "section-damaged",
                "note": "The log's cap 4. A plays 1-8; the boot saves them (MARK 8; TRKD 8 rows) and ACKS (4) trims "
                        "plays.1.jnl (1-4): they are only in stats.bin now. A plays 9; then TRKD's first byte is "
                        "damaged on the card (its CRC fails, the header unchanged). Hs reads it: the session is "
                        "behind (Hs refused, 10 not folded). The next boot reads stats.bin Corrupt (stats.bad) and "
                        "counts from what the log still holds: 5-10; 1-4 are lost to the stats (4).",
                "cap": 4, "files": {}, "players": {"A": A()}, "steps": steps, "lost": 4})
    run = _run(S + 2, 0, 2)
    steps = [_boot("A", 1), _batch("A", run[:S]), {"do": "update", "player": "A"}, _batch("A", run[S:]), hs(2),
             _boot("A", 3)]
    out.append({"name": "update-nothing",
                "note": "A plays 1-%d (the records' save due), then a library update (its fence over no play), then "
                        "%d-%d: the update moves no stats byte (no give-back, no save, no read): the save due waits "
                        "out its fence and runs at the next play, before %d's slot, holding 1-%d. Hs saves %d-%d; "
                        "the next boot folds nothing." % (S, S + 1, S + 2, S + 1, S, S + 1, S + 2),
                "cap": playlog.FILE_SLOTS, "files": {}, "players": {"A": A()}, "steps": steps, "lost": 0})
    return out


def _hex64(v):
    return "0x%016x" % playlog.u64(v)


def run_session(sc):
    """Each step of a session in turn: (its sessions.json entry, {path:
    bytes}). The card after each step is stored as run_scenario's files
    are (each file once, in the first folder with those bytes). Each step
    of a player also gives the player's records after it and whether its
    session is behind."""
    card = _Card(sc["files"])
    # The slots the stats should hold at the end: those the card's stats
    # hold at the start, those of its log above their MARK (each once),
    # and those the steps write.
    first = Session(dict(sc["files"]))
    first.scan()
    expect = first.stats.head["slots"]
    players = {}
    for name, d in sorted(sc["players"].items()):
        p = _Player(card, d["device"], d["epochs"], sc["cap"])
        if d["nvs"] is not None:
            p.nvs = {"epoch": d["nvs"][0], "seq": d["nvs"][1], "boots": d["nvs"][2]}
        players[name] = p
    out, where = {}, {}

    def stored(folder):
        for n, b in sorted(card.files.items()):
            if n not in where or where[n][1] != b:
                where[n] = (folder, b)
                out["stats/sessions/%s/%s" % (folder, n)] = b
        return {n: where[n][0] for n in sorted(card.files)}

    entry = {"name": sc["name"], "note": sc["note"], "cap": sc["cap"],
             "players": {n: {"device": d["device"], "epochs": [_hex64(e) for e in d["epochs"]],
                             "nvs": None if d["nvs"] is None else [_hex64(d["nvs"][0]), d["nvs"][1], d["nvs"][2]]}
                         for n, d in sorted(sc["players"].items())},
             "in": stored("%s/0" % sc["name"]), "steps": []}
    written = 0

    def count(p, seq):
        return seq is not None and counts(dict(kind=playlog.KIND_PLAY, epoch=p.nvs["epoch"], seq=seq,
                                               deviceId=p.device_id))

    for i, st in enumerate(sc["steps"], 1):
        e = {k: v for k, v in st.items()}
        if "acks" in e:
            e["acks"] = acks_json(e["acks"])
        if e["do"] == "play":
            e["hash"] = _hex64(e["hash"])
        if e["do"] == "plays":
            e["plays"] = {k: _Inline([_hex64(h) for h in v] if k == "hash" else v) for k, v in e["plays"].items()}
        p = players.get(st.get("player"))
        if st["do"] == "boot":
            e.update(_session_boot(p, st))
        elif st["do"] == "play":
            e.update(_session_play(p, st))
            written += count(p, e["seq"])
        elif st["do"] == "plays":
            e.update(_session_plays(p, st))
            written += sum(count(p, q) for q in e["each"]["seq"])
        elif st["do"] == "hs":
            rec = _save(p, "records", st) if _records_due(p) and not p.fenced else None
            hs = _save(p, "hs", st)
            e.update(ok=hs in SAVED, hsSave=hs, recordsSave=rec)
        elif st["do"] == "fence":
            assert not p.fenced
            p.fenced = True
        elif st["do"] == "unfence":
            assert p.fenced
            p.fenced = False
        elif st["do"] == "update":
            assert not p.fenced  # a fence over no play: nothing of the stats
        elif st["do"] == "remount":
            assert p.appended, "a remount before the session's first slot begins the scan again"
            p.settle()
        elif st["do"] == "nvs":
            p.nvs["seq"] = st["seq"]
        elif st["do"] == "erase":
            p.erase_nvs()
        elif st["do"] == "delete":
            del card.files[st["name"]]
            card.chains.pop(st["name"], None)
        elif st["do"] == "link":
            _link(card, st["a"], st["b"])
        elif st["do"] == "damage":
            _damage(card, st["name"], st["section"])
        else:
            raise ValueError(st["do"])
        if p is not None:
            e["records"] = p.se.records if p.se else 0
            e["behind"] = bool(p.se and p.se.behind)
        e["out"] = stored("%s/%d" % (sc["name"], i))
        entry["steps"].append(e)
    # The undercount: slots that count, written, that the card's stats
    # don't hold (and with ACKS below MARK, counted in HEAD's gap).
    st = load(card.files)
    lost = expect + written - st.head["slots"]
    assert lost == sc["lost"], (sc["name"], lost, sc["lost"])
    entry["slots"], entry["lost"] = expect + written, lost
    return entry, out


def _json(obj) -> str:
    """sessions.json's text: json.dumps's, indent 1, with each _Inline list
    (a batch's columns) on one line."""
    rows = []

    def walk(v):
        if isinstance(v, _Inline):
            rows.append(json.dumps(list(v), ensure_ascii=False))
            return "\x00%d\x00" % (len(rows) - 1)
        if isinstance(v, dict):
            return {k: walk(x) for k, x in v.items()}
        if isinstance(v, list):
            return [walk(x) for x in v]
        return v
    text = json.dumps(walk(obj), indent=1, ensure_ascii=False)
    return re.sub(r'"\\u0000(\d+)\\u0000"', lambda m: rows[int(m.group(1))], text)


def session_files():
    """test/fixtures/card/stats/sessions.json and the sessions' files."""
    files, index = {}, []
    for sc in session_specs():
        entry, out = run_session(sc)
        index.append(entry)
        files.update(out)
    files["stats/sessions.json"] = (_json(index) + "\n").encode("utf-8")
    return files


# ---------------------------------------------------------------------------
# The rankers' scenarios (test/fixtures/card/stats/rank.json; S2's C12): a
# card's stats.bin and its base as a long history leaves them, every list
# past its cap and 16, and the lists the rankers MUST give over them; since
# S3a's V1, the totals, the 7 days and their span, and the u40 played set.
# ---------------------------------------------------------------------------
RANK_TODAY = 320                        # 2026-11-17


def _rank_stats(seed, today=RANK_TODAY, tracks=900, base_share=0.7, delta=320, rcnt=1800, ties=False,
                days_from=None, days_share=0.8):
    """A card's stats as a long history leaves them: `tracks` hashes, about
    base_share of them in the base (compacted), `delta` rows in TRKD (some
    over a base row, some new), RCNT's rows over the 32 days to `today` and
    the day after it (a clock ahead), none before day 0. Rows of every kind: dated, undated
    (lastAt 0, by lastOrd), skips only, old favourites, new finds. ties:
    plays and times from a few values, so the hash decides. DAYS (S3a): a
    row on about days_share of the days from days_from (60 days back by
    default, not before day 0) to the day after today, from a generator of
    its own, so the rows above are the ones S2's fixtures had."""
    rng = random.Random(seed)
    hashes = sorted({rng.getrandbits(64) | 1 for _ in range(tracks)})
    day_s = 86400

    def t_of(day):
        return day * day_s + rng.randrange(day_s) + 1 if not ties else day * day_s + 1

    def row():
        k = rng.random()
        if k < 0.06:
            return Row(0, rng.randint(1, 4), NO_FIRST, 0, 0, YOU)       # skips only
        plays = rng.choice([1, 2, 3, 5, 8]) if ties else rng.randint(1, 30)
        src = rng.choice([YOU, SHUFFLE, AUTODJ])
        if k < 0.14:
            return Row(plays, rng.randint(0, 2), 0, 0, rng.randint(1, 900), src)   # undated
        if k < 0.34 and not ties:  # found in the 30 days
            first_day = rng.randint(max(0, today - 29), today)
            last_day = rng.randint(first_day, today)
        else:
            last_day = max(0, rng.choice([today, today - 3, today - 40]) if ties else rng.randint(today - 400, today))
            first_day = last_day if ties else rng.randint(max(0, last_day - 300), last_day)
        first, last = t_of(first_day), t_of(last_day)
        if first > last:
            first, last = last, first
        return Row(plays, rng.randint(0, 3), first, last, rng.randint(1, 900), src)

    st = Stats()
    for h in hashes:
        if rng.random() < base_share:
            st.base[h] = row()
    in_base = sorted(st.base)
    for h in rng.sample(in_base, min(delta // 2, len(in_base))):
        st.trkd[h] = row()                                              # over a base row
    rest = [h for h in hashes if h not in st.base]
    for h in rest[:delta - min(delta // 2, len(in_base))]:
        st.trkd[h] = row()                                              # new since the base
    played = sorted(h for h, r in st.tracks().items() if r.plays)  # (sorted: a set's order is CPython's)
    while len(st.rcnt) < rcnt:
        day = rng.randint(max(0, today - 31), today + 1)
        st.rcnt[(day, rng.choice(played))] = rng.choice([1, 2]) if ties else rng.randint(1, 6)
    drng = random.Random(seed + 1000)
    for day in range(max(0, today - 60) if days_from is None else days_from, today + 2):
        if drng.random() < days_share:
            plays = drng.randint(0, 60)
            adj, shuffle = drng.randint(0, min(plays, 40)), drng.randint(0, min(plays, 20))
            # [plays, skips, Auto DJ's plays and skips, listenedMs, shuffle's plays and skips]
            st.days[day] = [plays, drng.randint(0, 9), adj, drng.randint(0, 5),
                            plays * drng.randint(30000, 400000) + drng.randint(0, 120000), shuffle, drng.randint(0, 3)]
    st.generation = 9
    if st.base:
        st.head["baseGen"], st.head["baseRows"] = 8, len(st.base)
        st.head["baseCrc"] = header_crc(write_base(st.base, 8))
    st.head["sinceDay"] = max(0, today - 400)
    return st


def _rank_days(today=500):
    """DAYS at its 400 rows (5.2's retention, the newest kept): a row on
    most days from day 40 to the day after today, then retain(), as a save
    leaves them."""
    st = _rank_stats(6, today=today, tracks=400, delta=200, rcnt=900, days_from=40, days_share=0.92)
    if len(st.days) <= DAYS_ROWS:
        raise ValueError("rank-days: %d DAYS rows, not past %d" % (len(st.days), DAYS_ROWS))
    retain(st, today)
    return st


def _rank_midnight(today=RANK_TODAY):
    """Plays around UTC midnights, folded as a boot folds them (Stats.fold):
    a play counts whole on the day it began (4.1), one that runs past
    midnight too. No base."""
    def at(day, h, m, s, ms=0):
        return dict(startedAt=MIN_MS + day * DAY_MS + ((h * 60 + m) * 60 + s) * 1000 + ms, clock=1)

    a = lambda seq, *args, **kw: slot(DEV_A, EPOCH_A, seq, *args, **kw)  # noqa: E731
    slots = [
        a(1, "Night Ticket", 1200000, 1500000, src=3, **at(today - 1, 23, 50, 0)),    # Auto DJ, 20 min past midnight
        a(2, "Brine", 10000, 200000, outcome=2, src=3, **at(today - 1, 23, 59, 59, 999)),  # an Auto DJ skip
        a(3, "Low Tide", 200000, 200000, src=2, **at(today, 0, 0, 0)),                 # shuffle, at midnight
        a(4, "Orbit Room", 240000, 240000, **at(today, 23, 59, 59)),                   # today's, into tomorrow
        a(5, "Paper Comets", 1800000, 1800000, **at(today - 7, 23, 58, 0)),            # into the window's first day
        a(6, "Quiet Engine", 200000, 200000, src=3, **at(today - 6, 0, 0, 0)),         # the window's first day
        a(7, "Signal Fires", 5000, 200000, outcome=3, **at(today - 3, 12, 0, 0)),      # stopped under the line
        a(8, "Glass Harbour", 200000, 200000, **at(today + 1, 0, 10, 0)),              # a clock ahead
        a(9, "Slow Lantern", 200000, 200000),                                          # undated
        a(10, "Copper Season", 200000, 200000, **at(today - 40, 9, 0, 0)),             # the span's first day
        a(11, "Orbit Room", 300000, 300000, src=3, **at(today - 1, 0, 0, 0)),
    ]
    st = Stats()
    for s in slots:
        st.fold(s, 1)
    st.generation = 9
    st.head["sinceDay"] = today - 40
    return st


def _rank_rcnt_cap(today=RANK_TODAY):
    """RCNT capped (5.2): 300 played tracks on each of the last 20 days,
    6,000 rows, so retain() drops whole days, oldest first, to 3,900: RCNT
    holds 13 days, and its oldest is inside the 30 days (7.6's
    stats.days_n)."""
    st = _rank_stats(5, rcnt=0)
    rng = random.Random(5 + 2000)
    played = sorted(h for h, r in st.tracks().items() if r.plays)
    for day in range(today - 19, today + 1):
        for h in rng.sample(played, 300):
            st.rcnt[(day, h)] = rng.randint(1, 3)
    retain(st, today)
    return st


RANK_LOADED = 900                       # rank-no-clock-session: the round its session loaded


def _rank_session():
    """This session's plays with the clock unset, as its live save leaves
    the card (5.9; the owner's call, 2026-11-24: with no clock Recently
    played puts this session's plays first, 4.3's "This session", then
    "Earlier"). A history from earlier sessions, rounds 1 to 900: 432
    tracks, 365 of them dated and played (over 216); then this session,
    which loaded round 900: the boot's fold of an earlier session's undated
    play (round 901), and its own plays, all undated, round 902
    (Fold::liveRound(), its live save's HEAD round): three tracks never
    played before (one twice), one played before but never dated, the
    oldest and the newest dated tracks played again (lastAt kept), and a
    skip of a dated one (no play: not this session's). (stats, live
    round)."""
    st = _rank_stats(7, tracks=500, delta=200, rcnt=600)
    live = RANK_LOADED + 2
    rows = st.tracks()
    dated_rows = sorted((r.lastAt, h) for h, r in rows.items() if r.plays and r.lastAt)
    never = sorted(h for h, r in rows.items() if r.plays and not r.lastAt)
    rng = random.Random(7 + 3000)
    fresh = sorted({rng.getrandbits(64) | 1 for _ in range(3)} - set(rows))
    a = lambda seq, h, **kw: slot(DEV_A, EPOCH_A, seq, h, 200000, 200000, **kw)  # noqa: E731
    st.fold(a(1, never[1]), RANK_LOADED + 1)                    # the boot's fold: an earlier session's play
    for seq, h in enumerate([fresh[0], dated_rows[0][1], never[0], fresh[1], dated_rows[-1][1], fresh[2],
                             fresh[0]], 2):
        st.fold(a(seq, h), live)
    st.fold(slot(DEV_A, EPOCH_A, 9, dated_rows[5][1], 10000, 200000, outcome=SKIPPED), live)  # a skip
    st.head["round"] = live
    st.generation = 10
    return st, live


def rank_scenarios():
    """(name, note, stats, today, liveRound): the rankers' scenarios; with
    the clock unset, the ranking session's live round (Fold::liveRound())."""
    big = _rank_stats(1)
    big.days[RANK_TODAY - 2] = [M16, 7, M8, M8, M32, 3, 1]  # a row at its fields' tops, in the 7 days
    session, live = _rank_session()
    return [
        ("rank-big", "900 hashes: 635 in the base, 320 TRKD rows (160 over a base row), 1,800 RCNT rows over "
         "the 32 days to day 320 and day 321 (a clock ahead): every list past its cap and 16. DAYS on most of the "
         "60 days to day 321, day 318's row at its fields' tops", big, RANK_TODAY, None),
        ("rank-no-clock", "rank-big's files with the clock unset: Most played All time and Recently played "
         "alone; no 7 days, the span still read", big, None, live_round_of(big)),
        ("rank-ties", "plays, days and times from a few values: the hash decides most places, the caps' edges "
         "among them", _rank_stats(2, ties=True), RANK_TODAY, None),
        ("rank-early", "a history to day 12: the windows begin before 2026-01-01", _rank_stats(3, today=12), 12,
         None),
        ("rank-no-base", "TRKD alone (no base yet), 300 hashes", _rank_stats(4, tracks=300, base_share=0,
                                                                            delta=300, rcnt=700), RANK_TODAY,
         None),
        ("rank-days", "DAYS at its 400 rows (the newest kept, day 501 a clock ahead), today day 500",
         _rank_days(), 500, None),
        ("rank-midnight", "plays folded around UTC midnights: each counts whole on the day it began (an Auto DJ "
         "play from 23:50 on day 319, a skip at 23:59:59.999, a shuffle play at 00:00 on day 320, one from "
         "23:59:59 on day 320, one from 23:58 on day 313, outside the 7 days); a stop under the line, an undated "
         "play, a clock ahead", _rank_midnight(), RANK_TODAY, None),
        ("rank-rcnt-cap", "RCNT capped: 300 tracks a day over 20 days, the oldest 7 dropped whole, so its oldest "
         "day is 308, inside the 30 days", _rank_rcnt_cap(), RANK_TODAY, None),
        ("rank-no-clock-session", "this session's plays with the clock unset, as its live save leaves the card "
         "(its HEAD round, 902, the session's live round: it loaded 900), ranked by that session: three tracks "
         "never played before, one played before but never dated, the oldest and the newest dated tracks played "
         "again, all undated, first (This session), then the rest (Earlier) by round, newest first: the boot's "
         "fold of an earlier undated play (round 901), then the rows of rounds up to 900, over 216 of them "
         "dated; a skip moves no row", session, None, live),
        ("rank-no-clock-next", "rank-no-clock-session's card ranked by the next session, the clock still unset, "
         "before it plays (it loads 902: live round 904): no row is this session's; Earlier by round, newest "
         "first: the last session's six rows (two dated, four never dated) lead it, then round 901's, though the "
         "card has over 216 dated tracks", session, None, live_round_of(session)),
    ]


def rank_files():
    """test/fixtures/card/stats/rank.json and the scenarios' files."""
    files, index, where = {}, [], {}
    for name, note, st, today, live in rank_scenarios():
        bin_b = write_stats(st)
        folder = where.get(bin_b)
        if folder is None:
            folder = where[bin_b] = "rank/" + name
            files["stats/%s/stats.bin" % folder] = bin_b
            if st.base:
                files["stats/%s/%s" % (folder, base_name(st.head["baseGen"]))] = write_base(st.base,
                                                                                     st.head["baseGen"])
        rows = st.tracks()
        w = window(today, 30)
        entry = {"name": name, "note": note, "in": folder, "today": today}
        if today is None:
            entry["liveRound"] = live
        entry.update({"rows": {"base": len(st.base), "trkd": len(st.trkd), "rcnt": len(st.rcnt),
                               "played": sum(1 for r in rows.values() if r.plays), "days": len(st.days),
                               "tracks": len(rows),
                               "window": 0 if w is None else sum(1 for (d, _), n in st.rcnt.items()
                                                                 if n and w[0] <= d <= w[1])},
                      "lists": lists_json(lists(st, today, live_round=live)), "totals": totals(st, today, rows),
                      "days7": days7(st, today), "played40": played40_bytes(played40(st, rows)).hex()})
        index.append(entry)
    files["stats/rank.json"] = (json.dumps(index, indent=1) + "\n").encode("utf-8")
    return files


def fixture_files():
    """test/fixtures/card/stats/: {path under test/fixtures/card: bytes}."""
    files, index = {}, []
    for sc in scenarios():
        entry, out = run_scenario(sc)
        index.append(entry)
        files.update(out)
    files.update(rank_files())
    lib = [{"album": album, "artist": artist, "tracks": [{"path": "%s/%s/%s" % (artist, album, f),
                                                          "hash": hx(path_hash("%s/%s/%s" % (artist, album, f)))}
                                                         for f in fs]}
           for artist, album, fs in sorted(LIBRARY, key=lambda a: a[1].casefold())]
    files.update(session_files())
    files["stats/library.json"] = (json.dumps(lib, indent=1, ensure_ascii=False) + "\n").encode("utf-8")
    files["stats/index.json"] = (json.dumps(index, indent=1, ensure_ascii=False) + "\n").encode("utf-8")
    return files


# ---------------------------------------------------------------------------
# seed: a test card's history (S1-S3's device passes)
# ---------------------------------------------------------------------------
SEED_DEVICE = "02005e0053a1"            # made-up, locally administered: never a board's
SEED_OLD_DEVICE = "02005e0053a2"        # the older history behind --base
TWIN_NAMES = playlog.TWINS
STATS_TWINS = ["stats.xl%d" % n for n in range(1, 10)]   # stats.bin's twins (METADATA 2.12.6)


class Refused(Exception):
    """seed won't do it; the message says why."""


def seed_card(card, days=40, per_day=25, slots=None, device=SEED_DEVICE, epoch=None, until=None, distinct=5000,
              base=None, base_days=365, seed=1, dry_run=False, replace=False, probe=None, out=None):
    """Writes a history into CARD's /.player: plays.jnl and plays.1.jnl of
    `slots` dated plays (default days x per_day) by another deviceId over
    the card's /music, ending on `until`'s day (Unix ms; default now), each
    file to the contract's cap, as playlog.Device writes them (a boot a
    day, the clock set). With base=ROWS, also a stats.bin and its base of
    ROWS rows: the stats an earlier history left (another made-up device,
    over base_days days before the log's), compacted, so the boot folds the
    log on top. Deterministic from `seed` and `until`.

    The guards: a drive root is never the system drive, and FAT32 (tools/
    synthtransfer.py's check_drive); the card has SYNTHCARD.TXT at its root
    (tools/synthcard.py made it) and a /.mstream root tools/synthtransfer.py
    wrote (its producer). Seeded plays on a card the listener's transfer
    reads would be posted to their mStream at the next sync. Log files (and
    with base, stats files) already there are refused unless replace; a
    twin always is (plays.xl1-9, and with base stats.xl1-9): it may share a
    chain with a file this would remove, so a disk check first.
    dry_run writes nothing. Returns a summary dict."""
    import synthcard
    import synthtransfer as stt  # here: it imports card_fixtures, which imports this file
    out = out or sys.stdout
    w = out.write
    card = stt.card_root(card)
    if not os.path.isdir(card):
        raise Refused("%s: not a folder" % card)
    try:
        if stt.is_drive_root(card):
            stt.check_drive(card, probe or synthcard.DriveProbe(), w)
        line = stt.synthcard_line(card)
        if line is None:
            raise Refused("%s: no SYNTHCARD.TXT at its root: not a tools/synthcard.py card, refusing" % card)
        _, _, root, _ = stt.mstream_state(card)
    except stt.Refused as e:
        raise Refused(str(e))
    if root is None:
        raise Refused("%s: no /.mstream root that reads: tools/synthtransfer.py hasn't written this card, "
                      "refusing" % card)
    if not (root["producer"] or "").startswith(stt.PRODUCER.rsplit(" ", 1)[0]):
        raise Refused("%s: its /.mstream root was written by %r, not tools/synthtransfer.py: refusing (seeded "
                      "plays would reach a real server at the next sync)" % (card, root["producer"]))
    w("card %s: %s; root generation %d by %s\n" % (card, line, root["generation"], root["producer"]))
    tracks, _ = stt.walk_music(card)
    if not tracks:
        raise Refused("%s: no tracks under music" % card)
    player = os.path.join(card, ".player")
    present = sorted(os.listdir(player)) if os.path.isdir(player) else []
    twins = [n for n in present if n.lower() in TWIN_NAMES or (base is not None and n.lower() in STATS_TWINS)]
    if twins:
        raise Refused("%s holds %s (a cut rename's twin, which may share a chain): run a disk check first" % (
            player, ", ".join(twins)))
    ours = [n for n in present if n.lower() in LOG_NAMES]
    if base is not None:
        ours += [n for n in present if n.lower() in (STATS_BIN, STATS_TMP) or
                 (n.lower().startswith("stats-") and n.lower().endswith(".bin"))]
    if ours and not replace:
        raise Refused("%s already holds %s: refusing (--replace removes them first)" % (player, ", ".join(ours)))

    rng = random.Random("statsref seed %d" % seed)
    epoch = epoch if epoch is not None else rng.getrandbits(64) | 1
    n = slots if slots is not None else days * per_day
    if n < 1 or days < 1:
        raise Refused("nothing to seed: --slots and --days at least 1")
    if until is None:
        import time
        until = int(time.time() * 1000)
    if not believable(until):
        raise Refused("--until: a time from 2026 to 2099")
    last_day = day_of(until)
    first_day = last_day - days + 1
    if first_day < 0:
        raise Refused("--days: the history would start before 2026")
    pool = rng.sample(tracks, min(distinct, len(tracks)))
    weights = [1.0 / (i + 1) ** 0.8 for i in range(len(pool))]
    jsonl = os.path.join(card, stt.TAGS_JSONL)
    lengths = stt.read_lengths(jsonl) if os.path.isfile(jsonl) else {}
    stat_cache = {}

    def facts(rel):
        if rel not in stat_cache:
            stat_cache[rel] = stt.file_stat(card, rel)
        return stat_cache[rel]

    dev = playlog.Device(device, [epoch], cap=playlog.FILE_SLOTS)
    picks = rng.choices(pool, weights=weights, k=n)
    at = 0
    for d in range(days):
        k = n // days + (1 if d < n % days else 0)
        if k == 0:
            continue
        day0 = MIN_MS + (first_day + d) * DAY_MS
        dev.boot(clock=1)
        step = 17 * HOUR_MS // k
        for i in range(k):
            rel = picks[at]
            at += 1
            dur = lengths.get(rel) or 150000 + rng.randrange(150000)
            start = day0 + 6 * HOUR_MS + i * step + rng.randrange(max(1, step // 4))
            r = rng.random()
            if r < 0.8:
                outcome, played = 1, dur
            elif r < 0.92:
                outcome, played = 2, 3000 + rng.randrange(22000)
            else:
                outcome, played = 3, dur * (30 + rng.randrange(60)) // 100
            played = min(played, step)
            src = rng.choices((1, 2, 3), weights=(5, 3, 2))[0]
            size, fat = facts(rel)
            dev.play(path_hash(rel), size, fat, root["generation"], played, dur, outcome, src,
                     uptime_ms=start - day0, started_at=start)
    write = {name: dev.files[name] for name in LOG_NAMES if name in dev.files}
    summary = {"device": device, "epoch": "0x%016x" % epoch, "slots": n, "days": [first_day, last_day],
               "distinct": len(set(picks)), "files": {k: len(v) // playlog.SLOT for k, v in write.items()}}
    if base is not None:
        stb, info = seed_base(tracks, base, base_days, first_day, rng)
        write.update(stb)
        summary["base"] = info
    w("plays: %d slots by %s epoch 0x%016x over days %d-%d (%s to %s UTC), %d distinct tracks of %d\n" % (
        n, device, epoch, first_day, last_day, iso_day(first_day), iso_day(last_day), summary["distinct"],
        len(tracks)))
    if base is not None:
        b = summary["base"]
        w("base: %d rows (%d orphans) from %d slots by %s, sinceDay %d (%s)\n" % (
            b["rows"], b["orphans"], b["slots"], b["device"], b["sinceDay"], iso_day(b["sinceDay"])))
    for name in ours:
        w("%s /.player/%s\n" % ("would remove" if dry_run else "removing", name))
        if not dry_run:
            os.remove(os.path.join(player, name))
    if not dry_run:
        os.makedirs(player, exist_ok=True)
    for name, data in sorted(write.items()):
        w("%s /.player/%s: %d bytes%s\n" % ("would write" if dry_run else "writing", name, len(data),
                                             " (%d slots)" % (len(data) // playlog.SLOT) if name in LOG_NAMES else ""))
        if not dry_run:
            stt.write_file(os.path.join(player, name), data)
    return summary


def iso_day(day):
    import datetime
    return (datetime.datetime(2026, 1, 1) + datetime.timedelta(days=day)).strftime("%Y-%m-%d")


def seed_base(tracks, rows, days, before_day, rng):
    """--base: {stats.bin, stats-00000001.bin} of `rows` per-track rows, the
    fold of a made-up older history (SEED_OLD_DEVICE, 1-7 slots a track,
    some of them skips, dated over `days` days before `before_day`),
    compacted. The card's tracks first; past them, made-up paths no track
    has (rows a rename left: orphans)."""
    rels = rng.sample(tracks, min(rows, len(tracks)))
    rels += ["Seeded/Gone %05d.mp3" % i for i in range(rows - len(rels))]
    epoch = rng.getrandbits(64) | 1
    slots, seq = [], 0
    for rel in rels:
        h = path_hash(rel)
        for k in range(1 + rng.randrange(6) + (rng.random() < 0.15)):
            seq += 1
            day = max(0, before_day - 1 - rng.randrange(days))
            skip = k > 0 and rng.random() < 0.15
            slots.append({"kind": playlog.KIND_PLAY, "deviceId": SEED_OLD_DEVICE, "epoch": epoch, "seq": seq,
                          "outcome": 2 if skip else 1, "source": rng.choice((1, 2, 3)),
                          "startedAt": MIN_MS + day * DAY_MS + rng.randrange(DAY_MS),
                          "playedMs": 8000 if skip else 200000, "durationMs": 200000, "pathHash": h})
    files = earlier(slots, now=MIN_MS + before_day * DAY_MS - 1)
    st = load(files)
    return files, {"rows": len(rels), "slots": seq, "orphans": rows - min(rows, len(tracks)),
                   "device": SEED_OLD_DEVICE, "epoch": "0x%016x" % epoch, "sinceDay": st.head["sinceDay"]}


# ---------------------------------------------------------------------------
# The command line
# ---------------------------------------------------------------------------
def player_files(d):
    """A copy of a card's /.player: the log's files and the stats' ({name:
    bytes}; stats.tmp too, which the read settles first)."""
    out = {}
    for name in sorted(os.listdir(d)):
        low = name.lower()
        base = low.startswith("stats-") and low.endswith(".bin")
        if low in LOG_NAMES or low in (STATS_BIN, STATS_BAD, STATS_TMP) or base:
            with open(os.path.join(d, name), "rb") as fh:
                out[low] = fh.read()
    return out


def parse_time(v):
    """Unix ms, or an ISO time (UTC): '2026-11-10T09:00:00', or with its
    UTC designator, '2026-11-10T09:00:00Z' (as other tools print it)."""
    if v is None:
        return None
    if v.isdigit():
        return int(v)
    return ms_of(v[:-1] if v[-1:] in ("Z", "z") else v)


def parse_acks(v):
    """DEV:EPOCH:SEQ[,...]: a deviceId's 12 hex digits, the epoch in hex,
    the seq in decimal."""
    out = []
    for part in filter(None, (v or "").split(",")):
        d, e, s = part.split(":")
        out.append((playlog.device_bytes(d).hex(), int(e, 16), int(s)))
    return out


def dump(stats_path, base_path=None, rows=False):
    """stats.bin as the device reads it, as JSON on stdout. The base is
    BASE when given, else the stats-<baseGen>.bin beside it; either is read
    under the name stats.bin gives it (whatever its own name), so a base
    that isn't the one named reads as lost, as on the card. With no base
    found, HEAD is printed as read ("base": "not given"), never as lost.
    0 when it reads."""
    data = Path(stats_path).read_bytes()
    try:
        st = read_stats(data)
    except Bad as e:
        print(json.dumps({"read": verdict(e.why), "why": e.why}, indent=1))
        return 1
    gen = st.head["baseGen"]
    if base_path is None and gen:
        beside = Path(stats_path).with_name(base_name(gen))
        base_path = beside if beside.is_file() else None
    if not gen:
        res = {"read": "loaded", "why": None, "base": "none named", "stats": stats_json(st, rows)}
    elif base_path is None:
        res = {"read": "loaded", "why": None, "base": "not given", "stats": stats_json(st, rows)}
    else:
        se = Session({STATS_BIN: data, base_name(gen): Path(base_path).read_bytes()})
        res = {"read": se.read, "why": se.why, "base": str(base_path)}
        if se.read == "loaded":
            res["stats"] = stats_json(se.stats, rows)
    print(json.dumps(res, indent=1))
    return 0 if res["read"] == "loaded" else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description="Listening stats (docs/LISTENING-STATS.md): the reference.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("dump", help="a stats.bin (and its base) as the device reads them")
    d.add_argument("stats")
    d.add_argument("base", nargs="?", help="its base file (default: the stats-<gen>.bin stats.bin names, beside it)")
    d.add_argument("--rows", action="store_true", help="every section's rows, not their counts")
    b = sub.add_parser("boot", help="a boot over a copy of a card's /.player; nothing is written")
    b.add_argument("dir")
    b.add_argument("--now", help="the board's clock at the save: Unix ms or an ISO time (UTC); none: unset")
    b.add_argument("--acks", help="the root's ACKS: DEV:EPOCH:SEQ[,...] (epoch in hex)")
    b.add_argument("--commit-time", type=int, help="the root's commitTime (Unix s)")
    b.add_argument("--playing", action="store_true", help="a track plays at the save (no compaction)")
    s = sub.add_parser("seed", help="a test card's play log (and with --base its stats)")
    s.add_argument("card", help="the card's root (E:\\) or a synthcard tree")
    s.add_argument("--days", type=int, default=40)
    s.add_argument("--per-day", type=int, default=25)
    s.add_argument("--slots", type=int, help="slots in all (default days x per-day)")
    s.add_argument("--full", action="store_true", help="a full log: 13,106 slots, both files at the cap")
    s.add_argument("--device", default=SEED_DEVICE, help="the deviceId, 12 hex digits (default %s)" % SEED_DEVICE)
    s.add_argument("--epoch", help="the epoch, hex (default from --seed)")
    s.add_argument("--until", help="the last day's time: Unix ms or an ISO time (UTC); default now")
    s.add_argument("--distinct", type=int, default=5000, help="tracks played at most (default 5,000)")
    s.add_argument("--base", type=int, metavar="ROWS", help="also a stats.bin and a base of ROWS rows")
    s.add_argument("--base-days", type=int, default=365)
    s.add_argument("--seed", type=int, default=1)
    s.add_argument("--dry-run", action="store_true", help="say what it would write; write nothing")
    s.add_argument("--replace", action="store_true",
                   help="remove the log's files (and with --base the stats') first")
    a = ap.parse_args(argv)
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass
    if a.cmd == "dump":
        return dump(a.stats, a.base, a.rows)
    if a.cmd == "boot":
        now = parse_time(a.now)
        r = boot(player_files(a.dir), parse_acks(a.acks), now, a.commit_time, a.playing, not a.playing)
        res = {k: r[k] for k in ("read", "why", "folded", "twice", "gap", "saved", "compacted", "removed", "trimmed")}
        st = load(r["files"])
        if st is not None:
            res["stats"] = stats_json(st)
            res["lists"] = lists_json(lists(st, today_of(now)))
            res["view"] = view(st, today_of(now))
        print(json.dumps(res, indent=1))
        return 0
    try:
        slots = 2 * playlog.FILE_SLOTS if a.full else a.slots
        seed_card(a.card, a.days, a.per_day, slots, playlog.device_bytes(a.device).hex(),
                  int(a.epoch, 16) if a.epoch else None, parse_time(a.until), a.distinct, a.base, a.base_days,
                  a.seed, a.dry_run, a.replace)
    except Refused as e:
        print("statsref seed: %s" % e, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
