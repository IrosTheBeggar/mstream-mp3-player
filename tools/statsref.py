#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Listening stats (docs/LISTENING-STATS.md): the reference.

Every rule the firmware's stats must reproduce is here in its plainest
form, with the plan's section it implements: what counts (4.1), the fold
of a slot into the aggregate and each slot once (5.6), the merge of two
per-track rows (5.2), `/.player/stats.bin` and its base file byte for
byte (5.2, 5.3), the boot (5.4-5.6: the read, the fold above MARK, the
save, the trim, the seq rule), the shelves' rankers (4.3) and the
windows (4.1, 4.2).
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
    the MARK read at the boot."""

    def __init__(self, files, acks=(), commit_time=None, unread=None):
        self.files = files
        self.acks = list(acks)
        self.commit_time = commit_time
        self.unread = dict(unread or {})
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

    def _load(self):
        self.stats = Stats()
        data = self.files.get(STATS_BIN)
        if data is None:
            return "absent", None
        try:
            st = read_stats(data)
        except Bad as e:
            if verdict(e.why) == "newer":
                return "newer", e.why
            self.files.pop(STATS_BAD, None)
            self.files[STATS_BAD] = self.files.pop(STATS_BIN)
            return "corrupt", e.why
        h = st.head
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
        self.used = rnd if self.used is None else max(self.used, rnd)
        self.new = True
        if is_play(s) and dated(s):
            d = day_of(s["startedAt"])
            self.first_play_day = d if self.first_play_day is None else min(self.first_play_day, d)

    def live(self, s):
        """A play written this session (Outbox::done(), Written only): folded
        at round HEAD.round + 2, in RAM only."""
        if self.on and counts(s):
            self.fold(s, sat(self.round + 2, M24))

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
    def save(self, now=None, why="boot", playing=False, safe_point=True, fails=False):
        """5.5: True when the card's stats.bin is now this session's (written,
        or nothing new to write); False when skipped or failed (fails: a full
        or failing card; every save of a session whose scan was partial). A
        boot's save may compact (5.3); a rotation's never does. Writes
        stats.bin (generation + 1, retention applied, sinceDay set once), a
        new base when compacting, and removes the old base. The new base's
        name is one stats.bin doesn't name: a stale file there (a stats.bin
        deleted or corrupt, or a base lost, left it) is replaced."""
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
        self.compacted = why == "boot" and compaction_due(len(st.trkd), h["baseRows"], playing, safe_point)
        if self.compacted:
            st.base = st.tracks()
            st.trkd = {}
            h["baseGen"] = sat(h["baseGen"] + 1, M32)
            base = write_base(st.base, h["baseGen"])
            h["baseRows"], h["baseCrc"] = len(st.base), header_crc(base)
            self.files[base_name(h["baseGen"])] = base
        st.generation = sat(st.generation + 1, M32)
        self.files[STATS_BIN] = write_stats(st)
        if self.compacted and old_base and old_base in self.files:
            del self.files[old_base]
            self.removed.append(old_base)
        self.saved_mark = dict(st.mark)
        self.new = False
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
    boot's save, the trim. now: the board's clock at the save (Unix ms, or
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


def recently_played(st: Stats, rows=None):
    """[(pathHash, plays, lastAt, lastOrd)]: plays at least 1. lastAt, newest
    first; then the rows with no dated play by lastOrd, newest first; then
    the hash."""
    rows = st.tracks() if rows is None else rows
    items = sorted(((0, -r.lastAt) if r.lastAt else (1, -r.lastOrd), h) for h, r in rows.items() if r.plays >= 1)
    items = items[:CAPS["recentlyPlayed"] + PAST_CAP]
    return [(h, rows[h].plays, rows[h].lastAt, rows[h].lastOrd) for _, h in items]


def not_played(st: Stats, albums, rows=None):
    """Not played yet: the albums [(name, [pathHash...])], in the library's
    A-Z order, with no track played (a track only skipped is not played)."""
    rows = st.tracks() if rows is None else rows
    return [name for name, hashes in albums if not any(h in rows and rows[h].plays >= 1 for h in hashes)]


def lists(st: Stats, today, albums=None):
    """Every shelf's list (4.3) as the rankers give it: hashes, no names."""
    rows = st.tracks()
    out = {"mostPlayed": {str(d or "all"): most_played(st, today, d, rows) for d in (7, 30, None)},
           "newFinds": new_finds(st, today, rows), "oldFavourites": old_favourites(st, today, rows),
           "recentlyPlayed": recently_played(st, rows)}
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
    out.append({"name": "base-other",
                "note": "Another file under the base's name: its header whole, its CRC not stats.bin's baseCrc. "
                        "The base is lost (flags bit 1, the totals kept). Nothing playing, so the save compacts, "
                        "and the new base, stats-00000001.bin again, replaces that file; nothing is removed.",
                "device": dev, "files": other, "boots": boot1(False)})
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


def fixture_files():
    """test/fixtures/card/stats/: {path under test/fixtures/card: bytes}."""
    files, index = {}, []
    for sc in scenarios():
        entry, out = run_scenario(sc)
        index.append(entry)
        files.update(out)
    lib = [{"album": album, "artist": artist, "tracks": [{"path": "%s/%s/%s" % (artist, album, f),
                                                          "hash": hx(path_hash("%s/%s/%s" % (artist, album, f)))}
                                                         for f in fs]}
           for artist, album, fs in sorted(LIBRARY, key=lambda a: a[1].casefold())]
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
    bytes})."""
    out = {}
    for name in sorted(os.listdir(d)):
        low = name.lower()
        if low in LOG_NAMES or low in (STATS_BIN, STATS_BAD) or (low.startswith("stats-") and low.endswith(".bin")):
            with open(os.path.join(d, name), "rb") as fh:
                out[low] = fh.read()
    return out


def parse_time(v):
    """Unix ms, or an ISO time (UTC): '2026-11-10T09:00:00'."""
    if v is None:
        return None
    return int(v) if v.isdigit() else ms_of(v)


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
