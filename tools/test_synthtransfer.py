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
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import card_fixtures as cf  # noqa: E402  (cleanup, private_vectors: docs/METADATA.md 2.12.7)
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
# The firmware's readers on the host: cardroot::read (the boot), then Auto
# DJ's open (mpdj::Table::open, check, msmf::companionMatches) and
# Table::rowOf for each track, the waves job's (mpwf::Table::open, check,
# msmf::companionMatches) and Table::find for each track, over a read-only
# file system on the folder.
# ---------------------------------------------------------------------------
PROBE = r"""// synthtransfer's probe (tools/test_synthtransfer.py): the card in the
// current folder read by lib/core's readers. One track path (relative to
// /music) per line on stdin; one JSON line out.
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "CardAutoDj.h"
#include "CardManifest.h"
#include "CardRoot.h"
#include "CardWaves.h"

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

}  // namespace

int main() {
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
  std::printf("}}\n");
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
    for s in srcs:
        h.update(Path(s).read_bytes())
    exe = os.path.join(work, "synthtransfer_probe_%s.exe" % h.hexdigest()[:12])
    if not os.path.exists(exe):
        cmd = [gxx, "-std=gnu++17", "-O1", "-w", "-I", str(CORE), "-I", str(ROOT / "src"), entry] + srcs + ["-o", exe]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            _PROBE.update(exe=None, how="g++ failed: %s" % r.stderr.strip().splitlines()[:3])
            return None, _PROBE["how"]
    _PROBE.update(exe=exe, how="g++, lib/core's CardRoot, CardManifest, CardAutoDj and CardWaves")
    return exe, _PROBE["how"]


def run_probe(exe, card, tracks):
    r = subprocess.run([exe], input=("\n".join(tracks) + "\n").encode("utf-8"), capture_output=True, cwd=card,
                       timeout=300)
    if r.returncode != 0:
        raise AssertionError("the probe failed: %r" % r.stderr[:500])
    return json.loads(r.stdout.decode("utf-8"))


if __name__ == "__main__":
    unittest.main()
