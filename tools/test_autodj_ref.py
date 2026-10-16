# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Tests for tools/autodj_ref.py, the Auto DJ engine's reference pick rule,
and for its fixture, test/fixtures/autodj/.

    python -m unittest discover -s tools -p "test_autodj_ref.py"

PCG32 against its published outputs; the tempo windows, the Camelot
neighbourhood and the rank tables against a port of mStream's waterfall
(random.js: the steps for one pick, classifyRow's tiers) cell by cell;
D3's imputation odds; the engine's rules one by one on small hand-made
tables (the gate and its clamp, the ring and D1's joins, the quick skip
and whether it steered, the pin (a seed before the join's maps too), the
key anchor, both cooldowns, loosen
before repeating (the device's case, dry and loosened at once, every
listed row cooled), dry and random picks, the read budget, damaged,
failed and broken slots, the stream); and the fixture up to date.
"""
import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import autodj_ref as A  # noqa: E402
import mpdj  # noqa: E402

FIX = Path(__file__).resolve().parent.parent / "test" / "fixtures"


class FixedRng:
    """below(n) answers from a list (checks n as it goes)."""

    def __init__(self, answers):
        self.answers = list(answers)
        self.asked = []

    def below(self, n):
        self.asked.append(n)
        u = self.answers.pop(0)
        assert 0 <= u < n
        return u


# ---------------------------------------------------------------------------
# mStream's waterfall for one pick (random.js 1005-1066, 422-450), as data
# ---------------------------------------------------------------------------
def waterfall_rank(t, k, tempo_on, key_on, anchored):
    """The rank mStream's order gives a row of tempo status t and key status k
    (the engine's codes): the first step that admits it, then its
    classifyRow tier inside that step, numbered over the (step, tier) pairs
    that can occur. Before a key anchor the request carries
    requireMusicalKey and no keys: steps with a key admit any known key, and
    classifyRow sees the key as "na"."""
    has_bpm = has_wide = tempo_on
    has_key = key_on
    steps = []
    if has_bpm or has_key:
        steps.append(("tight" if has_bpm else None, has_key))
    if has_wide and has_key:
        steps.append(("wide", True))
    if has_bpm and has_key:
        steps.append(("tight", False))
    if has_wide:
        steps.append(("wide", False))
    steps.append((None, False))

    def admits(step):
        b, kc = step
        if b == "tight" and t != A.T_TIGHT:
            return False
        if b == "wide" and t not in (A.T_TIGHT, A.T_WIDE):
            return False
        if kc and k not in ((A.K_GOOD,) if anchored else (A.K_GOOD, A.K_WRONG)):
            return False
        return True

    def tier():
        bs = "na" if not tempo_on else {A.T_TIGHT: "good", A.T_UNKNOWN: "unknown"}.get(t, "wrong")
        if not key_on or not anchored:
            ks = "na"
        else:
            ks = {A.K_GOOD: "good", A.K_UNKNOWN: "unknown"}.get(k, "wrong")
        if (bs == "good" and ks != "wrong") or (ks == "good" and bs != "wrong"):
            return 0
        if bs != "wrong" and ks != "wrong":
            return 1
        return 2

    first = next(i for i, s in enumerate(steps) if admits(s))
    return first, tier()


def all_statuses(tempo_on, key_on):
    ts = (A.T_TIGHT, A.T_WIDE, A.T_WRONG, A.T_UNKNOWN) if tempo_on else (A.T_NA,)
    ks = (A.K_GOOD, A.K_WRONG, A.K_UNKNOWN) if key_on else (A.K_NA,)
    return [(t, k) for t in ts for k in ks]


class Rng(unittest.TestCase):
    def test_pcg32_reference_outputs(self):
        # pcg32-demo's first numbers for pcg32_srandom_r(42, 54).
        p = A.Pcg32(42, 54)
        self.assertEqual([p.next() for _ in range(6)],
                         [0xA15C02B7, 0x7B47F409, 0xBA1D3330, 0x83D2F293, 0xBFA4784B, 0xCBED606E])

    def test_below_is_in_range_and_takes_every_value(self):
        p = A.Pcg32(7)
        for n in (1, 2, 3, 7, 50, 169):
            seen = {p.below(n) for _ in range(40 * n)}
            self.assertEqual(seen, set(range(n)))
        self.assertEqual(p.below(1), 0)
        for n in (0x80000001, 0xFFFFFFFF):
            for _ in range(200):
                self.assertLess(p.below(n), n)
        with self.assertRaises(ValueError):
            p.below(0)

    def test_below_is_lemire(self):
        # m = next * n; its high word, unless the low word is under 2^32 mod n.
        p, q = A.Pcg32(99), A.Pcg32(99)
        for n in (3, 1000, 0x80000001):
            for _ in range(500):
                while True:
                    m = q.next() * n
                    if (m & A.M32) >= ((1 << 32) - n) % n:
                        break
                self.assertEqual(p.below(n), m >> 32)

    def test_streams_differ_and_repeat(self):
        a = [A.Pcg32(5).next() for _ in range(3)]
        self.assertEqual(a, [A.Pcg32(5).next() for _ in range(3)])
        self.assertNotEqual(A.Pcg32(5).next(), A.Pcg32(6).next())
        self.assertNotEqual(A.Pcg32(5).next(), A.Pcg32(5, 1).next())


class Tiers(unittest.TestCase):
    def test_windows_half_rounds_up(self):
        self.assertEqual(A.windows(121, 8), ((113, 129), (53, 69), (234, 250)))
        self.assertEqual(A.windows(120, 8), ((112, 128), (52, 68), (232, 248)))
        self.assertEqual(A.windows(1, 10), ((-9, 11), (-9, 11), (-8, 12)))

    def test_tempo_status(self):
        ref = 121
        cases = {113: A.T_TIGHT, 129: A.T_TIGHT, 112: A.T_WIDE, 111: A.T_WIDE, 110: A.T_WRONG, 131: A.T_WIDE,
                 132: A.T_WRONG, 61: A.T_TIGHT, 69: A.T_TIGHT, 70: A.T_WIDE, 72: A.T_WRONG, 242: A.T_TIGHT,
                 252: A.T_WIDE, 253: A.T_WRONG, 0: A.T_UNKNOWN, 255: A.T_WRONG, 51: A.T_WIDE, 50: A.T_WRONG}
        for bpm, want in cases.items():
            self.assertEqual(A.tempo_status(bpm, ref), want, bpm)
        self.assertEqual(A.tempo_status(120, 0), A.T_NA)
        self.assertEqual(A.tempo_status(0, 0), A.T_NA)

    def test_camelot_six_codes_with_the_wrap(self):
        def near(c):
            return sorted(x for x in range(1, 25) if A.camelot_near(c, x))
        self.assertEqual(near(1), [1, 2, 12, 13, 14, 24])       # 1A: 12A 1A 2A 12B 1B 2B
        self.assertEqual(near(12), [1, 11, 12, 13, 23, 24])     # 12A
        self.assertEqual(near(13), [1, 2, 12, 13, 14, 24])      # 1B: the same six
        self.assertEqual(near(20), [7, 8, 9, 19, 20, 21])       # 8B
        for a in range(1, 25):
            self.assertEqual(len(near(a)), 6)
            for b in range(1, 25):
                self.assertEqual(A.camelot_near(a, b), A.camelot_near(b, a))

    def test_key_status(self):
        self.assertEqual(A.key_status(5, 1, False), A.K_NA)
        self.assertEqual(A.key_status(0, 1, True), A.K_UNKNOWN)
        self.assertEqual(A.key_status(5, 0, True), A.K_GOOD)      # no anchor yet: any known key
        self.assertEqual(A.key_status(2, 1, True), A.K_GOOD)
        self.assertEqual(A.key_status(3, 1, True), A.K_WRONG)

    def test_ranks_follow_the_waterfall_cell_by_cell(self):
        for tempo_on in (False, True):
            for key_on in (False, True):
                pairs = sorted({waterfall_rank(t, k, tempo_on, key_on, True)
                                for t, k in all_statuses(tempo_on, key_on)})
                for t, k in all_statuses(tempo_on, key_on):
                    want = pairs.index(waterfall_rank(t, k, tempo_on, key_on, True))
                    self.assertEqual(A.music_rank(t, k), want, (tempo_on, key_on, t, k))

    def test_ranks_before_a_key_anchor(self):
        # Before an anchor every known key is good (the design's 1.4.1). Against
        # mStream's order (requireMusicalKey, classifyRow's "na"), only an
        # unknown tempo with a known key differs: 5 here, 6 there (it ties
        # with both unknown).
        for t, k in all_statuses(True, True):
            if k == A.K_WRONG:
                continue
            seen = waterfall_rank(t, k, True, True, False)
            if (t, k) == (A.T_UNKNOWN, A.K_GOOD):
                self.assertEqual(seen, waterfall_rank(A.T_UNKNOWN, A.K_UNKNOWN, True, True, False))
                self.assertEqual(A.music_rank(t, k), 5)
            else:
                self.assertEqual(seen, waterfall_rank(t, k, True, True, True), (t, k))
        for k in (A.K_GOOD, A.K_UNKNOWN):
            self.assertEqual(waterfall_rank(A.T_NA, k, False, True, False)[0] > 0, k == A.K_UNKNOWN)

    def test_the_design_tables(self):
        both = {(A.T_TIGHT, A.K_GOOD): 0, (A.T_WIDE, A.K_GOOD): 1, (A.T_TIGHT, A.K_UNKNOWN): 2,
                (A.T_TIGHT, A.K_WRONG): 3, (A.T_WIDE, A.K_UNKNOWN): 4, (A.T_WIDE, A.K_WRONG): 4,
                (A.T_UNKNOWN, A.K_GOOD): 5, (A.T_UNKNOWN, A.K_UNKNOWN): 6, (A.T_UNKNOWN, A.K_WRONG): 7,
                (A.T_WRONG, A.K_GOOD): 7, (A.T_WRONG, A.K_WRONG): 7, (A.T_WRONG, A.K_UNKNOWN): 7}
        for (t, k), r in both.items():
            self.assertEqual(A.music_rank(t, k), r)
        self.assertEqual([A.music_rank(t, A.K_NA) for t in (A.T_TIGHT, A.T_WIDE, A.T_UNKNOWN, A.T_WRONG)],
                         [0, 1, 2, 3])
        self.assertEqual([A.music_rank(A.T_NA, k) for k in (A.K_GOOD, A.K_UNKNOWN, A.K_WRONG)], [0, 1, 2])
        self.assertEqual(A.music_rank(A.T_NA, A.K_NA), 0)

    def test_imputation_odds_in_integers(self):
        tempo = [A.T_TIGHT, A.T_UNKNOWN, A.T_WIDE, A.T_WRONG, A.T_TIGHT, A.T_UNKNOWN, A.T_UNKNOWN]
        key = [A.K_UNKNOWN, A.K_GOOD, A.K_WRONG, A.K_UNKNOWN, A.K_GOOD, A.K_GOOD, A.K_NA]
        # nT 2, nW 1, nX 1: u 0-1 tight, 2 wide, 3 wrong; nG 3, nX 1: u 0-2 good, 3 wrong.
        r = FixedRng([1, 2, 3, 2, 3])
        t, k, it, ik = A.impute(tempo, key, r)
        self.assertEqual(r.asked, [4, 4, 4, 4, 4])
        self.assertEqual(t, [A.T_TIGHT, A.T_TIGHT, A.T_WIDE, A.T_WRONG, A.T_TIGHT, A.T_WIDE, A.T_WRONG])
        self.assertEqual(it, [False, True, False, False, False, True, True])
        self.assertEqual(k, [A.K_GOOD, A.K_GOOD, A.K_WRONG, A.K_WRONG, A.K_GOOD, A.K_GOOD, A.K_NA])
        self.assertEqual(ik, [True, False, False, True, False, False, False])

    def test_imputation_without_a_known_value(self):
        r = FixedRng([])
        t, k, it, ik = A.impute([A.T_UNKNOWN, A.T_UNKNOWN], [A.K_UNKNOWN, A.K_NA], r)
        self.assertEqual((t, k, it, ik), ([A.T_UNKNOWN] * 2, [A.K_UNKNOWN, A.K_NA], [False] * 2, [False] * 2))
        self.assertEqual(r.asked, [])

    def test_floor_levels(self):
        self.assertEqual([A.floor_level(p, 0) for p in (55, 60, 65, 70, 75)], [141, 153, 166, 179, 192])
        self.assertEqual(A.floor_level(30, 140), 140)
        self.assertEqual(A.floor_level(55, 160), 160)
        self.assertEqual(A.floor_level(150, 0), 255)
        for p in range(0, 101):
            self.assertEqual(A.floor_level(p, 0), mpdj.floor_level(p))

    def test_the_filter_word(self):
        f = A.pack_filter(1205, 7, True, 0xB460A770)
        self.assertEqual((A.bpm_of(f), A.camelot_of(f), A.artist_of(f)), (121, 7, 0xA770))
        self.assertTrue(f & A.FILTER_ANALYSED)
        self.assertFalse(f & A.FILTER_PRESENT)
        self.assertEqual(A.bpm_of(A.pack_filter(1204, 0, False, 0)), 120)
        self.assertEqual(A.bpm_of(A.pack_filter(9999, 0, False, 0)), 255)
        self.assertEqual(A.camelot_of(A.pack_filter(0, 25, False, 0)), 0)
        self.assertEqual(A.artist_of(A.pack_filter(0, 0, False, 0x12340000)), 1)   # low 16 bits 0, key not 0
        self.assertEqual(A.artist_of(A.pack_filter(0, 0, False, 0)), 0)
        self.assertTrue(A.pack_filter(0, 0, False, 0, present=True) & A.FILTER_PRESENT)


# ---------------------------------------------------------------------------
# The engine on hand-made tables
# ---------------------------------------------------------------------------
def table(lists, filters=None, tracks=None):
    """Rows 0..n-1 with these lists; track t is row t unless `tracks` maps
    tracks to rows (NO_ROW: a track with no row)."""
    n = len(lists)
    filters = filters or [0] * n
    t2r = tracks if tracks is not None else list(range(n))
    r2t = [A.NO_ROW] * n
    for t, r in enumerate(t2r):
        if r != A.NO_ROW and r2t[r] == A.NO_ROW:
            r2t[r] = t
    return A.Maps(t2r, r2t, filters)


def star(n, centre=0, level=200):
    """Row `centre` lists every other row at `level` - row; the others list
    the centre only."""
    lists = []
    for r in range(n):
        if r == centre:
            lists.append([(b, max(level - b, 1)) for b in range(n) if b != r])
        else:
            lists.append([(centre, level)])
    return lists


def full(n):
    """Every row lists every other, nearer rows higher."""
    return [sorted(((b, 250 - abs(a - b)) for b in range(n) if b != a), key=lambda e: (-e[1], e[0]))
            for a in range(n)]


class EngineRules(unittest.TestCase):
    def pin(self, maps, src, playing, sel_floor=140, k=169):
        return A.PickIn(maps, src, k=k, sel_floor=sel_floor, playing=playing, library_tracks=maps.tracks)

    def test_the_defaults(self):
        # D2: BPM continuity and Harmonic mixing off by default.
        s = A.Settings()
        self.assertEqual((s.mode, s.floor, s.tempo, s.key), (A.ROLLING, 55, False, False))

    def test_the_gate_and_its_clamp(self):
        lists = [[(1, 200), (2, 150), (3, 145), (4, 141), (5, 140), (6, 139)]] + [[(0, 200)]] * 6
        maps = table(lists)
        src = A.SlotSource(lists)
        for floor, sel_floor, gated in ((55, 140, 4), (55, 0, 4), (30, 140, 5), (30, 0, 6), (60, 140, 1),
                                        (75, 140, 1), (55, 145, 3)):
            e = A.Engine()
            e.new_session(1, 0, maps)
            p = e.pick(A.Settings(A.ROLLING, floor), self.pin(maps, src, 0, sel_floor))
            want = gated if not (floor == 30 and sel_floor == 0) else 6
            self.assertEqual((p.gated, p.dry), (want, False), (floor, sel_floor))
            self.assertGreaterEqual(p.level, A.floor_level(floor, sel_floor))

    def test_dry_takes_the_50_best_sums(self):
        n = 70
        # Anchors 0 and 1 (row 1 the seed's pick) both list rows 2..69 below
        # the gate; the sums decide, ties by row.
        lists = [[]] * n
        lists = [list(x) for x in lists]
        lists[0] = [(r, 130 - (r % 7)) for r in range(2, n)]
        lists[0].sort(key=lambda e: (-e[1], e[0]))
        lists[1] = [(r, 100 + (r % 5)) for r in range(2, n)]
        lists[1].sort(key=lambda e: (-e[1], e[0]))
        maps = table(lists)
        e = A.Engine()
        e.new_session(3, 0, maps)
        e.track_started(1, maps, False)
        p = e.pick(A.Settings(A.ROLLING, 55), self.pin(maps, A.SlotSource(lists), 1))
        self.assertTrue(p.dry)
        self.assertEqual(p.gated, 50)
        sums = sorted(((130 - r % 7) + (100 + r % 5), r) for r in range(2, n))
        best50 = sorted(sorted(sums, key=lambda s: (-s[0], s[1]))[:50], key=lambda s: s[1])
        self.assertIn(p.row, [r for _, r in best50])

    def test_dry_ties_go_to_the_lower_rows(self):
        # Every sum tied: the 50 lowest rows, the anchors and the playing row
        # left out first.
        t = A.ties_table()
        maps = table(t["lists"])
        seen = set()
        for seed in range(40):
            e = A.Engine()
            e.new_session(seed, 0, maps)
            p = e.pick(A.Settings(A.LOCKED), self.pin(maps, A.SlotSource(t["lists"]), 0))
            self.assertEqual((p.dry, p.candidates, p.gated), (True, 100, 50))
            self.assertTrue(1 <= p.row <= 50, p.row)
            seen.add(p.row)
            f = A.Engine()
            f.new_session(seed, 0, maps)
            f.track_started(1, maps, False)
            q = f.pick(A.Settings(A.ROLLING, 65), self.pin(maps, A.SlotSource(t["lists"]), 1))
            self.assertEqual((q.dry, q.candidates, q.gated), (True, 99, 50))
            self.assertTrue(2 <= q.row <= 51, q.row)
        self.assertGreaterEqual(len(seen), 20)

    def test_the_ring_d1_and_the_quick_skip(self):
        lists = star(12)
        maps = table(lists)
        e = A.Engine()
        e.new_session(5, 3, maps)
        self.assertEqual(e.ring, [3])
        for t in (4, 5, 6, 7, 8):
            e.track_started(t, maps, False)
        self.assertEqual(e.ring, [4, 5, 6, 7, 8])          # 5 at most, the seed out
        e.track_started(5, maps, False)
        self.assertEqual(e.ring, [4, 6, 7, 8, 5])          # a track again: to the end
        e.track_started(3, maps, True)                     # Auto DJ's (row cooled: joined when picked): no move
        self.assertEqual(e.ring, [4, 6, 7, 8, 5])
        e.track_left(5, maps, 29999, False)                # a quick skip: out of the ring
        self.assertEqual(e.ring, [4, 6, 7, 8])
        self.assertIn(5, e.cool)                           # ... still cooled
        e.track_left(8, maps, 12000, True)                 # a natural end under 30 s: stays
        e.track_left(7, maps, 30000, False)                # 30 s played: stays
        self.assertEqual(e.ring, [4, 6, 7, 8])
        self.assertEqual(e.stats.quick_skips, 1)
        p = e.pick(A.Settings(), self.pin(maps, A.SlotSource(lists), 8))
        self.assertEqual(e.ring[-1], p.row)                # the pick joins when picked

    def test_a_pick_by_id_steers_once_the_index_is_there(self):
        lists = star(10)
        maps = table(lists)
        e = A.Engine()
        e.new_session(3, 4)                                 # no maps: the seed's row unknown
        p = e.pick(A.Settings(), A.PickIn(None, None, playing=4, library_tracks=maps.tracks))
        self.assertTrue(p.ok and p.no_index)
        e.track_started(p.track, maps, True)                # the index is back when it starts
        self.assertEqual(e.ring, [maps.track_to_row[p.track]])
        q = e.pick(A.Settings(), self.pin(maps, A.SlotSource(lists), p.track))
        self.assertFalse(q.random)                          # it anchors this pick
        e.track_started(7, maps, False)
        ring = list(e.ring)
        self.assertEqual(ring[1], q.row)
        e.track_started(q.track, maps, True)                # joined when picked: stays where it is
        self.assertEqual(e.ring, ring)

    def test_tracks_without_rows_never_anchor(self):
        lists = star(6)
        maps = table(lists, tracks=[0, 1, 2, 3, 4, 5, A.NO_ROW, A.NO_ROW])
        e = A.Engine()
        e.new_session(9, 6, maps)                           # the seed has no row
        self.assertEqual((e.ring, e.pin, e.seeded), ([], A.NO_ROW, True))
        p = e.pick(A.Settings(A.LOCKED), self.pin(maps, A.SlotSource(lists), 6))
        self.assertTrue(p.random and p.ok)
        self.assertEqual(e.pin, p.row)                      # the first pick becomes the pin
        e.track_started(7, maps, False)
        self.assertEqual(e.ring, [p.row])

    def test_the_pin_and_anchor_changes(self):
        lists = star(10)
        maps = table(lists)
        src = A.SlotSource(lists)
        e = A.Engine()
        e.new_session(2, 0, maps)
        s = A.Settings(A.LOCKED)
        for _ in range(4):
            p = e.pick(s, self.pin(maps, src, 0))
            self.assertEqual(e.pin, 0)
            self.assertNotEqual(p.row, 0)
        e.mode_changed()
        self.assertEqual(e.pin, A.NO_ROW)
        p = e.pick(s, self.pin(maps, src, 4))               # the playing track becomes the pin
        self.assertEqual(e.pin, 4)
        self.assertEqual(p.row, 0)                          # row 4 lists only the centre
        self.assertEqual(e.prefetch_due(s), None)

    def test_a_seed_before_the_join_becomes_the_pin(self):
        lists = star(10)
        maps = table(lists)
        src = A.SlotSource(lists)
        s = A.Settings(A.LOCKED)
        e = A.Engine()
        e.new_session(5, 3)                                 # "Auto DJ from here" while it joins: no maps
        self.assertEqual((e.pin, e.ring, e.seeded), (A.NO_ROW, [], True))
        e.track_left(3, None, 2000, False)                  # skipped before the maps came
        e.track_started(6, None, False)
        e.maps_arrived(maps)
        self.assertEqual(e.pin, 3)                          # the seed's row, not the playing track's
        self.assertEqual(e.prefetch_due(s), 3)
        e.track_started(6, maps, False)                     # (the loop step: the playing track with the maps)
        self.assertEqual(e.ring, [6])
        p = e.pick(s, self.pin(maps, src, 6))
        self.assertEqual((e.pin, p.row), (3, 0))            # row 3 lists only the centre
        e.maps_arrived(maps)                                # once a session
        self.assertEqual(e.pin, 3)
        for change in ("mode", "renumber", "taken"):
            e.new_session(5, 3)
            if change == "mode":
                e.mode_changed()
            elif change == "renumber":
                e.tracks_renumbered()
            else:
                e.pick(s, self.pin(maps, src, 6))           # a pick with the maps took the pin first
            e.maps_arrived(maps)
            self.assertEqual(e.pin, 6 if change == "taken" else A.NO_ROW, change)
        rowless = table(lists, tracks=list(range(10)) + [A.NO_ROW])
        e.new_session(5, 10)                                # a seed without a row: no pin
        e.maps_arrived(rowless)
        self.assertEqual(e.pin, A.NO_ROW)
        e.new_session(5, 3, maps)                           # the maps at hand: nothing waits
        e.mode_changed()
        e.maps_arrived(maps)
        self.assertEqual(e.pin, A.NO_ROW)

    def test_the_session_seed_from_the_first_pick(self):
        lists = star(8)
        maps = table(lists)
        e = A.Engine()
        e.new_session(4)
        self.assertFalse(e.seeded)
        e.pick(A.Settings(A.LOCKED), self.pin(maps, A.SlotSource(lists), 0))
        self.assertTrue(e.seeded)
        self.assertEqual(e.pin, 0)
        e.new_session(4)
        e.track_started(3, maps, False)                     # the first track to start: the ring's, no pin
        self.assertEqual((e.ring, e.pin, e.seeded), ([3], A.NO_ROW, True))
        e.track_started(5, maps, False)
        p = e.pick(A.Settings(A.LOCKED), self.pin(maps, A.SlotSource(lists), 5))
        self.assertEqual((e.pin, p.reads, p.row), (5, 1, 0))    # the playing track at the first pick

    def test_the_key_anchor_is_lazy(self):
        lists = star(10)
        filters = [A.pack_filter(1200, 0 if r in (0, 1) else 1 + r, False, 0) for r in range(10)]
        maps = table(lists, filters)
        src = A.SlotSource(lists)
        e = A.Engine()
        e.new_session(6, 0, maps)
        e.pick(A.Settings(A.LOCKED, 55, False, False), self.pin(maps, src, 0))
        self.assertEqual(e.key_anchor, 0)                   # off: never taken
        p = e.pick(A.Settings(A.LOCKED, 55, False, True), self.pin(maps, src, 1))
        self.assertEqual(e.key_anchor, A.camelot_of(filters[p.row]))   # the playing track had none: the pick's
        before = e.key_anchor
        e.pick(A.Settings(A.LOCKED, 55, False, False), self.pin(maps, src, 5))
        self.assertEqual(e.key_anchor, before)              # off keeps it
        e.new_session(7, 0, maps)
        e.pick(A.Settings(A.LOCKED, 55, False, True), self.pin(maps, src, 6))
        self.assertEqual(e.key_anchor, 7)                   # the playing track's first

    def test_the_cooldowns(self):
        n = 80
        lists = star(n)
        art = [r % 4 for r in range(n)]                    # artists 0 (none), 1, 2, 3
        maps = table(lists, [A.pack_filter(0, 0, False, a) for a in art])
        src = A.SlotSource(lists)
        e = A.Engine()
        e.new_session(8, 0, maps)
        picks = []
        for _ in range(60):
            p = e.pick(A.Settings(A.LOCKED), self.pin(maps, src, picks[-1] if picks else 0))
            picks.append(p.row)
        self.assertEqual(len(set(picks[:50])), 50)          # no repeat within 50
        self.assertEqual(len(e.cool), 50)
        self.assertEqual(len(e.arts), 3)
        # Three artists cooled: while a fresh row of an uncooled artist (0: none) is
        # left, it wins over the cooled ones.
        e2 = A.Engine()
        e2.new_session(9, 0, maps)
        levels = []
        for i in range(12):
            p = e2.pick(A.Settings(A.LOCKED), self.pin(maps, src, 0))
            levels.append(p.cooldown_level)
            if p.cooldown_level == 0:
                self.assertTrue(art[p.row] == 0 or art[p.row] not in e2.arts[:-1])
        self.assertEqual(levels[:6].count(2), 0)

    def test_loosen_before_repeating_the_device_case(self):
        # Stay on seed at 75 % (level 192) from a seed listing 3 rows above
        # the floor and 57 below: once the 3 are cooled, the 50 nearest rows
        # outside the track cooldown (the old rule repeated the 3).
        n = 61
        lv = {r: (202 - 2 * r if r <= 3 else 195 - r) for r in range(1, n)}
        lists = [[(r, lv[r]) for r in range(1, n)]] + [[(0, 200)] for _ in range(1, n)]
        maps = table(lists, [A.pack_filter(1200, 1 + r % 24, False, 1 + r % 12) for r in range(n)])
        src = A.SlotSource(lists)
        e = A.Engine()
        e.new_session(0x717FEDA5, 0, maps)
        playing, loosened, seen = 0, 0, set()
        for i in range(200):
            fresh = [r for r in range(1, n) if r not in e.cool and r != playing]
            p = e.pick(A.Settings(A.LOCKED, 75), self.pin(maps, src, playing))
            self.assertTrue(p.ok and not p.random and not p.dry)
            self.assertLess(p.cooldown_level, 2)            # never a repeat inside the cooldown
            if not [r for r in fresh if r <= 3]:
                self.assertTrue(p.loosened, i)
                self.assertEqual(p.gated, min(50, len(fresh)))
                self.assertLess(p.level, 192)
                self.assertIn(p.row, fresh[:50])            # the 50 nearest fresh rows
                loosened += 1
            else:
                self.assertFalse(p.loosened, i)
                self.assertIn(p.row, (1, 2, 3))
            seen.add(p.row)
            playing = p.track
        self.assertEqual(len(seen), 60)
        self.assertEqual(e.stats.loosened, loosened)
        self.assertTrue(180 <= loosened <= 194, loosened)

    def test_dry_and_loosened(self):
        # The anchor lists 60 rows below the gate (dry: rows 1-50 have the
        # best sums); quick skips have cooled exactly those, and the playing
        # track has no row: the 50 best sums outside the cooldown, 51-60.
        n = 61
        lists = [[(r, 139 - (r - 1) // 2) for r in range(1, n)]] + [[(0, 139)] for _ in range(1, n)]
        maps = table(lists, tracks=list(range(n)) + [A.NO_ROW])
        e = A.Engine()
        e.new_session(4, 0, maps)
        for r in range(1, 51):
            e.track_started(r, maps, False)
            self.assertTrue(e.track_left(r, maps, 1000, False))
        e.track_started(61, maps, False)
        self.assertEqual(e.ring, [0])
        p = e.pick(A.Settings(), self.pin(maps, A.SlotSource(lists), 61))
        self.assertEqual((p.dry, p.loosened, p.candidates, p.gated, p.cooldown_level), (True, True, 60, 10, 0))
        self.assertTrue(51 <= p.row <= 60)

    def test_every_listed_row_cooled_repeats(self):
        # Nothing outside the cooldown is listed: the pool stays, a repeat.
        lists = star(6)
        maps = table(lists)
        src = A.SlotSource(lists)
        e = A.Engine()
        e.new_session(3, 0, maps)
        for i in range(10):
            p = e.pick(A.Settings(A.LOCKED, 75), self.pin(maps, src, 0))
            self.assertFalse(p.loosened)
            self.assertEqual(p.cooldown_level, 0 if i < 5 else 2)

    def test_a_quick_skip_says_whether_it_steered(self):
        lists = star(8)
        maps = table(lists, tracks=list(range(8)) + [A.NO_ROW])
        e = A.Engine()
        e.new_session(1, 0, maps)
        e.track_started(3, maps, False)
        self.assertTrue(e.track_left(3, maps, 2000, False))
        self.assertFalse(e.track_left(3, maps, 2000, False))  # already out of the ring
        e.track_started(8, maps, False)
        self.assertFalse(e.track_left(8, maps, 2500, False))  # no row: it never steered
        self.assertFalse(e.track_left(0, maps, 1000, True))   # a natural end
        self.assertEqual(e.stats.quick_skips, 3)

    def test_cooldown_before_music(self):
        # Row 1 is a tempo match but cooled; row 2 is fresh and wrong: row 2.
        lists = [[(1, 200), (2, 199)], [(0, 200)], [(0, 199)]]
        filters = [A.pack_filter(1200, 0, False, 0), A.pack_filter(1200, 0, False, 0), A.pack_filter(900, 0, False, 0)]
        maps = table(lists, filters)
        e = A.Engine()
        e.new_session(1, 0, maps)
        e.cool.append(1)
        p = e.pick(A.Settings(A.LOCKED, 55, True, False), self.pin(maps, A.SlotSource(lists), 0))
        self.assertEqual((p.row, p.cooldown_level, p.music_rank, p.tempo_break), (2, 0, 3, True))

    def test_the_read_budget_and_prefetch(self):
        lists = star(12)
        maps = table(lists)
        src = A.SlotSource(lists)
        e = A.Engine()
        e.new_session(1, 0, maps)
        s = A.Settings()
        for t in (1, 2, 3, 4):
            e.track_started(t, maps, False)
        self.assertEqual(e.prefetch_due(s), 4)              # newest first
        self.assertEqual(e.prefetch(src, 4), A.R_OK)
        self.assertEqual(e.prefetch_due(s), 3)
        p = e.pick(s, self.pin(maps, src, 4))
        self.assertEqual(p.reads, 2)                        # 3 and 2; 1 and 0 left out
        self.assertEqual(e.stats.left_out, 2)
        self.assertEqual(src.reads, 3)
        self.assertEqual(e.stats.reads_at_pick, [0, 0, 1])

    def test_the_cache_keeps_the_ring_and_the_pin(self):
        lists = full(20)
        maps = table(lists)
        src = A.SlotSource(lists)
        s = A.Settings()
        e = A.Engine()
        e.new_session(1)
        for t in range(1, 7):                               # 5 and 6 quickly skipped: the newest slots
            e.track_started(t, maps, False)
            self.assertEqual(e.prefetch(src, e.prefetch_due(s)), A.R_OK)
            if t >= 5:
                e.track_left(t, maps, 5000, False)
        e.track_started(7, maps, False)
        self.assertEqual(e.prefetch_due(s), 7)
        e.prefetch(src, 7)                                  # takes 5's slot, not 1's (the oldest, in the ring)
        self.assertEqual(sorted(c[0] for c in e.cache), [1, 2, 3, 4, 6, 7])
        f = A.Engine()
        f.new_session(2, 0, maps)                           # the pin: row 0, read first
        f.prefetch(src, f.prefetch_due(A.Settings(A.LOCKED)))
        for t in range(1, 7):
            f.track_started(t, maps, False)
            f.prefetch(src, f.prefetch_due(s))
            self.assertIsNone(f.prefetch_due(s))
        self.assertEqual(sorted(c[0] for c in f.cache), [0, 2, 3, 4, 5, 6])

    def test_damaged_io_and_broken(self):
        lists = full(12)
        maps = table(lists)
        s = A.Settings()
        src = A.SlotSource(lists, {"damaged": {3}, "io": {2}, "flaky": {1}})
        e = A.Engine()
        e.new_session(1, 0, maps)
        for t in (1, 2, 3):
            e.track_started(t, maps, False)
        self.assertEqual(e.prefetch(src, 3), A.R_SUSPECT)
        self.assertEqual(e.prefetch(src, 3), A.R_DAMAGED)   # twice in a row: the ring
        self.assertEqual(e.damaged, [3])
        self.assertEqual(e.prefetch_due(s), 2)              # never re-read
        self.assertEqual(e.prefetch(src, 2), A.R_IO)        # Io: skipped, not in the ring
        self.assertEqual(e.damaged, [3])
        self.assertEqual(e.prefetch_due(s), 2)
        p = e.pick(s, self.pin(maps, src, 3))
        self.assertEqual(p.reads, 2)                        # 2 (ok now), then 1 (flaky: suspect)
        self.assertFalse(p.random)
        p = e.pick(s, self.pin(maps, src, p.track))
        self.assertIn(1, [c[0] for c in e.cache if c])     # read again: good
        e.file_reopened()
        self.assertEqual((e.damaged, e.cache), ([], [None] * A.SLOT_CACHE))
        # Broken: the file is absent, random picks without reads.
        src = A.SlotSource(lists, {"broken": {5}})
        e = A.Engine()
        e.new_session(1, 5, maps)
        p = e.pick(s, self.pin(maps, src, 5))
        self.assertTrue(p.broken and p.random and p.ok)
        self.assertEqual(e.prefetch_due(s), None)
        reads = src.reads
        p = e.pick(s, self.pin(maps, src, p.track))
        self.assertTrue(p.broken and p.random)
        self.assertEqual(src.reads, reads)

    def test_every_anchor_damaged_gives_a_random_pick(self):
        lists = star(12)
        maps = table(lists)
        src = A.SlotSource(lists, {"damaged": {0}})
        e = A.Engine()
        e.new_session(1, 0, maps)
        p = e.pick(A.Settings(A.LOCKED), self.pin(maps, src, 0))
        self.assertTrue(p.random)
        self.assertEqual((p.reads, e.damaged), (2, [0]))
        self.assertNotEqual(p.row, 0)
        self.assertEqual(e.pin, p.row)                      # the damaged pin gives way to the random pick
        q = e.pick(A.Settings(A.LOCKED), self.pin(maps, src, p.track))
        self.assertEqual((q.random, q.reads, src.count[0]), (False, 1, 2))

    def test_no_index_by_track_id(self):
        e = A.Engine()
        e.new_session(3, 2)
        pin = A.PickIn(None, None, playing=2, library_tracks=10)
        got = []
        for _ in range(9):
            p = e.pick(A.Settings(), pin)
            self.assertTrue(p.ok and p.no_index and p.random)
            got.append(p.track)
        self.assertNotIn(2, got)
        self.assertEqual(len(set(got)), 9)                  # the cooldown by id
        e.tracks_renumbered()
        self.assertEqual(e.ids, [])
        p = e.pick(A.Settings(), A.PickIn(None, None, playing=0, library_tracks=1))
        self.assertFalse(p.ok)                              # the only track is the playing one
        self.assertEqual(e.stats.failed, 1)

    def test_surprise(self):
        lists = star(6)
        maps = table(lists, tracks=[0, 2, 4])
        e = A.Engine()
        e.new_session(5)
        for _ in range(20):
            p = e.surprise(A.Settings(), self.pin(maps, A.SlotSource(lists), A.NO_TRACK))
            self.assertIn(p.row, (0, 2, 4))
            self.assertEqual(maps.track_to_row[p.track], p.row)
        self.assertFalse(e.surprise(A.Settings(), A.PickIn(None, None)).ok)

    def test_the_same_seed_gives_the_same_session(self):
        tables, sc = A.scenes()
        again = {s.name: s.json() for s in A.scenes()[1]}
        for s in sc:
            self.assertEqual(s.json(), again[s.name])


class Fixture(unittest.TestCase):
    def test_the_fixture_is_up_to_date(self):
        for name, data in A.generate().items():
            self.assertEqual((FIX / "autodj" / name).read_bytes(), data, name)

    def test_the_fixture_covers_the_rules(self):
        doc = json.loads((FIX / "autodj" / "sessions.json").read_text(encoding="utf-8"))
        picks = [e["expect"] for s in doc["sessions"] for e in s["events"] if e["op"] == "pick"]
        self.assertGreaterEqual(len(picks), 300)
        for flag in ("dry", "loose", "random", "noIndex", "impT", "impK", "tb", "kb", "broken"):
            self.assertTrue(any(p.get(flag) for p in picks), flag)
        self.assertTrue(any(p.get("dry") and p.get("loose") for p in picks))
        self.assertFalse(any(p.get("loose") and p.get("cl", 0) == 2 for p in picks))   # a loosened pool is fresh
        for cl in (0, 1, 2):
            self.assertTrue(any(p.get("ok") and p.get("cl", 0) == cl and not p.get("noIndex") for p in picks), cl)
        self.assertEqual({p.get("reads", 0) for p in picks}, {0, 1, 2})
        st = [s["stats"] for s in doc["sessions"]]
        for key in ("damaged", "ioSkips", "leftOut", "quickSkips", "prefetchReads"):
            self.assertTrue(any(x[key] for x in st), key)
        ranks = {p.get("mr", 0) for p in picks if p.get("ok") and not p.get("noIndex")}
        self.assertEqual(ranks, set(range(8)))
        rng = doc["rng"]
        p = A.Pcg32(rng["seed"], rng["stream"])
        self.assertEqual(rng["next"], [p.next() for _ in range(6)])


if __name__ == "__main__":
    unittest.main()
