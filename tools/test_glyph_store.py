# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Tests for tools/glyph_store.py: the runs, the store's layout read back,
the character sets, the em-box rule and the test fixture (docs/I18N.md,
phase 2).

    python -m unittest discover -s tools -p "test_glyph_store.py"

The font-free parts need nothing but Python; the font tests need Pillow and
the CJK font (GLYPH_STORE_FONT, as the tool takes it; skipped without).
test_glyph_store (pio test -e native) reads the generated store and the
fixture through the firmware's own reader.
"""
import os
import random
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import glyph_store as gs  # noqa: E402

FONT = os.environ.get("GLYPH_STORE_FONT", "")


class Runs(unittest.TestCase):
    def test_tokens(self):
        # 0 n: n + 1 zeros; 15 n: n + 1 fifteens; 1-14 as they are; the
        # first nibble high, an odd one padded with 0.
        self.assertEqual(gs.runs([0, 0, 0]), bytes([0x02]))
        self.assertEqual(gs.runs([15, 7, 0]), bytes([0xF0, 0x70, 0x00]))
        self.assertEqual(gs.runs([1, 14]), bytes([0x1E]))
        self.assertEqual(gs.runs([5]), bytes([0x50]))

    def test_a_run_stops_at_sixteen(self):
        self.assertEqual(gs.runs([0] * 16), bytes([0x0F]))
        self.assertEqual(gs.runs([0] * 17), bytes([0x0F, 0x00]))
        self.assertEqual(gs.runs([15] * 18), bytes([0xFF, 0xF1]))

    def test_round_trip(self):
        rnd = random.Random(7)
        for _ in range(300):
            n = rnd.randrange(0, 80)
            # Mostly 0 and 15, as glyphs are.
            px = [rnd.choice((0, 0, 0, 15, 15, rnd.randrange(16))) for _ in range(n)]
            data = gs.runs(px)
            back, used = gs.unruns(data, n)
            self.assertEqual(back, px)
            self.assertEqual(used, len(data))

    def test_a_run_past_the_box_is_refused(self):
        with self.assertRaises(ValueError):
            gs.unruns(bytes([0x05]), 3)

    def test_raw(self):
        px = [1, 2, 3, 15, 0]
        self.assertEqual(gs.pack(px), bytes([0x12, 0x3F, 0x00]))
        self.assertEqual(gs.unpack(gs.pack(px), 5), px)


class Raster(unittest.TestCase):
    def test_quantise_rounds(self):
        self.assertEqual([gs.quantise(v) for v in (0, 8, 9, 17, 25, 26, 128, 246, 247, 255)],
                         [0, 0, 1, 1, 1, 2, 8, 14, 15, 15])

    def test_trim(self):
        px = [0, 0, 0, 0,
              0, 3, 0, 0,
              0, 0, 7, 0]
        self.assertEqual(gs.trim(4, 3, px), (1, 1, 2, 2))
        self.assertIsNone(gs.trim(2, 2, [0, 0, 0, 0]))

    def test_the_em_box_rule(self):
        # (w, h, x, y, advance, pixels): 2 px past the em box at most.
        self.assertTrue(gs.fits((16, 18, 0, 16, 16, ()), 14, 2))
        self.assertFalse(gs.fits((16, 18, 0, 17, 16, ()), 14, 2))
        self.assertTrue(gs.fits((4, 8, 0, 4, 16, ()), 14, 2))
        self.assertFalse(gs.fits((4, 9, 0, 4, 16, ()), 14, 2))
        self.assertTrue(gs.fits((0, 0, 0, 0, 16, ()), 14, 2))

    def test_metrics_word(self):
        w = gs.metrics_word(16, 17, -5, 14, 16)
        self.assertEqual((w & 63, w >> 6 & 63, gs.signed7(w >> 12 & 0x7F), gs.signed7(w >> 19 & 0x7F), w >> 26),
                         (16, 17, -5, 14, 16))
        with self.assertRaises(ValueError):
            gs.metrics_word(64, 1, 0, 0, 0)


class Layout(unittest.TestCase):
    def test_fixture_reads_back(self):
        glyphs = gs.fixture_glyphs()
        for enc in (gs.RUNS, gs.RAW):
            store = gs.build(glyphs, 16, 14, 2, enc)
            self.assertEqual(len(store) % 4, 0)
            head, back = gs.parse(store)
            self.assertEqual(back, glyphs)
            self.assertEqual((head["count"], head["pages"], head["encoding"]), (8, 5, enc))
            self.assertEqual((head["ink_ascent"], head["ink_descent"], head["widest"], head["tallest"]),
                             (14, 0, 19, 7))

    def test_header(self):
        store = gs.build(gs.fixture_glyphs(), 16, 14, 2)
        magic, version, sections = struct.unpack("<4sHH", store[:8])
        self.assertEqual((magic, version, sections), (b"GLYS", 1, 5))
        self.assertEqual(struct.unpack("<I", store[28:32])[0], len(store))
        tags = [store[32 + 12 * i:36 + 12 * i] for i in range(sections)]
        self.assertEqual(tags, [b"PAGE", b"PMAP", b"METR", b"OFFS", b"BITS"])
        for i in range(sections):
            at = struct.unpack("<I", store[36 + 12 * i:40 + 12 * i])[0]
            self.assertEqual(at % 4, 0)

    def test_index_by_bit_count(self):
        # Glyphs in code point order across pages: a page's first is the
        # count of the pages before it.
        glyphs = {cp: (1, 1, 0, 1, 1, (cp % 15 + 1,)) for cp in (0x41, 0x1FF, 0x200, 0x2FF, 0x10FFFF)}
        store = gs.build(glyphs, 8, 7, 1)
        self.assertEqual(gs.parse(store)[1], glyphs)
        pages = struct.unpack("<4H", store[gs.HEADER + 12 * 5:gs.HEADER + 12 * 5 + 8])
        self.assertEqual(pages, (0x0, 0x1, 0x2, 0x10FF))

    def test_digest_is_order_and_value_sensitive(self):
        glyphs = gs.fixture_glyphs()
        d = gs.digest(glyphs)
        changed = dict(glyphs)
        w, h, x, y, adv, px = changed[0x3042]
        changed[0x3042] = (w, h, x, y, adv, (px[0] + 1,) + px[1:])
        self.assertNotEqual(gs.digest(changed), d)

    def test_fixture_file_is_current(self):
        with open(os.path.join(gs.ROOT, "test", "test_glyph_store", "fixture_store.h"), encoding="utf-8") as f:
            self.assertEqual(f.read(), gs.fixture_header())


class Sets(unittest.TestCase):
    def test_standards(self):
        self.assertEqual(len(gs.ks_hangul()), 2350)
        self.assertEqual(len(gs.gb2312_level1()), 3755)
        self.assertEqual(len(gs.big5_level1()), 5401)
        self.assertEqual(len(gs.jamo()), 94)
        jis = gs.jis0208()
        self.assertEqual(sum(1 for cp in jis if gs.is_han(cp)), 6356)  # the 6,355 kanji, and 仝 in row 1
        euc = set(gs.dbcs("euc_jp", range(0xA1, 0xF5), range(0xA1, 0xFF)).values())
        self.assertEqual(len(euc), 6879)
        # cp932's other spellings of six cells: ∥ － ～ ￠ ￡ ￢.
        self.assertEqual(sorted(jis - euc), [0x2225, 0xFF0D, 0xFF5E, 0xFFE0, 0xFFE1, 0xFFE2])
        for ch in "東京事変宇多田坂本龍一ヶ‖〜":
            self.assertIn(ord(ch), jis, ch)

    def test_shift_jis_cells(self):
        # The cell arithmetic agrees with Python's Shift_JIS for every cell.
        cells = gs.dbcs("euc_jp", range(0xA1, 0xF5), range(0xA1, 0xFF))
        for (a, b), cp in cells.items():
            self.assertEqual(gs.sjis(a, b).decode("shift_jis"), chr(cp))

    def test_sets_nest(self):
        pan, jis, hangul = gs.SETS["pan"](), gs.SETS["jis"](), gs.SETS["hangul"]()
        self.assertTrue(hangul <= jis <= pan <= gs.SETS["pan-big5"]())
        self.assertTrue(gs.gb2312_level1() <= pan)

    def test_folded_ones_are_left_out(self):
        for cp in (0xFF01, 0xFF21, 0xFF5E, 0xFF61, 0xFF76, 0xFF9F, 0x2212):
            self.assertTrue(gs.folded(cp))
        for cp in (0xFF5F, 0xFF60, 0xFFE0, 0xFFE5, 0x3000, 0x30AB, 0x2015):
            self.assertFalse(gs.folded(cp))
        self.assertIn(0x2212, gs.jis0208())  # JIS X 0208's minus (1-1-61), as euc_jp has it
        vlw = {0x41}
        self.assertFalse(any(gs.folded(cp) for cp in gs.wanted("pan", vlw)))

    def test_vlw_codepoints(self):
        path = os.path.join(gs.ROOT, "src", "ui", "VlwFonts.cpp")
        cps = gs.vlw_codepoints(path)
        self.assertEqual(len(cps), 734)
        for cp in (0x20, 0x41, 0xE9, 0x0416, 0x03A9, 0x2026, 0x20AC):
            self.assertIn(cp, cps)
        self.assertNotIn(0x3042, cps)


@unittest.skipUnless(FONT and os.path.exists(FONT), "no GLYPH_STORE_FONT")
class Font(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from PIL import ImageFont
        with open(FONT, "rb") as f:
            cls.data = f.read()
        cls.index, cls.face = gs.pick_face(cls.data, "JP")
        cls.font = ImageFont.truetype(FONT, 16, index=cls.index)

    def test_faces_by_region(self):
        for region in gs.REGIONS:
            _, face = gs.pick_face(self.data, region)
            self.assertTrue(face.name(1).endswith(" " + region))
            self.assertNotIn("Mono", face.name(1))
        self.assertTrue(self.face.name(5).startswith("Version 2.004"))
        self.assertIn("Adobe", self.face.name(0))

    def test_em_box(self):
        upem, asc, desc = self.face.em_box()
        self.assertEqual((round(16 * asc / upem), round(16 * desc / upem)), (14, 2))

    def test_a_han_glyph(self):
        w, h, x, y, adv, px = gs.rasterise(self.font, ord("東"))
        self.assertEqual(adv, 16)
        self.assertTrue(14 <= w <= 16 and 15 <= h <= 17)
        self.assertTrue(0 <= x <= 1 and 13 <= y <= 15)
        self.assertEqual(len(px), w * h)
        self.assertEqual(max(px), 15)

    def test_hangul_is_narrower(self):
        self.assertEqual(gs.rasterise(self.font, ord("아"))[4], 15)

    def test_the_space_is_blank(self):
        self.assertEqual(gs.rasterise(self.font, 0x3000), (0, 0, 0, 0, 16, ()))


if __name__ == "__main__":
    unittest.main()
