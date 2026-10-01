#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Generates the sample-rate converter's filter tables
(lib/core/ResamplerTables.cpp; the declarations are in ResamplerTables.h).

    python tools/gen_resampler_tables.py            # regenerate
    python tools/gen_resampler_tables.py --check    # exit 1 if the checked-in file differs

The design is docs/RESAMPLER.md, section 3. Four tables, all Q15 int16:

  D147   147/160 (48 -> 44.1 kHz): 147 rows of K = 48 taps; rows 0..73 stored.
  U12    the upsampling prototype, 12 rows of 48; rows 0..6 stored. Every
         2nd/3rd/4th/6th row is the x6/x4/x3/x2 filter: 8/12/16/24/32 kHz go
         up to 48 kHz with it (then D147), 11.025/22.05 kHz straight to 44.1.
  HB96   halfband, 71 taps (96 -> 48 kHz): one side's 18 nonzero taps.
  HB88   halfband, 123 taps (88.2 -> 44.1 kHz): one side's 31 nonzero taps.

A row's element j multiplies the history's x[j], oldest first (x[47] is the
newest), and row L-p is row p reversed, so only rows 0..L/2 are stored. Every
row sums to exactly 32768 (unity DC gain). The rounding residue goes onto the
largest taps, then pairs of +1/-1 moves that keep the sum are kept while they
lower the error's energy in the band that matters (the passband for the
polyphase rows, the stopband for the halfbands), which gains ~4 dB of THD+N.
The generator refuses a table whose half-row sums of |c| could overflow the
kernel's int32 accumulators (each half < 65536).

The host tests (test/test_rate_converter) check the committed tables by
their properties, not their bytes: the optimiser compares floating-point
energies, so a last-bit difference in another platform's libm can flip a tap.
--check is for a local run on one machine. Standard library only.
"""
import argparse
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "lib" / "core" / "ResamplerTables.cpp"

K = 48  # taps per polyphase row (RateConverter.cpp's kernel is written for 48)


def bessel_i0(x):
    s = t = 1.0
    k = 1
    while True:
        h = x / (2.0 * k)
        t *= h * h
        s += t
        if t < 1e-18 * s:
            return s
        k += 1


def kaiser_beta(a):
    if a > 50:
        return 0.1102 * (a - 8.7)
    if a > 21:
        return 0.5842 * (a - 21) ** 0.4 + 0.07886 * (a - 21)
    return 0.0


def kaiser_w(r, beta):
    if abs(r) >= 1.0:
        return 0.0
    return bessel_i0(beta * math.sqrt(1.0 - r * r)) / bessel_i0(beta)


def sinc(x):
    return 1.0 if abs(x) < 1e-12 else math.sin(math.pi * x) / (math.pi * x)


def kaiser_a(fs, fpass, fstop, taps):
    """Kaiser's attenuation estimate for `taps` taps over this transition."""
    return min(100.0, 2.285 * 2 * math.pi * (fstop - fpass) / fs * taps + 8)


def residue_onto_largest(q, g, total):
    """Adds the rounding residue, one LSB at a time, to the largest taps."""
    order = sorted(range(len(q)), key=lambda k: -abs(g[k]))
    diff = total - sum(q)
    i = 0
    while diff != 0:
        s = 1 if diff > 0 else -1
        q[order[i % len(order)]] += s
        diff -= s
        i += 1


def optimise(q, ideal, r):
    """Pairwise +1/-1 moves (the sum kept) while e'Re falls; e = q - ideal."""
    n = len(q)
    e = [q[k] - ideal[k] for k in range(n)]
    re = [sum(r[a][b] * e[b] for b in range(n)) for a in range(n)]
    for _ in range(100):
        improved = False
        for a in range(n):
            for b in range(n):
                if a == b:
                    continue
                d = 2 * (re[a] - re[b]) + r[a][a] + r[b][b] - 2 * r[a][b]
                if d < -1e-9 and q[a] < 32767 and q[b] > -32768:
                    q[a] += 1
                    q[b] -= 1
                    e[a] += 1
                    e[b] -= 1
                    for k in range(n):
                        re[k] += r[k][a] - r[k][b]
                    improved = True
        if not improved:
            return


def poly_rows(L, fs, fpass, fstop):
    """Rows 0..L/2 of an L-row polyphase table, each K taps in history order."""
    a = kaiser_a(fs, fpass, fstop, K)
    beta = kaiser_beta(a)
    nyq = (fpass + fstop) / fs  # 2 fc / fs, fc halfway across the transition
    band = 2.0 * fpass / fs     # the passband as a fraction of the source rate
    r = [[band * sinc(band * (x - y)) for y in range(K)] for x in range(K)]
    rows = []
    for p in range(L // 2 + 1):
        g = []
        for j in range(K):
            t = K / 2 - 1 - j + p / L  # source samples from the output instant
            g.append(0.0 if abs(t) >= K / 2 else nyq * sinc(nyq * t) * kaiser_w(t / (K / 2), beta))
        s = sum(g)
        ideal = [v / s * 32768.0 for v in g]
        q = [int(math.floor(v + 0.5)) for v in ideal]
        residue_onto_largest(q, g, 32768)
        optimise(q, ideal, r)
        assert sum(q) == 32768
        assert all(-32768 <= v <= 32767 for v in q)
        rows.append(q)
    return rows, a, beta


def halfband_side(taps, fs, fpass, fstop):
    """One side's nonzero taps (distances c, c-2, ..., 1 from the centre), summing to 8192."""
    c = (taps - 1) // 2
    assert c % 2 == 1, "the outermost tap must be at an odd distance"
    a = kaiser_a(fs, fpass, fstop, taps)
    beta = kaiser_beta(a)
    dists = list(range(c, 0, -2))
    g = [0.5 * sinc(0.5 * d) * kaiser_w(d / (c + 1), beta) for d in dists]
    s = sum(g)
    ideal = [v / s * 8192.0 for v in g]
    q = [int(math.floor(v + 0.5)) for v in ideal]
    residue_onto_largest(q, g, 8192)
    # Error energy over the stopband (the passband's error mirrors it):
    # E(f) = 2 sum e_d cos(2 pi f d), integrated over [fstop/fs, 1/2].
    f1 = fstop / fs

    def integral(delta):
        if delta == 0:
            return 0.5 - f1
        return (math.sin(math.pi * delta) - math.sin(2 * math.pi * f1 * delta)) / (2 * math.pi * delta)

    r = [[0.5 * (integral(x - y) + integral(x + y)) for y in dists] for x in dists]
    optimise(q, ideal, r)
    assert sum(q) == 8192
    return q, a, beta


def check_halves(name, rows):
    worst = 0
    for q in rows:
        for half in (q[: K // 2], q[K // 2:]):
            worst = max(worst, sum(abs(v) for v in half))
    if worst >= 65536:
        sys.exit(f"{name}: a half-row's sum of |c| is {worst}: the kernel's int32 sums could overflow")
    return worst


def fmt_rows(rows):
    out = []
    for q in rows:
        lines = []
        for i in range(0, len(q), 12):
            lines.append("     " + ", ".join(f"{v:6d}" for v in q[i:i + 12]))
        out.append("    {\n" + ",\n".join(lines) + "},")
    return "\n".join(out)


def fmt_list(q):
    lines = []
    for i in range(0, len(q), 12):
        lines.append("    " + ", ".join(f"{v:6d}" for v in q[i:i + 12]))
    return ",\n".join(lines)


def generate():
    d147, d_a, d_beta = poly_rows(147, 48000.0, 20000.0, 25950.0)
    # Normalised to the source rate: passband 0.875 of its Nyquist, stopband
    # from 1 - 0.4375 - 0.005, the cutoff at 0.995 of Nyquist.
    u12, u_a, u_beta = poly_rows(12, 1.0, 0.4375, 0.5575)
    hb96, h96_a, h96_beta = halfband_side(71, 96000.0, 20000.0, 28000.0)
    hb88, h88_a, h88_beta = halfband_side(123, 88200.0, 20000.0, 24100.0)
    d_half = check_halves("D147", d147)
    u_half = check_halves("U12", u12)

    return f"""// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar
// Generated by tools/gen_resampler_tables.py. Do not edit: change the
// generator and run `python tools/gen_resampler_tables.py`.

#include "ResamplerTables.h"

namespace resampler {{

// D147: 48 kHz -> 44.1 kHz. Kaiser-windowed sinc, passband 20 kHz, stopband
// from 25.95 kHz, A {d_a:.1f} dB (beta {d_beta:.3f}). Worst half-row sum of |c|: {d_half}.
alignas(4) const int16_t kD147[kD147Stored][kTaps] = {{
{fmt_rows(d147)}
}};

// U12: the upsampling prototype, normalised to the source rate: passband
// 0.875 of its Nyquist, stopband from 0.5575 of its rate, A {u_a:.1f} dB
// (beta {u_beta:.3f}). Worst half-row sum of |c|: {u_half}.
alignas(4) const int16_t kU12[kU12Stored][kTaps] = {{
{fmt_rows(u12)}
}};

// HB96: 96 kHz -> 48 kHz, 71 taps: passband 20 kHz, stopband from 28 kHz,
// A {h96_a:.1f} dB (beta {h96_beta:.3f}). Distances 35, 33, ..., 1 from the centre.
const int16_t kHb96Side[kHb96SideTaps] = {{
{fmt_list(hb96)}
}};

// HB88: 88.2 kHz -> 44.1 kHz, 123 taps: passband 20 kHz, stopband from
// 24.1 kHz, A {h88_a:.1f} dB (beta {h88_beta:.3f}). Distances 61, 59, ..., 1 from the centre.
const int16_t kHb88Side[kHb88SideTaps] = {{
{fmt_list(hb88)}
}};

}}  // namespace resampler
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="fail if the generated file is out of date")
    args = ap.parse_args()
    text = generate()
    if args.check:
        old = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if old != text:
            sys.exit(f"{OUT.relative_to(ROOT)} is stale: run python tools/gen_resampler_tables.py")
        print("up to date")
        return
    OUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {OUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
