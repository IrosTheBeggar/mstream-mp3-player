#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""The Ogg Opus reader checked on real files with the real decoder (docs/OPUS.md).

  opus_check.py build [--work DIR] [--cxx g++]
      compiles tools/opus_check/runner.cpp with lib/core's reader and ESP8266Audio's
      bundled libopus (.pio/libdeps/core2/ESP8266Audio/src/libopus: the firmware's
      decoder, FIXED_POINT, built here for the host) into DIR/bin/runner.
  opus_check.py run [--work DIR] [--ffmpeg PATH] FILE...
      for each .opus file: the runner decodes it as the firmware's generator will
      (per-frame split, the pre-skip and the EOS trim), ffmpeg decodes it too, and
      the two are compared: the trimmed lengths must be equal, the tail scan's length
      must equal what came out, the per-frame split must be bit-identical to a
      whole-packet decode, and the output must be close to ffmpeg's (its decoder is
      float, ours fixed point: a few LSB; the SNR is printed). A chained file's first
      link is compared with its tail scan only (ffmpeg plays every link). Exit 1 on
      any failure.
  opus_check.py seek [--work DIR] [--seeks N] [--seed S] [--seeds K] [--prerolls MS,MS,...] FILE...
      for each file: N random starts planned with the seek preroll (the reader's
      kSeekPrerollMs, 200 ms) and N with the resume preroll (600 ms), each decoded
      for the 4,096 samples after its target and compared with the decode from the
      top (the research's convergence measure; docs/OPUS.md section 10 measured
      80-240 ms this way on 54 files, 500 starts each: 200 ms keeps the 128k files
      at 35 dB or better at their worst start but for a few quiet windows that score
      the same at any preroll, 160 ms takes them to 30-33 dB; SILK and hybrid sit at
      35-45 dB, never converging bit for bit; 600 ms is bit-exact but for 1 LSB on
      CELT). --seeds K runs K seeds from --seed on and reports the worst, the maxima
      and the means over all of them: one seed's 50 starts miss a file's worst start
      by 5-10 dB (section 10.7), so a figure to decide anything by is --seeds 10.
      --prerolls measures other prerolls instead (e.g. 80,120,160,200; a plan at 600
      is a resume's, held to the 4 LSB rule).
      Every start must land on its sample, the worst SNR must reach 35 dB, and on a
      CELT-only file the resumes must be within 4 LSB of the decode from the top
      (how many were bit-exact is printed). The probes and reads a plan cost are
      printed too, with the device's time they model (3 ms a read and 0.65 ms a
      KB, fitted to M3's device figures: section 10). Exit 1 on any failure.
  opus_check.py corrupt [--work DIR] FILE BYTE
      flips one byte and reports the resync, the gap filled and the samples lost.
  opus_check.py plan [--work DIR] [--ffmpeg PATH] [--sample S] [--preroll MS] [--window N] FILE MS
      one start at MS (a seek: the tail rule, the seek preroll), or with --sample at
      a trimmed sample as a resume anchor's (the anchor checked, the 600 ms preroll),
      planned and decoded as the firmware does it (the runner's plan command,
      docs/OPUS.md section 9). It prints the plan (the page reading starts at and its
      granule, where decoding and keeping begin, the probes and reads: what the
      device's `[audio] Opus: starting ... in` line must name for the same file and
      second, the plan being a function of the file alone), where the start landed,
      and the window (4,096 samples; 0: to the end) against the decode from the top
      in-process and against ffmpeg's decode of the whole file at the same offset
      (the max |difference| and SNR). Exit 1 unless it landed on its sample and the
      SNR against ffmpeg reaches 35 dB.

--work (or OPUS_CHECK_WORK) defaults to <temp>/opus_check, and goes before or after
the subcommand (`opus_check.py --work DIR plan ...` as docs/OPUS.md 9.6 writes it, or
as above). --ffmpeg (or OPUS_CHECK_FFMPEG) defaults to the repo's .venv-tools
imageio-ffmpeg binary, else ffmpeg on PATH. A directory given to `run` or `seek` is
every .opus in it.
"""
import argparse
import glob
import math
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
LIBOPUS = REPO / ".pio/libdeps/core2/ESP8266Audio/src/libopus"
EXE = "runner.exe" if os.name == "nt" else "runner"


def work_dir(args):
    w = args.work or os.environ.get("OPUS_CHECK_WORK") or os.path.join(tempfile.gettempdir(), "opus_check")
    return Path(w)


def find_ffmpeg(args):
    if args.ffmpeg:
        return args.ffmpeg
    env = os.environ.get("OPUS_CHECK_FFMPEG")
    if env:
        return env
    for p in glob.glob(str(REPO / ".venv-tools/Lib/site-packages/imageio_ffmpeg/binaries/ffmpeg*")):
        return p
    return "ffmpeg"


# ---- build ----

def cmd_build(args):
    if not LIBOPUS.is_dir():
        sys.exit(f"no bundled libopus at {LIBOPUS}: run `pio run -e core2` once to fetch ESP8266Audio")
    work = work_dir(args)
    obj = work / "obj"
    obj.mkdir(parents=True, exist_ok=True)
    (work / "bin").mkdir(parents=True, exist_ok=True)
    cxx = args.cxx or os.environ.get("CXX", "g++")
    cc = cxx.replace("g++", "gcc") if "g++" in cxx else "gcc"
    inc = ["-I", str(LIBOPUS / "include"), "-I", str(LIBOPUS / "celt"), "-I", str(LIBOPUS / "silk"),
           "-I", str(LIBOPUS / "silk/fixed"), "-I", str(LIBOPUS / "src")]
    # Every libopus source, as the firmware compiles them (its config.h: FIXED_POINT,
    # VAR_ARRAYS, no float API), -O2 here; unchanged objects are kept.
    objects = []
    for src in sorted(LIBOPUS.rglob("*.c")):
        rel = src.relative_to(LIBOPUS)
        o = obj / (str(rel).replace("/", "_").replace("\\", "_") + ".o")
        objects.append(str(o))
        if o.exists() and o.stat().st_mtime >= src.stat().st_mtime:
            continue
        cmd = [cc, "-O2", "-w", "-c", str(src), "-o", str(o)] + inc
        subprocess.run(cmd, check=True)
    out = work / "bin" / EXE
    cmd = [cxx, "-std=gnu++17", "-O2", "-Wall", "-Wextra", "-I", str(REPO / "lib/core"), "-I", str(LIBOPUS / "include"),
           str(HERE / "runner.cpp"), str(REPO / "lib/core/OggPage.cpp"), str(REPO / "lib/core/OggOpus.cpp")] + objects + [
        "-o", str(out)]
    if os.name == "nt":
        cmd.insert(1, "-static")  # MSYS2's libstdc++ DLLs aren't on every PATH
    print(" ".join(cmd[:12]) + " ...")
    subprocess.run(cmd, check=True)
    print(f"built {out}")


# ---- run ----

def runner_path(args):
    out = work_dir(args) / "bin" / EXE
    if not out.exists():
        sys.exit(f"{out} missing: opus_check.py build first")
    return out


def parse_kv(text):
    d = {}
    for line in text.splitlines():
        for tok in line.split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                d[k] = v
    return d


def ffmpeg_decode(ffmpeg, path, channels, out):
    """ffmpeg's libopus decoder (the reference: the same decoder, bit for bit on every
    file tried), else its native one (float, and its own SILK resampler: 12 dB SNR on
    SILK files, 55-70 dB on CELT)."""
    for codec in (["-c:a", "libopus"], []):
        cmd = [ffmpeg, "-v", "error", "-y"] + codec + ["-i", str(path), "-f", "s16le", "-acodec", "pcm_s16le",
                                                       "-ar", "48000", "-ac", str(channels), str(out)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode == 0:
            return True, "" if codec else "ffmpeg native decoder"
        why = r.stderr.strip()
    return False, why


def compare(ours, ref, channels, ref_from=0, limit=0):
    """Samples (frames) in each, the max |difference| and the SNR over the common part
    (ours against ref from frame `ref_from` on; at most `limit` frames when given), and
    the frames the comparison covered (the runner writes whole frames, so `ours` can
    hold more than `limit`: the count printed must be this one)."""
    import array
    a = array.array("h")
    b = array.array("h")
    with open(ours, "rb") as f:
        a.frombytes(f.read())
    with open(ref, "rb") as f:
        b.frombytes(f.read())
    at = ref_from * channels
    n = min(len(a), max(0, len(b) - at))
    if limit:
        n = min(n, limit * channels)
    maxd = 0
    err = 0.0
    sig = 0.0
    for i in range(n):
        d = a[i] - b[at + i]
        if d < 0:
            d = -d
        if d > maxd:
            maxd = d
        err += d * d
        sig += b[at + i] * b[at + i]
    snr = 999.0 if err == 0 else (10 * math.log10(sig / err) if sig > 0 else 0.0)
    return len(a) // channels, len(b) // channels, maxd, snr, n // channels


def expand(paths):
    files = []
    for p in paths:
        if os.path.isdir(p):
            files += sorted(glob.glob(os.path.join(p, "*.opus")))
            files += sorted(glob.glob(os.path.join(p, "*.ogg")))
            files += sorted(glob.glob(os.path.join(p, "*.ogv")))
        else:
            files.append(p)
    return files


def cmd_run(args):
    runner = runner_path(args)
    ffmpeg = find_ffmpeg(args)
    work = work_dir(args)
    raw = work / "raw"
    raw.mkdir(parents=True, exist_ok=True)
    files = expand(args.files)
    if not files:
        sys.exit("no files")
    failures = 0
    print(f"{'file':40} {'open':12} {'ch':>2} {'skip':>5} {'len':>9} {'ffmpeg':>9} {'tail':>9} {'split':>5} "
          f"{'maxd':>4} {'snr':>6} {'frames':>7} {'maxfr':>5} {'bad':>3} notes")
    for path in files:
        name = Path(path).name[:40]
        ours = raw / (Path(path).stem + ".ours.raw")
        ref = raw / (Path(path).stem + ".ff.raw")
        r = subprocess.run([str(runner), "decode", path, str(ours)], capture_output=True, text=True)
        if r.returncode != 0:
            print(f"{name:40} runner failed: {r.stderr.strip()[-200:]}")
            failures += 1
            continue
        kv = parse_kv(r.stdout)
        if kv.get("open") != "ok":
            print(f"{name:40} {kv.get('open', '?'):12} refused: {r.stdout.splitlines()[1].split('=', 1)[1]}")
            continue
        ch = int(kv["channels"])
        ok, why = ffmpeg_decode(ffmpeg, path, ch, ref)
        notes = []
        if not ok:
            notes.append(f"ffmpeg: {why[-80:]}")
            ours_n = int(kv["outSamples"])
            ref_n = -1
            maxd = -1
            snr = 0.0
        else:
            if why:
                notes.append(why)
            ours_n, ref_n, maxd, snr, _ = compare(ours, ref, ch)
            chained = kv.get("chained") == "1"
            malformed = int(kv.get("malformed", "0")) + int(kv.get("dropped", "0"))
            if ours_n != ref_n and not chained:
                if malformed and ref_n < ours_n:
                    # ffmpeg stops at its first undecodable packet; the player
                    # drops it and plays on (the fuzz file).
                    notes.append(f"ffmpeg stopped at {ref_n} (its first undecodable packet)")
                else:
                    notes.append(f"LENGTH {ours_n} vs ffmpeg {ref_n}")
                    failures += 1
            if chained:
                # ffmpeg plays every link; the player plays the first, whose length
                # the tail scan found by bisection: it must match what came out.
                notes.append(f"chained: the first link ({ours_n}), ffmpeg's decode covers every link ({ref_n})")
            if maxd > 64 and snr < 40:
                notes.append("check: far from ffmpeg's decode")
        tail = int(kv["lengthSamples"]) if kv.get("tail") == "1" else -1
        if tail != ours_n and kv.get("end") == "eos" and int(kv.get("badPages", "0")) == 0:
            notes.append(f"TAIL {tail}")
            failures += 1
        if kv.get("splitExact") != "1":
            notes.append(f"SPLIT {kv.get('splitMismatchedSamples')} samples in {kv.get('splitMismatchedPackets')} packets")
            failures += 1
        if kv.get("finished") != "1":
            notes.append(f"end: {kv.get('end')}")
        if int(kv.get("malformed", "0")) or int(kv.get("dropped", "0")) or int(kv.get("concealed", "0")):
            notes.append(f"malformed {kv.get('malformed')} (concealed) dropped {kv.get('dropped')} frames concealed "
                         f"{kv.get('concealed')}")
        if int(kv.get("filled", "0")):
            notes.append(f"filled {kv.get('filled')} in {kv.get('gaps')} gaps")
        if int(kv.get("tagsBytes", "0")) > 65536:
            notes.append(f"tags {int(kv['tagsBytes']) // 1024} KB in {kv['tagsPages']} pages, open {kv['openBytes']} B")
        print(f"{name:40} {'ok':12} {ch:>2} {kv['preSkip']:>5} {ours_n:>9} {ref_n:>9} {tail:>9} "
              f"{'yes' if kv.get('splitExact') == '1' else 'NO':>5} {maxd:>4} {snr:>6.1f} {kv['frames']:>7} "
              f"{kv['maxFrameSamples']:>5} {kv['badPages']:>3} {'; '.join(notes)}")
    print(f"{len(files)} files, {failures} failures")
    return 1 if failures else 0


def cmd_corrupt(args):
    runner = runner_path(args)
    r = subprocess.run([str(runner), "decode", args.file, "--corrupt", str(args.byte)], capture_output=True, text=True)
    print(r.stdout, end="")
    kv = parse_kv(r.stdout)
    r0 = subprocess.run([str(runner), "decode", args.file], capture_output=True, text=True)
    kv0 = parse_kv(r0.stdout)
    if kv.get("open") == "ok" and kv0.get("open") == "ok":
        lost = int(kv0["keptSamples"]) - int(kv["keptSamples"])
        filled = int(kv.get("filled", "0"))
        print(f"lost {lost} samples ({lost / 48:.0f} ms) of the length, {filled} filled ({filled / 48:.0f} ms) in "
              f"{kv.get('gaps')} gaps ({kv.get('gapsSized')} sized); resyncs {kv['resyncs']} over {kv['resyncBytes']} "
              f"bytes, the most one next() call read {kv.get('maxCallBytes', '?')} B (a slice and a header, or a "
              f"chunk of the scan: docs/OPUS.md 8.11); corrections {kv['corrections']} slip {kv['slip']}")
    return 0


# ---- plan ----

def cmd_plan(args):
    runner = runner_path(args)
    ffmpeg = find_ffmpeg(args)
    work = work_dir(args)
    raw = work / "raw"
    raw.mkdir(parents=True, exist_ok=True)
    path = args.file
    ours = raw / (Path(path).stem + f".plan{args.ms}.raw")
    ref = raw / (Path(path).stem + ".ff.raw")
    cmd = [str(runner), "plan", path, str(args.ms), "--window", str(args.window)]
    if args.sample is not None:
        cmd += ["--sample", str(args.sample)]
    if args.preroll is not None:
        cmd += ["--preroll", str(args.preroll)]
    cmd.append(str(ours))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"runner failed: {r.stderr.strip()[-200:]}")
        return 1
    kv = parse_kv(r.stdout)
    if kv.get("open") != "ok":
        print(f"{kv.get('open', '?')} refused: {r.stdout.splitlines()[1].split('=', 1)[1]}")
        return 1
    for line in r.stdout.splitlines():
        if line.startswith(("plan:", "landed=", "window=", "anchorCheck=")):
            print(line)
    failures = 0
    target = int(kv["target"])
    if int(kv["landed"]) != target:
        print(f"LANDED {kv['landed']}, not the target {target}")
        failures += 1
    if "anchorCheck" in kv and kv["anchorCheck"] != "ok":
        # As the firmware: "the resume anchor isn't this file's (...): by its
        # second", the ms planned instead (the tail rule: 0 is the top).
        print(f"the anchor refused ({kv['anchorCheck']}: the kind, the size, the length, or in its last 5 s): "
              f"planned by the ms instead, as the firmware does")
    ch = int(kv["channels"])
    in_process = (f"against the in-process decode from the top: max |diff| {kv['maxDiff']}, SNR {kv['snr']} dB"
                  f"{', bit-exact' if kv.get('bitExact') == '1' else ''}")
    if int(kv.get("refFilled", "0")) > 0:
        # A damaged file: the firmware (and the runner's reference) fill the
        # gap so the timeline stays exact; ffmpeg drops the lost samples and
        # its timeline runs short from there, so the same offset isn't the
        # same sample. The in-process decode is the reference then.
        print(f"a damaged file ({kv['refBadPages']} bad pages, {kv['refFilled']} samples filled in {kv['refGaps']} "
              f"gaps before or inside the window): ffmpeg's timeline runs short by what it drops, so no comparison "
              f"with it; {in_process}")
        if float(kv["snr"]) < 35.0:
            print(f"SNR {float(kv['snr']):.1f} dB under 35 against the decode from the top")
            failures += 1
        return 1 if failures else 0
    ok, why = ffmpeg_decode(ffmpeg, path, ch, ref)
    if not ok:
        print(f"ffmpeg: {why[-120:]}")
        return 1
    # The runner wrote whole frames (4,488 for the 4,096 window on a 20 ms
    # file), so the count printed is what compare() covered, not what it read.
    _, _, maxd, snr, compared = compare(ours, ref, ch, target, int(kv["compared"]))
    print(f"against ffmpeg's decode from the top, from its sample {target}: {compared} samples "
          f"compared, max |diff| {maxd}, SNR {snr:.1f} dB{' (' + why + ')' if why else ''}; {in_process}")
    if snr < 35.0:
        print(f"SNR {snr:.1f} dB under 35 against ffmpeg")
        failures += 1
    return 1 if failures else 0


# ---- seek ----

# The device's time a plan's reads take, modelled: ~3 ms a read (the seek
# and the first sector) and ~0.65 ms a KB (the card's ~1.5-2 MB/s through
# the LCD's shared SPI bus), fitted to M3's device figures (docs/OPUS.md
# section 10: 21 reads / 63 KB in 127 ms, 73 / 318 in 383, 6 / 12 in 30).
MS_PER_READ = 3.0
MS_PER_KB = 0.65


def model_ms(reads, kb):
    return reads * MS_PER_READ + kb * MS_PER_KB


def merge_seek(a, b):
    """Two seek runs' lines as one: the counts summed, the worsts and the
    maxima the worse, the means weighted by their starts."""
    if a is None:
        return dict(b)
    na, nb = int(a["seeks"]), int(b["seeks"])
    n = na + nb
    out = dict(a)
    for k in ("seeks", "landed", "bitExact"):
        out[k] = str(int(a[k]) + int(b[k]))
    out["maxDiffWorst"] = str(max(int(a["maxDiffWorst"]), int(b["maxDiffWorst"])))
    out["snrWorst"] = str(min(float(a["snrWorst"]), float(b["snrWorst"])))
    for k in ("probesMax", "readsMax", "bytesMax", "decodedBeforeMax"):
        out[k] = str(max(int(a[k]), int(b[k])))
    if n:
        for k in ("snrMean", "probesMean", "readsMean"):
            out[k] = str((float(a[k]) * na + float(b[k]) * nb) / n)
        for k in ("bytesMean", "decodedBeforeMean"):
            out[k] = str(round((float(a[k]) * na + float(b[k]) * nb) / n))
    if b.get("anchorCheck", "ok") != "ok":
        out["anchorCheck"] = b["anchorCheck"]
    return out


def cmd_seek(args):
    runner = runner_path(args)
    files = expand(args.files)
    if not files:
        sys.exit("no files")
    failures = 0
    # The seek preroll is the runner's default (the reader's constant); 600
    # is the resume's. --prerolls measures a list instead.
    prerolls = [(None, "seek"), (600, "resume")]
    if args.prerolls:
        prerolls = [(int(p), "resume" if int(p) >= 600 else "seek") for p in args.prerolls.split(",")]
    print(f"{'file':40} {'preroll':>7} {'seeks':>5} {'landed':>6} {'exact':>5} {'maxd':>4} {'snr worst':>9} "
          f"{'snr mean':>8} {'probes':>6} {'max':>3} {'reads':>5} {'max':>3} {'KB':>4} {'max':>4} {'ms':>4} {'max':>4} "
          f"{'before':>6} notes")
    for path in files:
        name = Path(path).name[:40]
        for preroll, kind in prerolls:
            # One runner call a seed, from --seed on, merged: a file's worst
            # start is what the figure is for, and one seed's 50 starts miss
            # it by 5-10 dB (docs/OPUS.md 10.7).
            kv = None
            for seed in range(args.seed, args.seed + max(1, args.seeds)):
                cmd = [str(runner), "seek", path, str(args.seeks), "--seed", str(seed)]
                if preroll is not None:
                    cmd += ["--preroll", str(preroll)]
                r = subprocess.run(cmd, capture_output=True, text=True)
                if r.returncode != 0:
                    print(f"{name:40} runner failed: {r.stderr.strip()[-200:]}")
                    failures += 1
                    kv = None
                    break
                one = parse_kv(r.stdout)
                if one.get("open") != "ok":
                    print(f"{name:40} {one.get('open', '?'):12} refused: {r.stdout.splitlines()[1].split('=', 1)[1]}")
                    kv = None
                    break
                if "seeks" not in one:
                    print(f"{name:40} no seeks: {r.stdout.strip()[-120:]}")
                    kv = None
                    break
                kv = merge_seek(kv, one)
            if kv is None:
                break
            notes = [kv.get("modes", "?")]
            n = int(kv["seeks"])
            if n == 0:
                notes.append(kv.get("note", "no seeks"))
            if "anchorCheck" in kv and kv["anchorCheck"] != "ok":
                notes.append(f"ANCHOR {kv['anchorCheck']}")
                failures += 1
            if int(kv["landed"]) != n:
                notes.append(f"LANDED {kv['landed']} of {n}")
                failures += 1
            snr_worst = float(kv["snrWorst"])
            maxd = int(kv["maxDiffWorst"])
            if n and snr_worst < 35.0:
                notes.append(f"SNR {snr_worst:.1f} dB under 35")
                failures += 1
            if kind == "resume" and n and kv.get("modes") == "celt" and maxd > 4:
                notes.append(f"RESUME |diff| {maxd} over 4 on a CELT file")
                failures += 1
            used = int(kv.get("preroll", preroll or 0))
            ms_mean = model_ms(float(kv["readsMean"]), int(kv["bytesMean"]) / 1024)
            ms_max = model_ms(int(kv["readsMax"]), int(kv["bytesMax"]) / 1024)
            print(f"{name:40} {used:>7} {n:>5} {kv['landed']:>6} {kv['bitExact']:>5} {maxd:>4} {snr_worst:>9.1f} "
                  f"{float(kv['snrMean']):>8.1f} {float(kv['probesMean']):>6.2f} {kv['probesMax']:>3} "
                  f"{float(kv['readsMean']):>5.1f} {kv['readsMax']:>3} {int(kv['bytesMean']) // 1024:>4} "
                  f"{int(kv['bytesMax']) // 1024:>4} {ms_mean:>4.0f} {ms_max:>4.0f} "
                  f"{int(kv['decodedBeforeMean']) // 48:>5}ms {'; '.join(notes)}")
    print(f"{len(files)} files, {max(1, args.seeds)} seed(s) x {args.seeks} starts a file, {failures} failures")
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--work")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def subparser(name):
        # --work after the subcommand too (the usage lines above write it
        # there): SUPPRESS leaves the top-level value alone when it isn't
        # given, so either place sets args.work. Without this, `plan --work
        # DIR FILE MS` took DIR for the file and FILE for the ms.
        s = sub.add_parser(name)
        s.add_argument("--work", default=argparse.SUPPRESS)
        return s

    b = subparser("build")
    b.add_argument("--cxx")
    r = subparser("run")
    r.add_argument("--ffmpeg")
    r.add_argument("files", nargs="+")
    c = subparser("corrupt")
    c.add_argument("file")
    c.add_argument("byte", type=lambda s: int(s, 0))
    k = subparser("seek")
    k.add_argument("--seeks", type=int, default=50)
    k.add_argument("--seed", type=int, default=1)
    k.add_argument("--seeds", type=int, default=1)
    k.add_argument("--prerolls")
    k.add_argument("files", nargs="+")
    p = subparser("plan")
    p.add_argument("--ffmpeg")
    p.add_argument("--sample", type=int)
    p.add_argument("--preroll", type=int)
    p.add_argument("--window", type=int, default=4096)
    p.add_argument("file")
    p.add_argument("ms", type=int)
    args = ap.parse_args()
    if args.cmd == "build":
        cmd_build(args)
        return 0
    if args.cmd == "run":
        return cmd_run(args)
    if args.cmd == "seek":
        return cmd_seek(args)
    if args.cmd == "plan":
        return cmd_plan(args)
    return cmd_corrupt(args)


if __name__ == "__main__":
    sys.exit(main())
