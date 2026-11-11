# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Tests for tools/statsref.py (the listening stats' reference,
docs/LISTENING-STATS.md).

    python -m unittest discover -s tools -p "test_statsref.py"

- What counts (4.1): the line (an odd durationMs's half not rounded,
  durationMs 0), skips, the outcomes and sources read as the plan says,
  the dated range's ends, the hour, day and time of the worked example.
- The merge (5.2): every order of the same slots gives the same row
  (firstAt's dated, undated and none, the source on a tie, saturation).
- The files (5.2, 5.3): stats.bin and a base read back as written; the
  frame the same bytes as card_fixtures.build_container's; each check a
  reader makes (5.4's verdicts: newer for Major and UnknownRequired,
  corrupt for the rest), a newer minor's OPTIONAL section and longer rows.
- The boot (5.4-5.6): the worked example (5.13) by hand; each slot once;
  the gap counted once; a failed save trims by the boot's MARK; a newer
  stats.bin turns the stats off; a corrupt one goes to stats.bad; a lost
  base; the bases either side of the named one removed; the live fold's
  round and a rotation's save, which never compacts; the compaction's
  rule; retention (HOUR, DAYS, RCNT by date and by count); sinceDay.
- The rankers and windows (4.2, 4.3, 7.6): each shelf's rule and ties,
  the caps plus 16, the windows' edges, no lists that need dates without
  a clock, Not played yet, the tray, the week.
- The fixtures (test/fixtures/card/stats) as card_fixtures.py makes them,
  and the plan's own figures in them.
- seed: its refusals (no SYNTHCARD.TXT, a root another program wrote or
  none, the system drive, not FAT32, files already there, a twin),
  --dry-run, a history and a full log, --base; the command line.
"""
import contextlib
import io
import itertools
import json
import os
import shutil
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import card_fixtures as cf  # noqa: E402
import playlog as pl  # noqa: E402
import statsref as sr  # noqa: E402

FIXTURES = Path(__file__).resolve().parent.parent / "test" / "fixtures" / "card"
A, E = sr.DEV_A, sr.EPOCH_A


def play(seq, track="Night Ticket", played=200000, duration=200000, **kw):
    return sr.slot(A, E, seq, track, played, duration, **kw)


def files_of(*slots, name="plays.jnl"):
    return {name: sr.log(*slots)}


class RuleTests(unittest.TestCase):
    def test_the_line(self):
        s = lambda played, dur, outcome=3: play(1, played=played, duration=dur, outcome=outcome)  # noqa: E731
        self.assertFalse(sr.is_play(s(20000, 40001)))      # 20,000.5 is the line
        self.assertTrue(sr.is_play(s(20001, 40001)))
        self.assertTrue(sr.is_play(s(20000, 40000)))
        self.assertFalse(sr.is_play(s(19999, 40000)))
        self.assertTrue(sr.is_play(s(30000, 0)))          # durationMs 0: 30 s
        self.assertFalse(sr.is_play(s(29999, 0)))
        self.assertTrue(sr.is_play(s(30000, 10 ** 7)))     # 30 s whatever the length
        self.assertFalse(sr.is_play(s(29999, 10 ** 7)))
        self.assertTrue(sr.is_play(s(1, 2)))               # a 2 ms track: 1 ms
        self.assertTrue(sr.is_play(s(31000, 100000, outcome=2)), "a skip over the line is a play")

    def test_skips_outcomes_sources(self):
        self.assertTrue(sr.is_skip(play(1, played=5000, outcome=2)))
        for outcome in (0, 1, 3, 4, 200):
            self.assertFalse(sr.is_skip(play(1, played=5000, outcome=outcome)), outcome)
        self.assertEqual([sr.source(play(1, src=v)) for v in (0, 1, 2, 3, 4, 255)],
                         [sr.YOU, sr.YOU, sr.SHUFFLE, sr.AUTODJ, sr.YOU, sr.YOU])

    def test_counts(self):
        self.assertTrue(sr.counts(play(1)))
        self.assertFalse(sr.counts(play(0)))
        self.assertFalse(sr.counts(sr.slot(A, 0, 1, "Brine", 1, 1)))
        self.assertFalse(sr.counts(sr.slot("000000000000", E, 1, "Brine", 1, 1)))
        self.assertFalse(sr.counts({"kind": pl.KIND_DROP}))

    def test_dated_and_times(self):
        self.assertTrue(sr.dated(play(1, startedAt=sr.MIN_MS)))
        self.assertTrue(sr.dated(play(1, startedAt=sr.MAX_MS)))
        self.assertFalse(sr.dated(play(1, startedAt=sr.MIN_MS - 1)))
        self.assertFalse(sr.dated(play(1, startedAt=sr.MAX_MS + 1)))
        self.assertFalse(sr.dated(play(1, startedAt=0)))
        ms = sr.ms_of("2026-11-08T21:40:00")             # 5.13's slot 42
        self.assertEqual((sr.day_of(ms), sr.hour_of(ms), sr.time_of(ms)), (311, 7485, 26948401))
        self.assertEqual(sr.day_of_time(sr.time_of(ms)), 311)
        self.assertEqual(sr.time_of(sr.MAX_MS), 2335219200)  # under NO_FIRST
        self.assertLess(sr.day_of(sr.MAX_MS), sr.UNSET_DAY)
        self.assertLess(sr.hour_of(sr.MAX_MS), 1 << 24)
        self.assertIsNone(sr.today_of(None))
        self.assertIsNone(sr.today_of(1000))


class MergeTests(unittest.TestCase):
    def rows(self):
        t = sr.time_of(sr.ms_of("2026-11-03T10:00:00"))
        return [sr.Row(1, 0, 0, 0, 3, sr.AUTODJ), sr.Row(1, 0, t + 50, t + 50, 4, sr.SHUFFLE),
                sr.Row(0, 1, sr.NO_FIRST, 0, 0, sr.YOU), sr.Row(1, 0, 0, 0, 5, sr.SHUFFLE),
                sr.Row(1, 0, t, t, 2, sr.AUTODJ), sr.Row(1, 0, t, t, 1, sr.YOU)]

    def test_every_order_the_same_row(self):
        rows = self.rows()
        results = set()
        for perm in itertools.permutations(rows):
            acc = perm[0]
            for r in perm[1:]:
                acc = sr.merge(acc, r)
            results.add(acc.tuple())
        t = sr.time_of(sr.ms_of("2026-11-03T10:00:00"))
        self.assertEqual(results, {(5, 1, t, t + 50, 5, sr.YOU)})  # the earlier dated time; on a tie the smaller source

    def test_first_at(self):
        undated_a, undated_s = sr.Row(1, 0, 0, 0, 1, sr.AUTODJ), sr.Row(1, 0, 0, 0, 1, sr.SHUFFLE)
        skip = sr.Row(0, 1, sr.NO_FIRST, 0, 0, sr.YOU)
        self.assertEqual(sr.merge(undated_a, undated_s).tuple()[2:], (0, 0, 1, sr.SHUFFLE))
        self.assertEqual(sr.merge(skip, undated_a).tuple(), (1, 1, 0, 0, 1, sr.AUTODJ))
        self.assertEqual(sr.merge(skip, skip).tuple(), (0, 2, sr.NO_FIRST, 0, 0, sr.YOU))
        dated = sr.Row(1, 0, 99, 99, 2, sr.AUTODJ)
        self.assertEqual(sr.merge(undated_s, dated).tuple(), (2, 0, 99, 99, 2, sr.AUTODJ))

    def test_saturates(self):
        r = sr.merge(sr.Row(65535, 65000, 1, 1, 1, 0), sr.Row(1, 1000, 1, 1, 1, 0))
        self.assertEqual((r.plays, r.skips), (65535, 65535))

    def test_slot_rows(self):
        t = sr.ms_of("2026-11-08T21:40:00")
        self.assertEqual(sr.slot_row(play(1, src=3, startedAt=t, clock=1), 7).tuple(),
                         (1, 0, 26948401, 26948401, 7, sr.AUTODJ))
        self.assertEqual(sr.slot_row(play(1, src=2), 7).tuple(), (1, 0, 0, 0, 7, sr.SHUFFLE))
        self.assertEqual(sr.slot_row(play(1, played=5000, outcome=2), 7).tuple(), (0, 1, sr.NO_FIRST, 0, 0, sr.YOU))
        self.assertIsNone(sr.slot_row(play(1, played=5000, outcome=1), 7))


def stats_with_rows():
    se = sr.Session({})
    t = lambda d, h: sr.ms_of("2026-11-%02dT%02d:00:00" % (d, h))  # noqa: E731
    slots = [play(1, "Night Ticket", startedAt=t(1, 9), clock=1, src=3),
             play(2, "Brine", played=5000, outcome=2, startedAt=t(1, 10), clock=1),
             play(3, "Brine", startedAt=t(2, 9), clock=1, src=2),
             play(4, "Low Tide", played=1000, startedAt=t(2, 10), clock=1),
             play(5, "Glass Harbour"),
             sr.slot(sr.DEV_B, sr.EPOCH_B, 7, "Orbit Room", 200000, 200000, at="2026-11-03T08:00:00")]
    for s in slots:
        se.live(s)
    return se.stats


class FileTests(unittest.TestCase):
    def test_round_trip(self):
        st = stats_with_rows()
        st.generation = 3
        st.head.update(sinceDay=300, flags=1, round=9, baseGen=2, baseRows=7, baseCrc=0xDEADBEEF, twice=4, gap=5)
        b = sr.write_stats(st)
        back = sr.read_stats(b)
        self.assertEqual(sr.write_stats(back), b)
        self.assertEqual(sr.stats_json(back, True), sr.stats_json(st, True) | {"base": []})
        base = sr.write_base(st.trkd, 2)
        self.assertEqual(sr.read_base(base, 2), st.trkd)
        with self.assertRaises(sr.Bad) as e:
            sr.read_base(base, 3)
        self.assertEqual(e.exception.why, "Counts")

    def test_layout(self):
        st = stats_with_rows()
        b = sr.write_stats(st)
        magic, major, minor, hb, n, fb, gen, card, _, _ = struct.unpack_from("<4sHHIIIIQII", b, 0)
        self.assertEqual((magic, major, minor, hb, n, fb, card), (b"MPST", 1, 0, 40, 6, len(b), 0))
        types = [struct.unpack_from("<4sI", b, 40 + 32 * i) for i in range(6)]
        self.assertEqual(types, [(t, 1) for t in (b"HEAD", b"MARK", b"HOUR", b"DAYS", b"RCNT", b"TRKD")])
        head = struct.unpack_from("<4sIIIIII", b, 40)
        self.assertEqual((head[3], head[4], head[5]), (128, 0, 0))   # a blob: count and stride 0
        # HEAD's offsets (5.2): sinceDay at 84, round at 88.
        st.head.update(sinceDay=0x1234, round=0x56789A)
        h = sr.head_bytes(st.head)
        self.assertEqual(struct.unpack_from("<HHI", h, 84), (0x1234, 0, 0x56789A))
        self.assertEqual(h[112:], bytes(16))
        # A TRKS row: lastOrd u24 at 20, flags at 23.
        row = sr.rows_bytes({5: sr.Row(1, 2, 3, 4, 0xABCDEF, sr.AUTODJ)})
        self.assertEqual(row, struct.pack("<QHHII", 5, 1, 2, 3, 4) + bytes([0xEF, 0xCD, 0xAB, 2]))

    def test_the_same_frame_as_card_fixtures(self):
        st = stats_with_rows()
        st.generation = 5
        secs = [(b"HEAD", 0, sr.head_bytes(st.head)), (b"TRKD", 24, sr.rows_bytes(st.trkd)), (b"RCNT", 12, b"")]
        mine = sr.frame(5, secs)
        theirs = cf.build_container("MPST", 5, 0, b"", [(t, 1, len(d) // w if w else 0, w, d) for t, w, d in secs])
        self.assertEqual(mine, theirs)

    def reseal(self, b):
        return cf.reseal(bytearray(b))

    def test_checks_and_verdicts(self):
        good = sr.write_stats(stats_with_rows())

        def why(b):
            try:
                sr.read_stats(bytes(b))
                return None
            except sr.Bad as e:
                return e.why
        self.assertIsNone(why(good))
        self.assertEqual(why(good[:30]), "Short")
        b = bytearray(good)
        b[0:4] = b"MPSX"
        self.assertEqual(why(b), "Magic")
        b = bytearray(good)
        struct.pack_into("<H", b, 4, 2)
        self.assertEqual(why(self.reseal(b)), "Major")
        self.assertEqual(sr.verdict("Major"), "newer")
        b = bytearray(good)
        struct.pack_into("<H", b, 6, 7)
        self.assertEqual(why(self.reseal(b)), None, "a newer minor reads")
        b = bytearray(good)
        b[33] ^= 1
        self.assertEqual(why(b), "HeaderCrc")
        self.assertEqual(why(good + bytes(8)), "FileBytes")
        b = bytearray(good)
        struct.pack_into("<I", b, 40 + 32 * 5 + 4, 1)     # TRKD's flags: still REQUIRED
        b[40 + 32 * 5:40 + 32 * 5 + 4] = b"TRKX"          # a type this reader doesn't know
        self.assertEqual(why(self.reseal(b)), "UnknownRequired")
        self.assertEqual(sr.verdict("UnknownRequired"), "newer")
        struct.pack_into("<I", b, 40 + 32 * 5 + 4, 0)     # the same section OPTIONAL: skipped, TRKD missing
        self.assertEqual(why(self.reseal(b)), "Missing")
        b = bytearray(good)
        struct.pack_into("<I", b, 40 + 32 * 2 + 20, 4)     # HOUR's stride under 8
        self.assertEqual(why(self.reseal(b)), "Shape")
        b = bytearray(good)
        off = struct.unpack_from("<I", b, 40 + 32 * 2 + 8)[0]
        b[off] ^= 1                                        # HOUR's bytes, its CRC not made again
        self.assertEqual(why(b), "SectionCrc")
        b = bytearray(good)
        struct.pack_into("<I", b, 40 + 32 * 1 + 8, struct.unpack_from("<I", b, 40 + 32 * 1 + 8)[0] + 8)
        self.assertEqual(why(self.reseal(b)), "Layout")
        for w in ("Short", "Magic", "HeaderCrc", "Layout", "Missing", "Shape", "SectionCrc", "Order", "Counts"):
            self.assertEqual(sr.verdict(w), "corrupt", w)

    def test_order(self):
        st = stats_with_rows()
        b = bytearray(sr.write_stats(st))
        off, nb = struct.unpack_from("<II", b, 40 + 32 * 5 + 8)  # TRKD: swap its first two rows
        rows = bytes(b[off:off + nb])
        b[off:off + 48] = rows[24:48] + rows[0:24]
        with self.assertRaises(sr.Bad) as e:
            sr.read_stats(self.reseal(b))
        self.assertEqual(e.exception.why, "Order")
        b = bytearray(sr.write_stats(st))
        off = struct.unpack_from("<I", b, 40 + 8)[0]
        with self.assertRaises(sr.Bad) as e:          # HEAD shorter than 128 bytes
            sr.read_stats(sr.frame(0, [(b"HEAD", 0, bytes(120))] + [(t, w, b"") for t, w in sr.STATS_SECTIONS[1:]]))
        self.assertEqual(e.exception.why, "Shape")

    def test_newer_minor(self):
        """5.12: a newer minor adds OPTIONAL sections and longer rows; this
        reader takes its sections' known prefixes and skips the rest."""
        st = stats_with_rows()
        longer = lambda rows, w: b"".join(rows[i:i + w] + b"\xEE" * 8 for i in range(0, len(rows), w))  # noqa: E731
        secs = [(b"HEAD", 0, sr.head_bytes(st.head) + bytes(32))]
        for (t, w), data in zip(sr.STATS_SECTIONS[1:], sr.write_stats(st) and [
                b"".join(sr.MARK_ROW.pack(d, 0, e, s, 0) for (d, e), s in sorted(st.mark.items())),
                b"".join(sr.HOUR_ROW.pack(k | v[0] << 24, v[1]) for k, v in sorted(st.hour.items())),
                b"".join(sr.DAYS_ROW.pack(k, *v) for k, v in sorted(st.days.items())),
                b"".join(sr.RCNT_ROW.pack(d, n, h) for (d, h), n in sorted(st.rcnt.items())),
                sr.rows_bytes(st.trkd)]):
            secs.append((t, w + 8, longer(data, w)))
        sections = [(t, 1, len(d) // w if w else 0, w, d) for t, w, d in secs] + [(b"XTRA", 0, 2, 4, bytes(8))]
        b = cf.build_container("MPST", 4, 0, b"", sections, 1, 3)
        back = sr.read_stats(b)
        self.assertEqual(sr.stats_json(back, True), sr.stats_json(st, True) | {"generation": 4, "base": []})


class BootTests(unittest.TestCase):
    def test_worked_example(self):
        """5.13 by hand, against the fixture's scenario."""
        sc = sr.scenario_worked()
        r = sr.boot(sc["files"], **self.args(sc["boots"][0]))
        st = r["stats"]
        self.assertEqual((r["folded"], r["saved"], r["trimmed"]), (7, True, ["plays.1.jnl"]))
        self.assertEqual(st.mark, {(bytes.fromhex(A), E): 48})
        self.assertIn("plays.jnl", r["files"])
        y = st.track(sr.TRACKS["Orbit Room"][1])
        t44 = sr.time_of(sr.ms_of("2026-11-08T21:45:00"))
        self.assertEqual((y.plays, y.skips, y.firstAt, y.lastAt), (1, 1, t44, t44))
        x = st.track(sr.TRACKS["Weather Vane"][1])
        self.assertEqual((x.firstAt, x.src), (26948401, sr.AUTODJ))
        self.assertIsNone(st.track(sr.TRACKS["Low Tide"][1]), "45: neither play nor skip, no row")
        self.assertEqual(st.hour[7485], [2, 120000 + 12000 + 25000 + 8000])  # 42-45 in 21:00 of day 311
        self.assertEqual(st.days[311][:5], [2, 1, 1, 0, 165000])
        r2 = sr.boot(r["files"], **self.args(sc["boots"][1]))
        self.assertEqual((r2["folded"], r2["saved"], r2["trimmed"]), (0, False, []))
        self.assertEqual(r2["files"], r["files"])

    def args(self, b):
        return dict(acks=b["acks"], now=b["now"], commit_time=b.get("commitTime"), playing=b["playing"],
                    safe_point=b["safePoint"], save_fails=b.get("saveFails", False))

    def test_each_slot_once(self):
        slots = [play(q, startedAt=sr.MIN_MS + q * 3600000, clock=1) for q in range(1, 5)]
        files = {"plays.jnl": sr.log(*slots[2:]), "plays.1.jnl": sr.log(*slots), "plays.2.jnl": sr.log(*slots[:2])}
        r = sr.boot(files, now=sr.MIN_MS + 10 * sr.DAY_MS)
        self.assertEqual((r["folded"], r["twice"], r["stats"].head["plays"]), (4, 4, 4))
        same = files_of(*(slots + slots[3:]))                 # one file twice: skipped, not counted in twice
        r = sr.boot(same, now=sr.MIN_MS + 10 * sr.DAY_MS)
        self.assertEqual((r["folded"], r["twice"]), (4, 0))
        conflict = play(4, "Brine", startedAt=sr.MIN_MS, clock=1)
        r = sr.boot({"plays.jnl": sr.log(conflict), "plays.1.jnl": sr.log(*slots)}, now=sr.MIN_MS)
        self.assertEqual(r["stats"].track(sr.TRACKS["Brine"][1]).plays, 1, "the first read kept")
        self.assertEqual(r["stats"].track(sr.TRACKS["Night Ticket"][1]).plays, 3)

    def test_above_mark_only_and_every_device(self):
        before = sr.earlier([play(q) for q in range(1, 4)])
        b = sr.slot(sr.DEV_B, sr.EPOCH_B, 1, "Brine", 200000, 200000)
        before["plays.jnl"] = sr.log(*[play(q) for q in range(1, 7)], b)
        r = sr.boot(before)
        self.assertEqual(r["folded"], 4)
        self.assertEqual(r["stats"].head["plays"], 7)

    def test_gap_counted_once(self):
        sc = sr.scenario_gap()
        r = sr.boot(sc["files"], **self.args(sc["boots"][0]))
        self.assertEqual((r["gap"], r["stats"].head["gap"], r["stats"].mark[(bytes.fromhex(A), E)]), (30, 30, 60))
        r = sr.boot(r["files"], **self.args(sc["boots"][1]))
        self.assertEqual((r["gap"], r["saved"], r["stats"].head["gap"]), (0, False, 30))
        # A log trimmed whole: nothing folded, the gap alone is new and saved once.
        before = sr.earlier([play(q) for q in range(1, 11)])
        r = sr.boot(before, acks=[(A, E, 50)])
        self.assertEqual((r["folded"], r["gap"], r["saved"]), (0, 40, True))
        r = sr.boot(r["files"], acks=[(A, E, 50)])
        self.assertEqual((r["gap"], r["saved"], r["stats"].head["gap"]), (0, False, 40))

    def test_a_failed_save_trims_by_the_boots_mark(self):
        sc = sr.scenario_save_fails()
        r = sr.boot(sc["files"], **self.args(sc["boots"][0]))
        self.assertEqual((r["saved"], r["trimmed"]), (False, ["plays.1.jnl"]))
        self.assertEqual(r["files"]["stats.bin"], sc["files"]["stats.bin"])
        # The same with the save: both go.
        r = sr.boot(sc["files"], acks=sc["boots"][0]["acks"], now=sc["boots"][0]["now"])
        self.assertEqual(r["trimmed"], ["plays.1.jnl", "plays.jnl"])

    def test_no_acks_no_trim_and_the_rule_of_today(self):
        files = files_of(play(1), play(2))
        self.assertEqual(sr.boot(files)["trimmed"], [])
        self.assertEqual(sr.boot(files, acks=[(A, E, 2)])["trimmed"], ["plays.jnl"])
        self.assertEqual(sr.boot(files, acks=[(A, E, 1)])["trimmed"], [])
        note = {"kind": pl.KIND_DROP, "deviceId": A, "epoch": E, "dropped": 1}
        self.assertEqual(sr.boot({"plays.jnl": sr.log(play(1), note)}, acks=[(A, E, 1)])["trimmed"], [],
                         "a drop note no play follows")

    def test_newer_and_corrupt(self):
        files = sr.earlier([play(1)])
        good = files["stats.bin"]
        b = bytearray(good)
        struct.pack_into("<H", b, 4, 2)
        files["stats.bin"] = cf.reseal(b)
        files["plays.jnl"] = sr.log(play(1), play(2))
        r = sr.boot(files, acks=[(A, E, 2)])
        self.assertEqual((r["read"], r["folded"], r["saved"], r["trimmed"]), ("newer", 0, False, []))
        self.assertEqual(r["files"], files, "a newer major is left alone")
        files["stats.bin"] = good[:-1]
        files["stats.bad"] = b"an older one"
        r = sr.boot(files, acks=[(A, E, 2)])
        self.assertEqual((r["read"], r["why"], r["folded"], r["trimmed"]), ("corrupt", "FileBytes", 2, ["plays.jnl"]))
        self.assertEqual(r["files"]["stats.bad"], files["stats.bin"])
        self.assertEqual(r["stats"].generation, 1, "counting starts again")

    def test_bases(self):
        files = sr.earlier([play(q, ["Brine", "Night Ticket"][q % 2]) for q in range(1, 5)])
        self.assertEqual(sorted(files), ["stats-00000001.bin", "stats.bin"])
        files["stats-00000002.bin"] = b"a cut compaction's"
        se = sr.Session(dict(files))
        self.assertEqual((se.read, se.removed), ("loaded", ["stats-00000002.bin"]))
        self.assertEqual(len(se.stats.base), 2)
        lost = dict(files)
        del lost["stats-00000001.bin"]
        se = sr.Session(lost)
        h = se.stats.head
        self.assertEqual((se.read, h["flags"] & sr.FLAG_BASE_LOST, h["baseGen"], h["plays"], len(se.stats.base),
                          se.removed), ("loaded", 2, 0, 4, 0, []))
        other = dict(files)
        other["stats-00000001.bin"] = sr.write_base({}, 1)        # another file under its name
        self.assertEqual(sr.Session(other).stats.head["flags"] & sr.FLAG_BASE_LOST, 2)
        broken = dict(files)
        b = bytearray(broken["stats-00000001.bin"])
        b[-1] ^= 1                                              # its rows, the header as named
        broken["stats-00000001.bin"] = bytes(b)
        se = sr.Session(broken)
        self.assertEqual((se.read, se.why, se.on), ("unread", "SectionCrc", False))

    def test_live_folds_and_a_rotation_save(self):
        files = sr.earlier([play(q) for q in range(1, 3)])
        se = sr.Session(files)
        se.scan()
        self.assertTrue(se.save(sr.MIN_MS))
        self.assertFalse(se.wrote, "nothing new")
        se.live(play(3, "Brine", played=40000, startedAt=sr.MIN_MS + 5000, clock=1))
        self.assertTrue(se.save(sr.MIN_MS + sr.DAY_MS, "rotation"))
        self.assertFalse(se.compacted, "a rotation's save never compacts")
        st = sr.load(files)
        self.assertEqual((st.head["round"], st.track(sr.TRACKS["Brine"][1]).lastOrd), (3, 3))
        self.assertEqual(st.generation, 2)
        self.assertEqual(st.mark[(bytes.fromhex(A), E)], 3)

    def test_compaction_rule(self):
        due = sr.compaction_due
        self.assertTrue(due(1, 0, False, True), "any delta over no base")
        self.assertFalse(due(0, 0, False, True))
        self.assertFalse(due(1, 0, True, True), "not while playing")
        self.assertTrue(due(26, 100, False, False))
        self.assertFalse(due(25, 100, False, False))
        self.assertTrue(due(1366, 10 ** 6, False, False), "over 32 KB")
        self.assertFalse(due(1365, 10 ** 6, False, False))
        self.assertTrue(due(4097, 10 ** 6, True, True), "past 96 KB at a safe point")
        self.assertFalse(due(4097, 10 ** 6, True, False))
        self.assertFalse(due(4096, 10 ** 6, True, True))

    def test_retention(self):
        st = sr.Stats()
        for h in range(900):
            st.hour[h] = [1, 1]
        for d in range(450):
            st.days[d] = [1, 0, 0, 0, 1, 0, 0]
            st.rcnt[(d, 1)] = 1
        st.rcnt[(460, 1)] = 1                                  # newer than today
        sr.retain(st, 440)
        self.assertEqual((min(st.hour), len(st.hour), min(st.days), len(st.days)), (60, 840, 50, 400))
        self.assertEqual(sorted(d for d, _ in st.rcnt), list(range(409, 450)) + [460])
        st.rcnt = {(d, 1): 1 for d in range(10)}
        sr.retain(st, None)
        self.assertEqual(len(st.rcnt), 10, "nothing by date without a clock")
        st.days = {5: [0] * 7}
        sr.retain(st, 300)                                      # the reference: DAYS's newest, 5
        self.assertEqual(sorted(d for d, _ in st.rcnt), list(range(10)))
        st.rcnt = {(d, h): 1 for d in range(3) for h in range(2000)}
        sr.retain(st, None)
        self.assertEqual(sorted({d for d, _ in st.rcnt}), [1, 2], "whole days, oldest first")
        st.rcnt = {(0, h): 1 for h in range(4097)}
        sr.retain(st, None)
        self.assertEqual(st.rcnt, {}, "a day over the cap goes whole")

    def test_since_day(self):
        st = sr.Stats()
        sr.since_day(st, 40, 50, 1791900000)
        self.assertEqual((st.head["sinceDay"], st.head["flags"]), (40, 0))
        sr.since_day(st, 10, 50, None)
        self.assertEqual(st.head["sinceDay"], 40, "set once")
        st = sr.Stats()
        sr.since_day(st, None, 50, 1791900000)
        self.assertEqual(st.head["sinceDay"], 50)
        st = sr.Stats()
        sr.since_day(st, None, None, 1791900000)
        self.assertEqual((st.head["sinceDay"], st.head["flags"]), (sr.day_of(1791900000000), sr.FLAG_SINCE_COMMIT))
        st = sr.Stats()
        sr.since_day(st, None, None, 1000)
        self.assertEqual(st.head["sinceDay"], sr.UNSET_DAY)

    def test_rounds_saturate(self):
        files = sr.earlier([play(1)])
        se = sr.Session(files)
        se.stats.head["round"] = se.round = sr.M24
        se.live(play(2, "Brine"))
        self.assertEqual(se.stats.trkd[sr.TRACKS["Brine"][1]].lastOrd, sr.M24)


def t_at(day, h=12):
    return sr.time_of(sr.MIN_MS + day * sr.DAY_MS + h * sr.HOUR_MS)


class RankTests(unittest.TestCase):
    def test_most_played(self):
        st = sr.Stats()
        st.base = {1: sr.Row(5, 0, t_at(1), t_at(9), 1, 0), 2: sr.Row(5, 0, t_at(1), t_at(10), 1, 0),
                   3: sr.Row(5, 0, t_at(1), t_at(10), 1, 0), 4: sr.Row(0, 3, sr.NO_FIRST, 0, 0, 0),
                   5: sr.Row(1, 0, 0, 0, 1, 0)}
        self.assertEqual(sr.most_played(st, None), [(2, 5), (3, 5), (1, 5), (5, 1)])
        st.rcnt = {(100, 7): 2, (101, 8): 2, (94, 9): 3, (93, 9): 9, (102, 7): 1}
        self.assertEqual(sr.most_played(st, 100, 7), [(9, 3), (7, 2)], "day 101 and 102 not yet; 93 too old")
        self.assertEqual(sr.most_played(st, 101, 7), [(8, 2), (7, 2)], "a tie: the latest day first")
        self.assertEqual(sr.most_played(st, 102, 7), [(7, 3), (8, 2)])
        self.assertIsNone(sr.most_played(st, None, 7))
        st.base = {h: sr.Row(1, 0, 0, 0, 1, 0) for h in range(100)}
        self.assertEqual(len(sr.most_played(st, None)), 66)

    def test_new_finds_and_old_favourites(self):
        today = 200
        st = sr.Stats()
        st.base = {1: sr.Row(2, 0, t_at(today - 29), t_at(today), 3, sr.AUTODJ),      # in the window's first day
                   2: sr.Row(2, 0, t_at(today - 30), t_at(today), 3, 0),             # a day before: out
                   3: sr.Row(9, 0, 0, 0, 3, 0),                                      # no dated play: never
                   4: sr.Row(5, 0, t_at(10), t_at(today - 30), 3, 0),                # last play before: old
                   5: sr.Row(5, 0, t_at(10), t_at(today - 29), 3, 0),                # inside: not old
                   6: sr.Row(4, 0, t_at(10), t_at(20), 3, 0),                        # 4 plays: not old
                   7: sr.Row(2, 0, t_at(today - 29, 13), t_at(today), 3, sr.SHUFFLE)}
        self.assertEqual(sr.new_finds(st, today), [(7, 2, t_at(today - 29, 13), sr.SHUFFLE),
                                                   (1, 2, t_at(today - 29), sr.AUTODJ)])
        self.assertEqual(sr.old_favourites(st, today), [(4, 5, t_at(today - 30))])
        self.assertIsNone(sr.new_finds(st, None))
        self.assertIsNone(sr.old_favourites(st, None))

    def test_recently_played_and_not_played(self):
        st = sr.Stats()
        st.base = {1: sr.Row(1, 0, 0, 0, 5, 0), 2: sr.Row(1, 0, t_at(3), t_at(3), 2, 0),
                   3: sr.Row(1, 0, 0, 0, 7, 0), 4: sr.Row(0, 1, sr.NO_FIRST, 0, 0, 0),
                   5: sr.Row(2, 0, t_at(1), t_at(4), 1, 0), 6: sr.Row(1, 0, 0, 0, 7, 0)}
        self.assertEqual([r[0] for r in sr.recently_played(st)], [5, 2, 3, 6, 1])
        albums = [("Only skipped", [4]), ("Played", [4, 2]), ("Never", [99])]
        self.assertEqual(sr.not_played(st, albums), ["Only skipped", "Never"])

    def test_windows(self):
        self.assertEqual([sr.windows_offered(d) for d in (None, 1, 6, 7, 29, 30, 400)],
                         [[], [], [], [7, None], [7, None], [7, 30, None], [7, 30, None]])
        st = sr.Stats()
        st.head["sinceDay"] = 100
        self.assertEqual([sr.counting_days(st, t) for t in (None, 90, 100, 129)], [None, 1, 1, 30])
        self.assertEqual(sr.window(50, 7), (44, 50))
        self.assertIsNone(sr.window(None, 30))

    def test_week(self):
        st = sr.Stats()
        st.hour = {50 * 24 + 1: [2, 1000], 50 * 24 + 23: [1, 500], 49 * 24: [3, 1500], 44 * 24: [1, 9], 43 * 24: [9, 9]}
        st.days = {50: [3, 1, 2, 1, 1500, 0, 0], 43: [9, 0, 9, 0, 9, 0, 0]}
        w = sr.week(st, 50)
        self.assertEqual(w["days"][0], (44, 9, 1))
        self.assertEqual(w["days"][-2:], [(49, 1500, 3), (50, 1500, 3)])
        self.assertEqual((w["listenedMs"], w["plays"], w["topDay"], w["autodj"]), (3009, 7, 50, [2, 1]))
        self.assertIsNone(sr.week(st, None))
        self.assertIsNone(sr.week(sr.Stats(), 50)["topDay"])


class FixtureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.files = sr.fixture_files()
        cls.index = json.loads(cls.files["stats/index.json"])
        cls.by = {sc["name"]: sc for sc in cls.index}

    def test_card_fixtures_writes_them(self):
        for name, data in self.files.items():
            self.assertEqual((FIXTURES / name).read_bytes(), data, name)
        on_disk = {p.relative_to(FIXTURES).as_posix() for p in (FIXTURES / "stats").rglob("*") if p.is_file()}
        self.assertEqual(on_disk, set(self.files))

    def card(self, where):
        return {n: self.files["stats/%s/%s" % (d, n)] for n, d in where.items()}

    def test_every_boot_again(self):
        """Each boot's out from its in and inputs, and its stats read back
        as the index says."""
        for sc in self.index:
            for b in sc["boots"]:
                acks = [(d, int(e, 16), s) for d, e, s in b["acks"]]
                r = sr.boot(self.card(b["in"]), acks, b["now"], b["commitTime"], b["playing"], b["safePoint"],
                            b["saveFails"])
                self.assertEqual(r["files"], self.card(b["out"]), sc["name"])
                for k in ("read", "folded", "twice", "gap", "saved", "compacted", "removed", "trimmed"):
                    self.assertEqual(r[k], b[k], (sc["name"], k))
                self.assertEqual(sr.stats_json(sr.load(r["files"])), b["stats"], sc["name"])

    def test_the_plans_figures(self):
        w = self.by["worked-example"]["boots"]
        self.assertEqual([(b["folded"], b["saved"], b["trimmed"]) for b in w],
                         [(7, True, ["plays.1.jnl"]), (0, False, [])])
        self.assertEqual(w[0]["stats"]["mark"], [[A, "0x%016x" % E, 48]])
        st = sr.load(self.card(w[0]["out"]))
        x = st.track(sr.TRACKS["Weather Vane"][1])
        self.assertEqual(x.firstAt, 26948401)
        self.assertEqual(st.hour[7485], [2, 165000])
        rows = [sr.load(self.card(self.by[n]["boots"][0]["out"])).tracks() for n in ("first-at-a", "first-at-b")]
        self.assertEqual(rows[0], rows[1])
        ha = sr.TRACKS["Harbour Lights"][1]
        self.assertEqual((rows[0][ha].firstAt, rows[0][ha].src), (t_at(305, 10), sr.YOU))
        self.assertEqual((rows[0][sr.TRACKS["Tidewater"][1]].firstAt, rows[0][sr.TRACKS["Tidewater"][1]].src),
                         (0, sr.SHUFFLE))
        ret = self.by["retention"]["boots"][0]["stats"]["rows"]
        self.assertEqual((ret["hour"], ret["days"]), (840, 400))
        st = sr.load(self.card(self.by["retention"]["boots"][0]["out"]))
        self.assertEqual(sorted({d for d, _ in st.rcnt}), list(range(421, 454)) + [sr.day_of(sr.ms_of(
            "2099-06-01T10:00:00"))])
        cap = sr.load(self.card(self.by["rcnt-cap"]["boots"][0]["out"]))
        self.assertEqual((len(cap.rcnt), min(d for d, _ in cap.rcnt)), (3999, 289))
        g = self.by["gap"]["boots"]
        self.assertEqual([b["gap"] for b in g], [30, 0])
        self.assertEqual(self.by["no-clock"]["boots"][0]["stats"]["head"]["flags"], sr.FLAG_SINCE_COMMIT)
        self.assertIsNone(self.by["no-clock"]["boots"][0]["lists"]["newFinds"])
        self.assertEqual(self.by["twice"]["boots"][0]["twice"], 3)
        self.assertEqual(self.by["compaction"]["boots"][0]["removed"], ["stats-00000001.bin"])
        self.assertEqual(self.by["rules"]["boots"][0]["stats"]["head"]["plays"], 9)

    def test_the_library(self):
        lib = json.loads(self.files["stats/library.json"])
        names = [a["album"] for a in lib]
        self.assertEqual(names, sorted(names, key=str.casefold))
        for a in lib:
            for t in a["tracks"]:
                self.assertEqual(int(t["hash"], 16), cf.path_hash(t["path"]))


class FakeProbe:
    def __init__(self, fs="FAT32", system="C:\\"):
        self.fs, self.sys = fs, system

    def system_drive(self):
        return self.sys

    def info(self, root):
        return {"type": "removable", "label": "TESTCARD", "fs": self.fs, "size": 32 * 10 ** 9, "free": 10 ** 10,
                "cluster": 32768, "readable": True}


UNTIL = sr.ms_of("2026-11-10T12:00:00")


class SeedTests(unittest.TestCase):
    def setUp(self):
        import synthtransfer as st
        self.st = st
        self.tmp = tempfile.mkdtemp(prefix="statsref_")
        self.card = os.path.join(self.tmp, "card")
        for a in range(3):
            for b in range(2):
                d = os.path.join(self.card, "music", "Artist %d" % a, "Album %d" % b)
                os.makedirs(d)
                for t in range(4):
                    Path(d, "%02d Song %d.mp3" % (t + 1, t)).write_bytes(b"x" * (100 + t))
        Path(self.card, "SYNTHCARD.TXT").write_text("synthcard: a test tree\n", encoding="utf-8")
        st.add_to(self.card, synthetic=True, commit_time=1791900000, out=io.StringIO(), k=8, head=4, floor=6)
        self.player = os.path.join(self.card, ".player")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def seed(self, **kw):
        out = io.StringIO()
        kw.setdefault("until", UNTIL)
        return sr.seed_card(self.card, out=out, **kw), out.getvalue()

    def test_a_history(self):
        summary, said = self.seed(days=10, per_day=6)
        self.assertEqual(sorted(os.listdir(self.player)), ["plays.jnl"])
        files = sr.player_files(self.player)
        plays = pl.read_log(files)["plays"]
        self.assertEqual(len(plays), 60)
        self.assertTrue(all(p["deviceId"] == sr.SEED_DEVICE for p in plays))
        days = {sr.day_of(p["startedAt"]) for p in plays}
        self.assertEqual(days, set(range(sr.day_of(UNTIL) - 9, sr.day_of(UNTIL) + 1)))
        tracks, _ = self.st.walk_music(self.card)
        self.assertLessEqual({p["pathHash"] for p in plays}, {sr.path_hash(t) for t in tracks})
        r = sr.boot(files, now=UNTIL)
        self.assertEqual((r["folded"], r["stats"].head["sinceDay"]), (60, sr.day_of(UNTIL) - 9))
        self.assertIn("plays: 60 slots", said)
        again, _ = self.seed(days=10, per_day=6, replace=True)
        self.assertEqual(sr.player_files(self.player), files, "deterministic")

    def test_full_log_and_base(self):
        summary, said = self.seed(slots=2 * pl.FILE_SLOTS, base=40)
        files = sr.player_files(self.player)
        self.assertEqual(sorted(files), ["plays.1.jnl", "plays.jnl", "stats-00000001.bin", "stats.bin"])
        self.assertEqual([len(files[n]) // pl.SLOT for n in ("plays.1.jnl", "plays.jnl")], [pl.FILE_SLOTS] * 2)
        st = sr.load(files)
        self.assertEqual((len(st.base), len(st.trkd), st.head["baseRows"]), (40, 0, 40))
        self.assertEqual(summary["base"]["orphans"], 16)
        self.assertEqual(list(st.mark), [(bytes.fromhex(sr.SEED_OLD_DEVICE), int(summary["base"]["epoch"], 16))])
        r = sr.boot(files, now=UNTIL)
        self.assertEqual(r["folded"], 2 * pl.FILE_SLOTS)

    def test_dry_run_writes_nothing(self):
        summary, said = self.seed(days=3, per_day=2, base=5, dry_run=True)
        self.assertFalse(os.path.exists(self.player))
        self.assertIn("would write /.player/plays.jnl", said)
        self.assertIn("would write /.player/stats.bin", said)

    def test_refusals(self):
        self.seed(days=2, per_day=2)
        with self.assertRaises(sr.Refused) as e:
            self.seed(days=2, per_day=2)
        self.assertIn("--replace", str(e.exception))
        Path(self.player, "plays.xl1").write_bytes(b"")
        with self.assertRaises(sr.Refused) as e:
            self.seed(days=2, per_day=2, replace=True)
        self.assertIn("disk check", str(e.exception))
        os.remove(os.path.join(self.player, "plays.xl1"))
        # No SYNTHCARD.TXT.
        os.remove(os.path.join(self.card, "SYNTHCARD.TXT"))
        with self.assertRaises(sr.Refused) as e:
            self.seed(days=2, per_day=2, replace=True)
        self.assertIn("SYNTHCARD.TXT", str(e.exception))
        Path(self.card, "SYNTHCARD.TXT").write_text("synthcard\n", encoding="utf-8")
        # A root another program wrote, and none at all.
        with mock.patch.object(self.st, "PRODUCER", "mstream-terminal 0.13.0"):
            self.st.add_to(self.card, synthetic=True, commit_time=1791900000, out=io.StringIO(), k=8, head=4,
                           floor=6, replace=True, replace_foreign=True)
        before = sr.player_files(self.player)
        with self.assertRaises(sr.Refused) as e:
            self.seed(days=2, per_day=2, replace=True)
        self.assertIn("not tools/synthtransfer.py", str(e.exception))
        shutil.rmtree(os.path.join(self.card, ".mstream"))
        with self.assertRaises(sr.Refused) as e:
            self.seed(days=2, per_day=2, replace=True)
        self.assertIn("no /.mstream root", str(e.exception))
        self.assertEqual(sr.player_files(self.player), before, "nothing changed")

    def test_drives(self):
        with mock.patch.object(self.st, "is_drive_root", return_value=True), \
                mock.patch.object(self.st, "card_root", return_value=self.card):
            with self.assertRaises(sr.Refused) as e:
                sr.seed_card("C:\\", until=UNTIL, probe=FakeProbe(system=self.card), out=io.StringIO())
            self.assertIn("system drive", str(e.exception))
            with self.assertRaises(sr.Refused) as e:
                sr.seed_card("E:\\", until=UNTIL, probe=FakeProbe(fs="exFAT"), out=io.StringIO())
            self.assertIn("not FAT32", str(e.exception))
        self.assertFalse(os.path.exists(self.player))

    def test_command_line(self):
        err = io.StringIO()
        with contextlib.redirect_stdout(io.StringIO()) as out, contextlib.redirect_stderr(err):
            self.assertEqual(sr.main(["seed", self.card, "--days", "3", "--per-day", "2", "--until",
                                      "2026-11-10T12:00:00"]), 0)
            self.assertEqual(sr.main(["seed", self.card, "--days", "3"]), 2)
        self.assertIn("--replace", err.getvalue())
        self.assertIn("writing /.player/plays.jnl", out.getvalue())
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(sr.main(["boot", self.player, "--now", "2026-11-10T12:00:00",
                                      "--acks", "%s:%x:%d" % (sr.SEED_DEVICE, 1, 2)]), 0)
        res = json.loads(out.getvalue())
        self.assertEqual((res["read"], res["folded"], res["stats"]["head"]["plays"] <= 6), ("absent", 6, True))
        st = sr.boot(sr.player_files(self.player), now=UNTIL)
        Path(self.tmp, "stats.bin").write_bytes(st["files"]["stats.bin"])
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(sr.main(["dump", os.path.join(self.tmp, "stats.bin"), "--rows"]), 0)
        res = json.loads(out.getvalue())
        self.assertEqual((res["read"], res["stats"]["head"]["flags"]), ("loaded", sr.FLAG_BASE_LOST), "its base not given")


if __name__ == "__main__":
    unittest.main()
