# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Tests for tools/fatmodel.py (the host FatFs model's driver: docs/METADATA.md
3.2.7).

    python -m unittest discover -s tools -p "test_fatmodel.py"

The listing follows synthcard's copy (each folder before what it holds,
names in NTFS's order) and the model runs on a small card; with
FATMODEL_FULL=1 also on the default 19,519-file card, against 3.2.7's
figures (about 35 s). Needs gcc and g++ on PATH (skipped without them).
The model's own checks are test_fat_model's (pio test -e native).
"""
import os
import shutil
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fatmodel  # noqa: E402
import synthcard  # noqa: E402

HAVE_GCC = bool(shutil.which("gcc") and shutil.which("g++"))


class Listing(unittest.TestCase):
    def test_copy_order(self):
        rows, probes = fatmodel.plan_listing(300, 5)
        self.assertEqual(rows[0][:2], ("F", "SYNTHCARD.TXT"))
        self.assertEqual(rows[1], ("D", "music", 0))
        made = {""}
        last = {}
        for kind, rel, size in rows:
            parent, _, name = rel.rpartition("/")
            self.assertIn(parent, made, rel)  # its folder came first
            key = synthcard.ntfs_key(name)
            if parent and parent in last:  # (the root: SYNTHCARD.TXT, then music)
                self.assertLess(last[parent], key, rel)  # names in NTFS's order
            last[parent] = key
            if kind == "D":
                made.add(rel)
            else:
                self.assertGreater(size, 0, rel)
        for p in probes:
            self.assertIn(("F", p), [(k, r) for k, r, _ in rows])


@unittest.skipUnless(HAVE_GCC, "needs gcc and g++ on PATH")
class Model(unittest.TestCase):
    def test_small_card(self):
        rows, probes = fatmodel.plan_listing(400, 3)
        r = fatmodel.measure(rows, probes, caches="64,256", as_json=True)
        audio = [rel for k, rel, _ in rows if k == "F" and rel.startswith("music/")
                 and not rel.rsplit("/", 1)[1].startswith(".")
                 and rel.lower().rsplit(".", 1)[-1] in ("mp3", "flac", "opus")]
        self.assertEqual(r["audio"], len(audio))
        self.assertEqual(r["folders"], sum(1 for k, _, _ in rows if k == "D"))
        stock = r["lookup"]["stock"]
        # The root, /music, the artist and the album: a sector each at least.
        self.assertGreaterEqual(stock["p50"], 4)
        self.assertLess(r["lookup"]["64"]["mean"], stock["mean"])
        self.assertLessEqual(r["lookup"]["256"]["mean"], r["lookup"]["64"]["mean"])
        # A walk reads each folder's sectors at least once; the stock one its
        # lookups too.
        self.assertGreater(r["walk"]["stock"], r["walk"]["256"])
        self.assertGreaterEqual(r["walk"]["256"], r["folders"])
        self.assertGreater(r["scan_per_file"]["stock"], r["scan_per_file"]["256"])
        for p in probes:
            self.assertGreaterEqual(r["probes"][p], 4)

    @unittest.skipUnless(os.environ.get("FATMODEL_FULL"), "FATMODEL_FULL=1 runs the 19,519-file card")
    def test_default_card(self):
        rows, probes = fatmodel.plan_listing(synthcard.MEASURED["audio_files"], 1)
        r = fatmodel.measure(rows, probes, caches="128,256", as_json=True)
        # 3.2.7: /music as the user's (102 sectors), a lookup as the
        # research's (56), the walk as its 148,000 sectors; the cache.
        self.assertTrue(98 <= r["music_sectors"] <= 110, r["music_sectors"])
        self.assertTrue(50 <= r["lookup"]["stock"]["mean"] <= 65, r["lookup"]["stock"])
        self.assertTrue(130000 <= r["walk"]["stock"] <= 170000, r["walk"])
        self.assertLess(r["lookup"]["256"]["mean"], 5)
        self.assertLessEqual(r["lookup"]["256"]["p99"], 12)
        self.assertLess(r["walk"]["256"], 8000)
        first, mid, last = (r["probes"][p] for p in probes)
        self.assertLess(first, mid)
        self.assertLess(mid, last)


if __name__ == "__main__":
    unittest.main()
