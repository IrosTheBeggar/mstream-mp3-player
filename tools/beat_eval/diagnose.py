# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Why did the tracker do that on this track? (docs/BEAT-TRACKER-EVAL.md)

  diagnose.py --name RUN 21 61 ...

For each corpus track:
  - the tracker's lock episodes in the run (from its output): when, the tempo and
    its relation to the reference, where its beats fall in the reference's beat;
  - the onset signals folded on the reference's beat grid, in sixteenths of a
    beat (0 = the reference beat, 8 = the off-beat): the tracker's own low and mid
    band onsets (rebuilt from its HopFrontEnd's energies, which the runner dumps
    with --hops-out, with BeatTracker::onHop()'s formula) and a full-band and a
    high-band (> 2 kHz) spectral flux from the PCM. Each row is normalised to a
    mean of 1, so a clear beat shows a tall bin 0 and noise a flat row. If the
    low band peaks away from bin 0 while the full band peaks on it, the reference
    follows the snare/hats and the kick sits elsewhere (or the reference is late);
  - the same folded at half and double the reference tempo is not shown: use the
    run's report for the metre.
Needs the run (beat_eval.py run) and the PCM cache.
"""
import argparse
import json
import math
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import beat_eval  # noqa: E402
import score  # noqa: E402

RATE = 44100
HOP = 512
BINS = 16


def tracker_onsets(low, mid):
    """BeatTracker::onHop()'s onset strength from the hop energies (floats as doubles)."""
    hop_rate = RATE / HOP
    level_alpha = 1.0 - math.exp(-1.0 / (1.5 * hop_rate))
    floor = 64 * (10 ** (-50 / 20)) ** 2
    lv = mv = 0.0
    pe = pm = 0.0
    on_low = np.zeros(len(low))
    on_mid = np.zeros(len(low))
    for k in range(len(low)):
        e, m = float(low[k]), float(mid[k])
        lv += (e - lv) * level_alpha
        mv += (m - mv) * level_alpha

        def rise(x, prev, level):
            a, b = math.log(x + floor), math.log(prev + floor)
            if a <= b:
                return 0.0
            return (a - b) * min(1.0, x / (level + floor))
        lin = (e - pe) / (lv + floor) if e > pe else 0.0
        on_low[k] = rise(e, pe, lv) + 0.05 * lin
        on_mid[k] = 0.5 * rise(m, pm, mv)
        pe, pm = e, m
    return on_low, on_mid


def flux(mono, hop=256, n_fft=1024, lo_hz=0.0):
    """Log-magnitude spectral flux (half-wave rectified), one value per `hop` frames."""
    x = mono.astype(np.float32) / 32768.0
    win = np.hanning(n_fft).astype(np.float32)
    n = 1 + (len(x) - n_fft) // hop
    idx = np.arange(n_fft)[None, :] + hop * np.arange(n)[:, None]
    out = np.zeros(n, dtype=np.float32)
    k0 = int(lo_hz / (RATE / n_fft))
    prev = None
    step = 4096
    for s in range(0, n, step):
        frames = x[idx[s:s + step]] * win
        mag = np.log1p(100.0 * np.abs(np.fft.rfft(frames, axis=1))[:, k0:])
        if prev is not None:
            mag_prev = np.vstack([prev[None, :], mag[:-1]])
        else:
            mag_prev = np.vstack([mag[:1], mag[:-1]])
        out[s:s + step] = np.maximum(mag - mag_prev, 0).sum(axis=1)
        prev = mag[-1]
    times = (np.arange(n) * hop + n_fft / 2) / RATE  # frame centre
    return times, out


def fold(times, values, beats, lo, hi):
    """Onset mass by position in the reference's beat (BINS bins), mean 1."""
    sel = (times >= lo) & (times <= hi)
    t, v = times[sel], values[sel]
    i = np.searchsorted(beats, t) - 1
    ok = (i >= 0) & (i < len(beats) - 1)
    t, v, i = t[ok], v[ok], i[ok]
    ibi = beats[i + 1] - beats[i]
    med = np.median(np.diff(beats))
    keep = ibi < 1.6 * med  # not across gaps
    frac = (t[keep] - beats[i[keep]]) / ibi[keep]
    h = np.zeros(BINS)
    np.add.at(h, np.round(frac * BINS).astype(int) % BINS, v[keep])
    return h / h.mean() if h.mean() > 0 else h


def row(label, h):
    peak = int(np.argmax(h))
    cells = " ".join(f"{x:4.1f}" for x in h)
    return f"  {label:10s} peak {peak:2d}/16  {cells}"


def episodes(out, ref):
    B = out["B"]
    if not len(B):
        return []
    eps = []
    cur = None
    for r in B:
        if r[4] > 0.5:
            if cur is None or r[0] - cur[-1][0] > 2.5 * max(r[1], 0.2):
                cur = []
                eps.append(cur)
            cur.append(r)
        else:
            cur = None
    lines = []
    for e in eps:
        e = np.array(e)
        t0, t1 = e[0, 0], e[-1, 0]
        bpm = float(np.median(e[:, 2]))
        rel = score.relation(bpm, 60.0 / ref.period_at((t0 + t1) / 2)) if ref.segments else "-"
        hist = [0] * 8
        for t in e[:, 0]:
            i = np.searchsorted(ref.beats, t) - 1
            if 0 <= i < len(ref.beats) - 1:
                f = (t - ref.beats[i]) / (ref.beats[i + 1] - ref.beats[i])
                hist[int(round(f * 8)) % 8] += 1
        lines.append(f"  locked {t0:6.1f}-{t1:6.1f} s ({len(e)} beats) {bpm:6.1f} BPM ({rel}), "
                     f"eighths {' '.join(map(str, hist))}, conf {np.median(e[:, 3]):.2f}")
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--corpus")
    ap.add_argument("--work")
    ap.add_argument("--name", required=True, help="the run whose output to read")
    ap.add_argument("tracks", nargs="+", type=int)
    args = ap.parse_args()
    corpus, work = beat_eval.paths(args)
    gt = beat_eval.load_corpus(corpus)
    tracks = {t["index"]: t for t in gt["tracks"]}
    exe = str(work / "bin" / beat_eval.EXE)
    for i in args.tracks:
        t = tracks[i]
        meta = json.loads((work / "pcm" / f"{i:02d}.json").read_text(encoding="utf-8"))
        ref = score.corpus_ref(t, meta)
        pcm = work / "pcm" / f"{i:02d}.s16"
        with tempfile.TemporaryDirectory(dir=work) as tmp:
            hops = Path(tmp) / "hops.f32"
            subprocess.run([exe, "--pcm", beat_eval.short_path(pcm), "--hops-out",
                            beat_eval.short_path(tmp) + os.sep + "hops.f32"], capture_output=True, check=True)
            e = np.fromfile(hops, dtype="<f4").reshape(-1, 2)
        on_low, on_mid = tracker_onsets(e[:, 0], e[:, 1])
        ht = np.arange(len(on_low)) * HOP / RATE
        mono = np.fromfile(pcm, dtype="<i2")
        ft, fl = flux(mono)
        _, fh = flux(mono, lo_hz=2000.0)
        lo, hi = ref.first, ref.last
        print(f"[{i}] {score.title_of(t)} ({t['sync_test_case']}, ref {t['tempo_bpm']} BPM, {t['beat_source']}, "
              f"clarity {t['clarity']}, beats {lo:.1f}-{hi:.1f} s of {ref.duration:.1f} s)")
        print(f"  reference notes: {t.get('notes', '')[:400]}")
        print("  folded on the reference's beat (sixteenths; 0 = beat, 8 = off-beat):")
        print(row("low (trk)", fold(ht, on_low, ref.beats, lo, hi)))
        print(row("mid (trk)", fold(ht, on_mid, ref.beats, lo, hi)))
        print(row("full flux", fold(ft, fl, ref.beats, lo, hi)))
        print(row(">2k flux", fold(ft, fh, ref.beats, lo, hi)))
        # how periodic is the tracker's own onset at the reference tempo, vs 4:3 / 3:2 / 2:3
        on = on_low + on_mid
        on = on - np.convolve(on, np.ones(259) / 259, mode="same")
        span = (ht >= lo) & (ht <= hi)
        x = on[span]
        lag0 = float(np.dot(x, x)) or 1.0
        hop_rate = RATE / HOP
        acs = []
        for name, k in (("ref", 1.0), ("x2", 2.0), ("x1/2", 0.5), ("x3/2", 1.5), ("x2/3", 2 / 3), ("x4/3", 4 / 3),
                        ("x3/4", 0.75)):
            bpm = t["tempo_bpm"] * k
            lag = 60.0 * hop_rate / bpm
            if lag < 2 or lag * 4 >= len(x):
                continue
            vals = []
            for m in (1, 2, 4):
                L = lag * m
                l0 = int(L)
                f = L - l0
                a = (1 - f) * np.dot(x[:-l0 - 1], x[l0:-1]) + f * np.dot(x[:-l0 - 1], x[l0 + 1:])
                vals.append(a / lag0)
            acs.append(f"{name} {bpm:.0f}: {np.mean(vals):+.2f}")
        print("  tracker-onset autocorrelation at lag, 2 and 4 lags (span): " + ", ".join(acs))
        run_out = work / "runs" / args.name / "corpus" / f"{i:02d}.txt"
        if run_out.exists():
            out = score.parse_output(run_out)
            T = out["T"]
            print(f"  run '{args.name}': lock {out['end'].get('lock_s')} s; estimate median "
                  f"{np.median(T[T[:, 2] > 0, 2]) if (T[:, 2] > 0).any() else 0:.1f}, clarity median "
                  f"{np.median(T[:, 3]):.3f} p90 {np.percentile(T[:, 3], 90):.3f}, conf p90 {np.percentile(T[:, 4], 90):.2f}")
            for line in episodes(out, ref)[:12]:
                print(line)
        print()


if __name__ == "__main__":
    main()
