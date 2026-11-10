# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Tests for tools/synthtransfer.py (a test card's /.mstream: a tags file
with no records, an AutoDJ index, with --waves a waveform index, the root
written last).

    python -m unittest discover -s tools -p "test_synthtransfer.py"

- The lists: synthtransfer's per-genre computation against tools/mpdj.py's
  rule as written (mpdj.lists_rule over the whole vectors), byte for byte:
  strided lists (a small k), top-k lists filled with level-0 rows in row
  order (a k past the genres' sizes), the same song's rows left out.
- Determinism, the BPM/key priors' coverage, the Camelot codes.
- --add-to on a small synthcard tree: the commit order (the companions,
  then the root through manifest.tmp), the Python mirror (--check),
  mpdj.py's check, every DJPH path a track of the tree; and the firmware's
  own readers on the host (cardroot::read, mpdj::Table::open and check,
  msmf::companionMatches, Table::rowOf for every track: a probe built from
  lib/core with g++ when there is one).
- --mpdj: the restamp equals tools/mpdj.py restamp's file for the same
  description; an index for another tree, or a broken one, refused.
- --waves: the shapes keyed by path and seed, their bars the rule's of their
  peaks, a seeded share without a slot, lengths from the jsonl; the file
  written after the index and before the root, named last in COMP, read back
  by the Python mirror and by the firmware's readers (mpwf::Table::open and
  check, msmf::companionMatches, Table::find for every track, with the same
  reads); a bad file costs only itself, and the mirror takes the root's row as
  the shared fixtures say.
- --moods: one record a track at stride 80, minor 1, each the device's own
  for the file (its size, its FAT time, its qfp of the whole file by
  card_fixtures.qfp) so rule 1 takes it; its names the jsonl's with FROM_API
  and their `known` bits, its picture anchor the jsonl's; its tail
  moodfilter's of the seeded scores, which validates, and its word
  moodfilter's; --mood-coverage 0 (stride 72, no tails, minor 0),
  --mood-null, --seed; the default T byte for byte as before; --check's
  rule 1 (a changed time confirmed by the qfp, a changed size stale) and
  --mood-filter's tracks and albums against a recount of T's own bytes; the
  firmware's cardroot::read taking the root and T, and its build of the
  library from T (LibraryBuilder, the boot's after a commit): each track's
  word in the index moodfilter's of its tail, gm's figures and gm<q>'s
  tracks and albums (lib/core/TagText) --check's and --check
  --mood-filter's, the control card's records taken with no block;
  --mood-filter's Q in bare hex as the device's gm<hex>, or with 0x.
- --check's words and --mood-filter's tracks and albums on a hand-made card
  (T written record by record), expected by hand from the rules: a track
  four folders deep in its depth-2 album, an artist's loose tracks and the
  root's, an album with no server row (never fits, not analysed), a record
  UNREADABLE without FROM_API (no word, its tail aside) and one with it;
  the firmware's own build of that card and gm<q>'s passes the same.
- Names as stored (METADATA 2.8.5): on a host that lists NFD, T's record
  paths, the path hashes and the jsonl's lookup are NFC while each file is
  opened by its listed name; elsewhere names are taken as listed.
- The refusals and --replace: a /.mstream that isn't empty, no music
  folder, the system drive, a drive that isn't FAT32; --replace commits the
  next generation with the card's id and removes only contract files the
  new root doesn't name, a subset of what a contract run's clean-up
  deletes (card_fixtures.cleanup), keeping the private files and the
  partials (METADATA 2.12.7, 2.12.8); --replace refuses another program's
  /.mstream (a transfer's), or a run.lock, without --replace-foreign;
  nothing outside /.mstream is touched; --dry-run writes nothing.
"""
import contextlib
import hashlib
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unicodedata
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import card_fixtures as cf  # noqa: E402  (cleanup, private_vectors: docs/METADATA.md 2.12.7)
import moodfilter as mf  # noqa: E402
import mpdj  # noqa: E402
import mpwf  # noqa: E402
import synthcard  # noqa: E402
import synthtransfer as st  # noqa: E402
from test_synthcard import include_closure  # noqa: E402  (the lib/core sources a probe needs)

ROOT = Path(__file__).resolve().parent.parent
CORE = ROOT / "lib" / "core"
FIXTURES = ROOT / "test" / "fixtures" / "card"
TIME = 1791900000


def fake_tracks(artists=12, albums=2, per_album=6):
    """Made-up paths and tags: a few artists' albums, one song on two of an
    artist's albums (a same-song group) and a track with no title."""
    tracks, tags = [], {}
    for a in range(artists):
        for b in range(albums):
            for t in range(per_album):
                rel = "Artist %02d/Album %d/%02d - Song %d.mp3" % (a, b, t + 1, t)
                tracks.append(rel)
                title = "Song %d" % t if (a, t) != (3, 2) else None
                if a % 4 == 0 and t == 0:
                    title = "The Same Song"
                tags[rel] = (title, ["Artist %02d" % a])
    return sorted(tracks, key=lambda r: r.encode("utf-8")), tags


def prepared(tracks, tags, seed, genres, k, head, floor):
    rows, info, dim, _ = st.synthetic_rows(tracks, tags, seed, genres, unindexed=0.0)
    desc = dict(st.STRINGS, format="MPDJ", generation=3, cardId=7, builtTime=TIME, tagsGeneration=3, k=k,
                selKind=mpdj.SEL_STRIDED, selHead=head, selFloor=floor, dim=dim, vectors="v.f32", rows=rows)
    return mpdj.prepare(desc, sidecars={"v.f32": st.vectors_bytes(rows, info, dim)}), info


class ListsTests(unittest.TestCase):
    def rule_and_ours(self, genres, k, head, floor, seed=4):
        tracks, tags = fake_tracks()
        p, info = prepared(tracks, tags, seed, genres, k, head, floor)
        ours = st.block_lists(p, info)
        rule = mpdj.compute_lists(p, fast=False)
        return p, ours, rule

    def test_strided_lists_equal_the_rule(self):
        p, ours, rule = self.rule_and_ours(genres=2, k=8, head=3, floor=100)
        self.assertEqual(ours, rule)
        # Strided lists happened: some row has more than k candidates at the floor.
        lv = mpdj.level_matrix(p.emb)
        ids = mpdj.song_ids(p.keys)
        m = [sum(1 for b in range(len(lv)) if b != a and ids[b] != ids[a] and lv[a][b] >= p.floor)
             for a in range(len(lv))]
        self.assertTrue(any(x > p.k for x in m))
        self.assertTrue(p.groups, "a same-song group in the rows")

    def test_level_zero_fill_equals_the_rule(self):
        p, ours, rule = self.rule_and_ours(genres=6, k=60, head=20, floor=150)
        self.assertEqual(ours, rule)
        r = mpdj.Reader(mpdj.write(p, ours))
        self.assertEqual(r.check("all"), "ok")
        zero = [e for i in range(r.rows) for e in r.slot(i)[2] if e[1] == 0]
        self.assertTrue(zero, "level-0 rows fill the lists")

    def test_default_selection_equals_the_rule(self):
        p, ours, rule = self.rule_and_ours(genres=16, k=mpdj.DEFAULT_K, head=mpdj.DEFAULT_HEAD,
                                           floor=mpdj.DEFAULT_FLOOR, seed=9)
        self.assertEqual(ours, rule)

    def test_same_song_rows_left_out(self):
        p, ours, _ = self.rule_and_ours(genres=1, k=8, head=3, floor=100)
        r = mpdj.Reader(mpdj.write(p, ours))
        for g in p.groups:
            for a in g:
                listed = {e[0] for e in r.slot(a)[2]}
                self.assertFalse(listed & (set(g) - {a}), "row %d lists its own song" % a)

    def test_deterministic(self):
        tracks, tags = fake_tracks()
        a = st.synthetic_index(tracks, tags, 5, 2, 9, TIME, k=12, head=4, floor=120, genres=3, unindexed=0)[0]
        b = st.synthetic_index(tracks, tags, 5, 2, 9, TIME, k=12, head=4, floor=120, genres=3, unindexed=0)[0]
        c = st.synthetic_index(tracks, tags, 6, 2, 9, TIME, k=12, head=4, floor=120, genres=3, unindexed=0)[0]
        self.assertEqual(a, b)
        self.assertNotEqual(mpdj.Reader(a).sig, mpdj.Reader(c).sig)
        self.assertEqual(mpdj.Reader(a).rows, mpdj.Reader(c).rows)

    def test_priors(self):
        tracks = ["Artist %03d/Album/%03d.flac" % (i % 150, i) for i in range(3000)]
        rows, info, dim, s = st.synthetic_rows(tracks, {}, 1, genres=16, unindexed=1.0)
        self.assertEqual(dim, 16 * st.BLOCK)
        self.assertTrue(0.3 <= 100.0 * s["unindexed"] / s["tracks"] <= 2.0, s)
        self.assertTrue(0.92 <= s["bpm"] / s["rows"] <= 0.98, s)
        self.assertTrue(0.92 <= s["key"] / s["rows"] <= 0.98, s)
        for row in rows:
            f = row["files"][0]
            self.assertTrue(f["bpm10"] == 0 or 500 <= f["bpm10"] <= 2200, f)
            self.assertTrue(0 <= f["camelot"] <= 24, f)
            self.assertEqual(f["bpmAnalysed"], f["bpm10"] > 0)
            self.assertEqual(f["artists"], [f["path"].split("/")[0]])
            self.assertNotIn("title", f)
            g, vec = info[row["hash"]]
            self.assertTrue(0 <= g < 16 and len(vec) == st.BLOCK)
            self.assertAlmostEqual(sum(x * x for x in vec), 1.0, places=5)

    def test_camelot(self):
        self.assertEqual(st.camelot(0, False), 20)   # C major, 8B
        self.assertEqual(st.camelot(9, True), 8)     # A minor, 8A
        self.assertEqual(st.camelot(7, False), 21)   # G major, 9B
        self.assertEqual(st.camelot(4, True), 9)     # E minor, 9A
        self.assertEqual(st.camelot(2, True), 7)     # D minor, 7A
        self.assertEqual(st.camelot(11, False), 13)  # B major, 1B
        self.assertEqual(sorted(st.camelot(pc, m) for pc in range(12) for m in (False, True)), list(range(1, 25)))


def track_peaks(seed, rel):
    """The peaks synthetic_waves makes for a track, its first draw (whether it
    has a slot) spent."""
    r = st.Rng(seed, "track-shape", rel)
    r.u()
    return st.shape_peaks(r)


class WavesTests(unittest.TestCase):
    def test_deterministic_and_keyed_by_path(self):
        tracks, _ = fake_tracks()
        a = st.synthetic_waves(tracks, {}, 5, 2, 9, unshaped=0.0)[0]
        self.assertEqual(a, st.synthetic_waves(tracks, {}, 5, 2, 9, unshaped=0.0)[0])
        self.assertNotEqual(a, st.synthetic_waves(tracks, {}, 6, 2, 9, unshaped=0.0)[0])
        # A track's shape is its own, whatever else the tree holds.
        part = st.synthetic_waves(tracks[:10], {}, 5, 2, 9, unshaped=0.0)[0]
        whole, some = mpwf.Reader(a), mpwf.Reader(part)
        for rel in tracks[:10]:
            self.assertEqual(some.find(mpwf.path_hash(rel)).slot, whole.find(mpwf.path_hash(rel)).slot)

    def test_slots(self):
        tracks = ["Artist %03d/Album/%03d.flac" % (i % 150, i) for i in range(3000)]
        lengths = {rel: 1000 + i for i, rel in enumerate(tracks[:100])}
        data, s = st.synthetic_waves(tracks, lengths, 1, 3, 9)
        self.assertEqual(mpwf.check(data, "all"), "ok")
        r = mpwf.Reader(data)
        self.assertEqual((r.generation, r.card_id, r.rows), (3, 9, s["rows"]))
        self.assertEqual(s["rows"], len(tracks) - s["unshaped"])
        self.assertTrue(0.3 <= 100.0 * s["unshaped"] / s["tracks"] <= 2.0, s)
        hits = 0
        for rel in tracks:
            got = r.find(mpwf.path_hash(rel))
            hits += got.read == mpwf.HIT
            if got.read == mpwf.HIT:
                self.assertEqual(got.slot.length_ms, lengths.get(rel, 0))
                self.assertEqual(got.slot.bars, mpwf.bars_from_peaks(track_peaks(1, rel)), rel)
        self.assertEqual(hits, r.rows)
        self.assertEqual(s["timed"], sum(1 for rel in lengths if r.find(mpwf.path_hash(rel)).read == mpwf.HIT))

    def test_shapes(self):
        """The kinds a reader must meet: silence, a brick, a song that varies."""
        silent, bricks, songs = 0, 0, 0
        for i in range(1000):
            peaks = track_peaks(2, "Artist/Album/%04d.mp3" % i)
            self.assertEqual(len(peaks), st.SERVER_PEAKS)
            self.assertTrue(all(isinstance(b, int) and 0 <= b <= 255 for b in peaks))
            bars = mpwf.bars_from_peaks(peaks)
            middle = bars[60:150]  # past any fade
            if not any(bars):
                silent += 1
            elif min(middle) >= 200:
                bricks += 1
            elif max(bars) >= 1.5 * min(middle):
                songs += 1
        self.assertTrue(10 <= silent <= 35, silent)
        self.assertTrue(110 <= bricks <= 190, bricks)
        self.assertGreater(songs, 750)

    def test_no_slot_at_all_is_refused(self):
        with self.assertRaises(st.Refused) as e:
            st.synthetic_waves(["A/x.mp3"], {}, 1, 1, 1, unshaped=100.0)
        self.assertIn("no file", str(e.exception))

    def test_read_lengths(self):
        d = tempfile.mkdtemp(prefix="synthtransfer_")
        try:
            p = os.path.join(d, st.TAGS_JSONL)
            Path(p).write_text('{"path": "music/A/x.mp3", "ms": 1200, "tags": {"title": "T", "artist": ["A"]}}\n\n'
                               '{"path": "A/y.mp3", "ms": 0}\n{"path": "music/A/z.mp3"}\n', encoding="utf-8")
            self.assertEqual(st.read_lengths(p), {"A/x.mp3": 1200})
            self.assertEqual(st.read_tags(p), {"A/x.mp3": ("T", ["A"]), "A/y.mp3": (None, []), "A/z.mp3": (None, [])})
        finally:
            shutil.rmtree(d, ignore_errors=True)


class FakeProbe(synthcard.DriveProbe):
    def __init__(self, fs="FAT32", system="C:\\"):
        self.fs, self.sys = fs, system

    def system_drive(self):
        return self.sys

    def info(self, root):
        return {"type": "removable", "label": "TESTCARD", "fs": self.fs, "size": 32 * 10 ** 9, "free": 10 ** 10,
                "cluster": 32768, "readable": True}


def snapshot(card):
    """Every file and folder outside /.mstream: (size, mtime, digest)."""
    out = {}
    for dirpath, dirnames, filenames in os.walk(card):
        rel = os.path.relpath(dirpath, card)
        if rel == ".":
            dirnames[:] = [d for d in dirnames if d != st.MSTREAM]
        for d in dirnames:
            out[os.path.join(rel, d)] = "dir"
        for f in filenames:
            p = os.path.join(dirpath, f)
            with open(p, "rb") as fh:
                out[os.path.join(rel, f)] = (os.path.getsize(p), os.stat(p).st_mtime_ns,
                                             hashlib.sha256(fh.read()).hexdigest())
    return out


class CardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="synthtransfer_")
        cls.card = os.path.join(cls.tmp, "card")
        synthcard.build(cls.card, 300, 5, 2.0, progress=False)
        cls.tracks, cls.files = st.walk_music(cls.card)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        shutil.rmtree(os.path.join(self.card, st.MSTREAM), ignore_errors=True)

    def add(self, **kw):
        out, ops = io.StringIO(), []
        kw.setdefault("synthetic", kw.get("mpdj_file") is None)
        kw.setdefault("commit_time", TIME)
        res = st.add_to(self.card, out=out, ops=ops, **kw)
        return res, ops, out.getvalue()

    def mstream(self):
        return sorted(os.listdir(os.path.join(self.card, st.MSTREAM)))

    def test_walk(self):
        self.assertTrue(self.tracks)
        for rel in self.tracks:
            self.assertIn(rel.rsplit(".", 1)[1].lower(), st.TRACK_EXTS)
            self.assertFalse(any(n.startswith(".") for n in rel.split("/")))
        self.assertTrue(any(f.rsplit(".", 1)[-1] in ("jpg", "png") for f in self.files))
        with open(os.path.join(self.card, st.TAGS_JSONL), encoding="utf-8") as fh:
            jsonl = {json.loads(line)["path"][len("music/"):] for line in fh}
        self.assertEqual(set(self.tracks), jsonl)

    def test_add_to_synthetic(self):
        before = snapshot(self.card)
        res, ops, said = self.add()
        self.assertTrue(res["written"])
        self.assertEqual(self.mstream(), ["autodj-00000001.bin", "manifest.bin", "tags-00000001.bin"])
        # The commit order: the companions, then the root through its tmp.
        self.assertEqual(ops, [("write", "tags-00000001.bin"), ("write", "autodj-00000001.bin"),
                               ("write", "manifest.tmp"), ("rename", "manifest.tmp", "manifest.bin")])
        self.assertEqual(snapshot(self.card), before, "nothing outside /.mstream changed")
        ms = os.path.join(self.card, st.MSTREAM)
        dj = Path(ms, "autodj-00000001.bin").read_bytes()
        self.assertEqual(mpdj.check(dj, "all"), "ok")
        self.assertEqual(mpdj.check(dj, "device"), "ok")
        r = mpdj.Reader(dj)
        self.assertEqual((r.generation, r.tags_generation, r.k, r.kind, r.head, r.floor, r.built_time),
                         (1, 1, 169, 1, 84, 140, TIME))
        paths = {h for h, _ in r.paths()}
        on_card = {mpdj.path_hash(rel): rel for rel in self.tracks}
        self.assertTrue(paths <= set(on_card), "every DJPH path is a track of the tree")
        self.assertEqual(r.path_count, r.rows)
        self.assertEqual(r.rows, res["stats"]["rows"])
        self.assertEqual(len(self.tracks) - r.rows, res["stats"]["unindexed"])
        c = res["check"]
        self.assertTrue(c["present"] and c["autoDj"]["present"])
        self.assertEqual(c["tags"]["records"], 0)
        self.assertEqual((c["autoDj"]["tracks"], c["autoDj"]["mapped"]), (len(self.tracks), r.rows))
        # T: no records, FOLD /music alone, source 2.
        t = Path(ms, "tags-00000001.bin").read_bytes()
        self.assertEqual(t[:4], b"MPTG")
        self.assertEqual(struct.unpack_from("<B", t, 40)[0], 2)
        self.assertEqual(struct.unpack_from("<II", t, 48), (0, 1))
        # The same card id and lists from the same seed.
        self.assertEqual(res["cardId"], st.default_card_id(1))
        self.assertIn("done in", said)
        # No --waves: the root names none.
        self.assertEqual(c["waves"], {"present": False})
        self.assertIsNone(cf.root_waves(Path(ms, "manifest.bin").read_bytes()))
        self.assertIn("waveform index: none named", said)

    def test_add_to_waves(self):
        before = snapshot(self.card)
        res, ops, said = self.add(waves=True)
        self.assertEqual(self.mstream(), ["autodj-00000001.bin", "manifest.bin", "tags-00000001.bin",
                                          "waves-00000001.bin"])
        # The commit order: the companions in COMP's order, then the root.
        self.assertEqual(ops, [("write", "tags-00000001.bin"), ("write", "autodj-00000001.bin"),
                               ("write", "waves-00000001.bin"), ("write", "manifest.tmp"),
                               ("rename", "manifest.tmp", "manifest.bin")])
        self.assertEqual(snapshot(self.card), before, "nothing outside /.mstream changed")
        ms = os.path.join(self.card, st.MSTREAM)
        b = Path(ms, "waves-00000001.bin").read_bytes()
        self.assertEqual(mpwf.check(b, "all"), "ok")
        self.assertEqual(mpwf.check(b, "device"), "ok")
        r = mpwf.Reader(b)
        self.assertEqual((r.generation, r.card_id, r.rows), (1, res["cardId"], res["waves"]["rows"]))
        self.assertEqual(len(self.tracks) - r.rows, res["waves"]["unshaped"])
        # Every slot is a track of the tree, its length the jsonl's.
        lengths = st.read_lengths(os.path.join(self.card, st.TAGS_JSONL))
        on_card = {mpwf.path_hash(rel): rel for rel in self.tracks}
        for i in range(r.rows):
            slot = r.slot(i)[2]
            self.assertEqual(slot.length_ms, lengths[on_card[slot.path_hash]])
        self.assertEqual(res["waves"]["timed"], r.rows)
        # The root names it last in COMP, its name last in STRS, its key zeros.
        root = Path(ms, "manifest.bin").read_bytes()
        self.assertEqual(cf.root_waves(root), ("waves-00000001.bin", 1, len(b), r.header_crc, bytes(16)))
        p = cf.parse_container(root)
        comp = cf.section(p, "COMP")
        self.assertEqual([bytes(comp[4][40 * i:40 * i + 4]) for i in range(comp[2])], [b"MPTG", b"MPDJ", b"MPWF"])
        strs = bytes(cf.section(p, "STRS")[4])
        self.assertTrue(strs.endswith(b"\0autodj-00000001.bin\0waves-00000001.bin\0"), strs)
        c = res["check"]
        self.assertTrue(c["present"] and c["autoDj"]["present"] and c["waves"]["present"], c)
        self.assertEqual((c["waves"]["tracks"], c["waves"]["hits"]), (len(self.tracks), r.rows))
        self.assertIn("waveform index waves-00000001.bin: present", said)
        self.assertIn("%d with a shape" % r.rows, said)
        # The index is the same without --waves: their draws are apart.
        dj = Path(ms, "autodj-00000001.bin").read_bytes()
        shutil.rmtree(ms)
        self.add()
        self.assertEqual(Path(ms, "autodj-00000001.bin").read_bytes(), dj)

    def test_mpdj_with_waves(self):
        other = st.synthetic_index(self.tracks, {}, 8, 40, 0x1234, TIME - 100, unindexed=0.0)[0]
        f = os.path.join(self.tmp, "given-waves.bin")
        Path(f).write_bytes(other)
        res, ops, _ = self.add(mpdj_file=f, waves=True, unshaped=0.0)
        self.assertEqual([s[1] for s in ops if s[0] == "write"], ["tags-00000001.bin", "autodj-00000001.bin",
                                                                 "waves-00000001.bin", "manifest.tmp"])
        self.assertEqual((res["check"]["autoDj"]["mapped"], res["check"]["waves"]["hits"]),
                         (len(self.tracks), len(self.tracks)))

    def test_firmware_readers(self):
        exe, how = build_probe()
        if exe is None:
            self.skipTest("the firmware's readers on the host: %s (the Python mirror is checked instead)" % how)
        res, _, _ = self.add()
        got = run_probe(exe, self.card, self.tracks)
        self.assertTrue(got["present"], got)
        self.assertEqual(got["why"], "ok")
        self.assertEqual((got["generation"], got["cardId"]), (1, "0x%016X" % res["cardId"]))
        self.assertEqual((got["tagsRecords"], got["tagsPath"], got["plan"], got["roots"]),
                         (0, "/.mstream/tags-00000001.bin", False, 0))
        dj = got["autoDj"]
        self.assertTrue(dj["present"], dj)
        self.assertEqual((dj["path"], dj["open"], dj["checkDevice"], dj["checkAll"], dj["matches"]),
                         ("/.mstream/autodj-00000001.bin", "ok", "ok", "ok", True))
        self.assertEqual((dj["rows"], dj["paths"], dj["k"], dj["generation"], dj["tagsGeneration"]),
                         (res["stats"]["rows"], res["stats"]["rows"], 169, 1, 1))
        self.assertEqual((dj["mapped"], dj["unmapped"]), (res["stats"]["rows"], res["stats"]["unindexed"]))
        self.assertFalse(got["waves"]["present"])
        print("\n  the firmware's readers (%s): root, T and the index accepted; %d of %d tracks mapped" % (
            how, dj["mapped"], len(self.tracks)), file=sys.stderr)

    def test_firmware_readers_waves(self):
        exe, how = build_probe()
        if exe is None:
            self.skipTest("the firmware's readers on the host: %s (the Python mirror is checked instead)" % how)
        res, _, _ = self.add(waves=True)
        got = run_probe(exe, self.card, self.tracks)
        self.assertTrue(got["present"] and got["autoDj"]["present"], got)
        wv = got["waves"]
        self.assertTrue(wv["present"], wv)
        self.assertEqual((wv["path"], wv["open"], wv["checkAll"], wv["matches"]),
                         ("/.mstream/waves-00000001.bin", "ok", "ok", True))
        self.assertEqual((wv["rows"], wv["generation"]), (res["waves"]["rows"], 1))
        self.assertEqual((wv["hits"], wv["misses"], wv["other"]), (res["waves"]["rows"], res["waves"]["unshaped"], 0))
        # The firmware's lookup and the reference's read the same slots.
        self.assertEqual(wv["reads"], res["check"]["waves"]["reads"])
        print("\n  the firmware's readers (%s): the waveform index accepted; %d of %d tracks have a shape, %.1f slot "
              "reads a lookup" % (how, wv["hits"], len(self.tracks), wv["reads"] / len(self.tracks)), file=sys.stderr)

    def test_check_command(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(st.main(["--check", self.card]), 1)
            self.add()
            self.assertEqual(st.main(["--check", self.card]), 0)
        self.assertIn("no transfer data", out.getvalue())
        self.assertIn("AutoDJ index autodj-00000001.bin: present", out.getvalue())

    def test_check_command_waves(self):
        self.add(waves=True)
        p = Path(self.card, st.MSTREAM, "waves-00000001.bin")
        good = p.read_bytes()
        bad = bytearray(good)
        bad[300] ^= 1
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(st.main(["--check", self.card]), 0)
            # Absent, by a flipped bit and by no file: the root is still good.
            p.write_bytes(bytes(bad))
            self.assertEqual(st.main(["--check", self.card]), 1)
            p.unlink()
            self.assertEqual(st.main(["--check", self.card]), 1)
        said = out.getvalue()
        self.assertIn("waveform index waves-00000001.bin: present", said)
        self.assertIn("with a shape", said)
        self.assertIn("waveform index waves-00000001.bin: absent (wvSlotCrc)", said)
        self.assertIn("waveform index waves-00000001.bin: absent (missing)", said)
        self.assertEqual(said.count("AutoDJ index autodj-00000001.bin: present"), 3)

    def test_dry_run_writes_nothing(self):
        before = snapshot(self.card)
        res, ops, said = self.add(dry_run=True)
        self.assertFalse(res["written"])
        self.assertEqual(ops, [])
        self.assertFalse(os.path.exists(os.path.join(self.card, st.MSTREAM)))
        self.assertEqual(snapshot(self.card), before)
        self.assertIn("nothing written", said)
        self.assertEqual([s[0] for s in res["steps"]], ["write", "write", "write", "rename"])

    def test_waves_on_the_command_line(self):
        before = snapshot(self.card)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(st.main(["--add-to", self.card, "--synthetic", "--waves", "--unshaped", "0", "--dry-run",
                                      "--time", str(TIME)]), 0)
        self.assertIn("write waves-00000001.bin", out.getvalue())
        n = len(self.tracks)
        self.assertIn("%d rows (%d tracks, 0 left without a slot)" % (n, n), out.getvalue())
        self.assertFalse(os.path.exists(os.path.join(self.card, st.MSTREAM)))
        self.assertEqual(snapshot(self.card), before)

    def test_refuses_a_mstream_that_isnt_empty(self):
        ms = os.path.join(self.card, st.MSTREAM)
        os.makedirs(ms)
        Path(ms, "state.json").write_text("{}")
        with self.assertRaises(st.Refused):
            self.add()
        self.assertEqual(self.mstream(), ["state.json"])
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(st.main(["--add-to", self.card, "--synthetic", "--time", str(TIME)]), 2)
        res, _, _ = self.add(replace=True)
        self.assertEqual(self.mstream(), ["autodj-00000001.bin", "manifest.bin", "state.json", "tags-00000001.bin"])

    def test_replace_commits_the_next_generation(self):
        first, _, _ = self.add(seed=3)
        ms = os.path.join(self.card, st.MSTREAM)
        Path(ms, "pending.bin").write_bytes(b"a cut run's plan")
        os.makedirs(os.path.join(ms, "thumbs", "ab"))
        Path(ms, "thumbs", "ab", "00000000.565").write_bytes(b"x")
        Path(ms, "state.json").write_text("{}")
        before = snapshot(self.card)
        res, ops, _ = self.add(replace=True, seed=4)
        self.assertEqual(res["generation"], 2)
        self.assertEqual(res["cardId"], first["cardId"], "the card keeps its id")
        self.assertEqual(ops, [("write", "tags-00000002.bin"), ("write", "autodj-00000002.bin"),
                               ("write", "manifest.tmp"), ("remove", "manifest.bin"),
                               ("rename", "manifest.tmp", "manifest.bin"), ("remove", "autodj-00000001.bin"),
                               ("remove", "pending.bin"), ("remove", "tags-00000001.bin")])
        self.assertEqual(self.mstream(), ["autodj-00000002.bin", "manifest.bin", "state.json", "tags-00000002.bin",
                                          "thumbs"])
        self.assertTrue(os.path.isfile(os.path.join(ms, "thumbs", "ab", "00000000.565")))
        self.assertEqual(snapshot(self.card), before)
        c = st.check_card(self.card)
        self.assertEqual((c["root"]["generation"], c["root"]["baseGeneration"]), (2, 1))
        self.assertNotEqual(c["autoDj"]["sig"], first["check"]["autoDj"]["sig"])
        # A generation the card has used is refused.
        with self.assertRaises(st.Refused):
            self.add(replace=True, generation=2)

    def test_replace_and_waves(self):
        self.add(seed=3, waves=True)
        _, ops, _ = self.add(replace=True, seed=4, waves=True)
        self.assertEqual(ops, [("write", "tags-00000002.bin"), ("write", "autodj-00000002.bin"),
                               ("write", "waves-00000002.bin"), ("write", "manifest.tmp"), ("remove", "manifest.bin"),
                               ("rename", "manifest.tmp", "manifest.bin"), ("remove", "autodj-00000001.bin"),
                               ("remove", "tags-00000001.bin"), ("remove", "waves-00000001.bin")])
        # No --waves: the new root names none, and the old file goes after it.
        _, ops, _ = self.add(replace=True, seed=5)
        self.assertEqual(ops[-3:], [("remove", "autodj-00000002.bin"), ("remove", "tags-00000002.bin"),
                                    ("remove", "waves-00000002.bin")])
        self.assertEqual(self.mstream(), ["autodj-00000003.bin", "manifest.bin", "tags-00000003.bin"])
        self.assertEqual(st.check_card(self.card)["waves"], {"present": False})
        # A waves file's name counts: the next generation is past it, and no
        # generation reuses one (METADATA 2.5.2).
        ms = os.path.join(self.card, st.MSTREAM)
        Path(ms, "waves-00000009.bin").write_bytes(b"a cut run's")
        with self.assertRaises(st.Refused) as e:
            self.add(replace=True, generation=9, seed=6)
        self.assertIn("waves-00000009.bin", str(e.exception))
        res, ops, _ = self.add(replace=True, seed=6)
        self.assertEqual(res["generation"], 10)
        self.assertIn(("remove", "waves-00000009.bin"), ops)

    def test_replace_refuses_another_programs_mstream(self):
        # A /.mstream a transfer wrote (its root's producer another's): its
        # tags file, index and plan would go, so --replace alone refuses.
        with mock.patch.object(st, "PRODUCER", "mstream transfer 9"):
            self.add(seed=3)
        ms = os.path.join(self.card, st.MSTREAM)
        Path(ms, "pending.bin").write_bytes(b"a transfer's plan")
        before = {n: Path(ms, n).read_bytes() for n in self.mstream()}
        with self.assertRaises(st.Refused) as e:
            self.add(replace=True, seed=4)
        self.assertIn("another program's files", str(e.exception))
        with self.assertRaises(st.Refused):
            self.add(replace=True, seed=4, dry_run=True)
        self.assertEqual({n: Path(ms, n).read_bytes() for n in self.mstream()}, before, "nothing changed")
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(st.main(["--add-to", self.card, "--synthetic", "--replace", "--time", str(TIME)]), 2)
        self.assertEqual({n: Path(ms, n).read_bytes() for n in self.mstream()}, before, "nothing changed")
        # --replace-foreign replaces it.
        res, _, _ = self.add(replace=True, seed=4, replace_foreign=True)
        self.assertEqual(res["generation"], 2)
        self.assertEqual(self.mstream(), ["autodj-00000002.bin", "manifest.bin", "tags-00000002.bin"])
        # Contract files with no root that reads: whose they are is unknown.
        shutil.rmtree(ms)
        os.makedirs(ms)
        Path(ms, "pending.bin").write_bytes(b"a cut first run's plan")
        with self.assertRaises(st.Refused) as e:
            self.add(replace=True)
        self.assertIn("no root that reads", str(e.exception))
        self.assertEqual(self.mstream(), ["pending.bin"])
        res, _, _ = self.add(replace=True, replace_foreign=True)
        self.assertEqual(self.mstream(), ["autodj-00000001.bin", "manifest.bin", "tags-00000001.bin"])
        self.assertEqual(st.foreign(None, []), None)

    def test_replace_keeps_the_private_files(self):
        # METADATA 2.12.7, 2.12.8: the software's private files and its kept
        # partials stay through --replace, and through --replace-foreign; a
        # run.lock (a transfer writing the card, or a cut one) refuses
        # --replace alone.
        self.add(seed=3)
        ms = os.path.join(self.card, st.MSTREAM)
        state = lambda seq: ('{"format": "mstream-card-state", "version": 1, "seq": %d}\n' % seq).encode()  # noqa: E731
        private = {"state.json": state(2), "state.new": state(3), "notes.txt": b"x", "cache/index.bin": b"y",
                   "resume.json": b'{"format": "mstream-card-resume", "version": 1, "seq": 1, "parts": []}\n',
                   "resume.new": b"torn", "resume/0000002a.part": b"z" * 100}
        for name, data in private.items():
            os.makedirs(os.path.dirname(os.path.join(ms, name)), exist_ok=True)
            Path(ms, name).write_bytes(data)
        res, ops, _ = self.add(replace=True, seed=4)
        self.assertEqual(res["generation"], 2)
        self.assertEqual({n: Path(ms, n).read_bytes() for n in private}, private)
        self.assertEqual([s for s in ops if s[0] == "remove"], [("remove", "manifest.bin"),
                                                               ("remove", "autodj-00000001.bin"),
                                                               ("remove", "tags-00000001.bin")])
        Path(ms, "run.lock").write_text('{"format": "mstream-card-run", "runId": "0x1", "started": 1}\n')
        with self.assertRaises(st.Refused) as e:
            self.add(replace=True, seed=5)
        self.assertIn("run.lock", str(e.exception))
        self.assertEqual(st.check_card(self.card)["root"]["generation"], 2, "nothing written")
        res, _, _ = self.add(replace=True, seed=5, replace_foreign=True)
        self.assertEqual(res["generation"], 3)
        self.assertTrue(Path(ms, "run.lock").is_file(), "the run.lock is the other program's to take over")
        self.assertEqual({n: Path(ms, n).read_bytes() for n in private}, private)

    def test_replace_deletes_only_what_a_run_would(self):
        # What --replace deletes is a subset of what a contract run deletes
        # in the same place (2.12.1: step 4's clean-up, and the final
        # commit's pending.bin, step 12), card_fixtures.cleanup being the
        # reference; the private names never match a contract file's.
        self.add(seed=3, waves=True)
        ms = os.path.join(self.card, st.MSTREAM)
        extra = ["manifest.tmp", "pending.bin", "pending.tmp", "stage/00000001.tmp", "thumbs/ab/00000000.tmp",
                 "resume/0000002a.part", "state.json", "resume.json", "notes.txt"]
        for name in extra:
            os.makedirs(os.path.dirname(os.path.join(ms, name)), exist_ok=True)
            Path(ms, name).write_bytes(b"x")
        listing = {}
        for dirpath, _, names in os.walk(ms):
            for n in names:
                rel = os.path.relpath(os.path.join(dirpath, n), ms).replace(os.sep, "/")
                listing[rel] = os.path.getsize(os.path.join(dirpath, n))
        _, ops, _ = self.add(replace=True, seed=4)
        removed = {s[1] for s in ops if s[0] == "remove"} - {"manifest.bin"}  # the root's own replacement
        run = set(cf.cleanup(listing, ["tags-00000002.bin", "autodj-00000002.bin"], None)) | {"pending.bin"}
        self.assertTrue(removed <= run, removed - run)
        self.assertEqual(removed, {"manifest.tmp", "pending.bin", "pending.tmp", "autodj-00000001.bin",
                                   "tags-00000001.bin", "waves-00000001.bin"})
        for e in cf.private_vectors()["names"]:
            if e["ok"] and "/" not in e["name"]:
                self.assertIsNone(st.CONTRACT.match(e["name"]), e["name"])
        for name in cf.RESERVED:
            self.assertIsNone(st.CONTRACT.match(name), name)
        # Every companion's name is a contract file's, and counts generations.
        for stem in cf.COMPANION_STEMS.values():
            name = "%s-0000002a.bin" % stem
            self.assertIsNotNone(st.CONTRACT.match(name), name)
            self.assertEqual(st.GENERATION_NAME.match(name).group(2), "0000002a")

    def test_stale_manifest_tmp_goes_first(self):
        self.add()
        ms = os.path.join(self.card, st.MSTREAM)
        Path(ms, "manifest.tmp").write_bytes(b"a cut write")
        _, ops, _ = self.add(replace=True)
        self.assertEqual(ops, [("remove", "manifest.tmp"), ("write", "tags-00000002.bin"),
                               ("write", "autodj-00000002.bin"), ("write", "manifest.tmp"), ("remove", "manifest.bin"),
                               ("rename", "manifest.tmp", "manifest.bin"), ("remove", "autodj-00000001.bin"),
                               ("remove", "tags-00000001.bin")])
        self.assertTrue(st.check_card(self.card)["autoDj"]["present"])

    def test_refuses_a_tree_without_music(self):
        empty = os.path.join(self.tmp, "no-music")
        os.makedirs(empty, exist_ok=True)
        with self.assertRaises(st.Refused):
            st.add_to(empty, synthetic=True, out=io.StringIO())
        self.assertEqual(os.listdir(empty), [])

    def test_drives(self):
        w = io.StringIO().write
        with self.assertRaises(st.Refused):
            st.check_drive("C:\\", FakeProbe(), w)
        with self.assertRaises(st.Refused):
            st.check_drive("E:\\", FakeProbe(fs="exFAT"), w)
        st.check_drive("E:\\", FakeProbe(), w)
        st.check_drive("E:\\", FakeProbe(fs="FAT"), w)
        self.assertEqual(st.card_root("E:"), "E:\\" if os.name == "nt" else os.path.abspath("E:\\"))

    def test_mpdj_restamped(self):
        # An index built for this tree elsewhere (another generation and card id).
        other = st.synthetic_index(self.tracks, {}, 8, 40, 0x1234, TIME - 100, unindexed=0.0)[0]
        f = os.path.join(self.tmp, "given.bin")
        Path(f).write_bytes(other)
        res, _, _ = self.add(mpdj_file=f)
        ms = os.path.join(self.card, st.MSTREAM)
        dj = Path(ms, "autodj-00000001.bin").read_bytes()
        r = mpdj.Reader(dj)
        self.assertEqual((r.generation, r.card_id, r.tags_generation, r.built_time),
                         (1, res["cardId"], 1, TIME - 100))
        self.assertIsNone(mpdj.same_content(other, dj))
        self.assertEqual(r.sig, mpdj.Reader(other).sig)
        self.assertTrue(res["check"]["autoDj"]["present"])
        self.assertEqual(res["check"]["autoDj"]["mapped"], len(self.tracks))

    def test_mpdj_refusals(self):
        tracks, tags = fake_tracks()
        elsewhere = st.synthetic_index(tracks, tags, 1, 4, 5, TIME, k=12, head=4, floor=120, unindexed=0.0)[0]
        f = os.path.join(self.tmp, "elsewhere.bin")
        Path(f).write_bytes(elsewhere)
        with self.assertRaises(st.Refused) as e:
            self.add(mpdj_file=f)
        self.assertIn("another tree", str(e.exception))
        # Part of this tree: refused unless --allow-missing.
        part = st.synthetic_index(self.tracks + tracks, tags, 1, 4, 5, TIME, k=12, head=4, floor=120,
                                  unindexed=0.0)[0]
        Path(f).write_bytes(part)
        with self.assertRaises(st.Refused):
            self.add(mpdj_file=f)
        res, _, _ = self.add(mpdj_file=f, allow_missing=True)
        self.assertEqual(res["stats"]["missing"], len(tracks))
        shutil.rmtree(os.path.join(self.card, st.MSTREAM))
        # A path to a file of /music the device doesn't list (a cover):
        # no track on the device, so not on the card either.
        cover = [rel for rel in self.files if rel not in set(self.tracks)][:1]
        self.assertTrue(cover)
        with_cover = st.synthetic_index(self.tracks + cover, {}, 1, 4, 5, TIME, k=12, head=4, floor=120,
                                        unindexed=0.0)[0]
        Path(f).write_bytes(with_cover)
        with self.assertRaises(st.Refused) as e:
            self.add(mpdj_file=f)
        self.assertIn("aren't tracks", str(e.exception))
        res, _, _ = self.add(mpdj_file=f, allow_missing=True)
        self.assertEqual(res["stats"]["missing"], 1)
        shutil.rmtree(os.path.join(self.card, st.MSTREAM))
        # A broken file.
        bad = bytearray(part)
        bad[50] ^= 1
        Path(f).write_bytes(bytes(bad))
        with self.assertRaises(st.Refused) as e:
            self.add(mpdj_file=f)
        self.assertIn("absent", str(e.exception))
        self.assertFalse(os.path.exists(os.path.join(self.card, st.MSTREAM)))


class RestampTests(unittest.TestCase):
    def test_equals_mpdj_restamp(self):
        """The header restamp is tools/mpdj.py restamp's file when the
        description gives the same rows (AUTODJ-INDEX.md 7.3)."""
        desc = mpdj.g1()
        old = mpdj.build(desc)
        new = dict(desc, generation=77, cardId="0x0102030405060708", tagsGeneration=77, builtTime=5)
        self.assertEqual(st.restamp_header(old, 77, 0x0102030405060708, 77), mpdj.restamp(old, new))

    def test_check_root_mirror(self):
        tags = st.tags_file(4, 99)
        dj = st.synthetic_index(*fake_tracks(), 2, 4, 99, TIME, k=12, head=4, floor=120, unindexed=0.0)[0]
        root = st.root_file(4, 99, 3, TIME, tags, dj)
        files = {"tags-00000004.bin": tags, "autodj-00000004.bin": dj, "manifest.bin": root}
        c = st.check_commit(files.get)
        self.assertTrue(c["present"] and c["autoDj"]["present"], c)
        self.assertEqual((c["root"]["generation"], c["root"]["cardId"], c["root"]["baseGeneration"]), (4, 99, 3))
        # Each companion must be the one COMP names; a byte off breaks it.
        for name in files:
            bad = dict(files)
            b = bytearray(bad[name])
            b[-1] ^= 0x40
            bad[name] = bytes(b)
            c = st.check_commit(bad.get)
            self.assertFalse(c["present"] and c["autoDj"]["present"], name)
        # manifest.tmp wins over an older manifest.bin (2.5.4).
        older = st.root_file(3, 99, 2, TIME, st.tags_file(3, 99), None)
        both = dict(files, **{"manifest.tmp": root, "manifest.bin": older, "tags-00000003.bin": st.tags_file(3, 99)})
        self.assertEqual(st.check_commit(both.get)["rootName"], "manifest.tmp")

    def test_check_commit_waves(self):
        tracks, tags = fake_tracks()
        t = st.tags_file(4, 99)
        dj = st.synthetic_index(tracks, tags, 2, 4, 99, TIME, k=12, head=4, floor=120, unindexed=0.0)[0]
        wv = st.synthetic_waves(tracks, {}, 2, 4, 99, unshaped=0.0)[0]
        root = st.root_file(4, 99, 3, TIME, t, dj, wv)
        files = {"tags-00000004.bin": t, "autodj-00000004.bin": dj, "waves-00000004.bin": wv, "manifest.bin": root}
        c = st.check_commit(files.get)
        self.assertTrue(c["present"] and c["autoDj"]["present"] and c["waves"]["present"], c)
        self.assertEqual((c["waves"]["rows"], c["waves"]["generation"], c["waves"]["cardId"]), (len(tracks), 4, 99))
        # A bad file costs only itself: the root, T and the index stay.
        flipped = bytearray(wv)
        flipped[-1] ^= 0x40
        other = st.synthetic_waves(tracks[:-1], {}, 2, 4, 99, unshaped=0.0)[0]
        for bad, why in ((None, "missing"), (bytes(flipped), "wvSlotCrc"), (b"MPWX" + wv[4:], "magic"),
                         (other, "not the one COMP names")):
            c = st.check_commit(dict(files, **{"waves-00000004.bin": bad}).get)
            self.assertTrue(c["present"] and c["autoDj"]["present"], why)
            self.assertEqual((c["waves"]["present"], c["waves"]["why"]), (False, why))
        # With no MPDJ row, the waves are read all the same.
        files["manifest.bin"] = st.root_file(4, 99, 3, TIME, t, None, wv)
        c = st.check_commit(files.get)
        self.assertTrue(c["present"] and c["waves"]["present"] and not c["autoDj"]["present"], c)

    def test_check_root_takes_the_row_the_fixtures_say(self):
        """The mirror against the firmware's rule for a root's MPWF row
        (WAVEFORM-INDEX.md 7.2; the mpwf group's rootRow, and index.json)."""
        rr = json.loads((FIXTURES / "vectors.json").read_text(encoding="utf-8"))["mpwf"]["rootRow"]
        golden = (FIXTURES / "golden" / rr["golden"]["waves"]).read_bytes()

        def read(name):
            return golden if name == rr["golden"]["waves"] else None
        cases = [dict(rr["golden"], path=FIXTURES / "golden" / rr["golden"]["file"])]
        cases += [dict(e, path=FIXTURES / "hardening" / e["file"]) for e in rr["hardening"]]
        for e in cases:
            why, root = st.check_root(e["path"].read_bytes())
            self.assertEqual(why, "ok", e["file"])
            self.assertEqual(root["waves"]["name"] if root["waves"] else None, e["waves"], e["file"])
            if root["waves"]:
                self.assertEqual(st.check_waves(root["waves"], read)["present"], e["matches"], e["file"])
        for name, want in (("msmf-waves-name-range.bin", "string"), ("msmf-waves-name-order.bin", "stringOrder")):
            self.assertEqual(st.check_root((FIXTURES / "hardening" / name).read_bytes())[0], want, name)


# ---------------------------------------------------------------------------
# --moods: T's records, read back with card_fixtures' reader (not
# synthtransfer's), the card's files, and tools/moodfilter.py
# ---------------------------------------------------------------------------
DEFAULT_T = "a6eb6a5414fa8f8fa4f23179d55c9ae5f78b4905cf0250709e9ca765a0d234ce"  # tags_file(4, 99) before --moods


def raw_records(t):
    """({path: its RECS row}, the stride, the minor), by card_fixtures'
    parse_container and ledger_rows."""
    p = cf.parse_container(t)
    recs = cf.section(p, "RECS")
    paths = [e["path"] for e in cf.ledger_rows(t)]
    return {path: bytes(recs[4][recs[3] * k:recs[3] * (k + 1)]) for k, path in enumerate(paths)}, recs[3], p["minor"]


def run_values(strs, row):
    """A record's string-run values (METADATA 2.6.5): [] for no run."""
    off = struct.unpack_from("<I", row, 8)[0]
    if not off:
        return []
    out, at = [], off + 1
    for _ in range(strs[off]):
        end = strs.index(0, at)
        out.append(strs[at:end].decode("utf-8"))
        at = end + 1
    return out


def fat_fields(mtime):
    """2.3.4's raw FAT fields of a file's time, by hand: its local wall time
    (on Windows, at the bias in force now)."""
    if os.name == "nt":
        t = time.gmtime(int(mtime) + time.localtime().tm_gmtoff)
    else:
        t = time.localtime(int(mtime))
    return ((t.tm_year - 1980) << 25 | t.tm_mon << 21 | t.tm_mday << 16 | t.tm_hour << 11 | t.tm_min << 5 |
            t.tm_sec // 2)


class MoodScoresTests(unittest.TestCase):
    def test_deterministic_and_keyed_by_path(self):
        tracks, _ = fake_tracks()
        a = st.mood_scores(tracks, 5)
        self.assertEqual(a, st.mood_scores(tracks, 5))
        self.assertNotEqual(a, st.mood_scores(tracks, 6))
        # A track's row is its own, whatever else the tree holds.
        self.assertEqual(st.mood_scores(tracks[::7], 5), {rel: a[rel] for rel in tracks[::7]})

    def test_rows_and_heads(self):
        tracks = ["Artist %03d/Album %d/%03d.flac" % (i % 150, i % 3, i) for i in range(4000)]
        s = st.mood_scores(tracks, 1)
        rows = [r for r in s.values() if r is not None]
        self.assertTrue(0.90 <= len(rows) / len(tracks) <= 0.94, len(rows))
        heads = [x for r in rows for x in r]
        self.assertEqual(len(heads), len(mf.MOOD_COLUMNS) * len(rows))
        self.assertTrue(0.005 <= sum(x is None for x in heads) / len(heads) <= 0.015)
        for x in heads:
            self.assertTrue(x is None or (0 <= x <= 1 and round(x, 4) == x), x)
        # Every head meets every bucket, so each bar has tracks on both sides.
        for i, head in enumerate(mf.MOOD_COLUMNS):
            self.assertEqual({mf.bucket(mf.value(r[i])) for r in rows if r[i] is not None}, {0, 1, 2, 3}, head)
        # The percentages decide only what they decide: a row at a lower
        # coverage is the same row, a head NULL at 1 % is NULL at 5 %.
        low = st.mood_scores(tracks, 1, coverage=50.0, null=5.0)
        self.assertTrue(0.45 <= sum(r is not None for r in low.values()) / len(tracks) <= 0.55)
        for rel, r in low.items():
            if r is not None:
                self.assertIsNotNone(s[rel], rel)
                for x, y in zip(s[rel], r):
                    self.assertTrue(y is None or x == y, rel)
                    self.assertTrue(x is not None or y is None, rel)
        self.assertTrue(all(r is None for r in st.mood_scores(tracks[:200], 1, coverage=0.0).values()))
        full = st.mood_scores(tracks[:200], 1, coverage=100.0, null=0.0).values()
        self.assertTrue(all(r is not None and None not in r for r in full))
        gone = st.mood_scores(tracks[:200], 1, coverage=100.0, null=100.0).values()
        self.assertTrue(all(r == [None] * len(mf.MOOD_COLUMNS) for r in gone))

    def test_albums_cluster(self):
        # Shared genre, artist and album draws: an album's tracks lie closer
        # together than the card's, so an album's majority means something.
        tracks = ["Artist %02d/Album %d/%02d.mp3" % (a, b, t) for a in range(40) for b in range(3) for t in range(10)]
        s = st.mood_scores(tracks, 3, coverage=100.0, null=0.0)

        def spread(xs):
            m = sum(xs) / len(xs)
            return sum((x - m) ** 2 for x in xs) / len(xs)
        for i, head in enumerate(mf.MOOD_COLUMNS):
            card = spread([s[rel][i] for rel in tracks])
            within = sum(spread([s[rel][i] for rel in tracks[k:k + 10]]) for k in range(0, len(tracks), 10))
            self.assertLess(within / (len(tracks) / 10), 0.5 * card, head)

    def test_api_record(self):
        j = {"kind": "flac", "ms": 1234, "pic": {"offset": 100, "length": 900, "coding": 0, "mime": "png"},
             "tags": {"title": "T", "artist": ["A", "B"], "album": "Al", "albumartist": ["X"], "genre": ["G"],
                      "year": 2001, "track": 3, "tracktotal": 9, "disc": 1, "disctotal": 0, "rg_track": "-3.21 dB",
                      "rg_album": "-2.00 dB", "mbtrack": "a-b", "mbalbum": "c-d", "artistsort": "Z", "compilation": 1,
                      "bpm": 120}}
        r = st.api_record("A/Al/03 T.flac", j)
        self.assertEqual(r["flags"], 1 << 9 | 1 << 2)  # FROM_API, HAS_RG_TRACK
        self.assertEqual(r["known"], sum(1 << b for b in (0, 1, 2, 4, 7, 8, 9, 10, 11, 14, 16)))
        # The fallback's fields (METADATA 2.7) alone: no album artist, sort
        # field, album id, album gain, compilation or BPM.
        self.assertEqual(r["tags"], {"title": ["T"], "artist": ["A", "B"], "album": ["Al"], "genre": ["G"],
                                     "mbRecordingId": ["a-b"]})
        self.assertEqual((r["year"], r["track"], r["trackTotal"], r["disc"], r["discTotal"], r["rgTrackGain"],
                          r["durationMs"], r["container"]), (2001, 3, 9, 1, 0, -321, 1234, 2))
        self.assertNotIn("rgAlbumGain", r)
        self.assertEqual(r["picture"], {"offset": 100, "length": 900, "type": 3, "mime": 2, "coding": 0})
        unlisted = st.api_record("A/x.opus", None)
        self.assertEqual((unlisted["flags"], unlisted["known"], unlisted["tags"], unlisted["container"]),
                         (1 << 9, 0, {}, 3))
        bare = st.api_record("A/y.mp3", {"kind": "mp3", "ms": 0, "pic": None, "tags": {}})
        self.assertEqual((bare["known"], bare["tags"], bare["container"]), (0, {}, 1))


class MoodCardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="synthtransfer_moods_")
        cls.card = os.path.join(cls.tmp, "card")
        synthcard.build(cls.card, 300, 5, 2.0, progress=False)
        cls.tracks, _ = st.walk_music(cls.card)
        cls.jsonl = dict(st.jsonl_rows(os.path.join(cls.card, st.TAGS_JSONL)))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        shutil.rmtree(os.path.join(self.card, st.MSTREAM), ignore_errors=True)

    def add(self, **kw):
        out = io.StringIO()
        kw.setdefault("synthetic", True)
        kw.setdefault("moods", True)
        kw.setdefault("commit_time", TIME)
        res = st.add_to(self.card, out=out, **kw)
        return res, out.getvalue()

    def again(self, **kw):
        shutil.rmtree(os.path.join(self.card, st.MSTREAM))
        return self.add(**kw)

    def tags(self):
        return Path(self.card, st.MSTREAM, "tags-00000001.bin").read_bytes()

    def music(self, rel):
        return os.path.join(self.card, "music", *rel.split("/"))

    def test_records_are_the_cards(self):
        # One record a track, each holding what the device reads of the file
        # (so rule 1 takes it), named from the jsonl.
        res, said = self.add()
        t = self.tags()
        rows, stride, minor = raw_records(t)
        self.assertEqual((stride, minor), (80, 1))
        self.assertEqual(sorted(rows, key=lambda r: r.encode("utf-8")), self.tracks)
        strs = bytes(cf.section(cf.parse_container(t), "STRS")[4])
        pictures = 0
        for rel in self.tracks:
            row, j = rows[rel], self.jsonl[rel]
            data = Path(self.music(rel)).read_bytes()
            size, fat, dur, qfp, known, flags = struct.unpack_from("<IIIQIH", row, 12)
            self.assertEqual((size, qfp, fat), (len(data), cf.qfp(data), fat_fields(os.stat(self.music(rel)).st_mtime)),
                             rel)
            self.assertEqual((size, dur), (j["bytes"], j["ms"]), rel)
            self.assertEqual(flags & (st.FROM_API | st.UNREADABLE), st.FROM_API, rel)
            title, artists = st.record_values(j["tags"])
            vals = run_values(strs, row)
            self.assertEqual(vals[0] if vals else "", title or "", rel)
            self.assertEqual(vals[1].split("\x1f") if len(vals) > 1 and vals[1] else [], artists, rel)
            self.assertEqual((known & 1, known >> 1 & 1), (int(bool(title)), int(bool(artists))), rel)
            off, length, typ, mime, coding = struct.unpack_from("<IIBBB", row, 60)
            if j["pic"]:
                pictures += 1
                self.assertEqual((off, length, typ, coding, mime, known >> 16 & 1),
                                 (j["pic"]["offset"], j["pic"]["length"], 3, j["pic"]["coding"],
                                  2 if j["pic"]["mime"] == "png" else 1, 1), rel)
            else:
                self.assertEqual((off, length, known >> 16 & 1), (0, 0, 0), rel)
        self.assertTrue(pictures)
        m = res["check"]["moods"]
        self.assertEqual((m["records"], m["byTime"], m["byQfp"], m["stale"], m["noRecord"]),
                         (len(self.tracks), len(self.tracks), 0, 0, 0))
        self.assertIn("rule 1 takes T's record for %d of %d tracks" % (len(self.tracks), len(self.tracks)), said)

    def test_tails_and_words(self):
        res, said = self.add(seed=7)
        rows = raw_records(self.tags())[0]
        want = st.mood_scores(self.tracks, 7)
        words = res["check"]["moods"]["words"]
        for rel in self.tracks:
            tail = rows[rel][72:80]
            self.assertEqual(tail, mf.tail_of(want[rel]), rel)
            self.assertEqual(mf.tail_valid(tail), want[rel] is not None, rel)
            self.assertEqual(words[rel], mf.word_of_tail(tail), rel)
            self.assertEqual(words[rel], mf.word_of(want[rel]), rel)
        n = sum(1 for s in want.values() if s is not None)
        self.assertTrue(0.8 <= n / len(self.tracks) <= 0.98, n)
        m = res["check"]["moods"]
        self.assertEqual((m["withWord"], m["tails"], m["blockBytes"], res["moods"]["rows"]),
                         (n, n, 2 * len(self.tracks), n))
        self.assertIn("moods: %d of %d tracks with a word" % (n, len(self.tracks)), said)

    def test_control_card(self):
        # --mood-coverage 0: the same records at stride 72, no tails, minor 0.
        res, said = self.add(mood_coverage=0.0)
        rows, stride, minor = raw_records(self.tags())
        self.assertEqual((stride, minor, len(rows)), (72, 0, len(self.tracks)))
        m = res["check"]["moods"]
        self.assertEqual((m["withWord"], m["tails"], m["blockBytes"], m["byTime"]), (0, 0, 0, len(self.tracks)))
        self.assertIn("moods: none: T's records are 72 bytes", said)
        self.again()
        tailed = raw_records(self.tags())[0]
        self.assertEqual(rows, {rel: row[:72] for rel, row in tailed.items()})

    def test_mood_null(self):
        res, _ = self.add(mood_null=100.0)
        rows = raw_records(self.tags())[0]
        self.assertEqual({row[72:80] for row in rows.values()}, {bytes(8), bytes([1, 255, 255, 255, 255, 255, 255, 0])})
        self.assertEqual(set(res["check"]["moods"]["words"].values()), {0, 0xE400})
        self.assertEqual(res["moods"]["nullHeads"], res["moods"]["heads"])
        res, _ = self.again(mood_null=0.0)
        self.assertFalse(any(255 in row[73:79] for row in raw_records(self.tags())[0].values()))
        self.assertEqual(res["moods"]["nullHeads"], 0)

    def test_deterministic_by_seed(self):
        self.add(seed=3)
        a = self.tags()
        self.again(seed=3)
        self.assertEqual(self.tags(), a)
        self.again(seed=4)
        ra, rb = raw_records(a)[0], raw_records(self.tags())[0]
        self.assertEqual({k: v[:72] for k, v in ra.items()}, {k: v[:72] for k, v in rb.items()})
        self.assertNotEqual({k: v[72:] for k, v in ra.items()}, {k: v[72:] for k, v in rb.items()})

    def test_default_unchanged(self):
        # Without --moods, T is the zero-record file it was before --moods.
        self.assertEqual(hashlib.sha256(st.tags_file(4, 99)).hexdigest(), DEFAULT_T)
        res, said = self.add(moods=False)
        self.assertEqual(self.tags(), st.tags_file(1, res["cardId"]))
        self.assertIsNone(res["moods"])
        self.assertNotIn("moods", res["check"])
        self.assertNotIn("moods:", said)
        self.assertIn("(MPTG source 2, no records)", said)

    def test_rule_1(self):
        # --check takes a record as the device's builder does: a file whose
        # time moved alone is confirmed by its qfp, one whose size changed
        # is stale and gets no word; a uniform move is T's skew.
        res, _ = self.add(mood_coverage=100.0, mood_null=0.0)
        n = len(self.tracks)
        words = res["check"]["moods"]["words"]
        moved, grown = self.tracks[3], self.tracks[10]
        stats = {rel: os.stat(self.music(rel)) for rel in self.tracks}
        kept = Path(self.music(grown)).read_bytes()
        try:
            os.utime(self.music(moved), ns=(stats[moved].st_atime_ns, stats[moved].st_mtime_ns + 3600 * 10 ** 9))
            with open(self.music(grown), "ab") as fh:
                fh.write(b"\0")
            m = st.check_card(self.card)["moods"]
            self.assertEqual((m["byTime"], m["byQfp"], m["stale"], m["skew"]), (n - 2, 1, 1, 0))
            self.assertEqual((m["words"][moved], m["words"][grown]), (words[moved], 0))
            Path(self.music(grown)).write_bytes(kept)
            for rel in self.tracks:
                os.utime(self.music(rel), ns=(stats[rel].st_atime_ns, stats[rel].st_mtime_ns + 3600 * 10 ** 9))
            m = st.check_card(self.card)["moods"]
            self.assertEqual((m["skew"], m["byTime"] + m["byQfp"], m["stale"]), (3600, n, 0))
            self.assertGreater(m["byTime"], n // 2)
            self.assertEqual(m["words"], words)
        finally:
            Path(self.music(grown)).write_bytes(kept)
            for rel, s in stats.items():
                os.utime(self.music(rel), ns=(s.st_atime_ns, s.st_mtime_ns))
        self.assertEqual(st.check_card(self.card)["moods"]["byTime"], n)

    def test_check_mood_filter(self):
        # --check --mood-filter against a recount from T's own bytes: each
        # track's word by moodfilter, the albums by the device's rule.
        self.add(seed=2)
        rows = raw_records(self.tags())[0]
        words = {rel: mf.word_of_tail(rows[rel][72:80]) for rel in self.tracks}
        albums = {}
        for rel in self.tracks:
            p = rel.split("/")
            albums.setdefault("/".join(p[:2]) if len(p) >= 3 else p[0] + "/" if len(p) == 2 else "", []).append(rel)
        partial = 0
        for q in (0x127, 0x001, 0x003, 0x020, 0x215, 0x40A, 0x100, 0x200):
            # Q in hex as the device's gm<hex> reads it ("127" is 0x127), a
            # 0x prefix allowed: the same answer.
            outs = []
            for arg in ("0x%X" % q, "%x" % q, "%03X" % q):
                out = io.StringIO()
                with contextlib.redirect_stdout(out):
                    self.assertEqual(st.main(["--check", self.card, "--mood-filter", arg]), 0)
                outs.append(out.getvalue())
            self.assertEqual(outs[1:], outs[:1] * 2, q)
            said = outs[0].splitlines()
            self.assertTrue(any(s.startswith("filter 0x%03X: " % q) for s in said), q)
            fit = [rel for rel in self.tracks if mf.matches(words[rel], q)]

            def fits(members):
                if not mf.active(q):
                    return True
                analysed = [words[rel] for rel in members if words[rel] & 0x8000]
                return 2 * sum(mf.matches(w, q) for w in analysed) > len(analysed)
            album_fit = sorted((a for a in albums if fits(albums[a])), key=lambda a: a.encode("utf-8"))
            partial += 0 < len(album_fit) < len(albums)
            self.assertIn("tracks that fit: %d of %d (%d analysed)" % (
                len(fit), len(self.tracks), sum(1 for w in words.values() if w)), said, q)
            self.assertIn("albums that fit: %d of %d (%d with an analysed track)" % (
                len(album_fit), len(albums), sum(1 for a in albums.values() if any(words[r] for r in a))), said, q)
            self.assertEqual([s.split(": ", 1)[1] for s in said if s.startswith("  track ")], fit, q)
            listed = [s.split(": ", 1)[1] for s in said if s.startswith("  album ")]
            self.assertEqual([a.replace(" (loose tracks)", "") for a in listed],
                             [a or "/" for a in album_fit], q)
        self.assertGreater(partial, 2, "filters that some albums fit and some don't")
        # Not a filter (voice 3, level 3, bit 11), or not 1-4 hex digits; or
        # without --check.
        bad = ("0x30", "30", "600", "0x800", "800", "12345", "0x", "", "-1", "1_0", "g1", "0o7")
        for argv in [["--check", self.card, "--mood-filter", q] for q in bad] + [
                ["--add-to", self.card, "--synthetic", "--mood-filter", "0x127"]]:
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as e:
                    st.main(argv)
            self.assertEqual(e.exception.code, 2, argv)

    def test_no_mood_data(self):
        for coverage in (0.0, None):
            if coverage is None:
                self.again(moods=False)
            else:
                self.add(mood_coverage=coverage)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(st.main(["--check", self.card, "--mood-filter", "0x127"]), 0)
            self.assertIn("no mood data on this card", out.getvalue())
            self.assertNotIn("tracks that fit", out.getvalue())

    def test_tracks_the_jsonl_lacks(self):
        # A track the jsonl doesn't list: a record with no names (FROM_API,
        # known 0), so the device names it by its path; its scores all the same.
        half = {rel: j for k, (rel, j) in enumerate(sorted(self.jsonl.items())) if k % 2 == 0}
        p = os.path.join(self.tmp, "half.jsonl")
        with open(p, "w", encoding="utf-8") as fh:
            for rel, j in half.items():
                fh.write(json.dumps(dict(j, path="music/" + rel)) + "\n")
        res, said = self.add(tags_path=p)
        rows = raw_records(self.tags())[0]
        for rel, row in rows.items():
            strings, known, flags = struct.unpack_from("<I", row, 8)[0], *struct.unpack_from("<IH", row, 32)
            if rel not in half:
                self.assertEqual((strings, known, flags), (0, 0, st.FROM_API), rel)
            else:
                self.assertTrue(known, rel)
        self.assertEqual(res["moods"]["named"], len(half))
        self.assertIn("warning: %d tracks aren't in" % (len(self.tracks) - len(half)), said)

    def test_command_line(self):
        before = snapshot(self.card)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(st.main(["--add-to", self.card, "--synthetic", "--moods", "--mood-coverage", "50",
                                      "--mood-null", "5", "--dry-run", "--time", str(TIME)]), 0)
        said = out.getvalue()
        self.assertIn("moods: %d records" % len(self.tracks), said)
        self.assertIn("%d records of 80 bytes, minor 1" % len(self.tracks), said)
        self.assertFalse(os.path.exists(os.path.join(self.card, st.MSTREAM)))
        self.assertEqual(snapshot(self.card), before)
        for argv in (["--add-to", self.card, "--synthetic", "--mood-coverage", "50"],
                     ["--add-to", self.card, "--synthetic", "--mood-null", "5"],
                     ["--add-to", self.card, "--synthetic", "--moods", "--mood-null", "101"],
                     ["--add-to", self.card, "--synthetic", "--moods", "--mood-coverage", "-1"],
                     ["--check", self.card, "--moods"],
                     ["--check", self.card, "--mood-coverage", "50"],
                     ["--check", self.card, "--mood-null", "5"],
                     ["--check", self.card, "--mood-filter", "127", "--mood-null", "0"],
                     ["--mood-coverage", "50"]):
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as e:
                    st.main(argv)
            self.assertEqual(e.exception.code, 2, argv)

    def test_firmware_reads_the_root(self):
        # The firmware's cardroot::read takes the root and T with its records
        # (the probe reads the root; T's records are the builder's:
        # test_firmware_builds_the_words).
        exe, how = build_probe()
        if exe is None:
            self.skipTest("the firmware's readers on the host: %s (the Python mirror is checked instead)" % how)
        for coverage in (92.0, 0.0):
            res, _ = self.again(mood_coverage=coverage) if coverage == 0.0 else self.add()
            got = run_probe(exe, self.card, self.tracks)
            self.assertTrue(got["present"] and got["autoDj"]["present"], got)
            self.assertEqual((got["why"], got["tagsRecords"], got["tagsPath"]),
                             ("ok", len(self.tracks), "/.mstream/tags-00000001.bin"))
            self.assertEqual(got["autoDj"]["mapped"], res["stats"]["rows"])

    def test_firmware_builds_the_words(self):
        # The firmware's own build of the library from T (LibraryBuilder, the
        # boot's after a commit): every track's word in the index is
        # moodfilter's of the tail T holds; gm's figures and gm<q>'s passes
        # (lib/core/TagText's, the console's own) are --check's and
        # --check --mood-filter's. The control card: T's records taken, no
        # block.
        exe, how = build_probe()
        if exe is None:
            self.skipTest("the firmware's build on the host: %s (the Python mirror is checked instead)" % how)
        n = len(self.tracks)
        filters = (0x127, 0x001, 0x003, 0x020, 0x215, 0x40A, 0x200)  # 0x200: Strict alone, filters nothing

        def check_fits(q):
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(st.main(["--check", self.card, "--mood-filter", "0x%X" % q]), 0)
            return out.getvalue().splitlines()

        def album_line(folder, passed, of, loose):
            # As --check prints an album (album_of's "Artist/" for an
            # artist's loose tracks, "" for the root's).
            return "  album %d of %d: %s" % (passed, of, (folder + "/" if folder else "/") + " (loose tracks)"
                                             if loose else folder)

        res, said = self.add(seed=5)
        rows = raw_records(self.tags())[0]
        lib = run_probe(exe, self.card, self.tracks, filters)["library"]
        self.assertEqual((lib["built"], lib["transferUsed"], lib["transferWhy"], lib["fromTransfer"], lib["fromPath"],
                          lib["tracks"]), (True, True, "ok", n, 0, n), lib)
        words = [mf.word_of_tail(rows[rel][72:80]) for rel in self.tracks]
        self.assertEqual(lib["words"], words)  # every track found, its word its tail's
        self.assertEqual(dict(zip(self.tracks, lib["words"])), res["check"]["moods"]["words"])
        with_word = sum(1 for w in words if w)
        self.assertTrue(0 < with_word < n, with_word)
        self.assertEqual((lib["hasMoods"], lib["moodTracks"], lib["withMoods"], lib["blockBytes"]),
                         (True, with_word, with_word, res["check"]["moods"]["blockBytes"]))
        # gm's line is --check's, its numbers ungrouped.
        self.assertIn("moods: %s; " % re.sub(r"(?<=\d),(?=\d{3})", "", lib["gm"]), said)
        partial = 0
        for f in lib["filters"]:
            q, lines = f["q"], check_fits(f["q"])
            self.assertEqual(f["tracks"], n, q)
            self.assertIn("tracks that fit: %d of %d (%d analysed)" % (f["fit"], n, f["analysed"]), lines, q)
            self.assertIn("albums that fit: %d of %d (%d with an analysed track)" % (
                f["albumsFit"], f["albums"], f["albumsAnalysed"]), lines, q)
            # Each album with its pass and analysed counts (gm lists them A-Z, --check in byte order).
            self.assertEqual(sorted(album_line(*a) for a in f["fitAlbums"]),
                             sorted(s for s in lines if s.startswith("  album ")), q)
            partial += 0 < f["fit"] < n and 0 < f["albumsFit"] < f["albums"]
        self.assertGreater(partial, 1, "filters that some tracks and albums fit and some don't")
        inactive = lib["filters"][-1]
        self.assertEqual((inactive["fit"], inactive["albumsFit"]), (n, inactive["albums"]))
        gm127 = lib["filters"][0]

        # The control card: T's records at stride 72, taken; no word, no block.
        self.again(mood_coverage=0.0)
        lib = run_probe(exe, self.card, self.tracks, filters)["library"]
        self.assertEqual((lib["built"], lib["transferUsed"], lib["fromTransfer"], lib["fromPath"], lib["tracks"]),
                         (True, True, n, 0, n), lib)
        self.assertEqual((lib["hasMoods"], lib["moodTracks"], lib["withMoods"], lib["blockBytes"]), (False, 0, 0, 0))
        self.assertEqual(lib["words"], [0] * n)
        self.assertEqual(lib["gm"], "no mood data: none of %d tracks has a word, block 0 B" % n)
        for f in lib["filters"]:
            active = mf.active(f["q"])
            self.assertEqual((f["fit"], f["analysed"], f["albumsFit"], f["albumsAnalysed"]),
                             (0 if active else n, 0, 0 if active else f["albums"], 0), f["q"])
        self.assertIn("no mood data on this card: the device lists no filter (T's records have no word)",
                      check_fits(0x127))
        print("\n  the firmware's build (%s): %d of %d tracks with a word, each its tail's; gm127 %d tracks, %d of %d "
              "albums, as --check --mood-filter 0x127; the control card no block" % (
                  how, with_word, n, gm127["fit"], gm127["albumsFit"], gm127["albums"]), file=sys.stderr)


# ---------------------------------------------------------------------------
# --check's rule 1 and --mood-filter on a hand-made card: T written here
# record by record, every expectation by hand from the rules (MOOD-FILTERS.md
# 4.1, 5.4; METADATA 2.9; lib/core/LibraryIndex.h's albums)
# ---------------------------------------------------------------------------
# Scores in MOOD_COLUMNS order (danceable, happy, sad, relaxed, aggressive,
# party, acoustic, instrumental): one valence head at 0.8, every other head
# under 0.3 (bucket 0).
SAD = [0.1, 0.1, 0.8, 0.2, 0.1, 0.1, 0.1, 0.1]
HAPPY = [0.1, 0.8, 0.1, 0.2, 0.1, 0.1, 0.1, 0.1]
# Their words (5.4): analysed (bit 15) and valence (bits 13-11) side << 2 |
# bucket, sad's side 1, bucket 3 for 80; every other field 0.
SAD_WORD, HAPPY_WORD = 0x8000 | (1 << 2 | 3) << 11, 0x8000 | 3 << 11


class MoodRulesTests(unittest.TestCase):
    # (path below /music, its record's scores, its record's flags).
    TREE = (
        ("Deep/Album/02 Happy.mp3", HAPPY, st.FROM_API),
        ("Deep/Album/03 Sad.mp3", SAD, st.FROM_API),
        ("Deep/Album/CD1/Bonus/01 Sad.mp3", SAD, st.FROM_API),  # four folders deep: still Deep/Album's
        ("Loose/01 Sad.mp3", SAD, st.FROM_API),                  # the artist's loose tracks, an album
        ("Loose/Album/01 Happy.mp3", HAPPY, st.FROM_API),
        ("Odd/Album/01 Happy.mp3", HAPPY, st.UNREADABLE),        # without FROM_API: rule 2's names, no word
        ("Odd/Album/02 Sad.mp3", SAD, st.UNREADABLE | st.FROM_API),  # with it: rule 1's, its word
        ("Root Sad.mp3", SAD, st.FROM_API),                      # the root's loose tracks
        ("Unrated/Album/01.mp3", None, st.FROM_API),             # no server row: word 0
        ("Unrated/Album/02.mp3", None, st.FROM_API),
    )
    WORDS = {"Deep/Album/02 Happy.mp3": HAPPY_WORD, "Deep/Album/03 Sad.mp3": SAD_WORD,
             "Deep/Album/CD1/Bonus/01 Sad.mp3": SAD_WORD, "Loose/01 Sad.mp3": SAD_WORD,
             "Loose/Album/01 Happy.mp3": HAPPY_WORD, "Odd/Album/01 Happy.mp3": 0, "Odd/Album/02 Sad.mp3": SAD_WORD,
             "Root Sad.mp3": SAD_WORD, "Unrated/Album/01.mp3": 0, "Unrated/Album/02.mp3": 0}

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="synthtransfer_rules_")
        cls.card = os.path.join(cls.tmp, "card")
        recs = []
        for i, (rel, scores, flags) in enumerate(cls.TREE):
            p = os.path.join(cls.card, "music", *rel.split("/"))
            os.makedirs(os.path.dirname(p), exist_ok=True)
            data = rel.encode("utf-8") * (40 + i)
            Path(p).write_bytes(data)
            recs.append({"path": rel, "flags": flags, "size": len(data), "fatTime": fat_fields(os.stat(p).st_mtime),
                         "qfp": cf.qfp(data), "scores": scores})
        tags = st.tags_file(1, 77, recs)
        ms = Path(cls.card, st.MSTREAM)
        ms.mkdir()
        (ms / "tags-00000001.bin").write_bytes(tags)
        (ms / "manifest.bin").write_bytes(st.root_file(1, 77, 0, TIME, tags, None))
        cls.tags = tags

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def fits(self, q):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(st.main(["--check", self.card, "--mood-filter", q]), 1)  # no AutoDJ index
        said = out.getvalue().splitlines()
        return said[next(i for i, s in enumerate(said) if s.startswith("filter 0x")):]

    def test_words(self):
        # Every record is rule 1's (its size, time and qfp the file's); the
        # word is its tail's, but none for the record UNREADABLE without
        # FROM_API though its tail reads (METADATA 2.9).
        rows = raw_records(self.tags)[0]
        self.assertTrue(mf.tail_valid(rows["Odd/Album/01 Happy.mp3"][72:80]))
        m = st.check_card(self.card)["moods"]
        self.assertEqual((m["records"], m["tracks"], m["byTime"], m["byQfp"], m["stale"], m["noRecord"]),
                         (10, 10, 10, 0, 0, 0))
        self.assertEqual(m["words"], self.WORDS)
        self.assertEqual((m["withWord"], m["blockBytes"]), (7, 20))

    def test_sad(self):
        # Sad at Standard. Albums: the root's loose track 1 of 1; Deep/Album
        # 2 of its 3 (the four-deep track in it: alone it would be 1 of 2);
        # Loose's loose track 1 of 1, apart from Loose/Album (0 of 1);
        # Odd/Album 1 of 1, its UNREADABLE track not analysed (with a word it
        # would be 1 of 2); Unrated/Album none analysed, so it never fits and
        # isn't counted with an analysed track.
        want = ["filter 0x003: valence sad at Standard (need 2, under 2)",
                "tracks that fit: 5 of 10 (7 analysed)",
                "albums that fit: 4 of 6 (5 with an analysed track)",
                "  album 1 of 1: / (loose tracks)",
                "  album 2 of 3: Deep/Album",
                "  album 1 of 1: Loose/ (loose tracks)",
                "  album 1 of 1: Odd/Album",
                "  track 0xB800: Deep/Album/03 Sad.mp3",
                "  track 0xB800: Deep/Album/CD1/Bonus/01 Sad.mp3",
                "  track 0xB800: Loose/01 Sad.mp3",
                "  track 0xB800: Odd/Album/02 Sad.mp3",
                "  track 0xB800: Root Sad.mp3"]
        for q in ("3", "003", "0x3", "0X003"):
            self.assertEqual(self.fits(q), want, q)

    def test_happy(self):
        # Happy at Standard: Deep/Album 1 of 3 doesn't fit, Loose/Album 1 of
        # 1 does; the UNREADABLE track's happy tail gives no word, so Odd/Album
        # is 0 of 1.
        self.assertEqual(self.fits("1"), [
            "filter 0x001: valence happy at Standard (need 2, under 2)",
            "tracks that fit: 2 of 10 (7 analysed)",
            "albums that fit: 1 of 6 (5 with an analysed track)",
            "  album 1 of 1: Loose/Album",
            "  track 0x9800: Deep/Album/02 Happy.mp3",
            "  track 0x9800: Loose/Album/01 Happy.mp3"])

    def test_no_group(self):
        # A level alone filters nothing: every track and every album, the
        # unanalysed ones too (4.1).
        said = self.fits("200")
        self.assertEqual(said[:3], ["filter 0x200: no group at Strict (need 3, under 1): filters nothing",
                                    "tracks that fit: 10 of 10 (7 analysed)",
                                    "albums that fit: 6 of 6 (5 with an analysed track)"])
        self.assertIn("  album 0 of 0: Unrated/Album", said)
        self.assertIn("  album 3 of 3: Deep/Album", said)

    def test_firmware_agrees(self):
        # The firmware's own build of this card (LibraryBuilder) and gm<q>'s
        # passes give the same words and albums: the record UNREADABLE
        # without FROM_API named from its path, the rest from T.
        exe, how = build_probe()
        if exe is None:
            self.skipTest("the firmware's build on the host: %s" % how)
        tracks = sorted(self.WORDS, key=lambda r: r.encode("utf-8"))
        lib = run_probe(exe, self.card, tracks, [0x003, 0x001, 0x200])["library"]
        self.assertEqual((lib["transferUsed"], lib["fromTransfer"], lib["fromPath"], lib["withMoods"],
                          lib["blockBytes"]), (True, 9, 1, 7, 20))
        self.assertEqual(lib["words"], [self.WORDS[rel] for rel in tracks])
        got = {f["q"]: (f["fit"], f["analysed"], f["albumsFit"], f["albums"], f["albumsAnalysed"],
                        sorted(map(tuple, f["fitAlbums"]))) for f in lib["filters"]}
        self.assertEqual(got[0x003], (5, 7, 4, 6, 5, [("", 1, 1, True), ("Deep/Album", 2, 3, False),
                                                      ("Loose", 1, 1, True), ("Odd/Album", 1, 1, False)]))
        self.assertEqual(got[0x001], (2, 7, 1, 6, 5, [("Loose/Album", 1, 1, False)]))
        self.assertEqual(got[0x200][:5], (10, 7, 6, 6, 5))


# ---------------------------------------------------------------------------
# Names as stored (METADATA 2.8.5): a host that lists NFD
# ---------------------------------------------------------------------------
def utf8(s):
    return s.encode("utf-8")


class CardNamesTests(unittest.TestCase):
    # As a Mac lists a FAT card's names: decomposed. The card stores NFC.
    NFD = ("Café Tacvba/Ré/01 Canción.mp3", "Café Tacvba/Ré/02 Plain.mp3",
           "Plain/Album/01 Song.mp3", "Sigur Rós/Takk/01 Glósóli.flac")
    NFC = ["Café Tacvba/Ré/01 Canción.mp3", "Café Tacvba/Ré/02 Plain.mp3",
           "Plain/Album/01 Song.mp3", "Sigur Rós/Takk/01 Glósóli.flac"]

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="synthtransfer_nfd_")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.card = os.path.join(self.tmp, "card")
        for i, rel in enumerate(self.NFD):
            p = os.path.join(self.card, "music", *rel.split("/"))
            os.makedirs(os.path.dirname(p), exist_ok=True)
            Path(p).write_bytes(bytes([i + 1]) * (5000 + 100 * i))
        if "Café Tacvba" not in os.listdir(os.path.join(self.card, "music")):
            self.skipTest("this file system doesn't list an NFD name as it was made")
        # The jsonl holds the card's spelling, as synthcard writes it.
        with open(os.path.join(self.card, st.TAGS_JSONL), "w", encoding="utf-8") as fh:
            for i, rel in enumerate(self.NFC):
                fh.write(json.dumps({"path": "music/" + rel, "kind": rel.rsplit(".", 1)[1], "ms": 1000 * (i + 1),
                                     "pic": None, "tags": {"title": "Song %d" % i, "artist": [rel.split("/")[0]]}})
                         + "\n")

    def test_walk(self):
        self.assertEqual(self.NFC, [unicodedata.normalize("NFC", rel) for rel in self.NFD])
        hosts = {}
        self.assertEqual(st.walk_music(self.card, hosts, nfc=True), (self.NFC, self.NFC))
        self.assertEqual(hosts, {c: d for c, d in zip(self.NFC, self.NFD) if c != d})
        self.assertEqual(len(hosts), 3)
        # Windows and Linux list the stored name unchanged: taken as listed.
        hosts = {}
        nfd = sorted(self.NFD, key=utf8)
        self.assertEqual(st.walk_music(self.card, hosts, nfc=False), (nfd, nfd))
        self.assertEqual(hosts, {})
        for lists_nfd, want in ((True, self.NFC), (False, nfd)):
            with mock.patch.object(st, "LISTS_NFD", lists_nfd):
                self.assertEqual(st.walk_music(self.card)[0], want)
        self.assertEqual(st.LISTS_NFD, sys.platform == "darwin")

    def test_add_to_and_check(self):
        # On a host that lists NFD: T's record paths, the path hashes of the
        # index and of the waveform index, and the jsonl's lookup are the
        # card's NFC; each file is opened (stat, size, time, qfp) by the name
        # the host listed.
        real, opened = st.track_file, []

        def spy(card, rel, hosts=None):
            p = real(card, rel, hosts)
            opened.append(p)
            return p
        out = io.StringIO()
        with mock.patch.object(st, "LISTS_NFD", True), mock.patch.object(st, "track_file", spy):
            res = st.add_to(self.card, synthetic=True, waves=True, moods=True, commit_time=TIME, out=out,
                            unindexed=0.0, unshaped=0.0, mood_coverage=100.0, mood_null=0.0)
            again = st.check_card(self.card)
        listed = {synthcard.long_path(os.path.join(self.card, "music", *rel.split("/"))) for rel in self.NFD}
        self.assertEqual(set(opened), listed)
        ms = Path(self.card, st.MSTREAM)
        t = (ms / "tags-00000001.bin").read_bytes()
        rows = raw_records(t)[0]
        self.assertEqual(sorted(rows, key=utf8), self.NFC)
        strs = bytes(cf.section(cf.parse_container(t), "STRS")[4])
        self.assertEqual([run_values(strs, rows[rel])[0] for rel in self.NFC], ["Song %d" % i for i in range(4)])
        self.assertEqual((res["moods"]["named"], res["stats"]["tagged"], res["waves"]["timed"]), (4, 4, 4))
        self.assertNotIn("aren't in", out.getvalue())
        dj = mpdj.Reader((ms / "autodj-00000001.bin").read_bytes())
        self.assertEqual({h for h, _ in dj.paths()}, {mpdj.path_hash(rel) for rel in self.NFC})
        wv = mpwf.Reader((ms / "waves-00000001.bin").read_bytes())
        self.assertEqual([wv.find(mpwf.path_hash(rel)).read for rel in self.NFC], [mpwf.HIT] * 4)
        for c in (res["check"], again):
            self.assertEqual((c["moods"]["byTime"], c["moods"]["noRecord"], c["moods"]["withWord"],
                              c["autoDj"]["mapped"], c["waves"]["hits"]), (4, 0, 4, 4, 4))

    def test_two_names_one_card_path(self):
        # Two names that differ by their normalisation alone are one card
        # path: refused. (A file system that keeps only one can't hold them.)
        d = os.path.join(self.card, "music", "Plain", "Album")
        Path(d, "José.mp3").write_bytes(b"x")
        Path(d, "José.mp3").write_bytes(b"y")
        if sum(n.startswith("Jos") for n in os.listdir(d)) < 2:
            self.skipTest("this file system keeps one of two names that differ by normalisation alone")
        with self.assertRaises(st.Refused):
            st.walk_music(self.card, nfc=True)
        self.assertEqual(len(st.walk_music(self.card, nfc=False)[0]), 6)


# ---------------------------------------------------------------------------
# The firmware's readers on the host: cardroot::read (the boot), then Auto
# DJ's open (mpdj::Table::open, check, msmf::companionMatches) and
# Table::rowOf for each track, the waves job's (mpwf::Table::open, check,
# msmf::companionMatches) and Table::find for each track, over a read-only
# file system on the folder; asked (`library`), the boot's build of the
# library from T (LibraryBuilder, as LibraryUpdate's boot after a commit
# makes it), each track's mood word in the index, and gm<q>'s two passes
# (tagtext::fittingTracks, fittingAlbums, albumFits: the console's own).
# ---------------------------------------------------------------------------
PROBE = r"""// synthtransfer's probe (tools/test_synthtransfer.py): the card in the
// current folder read by lib/core's readers. One track path (relative to
// /music) per line on stdin; one JSON line out. `probe library [q ...]`
// also builds the library from T and reads its mood words, and each hex q's
// tracks and albums.
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "CardAutoDj.h"
#include "CardManifest.h"
#include "CardRoot.h"
#include "CardWaves.h"
#include "LibraryBuilder.h"
#include "LibraryIndex.h"
#include "MoodFilter.h"
#include "TagText.h"

namespace cc = cardcontract;

namespace {

class DirFile final : public tagstore::File {
public:
  DirFile(FILE* f, uint32_t size) : f_(f), size_(size) {}
  ~DirFile() override { std::fclose(f_); }
  uint32_t size() const override { return size_; }
  bool read(uint32_t offset, void* out, uint32_t n) override {
    if (offset > size_ || n > size_ - offset) return false;
    if (std::fseek(f_, static_cast<long>(offset), SEEK_SET) != 0) return false;
    return std::fread(out, 1, n, f_) == n;
  }
  bool write(uint32_t, const void*, uint32_t) override { return false; }
  bool sync() override { return false; }
  bool truncate(uint32_t) override { return false; }

private:
  FILE* f_;
  uint32_t size_;
};

// Read only: nothing here can change the card.
class DirFs final : public tagstore::Fs {
public:
  tagstore::File* open(const char* path, Mode mode) override {
    if (mode != Mode::Read) return nullptr;
    FILE* f = std::fopen((std::string(".") + path).c_str(), "rb");
    if (!f) return nullptr;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    return new DirFile(f, static_cast<uint32_t>(n));
  }
  bool close(tagstore::File* f) override {
    delete f;
    return true;
  }
  bool exists(const char* path) override {
    FILE* f = std::fopen((std::string(".") + path).c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
  }
  bool remove(const char*) override { return false; }
  bool rename(const char*, const char*) override { return false; }
  uint32_t firstCluster(const char*) override { return 0; }
};

// A JSON string: the bytes as they are (UTF-8) but for '"', '\' and the
// control characters.
void putString(const char* s) {
  std::putchar('"');
  for (; *s; ++s) {
    const unsigned char c = static_cast<unsigned char>(*s);
    if (c == '"' || c == '\\') {
      std::printf("\\%c", c);
    } else if (c < 0x20) {
      std::printf("\\u%04x", c);
    } else {
      std::putchar(c);
    }
  }
  std::putchar('"');
}

// The library as the boot builds it on this card (LibraryUpdate's
// buildIndex): T alone (a synthcard tree has no /.player/tags.bin), its
// paths present and its records taken as they are (no walk since its
// commit), the root's library roots. Then each stdin track's word in the
// index (-1: no such track), and for each q gm<q>'s two passes, the albums
// that fit listed by their folders (relative to /music) as gm lists them.
void library(DirFs& fs, cardroot::Root& root, const std::vector<std::string>& tracks,
             const std::vector<uint16_t>& filters) {
  std::printf(",\"library\":{");
  tagstore::File* t = root.present ? fs.open(root.tagsPath, tagstore::Fs::Mode::Read) : nullptr;
  if (!t) {
    std::printf("\"built\":false,\"open\":\"missing\"}");
    return;
  }
  static LibraryIndex index;
  LibraryBuilder builder;
  LibraryBuilder::Config bc;
  bc.transfer = t;
  bc.transferLists = true;
  bc.libraryRoots = root.rootList();
  bc.libraryRootCount = root.rootCount;
  const LibraryBuilder::Result r = builder.build(index, bc);
  fs.close(t);
  std::printf("\"built\":%s,\"noRecords\":%s,\"transferUsed\":%s,\"transferWhy\":\"%s\",\"fromTransfer\":%" PRIu32
              ",\"fromPath\":%" PRIu32 ",\"withMoods\":%" PRIu32,
              r.built ? "true" : "false", r.noRecords ? "true" : "false", r.transferUsed ? "true" : "false",
              cc::whyName(r.transferWhy), r.fromTransfer, r.fromPath, r.withMoods);
  if (!r.built) {
    std::printf("}");
    return;
  }
  const tagtext::Moods m = tagtext::countMoods(index);
  char line[200];
  tagtext::moodsText(m, line, sizeof(line));
  std::printf(",\"tracks\":%" PRIu32 ",\"hasMoods\":%s,\"moodTracks\":%" PRIu32 ",\"blockBytes\":%zu,\"gm\":", m.tracks,
              index.hasMoods() ? "true" : "false", m.withWord, m.blockBytes);
  putString(line);
  std::printf(",\"words\":[");
  std::string path;
  for (size_t i = 0; i < tracks.size(); ++i) {
    path = "/music/" + tracks[i];
    const uint32_t id = index.findTrack(path.c_str());
    std::printf("%s%ld", i ? "," : "", id == LibraryIndex::kNone ? -1L : static_cast<long>(index.moodWord(id)));
  }
  std::printf("],\"filters\":[");
  char folder[1024];
  for (size_t k = 0; k < filters.size(); ++k) {
    const moodfilter::Filter f = moodfilter::Filter::unpack(filters[k]);
    const tagtext::TrackFits tf = tagtext::fittingTracks(index, f);
    const tagtext::AlbumFits af = tagtext::fittingAlbums(index, f);
    std::printf("%s{\"q\":%u,\"tracks\":%" PRIu32 ",\"fit\":%" PRIu32 ",\"analysed\":%" PRIu32 ",\"albums\":%" PRIu32
                ",\"albumsFit\":%" PRIu32 ",\"albumsAnalysed\":%" PRIu32 ",\"fitAlbums\":[",
                k ? "," : "", static_cast<unsigned>(filters[k]), tf.tracks, tf.fit, tf.analysed, af.albums, af.fit,
                af.analysed);
    // [folder, pass, analysed, loose] per album that fits, in albumsAZ()'s order.
    const LibraryIndex::Span az = index.albumsAZ();
    bool first = true;
    for (uint32_t i = 0; i < az.count; ++i) {
      moodfilter::Tally tally;
      if (!tagtext::albumFits(index, az[i], f, &tally)) continue;
      const LibraryIndex::Album& al = index.album(az[i]);
      index.folderPath(al.folder, folder, sizeof(folder));
      const char* rel = std::strncmp(folder, "/music", 6) == 0 ? folder + 6 : folder;
      std::printf("%s[", first ? "" : ",");
      putString(*rel == '/' ? rel + 1 : rel);
      std::printf(",%" PRIu32 ",%" PRIu32 ",%s]", tally.pass, tally.analysed,
                  (al.flags & LibraryIndex::kLoose) ? "true" : "false");
      first = false;
    }
    std::printf("]}");
  }
  std::printf("]}");
}

}  // namespace

int main(int argc, char** argv) {
  const bool withLibrary = argc > 1 && std::strcmp(argv[1], "library") == 0;
  std::vector<uint16_t> filters;
  for (int i = 2; withLibrary && i < argc; ++i)
    filters.push_back(static_cast<uint16_t>(std::strtoul(argv[i], nullptr, 16)));
  std::vector<std::string> tracks;
  char line[1024];
  while (std::fgets(line, sizeof(line), stdin)) {
    size_t n = std::strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (n) tracks.emplace_back(line, n);
  }
  DirFs fs;
  static uint8_t scratch[4096];
  static cardroot::Root root;
  cardroot::read(fs, scratch, sizeof(scratch), &root);
  std::printf("{\"present\":%s,\"why\":\"%s\",\"generation\":%" PRIu32 ",\"cardId\":\"0x%016" PRIX64
              "\",\"commitId\":\"0x%016" PRIX64 "\",\"tagsRecords\":%" PRIu32 ",\"tagsPath\":\"%s\",\"plan\":%s"
              ",\"roots\":%" PRIu32,
              root.present ? "true" : "false", cc::whyName(root.why), root.identity.generation, root.identity.cardId,
              root.identity.commitId, root.tagsRecords, root.tagsPath, root.plan ? "true" : "false", root.rootCount);
  std::printf(",\"autoDj\":{\"present\":%s", root.autoDj.present ? "true" : "false");
  if (root.autoDj.present) {
    std::printf(",\"path\":\"%s\"", root.autoDj.path);
    tagstore::File* f = fs.open(root.autoDj.path, tagstore::Fs::Mode::Read);
    if (!f) {
      std::printf(",\"open\":\"missing\"");
    } else {
      cc::mpdj::Table t;
      const cc::Why o = t.open(*f);
      std::printf(",\"open\":\"%s\"", cc::whyName(o));
      if (o == cc::Why::Ok) {
        const cc::Why dev = t.check(cc::mpdj::Table::kUseDevice, scratch, sizeof(scratch));
        const cc::Why all = t.check(cc::mpdj::Table::kUseAll, scratch, sizeof(scratch));
        const bool match = cc::msmf::companionMatches(root.autoDj.entry, t.frame(), 0, t.selectionSig());
        std::printf(",\"checkDevice\":\"%s\",\"checkAll\":\"%s\",\"matches\":%s,\"rows\":%" PRIu32
                    ",\"paths\":%" PRIu32 ",\"k\":%" PRIu32 ",\"generation\":%" PRIu32 ",\"tagsGeneration\":%" PRIu32,
                    cc::whyName(dev), cc::whyName(all), match ? "true" : "false", t.rowCount(), t.pathCount(), t.k(),
                    t.frame().generation, t.tagsGeneration());
        uint32_t mapped = 0, unmapped = 0;
        for (const std::string& path : tracks) {
          if (t.rowOf(cc::pathHash(path.data(), path.size())) != cc::mpdj::kNoRow) {
            ++mapped;
          } else {
            ++unmapped;
          }
        }
        std::printf(",\"mapped\":%" PRIu32 ",\"unmapped\":%" PRIu32, mapped, unmapped);
      }
      fs.close(f);
    }
  }
  std::printf("},\"waves\":{\"present\":%s", root.waves.present ? "true" : "false");
  if (root.waves.present) {
    std::printf(",\"path\":\"%s\"", root.waves.path);
    tagstore::File* f = fs.open(root.waves.path, tagstore::Fs::Mode::Read);
    if (!f) {
      std::printf(",\"open\":\"missing\"");
    } else {
      cc::mpwf::Table t;
      const cc::Why o = t.open(*f);
      std::printf(",\"open\":\"%s\"", cc::whyName(o));
      if (o == cc::Why::Ok) {
        const cc::Why all = t.check(scratch, sizeof(scratch));
        const bool match = cc::msmf::companionMatches(root.waves.entry, t.frame(), 0, nullptr);
        std::printf(",\"checkAll\":\"%s\",\"matches\":%s,\"rows\":%" PRIu32 ",\"generation\":%" PRIu32,
                    cc::whyName(all), match ? "true" : "false", t.rows(), t.frame().generation);
        uint32_t hits = 0, misses = 0, other = 0, reads = 0;
        cc::mpwf::Slot slot;
        for (const std::string& path : tracks) {
          const cc::mpwf::Lookup l = t.find(cc::pathHash(path.data(), path.size()), &slot);
          reads += t.reads();
          if (l == cc::mpwf::Lookup::Hit) {
            ++hits;
          } else if (l == cc::mpwf::Lookup::Miss) {
            ++misses;
          } else {
            ++other;
          }
        }
        std::printf(",\"hits\":%" PRIu32 ",\"misses\":%" PRIu32 ",\"other\":%" PRIu32 ",\"reads\":%" PRIu32, hits,
                    misses, other, reads);
      }
      fs.close(f);
    }
  }
  std::printf("}");
  if (withLibrary) library(fs, root, tracks, filters);
  std::printf("}\n");
  return 0;
}
"""

_PROBE = {}


def build_probe():
    """(exe, how) or (None, why)."""
    if "exe" in _PROBE:
        return _PROBE["exe"], _PROBE["how"]
    gxx = shutil.which("g++")
    if not gxx:
        _PROBE.update(exe=None, how="no g++ on PATH")
        return None, _PROBE["how"]
    work = os.path.join(tempfile.gettempdir(), "synthtransfer_probe")
    os.makedirs(work, exist_ok=True)
    entry = os.path.join(work, "probe.cpp")
    Path(entry).write_text(PROBE, encoding="utf-8")
    srcs = include_closure(entry)
    h = hashlib.sha256(PROBE.encode("utf-8"))
    # lib/core's headers too: a change to one alone (LibraryIndex.h's
    # inline accessors) builds the probe again.
    for s in srcs + sorted(str(x) for x in CORE.glob("*.h")):
        h.update(Path(s).read_bytes())
    exe = os.path.join(work, "synthtransfer_probe_%s.exe" % h.hexdigest()[:12])
    if not os.path.exists(exe):
        cmd = [gxx, "-std=gnu++17", "-O1", "-w", "-I", str(CORE), "-I", str(ROOT / "src"), entry] + srcs + ["-o", exe]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            _PROBE.update(exe=None, how="g++ failed: %s" % r.stderr.strip().splitlines()[:3])
            return None, _PROBE["how"]
    _PROBE.update(exe=exe, how="g++, lib/core's CardRoot, CardManifest, CardAutoDj, CardWaves, LibraryBuilder and "
                  "TagText")
    return exe, _PROBE["how"]


def run_probe(exe, card, tracks, filters=None):
    """The probe's JSON; with `filters` (a list of q, maybe empty) its
    "library" too: the build from T, the words, each q's passes."""
    argv = [exe] if filters is None else [exe, "library"] + ["%x" % q for q in filters]
    r = subprocess.run(argv, input=("\n".join(tracks) + "\n").encode("utf-8"), capture_output=True, cwd=card,
                       timeout=300)
    if r.returncode != 0:
        raise AssertionError("the probe failed: %r" % r.stderr[:500])
    return json.loads(r.stdout.decode("utf-8"))


if __name__ == "__main__":
    unittest.main()
