#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""synthtransfer.py: a synthetic transfer for a test card: the /.mstream
that gives a card tree an AutoDJ index the firmware accepts (docs/AUTODJ-
INDEX.md 11.8; the Auto DJ engine's device test card).

A card made by tools/synthcard.py has /music and no /.mstream, so the
firmware finds no transfer data and Auto DJ never sees an index. This
writes, into the card's /.mstream only:

- tags-<g>.bin: an MPTG tags file (source 2) with **zero records**
  (docs/METADATA.md 2.6): the root needs a tags file that opens and matches
  its COMP entry. With no records the library build is unchanged: every
  file is read as the device reads it today (2.9);
- autodj-<g>.bin: an AutoDJ index (MPDJ v2, docs/AUTODJ-INDEX.md), either
  --synthetic (below) or --mpdj FILE (a file built elsewhere for this tree,
  restamped to this commit);
- manifest.bin: the root (MSMF, 2.5) naming both, generation g, written
  last, through manifest.tmp (the commit order of 2.12.1 and AUTODJ-INDEX.md
  7.2): a cut before the rename leaves no root, or the old one.

It never formats anything and never writes or deletes anything outside
/.mstream. It refuses a card whose /.mstream isn't empty unless --replace,
which then commits the next generation (the card's id kept) and, after the
new root is in place, deletes only the contract's files the new root
doesn't name (manifest.tmp, pending.bin and .tmp, tags-*.bin,
autodj-*.bin); thumbs/, stage/ and resume/ stay, and so do the software's
private files (state.json, resume.json, run.lock, their .new, any other
name: METADATA 2.12.7, 2.12.8), whose names never match a contract
file's. --replace takes only a /.mstream this tool wrote (its root's
producer): one another program wrote (a transfer's, whose tags file,
index and plan would go), contract files with no root, or a run.lock (a
transfer may be writing the card, or one was cut: 2.12.7) are refused
unless --replace-foreign, which still leaves the run.lock and the private
files alone.

It is a test card's index, not a transfer's: DJRF's BPM, key and artist
keys are synthesized here (as if the records carried mStream's analysis),
while the tags file holds no records. The device never cross-checks them;
the real transfer software follows METADATA 2.13.2.

Usage:

    python tools/synthtransfer.py --add-to CARD --synthetic [--seed S] [--dry-run]
    python tools/synthtransfer.py --add-to CARD --mpdj FILE [--dry-run]
    python tools/synthtransfer.py --check CARD

CARD is a card's root (E:\\) or a folder holding a card tree (synthcard's
--out folder). Tracks are the files the device's walk lists: .mp3, .flac
and .opus under /music, no name starting with a dot, at most 8 folder
levels and 248 bytes below /music (lib/core/CardWalk, LibraryIndex).

--synthetic builds the index here, deterministic from --seed: one row a
track (a seeded --unindexed percent, default 1, left without a row, as
tracks the server never embedded); rows keyed by a made-up canonical hash.
Each row's record values come from synthcard-files.jsonl (CARD's, or
--tags FILE) through METADATA 2.3.6 (card_fixtures.build_field); a track it
doesn't list gets its top folder as its artist and no title. The vectors
are clustered: each artist belongs to one of --genres genres (default 16),
each genre owns 12 dimensions of its own, and a track's vector is the
genre's axis plus its artist's, its album's and its own random directions
(weights 1, 0.9, 0.5, 0.85), normalised, as f32. Tracks of two genres are
orthogonal (cosine 0), so the lists are computed exactly in pure Python
from the in-genre levels (every pair of a genre by 5.2's rule; every other
row is a level-0 candidate, in row order): the bytes equal tools/mpdj.py's
rule as written (tools/test_synthtransfer.py checks it). BPM by genre and
artist (about 5 % none), the Camelot key from a pitch-class prior with a
genre's minor share and an artist's home key (about 5 % none), both
BPM_ANALYSED. The selection is the writer's default S84-55-169 (--k,
--head, --floor for small tests). About a minute and a half for
synthcard.py's default tree (19,410 tracks, 14 M in-genre pairs), standard
library only.

--mpdj FILE takes an index already built for this tree (the research's
synthetic-card file, say): it must check whole (8.6), and every DJPH path
must be a track of CARD/music (a file the device's walk lists, below;
--allow-missing to accept fewer). Its
sections are kept byte for byte; generation, cardId and tagsGeneration are
rewritten to this commit's and the header CRC made again (7.3's case with
the same signature and the same DJRF, DJPH and STRS: tools/mpdj.py
restamp's output, without its description).

--dry-run builds and checks everything in memory and prints what it would
write; nothing is written. --check reads a card's /.mstream back as the
boot does (a Python mirror of cardroot::read and Auto DJ's open: the
election, the root, T, the MPDJ entry, mpdj.py's check) and says how many
of the card's tracks map to a row; exit 1 when there is no usable index.

The first boot after a new root rebuilds the library (the commit identity
changed, Library.cpp) before the UI.

Its tests: python -m unittest discover -s tools -p "test_synthtransfer.py"
(the lists against tools/mpdj.py's rule; the files read back by the
firmware's own readers on the host, cardroot::read and mpdj::Table, built
with g++ when there is one; the refusals).
"""
import argparse
import bisect
import hashlib
import json
import math
import os
import random
import re
import shutil
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import card_fixtures as cf  # noqa: E402  (mptg, msmf_raw, comp_row, build_field: docs/METADATA.md 2.17)
import mpdj  # noqa: E402  (prepare, write, check, encode_slot: docs/AUTODJ-INDEX.md)
import synthcard  # noqa: E402  (long_path, DriveProbe)

PRODUCER = "mstream-mp3-player tools/synthtransfer.py 1"
TRACK_EXTS = ("mp3", "flac", "opus")
MAX_DEPTH = 8                  # folder levels below /music (CardWalk.h kMaxDepth)
MAX_REL = 255 - len("/music/")  # a track's path below /music, in bytes (CardContract.h kMaxRelPath)
TAGS_JSONL = "synthcard-files.jsonl"
MSTREAM = ".mstream"
FINAL = 1                      # the root's flags: this commit ended a run
# The contract's own files at /.mstream's top (2.2): a private file's name
# never matches one (2.12.7; card_fixtures.private_name), so --replace keeps
# them.
CONTRACT = re.compile(r"^(manifest\.(bin|tmp)|pending\.(bin|tmp)|tags-[0-9a-f]{8}\.bin|autodj-[0-9a-f]{8}\.bin)$",
                      re.IGNORECASE)
GENERATION_NAME = re.compile(r"^(tags|autodj)-([0-9a-f]{8})\.bin$", re.IGNORECASE)

# The synthetic index's strings (AUTODJ-INDEX.md 4.1): made-up vectors, CC0.
STRINGS = {"modelId": "synthetic-embedding", "modelVersion": "1", "metric": "cosine", "license": "CC0-1.0",
           "attribution": "synthetic test-card vectors, not from any model"}
BLOCK = 12                                   # dimensions a genre owns (block_lists unrolls 12)
WEIGHTS = (1.0, 0.9, 0.5, 0.85)              # the genre axis, the artist, the album, the track
GENRE_TEMPOS = (72, 84, 92, 98, 105, 112, 120, 124, 128, 132, 140, 150, 160, 172)
PC_PRIOR = (1.0, 0.45, 0.85, 0.4, 0.7, 0.7, 0.4, 1.0, 0.45, 0.9, 0.55, 0.45)  # C to B
NO_BPM, NO_KEY, HOME_KEY, OCTAVE = 0.05, 0.05, 0.35, 0.06

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):
    pass


class Refused(Exception):
    """The tool won't do it; the message says why."""


# ---------------------------------------------------------------------------
# The card
# ---------------------------------------------------------------------------
def card_root(path):
    p = path.strip()
    if len(p) == 2 and p[1] == ":" and p[0].isalpha():
        p += "\\"
    return os.path.abspath(p)


def is_drive_root(root):
    drive, rest = os.path.splitdrive(root)
    return bool(drive) and rest in ("\\", "/")


def file_exists(path):
    return os.path.isfile(synthcard.long_path(path))


def walk_music(card):
    """(tracks, files): every file under CARD/music as rel paths (names as
    the directory stores them, '/' between), and the tracks the device's
    walk lists among them. Both sorted by their UTF-8 bytes."""
    music = os.path.join(card, "music")
    tracks, files = [], []

    def walk(full, rel, depth, seen):
        try:
            entries = list(os.scandir(full))
        except OSError as e:
            raise Refused("can't list %s: %s" % (full, e))
        for e in entries:
            r = e.name if not rel else rel + "/" + e.name
            try:
                r.encode("utf-8")
            except UnicodeEncodeError:
                continue  # a name no card stores (a lone surrogate): no path hash
            if e.is_dir(follow_symlinks=False):
                walk(e.path, r, depth + 1, seen and not e.name.startswith(".") and depth + 1 <= MAX_DEPTH)
                continue
            files.append(r)
            ext = e.name.rsplit(".", 1)[-1].lower() if "." in e.name[1:] else ""
            if (seen and not e.name.startswith(".") and ext in TRACK_EXTS
                    and len(r.encode("utf-8")) <= MAX_REL):
                tracks.append(r)
    walk(synthcard.long_path(music), "", 0, True)
    key = lambda r: r.encode("utf-8")  # noqa: E731
    return sorted(tracks, key=key), sorted(files, key=key)


def record_values(tags):
    """A file's record title (string-run field 0) and artist values (field
    1), through METADATA 2.3.6; None for no title."""
    def vals(v):
        if v is None:
            return []
        return v if isinstance(v, list) else [v]
    title, _ = cf.build_field(vals(tags.get("title")), False)
    artists, _ = cf.build_field(vals(tags.get("artist")), True)
    return (title.decode("utf-8") if title else None), (artists.decode("utf-8").split("\x1f") if artists else [])


def read_tags(path):
    """{rel: (title, artists)} from a synthcard-files.jsonl."""
    out = {}
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            if not line.strip():
                continue
            j = json.loads(line)
            rel = j["path"][len("music/"):] if j["path"].startswith("music/") else j["path"]
            out[rel] = record_values(j.get("tags") or {})
    return out


def synthcard_line(card):
    p = os.path.join(card, "SYNTHCARD.TXT")
    if not os.path.isfile(p):
        return None
    with open(p, encoding="utf-8", errors="replace") as fh:
        return fh.readline().strip()


# ---------------------------------------------------------------------------
# The synthetic index (--synthetic)
# ---------------------------------------------------------------------------
class Rng:
    """A seeded stream keyed by names (the same artist gets the same
    direction whatever else the tree holds). Only IEEE's basic operations
    on Mersenne Twister's doubles, so every platform draws the same bits."""

    def __init__(self, *parts):
        h = hashlib.sha256("\0".join(str(p) for p in parts).encode("utf-8")).digest()
        self.r = random.Random(int.from_bytes(h[:8], "little"))

    def u(self):
        return self.r.random()

    def normal(self):
        """About N(0, 1): Irwin-Hall's sum of 12 uniforms, less 6."""
        s = 0.0
        for _ in range(12):
            s += self.r.random()
        return s - 6.0

    def unit(self, d):
        v = [self.normal() for _ in range(d)]
        n = math.sqrt(sum_in_order(x * x for x in v))
        return [x / n for x in v] if n > 0 else [1.0] + [0.0] * (d - 1)

    def pick(self, cum):
        """An index by cumulative weights."""
        return min(bisect.bisect_right(cum, self.u() * cum[-1]), len(cum) - 1)


def sum_in_order(xs):
    s = 0.0
    for x in xs:
        s += x
    return s


def camelot(pc, minor):
    """The Camelot code (AUTODJ-INDEX.md 4.4: 1-12 = 1A-12A minor, 13-24 =
    1B-12B major) of pitch class pc (C = 0): C major 8B, A minor 8A."""
    n = (8 + 7 * (pc - (9 if minor else 0))) % 12 or 12
    return n if minor else 12 + n


def cumulative(ws):
    out, s = [], 0.0
    for w in ws:
        s += w
        out.append(s)
    return out


PC_CUM = cumulative(PC_PRIOR)


class Genre:
    def __init__(self, seed, g):
        r = Rng(seed, "genre", g)
        self.weight = 0.5 + r.u()
        self.tempo = GENRE_TEMPOS[int(r.u() * len(GENRE_TEMPOS))]
        self.minor = 0.25 + 0.4 * r.u()


class Artist:
    def __init__(self, seed, name, cum, genres):
        r = Rng(seed, "artist", name)
        self.genre = r.pick(cum)
        self.dir = r.unit(BLOCK)
        self.tempo = genres[self.genre].tempo + 6.0 * r.normal()
        self.home = camelot(r.pick(PC_CUM), r.u() < genres[self.genre].minor)


def track_values(seed, rel, artist, genre):
    """(bpm10, camelot, analysed) for a track."""
    r = Rng(seed, "track-music", rel)
    bpm10 = 0
    if r.u() >= NO_BPM:
        bpm = artist.tempo + 3.0 * r.normal()
        o = r.u()
        bpm = bpm / 2 if o < OCTAVE / 2 else bpm * 2 if o < OCTAVE else bpm
        bpm = min(220.0, max(50.0, bpm))
        bpm10 = int(bpm * 10.0 + 0.5)
    key = 0
    if r.u() >= NO_KEY:
        key = artist.home if r.u() < HOME_KEY else camelot(r.pick(PC_CUM), r.u() < genre.minor)
    return bpm10, key, bpm10 > 0


def synthetic_rows(tracks, tags, seed, genres=16, unindexed=1.0):
    """The description's rows and, by canonical hash, each row's genre and
    its 12 f32 components there. Returns (rows, info, dim, stats)."""
    gs = [Genre(seed, g) for g in range(genres)]
    cum = cumulative([g.weight for g in gs])
    artists, albums = {}, {}
    rows, info = [], {}
    stats = {"tracks": len(tracks), "unindexed": 0, "tagged": 0, "bpm": 0, "key": 0}
    wg, wa, wb, we = WEIGHTS
    for rel in tracks:
        r = Rng(seed, "track", rel)
        if r.u() * 100.0 < unindexed:
            stats["unindexed"] += 1
            continue
        parts = rel.split("/")
        artist_name = parts[0] if len(parts) > 1 else ""
        a = artists.get(artist_name)
        if a is None:
            a = artists[artist_name] = Artist(seed, artist_name, cum, gs)
        album = "/".join(parts[:-1])
        b = albums.get(album)
        if b is None:
            b = albums[album] = Rng(seed, "album", album).unit(BLOCK)
        e = r.unit(BLOCK)
        v = [wg * (1.0 if i == 0 else 0.0) + wa * a.dir[i] + wb * b[i] + we * e[i] for i in range(BLOCK)]
        n = math.sqrt(sum_in_order(x * x for x in v))
        vec = tuple(mpdj.f32(x / n) for x in v)
        if rel in tags:
            title, values = tags[rel]
            stats["tagged"] += 1
        else:
            title, values = None, [artist_name] if artist_name else []
        bpm10, key, analysed = track_values(seed, rel, a, gs[a.genre])
        stats["bpm"] += bpm10 > 0
        stats["key"] += key > 0
        h = hashlib.md5(("synthtransfer 1\n" + rel).encode("utf-8")).hexdigest()
        f = {"path": rel, "artists": values, "bpm10": bpm10, "camelot": key, "bpmAnalysed": analysed}
        if title is not None:
            f["title"] = title
        rows.append({"hash": h, "files": [f]})
        info[h] = (a.genre, vec)
    stats["rows"] = len(rows)
    stats["genres"] = len({g for g, _ in info.values()})
    stats["artists"] = len(artists)
    return rows, info, genres * BLOCK, stats


def vectors_bytes(rows, info, dim):
    """The rows' f32 vectors, little-endian, in the description's order: the
    genre's 12 components at its offset, zeros elsewhere."""
    out = bytearray(4 * dim * len(rows))
    fmt = struct.Struct("<%df" % BLOCK)
    for i, row in enumerate(rows):
        g, vec = info[row["hash"]]
        fmt.pack_into(out, 4 * (i * dim + g * BLOCK), *vec)
    return bytes(out)


def block_lists(p, info, log=None):
    """DJSL's bytes for a Prepared description whose rows' vectors live in
    their genres' blocks (info: hash -> (genre, 12 components)): the same
    entries as mpdj.lists_rule, computed per genre. A row's cosine to a row
    of another genre sums only products of zero (level 0); to a row of its
    own, it is the in-order sum over the 12 shared components, the zeros
    around them adding exact zeros. So a row's candidates are its genre's
    rows by level, then every other level-0 row by row index."""
    n, k, kind, head, floor = len(p.hashes), p.k, p.kind, p.head, p.floor
    ids = mpdj.song_ids(p.keys)
    blocks = {}
    for r, h in enumerate(p.hashes):
        blocks.setdefault(info[h][0], []).append(r)
    ranks = {}
    lists = [None] * n
    pairs, t0 = 0, time.time()
    said = t0
    for g in sorted(blocks):
        mem = blocks[g]
        vs = [info[p.hashes[r]][1] for r in mem]
        c = len(mem)
        pairs += c * (c - 1) // 2
        M = [[0] * c for _ in range(c)]
        for i in range(c):
            a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11 = vs[i]
            Mi = M[i]
            for j in range(i + 1, c):
                b0, b1, b2, b3, b4, b5, b6, b7, b8, b9, b10, b11 = vs[j]
                # The rule's sum: each product added in index order, in f64
                # (Python adds left to right; never sum(), which compensates
                # from Python 3.12).
                x = (a0 * b0 + a1 * b1 + a2 * b2 + a3 * b3 + a4 * b4 + a5 * b5 + a6 * b6 + a7 * b7 + a8 * b8 +
                     a9 * b9 + a10 * b10 + a11 * b11) * 255.0
                if x > 0.0:
                    f = math.floor(x)
                    lv = f + 1 if x - f >= 0.5 else f
                    lv = 255 if lv > 255 else int(lv)
                else:
                    lv = 0
                Mi[j] = lv
                M[j][i] = lv
        for i, a in enumerate(mem):
            Mi, ida = M[i], ids[a]
            cand = [j for j in range(c) if Mi[j] > 0 and ids[mem[j]] != ida]
            cand.sort(key=Mi.__getitem__, reverse=True)  # stable: equal levels keep row order
            m = 0
            while m < len(cand) and Mi[cand[m]] >= floor:
                m += 1
            if kind == mpdj.SEL_STRIDED and m > k:
                if m not in ranks:
                    ranks[m] = mpdj.strided_ranks(k, head, m)
                entries = [(mem[cand[q]], Mi[cand[q]]) for q in ranks[m]]
            else:
                entries = [(mem[j], Mi[j]) for j in cand[:k]]
                if len(entries) < k:
                    taken = {e[0] for e in entries}
                    b = 0
                    while len(entries) < k and b < n:
                        if b not in taken and ids[b] != ida:
                            entries.append((b, 0))
                        b += 1
            lists[a] = entries
        if log and time.time() - said > 5:
            said = time.time()
            log("  lists: %d of %d genres, %.1f M pairs, %.0f s" % (sorted(blocks).index(g) + 1, len(blocks),
                                                                   pairs / 1e6, said - t0))
    if log:
        log("lists: %d genres of %d to %d rows, %.1f M pairs by the rule, in %.1f s" % (
            len(blocks), min(map(len, blocks.values())), max(map(len, blocks.values())), pairs / 1e6,
            time.time() - t0))
    return b"".join(mpdj.encode_slot(r, lists[r], k, p.slot_bytes) for r in range(n))


def synthetic_index(tracks, tags, seed, generation, card_id, built_time, k=mpdj.DEFAULT_K,
                    head=mpdj.DEFAULT_HEAD, floor=mpdj.DEFAULT_FLOOR, genres=16, unindexed=1.0, log=None):
    """(the MPDJ file's bytes, stats)."""
    t0 = time.time()
    rows, info, dim, stats = synthetic_rows(tracks, tags, seed, genres, unindexed)
    if not rows:
        raise Refused("no track has a row: nothing to index")
    if len(rows) > mpdj.MAX_ROWS:
        raise Refused("%d rows: an index holds at most %d" % (len(rows), mpdj.MAX_ROWS))
    desc = dict(STRINGS, format="MPDJ", generation=generation, cardId=card_id, builtTime=built_time,
                tagsGeneration=generation, k=k, selKind=mpdj.SEL_STRIDED, selHead=head, selFloor=floor, dim=dim,
                vectors="synthetic.f32", rows=rows)
    if k < 3:
        desc.update(selKind=mpdj.SEL_TOP, selHead=k, selFloor=0)
    try:
        p = mpdj.prepare(desc, sidecars={"synthetic.f32": vectors_bytes(rows, info, dim)})
    except mpdj.WriterError as e:
        raise Refused("the index's description: %s" % e)
    stats["prepareSeconds"] = round(time.time() - t0, 1)
    if log:
        log("rows %d (%d tracks, %d left without a row), %d genres, dim %d; %d same-song groups; %.1f s" % (
            stats["rows"], stats["tracks"], stats["unindexed"], stats["genres"], dim, len(p.groups),
            time.time() - t0))
    t1 = time.time()
    djsl = block_lists(p, info, log)
    stats["listSeconds"] = round(time.time() - t1, 1)
    data = mpdj.write(p, djsl)
    why = mpdj.check(data, "all")
    if why != "ok":
        raise AssertionError("the synthetic index reads as absent: " + why)
    stats["sameSongGroups"] = len(p.groups)
    stats["seconds"] = round(time.time() - t0, 1)
    return data, stats


# ---------------------------------------------------------------------------
# A given index (--mpdj)
# ---------------------------------------------------------------------------
def restamp_header(data, generation, card_id, tags_generation):
    """The same file as this commit's: generation and cardId (the common
    header) and tagsGeneration (the type header) rewritten, the header CRC
    made again; every section unchanged."""
    b = bytearray(data)
    struct.pack_into("<I", b, 20, generation)
    struct.pack_into("<Q", b, 24, card_id)
    struct.pack_into("<I", b, 60, tags_generation)
    hb, n = struct.unpack_from("<II", b, 8)
    struct.pack_into("<I", b, 32, 0)
    struct.pack_into("<I", b, 32, mpdj.crc32(bytes(b[:hb + 32 * n])))
    return bytes(b)


def given_index(path, tracks, generation, card_id, allow_missing=False):
    """(the restamped file's bytes, stats). `tracks`: the card's tracks as
    walk_music() lists them (a path to any other file maps to no track on
    the device)."""
    try:
        with open(path, "rb") as fh:
            old = fh.read()
    except OSError as e:
        raise Refused("--mpdj %s: %s" % (path, e))
    r = mpdj.Reader(old)
    why = r.check("all")
    if why != "ok":
        raise Refused("--mpdj %s: absent to a reader (%s)" % (path, why))
    on_card = {mpdj.path_hash(rel) for rel in tracks}
    hashes = [h for h, _ in r.paths()]
    missing = sum(1 for h in hashes if h not in on_card)
    if hashes and missing == len(hashes):
        raise Refused("--mpdj %s: none of its %d paths is a track in this card's /music (an index for another "
                      "tree)" % (path, len(hashes)))
    if missing and not allow_missing:
        raise Refused("--mpdj %s: %d of its %d paths aren't tracks the device lists in this card's /music "
                      "(--allow-missing to write it anyway)" % (path, missing, len(hashes)))
    data = restamp_header(old, generation, card_id, generation)
    if mpdj.check(data, "all") != "ok" or mpdj.same_content(old, data) is not None:
        raise AssertionError("the restamped index differs from the given one")
    bpm = sum(1 for i in range(r.rows) if r.filter(i)[0])
    key = sum(1 for i in range(r.rows) if r.filter(i)[1])
    return data, {"rows": r.rows, "paths": r.path_count, "missing": missing, "bpm": bpm, "key": key,
                  "from": {"generation": r.generation, "cardId": r.card_id, "sig": r.sig.hex()}}


# ---------------------------------------------------------------------------
# The commit (METADATA 2.5, 2.6, 2.12.1)
# ---------------------------------------------------------------------------
def tags_file(generation, card_id):
    """T: an MPTG of source 2 with no records (FOLD holds /music alone)."""
    return cf.mptg({"source": 2, "generation": generation, "cardId": card_id, "parserVersion": 0,
                    "readRules": 1, "producer": PRODUCER, "records": []})


def root_file(generation, card_id, base, commit_time, tags, autodj):
    """The root naming T and the index (no LIBR: /music is the only root)."""
    rows = [cf.comp_row(cf.companion_name("MPTG", generation), tags)]
    if autodj is not None:
        rows.append(cf.comp_row(cf.companion_name("MPDJ", struct.unpack_from("<I", autodj, 20)[0]), autodj))
    h = hashlib.sha256(b"synthtransfer commit\0" + struct.pack("<IQI", generation, card_id, commit_time) + tags +
                       (autodj or b""))
    desc = {"generation": generation, "cardId": card_id, "commitTime": commit_time, "flags": FINAL,
            "commitId": int.from_bytes(h.digest()[:8], "little"), "producer": PRODUCER, "serverRevision": "",
            "baseGeneration": base, "serverUrlKey": 0}
    return cf.msmf_raw(desc, rows, [])


def default_card_id(seed):
    return int.from_bytes(hashlib.sha256(("synthtransfer card %s" % seed).encode()).digest()[:8], "little")


# ---------------------------------------------------------------------------
# Reading it back: a Python mirror of the boot's checks (cardroot::read,
# then Auto DJ's open: mpdj::Table::open and msmf::companionMatches)
# ---------------------------------------------------------------------------
COMMON = struct.Struct("<4sHHIIIIQII")
DIR_ENTRY = struct.Struct("<4sIIIIIII")


def open_frame(b, magic, major, min_header):
    """2.4.3's frame and directory, and every section's CRC: (why, frame)."""
    if len(b) < 40:
        return "short", None
    m, mj, mn, hb, n, fb, gen, card, hcrc, _ = COMMON.unpack_from(b, 0)
    if m != magic:
        return "magic", None
    if mj != major:
        return "major", None
    if hb % 8 or hb < min_header:
        return "headerBytes", None
    if n > 64:
        return "sectionCount", None
    end = hb + 32 * n
    if end > len(b):
        return "short", None
    if fb != len(b):
        return "fileBytes", None
    if mpdj.crc32(b[:32] + bytes(4) + b[36:end]) != hcrc:
        return "headerCrc", None
    secs = {}
    for i in range(n):
        t, flags, off, nb, count, stride, crc, _ = DIR_ENTRY.unpack_from(b, hb + 32 * i)
        if off % 8 or off < end or off - end >= 8 or off + nb > fb or t in secs:
            return "layout", None
        end = off + nb
        if (count * stride != nb) if stride else (count != 0):
            return "shape", None
        if mpdj.crc32(b[off:off + nb]) != crc:
            return "sectionCrc", None
        secs[t] = (off, nb, count, stride)
    if end != fb:
        return "layout", None
    return "ok", {"generation": gen, "cardId": card, "headerCrc": hcrc, "fileBytes": fb, "headerBytes": hb,
                  "sections": secs}


def cstring(strs, off):
    if off == 0:
        return ""
    if off >= len(strs):
        return None
    nul = strs.find(b"\0", off)
    return None if nul < 0 else strs[off:nul].decode("utf-8", "replace")


def check_root(b):
    """A root checked whole (2.4.3, 2.5): (why, root)."""
    why, f = open_frame(b, b"MSMF", 1, 96)
    if why != "ok":
        return why, None
    secs = f["sections"]
    if b"COMP" not in secs or b"STRS" not in secs:
        return "missing", None
    off, nb, count, stride = secs[b"COMP"]
    so, sn, _, _ = secs[b"STRS"]
    strs = b[so:so + sn]
    if stride < 40 or not strs or strs[0] != 0:
        return "shape", None
    comps, order = [], []
    for i in range(count):
        kind, gen, fb, hc, name, _, key = struct.unpack_from("<4sIIIII16s", b, off + i * stride)
        nm = cstring(strs, name)
        if nm is None:
            return "string", None
        order.append(name)
        if kind not in (b"MPTG", b"MPDJ"):
            continue
        want = cf.companion_name(kind.decode(), gen)
        if nm != want:
            return "compName", None
        comps.append({"kind": kind.decode(), "generation": gen, "fileBytes": fb, "headerCrc": hc, "name": nm,
                      "key": key})
    kinds = [c["kind"] for c in comps]
    if kinds not in (["MPTG"], ["MPTG", "MPDJ"]):
        return "comp", None
    producer, revision = struct.unpack_from("<II", b, 72)
    offs = [o for o in [producer, revision] + order if o]
    if offs != sorted(offs) or len(set(offs)) != len(offs):
        return "stringOrder", None
    commit_time, flags, commit_id = struct.unpack_from("<IIQ", b, 40)
    base = struct.unpack_from("<I", b, 80)[0]
    return "ok", dict(f, comps=comps, commitTime=commit_time, flags=flags, commitId=commit_id,
                      baseGeneration=base, producer=cstring(strs, producer), roots=secs.get(b"LIBR", (0, 0, 0, 0))[2])


def elect(roots):
    """2.5.4: {name: (why, root)} for manifest.bin and .tmp -> (name, root)."""
    ok = [(name, r) for name, (why, r) in sorted(roots.items()) if why == "ok"]
    if not ok:
        return None, None
    ok.sort(key=lambda nr: (-nr[1]["generation"], nr[0] != "manifest.bin"))
    return ok[0]


def check_commit(read):
    """What the boot finds (read(name) -> bytes or None, a /.mstream name):
    {"present", "why", "root", "tags", "autoDj": {...}}."""
    roots = {}
    for name in ("manifest.bin", "manifest.tmp"):
        b = read(name)
        roots[name] = ("missing", None) if b is None else check_root(b)
    name, root = elect(roots)
    out = {"present": False, "roots": {n: w for n, (w, _) in roots.items()}, "autoDj": {"present": False}}
    if root is None:
        out["why"] = roots["manifest.bin"][0] if roots["manifest.bin"][0] != "missing" else roots["manifest.tmp"][0]
        return out
    out["rootName"], out["root"] = name, root
    t = root["comps"][0]
    tb = read(t["name"])
    if tb is None:
        out["why"] = "missing " + t["name"]
        return out
    why, tf = open_frame(tb, b"MPTG", 1, 72)
    if why != "ok":
        out["why"] = "%s: %s" % (t["name"], why)
        return out
    source, = struct.unpack_from("<B", tb, 40)
    records, folders = struct.unpack_from("<II", tb, 48)
    secs = tf["sections"]
    if (source not in (2, 3) or b"FOLD" not in secs or b"RECS" not in secs or b"STRS" not in secs
            or secs[b"RECS"][2] != records or secs[b"FOLD"][2] != folders or folders < 1):
        out["why"] = "%s: header" % t["name"]
        return out
    if (tf["generation"], tf["fileBytes"], tf["headerCrc"]) != (t["generation"], t["fileBytes"], t["headerCrc"]):
        out["why"] = "%s: not the one COMP names" % t["name"]
        return out
    out.update(present=True, why="ok", tags={"name": t["name"], "records": records, "source": source})
    if len(root["comps"]) < 2:
        return out
    d = root["comps"][1]
    dj = {"present": False, "name": d["name"]}
    out["autoDj"] = dj
    db = read(d["name"])
    if db is None:
        dj["why"] = "missing"
        return out
    r = mpdj.Reader(db)
    why = r.check("all")
    if why != "ok":
        dj["why"] = why
        return out
    if (r.generation, len(db), r.header_crc, r.sig) != (d["generation"], d["fileBytes"], d["headerCrc"], d["key"]):
        dj["why"] = "not the one COMP names"
        return out
    dj.update(present=True, why="ok", rows=r.rows, paths=r.path_count, k=r.k, selection=[r.kind, r.head, r.floor],
              generation=r.generation, cardId=r.card_id, sig=r.sig.hex(), bytes=len(db),
              bpm=sum(1 for i in range(r.rows) if r.filter(i)[0]),
              key=sum(1 for i in range(r.rows) if r.filter(i)[1]), reader=r)
    return out


def check_card(card, tracks=None):
    """check_commit over CARD/.mstream, and how many of the card's tracks map
    to a row."""
    ms = os.path.join(card, MSTREAM)

    def read(name):
        p = os.path.join(ms, name)
        if not file_exists(p):
            return None
        with open(synthcard.long_path(p), "rb") as fh:
            return fh.read()
    out = check_commit(read)
    dj = out["autoDj"]
    if dj.get("present") and (tracks is not None or os.path.isdir(os.path.join(card, "music"))):
        if tracks is None:
            tracks, _ = walk_music(card)
        rows = dict(dj["reader"].paths())
        mapped = [rows.get(mpdj.path_hash(rel)) for rel in tracks]
        dj["tracks"] = len(tracks)
        dj["mapped"] = sum(1 for x in mapped if x is not None)
        dj["rowsMapped"] = len({x for x in mapped if x is not None})
    dj.pop("reader", None)
    return out


def describe_check(c, w):
    if not c["present"]:
        w("no transfer data: %s (manifest.bin %s, manifest.tmp %s)\n" % (
            c.get("why"), c["roots"]["manifest.bin"], c["roots"]["manifest.tmp"]))
        return
    r = c["root"]
    w("root %s: generation %d, cardId 0x%016X, commitId 0x%016X, base %d, %s, producer %r\n" % (
        c["rootName"], r["generation"], r["cardId"], r["commitId"], r["baseGeneration"],
        "FINAL" if r["flags"] & FINAL else "a checkpoint", r["producer"]))
    w("T %s: source %d, %d records\n" % (c["tags"]["name"], c["tags"]["source"], c["tags"]["records"]))
    dj = c["autoDj"]
    if not dj.get("name"):
        w("AutoDJ index: none named\n")
    elif not dj["present"]:
        w("AutoDJ index %s: absent (%s)\n" % (dj["name"], dj["why"]))
    else:
        w("AutoDJ index %s: present, %d bytes, %d rows, %d paths, k %d, selection %s, selectionSig %s; BPM on "
          "%.1f %%, key on %.1f %% of rows\n" % (
              dj["name"], dj["bytes"], dj["rows"], dj["paths"], dj["k"], dj["selection"], dj["sig"],
              100.0 * dj["bpm"] / dj["rows"], 100.0 * dj["key"] / dj["rows"]))
        if "tracks" in dj:
            w("tracks on the card: %d, %d with a row (%d rows), %d without\n" % (
                dj["tracks"], dj["mapped"], dj["rowsMapped"], dj["tracks"] - dj["mapped"]))


# ---------------------------------------------------------------------------
# --add-to
# ---------------------------------------------------------------------------
def foreign(old_root, contract, entries=()):
    """Why --replace would replace what another program wrote (None: this
    tool's own /.mstream, or no contract file in it at all). A run.lock
    (METADATA 2.12.7) says a transfer is writing the card, or was cut."""
    if any(n.lower() == "run.lock" for n in entries):
        return "a run.lock (a transfer may be writing this card, or one was cut short)"
    if not contract:
        return None
    if old_root is None:
        return "contract files (%s) and no root that reads" % ", ".join(contract[:4])
    if not (old_root["producer"] or "").startswith(PRODUCER.rsplit(" ", 1)[0]):
        return "a root written by %r, not this tool" % old_root["producer"]
    return None


def mstream_state(card):
    """What /.mstream holds: (entries, contract names, the root that counts
    or None, the highest generation any name or root shows)."""
    ms = os.path.join(card, MSTREAM)
    if not os.path.isdir(ms):
        if os.path.exists(ms):
            raise Refused("%s exists and isn't a folder" % ms)
        return [], [], None, 0
    entries = sorted(os.listdir(ms))
    contract = [n for n in entries if CONTRACT.match(n) and os.path.isfile(os.path.join(ms, n))]
    roots = {}
    for name in ("manifest.bin", "manifest.tmp"):
        p = os.path.join(ms, name)
        if os.path.isfile(p):
            with open(p, "rb") as fh:
                roots[name] = check_root(fh.read())
    _, root = elect(roots)
    top = max([int(m.group(2), 16) for m in (GENERATION_NAME.match(n) for n in contract) if m] +
              [r["generation"] for w, r in roots.values() if w == "ok"] + [0])
    return entries, contract, root, top


def check_drive(root, probe, w):
    """A drive's root: never the system drive; FAT32 (or FAT16), the file
    systems the firmware reads (README). It says what the drive is."""
    if root.upper() == probe.system_drive().upper():
        raise Refused("%s: that is the system drive: refusing" % root)
    info = probe.info(root)
    w("drive %s: label \"%s\", %s, %s, %.2f GB free\n" % (root, info["label"], info["type"],
                                                         info["fs"] or "no file system read", info["free"] / 1e9))
    if info["fs"].upper() not in ("FAT32", "FAT"):
        raise Refused("%s: %s, not FAT32 (the firmware reads FAT32 and FAT16 only): refusing" % (
            root, info["fs"] or "no file system"))


def write_file(path, data):
    """A new file, written whole and synced; never over an existing one."""
    with open(synthcard.long_path(path), "xb") as fh:
        fh.write(data)
        fh.flush()
        os.fsync(fh.fileno())


def add_to(card, synthetic=False, mpdj_file=None, tags_path=None, seed=1, generation=None, card_id=None,
           commit_time=None, replace=False, dry_run=False, allow_missing=False, k=mpdj.DEFAULT_K,
           head=mpdj.DEFAULT_HEAD, floor=mpdj.DEFAULT_FLOOR, genres=16, unindexed=1.0, probe=None,
           out=None, ops=None, replace_foreign=False):
    """Writes CARD's /.mstream (or, dry_run, says what it would write).
    Returns a summary dict. `ops`, when given, gets each step in order
    (("write", name), ("remove", name), ("rename", from, to)): the tests'
    view of the commit order."""
    out = out or sys.stdout
    w = out.write
    t_start = time.time()
    if synthetic == (mpdj_file is not None):
        raise Refused("one of --synthetic and --mpdj FILE")
    card = card_root(card)
    if not os.path.isdir(card):
        raise Refused("%s: not a folder" % card)
    if is_drive_root(card):
        check_drive(card, probe or synthcard.DriveProbe(), w)
    if not os.path.isdir(os.path.join(card, "music")):
        raise Refused("%s: no music folder: not a card tree" % card)
    line = synthcard_line(card)
    w("card %s: %s\n" % (card, line or "no SYNTHCARD.TXT (not a synthcard tree: any card root works)"))

    entries, contract, old_root, top = mstream_state(card)
    if entries and not replace:
        shown = ", ".join(entries[:5]) + (" and %d more" % (len(entries) - 5) if len(entries) > 5 else "")
        raise Refused("%s\\%s isn't empty (%s): refusing (--replace commits a new generation over it)" % (
            card, MSTREAM, shown))
    other = foreign(old_root, contract, entries)
    if other and not replace_foreign:
        raise Refused("%s\\%s holds %s: --replace would delete another program's files (a transfer's tags, "
                      "index and plan): refusing (--replace-foreign replaces them anyway)" % (card, MSTREAM, other))
    if generation is None:
        generation = top + 1
    elif generation <= 0 or generation > 0xFFFFFFFF:
        raise Refused("--generation: 1 to 0xFFFFFFFF")
    tags_name = cf.companion_name("MPTG", generation)
    dj_name = cf.companion_name("MPDJ", generation)
    clash = [n for n in (tags_name, dj_name) if n.lower() in {c.lower() for c in contract}]
    if clash:
        raise Refused("--generation %d: %s already there (a name is never reused, METADATA 2.5.2)" % (
            generation, ", ".join(clash)))
    if old_root is not None and generation <= old_root["generation"]:
        raise Refused("--generation %d: the card's root is generation %d already" % (
            generation, old_root["generation"]))
    if card_id is None:
        card_id = old_root["cardId"] if old_root is not None else default_card_id(seed)
    base = old_root["generation"] if old_root is not None else 0
    commit_time = int(time.time()) if commit_time is None else commit_time

    t0 = time.time()
    tracks, files = walk_music(card)
    w("music: %d files, %d tracks (.mp3, .flac, .opus the device lists) in %.1f s\n" % (
        len(files), len(tracks), time.time() - t0))
    if not tracks:
        raise Refused("%s: no tracks under music" % card)

    def log(s):
        w(s + "\n")
        if hasattr(out, "flush"):
            out.flush()
    if synthetic:
        tp = tags_path or os.path.join(card, TAGS_JSONL)
        tags = read_tags(tp) if os.path.isfile(tp) else {}
        w("tags: %s\n" % ("%d files from %s" % (len(tags), tp) if tags else
                          "no %s: each track's top folder as its artist, no titles" % TAGS_JSONL))
        autodj, stats = synthetic_index(tracks, tags, seed, generation, card_id, commit_time, k, head, floor,
                                        genres, unindexed, log)
        if tags and stats["tagged"] < stats["rows"]:
            w("warning: %d of the %d rows' tracks aren't in %s (another tree's?): their top folder is their "
              "artist\n" % (stats["rows"] - stats["tagged"], stats["rows"], tp))
        w("index: %d rows, BPM on %.1f %%, key on %.1f %%, %d tracks' tags from the jsonl, %d same-song groups; "
          "lists in %.1f s, %.1f s in all\n" % (
              stats["rows"], 100.0 * stats["bpm"] / stats["rows"], 100.0 * stats["key"] / stats["rows"],
              stats["tagged"], stats["sameSongGroups"], stats["listSeconds"], stats["seconds"]))
    else:
        autodj, stats = given_index(mpdj_file, tracks, generation, card_id, allow_missing)
        w("index: %s, %d rows, %d paths (%d not tracks on the card), from generation %d cardId 0x%016X, "
          "restamped\n" % (
            mpdj_file, stats["rows"], stats["paths"], stats["missing"], stats["from"]["generation"],
            stats["from"]["cardId"]))
    tags = tags_file(generation, card_id)
    root = root_file(generation, card_id, base, commit_time, tags, autodj)

    # Checked as the boot reads it, before anything is written.
    mem = {tags_name: tags, dj_name: autodj, "manifest.bin": root}
    c = check_commit(mem.get)
    if not (c["present"] and c["autoDj"].get("present")):
        raise AssertionError("the commit doesn't read back: %r" % {k_: v for k_, v in c.items() if k_ != "root"})
    # The steps, in the commit order (2.12.1; AUTODJ-INDEX.md 7.2): the
    # companions under their final names, then the root through
    # manifest.tmp (the old root deleted just before the rename), then the
    # contract files the new root doesn't name.
    ms = os.path.join(card, MSTREAM)
    lower = {n.lower(): n for n in contract}
    steps = []
    if "manifest.tmp" in lower:
        steps.append(("remove", lower["manifest.tmp"]))  # a cut run's (2.12.1, step 4)
    steps += [("write", tags_name), ("write", dj_name), ("write", "manifest.tmp")]
    if "manifest.bin" in lower:
        steps.append(("remove", lower["manifest.bin"]))
    steps.append(("rename", "manifest.tmp", "manifest.bin"))
    steps += [("remove", n) for n in contract if n.lower() not in ("manifest.bin", "manifest.tmp")]
    data = {tags_name: tags, dj_name: autodj, "manifest.tmp": root}
    hc = lambda b: struct.unpack_from("<I", b, 32)[0]  # noqa: E731
    w("commit: generation %d (base %d), cardId 0x%016X, commitTime %d\n" % (generation, base, card_id, commit_time))
    w("  %s  %10d bytes  headerCrc 0x%08X  (MPTG source 2, no records)\n" % (tags_name, len(tags), hc(tags)))
    w("  %s  %10d bytes  headerCrc 0x%08X  selectionSig %s\n" % (dj_name, len(autodj), hc(autodj),
                                                                autodj[64:80].hex()))
    w("  manifest.bin  %10d bytes  headerCrc 0x%08X  commitId 0x%016X (written last, through manifest.tmp)\n" % (
        len(root), hc(root), c["root"]["commitId"]))
    summary = {"generation": generation, "cardId": card_id, "stats": stats, "steps": steps,
               "bytes": {tags_name: len(tags), dj_name: len(autodj), "manifest.bin": len(root)}}
    if dry_run:
        w("dry run: would, in %s:\n" % ms)
        for s in steps:
            w("  %s %s\n" % (s[0], s[1] if s[0] != "rename" else "%s -> %s" % s[1:]))
        kept = [n for n in entries if n not in contract]
        if kept:
            w("  and keep %s\n" % ", ".join(kept))
        w("nothing written\n")
        return dict(summary, written=False)
    need = len(tags) + len(autodj) + len(root) + 3 * 65536
    try:
        free = shutil.disk_usage(card).free
    except OSError:
        free = None
    if free is not None and free < need:
        raise Refused("%s: %.1f MB free, %.1f MB needed" % (card, free / 1e6, need / 1e6))

    os.makedirs(synthcard.long_path(ms), exist_ok=True)
    for s in steps:
        if ops is not None:
            ops.append(s)
        if s[0] == "write":
            write_file(os.path.join(ms, s[1]), data[s[1]])
        elif s[0] == "remove":
            os.remove(synthcard.long_path(os.path.join(ms, s[1])))
        else:
            os.rename(synthcard.long_path(os.path.join(ms, s[1])), synthcard.long_path(os.path.join(ms, s[2])))

    back = check_card(card, tracks)
    describe_check(back, w)
    if not (back["present"] and back["autoDj"].get("present")):
        raise AssertionError("the card doesn't read back what was written")
    w("done in %.1f s; the first boot rebuilds the library (a new commit). Eject the card before taking it out.\n"
      % (time.time() - t_start))
    return dict(summary, written=True, check=back)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--add-to", metavar="CARD", help="the card's root (E:\\) or a card tree's folder")
    ap.add_argument("--check", metavar="CARD", help="read CARD's /.mstream back as the boot does; write nothing")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--synthetic", action="store_true", help="build the index here from synthesized vectors")
    g.add_argument("--mpdj", metavar="FILE", help="an index built for this tree, restamped to the commit")
    ap.add_argument("--tags", metavar="JSONL", help="synthcard-files.jsonl for the record values (default: "
                    "CARD's own, when there)")
    ap.add_argument("--seed", type=int, default=1, help="--synthetic's seed (default 1); also the new card's id")
    ap.add_argument("--generation", type=lambda s: int(s, 0), help="the commit's generation (default: one more "
                    "than any on the card, else 1)")
    ap.add_argument("--card-id", type=lambda s: int(s, 0), help="the card's id (default: the root's, else from "
                    "the seed)")
    ap.add_argument("--time", type=int, help="commitTime and builtTime, Unix seconds (default: now)")
    ap.add_argument("--replace", action="store_true", help="write over a /.mstream that isn't empty (a new "
                    "generation; the contract files the new root doesn't name deleted after it)")
    ap.add_argument("--replace-foreign", action="store_true", help="with --replace: also over a /.mstream another "
                    "program wrote (a transfer's: its tags file, index and plan deleted)")
    ap.add_argument("--dry-run", action="store_true", help="build and check in memory, say what it would write")
    ap.add_argument("--allow-missing", action="store_true", help="--mpdj: accept paths that aren't on the card")
    ap.add_argument("--genres", type=int, default=16, help="--synthetic: genres (default 16)")
    ap.add_argument("--unindexed", type=float, default=1.0, help="--synthetic: percent of tracks left without a "
                    "row (default 1)")
    ap.add_argument("--k", type=int, default=mpdj.DEFAULT_K, help="--synthetic: list length (default 169)")
    ap.add_argument("--head", type=int, default=mpdj.DEFAULT_HEAD, help="--synthetic: selHead (default 84)")
    ap.add_argument("--floor", type=int, default=mpdj.DEFAULT_FLOOR, help="--synthetic: selFloor (default 140)")
    a = ap.parse_args(argv)
    try:
        if a.check:
            card = card_root(a.check)
            c = check_card(card)
            describe_check(c, sys.stdout.write)
            return 0 if c["present"] and c["autoDj"].get("present") else 1
        if not a.add_to:
            ap.error("--add-to CARD or --check CARD")
        if not (a.synthetic or a.mpdj):
            ap.error("--add-to needs --synthetic or --mpdj FILE")
        if a.genres < 1 or a.genres > 200:
            ap.error("--genres: 1 to 200")
        if a.replace_foreign and not a.replace:
            ap.error("--replace-foreign goes with --replace")
        add_to(a.add_to, a.synthetic, a.mpdj, a.tags, a.seed, a.generation, a.card_id, a.time, a.replace,
               a.dry_run, a.allow_missing, a.k, a.head, a.floor, a.genres, a.unindexed,
               replace_foreign=a.replace_foreign)
    except Refused as e:
        print("refused: %s" % e, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
