#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""walk_shape.py: the validation walk's view of N11's synthetic card
(tools/synthcard.py's plan, seed 1), for test_card_jobs: one line per
folder the walk lists, in pre-order, "depth audio images others" as
lib/core CardWalk classifies them (names starting with "." skipped, paths
over kMaxRelPath bytes skipped, folders to 8 levels below /music). No
names. See README.md.

usage: python -I walk_shape.py <repo> <out> [<tree dump to compare>]

A tree dump has "D<TAB>folder" and "F<TAB>file<TAB>size" lines, paths
relative to /music.
"""
import os
import sys

KMAX_REL = 255 - 7  # cardcontract::kMaxRelPath: kMaxCardPath less "/music/"
KMAX_DEPTH = 8      # cardwalk::kMaxDepth
AUDIO = (".mp3", ".flac", ".opus")  # LibraryIndex::formatOf()
IMAGE = (".jpg", ".jpeg")           # LibraryIndex::imageRank()


def kind(leaf):
    low = leaf.lower()
    dot = low.rfind(".")
    if dot <= 0:
        return 2
    ext = low[dot:]
    if ext in AUDIO:
        return 0
    if ext in IMAGE:
        return 1
    return 2


def from_plan(repo):
    sys.path.insert(0, os.path.join(repo, "tools"))
    import synthcard  # noqa: E402

    plan = synthcard.make_plan(seed=1)
    folders = set()
    for f in plan.folders:
        folders.add("" if f == "music" else f[len("music/"):])
    files = [f["rel"][len("music/"):] for f in plan.files if f["rel"].startswith("music/")]
    return folders, files


def from_dump(path):
    folders, files = {""}, []
    for line in open(path, encoding="utf-8"):
        t = line.rstrip("\r\n").split("\t")
        if t[0] == "D":
            folders.add(t[1])
        elif t[0] == "F":
            files.append(t[1])
    return folders, files


def shape(folders, files):
    for rel in list(folders) + files:
        while "/" in rel:
            rel = rel.rsplit("/", 1)[0]
            folders.add(rel)

    def walked(rel):
        if rel == "":
            return True
        parts = rel.split("/")
        if len(parts) > KMAX_DEPTH or any(p.startswith(".") for p in parts):
            return False
        return len(rel.encode("utf-8")) <= KMAX_REL

    counts = {rel: [0, 0, 0] for rel in folders if walked(rel)}
    for rel in files:
        parent, leaf = rel.rsplit("/", 1) if "/" in rel else ("", rel)
        if parent in counts and not leaf.startswith(".") and len(rel.encode("utf-8")) <= KMAX_REL:
            counts[parent][kind(leaf)] += 1
    children = {}
    for rel in counts:
        if rel:
            children.setdefault(rel.rsplit("/", 1)[0] if "/" in rel else "", []).append(rel)
    lines = []
    stack = [("", 0)]
    while stack:
        rel, depth = stack.pop()
        a, i, o = counts[rel]
        lines.append(f"{depth} {a} {i} {o}")
        for c in sorted(children.get(rel, []), reverse=True):
            stack.append((c, depth + 1))
    totals = [sum(v[k] for v in counts.values()) for k in range(3)]
    return lines, totals


def main():
    repo, out = sys.argv[1], sys.argv[2]
    lines, tot = shape(*from_plan(repo))
    print(f"the plan: {len(lines)} folders, {tot[0]} audio, {tot[1]} images, {tot[2]} other")
    if len(sys.argv) > 3:
        other, t2 = shape(*from_dump(sys.argv[3]))
        print(f"the dump: {len(other)} folders, {t2[0]} audio, {t2[1]} images, {t2[2]} other; "
              f"the same shape: {other == lines}")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("# The validation walk's view of N11's synthetic card (tools/synthcard.py's\n")
        f.write("# plan, seed 1, as on the device on 2026-10-08): one line per folder it lists,\n")
        f.write("# in pre-order, \"depth audio images others\" (names starting with \".\" and\n")
        f.write("# paths over 248 bytes left out, as the walk does). No names: test_card_jobs\n")
        f.write(f"# makes them up. {len(lines)} folders, {tot[0]} audio, {tot[1]} images, {tot[2]} other.\n")
        for ln in lines:
            f.write(ln + "\n")


if __name__ == "__main__":
    main()
