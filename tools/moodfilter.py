#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Mood filters' data (docs/MOOD-FILTERS.md 4.1, 5.3, 5.4): a record's mood
tail (docs/METADATA.md 2.6.4, MPTG minor 1), a track's word in library.idx
(METADATA 3.4.3), the filter's word and the predicate, written from the
spec.

Every rule a second implementation must reproduce is here in its plainest
form, with the section it implements. card_fixtures.py imports it for
tags-moods.bin, the mood hardening files and moods.json; autodj_ref.py,
synthtransfer.py and test_mood_vectors.py import it too, so the rules live
in one Python source. Standard library only.

- `tail_of(scores)`: a record's bytes 72-79 from mStream's scores (5.3).
- `record_tail(row, stride)`, `tail_valid(tail)`, `word_of_tail(tail)`:
  what a reader takes (5.3, 5.4).
- `pack`, `unpack`, `q_valid`, `active`: the filter's 11 bits (5.4).
- `matches(word, q)`: the word's predicate; `matches_scores(scores, q)`:
  4.1's rules over the raw scores, as mStream's SQL reads them, for
  cross-checks; `album_matches(words, q)`: an album's majority.
"""
import math

# mStream's track_moods score columns, in its order (mStream
# src/db/mood-sync.js MOOD_COLUMNS): a description's "scores", a reply's
# "heads" and every score list here are in this order.
MOOD_COLUMNS = ("danceable", "happy", "sad", "relaxed", "aggressive", "party", "acoustic", "instrumental")

# ---------------------------------------------------------------------------
# The tail (5.3): a RECS record's bytes 72-79, with a stride of 80 or more
# ---------------------------------------------------------------------------
TAIL_AT = 72                 # the tail's first byte in a record
TAIL_BYTES = 8
TAIL_STRIDE = 80             # a stride under this means no record has a tail
NULL = 255                   # a value byte for a NULL score
PRESENT = 1 << 0             # moodFlags bit 0
SAD_WINS = 1 << 1            # bit 1: sad beat happy
AGGRESSIVE_WINS = 1 << 2     # bit 2: aggressive beat relaxed
FLAGS_RESERVED = 0xF8        # bits 3-7: 0, and never given a meaning in major 1
NO_ROW = bytes(TAIL_BYTES)   # no server row


def value(score):
    """A score as a value byte: min(100, max(0, floor(score*100 + 1e-9)))
    in f64, so 0.29 is 29 (plain floor gives 28; also 0.57, 0.58); 255 for
    NULL. Clamping before the floor is the same, as 0 and 100 are whole."""
    if score is None:
        return NULL
    return math.floor(min(100.0, max(0.0, float(score) * 100 + 1e-9)))


def tail_of(scores):
    """The 8 tail bytes a writer stores for a server row: `scores` are 8
    numbers or None, in MOOD_COLUMNS order; None (no row) gives 8 zero
    bytes. Each scale's byte is its larger head, 255 when either is NULL;
    its order bit comes from the raw doubles (a tie is happy, relaxed) and
    is 0 when either head is NULL."""
    if scores is None:
        return NO_ROW
    assert len(scores) == len(MOOD_COLUMNS), scores
    s = dict(zip(MOOD_COLUMNS, scores))
    flags = PRESENT

    def scale(lo, hi, bit):
        nonlocal flags
        if s[lo] is None or s[hi] is None:
            return NULL
        if s[hi] > s[lo]:
            flags |= bit
        return value(max(s[lo], s[hi]))

    valence = scale("happy", "sad", SAD_WINS)
    energy = scale("relaxed", "aggressive", AGGRESSIVE_WINS)
    return bytes([flags, valence, energy, value(s["instrumental"]), value(s["acoustic"]), value(s["party"]),
                  value(s["danceable"]), 0])


def record_tail(row, stride):
    """A record's tail as a reader finds it: bytes 72-79 of a row of
    `stride` bytes, or None when the stride is under 80 (no tail, though
    2.4.5 reads a shorter record's missing fields as zeros)."""
    if stride < TAIL_STRIDE:
        return None
    return bytes(row[TAIL_AT:TAIL_AT + TAIL_BYTES])


def tail_valid(tail):
    """Whether a reader takes the tail (5.3): bit 0 set, bits 3-7 clear,
    byte 79 zero, and each of bytes 73-78 0-100 or 255. Otherwise the
    record has no scores, and the record and the file are fine."""
    return (tail is not None and len(tail) == TAIL_BYTES and bool(tail[0] & PRESENT)
            and not tail[0] & FLAGS_RESERVED and tail[7] == 0
            and all(v <= 100 or v == NULL for v in tail[1:7]))


# ---------------------------------------------------------------------------
# The word (5.4): a u16 per track in library.idx v7
# ---------------------------------------------------------------------------
ANALYSED = 1 << 15
VOICE_NULL = 1 << 14
VALENCE_AT, ENERGY_AT = 11, 8            # 3 bits each: side << 2 | bucket, 4 NULL
INSTRUMENTAL_AT, ACOUSTIC_AT, PARTY_AT, DANCEABLE_AT = 6, 4, 2, 0  # 2 bits each: the bucket
SCALE_NULL = 4
BUCKET_BARS = (30, 50, 70)               # bucket 0 under 30, 1 for 30-49, 2 for 50-69, 3 for 70 and up


def bucket(v):
    """A value byte's bucket, 0-3."""
    return sum(v >= b for b in BUCKET_BARS)


def word_of_tail(tail):
    """A track's word from its record's tail: 0 (no row) when the reader
    doesn't take the tail. A scale's field is side << 2 | bucket, 0 for
    bucket 0 (no side), 4 for NULL, whose order bit is ignored. The voice
    NULL bit marks a NULL instrumental; a NULL tag stores 0, which never
    holds."""
    if not tail_valid(tail):
        return 0

    def scale(v, side):
        if v == NULL:
            return SCALE_NULL
        b = bucket(v)
        return side << 2 | b if b else 0

    def head(v):
        return 0 if v == NULL else bucket(v)

    w = ANALYSED
    w |= scale(tail[1], tail[0] >> 1 & 1) << VALENCE_AT
    w |= scale(tail[2], tail[0] >> 2 & 1) << ENERGY_AT
    if tail[3] == NULL:
        w |= VOICE_NULL
    w |= head(tail[3]) << INSTRUMENTAL_AT | head(tail[4]) << ACOUSTIC_AT
    w |= head(tail[5]) << PARTY_AT | head(tail[6]) << DANCEABLE_AT
    return w


def word_of(scores):
    """A track's word from a server row's scores (None: no row, word 0)."""
    return word_of_tail(tail_of(scores))


# ---------------------------------------------------------------------------
# The filter's 11 bits, q (5.4), and the levels (4.1)
# ---------------------------------------------------------------------------
VALENCE = (None, "happy", "neutral", "sad")         # bits 0-1
ENERGY = (None, "relaxed", "neutral", "aggressive")  # bits 2-3
VOICE = (None, "instrumental", "vocals")             # bits 4-5; 3 is invalid
TAGS = ("acoustic", "party", "danceable")            # bits 6, 7, 8
LEVEL_CODES = ("standard", "strict", "loose")        # bits 9-10; 3 is invalid
GROUPS = 0x1FF                                       # the bits that filter; the level alone filters nothing

# Each level's bars, on / mirror (phone app lib/objects/mood_filter.dart's
# MoodLevel; mStream's `threshold` and 1 - it), and the word's reading of
# them in buckets: a head that must hold has bucket >= need, one that must
# not has bucket < under.
LEVELS = {
    "strict": {"code": 1, "on": 0.7, "mirror": 0.3, "need": 3, "under": 1},
    "standard": {"code": 0, "on": 0.5, "mirror": 0.5, "need": 2, "under": 2},
    "loose": {"code": 2, "on": 0.3, "mirror": 0.7, "need": 1, "under": 3},
}

# The 11 predicates of 4.1, each a one-group filter's bits at Standard, in
# mStream's names (group.value): with a level's code in bits 9-10, the
# 33 checks of a row.
PREDICATES = (("valence.happy", 1), ("valence.neutral", 2), ("valence.sad", 3),
              ("energy.relaxed", 1 << 2), ("energy.neutral", 2 << 2), ("energy.aggressive", 3 << 2),
              ("voice.instrumental", 1 << 4), ("voice.vocals", 2 << 4),
              ("tags.acoustic", 1 << 6), ("tags.party", 1 << 7), ("tags.danceable", 1 << 8))


def q_valid(q):
    """Any bit 11-15, voice 3 or level 3 makes the whole word invalid."""
    return 0 <= q < 1 << 11 and q >> 4 & 3 != 3 and q >> 9 & 3 != 3


def read_q(q):
    """q as read: an invalid word reads as 0 (no filter, Standard)."""
    return q if q_valid(q) else 0


def active(q):
    """Whether q filters: a group set. A level alone filters nothing."""
    return bool(read_q(q) & GROUPS)


def pack(valence=None, energy=None, voice=None, tags=(), level="standard"):
    """The filter's 11 bits from its groups (None: left out) and level."""
    q = VALENCE.index(valence) | ENERGY.index(energy) << 2 | VOICE.index(voice) << 4
    for t in tags:
        q |= 1 << (6 + TAGS.index(t))
    return q | LEVELS[level]["code"] << 9


def unpack(q):
    """q's groups and level, as read (an invalid q is 0's)."""
    q = read_q(q)
    return {"valence": VALENCE[q & 3], "energy": ENERGY[q >> 2 & 3], "voice": VOICE[q >> 4 & 3],
            "tags": [t for i, t in enumerate(TAGS) if q >> (6 + i) & 1], "level": LEVEL_CODES[q >> 9 & 3]}


def predicate_q(name, level):
    """The one-group filter of a predicate at a level."""
    return dict(PREDICATES)[name] | LEVELS[level]["code"] << 9


# ---------------------------------------------------------------------------
# The predicates
# ---------------------------------------------------------------------------
def matches(word, q):
    """Whether a track's word passes the filter q (5.4). A filter that sets
    no group passes every track, scored or not; any other passes no track
    without a row (word 0). Groups are ANDed; a NULL scale fails only
    itself."""
    if not active(q):
        return True
    if not word & ANALYSED:
        return False
    f = unpack(q)
    need, under = LEVELS[f["level"]]["need"], LEVELS[f["level"]]["under"]

    def scale(at, value, lo):
        field = word >> at & 7
        if value is None:
            return True
        if field == SCALE_NULL:
            return False
        if value == "neutral":
            return field & 3 < under
        return field >> 2 == (0 if value == lo else 1) and field & 3 >= need

    if not scale(VALENCE_AT, f["valence"], "happy") or not scale(ENERGY_AT, f["energy"], "relaxed"):
        return False
    instrumental = word >> INSTRUMENTAL_AT & 3
    if f["voice"] == "instrumental" and instrumental < need:
        return False
    if f["voice"] == "vocals" and (word & VOICE_NULL or instrumental >= under):
        return False
    at = {"acoustic": ACOUSTIC_AT, "party": PARTY_AT, "danceable": DANCEABLE_AT}
    return all(word >> at[t] & 3 >= need for t in f["tags"])


def matches_scores(scores, q):
    """4.1's rules over a row's raw scores (None: no row), as mStream's SQL
    compares them in doubles: the reference the word must agree with.
    Happy and relaxed take a tie; a NULL head fails its scale."""
    if not active(q):
        return True
    if scores is None:
        return False
    f = unpack(q)
    s = dict(zip(MOOD_COLUMNS, scores))
    on, mirror = LEVELS[f["level"]]["on"], LEVELS[f["level"]]["mirror"]

    def scale(value, lo, hi):
        a, b = s[lo], s[hi]
        if value is None:
            return True
        if a is None or b is None:
            return False
        if value == "neutral":
            return a < mirror and b < mirror
        if value == lo:
            return a >= on and a >= b
        return b >= on and b > a

    if not scale(f["valence"], "happy", "sad") or not scale(f["energy"], "relaxed", "aggressive"):
        return False
    i = s["instrumental"]
    if f["voice"] == "instrumental" and not (i is not None and i >= on):
        return False
    if f["voice"] == "vocals" and not (i is not None and i < mirror):
        return False
    return all(s[t] is not None and s[t] >= on for t in f["tags"])


def holds(word):
    """The predicates a word passes, by level: {level: [names]} in
    PREDICATES order, levels Strict, Standard, Loose."""
    return {lv: [name for name, _ in PREDICATES if matches(word, predicate_q(name, lv))]
            for lv in ("strict", "standard", "loose")}


def album_counts(words, q):
    """(pass, analysed) over an album's tracks on this card: only tracks
    with a word count, an all-NULL one among them."""
    analysed = [w for w in words if w & ANALYSED]
    return sum(matches(w, q) for w in analysed), len(analysed)


def album_matches(words, q):
    """An album matches when 2 * pass > analysed, so one with no analysed
    track never does (under an active filter; an inactive one filters
    nothing)."""
    if not active(q):
        return True
    passed, analysed = album_counts(words, q)
    return 2 * passed > analysed
