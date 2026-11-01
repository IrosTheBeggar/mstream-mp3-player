# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Tests for tools/mpdj.py, the AutoDJ index's reference writer and reader
(docs/AUTODJ-INDEX.md), against the fixtures in test/fixtures/card.

    python -m unittest discover -s tools -p "test_mpdj.py"

Every golden file is rebuilt byte for byte from its description, its lists
are recomputed by a second, plainer reading of the rule (sorted() over
every candidate, no counting sort), and every MPDJ hardening file must read
as index.json says, for the reason it says. The fast path (numpy) is tested
against the rule when numpy is installed, and skipped otherwise.
"""
import contextlib
import copy
import io
import json
import random
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mpdj  # noqa: E402

FIX = Path(__file__).resolve().parent.parent / "test" / "fixtures" / "card"
LIB = FIX / "libraries"


def desc(name):
    return json.loads((LIB / name).read_text(encoding="utf-8"))


def golden(name):
    return (FIX / "golden" / name).read_bytes()


def mpdj_descs():
    return [(p.name, json.loads(p.read_text(encoding="utf-8"))) for p in sorted(LIB.glob("*.json"))
            if json.loads(p.read_text(encoding="utf-8")).get("format") == "MPDJ"]


def vectors():
    return json.loads((FIX / "vectors.json").read_text(encoding="utf-8"))["mpdj"]


def plain_lists(d):
    """The rule read a second way: every candidate scored, sorted() by
    (level desc, index asc), then the selection; keys from the files."""
    p = mpdj.prepare(d, base=LIB)
    n = len(p.hashes)
    out = []
    for a in range(n):
        cand = []
        for b in range(n):
            if b == a or (p.keys[a] is not None and p.keys[a] == p.keys[b]):
                continue
            s = 0.0
            for x, y in zip(p.emb[a], p.emb[b]):
                s += x * y
            cand.append((-mpdj.level(s), b))
        cand.sort()
        cand = [(b, -lv) for lv, b in cand]
        m = sum(1 for _, lv in cand if lv >= p.floor)
        if p.kind == mpdj.SEL_STRIDED and m > p.k:
            ranks = [j if j < p.head else p.head + (j - p.head) * (m - 1 - p.head) // (p.k - 1 - p.head)
                     for j in range(p.k)]
            out.append([cand[r] for r in ranks])
        else:
            out.append(cand[:p.k])
    return out


class Primitives(unittest.TestCase):
    def test_primitives_match_vectors(self):
        v = vectors()
        for e in v["levels"]:
            self.assertEqual(mpdj.level(e["cos"]), e["level"], e)
        for e in v["floors"]["cases"]:
            self.assertEqual(mpdj.floor_level(e["percent"]), e["level"], e)
            self.assertGreaterEqual(e["level"] * 100, 255 * e["percent"])          # L >= t x 255
            self.assertLess((e["level"] - 1) * 100, 255 * e["percent"])            # and the least such L
        for e in v["slotBytes"]:
            self.assertEqual(mpdj.slot_bytes_for(e["k"]), e["slotBytes"], e)
        for e in v["headerBytes"]:
            self.assertEqual(mpdj.header_bytes_for(e["sectionCount"], e["slotBytes"], e["minimum"]),
                             e["headerBytes"], e)
        for e in v["stridedRanks"]:
            ranks = list(range(e["k"])) if e["m"] <= e["k"] else mpdj.strided_ranks(e["k"], e["head"], e["m"])
            if "ranks" in e:
                self.assertEqual(ranks, e["ranks"], e)
            for q in e.get("pairs", []):
                self.assertEqual(ranks[q["j"]], q["rank"], q)
        sc = v["slotCrc"]
        raw = mpdj.Reader(golden(sc["file"])).slot_raw(sc["row"])
        self.assertEqual(mpdj.slot_crc(raw, sc["row"]), int(sc["crc"], 16))
        self.assertEqual(mpdj.crc32(raw[:-4]), int(sc["unseeded"], 16))
        self.assertEqual(mpdj.crc32(b"123456789"), 0xCBF43926)
        sig = v["signature"]
        p = mpdj.prepare(desc(sig["file"].replace(".bin", ".json")))
        self.assertEqual(p.sig_text, sig["text"])
        self.assertEqual(p.sig.hex(), sig["sig"])
        self.assertEqual(golden(sig["file"])[64:80].hex(), sig["sig"])
        self.assertEqual(p.vec_sha256, v["vec"]["sha256"])
        for e in v["deviceTxt"]["cases"]:
            text = None if e.get("noFile") else "contract=1\n" if e.get("keyMissing") else (
                "contract=1\nread.mpdj=%s\n" % e["readMpdj"])
            self.assertEqual(mpdj.writes_v2(text), e["writesV2"], e)
        t = v["trap"]
        self.assertEqual((mpdj.level(t["cos"]), mpdj.level(t["cosF32"])), (t["level"], t["levelF32"]))

    def test_level_rounding(self):
        self.assertEqual(mpdj.level(0.5), 128)                       # 127.5: half away from zero
        self.assertEqual(mpdj.level(127.49999999999999 / 255), 127)
        self.assertEqual(mpdj.level(0.5 / 255), 1)
        self.assertEqual(mpdj.round_half_away(0.49999999999999994), 0)   # floor(x + 0.5) gives 1
        self.assertEqual(mpdj.round_half_away(-2.5), -3)
        self.assertEqual(mpdj.round_half_away(2.5), 3)                   # Python's round() gives 2
        self.assertEqual(mpdj.level(-0.001), 0)
        self.assertEqual(mpdj.level(-1.0), 0)
        self.assertEqual(mpdj.level(1.01), 255)
        self.assertEqual(mpdj.level(254.5 / 255), 255)

    def test_name_keys(self):
        self.assertEqual(mpdj.name_key("  Generated  Artist\t03 "), "generated artist 03")
        self.assertEqual(mpdj.name_key("“A” – B’s"), '"a" - b\'s')
        self.assertEqual(mpdj.name_key("ΣΟΦΙΑΣ"), "σοφιας")
        self.assertEqual(mpdj.artist_display([" Generated Artist 04", "Generated Artist 05", "generated  artist 04",
                                              "  "]), "Generated Artist 04, Generated Artist 05")
        self.assertEqual(mpdj.artist_display([" One "]), "One")
        self.assertEqual(mpdj.artist_key(["  ", "　", "X"]), mpdj.artist_key(["x"]))  # the first non-blank
        self.assertEqual(mpdj.artist_key(["", " "]), 0)
        self.assertEqual(mpdj.artist_key([]), 0)

    def test_no_song_key(self):
        for title in (None, "", "   ", "  "):
            self.assertIsNone(mpdj.song_key(["Generated Artist 32"], title), repr(title))
        self.assertEqual(mpdj.song_key([], "Sample Café"), "|sample café")
        self.assertEqual(mpdj.same_groups(["a", None, "a", None, "b"]), [[0, 2]])
        # autodj-pair: two title-less rows of one artist list each other.
        r = mpdj.Reader(golden("autodj-pair.bin"))
        self.assertEqual([r.slot(i)[2] for i in range(2)], [[(1, 0)], [(0, 0)]])
        self.assertNotIn("same", mpdj.prepare(desc("autodj-pair.json")).sig_text)

    def test_strided_rank_properties(self):
        for k in range(3, 41):
            for h in range(1, k - 1):
                for m in range(k + 1, k + 61):
                    r = mpdj.strided_ranks(k, h, m)
                    self.assertEqual(r[:h], list(range(h)))
                    self.assertEqual(r[h], h)
                    self.assertEqual(r[-1], m - 1)
                    self.assertTrue(all(b > a for a, b in zip(r, r[1:])), (k, h, m))
        self.assertEqual(mpdj.strided_ranks(169, 84, 2258)[85], 109)
        with self.assertRaises(ValueError):
            mpdj.strided_ranks(4, 1, 4)            # m <= k: the top k, not strided

    def test_size(self):
        self.assertEqual(mpdj.size(20000), 10600646)
        self.assertEqual(mpdj.size(65535), 34734198)
        self.assertEqual(mpdj.size(20000, k=84), 5480390)
        self.assertEqual(mpdj.size(19410, strings=96), 10287912)
        self.assertEqual(mpdj.size(6, paths=7, k=4, strings=61), len(golden("autodj-00000029.bin")))
        self.assertEqual(mpdj.size(6, paths=7, k=169, strings=61), len(golden("autodj-small-default.bin")))


class Goldens(unittest.TestCase):
    def test_goldens_byte_for_byte(self):
        names = [n for n, _ in mpdj_descs()]
        self.assertEqual(len(names), 7, names)
        for name, d in mpdj_descs():
            with self.subTest(name):
                self.assertEqual(mpdj.build(d, base=LIB, fast=False), golden(d["file"]))

    def test_read_back_equals_the_rule(self):
        for name, d in mpdj_descs():
            with self.subTest(name):
                r = mpdj.Reader(golden(d["file"]))
                self.assertEqual(r.check("all"), "ok")
                got = [r.slot(i)[2] for i in range(r.rows)]
                self.assertEqual(got, plain_lists(d))
                p = mpdj.prepare(d, base=LIB)
                self.assertEqual([r.filter(i) for i in range(r.rows)],
                                 [(f[0], f[1], bool(f[2]), f[3]) for f in p.filters])
                self.assertEqual(r.strings(), [d.get(f, "") for f in mpdj.STRING_FIELDS])
                self.assertEqual((r.k, r.kind, r.head, r.floor, r.generation, r.built_time, r.tags_generation),
                                 (d["k"], d["selKind"], d.get("selHead", d["k"]), d.get("selFloor", 0),
                                  d["generation"], d["builtTime"], d["tagsGeneration"]))
                # Every card file's path hash finds its row (none collide here).
                for i, row in enumerate(p.desc_rows):
                    for f in row["files"]:
                        self.assertEqual(r.row_of(mpdj.path_hash(f["path"])), i)

    def test_primary_file(self):
        d = desc("autodj-00000029.json")
        r = mpdj.Reader(golden("autodj-00000029.bin"))
        row = next(i for i, x in enumerate(sorted(d["rows"], key=lambda x: x["hash"])) if len(x["files"]) == 2)
        self.assertEqual(r.filter(row)[0], 0)          # the NFC file's record (the smaller path hash)
        hashes = [mpdj.path_hash(f["path"]) for f in d["rows"][row]["files"]]
        self.assertLess(hashes[0], hashes[1])
        # Its records' values swapped: DJRF follows the primary file.
        d2 = copy.deepcopy(d)
        fs = d2["rows"][row]["files"]
        fs[0]["bpm10"], fs[1]["bpm10"] = 1000, 0
        self.assertEqual(mpdj.Reader(mpdj.build(d2, fast=False)).filter(row)[0], 1000)
        # The G4 copy: two files with one record, the primary by path hash.
        t = desc("autodj-tiny.json")
        self.assertEqual(t["rows"][1]["primaryPath"], min((f["path"] for f in t["rows"][1]["files"]),
                                                          key=mpdj.path_hash))


class Hardening(unittest.TestCase):
    def test_hardening_verdicts(self):
        index = json.loads((FIX / "hardening" / "index.json").read_text(encoding="utf-8"))
        entries = [e for e in index if e["format"] == "MPDJ"]
        self.assertGreaterEqual(len(entries), 50)
        for e in entries:
            with self.subTest(e["file"]):
                data = (FIX / "hardening" / e["file"]).read_bytes()
                why = mpdj.check(data, e["uses"])
                self.assertEqual(why, e["why"] if e["expect"] == "absent" else "ok")
                if "same" in e:
                    self.assertIsNone(mpdj.same_content(data, golden(e["same"])))
                if "slot" in e:
                    r = mpdj.Reader(data)
                    self.assertEqual(r.check("device"), "ok")
                    self.assertEqual(r.slot(e["slot"]["row"])[0], e["slot"]["read"])
        whys = {e["why"] for e in entries}
        for w in ("major", "headerBytes", "short", "layout", "djLayout", "enum", "djSelection", "counts", "shape",
                  "missing", "unknownRequired", "string", "stringOrder", "djPaths", "djSlotCrc", "djSlotUnused",
                  "djSlotIndex", "djSlotOrder", "ok"):
            self.assertIn(w, whys)
        # An empty DJPH reads, with the rows and their lists unchanged.
        r = mpdj.Reader((FIX / "hardening" / "mpdj-paths-empty.bin").read_bytes())
        g = mpdj.Reader(golden("autodj-00000029.bin"))
        self.assertEqual((r.path_count, r.paths(), r.sections[mpdj.DJPH].stride), (0, [], 10))
        self.assertEqual([r.slot(i) for i in range(6)], [g.slot(i) for i in range(6)])

    def test_strings_of_a_bad_file(self):
        """A file whose strings fail 8.3 reads as absent, and its strings and
        dump don't raise."""
        for name, why in (("mpdj-string-no-nul.bin", "string"), ("mpdj-string-out-of-range.bin", "string")):
            data = (FIX / "hardening" / name).read_bytes()
            self.assertEqual(mpdj.check(data), why)
            self.assertIsNone(mpdj.Reader(data).strings()[4])
            out = io.StringIO()
            mpdj.dump(data, out=out)
            self.assertIn("check (all): string", out.getvalue())

    def test_truncation_and_flips(self):
        """Every cut of autodj-00000029 is absent; every flipped bit is absent
        but in the two zero bytes before STRS (no CRC covers a gap), where
        the file reads the same."""
        g = golden("autodj-00000029.bin")
        for n in range(len(g)):
            self.assertNotEqual(mpdj.check(g[:n]), "ok", n)
        r = mpdj.Reader(g)
        ph, st = r.sections[mpdj.DJPH], r.sections[mpdj.STRS]
        gap = set(range(ph.offset + ph.bytes, st.offset))
        self.assertEqual(len(gap), 2)
        b = bytearray(g)
        for o in range(len(g)):
            for bit in range(8):
                b[o] ^= 1 << bit
                why = mpdj.check(b)
                if o in gap:
                    self.assertEqual(why, "ok", o)
                    self.assertIsNone(mpdj.same_content(bytes(b), g))
                else:
                    self.assertNotEqual(why, "ok", (o, bit))
                b[o] ^= 1 << bit


class Writer(unittest.TestCase):
    def test_f32_trap(self):
        t = vectors()["trap"]
        for name in ("autodj-mid.json", "autodj-mid-default.json"):
            with self.subTest(name):
                d = desc(name)
                p = mpdj.prepare(d, base=LIB)
                wrong = mpdj.write(p, mpdj.compute_lists(p, fast=False, f32_sum=True))
                good = golden(d["file"])
                self.assertEqual(len(wrong), len(good))
                ra, rw = mpdj.Reader(good), mpdj.Reader(wrong)
                self.assertEqual(rw.check("all"), "ok")       # wrong, but a well-formed file
                differ = [i for i in range(ra.rows) if ra.slot_raw(i) != rw.slot_raw(i)]
                self.assertTrue(differ)
                self.assertLessEqual(set(differ), {t["rowA"], t["rowB"]})
                s = ra.sections[mpdj.DJSL]
                self.assertEqual(good[s.offset + s.bytes:], wrong[s.offset + s.bytes:])

    def test_collision_rule(self):
        """A path hash held by two card files is left out of DJPH (both files);
        the rows stay, and the primary file is still the smallest hash's."""
        d = desc("autodj-00000029.json")
        real = mpdj.path_hash
        a = d["rows"][0]["files"][0]["path"]          # row 0
        b = d["rows"][3]["files"][0]["path"]          # row 3 (by hash order too)
        c1, c2 = (f["path"] for f in d["rows"][1]["files"])

        def fake(rel):
            if rel in (a, b):
                return 0x1111111111111111             # across rows
            if rel in (c1, c2):
                return 0x0000000000000005             # within one row: its smallest
            return real(rel)

        del d["rows"][1]["primaryPath"]               # the tie changes it (below)
        try:
            mpdj.path_hash = fake
            data = mpdj.build(d, fast=False)
        finally:
            mpdj.path_hash = real
        r = mpdj.Reader(data)
        self.assertEqual(r.check("all"), "ok")
        hs = [h for h, _ in r.paths()]
        self.assertNotIn(0x1111111111111111, hs)
        self.assertNotIn(5, hs)
        self.assertEqual(r.path_count, 7 - 4)
        self.assertEqual(r.rows, 6)
        self.assertEqual([r.slot(i)[2] for i in range(6)], [mpdj.Reader(golden("autodj-00000029.bin")).slot(i)[2]
                                                            for i in range(6)])
        # Row 1's two files tie on the hash: the primary is the smaller path's
        # bytes, the NFD spelling ("e", 0x65, before the NFC one's 0xC3).
        self.assertEqual(r.filter(1)[0], 1000)

    def test_no_paths_left(self):
        """Every card file's hash in a collision: pathCount 0, DJPH empty with
        its stride 10 (4.5), the rows and their lists as before."""
        d = desc("autodj-00000029.json")
        real = mpdj.path_hash
        for row in d["rows"]:
            row.pop("primaryPath")
        try:
            mpdj.path_hash = lambda rel: 0x0123456789ABCDEF
            data = mpdj.build(d, fast=False)
        finally:
            mpdj.path_hash = real
        r = mpdj.Reader(data)
        self.assertEqual(r.check("all"), "ok")
        s = r.sections[mpdj.DJPH]
        self.assertEqual((r.path_count, s.count, s.bytes, s.stride), (0, 0, 0, 10))
        self.assertEqual(r.row_of(0x0123456789ABCDEF), None)
        g = mpdj.Reader(golden("autodj-00000029.bin"))
        self.assertEqual([r.slot(i)[2] for i in range(6)], [g.slot(i)[2] for i in range(6)])

    def test_signature_with_an_empty_string(self):
        """6: each field as stored, an empty one leaving its separating space."""
        d = desc("autodj-00000029.json")
        d["modelVersion"] = ""
        p = mpdj.prepare(d)
        self.assertIn("\nmodel synthetic-embedding \nmetric cosine\n", p.sig_text)
        self.assertEqual(mpdj.Reader(mpdj.write(p, mpdj.compute_lists(p, fast=False))).strings()[1], "")

    def test_restamp(self):
        old = golden("autodj-00000029.bin")
        d = desc("autodj-00000029.json")
        d2 = copy.deepcopy(d)
        d2.update(generation=42, cardId="0x0123456789ABCDEF", tagsGeneration=42, builtTime=1799999999)
        d2["rows"][0]["files"][0]["bpm10"] = 1250                         # a retag: DJRF changes
        d2["rows"][4]["files"][0]["path"] = "Generated Artist 03/Moved/07 - SAMPLE TITLE 03.flac"
        d2["rows"][4]["primaryPath"] = d2["rows"][4]["files"][0]["path"]  # a moved file: DJPH changes
        new = mpdj.restamp(old, d2)
        fresh = mpdj.build(dict(d2, builtTime=1790000000), fast=False)
        self.assertEqual(new, fresh)
        ro, rn = mpdj.Reader(old), mpdj.Reader(new)
        self.assertEqual(ro.section_bytes(mpdj.DJSL), rn.section_bytes(mpdj.DJSL))
        self.assertEqual((rn.generation, rn.built_time, rn.tags_generation), (42, 1790000000, 42))
        for change in (lambda x: x.update(k=5), lambda x: x.update(selFloor=101),
                       lambda x: x["rows"][2]["embedding"].__setitem__(0, 0.25),
                       lambda x: x["rows"][5]["files"][0].update(title="Another Title")):
            d3 = copy.deepcopy(d)
            change(d3)
            for row in d3["rows"]:
                row.pop("songKey")
            with self.assertRaises(mpdj.WriterError):
                mpdj.restamp(old, d3)

    def test_writer_refusals(self):
        base = desc("autodj-00000029.json")

        def refused(fn):
            d = copy.deepcopy(base)
            fn(d)
            with self.assertRaises(mpdj.WriterError):
                mpdj.prepare(d)

        refused(lambda d: d.update(k=1))
        refused(lambda d: d.update(k=170))
        refused(lambda d: d.update(k=2))                   # selKind 1 needs k >= 3 (H 1 to k - 2)
        refused(lambda d: d.update(selHead=0))
        refused(lambda d: d.update(selHead=3))             # k - 1
        refused(lambda d: d.update(selFloor=0))
        refused(lambda d: d.update(selFloor=256))
        refused(lambda d: d.update(selKind=2))
        refused(lambda d: d.update(selKind=0))              # selHead 1 given: kind 0 is k / 0
        refused(lambda d: d.update(major=1))
        refused(lambda d: d["rows"][1].update(hash=d["rows"][0]["hash"]))
        refused(lambda d: d["rows"][1].update(hash="0123456789ABCDEF0123456789ABCDEF"))
        refused(lambda d: d["rows"][1].update(files=[]))
        refused(lambda d: d["rows"][0]["files"].append(dict(d["rows"][0]["files"][0])))
        refused(lambda d: d["rows"][2]["files"].append(dict(d["rows"][0]["files"][0])))   # one file, two rows
        refused(lambda d: d.update(attribution="x" * 256))
        refused(lambda d: d.update(license="CC0\0"))
        refused(lambda d: d["rows"][3].update(embedding=[0.5] * 7))
        refused(lambda d: d["rows"][3]["embedding"].__setitem__(0, float("inf")))
        refused(lambda d: d["rows"][3].update(songKey="something else"))   # a given key that disagrees
        many = {"format": "MPDJ", "generation": 1, "k": 4, "selKind": 0, "dim": 1,
                "rows": [{"hash": "%032x" % i, "files": [{"path": "%d.mp3" % i, "artists": []}], "embedding": [1.0]}
                         for i in range(65536)]}
        with self.assertRaises(mpdj.WriterError):
            mpdj.prepare(many)
        mpdj.prepare(copy.deepcopy(base))                    # and the base is fine
        d = copy.deepcopy(base)
        d.update(attribution="x" * 255)
        mpdj.prepare(d)

    def test_edge_cases(self):
        one = {"format": "MPDJ", "generation": 1, "k": 2, "selKind": 0, "dim": 1,
               "rows": [{"hash": "%032x" % 1, "files": [{"path": "a.mp3", "artists": []}], "embedding": [1.0]}]}
        r = mpdj.Reader(mpdj.build(one, fast=False))
        self.assertEqual((r.rows, r.slot(0)[2], r.head, r.floor), (1, [], 2, 0))
        # m == k and m == k + 1 at k 4, H 1, Lf 100 (G1's rows 2 and 0).
        g = mpdj.Reader(golden("autodj-00000029.bin"))
        self.assertEqual(g.slot(2)[2], [(0, 200), (5, 171), (4, 150), (3, 128)])
        self.assertEqual(g.slot(0)[2], [(2, 200), (5, 185), (4, 162), (3, 100)])


@unittest.skipUnless(mpdj.have_numpy(), "needs numpy")
class FastPath(unittest.TestCase):
    def test_fast_path_equals_rule(self):
        for name, d in mpdj_descs():
            with self.subTest(name):
                self.assertEqual(mpdj.build(d, base=LIB, fast=True), golden(d["file"]))
        # Ties on purpose: integer vectors of |v|^2 = 256, so every cosine is a
        # multiple of 1/256 and cos 0.5 lands on 127.5 (the guard band's
        # case); random keys, some blank titles; both kinds.
        rng = random.Random(7)
        vecs = []
        while len(vecs) < 300:
            v = [rng.randint(-9, 9) for _ in range(8)]
            if sum(x * x for x in v) == 256:
                vecs.append([x / 16 for x in v])
        rows = []
        for i, v in enumerate(vecs):
            title = None if i % 17 == 0 else "Sample Title %d" % (i % 40)
            artist = "Generated Artist %02d" % (i % 30)
            f = {"path": "%s/%03d.mp3" % (artist, i), "artists": [artist]}
            if title is not None:
                f["title"] = title
            rows.append({"hash": "%032x" % rng.getrandbits(128), "files": [f], "embedding": v})
        for kind, k, h, lf in ((0, 30, None, None), (1, 24, 8, 140), (1, 169, 84, 100), (1, 84, 20, 200)):
            d = {"format": "MPDJ", "generation": 3, "k": k, "selKind": kind, "dim": 8, "rows": rows}
            if kind:
                d.update(selHead=h, selFloor=lf)
            with self.subTest(kind=kind, k=k):
                self.assertEqual(mpdj.build(d, fast=True), mpdj.build(d, fast=False))


class Cli(unittest.TestCase):
    def run_cli(self, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            code = mpdj.main(list(args))
        return code, out.getvalue()

    def test_cli(self):
        with tempfile.TemporaryDirectory() as t:
            src = str(LIB / "autodj-mid.json")
            out = str(Path(t, "mid.bin"))
            self.assertEqual(self.run_cli("build", src, out, "--no-fast")[0], 0)
            self.assertEqual(Path(out).read_bytes(), golden("autodj-mid.bin"))
            code, text = self.run_cli("check", out)
            self.assertEqual((code, text.strip().endswith("present")), (0, True))
            code, text = self.run_cli("check", str(FIX / "hardening" / "mpdj-slot-crc.bin"))
            self.assertEqual(code, 1)
            self.assertIn("djSlotCrc", text)
            self.assertEqual(self.run_cli("check", str(FIX / "hardening" / "mpdj-slot-crc.bin"), "--uses",
                                          "device")[0], 0)
            code, text = self.run_cli("dump", str(FIX / "golden" / "autodj-00000029.bin"))
            self.assertIn("slot 0 crc", text)
            self.assertIn("2/200 5/185 4/162 3/100", text)
            code, text = self.run_cli("dump", str(FIX / "golden" / "autodj-00000029.bin"), "--slot", "3", "--hex")
            self.assertIn("|", text)
            code, text = self.run_cli("signature", str(LIB / "autodj-00000029.json"))
            self.assertIn("same 3 4 5\n", text)
            self.assertIn("selectionSig " + golden("autodj-00000029.bin")[64:80].hex(), text)
            d = desc("autodj-00000029.json")
            d["generation"] = 99
            Path(t, "d.json").write_text(json.dumps(d), encoding="utf-8")
            self.assertEqual(self.run_cli("restamp", str(FIX / "golden" / "autodj-00000029.bin"),
                                          str(Path(t, "d.json")), str(Path(t, "r.bin")))[0], 0)
            self.assertEqual(struct.unpack_from("<I", Path(t, "r.bin").read_bytes(), 20)[0], 99)
            code, text = self.run_cli("size", "20000")
            self.assertIn("10600646 bytes", text)
            d["k"] = 1
            Path(t, "bad.json").write_text(json.dumps(d), encoding="utf-8")
            self.assertEqual(self.run_cli("build", str(Path(t, "bad.json")), str(Path(t, "x.bin")))[0], 2)


if __name__ == "__main__":
    unittest.main()
