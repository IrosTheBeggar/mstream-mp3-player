# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Tests for the beat tracker's evaluation scorer (tools/beat_eval/score.py).

    python -m unittest discover -s tools/beat_eval -p "test_*.py"

Needs numpy (the corpus's venv has it). No corpus, no runner: the tracker's
output is made up here, so each metric can be checked against a known answer.
"""
import sys
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import beat_eval  # noqa: E402
import score  # noqa: E402


def fake_output(beats, bpm, locked_from=0.0, conf=0.9, T_bpm=None, duration=None, lock_s=None):
    """The runner's output for a tracker that reports `beats` (s) at `bpm`."""
    beats = np.asarray(beats, dtype=float)
    period = 60.0 / bpm
    B = np.array([[t, period, bpm, conf, 1.0 if t >= locked_from else 0.0, i] for i, t in enumerate(beats)])
    duration = duration if duration is not None else (beats[-1] + 1.0 if len(beats) else 10.0)
    ts = np.arange(0.0929, duration, 0.0929)
    locked = (ts >= locked_from).astype(float)
    T = np.column_stack([ts, np.full_like(ts, T_bpm or bpm), np.full_like(ts, T_bpm or bpm), np.full_like(ts, 0.3),
                         np.full_like(ts, conf), locked, np.ones_like(ts)])
    return dict(meta={}, end={"lock_s": locked_from if lock_s is None else lock_s, "ns_per_audio_s": 1.0},
                T=T.reshape(-1, 7), B=B.reshape(-1, 6), R=np.array([]))


def grid(bpm, start, end, phase=0.0):
    p = 60.0 / bpm
    return np.arange(start + phase * p, end, p)


class Relations(unittest.TestCase):
    def test_octaves_and_metres(self):
        self.assertEqual(score.relation(120.0, 120.0), "1")
        self.assertEqual(score.relation(240.5, 120.0), "2")
        self.assertEqual(score.relation(60.0, 121.0), "1/2")
        self.assertEqual(score.relation(88.0, 117.3), "3/4")
        self.assertEqual(score.relation(137.3, 103.0), "4/3")
        self.assertEqual(score.relation(130.8, 87.3), "3/2")
        self.assertEqual(score.relation(100.0, 120.0), "other")
        self.assertEqual(score.relation(0.0, 120.0), "none")
        # 4 % is the edge
        self.assertEqual(score.relation(124.7, 120.0), "1")
        self.assertEqual(score.relation(125.0, 120.0), "other")


class FMeasure(unittest.TestCase):
    def test_exact_shifted_and_double(self):
        ref = grid(120, 0.0, 30.0)
        self.assertAlmostEqual(score.fmeasure(ref, ref), 1.0)
        self.assertAlmostEqual(score.fmeasure(ref + 0.069, ref), 1.0)  # inside +-70 ms
        self.assertAlmostEqual(score.fmeasure(ref + 0.08, ref), 0.0)
        double = grid(240, 0.0, 30.0)
        self.assertAlmostEqual(score.fmeasure(double, ref), 2 * 60 / (120 + 60), places=2)
        self.assertIsNone(score.fmeasure(np.array([]), np.array([])))

    def test_one_to_one(self):
        ref = np.array([1.0, 2.0])
        est = np.array([0.98, 1.01, 1.03, 2.0])  # three near the first beat count once
        self.assertEqual(score.matches(est, ref), 2)


class Reference(unittest.TestCase):
    def test_segments_and_gaps(self):
        beats = np.concatenate([grid(120, 0.0, 10.0), grid(120, 20.0, 30.0)])
        ref = score.Ref(beats, 30.0)
        self.assertEqual(len(ref.segments), 2)
        self.assertTrue(ref.in_span(5.0))
        self.assertFalse(ref.in_span(15.0))
        self.assertAlmostEqual(ref.period_at(5.0), 0.5)
        # the half-beats inside each segment and one either side of it, none across the gap
        self.assertEqual(len(ref.mids), len(beats) - 2 + 4)
        self.assertFalse(((ref.mids > 10.3) & (ref.mids < 19.7)).any())

    def test_local_period_follows_a_tempo_change(self):
        beats = np.concatenate([grid(120, 0.0, 20.0), grid(90, 20.0, 40.0)])
        ref = score.Ref(beats, 40.0)
        self.assertAlmostEqual(ref.period_at(5.0), 0.5)
        self.assertAlmostEqual(ref.period_at(35.0), 60 / 90, places=3)


class ScoreCase(unittest.TestCase):
    def setUp(self):
        self.ref = score.Ref(grid(120, 1.0, 61.0), 62.0)

    def test_perfect_from_five_seconds(self):
        out = fake_output(grid(120, 1.0, 61.0) + 0.004, 120.0, locked_from=5.0)
        s = score.score_case(out, self.ref)
        self.assertEqual(s["beats"]["on"], s["locked_beats_in_span"])
        self.assertEqual(s["false_share"], 0.0)
        self.assertAlmostEqual(s["bias_ms"], 4.0, places=3)
        self.assertAlmostEqual(s["on_lock_s"], 5.004, places=3)
        self.assertAlmostEqual(s["on_lock_after_first_s"], 4.004, places=3)
        self.assertEqual(s["bpm_relation"], "1")
        self.assertGreater(s["f"], 0.9)
        self.assertEqual(s["false_episodes"], 0)
        self.assertEqual(s["phase_eighths"][0], s["beats"]["on"])

    def test_offbeat(self):
        out = fake_output(grid(120, 1.0, 61.0, phase=0.5), 120.0)
        s = score.score_case(out, self.ref)
        self.assertEqual(s["beats"]["offbeat"], s["locked_beats_in_span"])
        self.assertAlmostEqual(s["f"], 0.0)
        self.assertIsNone(s["on_lock_s"])
        self.assertEqual(s["false_episodes"], 1)
        self.assertEqual(s["phase_eighths"][4], s["beats"]["offbeat"])

    def test_double_and_half_tempo_are_on(self):
        s2 = score.score_case(fake_output(grid(240, 1.0, 61.0), 240.0), self.ref)
        self.assertEqual(s2["beats"]["on"], s2["locked_beats_in_span"])
        self.assertLess(s2["f"], 0.7)
        self.assertGreater(s2["f_oct"], 0.99)
        s_half = score.score_case(fake_output(grid(60, 1.0, 61.0), 60.0), self.ref)
        self.assertEqual(s_half["beats"]["on"], s_half["locked_beats_in_span"])
        self.assertGreater(s_half["f_oct"], 0.99)

    def test_wrong_metre(self):
        s = score.score_case(fake_output(grid(160, 1.0, 61.0), 160.0), self.ref)
        self.assertEqual(s["beats"]["wrong_tempo"], s["locked_beats_in_span"])
        self.assertEqual(list(s["wrong_relations"]), ["4/3"])
        self.assertEqual(s["acc2_time"], 0.0)

    def test_no_ref_outside_the_span(self):
        ref = score.Ref(grid(120, 10.0, 40.0), 50.0)
        out = fake_output(grid(120, 0.0, 50.0), 120.0)
        s = score.score_case(out, ref)
        self.assertGreater(s["beats"]["no_ref"], 30)  # 0-10 s and 40-50 s
        self.assertGreater(s["intro_locked_s"], 8.0)
        self.assertGreater(s["outro_locked_s"], 8.0)

    def test_tempo_change_relock(self):
        beats = np.concatenate([grid(120, 0.0, 30.0), grid(96, 30.0, 60.0)])
        ref = score.Ref(beats, 60.0, changes=[30.0])
        later = grid(96, 30.0, 60.0)
        est = np.concatenate([grid(120, 0.0, 30.0), later[later >= 36.0]])  # 6 s without a lock
        out = fake_output(est, 120.0)
        out["B"][est >= 36.0, 1] = 60.0 / 96.0
        out["B"][est >= 36.0, 2] = 96.0
        s = score.score_case(out, ref)
        self.assertAlmostEqual(s["changes"][0]["relock_s"], 6.25, places=2)
        # a tracker that stays on the beat through a change relocks at once
        steady = score.score_case(fake_output(grid(120, 0.0, 60.0), 120.0),
                                  score.Ref(grid(120, 0.0, 60.0), 60.0, changes=[30.0]))
        self.assertAlmostEqual(steady["changes"][0]["relock_s"], 0.0, places=2)


class DeviceTimeline(unittest.TestCase):
    def test_gapless_trim(self):
        lame = dict(codec="mp3", decode=dict(tag="Info", encoder="LAME3.92", enc_delay=576, xing_frame_samples=1152))
        self.assertEqual(beat_eval.device_offset(lame), 1152 + 576 + 529)
        bare = dict(codec="mp3", decode=dict(tag=None, encoder=None, enc_delay=None, xing_frame_samples=0))
        self.assertEqual(beat_eval.device_offset(bare), 0)
        other = dict(codec="mp3", decode=dict(tag="Xing", encoder="FhG", enc_delay=None, xing_frame_samples=1152))
        self.assertEqual(beat_eval.device_offset(other), 1152)  # the header frame is never audio
        self.assertEqual(beat_eval.device_offset(dict(codec="flac", decode={})), 0)


if __name__ == "__main__":
    unittest.main()
