# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Tests for tools/vlw_font.py: how a tall glyph is fitted to the line, M5GFX's
line height rule, and the cmap reader (docs/I18N.md, phase 1).

    python -m unittest discover -s tools -p "test_vlw_font.py"

Needs Pillow. The cmap and font tests use the DejaVu TTFs vlw_font.py's
default names (skipped without them); test_ui_library (pio test -e native)
checks the generated fonts' line heights.
"""
import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vlw_font  # noqa: E402

SANS = vlw_font.DEFAULT_FONTS.split(";")[0].rsplit(":", 2)[0]


def rows(*lines):
    """Bitmap rows from ASCII art: '#' full coverage, '+' half, ' ' none."""
    return [bytes({"#": 255, "+": 128, " ": 0}[c] for c in line) for line in lines]


def art(rs):
    return ["".join("#" if b == 255 else "+" if b else " " for b in r) for r in rs]


class Squeeze(unittest.TestCase):
    def test_a_second_gap_goes_first(self):
        # A breve over two blank rows over the letter: one gap row goes.
        g = rows(" # # ", "  #  ", "     ", "     ", "#   #", "#####")
        self.assertEqual(art(vlw_font.squeeze(g, 1, 4, 1)), [" # # ", "  #  ", "     ", "#   #", "#####"])

    def test_a_blank_edge_row_goes_first(self):
        g = rows("     ", "  #  ", "     ", "#####")
        self.assertEqual(art(vlw_font.squeeze(g, 1, 3, 1)), ["  #  ", "     ", "#####"])

    def test_a_tall_mark_loses_a_row_before_its_gap(self):
        # A three-row tilde keeps its gap: its two rows most alike merge,
        # the darker pixel kept.
        g = rows(" +#  #", "#  ## ", "#  ## ", "      ", "  ##  ")
        self.assertEqual(art(vlw_font.squeeze(g, 1, 4, 1)), [" +#  #", "#  ## ", "      ", "  ##  "])

    def test_stacked_marks_keep_the_tone_mark(self):
        # Ǘ at 16 px: an acute touching a diaeresis (one run, two marks), one
        # gap row, then the U. Two rows to go: the gap, then the diaeresis's
        # two alike rows; the acute keeps both of its rows.
        g = rows("   ##", "  ## ", "## ##", "## ##", "     ", "#   #")
        self.assertEqual(art(vlw_font.squeeze(g, 2, 5, 2)), ["   ##", "  ## ", "## ##", "#   #"])

    def test_the_letter_is_never_touched(self):
        g = rows("  #  ", "#   #", "#####")
        with self.assertRaises(ValueError):
            vlw_font.squeeze(g, 2, 1, 1)


class LineHeight(unittest.TestCase):
    def test_m5gfx_counts_ascii_and_above_latin1(self):
        # (code point, (height, width, advance, dY, dX, bits))
        tall = (16, 1, 1, 16, 0, b"")  # 1 px over an ascent of 15
        self.assertEqual(vlw_font.line_height([(0xC5, tall)], 15, 4), 19)  # Latin-1: not counted
        self.assertEqual(vlw_font.line_height([(0x41, tall)], 15, 4), 20)  # ASCII: counted
        self.assertEqual(vlw_font.line_height([(0x0102, tall)], 15, 4), 20)
        self.assertEqual(vlw_font.line_height([(0x0102, (5, 1, 1, -2, 0, b""))], 15, 4), 22)  # deep


@unittest.skipUnless(os.path.exists(SANS), f"no {SANS}")
class Font(unittest.TestCase):
    def test_cmap(self):
        mapped = vlw_font.cmap(SANS)
        for cp in (0x41, 0xE9, 0x0416, 0x03AC, 0x0219, 0x01DA, 0x0531):
            self.assertIn(cp, mapped, hex(cp))
        for cp in (0x0378, 0x0530, 0x4E2D):  # unassigned, unassigned, Han
            self.assertNotIn(cp, mapped, hex(cp))

    def test_the_fitted_glyphs_keep_their_letter(self):
        from PIL import ImageFont
        font = ImageFont.truetype(SANS, 16)
        ascent, descent = font.getmetrics()
        raw = vlw_font.glyph(font, 0x01D7)  # Ǘ
        u = vlw_font.glyph(font, ord("U"))
        fitted, changed = vlw_font.fit_to_line(font, 0x01D7, raw, ascent, descent)
        self.assertTrue(changed)
        h, w, _, dy, _, bits = fitted
        self.assertEqual(dy, ascent)
        # The U's rows are the bottom of the fitted glyph, unchanged.
        self.assertEqual(bits[(h - u[0]) * w:], raw[5][(raw[0] - u[0]) * w:])
        same, changed = vlw_font.fit_to_line(font, ord("U"), u, ascent, descent)
        self.assertFalse(changed)
        self.assertEqual(same, u)


if __name__ == "__main__":
    unittest.main()
