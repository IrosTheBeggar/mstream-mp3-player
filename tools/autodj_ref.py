#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""The Auto DJ engine's pick rule as plain Python: the reference that
lib/core/AutoDjEngine must match pick for pick, and the maker of its test
vectors, test/fixtures/autodj/.

    python tools/autodj_ref.py            write test/fixtures/autodj/
    python tools/autodj_ref.py --check    exit 1 when a file there differs

Standard library only (tools/mpdj.py reads the golden indexes). Every rule
here is the C++ engine's, in the same order, drawing from the same PCG32
stream, so a session replayed on both gives the same rows, the same
classes, the same imputed statuses, the same slot reads and the same
counters. The rule (docs/AUTODJ-INDEX.md 11 is the index's side):

- **The anchors.** Follow the vibe (Rolling): a ring of the session's last
  5 rows, newest last, a row that comes back moving to the end; the seed is
  its first entry. Auto DJ's own picks join it when picked, the listener's
  tracks when they start (D1), and so does a pick made by track id while
  the index was away, if it has come since; a track skipped in its first
  30 s (not a natural end) leaves it, and stays in both cooldowns. Stay on
  seed (Locked): the pin, the session's seed track; with none (no seed
  track: the first track to start anchors the ring but is no pin; a seed
  without a row; Anchor changed), the playing track's row at the next pick,
  else the next pick's own row. A damaged pin gives way to the random pick
  it causes. Never two-hop (D5): at most the pin's slot a pick.
- **The reads.** A slot cache of 6 (the ring and the pin are never evicted:
  the least recently used other slot is). A loop pass may prefetch one
  missing anchor slot, newest first; a pick reads the missing ones, newest
  first, at most 2 reads, and leaves the rest out of this pick. A slot
  whose CRC fails is read once more (by the same pick if its budget allows,
  else by the next read of it); failing twice in a row it goes into a ring
  of the last 8 damaged rows and is never read again until the file is
  reopened. A failed read (Why::Io) is only skipped. A slot that passes its
  CRC over a bad list (Broken) makes the file absent: every later pick is a
  random one until the file is reopened.
- **The candidates.** The union of the usable anchors' used entries, minus
  the anchors and the playing track's row, minus rows that map to no
  track: per row its best level and the sum of its levels, in row order.
- **The gate.** Best level >= L = max((255 p + 99) div 100, selFloor) for
  the slider p (11.4). Nothing passes (dry): the 50 with the highest sum,
  ties by row.
- **The class** of a gated candidate: cooldown level x 8 + music rank. The
  cooldown level: 0 fresh; 1 its artist among the last 3 (an artistKey of
  0 is never cooled); 2 its row among the last 50 played or picked (a row
  that comes back moves to the end, as mStream's ignoreList). The
  music rank (mStream's waterfall for one pick, the design's 1.4.1):
  tempo against the playing track's BPM (tight within 8 of it, of half of
  it rounded up or of double; wide within 10; else wrong), key against the
  session's Camelot anchor (good within its 6 codes: the same number and
  one either side, A or B; before there is an anchor any known key), then
  RANKS below. BPM continuity and Harmonic mixing default to off (D2).
- **A missing BPM or key** (D3) gets a status drawn with the odds of this
  pick's gated candidates that have one: below(n) over their tight / wide
  / wrong (good / wrong) counts, for each candidate missing it in the
  candidates' order, every tempo draw before every key draw. With no known
  value among them, it stays unknown and ranks as mStream ranks unknown.
- **The draw:** uniform over the candidates of the lowest class, one
  below(n). The pick joins the ring and both cooldowns; with Harmonic
  mixing on and no anchor yet the pick's key becomes it (the playing
  track's key first, at the pick's start).
- **Random picks** (no usable anchor, no candidate, a broken file): 64
  uniform draws over the rows, skipping rows that map to no track, the
  anchors and the playing track, each kept once; then the classes as above
  without a gate. No row usable: a pick by track id. **No index:** a
  random library track by id, not the playing one, up to 16 tries for one
  outside a 50-track cooldown kept by id.
"""
import argparse
import json
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mpdj  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "test" / "fixtures" / "autodj"
CARD = ROOT / "test" / "fixtures" / "card"

M32 = 0xFFFFFFFF
M64 = 0xFFFFFFFFFFFFFFFF

# ---------------------------------------------------------------------------
# Constants (lib/core/AutoDjEngine.h)
# ---------------------------------------------------------------------------
ROLLING, LOCKED = 0, 1          # "Follow the vibe", "Stay on seed"
ANCHORS = 5
TRACK_COOLDOWN = 50
ARTIST_COOLDOWN = 3
QUICK_SKIP_MS = 30000
READS_PER_PICK = 2
SLOT_CACHE = 6
DAMAGED_RING = 8
DRY = 50
RANDOM_DRAWS = 64
NO_INDEX_TRIES = 16
SURPRISE_DRAWS = 64
NO_ROW = 0xFFFF                 # Maps' u16 "none" (trackToRow, rowToTrack)
NO_TRACK = 0xFFFFFFFF
TIGHT_BPM, WIDE_BPM = 8, 10
STREAM = 0x4155544F444A         # the session stream: "AUTODJ"
MAX_UNION = ANCHORS * mpdj.MAX_K

# The filter word (Maps.filters): bpm | cam << 8 | art << 16.
FILTER_ANALYSED = 0x20 << 8     # cam bit 5: BPM_ANALYSED
FILTER_PRESENT = 0x80 << 8      # cam bit 7: the row maps to a track (the join's)

# Tempo and key statuses (lib/core/AutoDjTiers.h).
T_NA, T_TIGHT, T_WIDE, T_WRONG, T_UNKNOWN = 0, 1, 2, 3, 4
K_NA, K_GOOD, K_WRONG, K_UNKNOWN = 0, 1, 2, 3

# Slot reads (lib/core/AutoDjEngine.cpp's Read).
R_OK, R_SUSPECT, R_DAMAGED, R_IO, R_BROKEN = "ok", "suspect", "damaged", "io", "broken"


# ---------------------------------------------------------------------------
# PCG32 (O'Neill's pcg32_random_r, XSH RR 64/32) and Lemire's below(n)
# ---------------------------------------------------------------------------
class Pcg32:
    MULT = 6364136223846793005

    def __init__(self, seed, stream=STREAM):
        """pcg32_srandom_r(seed, stream)."""
        self.inc = ((stream << 1) | 1) & M64
        self.state = 0
        self.next()
        self.state = (self.state + seed) & M64
        self.next()

    def next(self):
        old = self.state
        self.state = (old * self.MULT + self.inc) & M64
        x = (((old >> 18) ^ old) >> 27) & M32
        r = old >> 59
        return ((x >> r) | (x << ((32 - r) & 31))) & M32

    def below(self, n):
        """Uniform in [0, n), 1 <= n < 2^32: Lemire's multiply-shift with
        rejection (no modulo bias, one next() almost always)."""
        if not 0 < n <= M32:
            raise ValueError(n)
        m = self.next() * n
        lo = m & M32
        if lo < n:
            t = ((1 << 32) - n) % n
            while lo < t:
                m = self.next() * n
                lo = m & M32
        return m >> 32


# ---------------------------------------------------------------------------
# The filter word, the windows, Camelot, the ranks (AutoDjTiers)
# ---------------------------------------------------------------------------
def pack_filter(bpm10, camelot, analysed, artist_key, present=False):
    """A DJRF record (4.4) as the engine keeps it (lib/core/AutoDjJoin's
    filterWord()): the BPM rounded half up and saturated at 255 (0 none);
    the Camelot code (above 24: none) with BPM_ANALYSED in bit 5 and
    "present" (the join's: the row maps to a track) in bit 7; the low 16
    bits of the artistKey, 0 made 1 when the key isn't 0."""
    bpm = min((bpm10 + 5) // 10, 255)
    cam = camelot if 0 <= camelot <= 24 else 0
    word = bpm | cam << 8 | (FILTER_ANALYSED if analysed else 0) | (FILTER_PRESENT if present else 0)
    art = artist_key & 0xFFFF
    if art == 0 and artist_key != 0:
        art = 1
    return word | art << 16


def bpm_of(f):
    return f & 0xFF


def camelot_of(f):
    c = (f >> 8) & 0x1F
    return c if c <= 24 else 0


def artist_of(f):
    return f >> 16


def floor_level(percent, sel_floor):
    """The gate's level (11.4): (255 p + 99) div 100, at least selFloor."""
    return max((255 * min(percent, 100) + 99) // 100, sel_floor)


def windows(ref, tol):
    half = (ref + 1) // 2
    return ((ref - tol, ref + tol), (half - tol, half + tol), (2 * ref - tol, 2 * ref + tol))


def in_windows(bpm, ref, tol):
    return any(lo <= bpm <= hi for lo, hi in windows(ref, tol))


def tempo_status(bpm, ref):
    """Against the playing track's BPM `ref` (0: tempo off for this pick)."""
    if ref == 0:
        return T_NA
    if bpm == 0:
        return T_UNKNOWN
    if in_windows(bpm, ref, TIGHT_BPM):
        return T_TIGHT
    if in_windows(bpm, ref, WIDE_BPM):
        return T_WIDE
    return T_WRONG


def camelot_near(anchor, code):
    """`code` is one of `anchor`'s 6 codes: the same number or one either side
    (12 next to 1), in either letter."""
    d = ((anchor - 1) % 12 - (code - 1) % 12) % 12
    return d in (0, 1, 11)


def key_status(code, anchor, on):
    if not on:
        return K_NA
    if code == 0:
        return K_UNKNOWN
    if anchor == 0 or camelot_near(anchor, code):
        return K_GOOD
    return K_WRONG


RANK_BOTH = {
    (T_TIGHT, K_GOOD): 0, (T_WIDE, K_GOOD): 1, (T_TIGHT, K_UNKNOWN): 2, (T_TIGHT, K_WRONG): 3,
    (T_WIDE, K_UNKNOWN): 4, (T_WIDE, K_WRONG): 4, (T_UNKNOWN, K_GOOD): 5, (T_UNKNOWN, K_UNKNOWN): 6,
}
RANK_TEMPO = {T_TIGHT: 0, T_WIDE: 1, T_UNKNOWN: 2, T_WRONG: 3}
RANK_KEY = {K_GOOD: 0, K_UNKNOWN: 1, K_WRONG: 2}


def music_rank(t, k):
    """The design's 1.4.1-1.4.2: the waterfall step that first admits the row,
    then its tier inside that step (0 best)."""
    if t != T_NA and k != K_NA:
        return RANK_BOTH.get((t, k), 7)
    if t != T_NA:
        return RANK_TEMPO[t]
    if k != K_NA:
        return RANK_KEY[k]
    return 0


class TempoOdds:
    def __init__(self, statuses):
        self.tight = sum(1 for s in statuses if s == T_TIGHT)
        self.wide = sum(1 for s in statuses if s == T_WIDE)
        self.wrong = sum(1 for s in statuses if s == T_WRONG)

    def known(self):
        return self.tight + self.wide + self.wrong

    def draw(self, rng):
        u = rng.below(self.known())
        return T_TIGHT if u < self.tight else T_WIDE if u < self.tight + self.wide else T_WRONG


class KeyOdds:
    def __init__(self, statuses):
        self.good = sum(1 for s in statuses if s == K_GOOD)
        self.wrong = sum(1 for s in statuses if s == K_WRONG)

    def known(self):
        return self.good + self.wrong

    def draw(self, rng):
        return K_GOOD if rng.below(self.known()) < self.good else K_WRONG


def impute(tempo, key, rng):
    """D3, in place on copies: every unknown tempo (in order), then every
    unknown key, drawn with the known ones' odds. Returns the statuses and
    which were drawn."""
    t, k = list(tempo), list(key)
    it, ik = [False] * len(t), [False] * len(k)
    to = TempoOdds(tempo)
    if to.known():
        for i, s in enumerate(tempo):
            if s == T_UNKNOWN:
                t[i], it[i] = to.draw(rng), True
    ko = KeyOdds(key)
    if ko.known():
        for i, s in enumerate(key):
            if s == K_UNKNOWN:
                k[i], ik[i] = ko.draw(rng), True
    return t, k, it, ik


# ---------------------------------------------------------------------------
# The engine (AutoDjEngine)
# ---------------------------------------------------------------------------
class Settings:
    def __init__(self, mode=ROLLING, floor=55, tempo=False, key=False):
        self.mode, self.floor, self.tempo, self.key = mode, floor, tempo, key


class Maps:
    """The join's product: track -> row, row -> track (NO_ROW: none) and a
    filter word a row."""

    def __init__(self, track_to_row, row_to_track, filters):
        self.track_to_row, self.row_to_track, self.filters = list(track_to_row), list(row_to_track), list(filters)
        self.tracks, self.rows = len(self.track_to_row), len(self.row_to_track)


def row_of(maps, track):
    return maps.track_to_row[track] if maps is not None and track < maps.tracks else NO_ROW


class SlotSource:
    """The tests' SlotReader: each row's used entries [(row, level), ...],
    and faults by row: "damaged" (the CRC always fails), "flaky" (it fails
    on the first read only), "io" (the first read fails: the card's
    hiccup), "broken" (a good CRC over a bad list)."""

    def __init__(self, lists, faults=None):
        self.lists = lists
        f = faults or {}
        self.damaged, self.flaky = set(f.get("damaged", ())), set(f.get("flaky", ()))
        self.io, self.broken = set(f.get("io", ())), set(f.get("broken", ()))
        self.count = {}
        self.reads = 0

    def read(self, row):
        """(SlotRead, why, entries): "ok"/"damaged"/"broken", "ok"/"io"/"crc"/"order"."""
        self.reads += 1
        n = self.count.get(row, 0)
        self.count[row] = n + 1
        if row in self.broken:
            return "broken", "order", []
        if row in self.io and n == 0:
            return "damaged", "io", []
        if row in self.damaged or (row in self.flaky and n == 0):
            return "damaged", "crc", []
        return "ok", "ok", list(self.lists[row])


class PickIn:
    def __init__(self, maps, slots, k=mpdj.DEFAULT_K, sel_head=mpdj.DEFAULT_HEAD, sel_floor=mpdj.DEFAULT_FLOOR,
                 playing=NO_TRACK, library_tracks=0):
        self.maps, self.slots, self.k, self.sel_head, self.sel_floor = maps, slots, k, sel_head, sel_floor
        self.playing, self.library_tracks = playing, library_tracks


class Pick:
    FIELDS = ("ok", "track", "row", "level", "cooldown_level", "music_rank", "dry", "random", "no_index",
              "imputed_tempo", "imputed_key", "tempo_break", "key_break", "reads", "broken", "candidates", "gated",
              "in_class")

    def __init__(self):
        self.ok = False
        self.track = NO_TRACK
        self.row = NO_ROW
        self.level = self.cooldown_level = self.music_rank = 0
        self.dry = self.random = self.no_index = False
        self.imputed_tempo = self.imputed_key = self.tempo_break = self.key_break = False
        self.reads = 0
        self.broken = False
        self.candidates = self.gated = self.in_class = 0

    def as_dict(self):
        return {f: getattr(self, f) for f in self.FIELDS}


class Stats:
    FIELDS = ("picks", "failed", "reads_at_pick", "prefetch_reads", "left_out", "damaged", "io_skips", "broken",
              "dry", "random", "no_index", "imputed_tempo", "imputed_key", "classes", "mode_picks", "tempo_breaks",
              "key_breaks", "quick_skips")

    def __init__(self):
        for f in self.FIELDS:
            setattr(self, f, 0)
        self.reads_at_pick = [0] * (READS_PER_PICK + 1)
        self.classes = [0] * 24
        self.mode_picks, self.tempo_breaks, self.key_breaks = [0, 0], [0, 0], [0, 0]

    def as_dict(self):
        return {f: (list(v) if isinstance(v, list) else v) for f, v in ((f, getattr(self, f)) for f in self.FIELDS)}


def ring_add(ring, v, cap):
    """Move-to-end, newest last, at most `cap`."""
    if v in ring:
        ring.remove(v)
    ring.append(v)
    if len(ring) > cap:
        del ring[0]


class Engine:
    def __init__(self):
        self.cache = [None] * SLOT_CACHE    # [row, entries, stamp]
        self.clock = 0
        self.damaged = []
        self.suspect = NO_ROW
        self.broken = False
        self.stats = Stats()
        self.new_session(0)

    # ---- the session ----
    def new_session(self, seed, seed_track=NO_TRACK, maps=None):
        self.rng = Pcg32(seed)
        self.ring, self.cool, self.arts, self.ids = [], [], [], []
        self.pin = NO_ROW
        self.key_anchor = 0
        self.seeded = False
        if seed_track != NO_TRACK:
            self._seed(seed_track, maps, True)

    def _seed(self, track, maps, pin):
        """The session's first anchor; `pin` also Stay on seed's (the seed
        track, the playing track at the first pick), not the first track to
        start after a session began without one."""
        self.seeded = True
        ring_add(self.ids, track, TRACK_COOLDOWN)
        row = row_of(maps, track)
        if row != NO_ROW:
            self._join(row, maps)
            if pin:
                self.pin = row

    def _join(self, row, maps):
        ring_add(self.ring, row, ANCHORS)
        ring_add(self.cool, row, TRACK_COOLDOWN)
        a = artist_of(maps.filters[row])
        if a:
            ring_add(self.arts, a, ARTIST_COOLDOWN)

    def track_started(self, track, maps, by_autodj):
        if not self.seeded:
            self._seed(track, maps, False)
            return
        row = row_of(maps, track)
        if by_autodj:
            # Joined when picked (its row cooled since); one picked by track id
            # while the index was away joins now that its row is known (D1).
            if row != NO_ROW and row not in self.cool:
                self._join(row, maps)
            return
        ring_add(self.ids, track, TRACK_COOLDOWN)
        if row != NO_ROW:
            self._join(row, maps)

    def track_left(self, track, maps, played_ms, natural_end):
        if natural_end or played_ms >= QUICK_SKIP_MS:
            return
        self.stats.quick_skips += 1
        row = row_of(maps, track)
        if row in self.ring:
            self.ring.remove(row)

    def mode_changed(self):
        self.pin = NO_ROW

    def file_reopened(self):
        self.cache = [None] * SLOT_CACHE
        self.damaged = []
        self.suspect = NO_ROW
        self.broken = False

    def tracks_renumbered(self):
        self.ids = []

    # ---- the slot cache and reads ----
    def _find(self, row):
        for i, c in enumerate(self.cache):
            if c is not None and c[0] == row:
                return i
        return -1

    def _insert(self, row, entries):
        keep = set(self.ring)
        keep.add(self.pin)
        at = -1
        for i, c in enumerate(self.cache):
            if c is None:
                at = i
                break
        if at < 0:
            for i, c in enumerate(self.cache):
                if c[0] not in keep and (at < 0 or c[2] < self.cache[at][2]):
                    at = i
        if at < 0:       # never: the ring and the pin are at most 6 rows, one of them `row`
            at = min(range(SLOT_CACHE), key=lambda i: self.cache[i][2])
        self.clock += 1
        self.cache[at] = [row, entries, self.clock]

    def _touch(self, i):
        self.clock += 1
        self.cache[i][2] = self.clock

    def _read(self, reader, row):
        st, why, entries = reader.read(row)
        if st == "ok":
            self._insert(row, entries)
            if self.suspect == row:
                self.suspect = NO_ROW
            return R_OK
        if st == "broken":
            self.broken = True
            self.stats.broken += 1
            return R_BROKEN
        if why == "io":
            self.stats.io_skips += 1
            return R_IO
        if self.suspect == row:
            self.suspect = NO_ROW
            ring_add(self.damaged, row, DAMAGED_RING)
            self.stats.damaged += 1
            return R_DAMAGED
        self.suspect = row
        return R_SUSPECT

    def prefetch_due(self, s):
        """The anchor slot a loop pass should read, or None."""
        if self.broken:
            return None
        want = [self.pin] if s.mode == LOCKED else list(reversed(self.ring))
        for r in want:
            if r != NO_ROW and r not in self.damaged and self._find(r) < 0:
                return r
        return None

    def prefetch(self, reader, row):
        self.stats.prefetch_reads += 1
        return self._read(reader, row)

    # ---- the pick ----
    def pick(self, s, pin):
        out = Pick()
        maps = pin.maps
        if maps is None or pin.slots is None:
            return self._no_index(s, pin, out)
        playing_row = row_of(maps, pin.playing)
        if not self.seeded and pin.playing != NO_TRACK:
            self._seed(pin.playing, maps, True)
        if s.key and self.key_anchor == 0 and playing_row != NO_ROW:
            self.key_anchor = camelot_of(maps.filters[playing_row])
        if s.mode == LOCKED and self.pin == NO_ROW and playing_row != NO_ROW:
            self.pin = playing_row
        if s.mode == LOCKED:
            anchors = [self.pin] if self.pin != NO_ROW else []
        else:
            anchors = list(self.ring)
        exclude = set(anchors)
        if playing_row != NO_ROW:
            exclude.add(playing_row)
        usable = []
        if not self.broken:
            for r in reversed(anchors):
                if r in self.damaged:
                    continue
                i = self._find(r)
                if i < 0:
                    if out.reads >= READS_PER_PICK:
                        self.stats.left_out += 1
                        continue
                    res = None
                    while out.reads < READS_PER_PICK:
                        out.reads += 1
                        res = self._read(pin.slots, r)
                        if res != R_SUSPECT:
                            break
                    if res == R_BROKEN:
                        break
                    if res != R_OK:
                        continue
                    i = self._find(r)
                usable.append(i)
        out.broken = self.broken
        if self.broken or not usable:
            # A damaged pin would make every Stay on seed pick random until the
            # file is reopened: this pick's row becomes the pin.
            if s.mode == LOCKED and self.pin in self.damaged:
                self.pin = NO_ROW
            return self._random(s, pin, playing_row, exclude, out)
        for i in reversed(usable):
            self._touch(i)
        acc = {}
        for i in usable:
            for row, lv in self.cache[i][1]:
                if row >= maps.rows or row in exclude or maps.row_to_track[row] == NO_ROW:
                    continue
                b, sm = acc.get(row, (0, 0))
                acc[row] = (max(b, lv), sm + lv)
        cand = sorted((row, b, sm) for row, (b, sm) in acc.items())
        out.candidates = len(cand)
        if not cand:
            return self._random(s, pin, playing_row, exclude, out)
        gate = floor_level(s.floor, pin.sel_floor)
        gated = [c for c in cand if c[1] >= gate]
        if not gated:
            out.dry = True
            gated = sorted(sorted(cand, key=lambda c: (-c[2], c[0]))[:DRY])
        out.gated = len(gated)
        rows = [c[0] for c in gated]
        j = self._choose(s, maps, rows, playing_row, out)
        out.level = gated[j][1]
        return self._finish(s, maps, rows[j], out)

    def _choose(self, s, maps, rows, playing_row, out):
        f = maps.filters
        ref = bpm_of(f[playing_row]) if s.tempo and playing_row != NO_ROW else 0
        tempo = [tempo_status(bpm_of(f[r]), ref) for r in rows]
        key = [key_status(camelot_of(f[r]), self.key_anchor, s.key) for r in rows]
        t, k, it, ik = impute(tempo, key, self.rng)
        cls = []
        for i, r in enumerate(rows):
            a = artist_of(f[r])
            cl = 2 if r in self.cool else 1 if a and a in self.arts else 0
            cls.append(cl * 8 + music_rank(t[i], k[i]))
        best = min(cls)
        sel = [i for i, c in enumerate(cls) if c == best]
        j = sel[self.rng.below(len(sel))]
        out.cooldown_level, out.music_rank, out.in_class = best // 8, best % 8, len(sel)
        out.imputed_tempo, out.imputed_key = it[j], ik[j]
        out.tempo_break = tempo[j] in (T_WIDE, T_WRONG)
        out.key_break = key[j] == K_WRONG
        return j

    def _random(self, s, pin, playing_row, exclude, out):
        out.random = True
        maps = pin.maps
        rows = []
        for _ in range(RANDOM_DRAWS):
            r = self.rng.below(maps.rows)
            if r in exclude or maps.row_to_track[r] == NO_ROW or r in rows:
                continue
            rows.append(r)
        out.candidates = out.gated = len(rows)
        if not rows:
            return self._no_index(s, pin, out)
        j = self._choose(s, maps, rows, playing_row, out)
        out.level = 0
        return self._finish(s, maps, rows[j], out)

    def _finish(self, s, maps, row, out):
        out.ok, out.row, out.track = True, row, maps.row_to_track[row]
        self._join(row, maps)
        ring_add(self.ids, out.track, TRACK_COOLDOWN)
        if s.key and self.key_anchor == 0:
            self.key_anchor = camelot_of(maps.filters[row])
        if s.mode == LOCKED and self.pin == NO_ROW:
            self.pin = row
        st = self.stats
        st.classes[out.cooldown_level * 8 + out.music_rank] += 1
        st.imputed_tempo += out.imputed_tempo
        st.imputed_key += out.imputed_key
        st.tempo_breaks[s.mode] += out.tempo_break
        st.key_breaks[s.mode] += out.key_break
        return self._count(s, out)

    def _no_index(self, s, pin, out):
        out.random = out.no_index = True
        n = pin.library_tracks
        t = NO_TRACK
        for _ in range(NO_INDEX_TRIES if n else 0):
            x = self.rng.below(n)
            if x == pin.playing:
                continue
            t = x
            if x not in self.ids:
                break
        if t == NO_TRACK:
            self.stats.failed += 1
            return out
        out.ok, out.track, out.row = True, t, NO_ROW
        ring_add(self.ids, t, TRACK_COOLDOWN)
        row = row_of(pin.maps, t)
        if row != NO_ROW:
            out.row = row
            self._join(row, pin.maps)
            if s.mode == LOCKED and self.pin == NO_ROW:
                self.pin = row
        return self._count(s, out)

    def _count(self, s, out):
        st = self.stats
        st.picks += 1
        st.mode_picks[s.mode] += 1
        st.reads_at_pick[out.reads] += 1
        st.dry += out.dry
        st.random += out.random
        st.no_index += out.no_index
        return out

    def surprise(self, s, pin):
        """The empty states' Auto DJ button: a random row that maps to a track
        (up to 64 draws). Changes nothing but the stream."""
        out = Pick()
        out.random = True
        maps = pin.maps
        if maps is None:
            return out
        for _ in range(SURPRISE_DRAWS):
            r = self.rng.below(maps.rows)
            if maps.row_to_track[r] != NO_ROW:
                out.ok, out.row, out.track = True, r, maps.row_to_track[r]
                return out
        return out


# ---------------------------------------------------------------------------
# The fixture: sessions replayed by test/test_autodj_engine
# ---------------------------------------------------------------------------
GOLDENS = ("autodj-mid.bin", "autodj-mid-default.bin", "autodj-00000029.bin", "autodj-small-default.bin")
SCENE_STREAM = 0x5343454E45       # the scenarios' own stream: "SCENE"


def golden_table(name):
    r = mpdj.Reader((CARD / "golden" / name).read_bytes())
    if r.why != "ok":
        raise ValueError(name + ": " + r.why)
    lists = []
    for row in range(r.rows):
        read, why, entries = r.slot(row)
        if read != mpdj.OK:
            raise ValueError("%s: slot %d %s" % (name, row, why))
        lists.append([(e[0], e[1]) for e in entries])
    filters = []
    for row in range(r.rows):
        bpm10, cam, analysed, ak = r.filter(row)
        filters.append(pack_filter(bpm10, cam, analysed, ak))
    return {"rows": r.rows, "k": r.k, "selHead": r.head, "selFloor": r.floor, "lists": lists, "filters": filters}


def synthetic_table(rows=300, k=40, clusters=12, seed=7):
    """A clustered table that isn't a writer's (no embedding, no stride):
    unit vectors in 3-D about `clusters` centres, each row's list its
    nearest rows by (level desc, row asc), every list cut to k; a row in 25
    keeps only its first 0-3 entries (sparse and empty rows), and one in 25
    has every level scaled below 121 (an outlier: dry at any floor). Its filters:
    BPMs and keys clustered with gaps, artists clustered with collisions in
    16 bits and key 0."""
    g = Pcg32(seed, SCENE_STREAM)

    def unit():
        while True:
            v = [g.below(2001) / 1000.0 - 1.0 for _ in range(3)]
            n = math.sqrt(sum(x * x for x in v))
            if 0.1 < n <= 1.0:
                return [x / n for x in v]

    centres = [unit() for _ in range(clusters)]
    vec = []
    for r in range(rows):
        c = centres[r % clusters]
        p = [c[i] + (g.below(601) - 300) / 1000.0 for i in range(3)]
        n = math.sqrt(sum(x * x for x in p))
        vec.append([x / n for x in p])
    lists, outliers = [], []
    for a in range(rows):
        cand = []
        for b in range(rows):
            if b != a:
                cand.append((-mpdj.level(sum(x * y for x, y in zip(vec[a], vec[b]))), b))
        cand.sort()
        lst = [(b, -lv) for lv, b in cand[:k]]
        u = g.below(50)
        if u < 2:
            lst = lst[:g.below(4)]
        elif u < 4:
            lst = sorted(((b, lv * 120 // 255) for b, lv in lst), key=lambda e: (-e[1], e[0]))
            outliers.append(a)
        lists.append(lst)
    filters = []
    for r in range(rows):
        c = r % clusters
        bpm10 = 0 if g.below(10) == 0 else (80 + 7 * c + g.below(30)) * 10 + g.below(10)
        cam = 0 if g.below(10) == 0 else 1 + (c * 5 + g.below(4)) % 24
        art = (c * 4 + g.below(4)) * 0x10000 + 0x100 * c   # 16 bits collide across clusters
        if g.below(15) == 0:
            art = 0
        filters.append(pack_filter(bpm10, cam, g.below(2) == 0, art))
    return {"rows": rows, "k": k, "selHead": 16, "selFloor": 140, "lists": lists, "filters": filters,
            "outliers": outliers}


def ties_table(n=100):
    """Row 0 lists rows 1..n at one level, below every floor; each of them
    lists row 0 back. Every candidate's sum ties: dry picks keep the 50
    lowest rows (the tie by row)."""
    lists = [[(r, 100) for r in range(1, n + 1)]] + [[(0, 100)] for _ in range(n)]
    return {"rows": n + 1, "k": n, "selHead": 16, "selFloor": 140, "lists": lists, "filters": [0] * (n + 1)}


def encode_lists(lists):
    return ["".join("%04x%02x" % (r, lv) for r, lv in lst) for lst in lists]


def make_maps(g, rows, extra, unmapped, base_filters, drop_bpm=0, drop_key=0):
    """Tracks for the table's rows in a shuffled order, `extra` tracks with no
    row among them, `unmapped` rows with no track (per mille); BPMs and keys
    dropped on a share of rows (per mille)."""
    mapped = [r for r in range(rows) if g.below(1000) >= unmapped]
    tracks = mapped + [NO_ROW] * (len(mapped) * extra // 1000)
    for i in range(len(tracks) - 1, 0, -1):     # Fisher-Yates from the end
        j = g.below(i + 1)
        tracks[i], tracks[j] = tracks[j], tracks[i]
    row_to_track = [NO_ROW] * rows
    for t, r in enumerate(tracks):
        if r != NO_ROW:
            row_to_track[r] = t
    filters = []
    for f in base_filters:
        if drop_bpm and g.below(1000) < drop_bpm:
            f &= ~0xFF
        if drop_key and g.below(1000) < drop_key:
            f &= ~(0x1F << 8)
        filters.append(f)
    for r in range(rows):
        if row_to_track[r] != NO_ROW:
            filters[r] |= FILTER_PRESENT
    return Maps(tracks, row_to_track, filters)


def settings_json(s):
    return {"m": s.mode, "f": s.floor, "t": int(s.tempo), "k": int(s.key)}


def pick_json(p):
    """Every field of a Pick; those at 0 (false) left out."""
    d = {"ok": int(p.ok), "track": p.track, "row": p.row, "level": p.level, "cl": p.cooldown_level,
         "mr": p.music_rank, "dry": int(p.dry), "random": int(p.random), "noIndex": int(p.no_index),
         "impT": int(p.imputed_tempo), "impK": int(p.imputed_key), "tb": int(p.tempo_break),
         "kb": int(p.key_break), "reads": p.reads, "broken": int(p.broken), "cand": p.candidates,
         "gated": p.gated, "inClass": p.in_class}
    return {k: v for k, v in d.items() if v}


def stats_json(st):
    return {"picks": st.picks, "failed": st.failed, "readsAtPick": st.reads_at_pick,
            "prefetchReads": st.prefetch_reads, "leftOut": st.left_out, "damaged": st.damaged,
            "ioSkips": st.io_skips, "broken": st.broken, "dry": st.dry, "random": st.random,
            "noIndex": st.no_index, "imputedTempo": st.imputed_tempo, "imputedKey": st.imputed_key,
            "classes": st.classes, "modePicks": st.mode_picks, "tempoBreaks": st.tempo_breaks,
            "keyBreaks": st.key_breaks, "quickSkips": st.quick_skips}


class Scene:
    """One session's script, run on the reference as it is written: each event
    records what the engine must do."""

    def __init__(self, name, table, tname, maps, faults=None, library_tracks=None, seed=1, note=""):
        self.name, self.table, self.tname, self.maps, self.note = name, table, tname, maps, note
        self.faults = faults or {}
        self.src = SlotSource(table["lists"], self.faults) if table else None
        self.lib = library_tracks if library_tracks is not None else (maps.tracks if maps else 0)
        self.e = Engine()
        self.events = []
        self.g = Pcg32(seed, SCENE_STREAM)
        self.playing = NO_TRACK
        self.live = True

    def cur(self):
        """The maps the engine is given: none while the index is away."""
        return self.maps if self.live else None

    def pin(self):
        t = self.table
        return PickIn(self.cur(), self.src if self.live else None, t["k"] if t else 0, t["selHead"] if t else 0,
                      t["selFloor"] if t else 0, self.playing, self.lib)

    def index(self, on):
        """The index goes away (picks by track id, no maps for the starts) or
        comes back (joined again)."""
        self.live = on
        self.events.append({"op": "index", "on": int(on)})

    def new(self, seed, track=NO_TRACK):
        self.e.new_session(seed, track, self.cur())
        self.events.append({"op": "new", "seed": seed, "track": track})
        if track != NO_TRACK:
            self.playing = track

    def start(self, track, dj):
        self.e.track_started(track, self.cur(), dj)
        self.events.append({"op": "start", "track": track, "dj": int(dj)})
        self.playing = track

    def left(self, track, ms, natural):
        self.e.track_left(track, self.cur(), ms, natural)
        self.events.append({"op": "left", "track": track, "ms": ms, "natural": int(natural)})

    def mode(self):
        self.e.mode_changed()
        self.events.append({"op": "mode"})

    def reopen(self):
        self.e.file_reopened()
        self.events.append({"op": "reopen"})

    def set_faults(self, **faults):
        """The reader's faults from now (the rows each kind names)."""
        for kind in ("damaged", "flaky", "io", "broken"):
            rows = set(faults.get(kind, ()))
            setattr(self.src, kind, rows)
        self.events.append({"op": "faults", **{k: sorted(v) for k, v in faults.items() if v}})

    def renumber(self):
        self.e.tracks_renumbered()
        self.events.append({"op": "renumber"})

    def prefetch(self, s):
        row = self.e.prefetch_due(s)
        ev = {"op": "prefetch", "m": s.mode, "row": NO_ROW if row is None else row}
        if row is not None:
            ev["res"] = self.e.prefetch(self.src, row)
        self.events.append(ev)

    def pick(self, s):
        p = self.e.pick(s, self.pin())
        self.events.append({"op": "pick", "s": settings_json(s), "playing": self.playing, "expect": pick_json(p)})
        return p

    def surprise(self, s):
        p = self.e.surprise(s, self.pin())
        self.events.append({"op": "surprise", "expect": pick_json(p)})
        return p

    def user_track(self):
        """A track the listener plays: any track of the library."""
        return self.g.below(self.lib)

    def run(self, s, picks, user=0, skips=0, modes=0, flips=None, prefetch=2):
        """`picks` picks as the last entry starts, each pick's track then
        starting (a natural end of the one before); between them, per mille,
        a track of the listener's (`user`), a quick skip of the playing
        track (`skips`), an Anchor change (`modes`); `flips` a list of
        settings to cycle through; 0 to `prefetch` passes before each pick."""
        g = self.g
        for n in range(picks):
            if flips:
                s = flips[n % len(flips)]
            if modes and g.below(1000) < modes:
                s = Settings(LOCKED if s.mode == ROLLING else ROLLING, s.floor, s.tempo, s.key)
                self.mode()
            if user and g.below(1000) < user and self.lib:
                t = self.user_track()
                if self.playing != NO_TRACK:
                    self.left(self.playing, 1000 * g.below(400), False)
                self.start(t, False)
            if skips and g.below(1000) < skips and self.playing != NO_TRACK:
                self.left(self.playing, 1000 * g.below(40), False)
                if self.lib:
                    self.start(self.user_track(), False)
            if self.cur() is not None:
                for _ in range(g.below(prefetch + 1)):
                    self.prefetch(s)
            p = self.pick(s)
            if p.ok:
                if self.playing != NO_TRACK:
                    self.left(self.playing, 120000 + 1000 * g.below(200), True)
                self.start(p.track, True)
        return s

    def json(self):
        d = {"name": self.name, "note": self.note, "table": self.tname, "libraryTracks": self.lib}
        if self.maps is not None:
            d["trackToRow"] = self.maps.track_to_row
            d["rowToTrack"] = self.maps.row_to_track
            d["filters"] = self.maps.filters
        if self.faults:
            d["faults"] = {k: sorted(v) for k, v in self.faults.items()}
        d["events"] = self.events
        d["stats"] = stats_json(self.e.stats)
        return d


def scenes():
    tables = {"syn": synthetic_table(), "ties": ties_table()}
    for name in GOLDENS:
        tables[name.rsplit(".", 1)[0]] = golden_table(name)
    mid, mdef, small, sdef, syn, ties = (tables[k] for k in ("autodj-mid", "autodj-mid-default", "autodj-00000029",
                                                              "autodj-small-default", "syn", "ties"))
    out = []
    g = Pcg32(20261013, SCENE_STREAM)
    both = Settings(ROLLING, 55, True, True)

    def maps_for(t, extra=100, unmapped=30, drop_bpm=0, drop_key=0):
        return make_maps(g, t["rows"], extra, unmapped, t["filters"], drop_bpm, drop_key)

    # 1. The defaults (D2: both off), Follow the vibe, the listener's tracks and skips.
    sc = Scene("defaults-rolling", mid, "autodj-mid", maps_for(mid), seed=11,
               note="D2's defaults on autodj-mid: no tempo or key; the listener's tracks join the ring as they "
                    "start (D1), quick skips leave it")
    sc.new(101, sc.maps.row_to_track.index(0))
    sc.run(Settings(), 40, user=120, skips=120)
    out.append(sc)

    # 2. Both on, every floor, Follow the vibe.
    sc = Scene("both-on-floors", mid, "autodj-mid", maps_for(mid), seed=12,
               note="both on at every floor step on autodj-mid, Follow the vibe")
    sc.new(202, sc.user_track())
    sc.run(both, 50, user=60, flips=[Settings(ROLLING, f, True, True) for f in (55, 60, 65, 70, 75)])
    out.append(sc)

    # 3. The default lists (k 169), both on, the top step: dry picks.
    sc = Scene("default-lists-75", mdef, "autodj-mid-default", maps_for(mdef), seed=13,
               note="autodj-mid-default (k 169) at 75 %: large unions, dry picks")
    sc.new(303, sc.user_track())
    sc.run(Settings(ROLLING, 75, True, True), 40, skips=80)
    out.append(sc)

    # 3b. Seeds far from everything: dry picks (the 50 by the sum of levels).
    m = maps_for(syn, unmapped=0)
    far = syn["outliers"]
    sc = Scene("dry-outliers", syn, "syn", m, seed=25,
               note="the synthetic table's outliers (every level below 121) as seeds: the gate passes nothing, "
                    "the 50 best by the sum of levels")
    sc.new(2525, m.row_to_track[far[0]])
    sc.run(Settings(LOCKED, 55, True, True), 8)
    sc.new(2526, m.row_to_track[far[1]])
    sc.run(Settings(ROLLING, 75, False, True), 8)
    sc.new(2527, m.row_to_track[far[2]])
    sc.run(Settings(ROLLING, 60, True, False), 6)
    out.append(sc)

    # 4. Stay on seed on the default lists: the cooldowns run out.
    sc = Scene("locked-default-lists", mdef, "autodj-mid-default", maps_for(mdef), seed=14,
               note="Stay on seed on autodj-mid-default at 70 %: the pin's one slot, repeats once its "
                    "neighbours are cooled")
    sc.new(404, sc.user_track())
    sc.run(Settings(LOCKED, 70, True, True), 60)
    out.append(sc)

    # 5. Tempo only, Anchor changes.
    sc = Scene("tempo-only-anchor-changes", mid, "autodj-mid", maps_for(mid), seed=15,
               note="BPM continuity alone at 65 %, Anchor changed now and then (the pin dropped and retaken)")
    sc.new(505, sc.user_track())
    sc.run(Settings(LOCKED, 65, True, False), 45, modes=150, user=80)
    out.append(sc)

    # 6. Half the BPMs and keys missing: the imputation.
    sc = Scene("half-missing", syn, "syn", maps_for(syn, drop_bpm=500, drop_key=500), seed=16,
               note="the synthetic table with half the BPMs and keys gone: D3's draws")
    sc.new(606, sc.user_track())
    sc.run(both, 45, user=100, flips=[both, Settings(ROLLING, 60, True, True), Settings(ROLLING, 55, False, True),
                                      Settings(ROLLING, 55, True, False)])
    out.append(sc)

    # 7. No BPM or key anywhere, then key only with none known.
    sc = Scene("no-tempo-or-key", syn, "syn", maps_for(syn, drop_bpm=1000, drop_key=1000), seed=17,
               note="no BPM or key on any row: both on, every candidate unknown (mStream's unknown ranks)")
    sc.new(707, sc.user_track())
    sc.run(Settings(LOCKED, 55, True, True), 15)
    sc.run(Settings(ROLLING, 60, True, True), 15)
    out.append(sc)

    # 8. Damaged, flaky and failing slots.
    m = maps_for(syn)
    faults = {"damaged": set(), "flaky": set(), "io": set()}
    for r in range(syn["rows"]):
        u = g.below(100)
        if u < 20:
            faults["damaged"].add(r)
        elif u < 35:
            faults["flaky"].add(r)
        elif u < 45:
            faults["io"].add(r)
    sc = Scene("damaged-slots", syn, "syn", m, faults, seed=18,
               note="slots whose CRC always fails, fails once, or whose read fails: re-read once, the damaged "
                    "ring, Io never in it; a reopen clears the ring")
    sc.new(808, sc.user_track())
    sc.run(both, 30, user=150, prefetch=1)
    sc.reopen()
    sc.run(Settings(LOCKED, 55, True, True), 15, modes=200, prefetch=0)
    out.append(sc)

    # 9. A broken slot: the file is absent from then on.
    m = maps_for(syn)
    sc = Scene("broken-slot", syn, "syn", m, seed=19,
               note="a slot with a good CRC over a bad list: broken; random picks over the rows until reopened")
    sc.new(909, sc.user_track())
    sc.run(both, 8)
    held = {c[0] for c in sc.e.cache if c is not None}
    sc.set_faults(broken=[r for r in range(syn["rows"]) if r not in held and g.below(3) == 0])
    sc.run(both, 10, user=300)
    sc.set_faults()
    sc.reopen()
    sc.run(both, 8)
    out.append(sc)

    # 10. A tiny table: few candidates, random picks, the by-id fallback.
    sc = Scene("tiny-table", small, "autodj-00000029", maps_for(small, extra=1000, unmapped=200), seed=20,
               note="autodj-00000029 (6 rows, selFloor 100): the cooldowns and the anchors use up every "
                    "candidate; random picks; picks by track id")
    sc.new(1010, sc.user_track())
    sc.run(Settings(ROLLING, 55, True, True), 20, user=200)
    sc.run(Settings(LOCKED, 75, False, True), 10, modes=300)
    out.append(sc)

    # 11. The default-k small table, Stay on seed with a seed that has no row.
    m = maps_for(sdef, extra=500, unmapped=0)
    rowless = [t for t in range(m.tracks) if m.track_to_row[t] == NO_ROW]
    sc = Scene("seed-without-a-row", sdef, "autodj-small-default", m, seed=21,
               note="Stay on seed from a track with no row: the first pick is random and becomes the pin")
    sc.new(1111, rowless[0])
    sc.run(Settings(LOCKED, 55, True, True), 12)
    sc.new(1112, rowless[-1])
    sc.run(Settings(ROLLING, 55, False, False), 8, user=250)
    out.append(sc)

    # 12. No index: picks by track id, the fence renumbering the tracks.
    sc = Scene("no-index", None, None, None, library_tracks=37, seed=22,
               note="no index: random library tracks by id, a 50-track cooldown by id, cleared when the tracks "
                    "are renumbered")
    sc.new(1212, 5)
    sc.run(both, 25, user=150)
    sc.renumber()
    sc.run(both, 25)
    out.append(sc)

    # 13. The empty states' button, then a session from it.
    sc = Scene("surprise", syn, "syn", maps_for(syn, unmapped=600), seed=23,
               note="the empty states' Auto DJ button: a random row with a track, then a session seeded by it")
    for n in range(6):
        p = sc.surprise(both)
        sc.new(1313 + n, p.track)
        sc.run(Settings(ROLLING if n % 2 else LOCKED, 60, n % 3 != 0, n % 2 == 0), 6, user=100)
    out.append(sc)

    # 14. Long Follow the vibe on the synthetic table: the 50-row cooldown.
    sc = Scene("long-rolling", syn, "syn", maps_for(syn, extra=0, unmapped=0), seed=24,
               note="80 picks of Follow the vibe at 70 %: the track cooldown at 50 rows, artists colliding in "
                    "16 bits and artist key 0")
    sc.new(1414, sc.user_track())
    sc.run(Settings(ROLLING, 70, True, True), 80, skips=50)
    out.append(sc)

    # 15. BPM continuity on, no candidate with a BPM (and then no key either):
    # mStream's unknown ranks.
    for name, drop_key, seed in (("tempo-unknown", 300, 26), ("tempo-and-key-unknown", 1000, 27)):
        m = maps_for(syn, extra=0, unmapped=0, drop_key=drop_key)
        seeds = {}
        for r in range(syn["rows"]):
            if bpm_of(syn["filters"][r]) and r % 12 not in seeds:
                seeds[r % 12] = r
        keep = set(seeds.values())
        m.filters = [(syn["filters"][r] | FILTER_PRESENT) if r in keep else f & ~0xFF
                     for r, f in enumerate(m.filters)]
        sc = Scene(name, syn, "syn", m, seed=seed,
                   note="a BPM on one row a cluster only%s, each seeding a session: candidates mostly "
                        "without one, the unknown tempo ranks (5, 6, 7 with key matching, 2 without)" % (
                            "" if drop_key < 1000 else " and keys on those rows only"))
        for n, r in enumerate(sorted(keep)):
            sc.new(seed * 100 + n, m.row_to_track[r])
            sc.run(Settings(LOCKED if n % 2 else ROLLING, 55, True, n % 3 != 2), 2)
        out.append(sc)

    # 16. Switched on mid-queue: no seed track. The listener's tracks that
    # start before the first pick anchor the ring; Stay on seed's pin is the
    # playing track at the first pick (the design's 2.2).
    m = maps_for(mid)
    sc = Scene("switched-on-mid-queue", mid, "autodj-mid", m, seed=28,
               note="Auto DJ switched on with tracks still to play (no seed track): the ring takes them as they "
                    "start, the pin is the playing track at the first pick")
    for n, s in enumerate((Settings(LOCKED, 60, True, True), Settings(ROLLING, 55), Settings(LOCKED, 75))):
        sc.new(2800 + n)
        for _ in range(3):
            if sc.playing != NO_TRACK:
                sc.left(sc.playing, 120000 + 1000 * sc.g.below(200), True)
            sc.start(sc.user_track(), False)
        sc.run(s, 8, user=100)
    out.append(sc)

    # 17. Stay on seed with a damaged pin: the random pick it causes becomes
    # the pin; a pin damaged later (read again after a reopen) gives way too.
    m = maps_for(syn, unmapped=0)
    seeds = [r for r in range(syn["rows"]) if len(syn["lists"][r]) >= 20][:2]
    sc = Scene("damaged-pin", syn, "syn", m, {"damaged": {seeds[0]}}, seed=29,
               note="Stay on seed from a seed whose slot fails its CRC: one random pick, which becomes the pin; "
                    "later the pin's slot fails after a reopen: the same again")
    sc.new(2900, m.row_to_track[seeds[0]])
    sc.run(Settings(LOCKED, 60, True, True), 6)
    sc.set_faults(damaged=[sc.e.pin], flaky=[seeds[1]])
    sc.reopen()
    sc.run(Settings(LOCKED, 60, True, True), 6, prefetch=1)
    sc.new(2901, m.row_to_track[seeds[1]])
    sc.run(Settings(LOCKED, 55, False, True), 4)
    out.append(sc)

    # 18. The index away, then back: a pick made by track id starts once the
    # index is there and steers (D1); the session's picks use it from then.
    sc = Scene("index-arrives", syn, "syn", maps_for(syn, unmapped=0), seed=30,
               note="picks by track id while the index is away; it comes back before the last of them starts: "
                    "that pick joins the ring, so the next pick isn't random")
    sc.index(False)
    sc.new(3000, sc.user_track())
    sc.run(Settings(), 3, user=200)
    for s in (Settings(), Settings(LOCKED, 60)):
        p = sc.pick(s)
        sc.index(True)
        sc.left(sc.playing, 150000, True)
        sc.start(p.track, True)
        sc.run(s, 5, user=100)
        sc.index(False)
        sc.run(s, 2)
    out.append(sc)

    # 19. Every sum tied: dry picks keep the 50 lowest rows.
    m = make_maps(g, ties["rows"], 0, 0, ties["filters"])
    sc = Scene("dry-ties", ties, "ties", m, seed=31,
               note="a table whose candidates' sums all tie, below every floor: the 50 dry candidates are the "
                    "50 lowest rows")
    sc.new(3100, m.row_to_track[0])
    sc.run(Settings(LOCKED, 55), 8)
    sc.new(3101, m.row_to_track[0])
    sc.start(m.row_to_track[60], False)
    sc.run(Settings(ROLLING, 65), 6)
    out.append(sc)
    return tables, out



README = """\
# Auto DJ engine fixtures

SPDX-License-Identifier: CC0-1.0. To the extent possible under law, the
authors have waived all copyright and related or neighboring rights to the
files in this folder (the CC0 1.0 Universal dedication,
https://creativecommons.org/publicdomain/zero/1.0/). The player's code
stays GPL-3.0-or-later.

Made by `tools/autodj_ref.py`, the pure-Python reference of the Auto DJ
engine's pick rule, and frozen: `python tools/autodj_ref.py --check` fails
if a file here differs from what it makes. `test/test_autodj_engine`
replays every session on `lib/core/AutoDjEngine` and must get every pick,
every prefetch read and the counters exactly; `tools/test_autodj_ref.py`
tests the reference itself.

`sessions.json`:
- `rng`: PCG32 (pcg32_srandom_r(seed, stream), XSH RR) outputs and
  `below(n)` draws (Lemire's method), the engine's one stream.
- `tables`: `golden` names a file in `test/fixtures/card/golden/` (read
  through `mpdj::Table`); otherwise a synthetic table's rows, k, selHead,
  selFloor and each row's used slot entries as hex, 6 digits an entry (the
  row, then the level).
- `sessions`: each a table (none: no index), the maps (`trackToRow`,
  `rowToTrack`, 65535 none), the filter words (bpm | camelot << 8 |
  artist16 << 16; BPM_ANALYSED and "present" in bits 13 and 15), the
  library's track count, the faults the slot reader injects by row
  (`damaged`: the CRC always fails; `flaky`: on the first read only;
  `io`: the first read fails, Why::Io; `broken`: a good CRC over a bad
  list; a `faults` event replaces them), the events,
  and the counters at the end. Events: `new` (a session: its PCG32 seed
  and seed track, 4294967295 none), `start` (a track starts: `dj`, Auto
  DJ's pick), `left` (a track leaves: ms played, a natural end or not),
  `mode` (Anchor changed), `reopen` (the file reopened), `renumber` (the
  library rebuilt: the by-id cooldown cleared), `index` (`on` 0: the index
  away from now, the engine given no maps and no slots; 1: back), `prefetch` (one loop pass
  in mode `m`: the row it reads, 65535 none, and the read's result),
  `pick` and `surprise` (the settings, the playing track, and every field
  of the result, a field left out being 0).

Synthetic data only: no real names, no embeddings.
"""


def rng_vectors():
    p = Pcg32(42, 54)
    nexts = [p.next() for _ in range(6)]
    q = Pcg32(1, STREAM)
    cases = []
    for n in (1, 2, 3, 6, 7, 50, 64, 169, 1000, 19410, 65535, 0x80000001, 0xFFFFFFFF):
        cases.append({"n": n, "draws": [q.below(n) for _ in range(4)]})
    return {"seed": 42, "stream": 54, "next": nexts, "sessionStream": STREAM, "belowSeed": 1, "below": cases}


def generate():
    tables, sc = scenes()
    goldens = {n.rsplit(".", 1)[0] for n in GOLDENS}
    tj = {}
    for name, t in tables.items():
        if name not in goldens:
            tj[name] = {"rows": t["rows"], "k": t["k"], "selHead": t["selHead"], "selFloor": t["selFloor"],
                        "slots": encode_lists(t["lists"])}
        else:
            tj[name] = {"golden": name + ".bin"}
    doc = {"about": "Auto DJ engine sessions (tools/autodj_ref.py; README.md)", "rng": rng_vectors(),
           "floors": [{"percent": p, "selFloor": f, "level": floor_level(p, f)}
                      for p in (0, 30, 55, 60, 65, 70, 75, 100) for f in (0, 100, 140, 160)],
           "tables": tj, "sessions": [s.json() for s in sc]}
    return {"sessions.json": dumps(doc).encode("utf-8"), "README.md": README.encode("utf-8")}


def dumps(doc):
    """One line per event, the rest indented: diffs stay readable."""
    def compact(v):
        return json.dumps(v, separators=(",", ":"))
    lines = ["{"]
    keys = list(doc)
    for i, k in enumerate(keys):
        end = "," if i < len(keys) - 1 else ""
        v = doc[k]
        if k == "tables":
            lines.append(' "tables": {')
            names = list(v)
            for j, n in enumerate(names):
                lines.append("  %s: %s%s" % (json.dumps(n), compact(v[n]), "," if j < len(names) - 1 else ""))
            lines.append(" }" + end)
        elif k == "sessions":
            lines.append(' "sessions": [')
            for j, s in enumerate(v):
                lines.append("  {")
                sk = list(s)
                for m, key in enumerate(sk):
                    e2 = "," if m < len(sk) - 1 else ""
                    if key == "events":
                        lines.append('   "events": [')
                        for q, ev in enumerate(s[key]):
                            lines.append("    " + compact(ev) + ("," if q < len(s[key]) - 1 else ""))
                        lines.append("   ]" + e2)
                    else:
                        lines.append("   %s: %s%s" % (json.dumps(key), compact(s[key]), e2))
                lines.append("  }" + ("," if j < len(v) - 1 else ""))
            lines.append(" ]" + end)
        else:
            lines.append(" %s: %s%s" % (json.dumps(k), compact(v), end))
    lines.append("}")
    return "\n".join(lines) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="compare with the files on disk; write nothing")
    args = ap.parse_args(argv)
    files = generate()
    # A file in the folder that generate() doesn't make is stale; .gitattributes
    # is the folder's own.
    extra = sorted(q.name for q in OUT.glob("*")
                   if q.is_file() and q.name not in files and q.name != ".gitattributes") if OUT.is_dir() else []
    if args.check:
        stale = [n for n, d in files.items() if not (OUT / n).is_file() or (OUT / n).read_bytes() != d]
        if stale or extra:
            sys.exit("autodj fixtures differ from tools/autodj_ref.py: " + ", ".join(stale) +
                     ("; not made by it: " + ", ".join(extra) if extra else ""))
        print("autodj fixtures: %d files up to date" % len(files))
        return
    OUT.mkdir(parents=True, exist_ok=True)
    for n in extra:
        (OUT / n).unlink()
    for n, d in files.items():
        (OUT / n).write_bytes(d)
    print("autodj fixtures: wrote %d files to %s" % (len(files), OUT.relative_to(ROOT).as_posix()))


if __name__ == "__main__":
    main()
