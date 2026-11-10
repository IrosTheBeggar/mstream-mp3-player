# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Tests for the mood filters' fixtures (test/fixtures/card/moods.json, as
card_fixtures.py makes them with tools/moodfilter.py) against mStream's own
predicate (docs/MOOD-FILTERS.md 4.1, 5.4, 9.1).

    python -m unittest discover -s tools -p "test_mood_vectors.py"

mStream's song predicate is ported below by hand from src/api/mood-filter.js
at mStream commit d7086996 (barOf(), bars(), scale() and moodPredicate(),
with majoritySet()'s HAVING): Python that prints the same SQL text for a
filter object. TestSplice holds the port to that text: against literals,
and, when an mStream checkout and node are here ($MSTREAM_DIR, else a folder
named mStream beside this repository or a folder above it), against
mood-filter.js itself, run in node over every filter below (a module that
doesn't load there fails the test; only no checkout or no node skips it),
bars off the grid and out of range among them. SQLite runs the
port's SQL over each row's raw scores in a track_moods table shaped as
mStream's (V80). Every row's `holds`, every filter's `matches` and every
album's verdict must be what moods.json says, and the word's predicate
(moodfilter.matches) must agree with SQL on every row and filter. The
fixtures flow one way (9.1): mStream takes no file from here, and a change
to its predicate is ported in by hand, with its commit.
"""
import json
import math
import os
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import card_fixtures as cf  # noqa: E402  (moods.json, tags-moods.bin's description)
import moodfilter as mf  # noqa: E402

FIX = Path(__file__).resolve().parent.parent / "test" / "fixtures" / "card"
MSTREAM_COMMIT = "d7086996"  # git -C mStream log -1 --format=%h -- src/api/mood-filter.js


def fixture():
    return json.loads((FIX / "moods.json").read_text(encoding="utf-8"))


# ---------------------------------------------------------------------------
# mStream src/api/mood-filter.js at d7086996, ported by hand (lines 44-48,
# 135-170, 174-182): the same SQL text for one filter object, which
# TestSplice.test_against_mood_filter_js checks in node when it can.
# ---------------------------------------------------------------------------
MOOD_THRESHOLD = 0.5
MOOD_THRESHOLD_MIN = 0.1
MOOD_THRESHOLD_MAX = 0.9


def js_round(x):
    """Math.round: half up."""
    return math.floor(x + 0.5)


def js_text(x):
    """A number as a JS template prints it (these bars: no exponent)."""
    return str(int(x)) if x == int(x) else repr(x)


def bar_of(threshold):
    t = threshold if isinstance(threshold, (int, float)) else float("nan")
    if not math.isfinite(t) or t < MOOD_THRESHOLD_MIN or t > MOOD_THRESHOLD_MAX:
        return MOOD_THRESHOLD
    return js_round(t * 100) / 100


def bars(threshold):
    t = bar_of(threshold)
    return {"on": ">= %s" % js_text(t), "off": "< %s" % js_text(js_round((1 - t) * 100) / 100)}


def scale(tm, value, lohi, b):
    lo, hi = lohi
    if value == "neutral":
        return "(%s.%s %s AND %s.%s %s)" % (tm, lo, b["off"], tm, hi, b["off"])
    if value == lo:
        return "(%s.%s %s AND %s.%s >= %s.%s)" % (tm, lo, b["on"], tm, lo, tm, hi)
    return "(%s.%s %s AND %s.%s > %s.%s)" % (tm, hi, b["on"], tm, hi, tm, lo)


def mood_predicate(moods, tm="tm"):
    b = bars(moods.get("threshold"))
    parts = []
    if moods.get("valence"):
        parts.append(scale(tm, moods["valence"], ["happy", "sad"], b))
    if moods.get("energy"):
        parts.append(scale(tm, moods["energy"], ["relaxed", "aggressive"], b))
    if moods.get("voice") == "instrumental":
        parts.append("%s.instrumental %s" % (tm, b["on"]))
    if moods.get("voice") == "vocals":
        parts.append("%s.instrumental %s" % (tm, b["off"]))
    for tag in moods.get("tags") or []:
        parts.append("%s.%s %s" % (tm, tag, b["on"]))
    return " AND ".join(parts) if parts else "1=1"


def majority_having(moods):
    return "HAVING 2 * SUM(CASE WHEN %s THEN 1 ELSE 0 END) > COUNT(*)" % mood_predicate(moods)


# ---------------------------------------------------------------------------
# The player's filter as mStream's request: its groups, and its level as the
# `threshold` the phone app sends (lib/objects/mood_filter.dart, MoodLevel),
# none for Standard; None when it filters nothing (normalizeMoods' null).
# ---------------------------------------------------------------------------
def moods_of(q):
    if not mf.active(q):
        return None
    f = mf.unpack(q)
    out = {k: f[k] for k in ("valence", "energy", "voice") if f[k]}
    if f["tags"]:
        out["tags"] = f["tags"]
    if f["level"] != "standard":
        out["threshold"] = mf.LEVELS[f["level"]]["on"]
    return out


# ---------------------------------------------------------------------------
# mood-filter.js itself, when there is a checkout and node to run it
# ---------------------------------------------------------------------------
NODE_SCRIPT = r"""
const m = await import(process.argv[1]);
let input = '';
for await (const c of process.stdin) { input += c; }
const out = JSON.parse(input).map((f) => ({
  predicate: m.moodPredicate(f),
  normalized: m.normalizeMoods(f),
  album: m.albumMoodClause(f, { clause: '1', params: [] }).sql,
}));
process.stdout.write(JSON.stringify(out));
process.exit(0);
"""


def mstream_checkout():
    """An mStream checkout holding src/api/mood-filter.js: $MSTREAM_DIR, else
    a folder named mStream beside this repository or a folder above it; None
    when there is none."""
    here = Path(__file__).resolve()
    for c in [os.environ.get("MSTREAM_DIR")] + [str(p / "mStream") for p in here.parents]:
        if c and (Path(c) / "src" / "api" / "mood-filter.js").is_file():
            return Path(c)
    return None


def checkout_commit(checkout):
    r = subprocess.run(["git", "-C", str(checkout), "log", "-1", "--format=%h", "--", "src/api/mood-filter.js"],
                       capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 and r.stdout.strip() else "unknown"


def run_mood_filter_js(checkout, filters):
    """(answers, why): mood-filter.js's moodPredicate, normalizeMoods and
    albumMoodClause for each filter object, run in node from an empty
    folder (it writes nothing); answers None and why when the module doesn't
    load (its packages not installed, say)."""
    js = checkout / "src" / "api" / "mood-filter.js"
    with tempfile.TemporaryDirectory(prefix="moodfilter_js_") as cwd:
        r = subprocess.run(["node", "--input-type=module", "-e", NODE_SCRIPT, js.as_uri()],
                           input=json.dumps(filters).encode("utf-8"), capture_output=True, cwd=cwd, timeout=120)
    if r.returncode != 0:
        return None, r.stderr.decode("utf-8", "replace").strip()[:500]
    return json.loads(r.stdout.decode("utf-8")), ""


class Library:
    """moods.json's rows in mStream's tables: track i is row i, with a
    track_moods row only when it has scores (an all-NULL row is a row)."""

    def __init__(self, rows):
        self.db = sqlite3.connect(":memory:")
        cols = ", ".join("%s REAL" % c for c in mf.MOOD_COLUMNS)
        self.db.execute("CREATE TABLE tracks (id INTEGER PRIMARY KEY, album_id INTEGER)")
        self.db.execute("CREATE TABLE track_moods (track_id INTEGER PRIMARY KEY REFERENCES tracks(id), %s)" % cols)
        for i, r in enumerate(rows):
            self.db.execute("INSERT INTO tracks (id) VALUES (?)", (i,))
            if r["scores"] is not None:
                self.db.execute("INSERT INTO track_moods VALUES (?, %s)" % ", ".join("?" * len(mf.MOOD_COLUMNS)),
                                [i] + r["scores"])
        self.n = len(rows)

    def songs(self, moods):
        """The rows a filter lets through: every track for no filter (no
        clause), else trackMoodClause's EXISTS."""
        if moods is None:
            return list(range(self.n))
        sql = ("SELECT t.id FROM tracks t WHERE EXISTS (SELECT 1 FROM track_moods tm WHERE tm.track_id = t.id AND "
               "%s) ORDER BY t.id" % mood_predicate(moods))
        return [i for (i,) in self.db.execute(sql)]

    def album(self, members, moods):
        """majoritySet over one album of these rows."""
        self.db.execute("UPDATE tracks SET album_id = NULL")
        self.db.executemany("UPDATE tracks SET album_id = 1 WHERE id = ?", [(i,) for i in members])
        sql = ("SELECT t.album_id FROM tracks t JOIN track_moods tm ON tm.track_id = t.id "
               "WHERE t.album_id IS NOT NULL GROUP BY t.album_id %s" % majority_having(moods))
        return bool(self.db.execute(sql).fetchall())


class TestSplice(unittest.TestCase):
    """The port prints what mood-filter.js prints."""

    def test_bars(self):
        self.assertEqual(bars(None), {"on": ">= 0.5", "off": "< 0.5"})
        self.assertEqual(bars(0.7), {"on": ">= 0.7", "off": "< 0.3"})  # not 0.30000000000000004
        self.assertEqual(bars(0.3), {"on": ">= 0.3", "off": "< 0.7"})
        self.assertEqual(bars(0.95), bars(None))
        self.assertEqual(bars(0.123), {"on": ">= 0.12", "off": "< 0.88"})  # rounded to the hundredth

    def test_predicate_text(self):
        self.assertEqual(mood_predicate({"valence": "sad", "energy": "relaxed", "voice": "vocals",
                                         "tags": ["danceable"]}),
                         "(tm.sad >= 0.5 AND tm.sad > tm.happy) AND (tm.relaxed >= 0.5 AND tm.relaxed >= "
                         "tm.aggressive) AND tm.instrumental < 0.5 AND tm.danceable >= 0.5")
        self.assertEqual(mood_predicate({"valence": "neutral", "threshold": 0.7}),
                         "(tm.happy < 0.3 AND tm.sad < 0.3)")
        self.assertEqual(mood_predicate({"valence": "happy", "threshold": 0.123}),
                         "(tm.happy >= 0.12 AND tm.happy >= tm.sad)")
        self.assertEqual(mood_predicate({"voice": "instrumental", "threshold": 0.95}), "tm.instrumental >= 0.5")
        self.assertEqual(mood_predicate({}), "1=1")

    def test_levels_are_the_bars(self):
        m = fixture()
        for lv in m["bars"]:
            b = bars(moods_of(mf.pack(voice="vocals", level=lv["level"])).get("threshold"))
            self.assertEqual(b, {"on": ">= %s" % lv["on"], "off": "< %s" % lv["mirror"]}, lv["level"])

    def test_against_mood_filter_js(self):
        # mood-filter.js run in node: its SQL for every filter here (the 33
        # one-group ones and moods.json's), the majority's HAVING, and bars
        # past the player's three, are the port's character for character;
        # and each request is already in its canonical form (normalizeMoods).
        # The bars past the player's: off the grid and out of range, on the
        # mirror bar (vocals, neutral) and on the 'on' bar (happy,
        # instrumental, a tag). With a checkout and node here, a module that
        # doesn't load fails: only their absence skips.
        checkout = mstream_checkout()
        if checkout is None or not shutil.which("node"):
            self.skipTest("no mStream checkout ($MSTREAM_DIR, or mStream beside this repository) or no node")
        m = fixture()
        qs = [int(p["q"], 16) | lv["code"] << 9 for p in m["predicates"] for lv in m["levels"]]
        qs += [int(f["q"], 16) for f in m["filters"]]
        requests = [moods_of(q) for q in qs if mf.active(q)]
        extra = [{"voice": "vocals", "threshold": t} for t in (0.95, 0.05, 0.123, 0.1, 0.9, "x")]
        extra += [{"valence": "happy", "threshold": t} for t in (0.123, 0.95, 0.876)]
        extra += [{"voice": "instrumental", "threshold": t} for t in (0.95, 0.123)]
        extra += [{"tags": ["acoustic"], "threshold": 0.123}, {"valence": "sad", "threshold": 0.345}]
        extra += [{"valence": "neutral", "energy": "aggressive", "tags": ["party"], "threshold": 0.35}]
        got, why = run_mood_filter_js(checkout, requests + extra)
        if got is None:
            self.fail("mood-filter.js at %s doesn't load in node: %s" % (checkout, why))
        at = "port of %s, checkout at %s" % (MSTREAM_COMMIT, checkout_commit(checkout))
        self.assertEqual(len(got), len(requests + extra))
        for f, js in zip(requests + extra, got):
            self.assertEqual(mood_predicate(f), js["predicate"], (f, at))
            self.assertTrue(js["album"].endswith(majority_having(f) + ")"), (f, js["album"], at))
        for f, js in zip(requests, got):
            self.assertEqual(js["normalized"], f, at)


class TestMoodVectors(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.m = fixture()
        cls.lib = Library(cls.m["rows"])

    def test_made_by_the_generator(self):
        # moods.json is generate()'s, so --check keeps it frozen.
        self.assertEqual(json.loads(cf.generate()["moods.json"]), self.m)

    def test_heads(self):
        self.assertEqual(self.m["heads"], ["danceable", "happy", "sad", "relaxed", "aggressive", "party", "acoustic",
                                           "instrumental"])

    def test_holds_against_sql(self):
        # Every row's 33 checks: those listed hold in SQLite, the others don't.
        names = [p["name"] for p in self.m["predicates"]]
        for lv in self.m["levels"]:
            for p in self.m["predicates"]:
                q = int(p["q"], 16) | lv["code"] << 9
                got = set(self.lib.songs(moods_of(q)))
                for i, r in enumerate(self.m["rows"]):
                    self.assertEqual(p["name"] in r["holds"][lv["level"]], i in got,
                                     "%s at %s: row %d, %s" % (p["name"], lv["level"], i, r["note"]))
        for r in self.m["rows"]:
            for lv, held in r["holds"].items():
                self.assertEqual(held, [n for n in names if n in held], r["note"])

    def test_word_agrees_with_sql(self):
        # The word's predicate against SQL, on every row and every filter:
        # the one-group ones at each level and moods.json's own.
        qs = [int(p["q"], 16) | lv["code"] << 9 for p in self.m["predicates"] for lv in self.m["levels"]]
        qs += [int(f["q"], 16) for f in self.m["filters"]]
        for q in qs:
            got = self.lib.songs(moods_of(q))
            words = [i for i, r in enumerate(self.m["rows"]) if mf.matches(int(r["word"], 16), q)]
            self.assertEqual(words, got, "q 0x%03X" % q)

    def test_filters(self):
        for f in self.m["filters"]:
            q = int(f["q"], 16)
            self.assertEqual(self.lib.songs(moods_of(q)), f["matches"], f["note"])
            self.assertEqual((mf.q_valid(q), int(f["reads"], 16), mf.active(q)), (f["valid"], mf.read_q(q),
                                                                                   f["active"]), f["note"])

    def test_albums_against_sql(self):
        for a in self.m["albums"]:
            moods = moods_of(int(a["q"], 16))
            self.assertEqual(self.lib.album(a["rows"], moods), a["matches"], a["note"])
            scored = [i for i in a["rows"] if self.m["rows"][i]["scores"] is not None]
            self.assertEqual(len(scored), a["analysed"], a["note"])
            self.assertEqual(len(set(scored) & set(self.lib.songs(moods))), a["pass"], a["note"])

    def test_rows_tail_and_word(self):
        for r in self.m["rows"]:
            tail = mf.tail_of(r["scores"])
            self.assertEqual(cf.spaced(tail), r["tail"], r["note"])
            self.assertEqual(mf.word_of_tail(tail), int(r["word"], 16), r["note"])
            self.assertEqual(mf.holds(int(r["word"], 16)), r["holds"], r["note"])

    def test_worked_example(self):
        # MOOD-FILTERS.md 5.7.
        r = next(r for r in self.m["rows"] if "song.flac" in r["note"])
        self.assertEqual((r["tail"], r["word"]), ("03 3A 46 1D 31 FF 46 00", "0xB313"))
        moods = moods_of(0x127)
        self.assertEqual(moods, {"valence": "sad", "energy": "relaxed", "voice": "vocals", "tags": ["danceable"]})
        self.assertIn(self.m["rows"].index(r), self.lib.songs(moods))

    def test_tails(self):
        for t in self.m["tails"]:
            b = bytes.fromhex(t["tail"])
            self.assertEqual((mf.tail_valid(b), mf.word_of_tail(b)), (t["valid"], int(t["word"], 16)), t["note"])
        # An order bit beside a 255 is ignored (5.3), not a bad tail.
        odd = next(t for t in self.m["tails"] if t["tail"] == "03 FF 46 1D 31 FF 46 00")
        self.assertTrue(odd["valid"])
        self.assertEqual(int(odd["word"], 16) >> mf.VALENCE_AT & 7, mf.SCALE_NULL)


class TestMoodFilter(unittest.TestCase):
    """tools/moodfilter.py's rules, apart from the fixture."""

    def test_value_rule(self):
        for x, want in ((0.29, 29), (0.57, 57), (0.58, 58), (0, 0), (1, 100), (0.7, 70), (0.6999, 69),
                        (1.5, 100), (-0.25, 0), (None, 255)):
            self.assertEqual(mf.value(x), want, x)
        # The slack is needed: a plain floor gets 0.29 wrong.
        self.assertEqual(math.floor(0.29 * 100), 28)

    def test_value_rule_on_the_grid(self):
        # 4 places, as the server writes them: the bucket agrees with the
        # raw comparison at every bar (5.4: exact at the three bars).
        for k in range(10001):
            x = k / 10000
            for bar, edge in ((0.3, 30), (0.5, 50), (0.7, 70)):
                self.assertEqual(mf.value(x) >= edge, x >= bar, x)

    def test_no_row_and_nulls(self):
        self.assertEqual(mf.tail_of(None), bytes(8))
        self.assertEqual(mf.word_of(None), 0)
        self.assertEqual(mf.tail_of([None] * 8), bytes([1, 255, 255, 255, 255, 255, 255, 0]))
        self.assertEqual(mf.word_of([None] * 8), 0xE400)
        # An analysed row passes no active filter's NULL scale, and a word of
        # 0 passes no active filter at all.
        for name, _ in mf.PREDICATES:
            for lv in mf.LEVELS:
                self.assertFalse(mf.matches(0, mf.predicate_q(name, lv)))

    def test_order_bits(self):
        tail = mf.tail_of([0.5, 0.6, 0.6, 0.4, 0.40001, 0.5, 0.5, 0.5])
        self.assertEqual(tail[0], mf.PRESENT | mf.AGGRESSIVE_WINS)  # a tie is happy's; 0.40001 beats 0.4
        self.assertEqual(mf.tail_of([0.5, None, 0.9, 0.4, 0.5, 0.5, 0.5, 0.5])[0], 0x05)  # no bit beside a NULL

    def test_q_words(self):
        self.assertEqual(mf.pack("sad", "relaxed", "vocals", ["danceable"]), 0x127)
        for bad in (0x800, 0x30, 0x600, 0xFFFF, -1):
            self.assertFalse(mf.q_valid(bad), bad)
            self.assertEqual(mf.read_q(bad), 0)
        self.assertFalse(mf.active(0x200))
        self.assertTrue(mf.active(0x201))
        for q in range(1 << 11):
            if mf.q_valid(q):
                f = mf.unpack(q)
                self.assertEqual(mf.pack(f["valence"], f["energy"], f["voice"], f["tags"], f["level"]), q)

    def test_every_word_against_the_raw_rule(self):
        # On a grid of scores every predicate at every level reads the same
        # from the word as from the raw doubles.
        grid = [None, 0, 0.2999, 0.3, 0.4999, 0.5, 0.6999, 0.7, 1]
        for a in grid:
            for b in grid:
                scores = [a, a, b, b, a, b, a, b]
                w = mf.word_of(scores)
                for name, _ in mf.PREDICATES:
                    for lv in mf.LEVELS:
                        q = mf.predicate_q(name, lv)
                        self.assertEqual(mf.matches(w, q), mf.matches_scores(scores, q), (scores, name, lv))

    def test_album_majority(self):
        yes, no = mf.word_of([0.9, 0.1, 0.8, 0.75, 0.05, 0.2, 0.3, 0.1]), mf.word_of([0.1] * 8)
        q = 0x127
        self.assertTrue(mf.album_matches([yes] * 5 + [no] * 4, q))
        self.assertFalse(mf.album_matches([yes] * 2 + [no] * 2, q))
        self.assertFalse(mf.album_matches([], q))
        self.assertFalse(mf.album_matches([0, 0], q))
        self.assertTrue(mf.album_matches([0, 0], 0))


if __name__ == "__main__":
    unittest.main()
