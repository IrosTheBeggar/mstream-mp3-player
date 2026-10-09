#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""fatmodel.py: the card reads of a lookup and of a walk on a card in the
user's shape, through FatFs as the Core2 builds it, with and without the
PSRAM sector cache (docs/METADATA.md 3.2.4 and 3.2.7, row N8 of 6.1).

Host only: ChaN's FatFs R0.15 (test/support/fatfs, configured as ESP-IDF
5.5.5 builds it for the Core2) on a RAM disk, with lib/core/SectorCache in
front of it, built with the host's gcc and g++ (tools/fatmodel.cpp). The
card is tools/synthcard.py's: its plan (no tree is written; the same seed
and count give the same card), copied in the order its --copy-to copies
(each folder before what it holds, names in NTFS's order), or a tree it
already built (--tree).

Usage:

    python tools/fatmodel.py [--count N] [--seed S] [--tree DIR]
                             [--cluster BYTES] [--gb N] [--caches 64,128,256]

        prints, per open (f_open of every audio file once, in a random
        order) and per walk (3.2.3's, and today's nested readdir), the card
        reads stock (every FatFs disk_read a card read, as the SD driver
        does) and through each cache size, and the seconds they cost at the
        research's 0.6-1.0 ms a card read and 35 us a cached sector; the
        probes' opens (L0's open bench); the scan's reads. --json: the
        same figures as one JSON object.

The model's own checks are test_fat_model's (pio test -e native -f
test_fat_model: the cache under FatFs, and FatFs's reads against the
research's count on lib/core/LibrarySynth's tree); this script's are
tools/test_fatmodel.py (python -m unittest discover -s tools -p
"test_fatmodel.py"), on a small card.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):
    pass

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import synthcard  # noqa: E402

CORE = ROOT / "lib" / "core"
SUPPORT = ROOT / "test" / "support"


def plan_listing(count, seed):
    """The copy's listing of synthcard's plan, and its probes' first files."""
    plan = synthcard.make_plan(count, seed)
    children = {}
    for d in plan.folders:
        if "/" in d:
            parent, name = d.rsplit("/", 1)
            children.setdefault(parent, []).append((name, None))
    for f in plan.files:
        parent, name = f["rel"].rsplit("/", 1)
        children.setdefault(parent, []).append((name, len(synthcard.file_bytes(f)[0])))
    rows = [("F", "SYNTHCARD.TXT", 260)]

    def walk(rel):
        rows.append(("D", rel, 0))
        for name, size in sorted(children.get(rel, []), key=lambda c: synthcard.ntfs_key(c[0])):
            if size is None:
                walk(f"{rel}/{name}")
            else:
                rows.append(("F", f"{rel}/{name}", size))
    walk("music")
    probes = [p["first"] for p in getattr(plan, "probes", [])]
    return rows, probes


def tree_listing(out):
    rows = [(("D" if kind == "dir" else "F"), rel, size) for kind, rel, size in synthcard.copy_plan(out)]
    probes = []
    marker = os.path.join(out, synthcard.MARKER)
    if os.path.isfile(marker):
        with open(marker, encoding="utf-8") as fh:
            probes = [p["first"] for p in json.load(fh).get("probes", [])]
    return rows, probes


def build():
    """The model's executable (cached in the temp folder by its sources' hash)."""
    gcc, gxx = shutil.which("gcc"), shutil.which("g++")
    if not gcc or not gxx:
        sys.exit("fatmodel: needs gcc and g++ on PATH (MinGW-w64 on Windows)")
    c_src = SUPPORT / "fatfs" / "FatFsBuild.c"
    cpp = [ROOT / "tools" / "fatmodel.cpp", CORE / "SectorCache.cpp", CORE / "CachedDrive.cpp"]
    deps = sorted((SUPPORT / "fatfs").iterdir()) + [SUPPORT / "FatModel.h", CORE / "SectorCache.h",
                                                    CORE / "CachedDrive.h", CORE / "SdBusy.h"] + cpp
    h = hashlib.sha256()
    for p in deps:
        if p.is_file():
            h.update(p.read_bytes())
    tmp = Path(tempfile.gettempdir())
    exe = tmp / f"fatmodel_{h.hexdigest()[:12]}{'.exe' if os.name == 'nt' else ''}"
    if exe.exists():
        return exe
    obj = tmp / f"fatmodel_fatfs_{h.hexdigest()[:12]}.o"
    r = subprocess.run([gcc, "-O2", "-w", "-c", str(c_src), "-o", str(obj)], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"fatmodel: FatFs didn't build:\n{r.stderr}")
    r = subprocess.run([gxx, "-std=gnu++17", "-O2", "-w", "-I", str(CORE), "-I", str(SUPPORT)] + [str(c) for c in cpp] +
                       [str(obj), "-o", str(exe)], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"fatmodel: the model didn't build:\n{r.stderr}")
    return exe


def measure(rows, probes, cluster=32768, gb=64, caches="64,128,256", seed=1, as_json=False):
    """Runs the model on a listing; the subprocess's result (its text, or
    its JSON with as_json, in stdout when captured)."""
    exe = build()
    fd, listing = tempfile.mkstemp(prefix="fatmodel_", suffix=".txt")
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as fh:
            for kind, rel, size in rows:
                fh.write(f"{kind}\t{rel}\t{size}\n" if kind == "F" else f"{kind}\t{rel}\n")
        args = [str(exe), listing, "--cluster", str(cluster), "--gb", str(gb), "--caches", caches, "--seed", str(seed)]
        for p in probes:
            args += ["--probe", p]
        if as_json:
            args += ["--json", "1"]
            r = subprocess.run(args, capture_output=True, text=True, encoding="utf-8")
            if r.returncode != 0:
                raise RuntimeError(f"fatmodel failed: {r.stderr.strip()[:500]}")
            return json.loads(r.stdout)
        sys.stdout.flush()
        return subprocess.run(args).returncode
    finally:
        os.remove(listing)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--count", type=int, default=synthcard.MEASURED["audio_files"],
                    help="audio files in synthcard's plan (default 19,519)")
    ap.add_argument("--seed", type=int, default=1, help="synthcard's seed (default 1)")
    ap.add_argument("--tree", help="a tree tools/synthcard.py built, instead of its plan")
    ap.add_argument("--cluster", type=int, default=32768, help="the FAT32 cluster in bytes (default 32768)")
    ap.add_argument("--gb", type=int, default=64, help="the card's size in GB (default 64)")
    ap.add_argument("--caches", default="64,128,256", help="the cache sizes in sectors (default 64,128,256)")
    ap.add_argument("--json", action="store_true", help="the figures as one JSON object")
    a = ap.parse_args(argv)
    if a.tree:
        rows, probes = tree_listing(a.tree)
        what = "a tree tools/synthcard.py built"
    else:
        rows, probes = plan_listing(a.count, a.seed)
        what = f"tools/synthcard.py's plan, seed {a.seed}, {a.count:,} audio files"
    if a.json:
        print(json.dumps(measure(rows, probes, a.cluster, a.gb, a.caches, a.seed, as_json=True), indent=1,
                         ensure_ascii=False))
        return 0
    print(f"FatFs R0.15 as the Core2 builds it (test/support/fatfs), on {what}.")
    return measure(rows, probes, a.cluster, a.gb, a.caches, a.seed)


if __name__ == "__main__":
    sys.exit(main())
