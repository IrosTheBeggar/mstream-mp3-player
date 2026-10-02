# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Scores a beat_eval run against the corpus's reference beats (docs/BEAT-TRACKER-EVAL.md).

The reference (beats.json) is librosa's, not a human's: tempo folded into
[72, 150) BPM by convention, and some phases and metres are doubtful. So every
tempo metric has an octave-tolerant twin, the tracker's beats are matched at its
own metrical level, and tracks whose reference looks wrong are flagged rather
than trusted.

Terms (per locked tracker beat inside the reference's span):
  on          within +-70 ms of a reference beat, at the reference tempo or an
              octave of it (at double tempo the half-beats count as beats)
  offbeat     at the reference tempo (or half) but on the half-beat (+-70 ms)
  off         at an octave-correct tempo but neither
  wrong_tempo the tracked tempo isn't the reference's or an octave of it
              (3:2, 4:3, ... are named in the report)
  no_ref      locked where the reference has no beat (intros, outros, gaps)
"""
import json
import math
from pathlib import Path

import numpy as np

TOL = 0.070          # beat F-measure / "on" tolerance, s
TEMPO_TOL = 0.04     # tempo accuracy, relative
RATIOS = [(1.0, "1"), (2.0, "2"), (0.5, "1/2"), (1.5, "3/2"), (2 / 3, "2/3"), (4 / 3, "4/3"), (0.75, "3/4"),
          (3.0, "3"), (1 / 3, "1/3")]
OCTAVE_OK = ("1", "2", "1/2")
BEAT_CLASSES = ("good", "ok")
RATE = 44100
IDLE_WEIGHT = 0.5    # dancerate::kIdleWeight: a dance weight above it is drawn dancing
DRUM_TOL = 0.05      # drum cases: the grid's median offset from the kick (or the lead-in), in beats


def monitored(index):
    """The monitored split: every third track (26 of 77), so each album contributes to both halves.
    Thresholds are chosen on the other two thirds, but these tracks' numbers are in every report
    and were seen while tuning: a check on fitting, not an independent test (that needs tracks
    nobody has looked at)."""
    return index % 3 == 2


# ---- parsing ----

def parse_output(path):
    meta, end = {}, {}
    T, B, R = [], [], []
    with open(path, encoding="utf-8") as f:
        for line in f:
            p = line.split()
            if not p:
                continue
            if p[0] == "T":
                T.append([float(x) for x in p[1:]])
            elif p[0] == "B":
                B.append([float(x) for x in p[1:]])
            elif p[0] == "R":
                R.append(float(p[1]))
            elif p[0] in ("meta", "end"):
                d = meta if p[0] == "meta" else end
                for kv in p[1:]:
                    k, v = kv.split("=", 1)
                    d[k] = float(v)
    # T: t bpm est clarity conf locked valid [dominance hits pulse steady [weight]]; B: t period bpm conf locked index
    return dict(meta=meta, end=end, T=np.array(T).reshape(len(T), -1) if T else np.zeros((0, 7)),
                B=np.array(B).reshape(-1, 6), R=np.array(R))


# ---- reference helpers ----

class Ref:
    """Reference beats on the run's timeline, with what the scorer derives from them."""

    def __init__(self, beats, duration, start=0.0, weak=(), info=None, changes=()):
        self.beats = np.asarray(sorted(beats), dtype=float)
        self.duration = duration
        self.start = start
        self.weak = list(weak)
        self.info = info or {}
        self.changes = list(changes)  # tempo changes / joins, s
        b = self.beats
        if len(b) >= 2:
            ibi = np.diff(b)
            # local period: running median of 9 intervals
            k = 4
            med = np.array([np.median(ibi[max(0, i - k):i + k + 1]) for i in range(len(ibi))])
            self.ibi_t = (b[:-1] + b[1:]) / 2
            self.ibi_med = med
            gap = ibi > 1.6 * med
            self.gap_after = gap
            # segments of continuous beats
            segs, s0 = [], 0
            for i, g in enumerate(gap):
                if g:
                    segs.append((s0, i))
                    s0 = i + 1
            segs.append((s0, len(b) - 1))
            self.segments = [(b[i] - 0.5 * self.period_at(b[i]), b[j] + 0.5 * self.period_at(b[j])) for i, j in segs]
            # half-beats, including the half-beat before and after each segment
            ends = [b[i] - 0.5 * self.period_at(b[i]) for i, _ in segs] + \
                   [b[j] + 0.5 * self.period_at(b[j]) for _, j in segs]
            self.mids = np.sort(np.concatenate([((b[:-1] + b[1:]) / 2)[~gap], ends]))
            self.doubled = np.sort(np.concatenate([b, self.mids]))
        else:
            self.ibi_t = np.array([0.0])
            self.ibi_med = np.array([0.5])
            self.mids = np.array([])
            self.doubled = b
            self.segments = []

    def period_at(self, t):
        i = int(np.clip(np.searchsorted(self.ibi_t, t), 0, len(self.ibi_t) - 1))
        if i > 0 and abs(self.ibi_t[i - 1] - t) < abs(self.ibi_t[i] - t):
            i -= 1
        return float(self.ibi_med[i])

    def beat_fraction(self, t):
        """Where `t` falls in the reference's beat: 0 on a beat, 0.5 on the half-beat."""
        b = self.beats
        i = int(np.searchsorted(b, t)) - 1
        if 0 <= i < len(b) - 1 and b[i + 1] - b[i] < 1.6 * self.period_at(t):
            return float((t - b[i]) / (b[i + 1] - b[i])) % 1.0
        p = self.period_at(t)
        anchor = b[0] if i < 0 else b[min(i, len(b) - 1)]
        return float((t - anchor) / p) % 1.0

    def in_span(self, t):
        return any(a <= t <= b for a, b in self.segments)

    @property
    def first(self):
        return float(self.beats[0]) if len(self.beats) else None

    @property
    def last(self):
        return float(self.beats[-1]) if len(self.beats) else None


def nearest(arr, t):
    if len(arr) == 0:
        return math.inf
    i = np.searchsorted(arr, t)
    best = math.inf
    for j in (i - 1, i):
        if 0 <= j < len(arr) and abs(arr[j] - t) < abs(best):
            best = t - arr[j]
    return best


def relation(bpm, ref_bpm):
    if bpm <= 0 or ref_bpm <= 0:
        return "none"
    r = bpm / ref_bpm
    for k, name in RATIOS:
        if abs(r / k - 1.0) < TEMPO_TOL:
            return name
    return "other"


def matches(est, ref, tol=TOL):
    i = j = m = 0
    while i < len(est) and j < len(ref):
        d = est[i] - ref[j]
        if abs(d) <= tol:
            m += 1
            i += 1
            j += 1
        elif d < 0:
            i += 1
        else:
            j += 1
    return m


def fmeasure(est, ref):
    if len(est) == 0 and len(ref) == 0:
        return None
    m = matches(est, ref)
    return 2.0 * m / (len(est) + len(ref))


def pct(values, q):
    return float(np.percentile(values, q)) if len(values) else None


def med(values):
    return float(np.median(values)) if len(values) else None


# ---- one case ----

def score_case(out, ref, expect_beat=True):
    T, B = out["T"], out["B"]
    beats = ref.beats
    s = {}
    s["lock_s"] = out["end"].get("lock_s", -1.0)
    s["lock_s"] = None if s["lock_s"] < 0 else s["lock_s"]
    s["ns_per_audio_s"] = out["end"].get("ns_per_audio_s")
    s["locked_share_all"] = out["end"].get("locked_share")
    locked = B[B[:, 4] > 0.5] if len(B) else B
    classes, rels, errs = [], [], []
    for row in locked:
        t, _, bpm = row[0], row[1], row[2]
        if not ref.in_span(t):
            classes.append("no_ref")
            rels.append(None)
            errs.append(None)
            continue
        rel = relation(bpm, 60.0 / ref.period_at(t))
        rels.append(rel)
        if rel in ("1", "1/2"):
            e = nearest(beats, t)
            if abs(e) <= TOL:
                c = "on"
            elif abs(nearest(ref.mids, t)) <= TOL:
                c = "offbeat"
            else:
                c = "off"
            errs.append(e)
        elif rel == "2":
            e = nearest(ref.doubled, t)
            c = "on" if abs(e) <= TOL else "off"
            errs.append(e)
        else:
            c = "wrong_tempo"
            errs.append(None)
        classes.append(c)
    counts = {c: classes.count(c) for c in ("on", "offbeat", "off", "wrong_tempo", "no_ref")}
    s["beats"] = counts
    in_span = sum(v for k, v in counts.items() if k != "no_ref")
    s["locked_beats_in_span"] = in_span
    s["false_share"] = (in_span - counts["on"]) / in_span if in_span else None
    wrong_rels = [r for r, c in zip(rels, classes) if c == "wrong_tempo"]
    s["wrong_relations"] = {r: wrong_rels.count(r) for r in sorted(set(wrong_rels))}
    e_ok = np.array([e for e in errs if e is not None])
    s["phase_abs_ms"] = [round(abs(x) * 1000, 2) for x in e_ok]  # pooled later
    s["phase_med_ms"] = med(np.abs(e_ok)) * 1000 if len(e_ok) else None
    s["phase_p95_ms"] = pct(np.abs(e_ok), 95) * 1000 if len(e_ok) else None
    on_e = e_ok[np.abs(e_ok) <= TOL] if len(e_ok) else e_ok
    s["bias_ms"] = float(np.mean(on_e)) * 1000 if len(on_e) else None
    s["on_errors_ms"] = [round(x * 1000, 2) for x in on_e]

    # where the octave-correct locked beats fall in the reference's beat, in eighths
    # (0: on the beat, 4: the off-beat, 2/6: a sixteenth-note lock)
    hist = [0] * 8
    for row, rel in zip(locked, rels):
        if rel in OCTAVE_OK:
            hist[int(round(ref.beat_fraction(row[0]) * 8)) % 8] += 1
    s["phase_eighths"] = hist
    if len(T):
        est = T[T[:, 2] > 0, 2]
        s["est_bpm_median"] = med(est)
        s["clarity_med"] = med(T[:, 3])
        s["conf_p90"] = pct(T[:, 4], 90)

    # beat F-measure over the reference's span
    if len(beats):
        est = np.array([row[0] for row in locked if ref.in_span(row[0])])
        s["f"] = fmeasure(est, beats)
        variants = [beats, ref.doubled, beats[::2], beats[1::2]]
        s["f_oct"] = max(fmeasure(est, v) or 0.0 for v in variants)
        m = matches(est, beats)
        s["precision"] = m / len(est) if len(est) else None
        s["recall"] = m / len(beats)
    else:
        s["f"] = s["f_oct"] = s["precision"] = s["recall"] = None

    # first lock on the beat: a locked beat starting 4 'on' in a row
    times = locked[:, 0] if len(locked) else np.array([])
    s["on_lock_s"] = None
    on_runs = []  # beats that start 4 'on' beats in a row (no gap among them)
    for i in range(len(classes) - 3):
        if all(c == "on" for c in classes[i:i + 4]) and \
                times[i + 3] - times[i] < 3.5 * max(locked[i, 1], 0.2):
            on_runs.append(float(times[i]))
    if on_runs:
        s["on_lock_s"] = on_runs[0]
    first = ref.first
    if first is not None:
        first_in = first if first >= ref.start else float(beats[np.searchsorted(beats, ref.start)]) \
            if np.searchsorted(beats, ref.start) < len(beats) else None
        s["first_ref_s"] = first_in
        s["lock_after_first_s"] = (ref.start + s["lock_s"] - first_in) if (s["lock_s"] is not None and first_in is not None) else None
        s["on_lock_after_first_s"] = (s["on_lock_s"] - first_in) if (s["on_lock_s"] is not None and first_in is not None) else None

    # time-based tempo accuracy over the span (timeline samples)
    dt = float(np.median(np.diff(T[:, 0]))) if len(T) > 1 else 0.0
    if len(T) and len(beats):
        span = np.array([ref.in_span(t) for t in T[:, 0]])
        lk = T[:, 5] > 0.5
        sel = span & lk
        rel_t = [relation(T[i, 1], 60.0 / ref.period_at(T[i, 0])) for i in np.where(sel)[0]]
        s["locked_share_span"] = float(np.mean(lk[span])) if span.any() else None
        # what the dancer shows: the runner's dance weight (column 11), dancing above IDLE_WEIGHT
        if T.shape[1] >= 12:
            s["dance_share_span"] = float(np.mean(T[span, 11] > IDLE_WEIGHT)) if span.any() else None
        s["acc1_time"] = rel_t.count("1") / len(rel_t) if rel_t else None
        s["acc2_time"] = sum(r in OCTAVE_OK for r in rel_t) / len(rel_t) if rel_t else None
        s["rel_time"] = {r: round(rel_t.count(r) * dt, 2) for r in sorted(set(rel_t))}
        bpms = T[sel, 1]
        s["bpm_median"] = med(bpms)
        mid_t = float(np.median(T[sel, 0])) if sel.any() else None
        s["bpm_relation"] = relation(s["bpm_median"], 60.0 / ref.period_at(mid_t)) if bpms.size else "none"
        s["ref_bpm_median"] = 60.0 / float(np.median(np.diff(beats))) if len(beats) > 1 else None
        # intro / outro
        s["intro_locked_s"] = float(np.sum(lk & (T[:, 0] < first - 0.5)) * dt) if first - ref.start > 2.0 else None
        last = ref.last
        p_last = ref.period_at(last)
        end_t = T[-1, 0]
        s["outro_s"] = end_t - last
        s["outro_locked_s"] = float(np.sum(lk & (T[:, 0] > last + p_last)) * dt) if end_t - last > 2.0 else None
        # breaks: weak-pulse regions inside the span, >= 4 s
        rec, inbreak, breaklen = [], 0.0, 0.0
        for a, b in ref.weak:
            if b - a < 4.0 or a < first or b > last - 4.0:
                continue
            w = (T[:, 0] >= a) & (T[:, 0] <= b)
            inbreak += float(np.sum(lk & w) * dt)
            breaklen += b - a
            after = [t for t, c in zip(times, classes) if t >= b and c == "on"]
            rec.append((after[0] - b) if after else None)
        s["breaks"] = len(rec)
        s["break_recovery_s"] = rec
        s["break_locked_share"] = inbreak / breaklen if breaklen else None
        # tempo changes / joins: from the change to 4 'on' beats in a row after it
        ch = []
        for c in ref.changes:
            runs = [t for t in on_runs if t >= c]
            first_after = beats[np.searchsorted(beats, c)] if np.searchsorted(beats, c) < len(beats) else c
            wrong = sum(1 for t, cl in zip(times, classes) if c <= t <= c + 10.0 and cl != "on")
            ch.append(dict(at=c, relock_s=(runs[0] - first_after) if runs else None, false_beats_10s=wrong))
        s["changes"] = ch
    # false-lock episodes: runs of locked beats; false if under half of them are on
    episodes, false_eps, false_secs = 0, 0, 0.0
    i = 0
    while i < len(classes):
        j = i
        while j + 1 < len(classes) and times[j + 1] - times[j] < 2.5 * max(locked[j, 1], 0.2):
            j += 1
        seg = classes[i:j + 1]
        episodes += 1
        if seg.count("on") < 0.5 * len(seg):
            false_eps += 1
            false_secs += float(times[j] - times[i] + locked[j, 1])
        i = j + 1
    s["episodes"] = episodes
    s["false_episodes"] = false_eps
    s["false_episode_s"] = false_secs
    s["expect_beat"] = expect_beat
    return s


def drum_lock(out, phase):
    """Where a drum case's grid sat while locked: the median offset of its locked beats from the nearest
    kick (the truth), in beats; 'kick' within DRUM_TOL of 0, 'lead-in' within DRUM_TOL of -(1 - phase)
    (the notes at `phase` before the next kick), else 'elsewhere'."""
    B, R = out["B"], out["R"]
    lock = out["end"].get("lock_s", -1.0)
    locked = B[B[:, 4] > 0.5] if len(B) else B
    x = dict(lock_s=None if lock < 0 else lock, locked_beats=int(len(locked)), median_frac=None, where="never")
    if len(locked) == 0 or len(R) < 2:
        return x
    period = float(np.median(np.diff(R)))
    frac = float(np.median([nearest(R, t) / period for t in locked[:, 0]]))
    x["median_frac"] = frac
    if abs(frac) <= DRUM_TOL:
        x["where"] = "kick"
    elif phase > 0.5 and abs(frac + (1.0 - phase)) <= DRUM_TOL:
        x["where"] = "lead-in"
    else:
        x["where"] = "elsewhere"
    return x


# ---- the corpus reference ----

def corpus_ref(t, meta, start=0.0, seconds=None):
    shift = meta["offset_frames"] / RATE
    beats = np.array(t["beat_times_s"]) - shift
    duration = meta["frames"] / RATE
    end = duration if seconds is None else min(duration, start + seconds)
    beats = beats[(beats >= start) & (beats <= end)]
    weak = [(a - shift, b - shift) for a, b in t.get("weak_pulse_regions_s", [])]
    return Ref(beats, end, start=start, weak=weak)


def ref_flags(t, meta):
    """Reasons to doubt the reference itself, from the reference and the file alone (never from the
    tracker's output: the clean-reference subset must be the same tracks for every run compared)."""
    f = []
    if t["sync_test_case"] in ("hard", "no-clear-beat"):
        f.append(t["sync_test_case"])
    if t["clarity"] < 0.35:
        f.append(f"low clarity {t['clarity']:.2f}")
    cd = t.get("clarity_detail", {})
    if cd.get("phase_ambiguous"):
        f.append("beat/off-beat ambiguous in the reference")
    qa = t.get("timing_qa", {})
    if qa.get("halfbeat_ratio", 2) < 1.0:
        f.append(f"more onset energy on the half-beat ({qa['halfbeat_ratio']:.2f})")
    hr = qa.get("high_rise_ms")
    if hr is not None and (hr > 10 or hr < -12):
        f.append(f"attacks {hr:+.0f} ms from the beats")
    if t["beat_source"] == "tracker":
        f.append("no constant grid (librosa's tracker beats)")
    rng = t.get("tempo_stability", {}).get("local_tempo_range_pct")
    if rng is not None and rng > 5:
        f.append(f"local tempo spread {rng:.0f} %")
    # metre ambiguity: a non-octave tempo family scoring close to the chosen one
    cands = sorted(t.get("tempo_candidates", []), key=lambda c: -c["score"])
    if len(cands) >= 2:
        best = cands[0]
        for c in cands[1:]:
            if relation(c["tempo"], best["tempo"]) not in OCTAVE_OK and c["score"] >= 0.85 * best["score"]:
                f.append(f"metre: {c['tempo']:.0f} scored {c['score'] / best['score']:.2f} of {best['tempo']:.0f}")
                break
    v1 = t.get("tempo_v1_plain_librosa")
    if v1 and relation(v1, t["tempo_bpm"]) not in OCTAVE_OK:
        f.append(f"plain librosa said {v1:.0f}")
    tag = meta.get("tag_bpm")
    if tag and relation(tag, t["tempo_bpm"]) not in OCTAVE_OK:
        f.append(f"tag BPM {tag:g} disagrees")
    return f


def tracker_flags(s):
    """Where the tracker disagrees with the reference confidently: worth a listen, but it depends on the
    run, so it is reported next to the reference's flags and never used to pick tracks."""
    f = []
    if s.get("locked_beats_in_span", 0) >= 32:
        n = s["locked_beats_in_span"]
        if s["beats"]["wrong_tempo"] > 0.5 * n:
            rel = max(s["wrong_relations"], key=s["wrong_relations"].get)
            f.append(f"tracker locked at {rel} of the reference for most beats")
        if s["beats"]["offbeat"] > 0.5 * n:
            f.append("tracker locked on the reference's off-beat for most beats")
    return f


# ---- aggregation ----

def summarize(rows, key="corpus"):
    """Headline numbers over a list of per-track scores."""
    out = dict(n=len(rows))
    if not rows:
        return out
    def mean(k):
        v = [r[k] for r in rows if r.get(k) is not None]
        return float(np.mean(v)) if v else None
    out["f"] = mean("f")
    out["f_oct"] = mean("f_oct")
    out["precision"] = mean("precision")
    out["recall"] = mean("recall")
    rel = [r.get("bpm_relation", "none") for r in rows]
    out["tracks_acc1"] = sum(x == "1" for x in rel) / len(rows)
    out["tracks_acc2"] = sum(x in OCTAVE_OK for x in rel) / len(rows)
    out["tracks_never_locked"] = sum(r["lock_s"] is None for r in rows)
    out["tracks_never_on"] = sum(r.get("on_lock_s") is None for r in rows)
    out["locked_share_span"] = mean("locked_share_span")
    out["dance_share_span"] = mean("dance_share_span")
    lt = [r["on_lock_after_first_s"] for r in rows if r.get("on_lock_after_first_s") is not None]
    out["on_lock_after_first_med_s"] = med(lt)
    out["on_lock_within_5s"] = sum(1 for x in lt if x <= 5.0) / len(rows)
    out["on_lock_within_10s"] = sum(1 for x in lt if x <= 10.0) / len(rows)
    ls = [r["lock_s"] for r in rows if r.get("lock_s") is not None]
    out["lock_med_s"] = med(ls)
    la = [r["lock_after_first_s"] for r in rows if r.get("lock_after_first_s") is not None]
    out["lock_after_first_med_s"] = med(la)
    beats = {c: sum(r["beats"][c] for r in rows) for c in ("on", "offbeat", "off", "wrong_tempo", "no_ref")}
    out["beats"] = beats
    in_span = sum(v for k, v in beats.items() if k != "no_ref")
    out["false_share"] = (in_span - beats["on"]) / in_span if in_span else None
    out["false_episodes"] = sum(r["false_episodes"] for r in rows)
    out["false_episode_s"] = sum(r["false_episode_s"] for r in rows)
    pa = np.concatenate([np.array(r["phase_abs_ms"]) for r in rows]) if rows else np.array([])
    out["phase_med_ms"] = med(pa)
    out["phase_p95_ms"] = pct(pa, 95)
    oe = np.concatenate([np.array(r["on_errors_ms"]) for r in rows])
    out["bias_ms"] = float(np.mean(oe)) if len(oe) else None
    out["on_abs_med_ms"] = med(np.abs(oe))
    acc1 = [r["acc1_time"] for r in rows if r.get("acc1_time") is not None]
    acc2 = [r["acc2_time"] for r in rows if r.get("acc2_time") is not None]
    out["acc1_time"] = float(np.mean(acc1)) if acc1 else None
    out["acc2_time"] = float(np.mean(acc2)) if acc2 else None
    il = [r["intro_locked_s"] for r in rows if r.get("intro_locked_s") is not None]
    out["intro_tracks"] = len(il)
    out["intro_false_lock_tracks"] = sum(1 for x in il if x > 1.0)
    out["intro_locked_s"] = float(np.sum(il)) if il else 0.0
    ol = [r["outro_locked_s"] for r in rows if r.get("outro_locked_s") is not None]
    out["outro_tracks"] = len(ol)
    out["outro_hold_med_s"] = med(ol)
    rec = [x for r in rows for x in r.get("break_recovery_s", [])]
    out["breaks"] = len(rec)
    out["break_recovery_med_s"] = med([x for x in rec if x is not None])
    out["break_never_recovered"] = sum(1 for x in rec if x is None)
    ns = [r["ns_per_audio_s"] for r in rows if r.get("ns_per_audio_s")]
    out["ns_per_audio_s_med"] = med(ns)
    return out


def fmt(x, nd=2, unit=""):
    if x is None:
        return "-"
    if isinstance(x, float):
        return f"{x:.{nd}f}{unit}"
    return f"{x}{unit}"


def pc(x):
    return "-" if x is None else f"{100 * x:.0f} %"


def summary_table(groups):
    """groups: list of (label, summary)."""
    cols = [("tracks", lambda s: str(s["n"])),
            ("F (±70 ms)", lambda s: fmt(s.get("f"))),
            ("F oct.", lambda s: fmt(s.get("f_oct"))),
            ("tempo exact", lambda s: pc(s.get("tracks_acc1"))),
            ("tempo oct.", lambda s: pc(s.get("tracks_acc2"))),
            ("locked (span)", lambda s: pc(s.get("locked_share_span"))),
            ("dancing (span)", lambda s: pc(s.get("dance_share_span"))),
            ("lock, med", lambda s: fmt(s.get("lock_med_s"), 1, " s")),
            ("on-beat lock after 1st beat, med", lambda s: fmt(s.get("on_lock_after_first_med_s"), 1, " s")),
            ("on-beat ≤10 s", lambda s: pc(s.get("on_lock_within_10s"))),
            ("never on", lambda s: str(s.get("tracks_never_on"))),
            ("phase med / p95", lambda s: f"{fmt(s.get('phase_med_ms'), 1)} / {fmt(s.get('phase_p95_ms'), 0)} ms"),
            ("bias", lambda s: fmt(s.get("bias_ms"), 1, " ms")),
            ("false beats", lambda s: pc(s.get("false_share"))),
            ("false episodes", lambda s: str(s.get("false_episodes")))]
    lines = ["| | " + " | ".join(c for c, _ in cols) + " |", "|---" * (len(cols) + 1) + "|"]
    for label, s in groups:
        lines.append(f"| {label} | " + " | ".join(f(s) if s.get("n") else "-" for _, f in cols) + " |")
    return "\n".join(lines)


def title_of(t):
    name = t["device_path"].rsplit("/", 1)[-1]
    name = name.rsplit(".", 1)[0]
    return name.replace("Aphex Twin - ", "")


# ---- the run ----

def score_run(corpus, work, name, quiet=False):
    import beat_eval
    gt = beat_eval.load_corpus(corpus)
    run_dir = work / "runs" / name
    info = json.loads((run_dir / "run.json").read_text(encoding="utf-8"))
    tracks = {t["index"]: t for t in gt["tracks"]}
    metas = {i: json.loads((work / "pcm" / f"{i:02d}.json").read_text(encoding="utf-8")) for i in tracks}
    result = dict(run=info, corpus={}, mid={}, joins={}, clicks={})

    # corpus
    for i, t in tracks.items():
        p = run_dir / "corpus" / f"{i:02d}.txt"
        if not p.exists():
            continue
        ref = corpus_ref(t, metas[i])
        s = score_case(parse_output(p), ref)
        s.update(index=i, title=title_of(t), album=metas[i]["album"], genre=metas[i]["genre"],
                 cls=t["sync_test_case"], ref_bpm=t["tempo_bpm"], clarity=t["clarity"],
                 first_beat_s=ref.first, duration_s=ref.duration, tag_bpm=metas[i].get("tag_bpm"))
        s["ref_flags"] = ref_flags(t, metas[i])
        s["tracker_flags"] = tracker_flags(s)
        result["corpus"][f"{i:02d}"] = s
    # mid-track starts
    for i, t in tracks.items():
        p = run_dir / "mid" / f"{i:02d}.txt"
        if not p.exists():
            continue
        out = parse_output(p)
        start = out["meta"]["start_frame"] / RATE
        ref = corpus_ref(t, metas[i], start=start, seconds=out["meta"]["frames"] / RATE)
        s = score_case(out, ref)
        s.update(index=i, cls=t["sync_test_case"], title=title_of(t))
        result["mid"][f"{i:02d}"] = s
    # joins
    jdir = run_dir / "joins"
    for p in sorted(jdir.glob("*.txt")) if jdir.exists() else []:
        a, b = (int(x) for x in p.stem.split("-"))
        jm = json.loads((work / "joins" / f"{p.stem}.json").read_text(encoding="utf-8"))
        ta, tb = tracks[a], tracks[b]
        a0 = jm["a_start_frame"] / RATE
        js = jm["join_frame"] / RATE
        ra = np.array(ta["beat_times_s"]) - metas[a]["offset_frames"] / RATE - a0
        ra = ra[(ra >= 0) & (ra < js)]
        rb = np.array(tb["beat_times_s"]) - metas[b]["offset_frames"] / RATE + js
        rb = rb[(rb >= js) & (rb <= jm["frames"] / RATE)]
        ref = Ref(np.concatenate([ra, rb]), jm["frames"] / RATE, changes=[js])
        out = parse_output(p)
        s = score_case(out, ref)
        ch = s["changes"][0] if s.get("changes") else {}
        lb = out["B"][(out["B"][:, 4] > 0.5) & (out["B"][:, 0] >= js)] if len(out["B"]) else out["B"]
        s["f_next"] = fmeasure(np.array([r[0] for r in lb if ref.in_span(r[0])]), rb) if len(rb) else None
        s.update(a=a, b=b, cls_a=ta["sync_test_case"], cls_b=tb["sync_test_case"], join_s=js,
                 relock_s=ch.get("relock_s"), false_beats_10s=ch.get("false_beats_10s"),
                 tempo_a=ta["tempo_bpm"], tempo_b=tb["tempo_bpm"])
        result["joins"][p.stem] = s
    # clicks, and the drum patterns
    expect = {c[0]: c[3] for c in beat_eval.CLICK_CASES + beat_eval.DRUM_CASES}
    result["drums"] = {}
    cdir = run_dir / "clicks"
    for p in sorted(cdir.glob("*.txt")) if cdir.exists() else []:
        out = parse_output(p)
        R = out["R"]
        if p.stem.startswith("drums_"):
            result["drums"][p.stem] = drum_lock(out, float(p.stem.split("_")[2]))
            continue
        changes = []
        if len(R) > 2:
            # a tempo change, or the beat coming back after a gap
            ibi = np.diff(R)
            typical = float(np.median(ibi))
            for k in range(1, len(ibi)):
                if ibi[k - 1] > 1.6 * typical:
                    changes.append(float(R[k]))
                elif ibi[k] < 1.6 * typical and abs(ibi[k] / ibi[k - 1] - 1) > 0.03:
                    changes.append(float(R[k]))
        dur = out["meta"]["frames"] / RATE
        s = score_case(out, Ref(R, dur, changes=changes), expect_beat=expect.get(p.stem, True))
        s["true_bpm"] = 60.0 / float(np.median(np.diff(R))) if len(R) > 1 else None
        if s.get("bpm_median") and s["true_bpm"] and not changes:
            s["bpm_err_pct"] = 100.0 * (s["bpm_median"] / s["true_bpm"] - 1.0)
        result["clicks"][p.stem] = s

    corpus_rows = list(result["corpus"].values())
    beat_rows = [r for r in corpus_rows if r["cls"] in BEAT_CLASSES]
    result["summary"] = {
        "beat tracks (good+ok)": summarize(beat_rows),
        "good": summarize([r for r in corpus_rows if r["cls"] == "good"]),
        "ok": summarize([r for r in corpus_rows if r["cls"] == "ok"]),
        "hard": summarize([r for r in corpus_rows if r["cls"] == "hard"]),
        "no-clear-beat": summarize([r for r in corpus_rows if r["cls"] == "no-clear-beat"]),
        "all 77": summarize(corpus_rows),
        "beat tracks, clean reference": summarize([r for r in beat_rows if not r["ref_flags"]]),
        "beat tracks, tuning split": summarize([r for r in beat_rows if not monitored(r["index"])]),
        "beat tracks, monitored split": summarize([r for r in beat_rows if monitored(r["index"])]),
        "mid-track start (60 s in), beat tracks": summarize([r for r in result["mid"].values() if r["cls"] in BEAT_CLASSES]),
        "mid-track start, monitored split": summarize([r for r in result["mid"].values()
                                                if r["cls"] in BEAT_CLASSES and monitored(r["index"])]),
        "gapless joins": summarize(list(result["joins"].values())),
    }
    d = list(result["drums"].values())
    result["drum_summary"] = {w: sum(x["where"] == w for x in d) for w in ("kick", "lead-in", "elsewhere", "never")}
    result["by_album"] = {a: summarize([r for r in beat_rows if r["album"] == a])
                          for a in sorted({r["album"] for r in corpus_rows})}
    result["by_genre"] = {g: summarize([r for r in beat_rows if r["genre"] == g])
                          for g in sorted({r["genre"] for r in corpus_rows})}
    ns = [r["ns_per_audio_s"] for r in corpus_rows if r.get("ns_per_audio_s")]
    result["cpu"] = dict(info.get("cpu", {}), corpus_parallel_ns_per_audio_s_median=med(ns))
    (run_dir / "score.json").write_text(json.dumps(result, indent=1, default=float), encoding="utf-8")
    report = make_report(result)
    (run_dir / "report.md").write_text(report, encoding="utf-8")
    if not quiet:
        print(report)
    return result


def worst(result, n=15):
    rows = [r for r in result["corpus"].values() if r["cls"] != "no-clear-beat"]
    return sorted(rows, key=lambda r: ((r["f"] or 0.0), (r["f_oct"] or 0.0), r.get("locked_share_span") or 0.0))[:n]


def make_report(result):
    L = []
    info = result["run"]
    L.append(f"# Beat tracker evaluation: {info['name']}\n")
    L.append(f"commit {info.get('commit')}{' (dirty)' if info.get('dirty') else ''}, tracker sources "
             f"{info.get('sources')}, prior {info.get('prior')}, via_hops {info.get('via_hops')}, {info.get('date')}\n")
    L.append("## Summary\n")
    L.append(summary_table(list(result["summary"].items())))
    L.append("")
    s = result["summary"]["beat tracks (good+ok)"]
    L.append(f"Beat classes (good+ok, locked beats): {s['beats']}; wrong-tempo relations pooled: "
             f"{pool_rel(result)}")
    L.append(f"Intro (first beat > 2 s in): {s['intro_false_lock_tracks']} of {s['intro_tracks']} tracks locked "
             f"> 1 s before the first beat ({fmt(s['intro_locked_s'], 1)} s in all). Outro (> 2 s after the last beat): "
             f"median hold {fmt(s['outro_hold_med_s'], 1)} s over {s['outro_tracks']} tracks. Breaks (weak-pulse regions "
             f"≥ 4 s): {s['breaks']}, median recovery {fmt(s['break_recovery_med_s'], 1)} s, "
             f"{s['break_never_recovered']} never recovered.")
    cpu = result["cpu"]
    per = ", ".join(f"{k} {v / 1000:.0f}" for k, v in cpu.items()
                    if k not in ("median_ns_per_audio_s", "corpus_parallel_ns_per_audio_s_median"))
    L.append(f"CPU (host, serial, best of 5): {fmt((cpu.get('median_ns_per_audio_s') or 0) / 1000, 0)} µs per "
             f"second of audio, median ({per} µs/s).\n")
    L.append("## Click tracks and synthetic cases\n")
    L.append("| case | expect beat | lock | BPM (err %) | F | phase med / p95 | bias | false beats | after change: relock / false beats in 10 s |")
    L.append("|---|---|---|---|---|---|---|---|---|")
    for k, c in result["clicks"].items():
        chs = "; ".join(f"{fmt(x['relock_s'], 1, ' s')} / {x['false_beats_10s']}" for x in c.get("changes", [])) or "-"
        L.append(f"| {k} | {'yes' if c['expect_beat'] else 'no'} | {fmt(c['lock_s'], 2, ' s')} | "
                 f"{fmt(c.get('bpm_median'), 2)} ({fmt(c.get('bpm_err_pct'), 2)}) | {fmt(c['f'])} | "
                 f"{fmt(c['phase_med_ms'], 1)} / {fmt(c['phase_p95_ms'], 1)} ms | {fmt(c['bias_ms'], 1)} | "
                 f"{c['locked_beats_in_span'] - c['beats']['on']} + {c['beats']['no_ref']} outside | {chs} |")
    L.append("")
    if result.get("drums"):
        ds = result["drum_summary"]
        L.append("## Drum patterns (the host tests' heavy off-beat cases)\n")
        L.append(f"Kick on every beat; bass and ghost kick 4-6 dB under it at PHASE of the beat (0.66 and 0.75: a "
                 f"lead-in to the next kick), a hat on top. Where the locked grid sits (median over its locked beats, "
                 f"±{DRUM_TOL} beat): on the kick {ds['kick']}, on the lead-in {ds['lead-in']}, elsewhere "
                 f"{ds['elsewhere']}, never locked {ds['never']} (of {len(result['drums'])}).\n")
        L.append("| case | lock | locked beats | median offset from the kick (beats) | where |")
        L.append("|---|---|---|---|---|")
        for k, x in result["drums"].items():
            L.append(f"| {k} | {fmt(x['lock_s'], 2, ' s')} | {x['locked_beats']} | {fmt(x['median_frac'], 3)} | "
                     f"{x['where']} |")
        L.append("")
    L.append("## Gapless joins (last 30 s of a track, first 60 s of the next, no reset)\n")
    L.append("| join | tempi | F | F, next track only | relock after the next track's first beat | "
             "false beats in the 10 s after |")
    L.append("|---|---|---|---|---|---|")
    for k, j in result["joins"].items():
        fb = j["false_beats_10s"]
        L.append(f"| {k} | {j['tempo_a']:.1f} → {j['tempo_b']:.1f} | {fmt(j['f'])} | {fmt(j.get('f_next'))} | "
                 f"{fmt(j['relock_s'], 1, ' s')} | {'-' if fb is None else fb} |")
    L.append("")
    L.append("## By album (good+ok tracks)\n")
    L.append(summary_table(list(result["by_album"].items())))
    L.append("")
    L.append("## By genre (good+ok tracks)\n")
    L.append(summary_table(list(result["by_genre"].items())))
    L.append("")
    L.append("## Per track\n")
    L.append("| # | track | class | ref BPM | tracked BPM (rel.) | lock | on-beat lock | F | F oct. | phase med / p95 | "
             "false | reference flags |")
    L.append("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for k, r in sorted(result["corpus"].items()):
        L.append(f"| {r['index']} | {r['title'][:34]} | {r['cls']} | {r['ref_bpm']:.1f} | "
                 f"{fmt(r.get('bpm_median'), 1)} ({r.get('bpm_relation', '-')}) | {fmt(r['lock_s'], 1, ' s')} | "
                 f"{fmt(r.get('on_lock_s'), 1, ' s')} | {fmt(r['f'])} | {fmt(r['f_oct'])} | "
                 f"{fmt(r['phase_med_ms'], 0)} / {fmt(r['phase_p95_ms'], 0)} | {pc(r['false_share'])} | "
                 f"{'; '.join(r['ref_flags'] + r.get('tracker_flags', []))} |")
    L.append("")
    L.append("## Worst 15 (by F; no-clear-beat tracks left out)\n")
    L.append("| # | track | class | F | F oct. | ref / tracked BPM | estimate (rel.), clarity, conf p90 | "
             "beats on / offbeat / off / wrong tempo / no ref | in the ref beat (eighths 0-7) | first beat | "
             "on-beat lock | reference flags |")
    L.append("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r in worst(result):
        b = r["beats"]
        est = r.get("est_bpm_median")
        L.append(f"| {r['index']} | {r['title'][:34]} | {r['cls']} | {fmt(r['f'])} | {fmt(r['f_oct'])} | "
                 f"{r['ref_bpm']:.1f} / {fmt(r.get('bpm_median'), 1)} ({r.get('bpm_relation')}) | "
                 f"{fmt(est, 1)} ({relation(est or 0, r['ref_bpm'])}), {fmt(r.get('clarity_med'))}, "
                 f"{fmt(r.get('conf_p90'))} | "
                 f"{b['on']} / {b['offbeat']} / {b['off']} / {b['wrong_tempo']} / {b['no_ref']} | "
                 f"{' '.join(str(x) for x in r.get('phase_eighths', []))} | "
                 f"{fmt(r['first_beat_s'], 1, ' s')} | {fmt(r.get('on_lock_s'), 1, ' s')} | "
                 f"{'; '.join(r['ref_flags'] + r.get('tracker_flags', []))} |")
    L.append("")
    return "\n".join(L)


def pool_rel(result):
    pooled = {}
    for r in result["corpus"].values():
        if r["cls"] in BEAT_CLASSES:
            for k, v in r["wrong_relations"].items():
                pooled[k] = pooled.get(k, 0) + v
    return pooled


def compare(work, a, b):
    A = json.loads((work / "runs" / a / "score.json").read_text(encoding="utf-8"))
    B = json.loads((work / "runs" / b / "score.json").read_text(encoding="utf-8"))
    keys = ["f", "f_oct", "tracks_acc1", "tracks_acc2", "locked_share_span", "dance_share_span", "lock_med_s",
            "on_lock_after_first_med_s", "on_lock_within_10s", "tracks_never_on", "phase_med_ms", "phase_p95_ms",
            "bias_ms", "false_share", "false_episodes", "false_episode_s"]
    for group in A["summary"]:
        sa, sb = A["summary"][group], B["summary"].get(group, {})
        print(f"\n{group}  ({a} vs {b})")
        for k in keys:
            va, vb = sa.get(k), sb.get(k)
            d = (va - vb) if isinstance(va, (int, float)) and isinstance(vb, (int, float)) else None
            print(f"  {k:28s} {fmt(va, 3):>10s} {fmt(vb, 3):>10s} {fmt(d, 3):>10s}")
    print("\nclicks (lock s, phase med ms, F):")
    for k in A["clicks"]:
        ca, cb = A["clicks"][k], B["clicks"].get(k, {})
        print(f"  {k:24s} {fmt(ca.get('lock_s'))}/{fmt(cb.get('lock_s'))}  {fmt(ca.get('phase_med_ms'), 1)}/"
              f"{fmt(cb.get('phase_med_ms'), 1)}  {fmt(ca.get('f'))}/{fmt(cb.get('f'))}")
    if A.get("drum_summary") or B.get("drum_summary"):
        print(f"\ndrum patterns (kick / lead-in / elsewhere / never): {A.get('drum_summary')} vs {B.get('drum_summary')}")
    for suite, label in (("corpus", "whole tracks"), ("mid", "mid-track starts")):
        rows = [(k, ra, B[suite].get(k)) for k, ra in A[suite].items()
                if ra["cls"] in BEAT_CLASSES and B[suite].get(k)]
        # paired: each beat track's on-beat lock time in both runs (never: counted as worse / better)
        da = [(ra.get("on_lock_after_first_s"), rb.get("on_lock_after_first_s")) for _, ra, rb in rows]
        both = [a - b for a, b in da if a is not None and b is not None]
        print(f"\n{label}, on-beat lock after the first beat, paired over {len(rows)} beat tracks ({a} minus {b}): "
              f"median {fmt(med(both), 1, ' s')} over the {len(both)} on in both; earlier in {sum(x < -0.5 for x in both)}, "
              f"later in {sum(x > 0.5 for x in both)}; on only in {a}: {sum(1 for x, y in da if x is not None and y is None)}, "
              f"only in {b}: {sum(1 for x, y in da if x is None and y is not None)}")
        # every regression and gain over 0.1 in F, with where the locked beats went
        moved = sorted(((ra["f"] or 0) - (rb["f"] or 0), k, ra, rb) for k, ra, rb in rows)
        moved = [m for m in moved if abs(m[0]) > 0.1]
        print(f"{label}, F changed by more than 0.1: {sum(m[0] < 0 for m in moved)} down, {sum(m[0] > 0 for m in moved)} up "
              f"(on / off-beat / off / wrong tempo, {a} vs {b})")
        for delta, k, ra, rb in moved:
            ba, bb = ra["beats"], rb["beats"]
            print(f"  {k} {ra['title'][:30]:30s} {fmt(ra['f'])} vs {fmt(rb['f'])} ({delta:+.2f})  "
                  f"{ba['on']}/{ba['offbeat']}/{ba['off']}/{ba['wrong_tempo']} vs "
                  f"{bb['on']}/{bb['offbeat']}/{bb['off']}/{bb['wrong_tempo']}")
    ca, cb = A["cpu"], B["cpu"]
    print(f"\ncpu ns per audio s: {ca.get('median_ns_per_audio_s')} vs {cb.get('median_ns_per_audio_s')}")
