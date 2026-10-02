# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""The beat tracker's evaluation harness (docs/BEAT-TRACKER-EVAL.md).

  beat_eval.py build                      compile tools/beat_eval/runner.cpp + lib/core with the host g++
  beat_eval.py decode                     decode the corpus to mono PCM on the device's timeline (cached)
  beat_eval.py run --name NAME            run the runner over every suite
  beat_eval.py score --name NAME          score a run (score.py), write score.json and report.md
  beat_eval.py compare --name A --against B
  beat_eval.py all --name NAME            build, decode (cached), run, score

Paths: --corpus is the ground-truth folder (beats.json, audio/, maddec/devmad.exe),
--work a scratch folder for the runner, the PCM cache and the runs. Both can come
from BEAT_EVAL_CORPUS and BEAT_EVAL_WORK. Nothing is written into the repository.

Needs numpy, and soundfile for the FLACs (the corpus's own venv has both);
mutagen, if installed, reads BPM tags.
"""
import argparse
import concurrent.futures
import datetime
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
RATE = 44100
TRACKER_SOURCES = ["lib/core/BeatTracker.cpp", "lib/core/BeatTracker.h", "lib/core/HopFrontEnd.cpp",
                   "lib/core/HopFrontEnd.h", "lib/core/ClickGen.cpp", "lib/core/ClickGen.h",
                   "tools/beat_eval/runner.cpp"]
EXE = "runner.exe" if os.name == "nt" else "runner"

# Genre by album: the files' own genre tags are missing or vague.
GENRES = {
    "Air/Moon Safari": "downtempo",
    "Aphex Twin/Selected Ambient Works 85-92": "ambient techno",
    "Daft Punk/Discovery": "french house",
    "Emancipator/Mountain of Memory": "downtempo",
    "Kanye West/Graduation": "hip-hop",
    "Kavinsky/OutRun": "synthwave",
}

# Synthetic cases: ClickGen's built-in tracks (exact beats) and variations.
# (name, runner --synth spec, extra args, beat expected)
CLICK_CASES = [
    ("click90", "click:90:60", [], True),
    ("click120", "click:120:60", [], True),
    ("click128", "click:128:60", [], True),
    ("click140", "click:140:60", [], True),
    ("click174", "click:174:60", [], True),
    ("click120off", "click:120off:60", [], True),
    ("click120_noise-30", "click:120:60", ["--noise", "-30", "--seed", "1"], True),
    ("click96_noise-20", "click:96:60", ["--noise", "-20", "--seed", "2"], True),
    ("click150off_noise-20", "click:150off:60", ["--noise", "-20", "--seed", "3"], True),
    ("tempo_120_135", "click:120:30,click:135:30", [], True),
    ("tempo_128_96", "click:128:30,click:96:30", [], True),
    ("tempo_140_128", "click:140:30,click:128:30", [], True),
    ("intro_outro_silence", "silence:10,click:120:30,silence:10", [], True),
    ("intro_outro_noise", "noise:-40:10,click:110:30,noise:-40:10", [], True),
    ("break_8s", "click:124:25,silence:8,click:124:25:0.5", [], True),
    ("noise_only", "noise:-30:30", [], False),
    ("silence_only", "silence:30", [], False),
]
CPU_TRACKS = [23, 26, 37, 53, 70]  # One More Time, HBFS, Alligator, Stronger, Testarossa
MID_START_S = 60.0
MID_SECONDS = 60.0
JOIN_TAIL_S = 30.0
JOIN_HEAD_S = 60.0


def short_path(p):
    """8.3 path on Windows: the scratch folders are deep enough to break MAX_PATH."""
    p = os.path.abspath(p)
    if os.name != "nt":
        return p
    import ctypes
    buf = ctypes.create_unicode_buffer(1024)
    n = ctypes.windll.kernel32.GetShortPathNameW(p, buf, 1024)
    return buf.value if n else p


def paths(args):
    corpus = args.corpus or os.environ.get("BEAT_EVAL_CORPUS")
    work = args.work or os.environ.get("BEAT_EVAL_WORK")
    if not corpus or not work:
        sys.exit("need --corpus and --work (or BEAT_EVAL_CORPUS / BEAT_EVAL_WORK)")
    return Path(corpus), Path(work)


def load_corpus(corpus):
    with open(corpus / "beats.json", encoding="utf-8") as f:
        return json.load(f)


def track_id(t):
    return f"{t['index']:02d}"


def album_of(t):
    parts = t["device_path"].split("/")
    return f"{parts[2]}/{parts[3]}"


# ---- build ----

def cmd_build(args):
    _, work = paths(args)
    out = work / "bin" / EXE
    out.parent.mkdir(parents=True, exist_ok=True)
    cxx = args.cxx or os.environ.get("CXX", "g++")
    cmd = [cxx, "-std=gnu++17", "-O2", "-Wall", "-Wextra", "-I", str(REPO / "lib/core"),
           str(HERE / "runner.cpp"), str(REPO / "lib/core/BeatTracker.cpp"), str(REPO / "lib/core/HopFrontEnd.cpp"),
           str(REPO / "lib/core/ClickGen.cpp"), "-o", str(out)]
    if os.name == "nt":
        cmd.insert(1, "-static")  # MSYS2's libstdc++ DLLs aren't on every PATH
    print(" ".join(cmd))
    subprocess.run(cmd, check=True)
    print(f"built {out}")


# ---- decode ----

def device_offset(t):
    """Frames the firmware's gapless trim drops from the start of devmad's output.

    devmad keeps everything (beats.json's timeline). The device never decodes a
    Xing/Info/VBRI header frame (1152 silent samples in devmad's output) and, for a
    LAME/Lavf/Lavc tag, also skips the encoder delay + 529 (docs/GAPLESS.md 4.2-4.3;
    the generator's lead sample is skipped too, and devmad never writes it)."""
    if t["codec"] != "mp3":
        return 0
    d = t["decode"]
    skip = d.get("xing_frame_samples") or 0
    enc = d.get("encoder") or ""
    if d.get("tag") in ("Xing", "Info") and enc[:4] in ("LAME", "Lavf", "Lavc") and d.get("enc_delay") is not None:
        skip += d["enc_delay"] + 529
    return skip


def read_tags(path):
    try:
        import mutagen
    except ImportError:
        return {}
    try:
        f = mutagen.File(path)
    except Exception:  # noqa: BLE001 (a tag reader's problem isn't the harness's)
        return {}
    out = {}
    if f is None or f.tags is None:
        return out
    for k in f.tags.keys():
        kl = str(k).lower()
        v = f.tags[k]
        v = v[0] if isinstance(v, list) and v else v
        v = str(v)
        if kl in ("tbpm", "bpm", "tempo"):
            try:
                bpm = float(v)
                if bpm > 0:
                    out["tag_bpm"] = bpm
            except ValueError:
                pass
        elif kl in ("tcon", "genre"):
            out["tag_genre"] = v
    return out


def decode_one(corpus, work, t, devmad):
    pcm_dir = work / "pcm"
    out = pcm_dir / f"{track_id(t)}.s16"
    meta_path = pcm_dir / f"{track_id(t)}.json"
    if out.exists() and meta_path.exists():
        return track_id(t), "cached"
    src = corpus / "audio" / t["device_path"][len("/music/"):]
    if t["codec"] == "mp3":
        with tempfile.TemporaryDirectory(dir=work) as tmp:
            raw = Path(tmp) / "stereo.s16"
            r = subprocess.run([devmad, short_path(src), short_path(tmp) + os.sep + "stereo.s16"],
                               capture_output=True, text=True)
            if r.returncode != 0:
                raise RuntimeError(f"devmad failed on {src}: {r.stderr[-400:]}")
            x = np.fromfile(raw, dtype="<i2").reshape(-1, 2)
    else:
        import soundfile as sf
        x, sr = sf.read(short_path(src), dtype="int16", always_2d=True)
        assert sr == RATE, (src, sr)
        if x.shape[1] == 1:
            x = np.repeat(x, 2, axis=1)
    expected = t["decode"].get("samples")
    if expected and expected != len(x):
        raise RuntimeError(f"{src}: decoded {len(x)} samples, beats.json says {expected}")
    mono = ((x[:, 0].astype(np.int32) + x[:, 1].astype(np.int32)) >> 1).astype("<i2")  # AudioTap's mix
    offset = device_offset(t)
    mono[offset:].tofile(out)
    meta = dict(index=t["index"], device_path=t["device_path"], codec=t["codec"], offset_frames=offset,
                frames=int(len(mono) - offset), devmad_frames=int(len(mono)), album=album_of(t),
                genre=GENRES.get(album_of(t), "?"))
    meta.update(read_tags(src))
    meta_path.write_text(json.dumps(meta, indent=1), encoding="utf-8")
    return track_id(t), "decoded"


def cmd_decode(args):
    corpus, work = paths(args)
    gt = load_corpus(corpus)
    (work / "pcm").mkdir(parents=True, exist_ok=True)
    devmad = short_path(corpus / "maddec" / "devmad.exe")
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        futs = [ex.submit(decode_one, corpus, work, t, devmad) for t in gt["tracks"]]
        for f in concurrent.futures.as_completed(futs):
            tid, what = f.result()
            if what != "cached":
                print(f"[{tid}] {what}", flush=True)
    print("decode done")


# ---- run ----

def git_state():
    def git(*a):
        return subprocess.run(["git", *a], cwd=REPO, capture_output=True, text=True).stdout.strip()
    return dict(commit=git("rev-parse", "--short", "HEAD"),
                dirty=bool(git("status", "--porcelain", "--", "lib/core", "tools/beat_eval")))


def sources_hash():
    h = hashlib.sha1()
    for s in TRACKER_SOURCES:
        h.update((REPO / s).read_bytes())
    return h.hexdigest()[:12]


def run_runner(exe, argv, out_path):
    r = subprocess.run([exe, *argv], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"runner {argv}: {r.stderr}")
    out_path.write_text(r.stdout, encoding="utf-8")


def prior_for(t, mode):
    if mode == "none":
        return 0.0
    if mode == "ref":
        return float(t["tempo_bpm"])
    raise ValueError(mode)


def join_pcm(work, a, b):
    """The end of track a and the start of track b, back to back: a gapless join
    (the epoch carries on, so the firmware doesn't reset the tracker)."""
    out = work / "joins" / f"{track_id(a)}-{track_id(b)}.s16"
    meta_path = out.with_suffix(".json")
    if out.exists() and meta_path.exists():
        return out, json.loads(meta_path.read_text(encoding="utf-8"))
    out.parent.mkdir(parents=True, exist_ok=True)
    xa = np.fromfile(work / "pcm" / f"{track_id(a)}.s16", dtype="<i2")
    xb = np.fromfile(work / "pcm" / f"{track_id(b)}.s16", dtype="<i2")
    tail = xa[max(0, len(xa) - int(JOIN_TAIL_S * RATE)):]
    head = xb[:int(JOIN_HEAD_S * RATE)]
    np.concatenate([tail, head]).tofile(out)
    meta = dict(a=a["index"], b=b["index"], a_start_frame=int(len(xa) - len(tail)), join_frame=int(len(tail)),
                frames=int(len(tail) + len(head)))
    meta_path.write_text(json.dumps(meta), encoding="utf-8")
    return out, meta


def cmd_run(args):
    corpus, work = paths(args)
    gt = load_corpus(corpus)
    exe = str(work / "bin" / EXE)
    if not Path(exe).exists():
        sys.exit("build first (beat_eval.py build)")
    run_dir = work / "runs" / args.name
    suites = args.suites.split(",")
    for s in suites:
        (run_dir / s).mkdir(parents=True, exist_ok=True)
    jobs = []
    tracks = gt["tracks"]
    if args.tracks:
        keep = {int(i) for i in args.tracks.split(",")}
        tracks = [t for t in tracks if t["index"] in keep]
    via = ["--via-hops"] if args.via_hops else []
    for t in tracks:
        pcm = short_path(work / "pcm" / f"{track_id(t)}.s16")
        prior = ["--prior", f"{prior_for(t, args.prior):.3f}"] if args.prior != "none" else []
        if "corpus" in suites:
            jobs.append(([f"--pcm", pcm, *prior, *via], run_dir / "corpus" / f"{track_id(t)}.txt"))
        if "mid" in suites and t["duration_s"] >= MID_START_S + MID_SECONDS:
            jobs.append((["--pcm", pcm, "--start", str(MID_START_S), "--seconds", str(MID_SECONDS), *prior, *via],
                         run_dir / "mid" / f"{track_id(t)}.txt"))
    if "joins" in suites:
        by_album = {}
        for t in tracks:
            by_album.setdefault(album_of(t), []).append(t)
        for album, ts in by_album.items():
            ts.sort(key=lambda t: t["index"])
            for a, b in zip(ts, ts[1:]):
                if b["index"] != a["index"] + 1:
                    continue
                pcm, _ = join_pcm(work, a, b)
                prior = ["--prior", f"{prior_for(b, args.prior):.3f}"] if args.prior != "none" else []
                jobs.append((["--pcm", short_path(pcm), *prior, *via],
                             run_dir / "joins" / f"{track_id(a)}-{track_id(b)}.txt"))
    if "clicks" in suites:
        for name, spec, extra, _ in CLICK_CASES:
            jobs.append((["--synth", spec, *extra, *via], run_dir / "clicks" / f"{name}.txt"))
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        list(ex.map(lambda j: run_runner(exe, *j), jobs))
    cpu = measure_cpu(exe, work)
    info = dict(name=args.name, date=datetime.datetime.now().isoformat(timespec="seconds"), prior=args.prior,
                via_hops=args.via_hops, suites=suites, sources=sources_hash(), cpu=cpu, **git_state())
    (run_dir / "run.json").write_text(json.dumps(info, indent=1), encoding="utf-8")
    print(f"ran {len(jobs)} cases into {run_dir}; cpu {cpu['median_ns_per_audio_s']:.0f} ns per audio s")


def kv(line):
    return {k: v for k, v in (tok.split("=", 1) for tok in line.split() if "=" in tok)}


def measure_cpu(exe, work):
    """The host's CPU proxy, measured serially after the runs (the per-case figures in
    the runs' output are taken in parallel, so they're only rough): the boot bench's
    case (click120, 60 s) and the first 60 s of a few real tracks, best of 5 each."""
    out = {}
    b = subprocess.run([exe, "--bench", "--repeat", "5"], capture_output=True, text=True, check=True).stdout
    out["click120"] = float(kv(b)["best_ns_per_audio_s"])
    for i in CPU_TRACKS:
        pcm = work / "pcm" / f"{i:02d}.s16"
        if not pcm.exists():
            continue
        r = subprocess.run([exe, "--pcm", short_path(pcm), "--time-only", "--seconds", "60", "--repeat", "5"],
                           capture_output=True, text=True, check=True).stdout
        out[f"{i:02d}"] = float(kv(r)["best_ns_per_audio_s"])
    out["median_ns_per_audio_s"] = float(np.median(list(out.values())))
    return out


def cmd_score(args):
    import score
    corpus, work = paths(args)
    score.score_run(corpus, work, args.name, quiet=args.quiet)


def cmd_compare(args):
    import score
    _, work = paths(args)
    score.compare(work, args.name, args.against)


def cmd_all(args):
    cmd_build(args)
    cmd_decode(args)
    cmd_run(args)
    cmd_score(args)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--corpus")
    p.add_argument("--work")
    p.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    sub = p.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--cxx")
    sub.add_parser("decode")
    for name in ("run", "all"):
        r = sub.add_parser(name)
        r.add_argument("--name", required=True)
        r.add_argument("--prior", choices=["none", "ref"], default="none",
                       help="ref: the reference tempo as a metadata prior (setPrior)")
        r.add_argument("--suites", default="corpus,mid,joins,clicks")
        r.add_argument("--tracks", help="comma-separated corpus indices (default: all)")
        r.add_argument("--via-hops", action="store_true", help="feed through feedHop() (the USB visualizer's path)")
        r.add_argument("--quiet", action="store_true")
        if name == "all":
            r.add_argument("--cxx")
    s = sub.add_parser("score")
    s.add_argument("--name", required=True)
    s.add_argument("--quiet", action="store_true")
    c = sub.add_parser("compare")
    c.add_argument("--name", required=True)
    c.add_argument("--against", required=True)
    args = p.parse_args()
    sys.path.insert(0, str(HERE))
    dict(build=cmd_build, decode=cmd_decode, run=cmd_run, score=cmd_score, compare=cmd_compare, all=cmd_all)[args.cmd](args)


if __name__ == "__main__":
    main()
