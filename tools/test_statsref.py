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
  base, and one that doesn't read whole (39 bytes: unread, never lost); a
  stale base replaced by the next compaction; the bases either side of
  the named one removed; the live fold's round and a rotation's save,
  which never compacts; a live row and a boot's differ in lastOrd alone;
  a partial scan (an open or a read the card failed: no save, the trim by
  the boot's MARK, the next whole boot the same as one whole boot); the
  compaction's rule; retention (HOUR, DAYS, RCNT by date and by count);
  sinceDay, never after today.
- The rankers and windows (4.2, 4.3, 7.6): each shelf's rule and ties,
  the caps plus 16, the windows' edges, no lists that need dates without
  a clock, Not played yet, the tray, the week.
- The fixtures (test/fixtures/card/stats) as card_fixtures.py makes them,
  and the plan's own figures in them; every log file in seq order per
  (deviceId, epoch), and 5.6's ranges choosing the same slots as the
  reference's keys on every boot.
- seed: its refusals (no SYNTHCARD.TXT, a root another program wrote or
  none, the system drive, not FAT32, files already there, a twin of the
  log, and with --base one of stats.bin), --dry-run, a history and a
  full log, --base; the command line, dump with and without its base.
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

    def test_the_seq_rule(self):
        # C5 (5.6): the next seq at boot takes the stats' MARK and ACKS's
        # seq for this device and epoch too: NVS put back behind a log
        # already trimmed takes neither a seq ACKS acknowledges nor one MARK
        # covers.
        before = sr.earlier([play(q) for q in range(1, 11)])  # MARK 10, the log trimmed whole
        se = sr.Session(dict(before), acks=[(A, E, 6)])
        se.scan()
        self.assertEqual(se.next_seq(3, A, E), 11, "MARK's 10 over ACKS's 6 and NVS's 3")
        self.assertEqual(se.next_seq(3, A, E + 1), 4, "another epoch's MARK and ACKS don't count")
        self.assertEqual(se.next_seq(3, sr.DEV_B, E), 4, "nor another device's")
        se = sr.Session(dict(before), acks=[(A, E, 50)])
        se.scan()
        self.assertEqual(se.next_seq(3, A, E), 51, "ACKS's 50 (the gap raised MARK to it too)")
        se = sr.Session(dict(before, **files_of(*[play(q) for q in range(11, 14)])))
        se.scan()
        self.assertEqual(se.next_seq(3, A, E), 14, "the log's 13, folded: MARK 13 too")
        self.assertEqual(se.next_seq(20, A, E), 21, "NVS ahead")

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
        for n, why in ((sr.HEADER_BYTES - 1, "Short"), (sr.HEADER_BYTES + 1, "Short"), (119, "FileBytes")):
            cut = dict(files)
            cut["stats-00000001.bin"] = files["stats-00000001.bin"][:n]   # its header doesn't read whole: a fault
            se = sr.Session(cut)
            self.assertEqual((se.read, se.why, se.on, se.stats.head["flags"]), ("unread", why, False, 0), n)
            cut["plays.jnl"] = sr.log(*[play(q) for q in range(1, 5)])
            r = sr.boot(cut, acks=[(A, E, 4)])
            self.assertEqual((r["saved"], r["trimmed"], r["files"]), (False, [], cut), n)

    def test_a_stale_base_is_replaced(self):
        """5.3: after a stats.bin deleted or corrupt, or a base lost, baseGen
        starts again and the first compaction writes stats-00000001.bin over
        the stale file there (named by nothing); nothing is removed. A lost
        base 1 is named by stats.bin until a save: that boot doesn't compact
        under its name (a cut after the create would leave a short base the
        named one, unread at every boot), the next boot's does."""
        files = sr.earlier([play(q, ["Brine", "Night Ticket"][q % 2]) for q in range(1, 5)])
        stale = files["stats-00000001.bin"]
        log = {"plays.jnl": sr.log(*[play(q, "Low Tide") for q in range(5, 7)])}
        for name, card in (("absent", {"stats-00000001.bin": stale}),
                           ("corrupt", {"stats.bin": files["stats.bin"][:-1], "stats-00000001.bin": stale}),
                           ("lost", dict(files, **{"stats-00000001.bin": sr.write_base({}, 1)}))):
            r = sr.boot(dict(card, **log))
            if name == "lost":
                self.assertEqual((r["read"], r["saved"], r["compacted"], r["removed"]), ("loaded", True, False, []))
                self.assertEqual(r["files"]["stats-00000001.bin"], card["stats-00000001.bin"], "left as it was")
                self.assertEqual(sr.load(r["files"]).head["baseGen"], 0)
                card = r["files"]
                r = sr.boot(dict(card, **{"plays.jnl": sr.log(*[play(q, "Low Tide") for q in range(5, 8)])}))
            new = r["files"]["stats-00000001.bin"]
            self.assertTrue(r["compacted"], name)
            self.assertEqual(r["removed"], [], name)
            self.assertNotEqual(new, card["stats-00000001.bin"], name)
            st = sr.load(r["files"])
            self.assertEqual((st.head["baseGen"], st.head["baseCrc"], st.base), (1, sr.header_crc(new),
                                                                                  sr.read_base(new, 1)), name)

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

    def test_live_and_boot_rows_differ_in_last_ord_alone(self):
        """5.2, 5.6: the same slots folded live and saved by a rotation, or
        folded at a boot, give rows the same but for lastOrd (a live
        fold's round is HEAD.round + 2, a boot's + 1): the order holds."""
        undated = [play(2, "Tidewater", src=3), play(3, "Tidewater", src=2)]
        se = sr.Session(sr.earlier([play(1)]))
        se.scan()
        for s in undated:
            se.live(s)
        self.assertTrue(se.save(sr.MIN_MS, "rotation"))
        live = sr.load(se.files).track(sr.TRACKS["Tidewater"][1])
        card = sr.earlier([play(1)])
        card["plays.jnl"] = sr.log(*undated)
        booted = sr.boot(card, now=sr.MIN_MS)["stats"].track(sr.TRACKS["Tidewater"][1])
        self.assertEqual((live.tuple()[:4], live.src), (booted.tuple()[:4], booted.src))
        self.assertEqual((live.lastOrd, booted.lastOrd), (3, 2))

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
        st = sr.Stats()
        sr.since_day(st, 26814, 313, None)                     # a play dated 2099, today day 313
        self.assertEqual(st.head["sinceDay"], 313, "never after today")
        st = sr.Stats()
        sr.since_day(st, 26814, None, None)
        self.assertEqual(st.head["sinceDay"], 26814, "with no today, nothing to cap it by")

    def test_a_foreign_2099_slot_moves_nothing(self):
        """5.2: one foreign slot dated 2099 (believable: 4.1) beside this
        device's undated plays sets no sinceDay in 2099: the hub's first
        month and Most played's tray go by today."""
        now = sr.ms_of("2026-11-10T09:00:00")
        foreign = sr.slot(sr.DEV_C, sr.EPOCH_C, 1, "Brine", 200000, 200000,
                          startedAt=sr.ms_of("2099-06-01T10:00:00"), clock=2)
        r = sr.boot(files_of(play(1, at=None), foreign), now=now)
        st = r["stats"]
        self.assertEqual(st.head["sinceDay"], sr.today_of(now))
        later = sr.today_of(now) + 40
        self.assertEqual((sr.counting_days(st, later), sr.windows_offered(sr.counting_days(st, later))),
                         (41, [7, 30, None]))

    def test_a_partial_scan(self):
        """5.6: an open or a read the card failed. The fold goes on; no
        save this session (the boot's, a rotation's, Hs); the trim keeps
        the unread file and follows the MARK read at the boot; no gap is
        counted. The next whole boot gives what one whole boot would."""
        slots = [play(q, ["Brine", "Low Tide"][q % 2], startedAt=sr.MIN_MS + q * sr.HOUR_MS, clock=1)
                 for q in range(1, 15)]
        before = sr.earlier(slots[:8])                         # MARK 8
        card = dict(before, **{"plays.1.jnl": sr.log(*slots[2:8]), "plays.jnl": sr.log(*slots[8:])})
        acks = [(A, E, 14), (sr.DEV_B, sr.EPOCH_B, 9)]          # B's 1-9: a gap a whole scan would count
        now = sr.MIN_MS + 2 * sr.DAY_MS
        whole = sr.boot(card, acks, now)
        self.assertEqual((whole["folded"], whole["gap"], whole["trimmed"]), (6, 9, ["plays.1.jnl", "plays.jnl"]))
        for unread, folded, trimmed in (({"plays.1.jnl": None}, 6, []), ({"plays.1.jnl": 3}, 6, []),
                                        ({"plays.jnl": 3}, 3, ["plays.1.jnl"]), ({"plays.jnl": 0}, 0, ["plays.1.jnl"])):
            r = sr.boot(card, acks, now, unread=unread)
            self.assertEqual((r["partial"], r["folded"], r["gap"], r["saved"], r["trimmed"]),
                             (True, folded, 0, False, trimmed), unread)
            self.assertEqual(r["files"]["stats.bin"], card["stats.bin"])
            again = sr.boot(r["files"], acks, now)
            self.assertEqual((again["folded"], again["twice"], again["saved"]), (6, 0, True), unread)
            self.assertEqual(again["files"]["stats.bin"], whole["files"]["stats.bin"], unread)
        # A read fault in plays.jnl's tail, a play written live, then Hs.
        se = sr.Session(dict(card), acks, unread={"plays.jnl": 3})
        se.scan()
        self.assertFalse(se.save(now))
        live = play(15, "Brine", startedAt=now, clock=1)
        se.live(live)
        self.assertFalse(se.save(now, "rotation"), "Hs (the rotation's path) refused after a partial scan")
        self.assertEqual(se.trim(False), ["plays.1.jnl"])
        self.assertEqual(se.files["stats.bin"], card["stats.bin"])
        se.files["plays.jnl"] += sr.log(live)
        again = sr.boot(se.files, acks, now)
        self.assertEqual((again["folded"], again["stats"].head["plays"]), (7, 15))

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
                            b["saveFails"], b["unread"])
                self.assertEqual(r["files"], self.card(b["out"]), sc["name"])
                for k in ("read", "why", "folded", "twice", "gap", "partial", "saved", "compacted", "removed",
                          "trimmed"):
                    self.assertEqual(r[k], b[k], (sc["name"], k))
                st = sr.load(r["files"])
                self.assertEqual(None if st is None else sr.stats_json(st), b.get("stats"), sc["name"])

    def logs(self):
        """Every log file of every boot's in: (scenario, name, bytes)."""
        for sc in self.index:
            for b in sc["boots"]:
                for n, d in b["in"].items():
                    if n in sr.LOG_NAMES:
                        yield sc["name"], n, self.files["stats/%s/%s" % (d, n)]

    def test_each_file_in_seq_order(self):
        """5.6: in one file, one device and epoch's seqs only rise, as a
        device writes them; every fixture keeps to it."""
        for name, n, data in self.logs():
            last = {}
            for _, why, s, _ in pl.read_file(data):
                if why == "ok" and s["kind"] == pl.KIND_PLAY:
                    k = (s["deviceId"], s["epoch"])
                    self.assertGreaterEqual(s["seq"], last.get(k, 0), (name, n, s["seq"]))
                    last[k] = s["seq"]

    def test_the_ranges_choose_the_same_slots(self):
        """5.6's technique for the C++ (in one file, a slot at or under the
        seq last folded for its row from that file is skipped; across
        files, one inside another file's range is skipped and counted in
        twice) chooses the same slots as the reference's keys, in the same
        order, with the same twice, on every boot of every fixture."""
        for sc in self.index:
            for b in sc["boots"]:
                files = self.card(b["in"])
                acks = [(d, int(e, 16), s) for d, e, s in b["acks"]]
                r = sr.boot(files, acks, b["now"], b["commitTime"], b["playing"], b["safePoint"], b["saveFails"],
                            b["unread"])
                if r["read"] not in ("loaded", "absent", "corrupt"):
                    continue
                mark = r["session"].mark_at_boot
                ranges, keys, twice = {}, [], 0
                for name in sr.LOG_NAMES:
                    if name not in files:
                        continue
                    data = files[name]
                    if name in b["unread"]:
                        data = data[:(b["unread"][name] or 0) * pl.SLOT]
                    last, mine = {}, {}
                    for _, why, s, _ in pl.read_file(data):
                        if why != "ok" or not sr.counts(s):
                            continue
                        row, q = (pl.device_bytes(s["deviceId"]), s["epoch"]), s["seq"]
                        if q <= mark.get(row, 0) or q <= last.get(row, 0):
                            continue
                        if any(lo <= q <= hi for lo, hi in ranges.get(row, ())):
                            twice += 1
                            continue
                        last[row] = q
                        lo, hi = mine.get(row, (q, q))
                        mine[row] = (min(lo, q), max(hi, q))
                        keys.append(row + (q,))
                    for row, rg in mine.items():
                        ranges.setdefault(row, []).append(rg)
                        self.assertLessEqual(len(ranges[row]), 10)
                self.assertEqual((keys, twice), (r["session"].keys, r["twice"]), sc["name"])

    def test_the_plans_figures(self):
        w = self.by["worked-example"]["boots"]
        self.assertEqual([(b["folded"], b["saved"], b["trimmed"]) for b in w],
                         [(7, True, ["plays.1.jnl"]), (0, False, [])])
        self.assertEqual(w[0]["stats"]["mark"], [[A, "0x%016x" % E, 48]])
        st = sr.load(self.card(w[0]["out"]))
        x = st.track(sr.TRACKS["Weather Vane"][1])
        self.assertEqual(x.firstAt, 26948401)
        self.assertEqual(st.hour[7485], [2, 165000])
        outs = [self.by[n]["boots"][0]["out"] for n in ("first-at-a", "first-at-b")]
        self.assertEqual(self.card(outs[0])["stats.bin"], self.card(outs[1])["stats.bin"], "the same stats.bin")
        rows = [sr.load(self.card(o)).tracks() for o in outs]
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

    def test_the_reads(self):
        """5.4's verdicts and 5.12's newer minor, as the fixtures hold them."""
        b = lambda name, i=0: self.by[name]["boots"][i]  # noqa: E731
        for name, why in (("newer", "Major"), ("base-unread", "SectionCrc"), ("base-short", "Short")):
            self.assertEqual((b(name)["read"], b(name)["why"], b(name)["folded"], b(name)["saved"],
                              b(name)["trimmed"]), ("newer" if name == "newer" else "unread", why, 0, False, []))
            self.assertEqual(self.card(b(name)["out"]), self.card(b(name)["in"]), name)
            self.assertNotIn("stats", b(name))
        c = b("corrupt")
        self.assertEqual((c["read"], c["why"], c["folded"], c["compacted"], c["removed"]),
                         ("corrupt", "FileBytes", 10, True, []))
        self.assertEqual(self.card(c["out"])["stats.bad"], self.card(c["in"])["stats.bin"])
        self.assertNotEqual(self.card(c["out"])["stats-00000001.bin"], self.card(c["in"])["stats-00000001.bin"],
                            "the stale base replaced")
        for name in ("base-lost", "base-other"):
            h = b(name)["stats"]["head"]
            self.assertEqual((h["flags"] & sr.FLAG_BASE_LOST, h["plays"], b(name)["removed"], b(name)["compacted"]),
                             (sr.FLAG_BASE_LOST, 10, [], False), name)
        self.assertIn("stats-00000002.bin", b("base-lost")["out"])
        # base-other: no compaction under the name stats.bin names (5.3);
        # the next boot's, its stats.bin naming no base, replaces that file.
        o1, o2 = b("base-other"), b("base-other", 1)
        self.assertEqual(self.card(o1["out"])["stats-00000001.bin"], self.card(o1["in"])["stats-00000001.bin"])
        self.assertEqual((o1["stats"]["head"]["baseGen"], o2["compacted"], o2["removed"], o2["stats"]["head"]["baseGen"]),
                         (0, True, [], 1))
        self.assertNotEqual(self.card(o2["out"])["stats-00000001.bin"], self.card(o1["in"])["stats-00000001.bin"])
        self.assertEqual(b("base-neighbours")["removed"], ["stats-00000002.bin"])
        nm = b("newer-minor")
        self.assertEqual(struct.unpack_from("<H", self.card(nm["in"])["stats.bin"], 6)[0], 3)
        out = self.card(nm["out"])["stats.bin"]
        self.assertEqual(struct.unpack_from("<HHI", out, 4), (1, 0, 40))
        self.assertEqual(out, sr.write_stats(sr.read_stats(out)), "minor 0, without the additions")
        self.assertEqual(nm["stats"]["head"]["plays"], 10)

    def test_clock_ahead_and_partial(self):
        ca = [sr.load(self.card(b["out"])) for b in self.by["clock-ahead"]["boots"]]
        self.assertEqual([sorted({d for d, _ in st.rcnt}) for st in ca], [[677], [313, 677]])
        self.assertEqual([(len(st.hour), len(st.days), st.head["plays"], st.head["sinceDay"]) for st in ca],
                         [(41, 41, 41, 273), (42, 42, 42, 273)])
        self.assertEqual(self.by["clock-ahead"]["boots"][1]["added"], ["plays.jnl"])
        for name, first, second in (("partial-open", 3, 7), ("partial-read", 5, 7), ("partial-tail", 3, 6)):
            b1, b2 = self.by[name]["boots"]
            self.assertEqual((b1["partial"], b1["folded"], b1["saved"], b2["partial"], b2["folded"], b2["saved"]),
                             (True, first, False, False, second, True), name)
            self.assertEqual(self.card(b1["out"])["stats.bin"], self.card(b1["in"])["stats.bin"], name)
        self.assertEqual([b["trimmed"] for b in self.by["partial-tail"]["boots"]], [["plays.1.jnl"], ["plays.jnl"]])

    def test_the_library(self):
        lib = json.loads(self.files["stats/library.json"])
        names = [a["album"] for a in lib]
        self.assertEqual(names, sorted(names, key=str.casefold))
        for a in lib:
            for t in a["tracks"]:
                self.assertEqual(int(t["hash"], 16), cf.path_hash(t["path"]))


class SessionTests(unittest.TestCase):
    """The sessions (stats/sessions.json): players' boots and plays on one
    card, the log as playlog's Device writes it and the stats as 5.5-5.8
    keep them; test_stats_store runs each step through the firmware's code."""

    @classmethod
    def setUpClass(cls):
        cls.files = sr.session_files()
        cls.index = json.loads(cls.files["stats/sessions.json"])
        cls.by = {e["name"]: e for e in cls.index}

    def test_written_by_fixture_files(self):
        all_files = sr.fixture_files()
        for n, b in self.files.items():
            self.assertEqual(all_files[n], b, n)

    def test_the_undercounts(self):
        """5.5's and 13's rotation hole, to the slot: a rotation whose save
        fails (over a twin's chain too), one in the end's flush with under
        1 s left, one after a partial scan; 0.14.0's trim (HEAD's gap).
        Every other session loses nothing and counts nothing twice."""
        lost = {"rotation-fails": 4, "rotation-twin-fails": 4, "flush-late": 4, "partial-rotation": 4,
                "old-firmware": 3, "update-rotation-unsaved": 4, "update-behind-rotation": 2}
        for e in self.index:
            self.assertEqual(e["lost"], lost.get(e["name"], 0), e["name"])
        self.assertEqual(len(self.index), 24)

    def test_the_updates_give_back(self):
        """6.7's give-back (the owner's decision of 2026-11-11): a session
        holding plays the card's stats.bin doesn't saves first; the plays
        written while given back are folded after the re-read (but those
        the card's MARK covers); another copy, a save that didn't land or a
        play past the ring leaves the session behind its log, and every
        later save of it refused. Each step's facts."""
        by = self.by

        def steps(name, do):
            return [s for s in by[name]["steps"] if s["do"] == do]

        self.assertEqual([s["saved"] for s in steps("update-quiet", "giveback")], [None])
        self.assertEqual([(s["same"], s["folded"], s["behind"]) for s in steps("update-quiet", "takeback")],
                         [(True, 0, False)])
        self.assertEqual([s["saved"] for s in steps("update-plays", "giveback")], [True, True])
        self.assertEqual([(s["same"], s["folded"], s["behind"]) for s in steps("update-plays", "takeback")],
                         [(True, 2, False), (True, 0, False)])
        self.assertEqual([s["ok"] for s in steps("update-plays", "hs")], [True])
        self.assertEqual([s["saved"] for s in steps("update-save-fails", "giveback")], [False])
        self.assertEqual([(s["read"], s["same"], s["folded"], s["behind"])
                          for s in steps("update-save-fails", "takeback")], [("loaded", False, 1, True)])
        self.assertEqual([s["ok"] for s in steps("update-save-fails", "hs")], [False])
        # Behind, then a rotation later in the session: its save refused,
        # the plays the give-back's save didn't land (3-4) lost with
        # plays.1.jnl (the give-back review).
        self.assertEqual([s["saved"] for s in steps("update-behind-rotation", "giveback")], [False])
        self.assertEqual([(s["read"], s["same"], s["behind"]) for s in steps("update-behind-rotation", "takeback")],
                         [("loaded", False, True)])
        self.assertEqual([s["save"] for s in steps("update-behind-rotation", "play")],
                         [None, None, None, None, None, None, None, None, False])
        # A rotation while given back: no save asked when the give-back's
        # covered plays.1.jnl; asked and refused (1-4 lost) when it didn't.
        self.assertIsNone(steps("update-rotation-saved", "play")[-1]["save"])
        self.assertEqual(steps("update-rotation-unsaved", "play")[-1]["save"], False)
        self.assertEqual(steps("update-rotation-unsaved", "takeback")[0]["read"], "absent")
        # Another player meanwhile: its stats.bin holds the play the ring
        # held (covered, not folded again); the session's saves refused.
        t = steps("update-other-player", "takeback")[0]
        self.assertEqual((t["same"], t["folded"], t["covered"], t["behind"]), (False, 0, 1, True))
        self.assertEqual([s["ok"] for s in steps("update-other-player", "hs")], [False])
        # The end while given back: no re-read; the flush's rotation covered.
        self.assertEqual(steps("update-ending", "takeback"), [])
        self.assertIsNone(steps("update-ending", "play")[-1]["save"])
        t = steps("update-ring-full", "takeback")[0]
        self.assertEqual((t["same"], t["folded"], t["over"], t["behind"]), (True, sr.RING, 2, True))
        self.assertEqual([s["ok"] for s in steps("update-ring-full", "hs")], [False])

    def test_a_take_back_reads_once(self):
        """The re-read after a give-back is a remount's (5.4): one read, so a
        stats.bin that fails its checks is unread (the stats off for the
        session), never renamed stats.bad."""
        good = bytearray(sr.write_stats(sr.Stats()))
        good[-1] ^= 0xFF  # (HEAD's last byte: its section's CRC fails)
        files = {sr.STATS_BIN: bytes(good)}
        se = sr.Session(files, once=True)
        self.assertEqual(se.read, "unread")
        self.assertFalse(se.on)
        self.assertIn(sr.STATS_BIN, files)
        self.assertNotIn(sr.STATS_BAD, files)
        se = sr.Session(dict(files))
        self.assertEqual(se.read, "corrupt")

    def test_the_steps(self):
        by = self.by
        # The rotation's save runs before plays.1.jnl leaves the log's names,
        # and the next boot folds what the log still holds.
        steps = by["rotation-saves"]["steps"]
        self.assertEqual([s["save"] for s in steps if s["do"] == "play"][-1], True)
        self.assertEqual([s["save"] for s in by["rotation-fails"]["steps"] if s["do"] == "play"][-1], False)
        self.assertEqual([s["save"] for s in by["flush-in-time"]["steps"] if s["do"] == "play"][-1], True)
        self.assertEqual([s["save"] for s in by["flush-late"]["steps"] if s["do"] == "play"][-1], False)
        self.assertIsNone([s["save"] for s in by["partial-rotation"]["steps"] if s["do"] == "play"][-1])
        # Another player's stats.bin refuses every later save of a session.
        hs = [s for s in by["second-player"]["steps"] if s["do"] == "hs"]
        self.assertEqual([s["ok"] for s in hs], [False])
        self.assertTrue(any(s.get("compacted") for s in by["second-player"]["steps"]))
        self.assertEqual([s["ok"] for s in by["partial-hs"]["steps"] if s["do"] == "hs"], [False])
        # The seq rule's reservations: the play after NVS put back is 5.
        for name in ("nvs-back", "log-deleted"):
            plays = [s["seq"] for s in by[name]["steps"] if s["do"] == "play"]
            self.assertEqual(plays[-1], 5, name)
        # 0.14.0's boot trims without folding; the next counts the gap.
        old = by["old-firmware"]["steps"]
        self.assertEqual(old[-2]["trimmed"], ["plays.1.jnl", "plays.jnl"])
        self.assertEqual(old[-1]["gap"], 3)

    def test_each_log_file_in_seq_order(self):
        """5.6's ranges rely on it: in a file, a device and epoch's seqs only
        rise."""
        for path, data in self.files.items():
            if path.rsplit("/", 1)[-1] not in sr.LOG_NAMES:
                continue
            last = {}
            for _, why, s, _ in pl.read_file(data):
                if why == "ok" and s["kind"] == pl.KIND_PLAY:
                    k = (s["deviceId"], s["epoch"])
                    self.assertGreater(s["seq"], last.get(k, 0), path)
                    last[k] = s["seq"]

    def test_live_and_rotation_rows(self):
        """first-at-live: the rows a rotation's save holds are first-at-a's
        but lastOrd (HEAD.round + 2 for a live play, + 1 for the boot's)."""
        steps = self.by["first-at-live"]["steps"]
        out = [s["out"] for s in steps if s.get("save")][0]
        st = sr.read_stats(self.files["stats/sessions/%s/stats.bin" % out["stats.bin"]])
        fa = json.loads(sr.fixture_files()["stats/index.json"])
        boot = [e for e in fa if e["name"] == "first-at-a"][0]["boots"][0]
        ref = sr.read_stats(sr.fixture_files()["stats/%s/stats.bin" % boot["out"]["stats.bin"]])
        for h, r in ref.trkd.items():
            got = st.trkd[h]
            self.assertEqual((got.plays, got.skips, got.firstAt, got.lastAt, got.src),
                             (r.plays, r.skips, r.firstAt, r.lastAt, r.src))
            self.assertIn(got.lastOrd, (1, 2))


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
        # stats.bin's twin, with --base (which would remove stats.bin): refused, nothing changed.
        self.seed(days=2, per_day=2, base=4, replace=True)
        Path(self.player, "stats.xl1").write_bytes(b"")
        before = sr.player_files(self.player)
        listed = sorted(os.listdir(self.player))
        with self.assertRaises(sr.Refused) as e:
            self.seed(days=2, per_day=2, base=4, replace=True)
        self.assertIn("disk check", str(e.exception))
        self.assertEqual((sr.player_files(self.player), sorted(os.listdir(self.player))), (before, listed))
        self.seed(days=2, per_day=2, replace=True)             # without --base the stats files aren't touched
        self.assertEqual(sr.player_files(self.player)["stats.bin"], before["stats.bin"])
        os.remove(os.path.join(self.player, "stats.xl1"))
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
        here = Path(self.tmp, "dump")
        here.mkdir()
        Path(here, "stats.bin").write_bytes(st["files"]["stats.bin"])
        head = sr.read_stats(st["files"]["stats.bin"]).head
        self.assertEqual(head["baseGen"], 1)

        def dump(*args):
            with contextlib.redirect_stdout(io.StringIO()) as out:
                code = sr.main(["dump", str(Path(here, "stats.bin"))] + list(args))
            return code, json.loads(out.getvalue())
        code, res = dump("--rows")                             # no base beside it: HEAD as read, never lost
        self.assertEqual((code, res["read"], res["base"], res["stats"]["head"]), (0, "loaded", "not given", head))
        Path(here, "the base.bin").write_bytes(st["files"]["stats-00000001.bin"])
        code, res = dump(str(Path(here, "the base.bin")))      # given under another name
        self.assertEqual((code, res["read"], res["stats"]["head"]["flags"], res["stats"]["rows"]["base"]),
                         (0, "loaded", 0, head["baseRows"]))
        Path(here, "stats-00000001.bin").write_bytes(st["files"]["stats-00000001.bin"])
        code, res = dump()                                     # beside it
        self.assertEqual((res["read"], res["stats"]["head"], res["stats"]["rows"]["base"]),
                         ("loaded", head, head["baseRows"]))
        Path(here, "other.bin").write_bytes(sr.write_base({}, 1))
        code, res = dump(str(Path(here, "other.bin")))         # not the base it names: lost, as on the card
        self.assertEqual((res["read"], res["stats"]["head"]["flags"]), ("loaded", sr.FLAG_BASE_LOST))
        Path(here, "stats.bin").write_bytes(st["files"]["stats.bin"][:-1])
        code, res = dump()
        self.assertEqual((code, res), (1, {"read": "corrupt", "why": "FileBytes"}))


if __name__ == "__main__":
    unittest.main()
