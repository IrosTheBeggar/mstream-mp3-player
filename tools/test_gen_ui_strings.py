# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Tests for tools/gen_ui_strings.py: what the catalogue's check refuses
(English's metadata, a placeholder's other fills, a translation's ids,
placeholders, plural forms, lines, buffers and characters the device
can't draw, a file that isn't UTF-8), the pseudo-locale, and the C
literal of the tables read back (docs/I18N.md, phase 3).

    python -m unittest discover -s tools -p "test_gen_ui_strings.py"

Standard library only. The real catalogue is checked by the generator
itself (--check, in CI) and test_ui_strings (pio test -e native).
"""
import copy
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_ui_strings as g  # noqa: E402

ROOM = {"font": "Body", "px": 200}


def english(extra=None):
    """A small en.json: a plain text, one with placeholders, a plural, a
    two-line one."""
    d = {
        "@@locale": "en",
        "@@autonym": "English",
        "a.plain": "Undo",
        "@a.plain": {"context": "a button", "room": ROOM},
        "a.of": "{0} of {1}",
        "@a.of": {"context": "a place", "room": ROOM,
                  "placeholders": {"0": {"means": "this", "type": "digits", "worst": 9},
                                   "1": {"means": "all", "type": "digits", "worst": 9}}},
        "a.tracks": {"one": "{0} track", "other": "{0} tracks"},
        "@a.tracks": {"context": "a count", "room": ROOM, "count": "0",
                      "placeholders": {"0": {"means": "tracks", "type": "grouped", "worst": 99999}}},
        "a.added": {"one": "Added", "other": "Added {0} tracks"},
        "@a.added": {"context": "an add", "room": ROOM, "count": "0",
                     "placeholders": {"0": {"means": "tracks", "type": "grouped", "worst": 99}}},
        "a.two": "first\nsecond",
        "@a.two": {"context": "two lines", "room": {"font": "Small", "px": 100, "lines": 2}},
    }
    if extra:
        d.update(extra)
    return d


def checked(d):
    return g.check_english(d)


def translate(tag, messages):
    eng, _ = checked(english())
    data = {"@@locale": tag, "@@autonym": g.AUTONYMS[tag]}
    data.update(messages)
    return g.check_translation(tag, data, eng)


class English(unittest.TestCase):
    def test_a_good_catalogue(self):
        eng, order = checked(english())
        self.assertEqual(order, ["a.plain", "a.of", "a.tracks", "a.added", "a.two"])
        self.assertTrue(eng["a.tracks"].plural)
        self.assertFalse(eng["a.of"].plural)

    def bad(self, d, words):
        with self.assertRaises(g.Bad) as e:
            checked(d)
        self.assertIn(words, str(e.exception))

    def test_ids(self):
        d = english({"Bad.id": "x", "@Bad.id": {"context": "c", "room": ROOM}})
        self.bad(d, "<screen>.<snake_case>")
        self.bad(english({"flat": "x", "@flat": {"context": "c", "room": ROOM}}), "<screen>.<snake_case>")

    def test_metadata(self):
        d = english()
        del d["@a.plain"]
        self.bad(d, "no @a.plain")
        d = english()
        d["@a.plain"] = {"room": ROOM}
        self.bad(d, "no context")
        d = english()
        d["@a.plain"] = {"context": "c", "room": ROOM, "colour": "red"}
        self.bad(d, "unknown metadata")
        d = english()
        d["@a.plain"] = {"context": "c"}
        self.bad(d, "no room")
        d = english()
        d["@a.plain"] = {"context": "c", "room": {"font": "Huge", "px": 10}}
        self.bad(d, "room font")
        d = english()
        d["@a.plain"] = {"context": "c", "room": {"font": "Body", "px": 10, "layout": "toast"}}
        self.bad(d, "two_line_px")
        d = english({"@nothing.here": {"context": "c"}})
        self.bad(d, "describes no message")

    def test_placeholders_are_described(self):
        d = english()
        d["a.plain"] = "Undo {0}"
        self.bad(d, "placeholders used [0], described []")
        d = english()
        d["a.of"] = "{0} of {2}"
        d["@a.of"]["placeholders"]["2"] = d["@a.of"]["placeholders"].pop("1")
        self.bad(d, "none skipped")
        d = english()
        d["@a.of"]["placeholders"]["0"]["type"] = "colour"
        self.bad(d, "type is one of")

    def test_plural_forms(self):
        d = english()
        d["a.tracks"] = {"one": "{0} track", "few": "{0} tracks", "other": "{0} tracks"}
        self.bad(d, "one and other")
        d = english()
        d["a.added"] = {"one": "Added {0}", "other": "Added"}
        self.bad(d, "the other form has every placeholder")
        d = english()
        d["@a.of"]["count"] = "0"
        self.bad(d, "isn't plural")

    def test_texts(self):
        d = english()
        d["a.plain"] = "Undo }"
        self.bad(d, "a brace")
        d = english()
        d["a.plain"] = "Un\tdo"
        self.bad(d, "control character")
        d = english()
        d["a.plain"] = "Un\ndo"
        self.bad(d, "2 lines, the room has 1")
        d = english()
        d["a.two"] = "1\n2\n3"
        self.bad(d, "3 lines, the room has 2")

    def test_buffers(self):
        # "bytes" and "line_bytes": the buffers on the device, a NUL in
        # them; the generator checks a text with no placeholders.
        d = english()
        d["@a.plain"]["bytes"] = 5
        checked(d)  # "Undo" and its NUL
        d["@a.plain"]["bytes"] = 4
        self.bad(d, "4 bytes of UTF-8, its buffer holds 3")
        d = english()
        d["@a.of"]["bytes"] = 2  # {0} of {1}: filled, test_ui_strings' to check
        checked(d)
        d = english()
        d["@a.two"]["line_bytes"] = 6
        self.bad(d, "a line of 6 bytes of UTF-8, its buffer holds 5")
        d["@a.two"]["line_bytes"] = 7
        checked(d)
        # A wrap room: its lines are where the fonts break it (test_ui_strings
        # measures them), and it says their buffer.
        d = english()
        d["@a.plain"]["room"] = {"font": "Small", "px": 40, "lines": 3, "layout": "wrap"}
        self.bad(d, "a wrap room needs line_bytes")
        d["@a.plain"]["line_bytes"] = 3  # (less than "Undo": not the generator's to measure)
        checked(d)
        d["@a.plain"]["room"] = [ROOM, {"font": "Small", "px": 40, "lines": 3, "layout": "wrap"}]
        checked(d)
        for bad in (0, 1, "8", 2.5, True):
            d = english()
            d["@a.plain"]["bytes"] = bad
            self.bad(d, "a buffer's size")

    def test_other_fills(self):
        # "or": the other messages a placeholder may hold.
        d = english()
        d["@a.of"]["placeholders"]["0"] = {"means": "m", "type": "message", "worst": "a.plain", "or": ["a.two"]}
        checked(d)
        d["@a.of"]["placeholders"]["0"]["or"] = ["a.nothing"]
        self.bad(d, "or names 'a.nothing'")
        d["@a.of"]["placeholders"]["0"]["or"] = ["a.of"]
        self.bad(d, "the message itself")
        d["@a.of"]["placeholders"]["0"]["or"] = ["a.two", "a.two"]
        self.bad(d, "each once")
        d["@a.of"]["placeholders"]["0"]["or"] = ["a.plain"]  # the worst again
        self.bad(d, "each once")
        d["@a.of"]["placeholders"]["0"]["or"] = []
        self.bad(d, "a list of message ids")
        d["@a.of"]["placeholders"]["0"] = {"means": "m", "type": "digits", "worst": 9, "or": ["a.two"]}
        self.bad(d, "or is for a text or a message")
        d["@a.of"]["placeholders"]["0"] = {"means": "m", "type": "text", "worst": "SD", "or": ["a.plain"]}
        checked(d)

    def test_rooms(self):
        d = english()
        d["@a.plain"]["room"] = None  # a fragment
        checked(d)
        d["@a.plain"]["room"] = [ROOM, {"font": "Small", "px": 50, "optional": True}]
        checked(d)
        d["@a.plain"]["room"] = []
        self.bad(d, "empty list")
        # Lines only where every room places them.
        d = english()
        d["@a.two"]["room"] = [{"font": "Small", "px": 100, "lines": 2}, ROOM]
        self.bad(d, "the room has 1")


class Translations(unittest.TestCase):
    def bad(self, tag, messages, words):
        with self.assertRaises(g.Bad) as e:
            translate(tag, messages)
        self.assertIn(words, str(e.exception))

    def test_a_good_translation(self):
        out = translate("de", {"a.plain": "Rückgängig", "a.of": "{1}: {0}",
                               "a.tracks": {"one": "{0} Titel", "other": "{0} Titel"}})
        self.assertEqual(out["a.of"], "{1}: {0}")
        self.assertNotIn("a.two", out)  # left out: English on the device

    def test_header(self):
        eng, _ = checked(english())
        with self.assertRaises(g.Bad):
            g.check_translation("de", {"@@locale": "fr", "@@autonym": "x"}, eng)
        with self.assertRaises(g.Bad):
            g.check_translation("de", {"@@locale": "de"}, eng)
        g.check_translation("de", {"@@locale": "de", "@@autonym": "Deutsch", "@@note": "machine"}, eng)
        # The name in itself is the mobile app's, never another spelling.
        with self.assertRaises(g.Bad) as e:
            g.check_translation("de", {"@@locale": "de", "@@autonym": "German"}, eng)
        self.assertIn("'Deutsch'", str(e.exception))

    def test_ids_and_kinds(self):
        self.bad("de", {"a.nothing": "x"}, "no such id")
        self.bad("de", {"a.tracks": "{0} Titel"}, "forms by category")
        self.bad("de", {"a.plain": {"one": "x", "other": "y"}}, "a string")
        self.bad("de", {"@a.plain": {"why": "x"}}, "a note and an overflow mark only")
        translate("de", {"@a.plain": {"note": "kurz"}})

    def test_overflow_marks(self):
        # A text too wide in the language until its layout is fixed: the
        # width test lets it be (catalogue.h's kLangOverflows).
        eng, _ = checked(english())
        marks = set()
        data = {"@@locale": "de", "@@autonym": "Deutsch", "a.plain": "Rückgängig machen",
                "@a.plain": {"note": "zu lang", "overflow": True}}
        g.check_translation("de", data, eng, marks)
        self.assertEqual(marks, {"a.plain"})
        self.bad("de", {"a.plain": "x", "@a.plain": {"overflow": "yes"}}, "true, or left out")
        self.bad("de", {"@a.plain": {"overflow": True}}, "hasn't the text")

    def test_placeholders(self):
        self.bad("de", {"a.of": "{0} von"}, "placeholders [0]")
        self.bad("de", {"a.of": "{0} von {1} {2}"}, "English has [0, 1]")  # {2}: one too many

    def test_plural_categories(self):
        # de: one and other, no more.
        self.bad("de", {"a.tracks": {"one": "{0}", "few": "{0}", "other": "{0}"}}, "isn't a category of de")
        # ru, pl: one, few, many needed; other optional.
        self.bad("ru", {"a.tracks": {"one": "{0}", "many": "{0}", "other": "{0}"}}, "['few'] missing")
        translate("ru", {"a.tracks": {"one": "{0} трек", "few": "{0} трека", "many": "{0} треков"}})
        translate("pl", {"a.tracks": {"one": "{0} utwór", "few": "{0} utwory", "many": "{0} utworów",
                                      "other": "{0} utworu"}})
        # fr: many optional; ja: other only.
        translate("fr", {"a.tracks": {"one": "{0} morceau", "other": "{0} morceaux"}})
        translate("fr", {"a.tracks": {"one": "{0} morceau", "many": "{0} de morceaux", "other": "{0} morceaux"}})
        self.bad("ja", {"a.tracks": {"one": "{0}", "other": "{0} 曲"}}, "isn't a category of ja")
        translate("ja", {"a.tracks": {"other": "{0} 曲"}})

    def test_one_may_leave_out_the_count_only_where_it_is_1(self):
        translate("de", {"a.added": {"one": "Hinzugefügt", "other": "{0} hinzugefügt"}})
        translate("pl", {"a.added": {"one": "Dodano", "few": "Dodano {0}", "many": "Dodano {0}"}})
        # fr's and pt's one is 0 and 1, ru's 1, 21, 31...: the count must show.
        self.bad("fr", {"a.added": {"one": "Ajouté", "other": "{0} ajoutés"}}, "[one]")
        self.bad("ru", {"a.added": {"one": "Добавлен", "few": "{0}", "many": "{0}"}}, "[one]")

    def test_lines(self):
        translate("de", {"a.two": "erste\nzweite"})
        translate("de", {"a.two": "eine Zeile"})
        self.bad("de", {"a.two": "1\n2\n3"}, "3 lines")
        self.bad("de", {"a.plain": "Rück\ngängig"}, "2 lines")

    def test_buffers(self):
        eng, _ = checked(english({"@a.plain": {"context": "a button", "room": ROOM, "bytes": 11}}))
        data = {"@@locale": "ru", "@@autonym": "Русский", "a.plain": "Отменить"}  # 16 bytes
        with self.assertRaises(g.Bad) as e:
            g.check_translation("ru", data, eng)
        self.assertIn("ru.json a.plain: 16 bytes of UTF-8, its buffer holds 10", str(e.exception))
        data["a.plain"] = "Отмена"  # 12: still not
        with self.assertRaises(g.Bad):
            g.check_translation("ru", data, eng)
        data["a.plain"] = "Назад"  # 10
        g.check_translation("ru", data, eng)

    def test_characters_the_device_draws(self):
        # The fonts' (Latin, Greek, Cyrillic) and the CJK store's, composed.
        translate("ja", {"a.plain": "元に戻す", "a.of": "{1} 曲中 {0}"})
        translate("zh", {"a.plain": "撤销。「完成」、"})
        translate("de", {"a.plain": "Rückgängig"})  # NFD: composed as the device does
        # Fullwidth forms are folded to ASCII on the device: refused, named.
        with self.assertRaises(g.Bad) as e:
            translate("zh", {"a.plain": "撤销（全部），完成：", "a.two": "第一行\n第二行！"})
        msg = str(e.exception)
        for cp in ("U+FF08", "U+FF09", "U+FF0C", "U+FF1A", "U+FF01"):
            self.assertIn(cp, msg)
        self.assertIn("5 character(s)", msg)  # every one at once
        self.assertIn("drawn as the ASCII ','", msg)
        self.assertIn("a.two", msg)
        # A hanzi neither the fonts nor the store has (GB2312 level 2).
        with self.assertRaises(g.Bad) as e:
            translate("zh", {"a.plain": "锂电池"})
        self.assertIn("U+9502", str(e.exception))
        self.assertIn("tools/glyph_store.py", str(e.exception))
        self.bad("ja", {"a.plain": "ｶﾀｶﾅ"}, "halfwidth katakana")
        self.bad("ru", {"a.plain": "−"}, "the minus sign")

    def test_not_utf8_or_a_surrogate(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "de.json"
            path.write_bytes(b'{"@@locale": "de", "a.plain": "R\xe9sum\xe9"}')
            with self.assertRaises(g.Bad) as e:
                g.load(path)
            self.assertIn("de.json: not UTF-8", str(e.exception))
            path.write_text('{"@@locale": "de", "@@autonym": "Deutsch", "a.plain": "Weiter \\ud800"}', encoding="utf-8")
            eng, _ = checked(english())
            with self.assertRaises(g.Bad) as e:
                g.check_translation("de", g.load(path), eng)
            self.assertIn("de.json a.plain: a lone surrogate U+D800", str(e.exception))

    def test_unknown_language(self):
        self.assertNotIn("xx", g.LANGS)


class Pseudo(unittest.TestCase):
    def test_accented_longer_bracketed(self):
        p = g.pseudo("Tap {0} of {1}")
        self.assertTrue(p.startswith("[Ţáp {0} öf {1} "), p)
        self.assertIn("{0}", p)
        self.assertIn("{1}", p)
        self.assertTrue(p.endswith("]"))
        self.assertGreater(len(p), len("Tap {0} of {1}") * 1.3)

    def test_lines_kept(self):
        p = g.pseudo("one\ntwo")
        self.assertEqual(p.count("\n"), 1)
        self.assertTrue(all(line.startswith("[") and line.endswith("]") for line in p.split("\n")))

    def test_only_drawable_letters(self):
        # Latin-1 and Latin Extended-A: the fonts' (tools/vlw_font.py).
        for c in g.ACCENTS.values():
            self.assertTrue(0xC0 <= ord(c) <= 0x17F, c)


def unescape(literal):
    """A C string literal's bytes (adjacent literals joined), as the
    compiler reads it."""
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', literal)
    out = bytearray()
    for part in parts:
        i = 0
        while i < len(part):
            c = part[i]
            if c != "\\":
                out += c.encode("utf-8")
                i += 1
                continue
            n = part[i + 1]
            if n == "x":
                j = i + 2
                while j < len(part) and part[j] in "0123456789abcdefABCDEF":
                    j += 1
                out.append(int(part[i + 2:j], 16) & 0xFF)
                i = j
            elif n in "01234567":
                j = i + 1
                while j < len(part) and j < i + 4 and part[j] in "01234567":
                    j += 1
                out.append(int(part[i + 1:j], 8))
                i = j
            else:
                out += {"n": b"\n", '"': b'"', "\\": b"\\"}[n]
                i += 2
    return bytes(out)


class Tables(unittest.TestCase):
    def test_literal_reads_back(self):
        # NUL before a digit, a hex escape before a hex digit, quotes,
        # backslashes, line breaks, "??" (a trigraph's start).
        samples = ["15 s", "0", "é1", "éa", "a\"b\\c", "x\ny", "??!", "日本語", "{0} of {1}", ""]
        blob = b"".join(s.encode("utf-8") + b"\0" for s in samples)
        lit = g.c_bytes(blob)
        self.assertEqual(unescape(lit), blob)
        for piece in re.findall(r'"((?:[^"\\]|\\.)*)"', lit):
            self.assertNotIn("??", piece)

    def test_tables_share_texts_and_mark_the_missing(self):
        eng, order = checked(english())
        msgs = {m: eng[m].value for m in order}
        blob, offs = g.build_table("en", msgs, eng, order)
        self.assertEqual(blob[offs[0]:].split(b"\0")[0], b"Undo")
        plural = blob[offs[order.index("a.tracks")]:].split(b"\0")[:2]
        self.assertEqual(plural, [b"{0} track", b"{0} tracks"])
        de, de_offs = g.build_table("de", {"a.plain": "Undo"}, eng, order)
        self.assertEqual(de_offs[0], 0)
        self.assertTrue(all(o == 0xFFFF for o in de_offs[1:]))
        # A language's forms in its categories' order, a left-out one empty.
        ru = {"a.tracks": {"one": "1", "few": "2", "many": "5"}}
        ru_blob, ru_offs = g.build_table("ru", ru, eng, order)
        self.assertEqual(ru_blob[ru_offs[order.index("a.tracks")]:], b"1\x002\x005\x00\x00")

    def test_every_language_is_listed(self):
        # The ten, English first, each by its name in itself; one with no
        # catalogue has no blob (English is drawn).
        eng, order = checked(english())
        msgs = {m: eng[m].value for m in order}
        tables = {"en": g.build_table("en", msgs, eng, order)}
        data = g.gen_data(order, eng, tables, g.build_table(g.PSEUDO, msgs, eng, order))
        rows = re.findall(r'\{"([a-zA-Z-]+)", (.*?), (\w+), (\w+)\},', data)
        self.assertEqual([r[0] for r in rows], g.ORDER + [g.PSEUDO])
        self.assertEqual(rows[0][2:], ("kBlob_en", "kOff_en"))
        self.assertTrue(all(r[2:] == ("nullptr", "nullptr") for r in rows[1:-1]))
        self.assertEqual(rows[1][1], '"Deutsch"')
        self.assertEqual(unescape(rows[3][1]), "Français".encode("utf-8"))  # (a hex escape before "a": split)
        ids = g.gen_ids(order, eng, ["en"])
        self.assertIn("kLangs = 10;", ids)
        self.assertIn("kLangs = 11;", ids)
        self.assertEqual(list(g.AUTONYMS), g.ORDER + [g.PSEUDO])

    def test_the_tests_view(self):
        # catalogue.h: a placeholder's other fills, a message's buffers.
        d = english()
        d["@a.of"]["placeholders"]["0"] = {"means": "m", "type": "text", "worst": "SD", "or": ["a.plain", "a.two"]}
        d["@a.of"]["bytes"] = 48
        d["@a.two"]["line_bytes"] = 64
        eng, order = checked(d)
        view = g.gen_test(order, eng, ["en"], {})
        self.assertIn('{Fill::Text, "SD", 0, 0, uistr::S{}, 2, {uistr::S::a_plain, uistr::S::a_two}}', view)
        self.assertIn("false, 48, 0,\n", view)  # a.of's bytes
        self.assertIn("false, 0, 64,\n", view)  # a.two's line_bytes
        self.assertIn(f"uistr::S alt[{g.MAX_OR}];", view)

    def test_the_real_catalogue_is_current(self):
        files, *_ = g.generate()
        for path, text in files.items():
            self.assertTrue(path.exists(), path)
            self.assertEqual(path.read_text(encoding="utf-8"), text, f"{path}: run tools/gen_ui_strings.py")


if __name__ == "__main__":
    unittest.main()
