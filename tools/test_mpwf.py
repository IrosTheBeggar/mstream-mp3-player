# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Tests for tools/mpwf.py, the waveform index's reference writer and reader
(docs/WAVEFORM-INDEX.md), against the fixtures in test/fixtures/card.

    python -m unittest discover -s tools -p "test_mpwf.py"

Every golden file is rebuilt byte for byte from its description, its bars
are recomputed by a second, plainer reading of the rule (integers, no square
root), every lookup's reads are counted against the vectors, and every MPWF
hardening file must read as index.json says, for the reason it says.
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
import card_fixtures as cf  # noqa: E402  (a root's MPWF row, the tags file behind the fixture card)
import mpwf  # noqa: E402

FIX = Path(__file__).resolve().parent.parent / "test" / "fixtures" / "card"
LIB = FIX / "libraries"
ONE, G1, SKEW = "waves-one.bin", "waves-0000002a.bin", "waves-skew.bin"


def desc(name):
    return json.loads((LIB / name).read_text(encoding="utf-8"))


def golden(name):
    return (FIX / "golden" / name).read_bytes()


def mpwf_descs():
    return [(p.name, json.loads(p.read_text(encoding="utf-8"))) for p in sorted(LIB.glob("*.json"))
            if json.loads(p.read_text(encoding="utf-8")).get("format") == "MPWF"]


def vectors():
    return json.loads((FIX / "vectors.json").read_text(encoding="utf-8"))["mpwf"]


def index():
    return json.loads((FIX / "hardening" / "index.json").read_text(encoding="utf-8"))


def plain_bars(peaks):
    """The rule read a second way, in integers: a bar is the largest v with
    w (2v - 1)^2 <= 4 n, n the sum of squares of its w peaks; 0 when v = 1 is
    already too big. That is floor(sqrt(n / w) + 1/2), exactly."""
    m = len(peaks)
    out = []
    for i in range(240):
        start = i * m // 240
        end = (i + 1) * m // 240
        if end <= start:
            end = start + 1
        w = end - start
        n = sum(b * b for b in peaks[start:end])
        v = 0
        while v < 255 and w * (2 * v + 1) ** 2 <= 4 * n:
            v += 1
        out.append(v)
    return bytes(out)


def plain_row(hashes, h):
    """Which row holds h, by a walk from the start: the lookup's answer."""
    for r, v in enumerate(hashes):
        if v == h:
            return r
    return None


def flat_rows(hashes):
    return [(h, 0, bytes(240)) for h in sorted(hashes)]


class Primitives(unittest.TestCase):
    def test_primitives_match_vectors(self):
        v = vectors()
        self.assertEqual(mpwf.crc32(b"123456789"), 0xCBF43926)
        for e in v["names"]:
            self.assertEqual(mpwf.file_name(e["generation"]), e["name"])
        for e in v["sizes"]:
            self.assertEqual(mpwf.size(e["rows"]), e["fileBytes"])
        for g in v["barRanges"]:
            for e in g["cases"]:
                self.assertEqual(mpwf.bar_range(e["i"], g["m"]), (e["start"], e["end"]), e)
        for e in v["bars"]:
            with self.subTest(e["note"]):
                peaks = list(bytes.fromhex(e["peaks"])) if "peaks" in e else [0] * e["m"]
                bars = list(bytes.fromhex(e["bars"])) if "bars" in e else [0] * 240
                for i, b in e.get("peaksAt", {}).items():
                    peaks[int(i)] = b
                for i, b in e.get("barsAt", {}).items():
                    bars[int(i)] = b
                self.assertEqual(len(peaks), e["m"])
                self.assertEqual(list(mpwf.bars_from_peaks(peaks)), bars)
                self.assertEqual(list(plain_bars(peaks)), bars)
        s = v["slot"]
        raw = mpwf.Reader(golden(s["file"])).slot_raw(s["row"])
        self.assertEqual(raw.hex(), s["bytes"])
        self.assertEqual(mpwf.slot_crc(raw, s["row"]), int(s["crc"], 16))
        self.assertEqual(mpwf.crc32(raw[:-4]), int(s["unseeded"], 16))
        self.assertEqual(mpwf.path_hash(s["path"]), int(s["pathHash"], 16))
        sc = v["slotCrc"]
        raw = mpwf.Reader(golden(sc["file"])).slot_raw(sc["row"])
        self.assertEqual(mpwf.slot_crc(raw, sc["row"]), int(sc["crc"], 16))
        self.assertEqual(struct.unpack_from("<I", raw, 252)[0], int(sc["crc"], 16))
        self.assertEqual(mpwf.crc32(raw[:252]), int(sc["unseeded"], 16))
        for e in v["pick"]:
            self.assertEqual(mpwf.probe_row(int(e["h"], 16), e["lo"], e["hi"], int(e["loH"], 16), int(e["hiH"], 16),
                                            e["probes"]), e["row"], e["note"])
        for e in v["deviceTxt"]["cases"]:
            self.assertEqual(mpwf.writes_waves(mpwf.device_text(e)), e["writes"], e)
        files = {name: golden(name) for name in (G1, ONE)}
        for e in v["commit"]["cases"]:
            self.assertEqual(mpwf.commit_outcome(mpwf.device_text(e), files.get(e["old"]), mpwf.commit_rows(e, files)),
                             e["outcome"], e["note"])
        state, first = 0, []
        for _ in range(3):
            state, h = mpwf.splitmix64(state)
            first.append("0x%016X" % h)
        self.assertEqual(first, v["scale"]["splitmix64"])

    def test_device_txt(self):
        """The fixture card's file says what the firmware's current() does, and
        any text a reader reads as lib/core's parse() and the writer's parser
        read it (7.4; M 2.15)."""
        text = (FIX / "golden" / "device.txt").read_text(encoding="ascii")
        self.assertEqual(mpwf.writes_waves(text), cf.device_main()["read.mpwf"] == "1")
        self.assertFalse(mpwf.writes_waves(text.replace("contract=1\n", "") + "read.mpwf=1\n"))
        self.assertTrue(mpwf.writes_waves(text + "read.mpwf=1\n"))
        self.assertFalse(mpwf.writes_waves(text + "read.mpwf=1\nread.mpwf=\n"))
        for value, want in (("32,1", True), ("1 ", True), ("1,x", True), ("1 1", False), ("11", False), ("100", False),
                            ("0", False), (",", False), ("1\x0b", False), ("\u0661", False)):
            self.assertEqual(mpwf.lists_major(value, 1), want, value)

    def test_hashes(self):
        """2.3's table."""
        for text, h in (("", 0xCBF29CE484222325), ("/music", 0x75DC8A6A38687865),
                        ("/music/Artist/Album/01 - Title.mp3", 0xDA70B312760188D8),
                        ("/music/" + mpwf.NFC_CAFE + "/Album/01 - Title.mp3", 0xF1B24FC757494F2B),
                        ("/music/" + mpwf.NFD_CAFE + "/Album/01 - Title.mp3", 0x57017C0CC011BB1B)):
            self.assertEqual(mpwf.fnv1a64(text.encode("utf-8")), h, text)
        self.assertEqual(mpwf.path_hash("Artist/Album/01 - Title.mp3"), 0xDA70B312760188D8)
        self.assertEqual(mpwf.path_hash(""), 0x75DC8A6A38687865)

    def test_bar_ranges(self):
        """Every range is non-empty and inside M; at M >= 240 they part M with
        no gap and no overlap; below it every peak is used, one to a bar."""
        for m in list(range(1, 1300)) + [2400, 4096, 65536]:
            ranges = [mpwf.bar_range(i, m) for i in range(240)]
            self.assertTrue(all(0 <= s < e <= m for s, e in ranges), m)
            self.assertEqual(ranges[0][0], 0)
            self.assertEqual(ranges[-1][1], m)
            if m >= 240:
                self.assertTrue(all(a[1] == b[0] for a, b in zip(ranges, ranges[1:])), m)
            else:
                self.assertTrue(all(e - s == 1 for s, e in ranges), m)
                self.assertEqual(len({s for s, _ in ranges}), m)
        self.assertEqual({mpwf.bar_range(i, 800)[1] - mpwf.bar_range(i, 800)[0] for i in range(240)}, {3, 4})

    def test_second_reading_of_the_bars(self):
        rng = random.Random(11)
        for m in (1, 2, 3, 5, 7, 60, 119, 120, 121, 239, 240, 241, 479, 480, 481, 799, 800, 801, 960, 1000, 3000):
            for peaks in ([rng.randrange(256) for _ in range(m)],
                          [rng.choice((0, 1, 3, 5, 253, 255)) for _ in range(m)], [0] * m, [255] * m):
                self.assertEqual(mpwf.bars_from_peaks(peaks), plain_bars(peaks), m)

    def test_rounding(self):
        def bar2(peaks):              # bar 2 of M = 800 takes peaks 6-9
            full = [0] * 800
            full[6:6 + len(peaks)] = peaks
            return mpwf.bars_from_peaks(full)[2]

        self.assertEqual(bar2([1]), 1)                  # 0.5: half up, not to 0
        self.assertEqual(bar2([3]), 2)                  # 1.5
        self.assertEqual(bar2([5]), 3)                  # 2.5: Python's round() gives 2
        self.assertEqual(round(2.5), 2)
        self.assertEqual(bar2([253]), 127)              # 126.5: round() gives 126
        self.assertEqual(bar2([2, 2]), 1)               # 1.41
        self.assertEqual(bar2([2, 2, 1]), 2)            # 1.5
        self.assertEqual(bar2([255, 255, 255, 255]), 255)
        self.assertEqual(mpwf.bars_from_peaks([255])[239], 255)
        with self.assertRaises(ValueError):
            mpwf.bars_from_peaks([])

    def test_slot(self):
        bars = bytes(range(240))
        raw = mpwf.encode_slot(5, 0x0123456789ABCDEF, 1234, bars)
        self.assertEqual(len(raw), 256)
        self.assertEqual(raw[:8], bytes.fromhex("EFCDAB8967452301"))
        self.assertEqual(raw[8:12], bytes.fromhex("D2040000"))
        self.assertEqual(raw[12:252], bars)
        self.assertEqual(struct.unpack_from("<I", raw, 252)[0], mpwf.slot_crc(raw, 5))
        self.assertNotEqual(mpwf.slot_crc(raw, 5), mpwf.slot_crc(raw, 6))       # the row seeds it
        self.assertEqual(mpwf.slot_crc(raw, 5), mpwf.crc32(raw[:252], mpwf.crc32(struct.pack("<I", 5))))

    def test_section_crc_says_nothing_of_the_slots(self):
        """4.2: a CRC run over data and its own CRC ends in a fixed state, so
        WVSL's section CRC follows from the row count alone."""
        rng = random.Random(3)
        for rows in (1, 2, 7, 50):
            crcs = set()
            for _ in range(5):
                hs = sorted(rng.getrandbits(64) for _ in range(rows))
                d = [(h, rng.getrandbits(32), bytes(rng.randrange(256) for _ in range(240))) for h in hs]
                crcs.add(mpwf.Reader(mpwf.write(d, 1)).section.crc)
            self.assertEqual(len(crcs), 1, rows)
        for e in vectors()["sectionCrc"]:
            self.assertEqual(mpwf.Reader(golden(e["file"])).section.crc, int(e["crc"], 16))
            same = mpwf.write(flat_rows(range(1, e["rows"] + 1)), 1)
            self.assertEqual(mpwf.Reader(same).section.crc, int(e["crc"], 16))
        # So two files of one generation and row count have one headerCrc.
        a = mpwf.write([(1, 10, bytes(240)), (2, 20, bytes([9]) * 240)], 5, 7)
        b = mpwf.write([(3, 30, bytes([1]) * 240), (4, 40, bytes(240))], 5, 7)
        self.assertNotEqual(a, b)
        self.assertEqual(a[:256], b[:256])

    def test_size(self):
        self.assertEqual(mpwf.size(20000), 5120256)
        self.assertEqual(mpwf.size(250000), 64000256)
        self.assertLess(mpwf.size(mpwf.MAX_ROWS), 64 << 20)
        for name, d in mpwf_descs():
            self.assertEqual(mpwf.size(mpwf.Reader(golden(d["file"])).rows), len(golden(d["file"])))


class Goldens(unittest.TestCase):
    def test_goldens_byte_for_byte(self):
        names = [n for n, _ in mpwf_descs()]
        self.assertEqual(names, ["waves-0000002a.json", "waves-one.json", "waves-skew.json"])
        for name, d in mpwf_descs():
            with self.subTest(name):
                self.assertEqual(mpwf.build(d), golden(d["file"]))

    def test_read_back_equals_the_rule(self):
        for name, d in mpwf_descs():
            with self.subTest(name):
                r = mpwf.Reader(golden(d["file"]))
                self.assertEqual(r.check("all"), "ok")
                self.assertEqual((r.major, r.minor, r.generation, r.card_id, r.header_bytes, r.section_count),
                                 (1, 0, d["generation"], int(d["cardId"], 16), 224, 1))
                self.assertEqual((r.slot_bytes, r.bar_count, r.section.offset, r.section.stride, r.section.flags),
                                 (256, 240, 256, 256, 1))
                # Whole file: the header's bytes past barCount are zero.
                self.assertEqual(golden(d["file"])[48:224], bytes(176))
                by_hash = {}
                for t in d["tracks"]:
                    h = int(t["hash"], 16) if "hash" in t else mpwf.path_hash(t["path"])
                    by_hash.setdefault(h, []).append(t)
                rows = sorted(h for h, ts in by_hash.items() if len(ts) == 1)
                self.assertEqual(r.rows, len(rows))
                for i, h in enumerate(rows):
                    t = by_hash[h][0]
                    self.assertEqual(r.slot(i)[2], mpwf.Slot(h, t["lengthMs"], plain_bars(t["peaks"])))
                    self.assertEqual(r.find(h).row, i)

    def test_tracks_in_any_order(self):
        rng = random.Random(5)
        for name, d in mpwf_descs():
            d2 = copy.deepcopy(d)
            rng.shuffle(d2["tracks"])
            self.assertEqual(mpwf.build(d2), golden(d["file"]), name)

    def test_the_fixture_card(self):
        """The root golden names the file after the other two, and every slot
        is a track of the tags file it names, with that record's length."""
        root = golden("manifest-waves.bin")
        row = cf.root_waves(root)
        self.assertEqual(row[0], G1)
        self.assertTrue(mpwf.companion_matches(golden(G1), *row[1:]))
        self.assertEqual(row[1:4], (42, 2048, struct.unpack_from("<I", golden(G1), 32)[0]))
        self.assertEqual(row[4], bytes(16))
        p = cf.parse_container(root)
        comp = cf.section(p, "COMP")
        self.assertEqual([bytes(comp[4][40 * i:40 * i + 4]) for i in range(comp[2])], [b"MPTG", b"MPDJ", b"MPWF"])
        strs = bytes(cf.section(p, "STRS")[4])
        names = [struct.unpack_from("<I", comp[4], 40 * i + 16)[0] for i in range(3)]
        self.assertEqual(names, sorted(names))
        self.assertEqual(strs[names[2]:strs.index(0, names[2])], G1.encode("ascii"))
        records = {x["pathHash"]: x for x in cf.ledger_rows(golden("tags-0000002a.bin"))}
        r = mpwf.Reader(golden(G1))
        for i in range(r.rows):
            s = r.slot(i)[2]
            self.assertEqual(records[s.path_hash]["durationMs"], s.length_ms)
        self.assertEqual(cf.root_waves(golden("manifest.bin")), None)      # manifest.bin names none


class Search(unittest.TestCase):
    def test_search_vectors(self):
        s = vectors()["search"]["files"]
        self.assertEqual(sorted(s), sorted((G1, ONE, SKEW)))
        for name in s:
            r = mpwf.Reader(golden(name))
            for e in s[name]:
                with self.subTest(name, hash=e["hash"]):
                    before = r.reads
                    f = r.find(int(e["hash"], 16))
                    self.assertEqual((f.read, f.row, f.probes), (e["read"], e.get("row"), e["probes"]))
                    self.assertEqual(r.reads - before, len(e["probes"]))

    def test_every_row_and_a_miss(self):
        for name in (G1, ONE, SKEW):
            r = mpwf.Reader(golden(name))
            hs = [r.slot(i)[2].path_hash for i in range(r.rows)]
            for h in hs:
                f = r.find(h)
                self.assertEqual((f.read, f.row, f.slot.path_hash), (mpwf.HIT, hs.index(h), h))
            for a, b in zip(hs, hs[1:]):
                if b - a > 1:
                    self.assertEqual(r.find((a + b) // 2).read, mpwf.MISS)

    def test_the_answer_is_the_walks(self):
        """Whatever the hashes' shape: uniform, clustered, consecutive."""
        rng = random.Random(9)
        shapes = [sorted({rng.getrandbits(64) for _ in range(300)}),
                  sorted({rng.getrandbits(32) for _ in range(300)}),
                  sorted({(rng.getrandbits(4) << 60) + rng.getrandbits(8) for _ in range(300)}),
                  list(range(300)),
                  [mpwf.M64 - i for i in range(299, -1, -1)]]
        for hs in shapes:
            r = mpwf.Reader(mpwf.write(flat_rows(hs), 1))
            self.assertEqual(r.check("all"), "ok")
            for h in hs + [rng.choice(hs) + 1 for _ in range(100)] + [0, mpwf.M64]:
                if h > mpwf.M64:
                    continue
                f = r.find(h)
                want = plain_row(hs, h)
                self.assertEqual(f.row, want, hex(h))
                self.assertEqual(f.read, mpwf.HIT if want is not None else mpwf.MISS)
                self.assertLessEqual(len(f.probes), 22)
                self.assertEqual(len(set(f.probes)), len(f.probes))           # never the same row twice

    def test_reads_at_scale(self):
        """The vectors' sums, and 2's claim for the hash: a card's real-shaped
        paths search as uniform hashes do. The means are 6's table."""
        v = vectors()["scale"]
        hashes = mpwf.scale_hashes()
        for name in ("uniform", "paths"):
            with self.subTest(name):
                got = mpwf.scale_case(*hashes[name])
                self.assertEqual(got, {k: v[name][k] for k in got})
                self.assertLess(got["hitReads"] / got["rows"], 5)
                self.assertLess(got["missReads"] / got["misses"], 6)
                self.assertLessEqual(max(got["hitMax"], got["missMax"]), 22)
                self.assertEqual(len(set(hashes[name][0])), got["rows"])

    def test_worst_reads(self):
        """The worst file there is: 250,000 rows of consecutive hashes. Four
        probes creep, then 18 bisect: 22, under the guard of 24. The slots are
        made as they are read."""
        rows = mpwf.MAX_ROWS

        class Lazy(mpwf.Reader):
            def __init__(self):
                self.rows, self.reads = rows, 0

            def slot_raw(self, r):
                return mpwf.encode_slot(r, r + 1, 0, bytes(240))

        r = Lazy()
        most = 0
        for row in list(range(0, 40)) + list(range(0, rows, 7919)) + [rows - 2, rows - 1]:
            f = r.find(row + 1)
            self.assertEqual((f.read, f.row), (mpwf.HIT, row))
            most = max(most, len(f.probes))
        self.assertEqual(most, 22)
        self.assertEqual(r.find(4 + 1).probes[:4], [0, 1, 2, 3])        # the creep, then the first bisection
        self.assertEqual(r.find(4 + 1).probes[4], 4 + (rows - 1 - 4) // 2)
        self.assertEqual(r.find(0).read, mpwf.MISS)
        self.assertEqual(r.find(rows + 1).read, mpwf.MISS)
        # 64 bits hold the arithmetic at its largest (6: no 128-bit integer).
        self.assertLess((rows - 1) * 0xFFFFFFFF + 0x7FFFFFFF, 1 << 51)

    def test_the_guard(self):
        """24 reads end a lookup as a miss. Only a search that creeps past the
        first probes gets there: a valid file with the rule's search doesn't."""
        real = mpwf.probe_row
        data = golden(SKEW)
        try:
            mpwf.probe_row = lambda h, lo, hi, lo_h, hi_h, probes: lo
            f = mpwf.Reader(data).find(mpwf.SKEW_ALONE)
        finally:
            mpwf.probe_row = real
        self.assertEqual((f.read, f.row, len(f.probes)), (mpwf.MISS, None, 24))
        self.assertEqual(mpwf.Reader(data).find(mpwf.SKEW_ALONE).read, mpwf.HIT)

    def test_damaged_and_broken_end_a_lookup(self):
        data = bytearray(golden(G1))
        r = mpwf.Reader(data)
        hs = [r.slot(i)[2].path_hash for i in range(r.rows)]
        data[256 + 2 * 256 + 100] ^= 0x10                           # slot 2: a bar
        r = mpwf.Reader(data)                                       # a frame's CRC doesn't cover the slots
        self.assertEqual(r.why, "ok")
        f = r.find(hs[2])
        self.assertEqual((f.read, f.row, f.slot), (mpwf.DAMAGED, 2, None))
        self.assertEqual(r.find(hs[6]).read, mpwf.HIT)              # the file stays
        self.assertEqual(r.check("device"), "ok")
        self.assertEqual(r.check("all"), "wvSlotCrc")
        # A hash below the slot read before it: the sort rule, under good CRCs.
        rows = [(10, 0, bytes(240)), (40, 0, bytes(240)), (20, 0, bytes(240)), (30, 0, bytes(240))]
        rows += [((1 << 63) + i, 0, bytes(240)) for i in range(4)]
        r = mpwf.Reader(mpwf.write(rows, 1))
        self.assertEqual(r.check("device"), "ok")
        self.assertEqual(r.check("all"), "wvSlotOrder")
        found = {r.find(h).read for h in (10, 20, 30, 40, 15, 25, 35, 45)}
        self.assertIn(mpwf.BROKEN, found)

    def test_hash_zero_and_the_top(self):
        """The first bounds aren't slots: both extreme hashes are valid in a
        slot, first and last, and a hash equal to a real bound is the sort
        rule's."""
        r = mpwf.Reader(golden(SKEW))
        self.assertEqual(r.check("all"), "ok")
        self.assertEqual(r.slot(0)[2].path_hash, 0)
        self.assertEqual(r.slot(r.rows - 1)[2].path_hash, mpwf.M64)
        self.assertEqual(r.find(0).row, 0)
        self.assertEqual(r.find(mpwf.M64).row, r.rows - 1)
        one = mpwf.Reader(mpwf.write([(0, 0, bytes(240))], 1))
        self.assertEqual((one.find(0).read, one.find(1).read), (mpwf.HIT, mpwf.MISS))
        top = mpwf.Reader(mpwf.write([(mpwf.M64, 0, bytes(240))], 1))
        self.assertEqual((top.find(mpwf.M64).read, top.find(1).read), (mpwf.HIT, mpwf.MISS))
        twice = mpwf.Reader(mpwf.write([(5, 0, bytes(240)), (5, 0, bytes(240)), (7, 0, bytes(240))], 1))
        self.assertEqual(twice.check("all"), "wvSlotOrder")


class Hardening(unittest.TestCase):
    def test_hardening_verdicts(self):
        entries = [e for e in index() if e["format"] == "MPWF"]
        self.assertGreaterEqual(len(entries), 40)
        for e in entries:
            with self.subTest(e["file"]):
                data = (FIX / "hardening" / e["file"]).read_bytes()
                self.assertEqual(mpwf.check(data, e["uses"]), e["why"] if e["expect"] == "absent" else "ok")
                if "same" in e:
                    self.assertIsNone(mpwf.same_content(data, golden(e["same"])))
                if "slot" in e:
                    r = mpwf.Reader(data)
                    self.assertEqual(r.check("device"), "ok")
                    f = r.find(int(e["slot"]["hash"], 16))
                    self.assertEqual((f.read, len(f.probes)), (e["slot"]["read"], e["slot"]["reads"]))
        whys = {e["why"] for e in entries}
        for w in ("short", "magic", "major", "headerBytes", "sectionCount", "fileBytes", "headerCrc", "layout",
                  "unknownRequired", "shape", "missing", "enum", "wvLayout", "wvSlotCrc", "wvSlotOrder", "sectionCrc",
                  "ok"):
            self.assertIn(w, whys)

    def test_the_reasons_in_order(self):
        """A file broken two ways gives the first reason of 8.4."""
        g = golden(G1)
        p = mpwf.Parts(g)
        p.put("<I", 40, 512)                                 # enum, and...
        s = p.wvsl()
        s[3] = 512                                           # ...shape, by the same section
        s[4] = bytearray(b"".join(bytes(s[4][256 * i:256 * (i + 1)]) + bytes(256) for i in range(s[2])))
        self.assertEqual(mpwf.check(p.build()), "enum")
        p = mpwf.Parts(g)
        d = p.wvsl()[4]
        d[256 + 252] ^= 1                                    # slot 1's CRC
        d[0:256], d[512:768] = d[512:768], d[0:256]          # slots 0 and 2 swapped: 0's CRC fails first
        self.assertEqual(mpwf.check(p.build()), "wvSlotCrc")
        b = bytearray(g)
        struct.pack_into("<I", b, 16, len(b) + 8)            # fileBytes, before the header's CRC
        self.assertEqual(mpwf.check(b), "fileBytes")

    def test_truncation_and_flips(self):
        """Every cut is absent and one byte more too; every flipped bit is
        absent: this file has no gap and no byte outside a CRC. The device,
        which reads the frame alone, finds only the frame's flips."""
        for name in (ONE, G1):
            g = golden(name)
            for n in range(len(g)):
                self.assertNotEqual(mpwf.check(g[:n]), "ok", n)
                self.assertNotEqual(mpwf.check(g[:n], "device"), "ok", n)
                if n >= 40:                                  # and with the frame made to agree
                    cut = bytearray(g[:n])
                    struct.pack_into("<I", cut, 16, n)
                    if n >= 256:
                        mpwf.reseal_header(cut)
                    self.assertNotEqual(mpwf.check(cut), "ok", n)
            self.assertEqual(mpwf.check(g + b"\0"), "fileBytes")
            b = bytearray(g)
            for o in range(len(g)):
                for bit in range(8):
                    b[o] ^= 1 << bit
                    self.assertNotEqual(mpwf.check(b), "ok", (name, o, bit))
                    why = mpwf.check(b, "device")
                    if o >= 256:
                        self.assertEqual(why, "ok", (name, o, bit))
                    else:
                        self.assertNotEqual(why, "ok", (name, o, bit))
                    b[o] ^= 1 << bit

    def test_row_limit(self):
        """Too big to be a fixture: a file of 64 MB, read as far as its frame."""
        for e in vectors()["rowLimit"]:
            with self.subTest(e["rows"]):
                data = bytes.fromhex(e["head"]) + bytes(e["fileBytes"] - 256)
                r = mpwf.Reader(data)
                self.assertEqual(r.check("device"), e["why"])
                self.assertEqual((r.file_bytes, r.section.count), (e["fileBytes"], e["rows"]))
        self.assertEqual([e["why"] for e in vectors()["rowLimit"]], ["ok", "enum"])

    def test_root_rows(self):
        rr = vectors()["rootRow"]
        files = {e["file"]: (FIX / "hardening" / e["file"]).read_bytes() for e in rr["hardening"]}
        by_file = {e["file"]: e for e in index()}
        for e in rr["hardening"]:
            with self.subTest(e["file"]):
                self.assertEqual(by_file[e["file"]]["expect"], "present")
                row = cf.root_waves(files[e["file"]])
                self.assertEqual(row[0] if row else None, e["waves"])
                if row:
                    self.assertEqual(mpwf.companion_matches(golden(row[0]), *row[1:]), e["matches"])
                else:
                    self.assertIsNone(e["matches"])
        for name, why in (("msmf-waves-name-range.bin", "string"), ("msmf-waves-name-order.bin", "stringOrder")):
            self.assertEqual((by_file[name]["expect"], by_file[name]["why"]), ("absent", why))

    def test_companion_matches(self):
        g = golden(G1)
        r = mpwf.Reader(g)
        self.assertTrue(mpwf.companion_matches(g, 42, len(g), r.header_crc))
        self.assertFalse(mpwf.companion_matches(g, 41, len(g), r.header_crc))
        self.assertFalse(mpwf.companion_matches(g, 42, len(g) + 1, r.header_crc))
        self.assertFalse(mpwf.companion_matches(g, 42, len(g), r.header_crc ^ 1))
        self.assertFalse(mpwf.companion_matches(g, 42, len(g), r.header_crc, bytes([1]) * 16))
        self.assertFalse(mpwf.companion_matches(g[:-1], 42, len(g), r.header_crc))
        cut = (FIX / "hardening" / "mpwf-truncated-row.bin").read_bytes()    # valid, and not the named file
        self.assertEqual(mpwf.check(cut), "ok")
        self.assertFalse(mpwf.companion_matches(cut, 42, len(g), r.header_crc))


class Writer(unittest.TestCase):
    def test_collision_rule(self):
        """A hash two tracks share is left out, both of them; the rows stay in
        order, a row's CRC seeded with its new place."""
        d = desc("waves-0000002a.json")
        real = mpwf.path_hash
        a, b = (t["path"] for t in d["tracks"][:2])

        def fake(rel):
            return 0x1111111111111111 if rel in (a, b) else real(rel)

        try:
            mpwf.path_hash = fake
            data = mpwf.build(d)
        finally:
            mpwf.path_hash = real
        r = mpwf.Reader(data)
        self.assertEqual((r.check("all"), r.rows), ("ok", 5))
        self.assertEqual(r.find(0x1111111111111111).read, mpwf.MISS)
        self.assertEqual(mpwf.Reader(golden(SKEW)).find(mpwf.SKEW_SHARED).read, mpwf.MISS)

    def test_when_to_write_a_new_file(self):
        """7.3, the first line of its table that holds. A file with rows {A, B},
        B gone and a fetch that gained nothing is a new file of {A}; a file
        with rows {A}, A gone, is no MPWF; a fetch has no say beyond the rows."""
        g = golden(G1)
        r = mpwf.Reader(g)
        rows = [tuple(r.slot(i)[2]) for i in range(r.rows)]
        lists, other = "contract=1\nread.mpwf=1\n", "contract=1\nread.mpwf=2\n"
        outcome = mpwf.commit_outcome
        self.assertEqual(outcome(other, g, rows), mpwf.NONE)
        self.assertEqual(outcome(None, g, rows), mpwf.NONE)
        self.assertEqual(outcome("read.mpwf=1\n", g, rows), mpwf.NONE)              # (no contract line: no file)
        self.assertEqual(outcome(lists, None, []), mpwf.NONE)
        self.assertEqual(outcome(lists, g, []), mpwf.NONE)                          # not a file of 0 rows (4.2)
        self.assertEqual(outcome(lists, None, rows), mpwf.NEW)
        self.assertEqual(outcome(lists, g, rows), mpwf.SAME)
        self.assertEqual(outcome(lists, g, rows[:-1]), mpwf.NEW)                    # a row went; none gained
        self.assertEqual(outcome(lists, g, rows[1:]), mpwf.NEW)                     # so every row after it moved
        # Only WVSL counts: another generation or card id names the same rows.
        self.assertEqual(outcome(lists, mpwf.write(rows, 7, 9), rows), mpwf.SAME)
        # One slot's bytes: the frame's CRCs can't see it (4.2), the comparison can.
        changed = list(rows)
        changed[3] = (rows[3][0], rows[3][1] + 1, rows[3][2])
        self.assertEqual(mpwf.write(changed, r.generation, r.card_id)[:mpwf.FIRST_SLOT], g[:mpwf.FIRST_SLOT])
        self.assertEqual(outcome(lists, g, changed), mpwf.NEW)
        # The new file of a row gone reads, and its rows name it again.
        new = mpwf.write(rows[:-1], 43, r.card_id)
        self.assertEqual(mpwf.check(new, "all"), "ok")
        self.assertEqual(outcome(lists, new, rows[:-1]), mpwf.SAME)
        # And no row can't be a file: a reader takes 0 rows for absent (8.2).
        self.assertEqual(mpwf.check((FIX / "hardening" / "mpwf-rows-zero.bin").read_bytes()), "enum")

    def test_writer_refusals(self):
        base = desc("waves-one.json")

        def refused(fn):
            d = copy.deepcopy(base)
            fn(d)
            with self.assertRaises(mpwf.WriterError):
                mpwf.prepare(d)

        track = base["tracks"][0]
        refused(lambda d: d.update(major=2))
        refused(lambda d: d.update(format="MPDJ"))
        refused(lambda d: d.update(generation=1 << 32))
        refused(lambda d: d.update(cardId=1 << 64))
        refused(lambda d: d.update(tracks=[]))                            # no row: no file
        refused(lambda d: d["tracks"][0].update(peaks=[]))
        refused(lambda d: d["tracks"][0].update(peaks=[256]))
        refused(lambda d: d["tracks"][0].update(peaks=[-1]))
        refused(lambda d: d["tracks"][0].update(peaks=[1.5]))
        refused(lambda d: d["tracks"][0].update(peaks=[True]))
        refused(lambda d: d["tracks"][0].pop("peaks"))
        refused(lambda d: d["tracks"][0].update(lengthMs=1 << 32))
        refused(lambda d: d["tracks"][0].update(lengthMs=-1))
        refused(lambda d: d["tracks"][0].update(path=""))
        refused(lambda d: d["tracks"][0].update(hash="0x1"))              # a path or a hash, not both
        refused(lambda d: d["tracks"][0].pop("path"))
        refused(lambda d: d["tracks"].append(dict(track)))                # one path twice
        two = {"format": "MPWF", "generation": 1,
               "tracks": [{"hash": "0x1", "peaks": [1]}, {"hash": "0x1", "peaks": [2]}]}
        with self.assertRaises(mpwf.WriterError):                         # both left out, so no row
            mpwf.prepare(two)
        mpwf.prepare(copy.deepcopy(base))

    def test_too_many_rows(self):
        many = {"format": "MPWF", "generation": 1, "tracks": [{"hash": i, "peaks": [0]} for i in range(250001)]}
        real = mpwf.bars_from_peaks
        try:
            mpwf.bars_from_peaks = lambda peaks: bytes(240)         # (the bars aren't what this is about)
            with self.assertRaises(mpwf.WriterError):
                mpwf.prepare(many)
            many["tracks"].pop()
            self.assertEqual(len(mpwf.prepare(many).rows), 250000)
        finally:
            mpwf.bars_from_peaks = real

    def test_edge_cases(self):
        one = mpwf.build({"format": "MPWF", "generation": 0xFFFFFFFF, "cardId": "0xFFFFFFFFFFFFFFFF",
                          "tracks": [{"path": "a.mp3", "lengthMs": 0xFFFFFFFF, "peaks": [7]}]})
        r = mpwf.Reader(one)
        self.assertEqual((len(one), r.rows, r.generation, r.card_id), (512, 1, 0xFFFFFFFF, mpwf.M64))
        self.assertEqual(r.find(mpwf.path_hash("a.mp3")).slot.length_ms, 0xFFFFFFFF)
        self.assertEqual(mpwf.path_hash("a.mp3"), mpwf.fnv1a64(b"/music/a.mp3"))
        # Minor is the caller's, and a reader accepts any.
        r = mpwf.Reader(mpwf.build(dict(desc("waves-one.json"), minor=7)))
        self.assertEqual((r.why, r.minor), ("ok", 7))


class Cli(unittest.TestCase):
    def run_cli(self, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            code = mpwf.main(list(args))
        return code, out.getvalue()

    def test_cli(self):
        with tempfile.TemporaryDirectory() as t:
            plain = Path(t, "tracks.json")
            plain.write_text(json.dumps({"Artist/Album/01 - Title.mp3": {"lengthMs": 180000, "peaks": [0x40]}}),
                             encoding="utf-8")
            out = str(Path(t, "w.bin"))
            self.assertEqual(self.run_cli("build", str(plain), out, "--generation", "1", "--card-id",
                                          "0x5EEDC0DE0A1B2C3D")[0], 0)
            self.assertEqual(Path(out).read_bytes(), golden(ONE))
            self.assertEqual(self.run_cli("build", str(LIB / "waves-0000002a.json"), out)[0], 0)
            self.assertEqual(Path(out).read_bytes(), golden(G1))
            code, text = self.run_cli("check", out)
            self.assertEqual((code, text.strip().endswith("present")), (0, True))
            bad = str(FIX / "hardening" / "mpwf-slot-crc.bin")
            code, text = self.run_cli("check", bad)
            self.assertEqual(code, 1)
            self.assertIn("wvSlotCrc", text)
            self.assertEqual(self.run_cli("check", bad, "--uses", "device")[0], 0)
            code, text = self.run_cli("dump", str(FIX / "golden" / ONE))
            self.assertEqual(code, 0)
            self.assertIn("rows 1", text)
            self.assertIn("hash 0xDA70B312760188D8 lengthMs 180000 peak 64 mean 64", text)
            code, text = self.run_cli("dump", str(FIX / "golden" / G1), "--slot", "3", "--hex")
            self.assertIn("|", text)
            code, text = self.run_cli("dump", str(FIX / "golden" / G1), "--path", "A/x.mp3")
            self.assertIn("hit", text)
            code, text = self.run_cli("dump", str(FIX / "golden" / G1), "--hash", "0x12")
            self.assertIn("miss", text)
            code, text = self.run_cli("dump", bad)
            self.assertIn("damaged (wvSlotCrc)", text)
            code, text = self.run_cli("dump", str(FIX / "hardening" / "mpwf-magic.bin"))
            self.assertIn("absent: magic", text)
            code, text = self.run_cli("vectors")
            self.assertEqual(json.loads(text), vectors_without_root_row())
            code, text = self.run_cli("size", "20000")
            self.assertIn("5120256 bytes", text)
            plain.write_text(json.dumps({"a.mp3": {"lengthMs": 1, "peaks": []}}), encoding="utf-8")
            self.assertEqual(self.run_cli("build", str(plain), out)[0], 2)


def vectors_without_root_row():
    """The group as tools/mpwf.py makes it: card_fixtures.py adds the root's."""
    v = vectors()
    del v["rootRow"]
    return v


if __name__ == "__main__":
    unittest.main()
