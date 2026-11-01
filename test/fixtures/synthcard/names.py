#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""names.py: names from N11's synthetic card (tools/synthcard.py's plan,
seed 1) for test_text_fold's old-against-new order test: every artist
folder's name, every artist, album artist, album and sort tag value, every
4th album folder's name, every 40th title, every 80th file name, every
4th title and file name with an accent or a typographic apostrophe, and
every name in another script, once each, in the plan's order. Every name
is made up (synthcard's invented syllables and common English words, its
accents, and its made-up Cyrillic, Greek, kana and Hangul strings). No
names from a real library. See README.md.

usage: python -I names.py <repo> <out>
"""
import os
import sys


def names(repo):
    sys.path.insert(0, os.path.join(repo, "tools"))
    import synthcard  # noqa: E402

    plan = synthcard.make_plan(seed=1)
    out, seen = [], set()

    def add(s):
        if isinstance(s, str) and s and s not in seen and "\n" not in s:
            seen.add(s)
            out.append(s)

    def other_script(s):
        return isinstance(s, str) and any(ord(c) >= 0x0370 and not 0x2000 <= ord(c) <= 0x206F for c in s)

    def accented(s):
        return isinstance(s, str) and not s.isascii()

    for i, f in enumerate(plan.folders):
        parts = f.split("/")[1:]
        if len(parts) == 1 or i % 4 == 0:
            add(parts[-1] if parts else None)
        for part in parts:
            if other_script(part):
                add(part)
    for i, f in enumerate(plan.files):
        tags = f.get("tags") or {}
        for key in ("artist", "albumartist", "album", "artistsort", "albumsort", "albumartistsort"):
            v = tags.get(key)
            for s in v if isinstance(v, list) else [v]:
                add(s)
        title = tags.get("title")
        leaf = f["rel"].rsplit("/", 1)[-1]
        if i % 40 == 0 or other_script(title) or (i % 4 == 0 and accented(title)):
            add(title)
        if i % 80 == 0 or other_script(leaf) or (i % 4 == 0 and accented(leaf)):
            add(leaf)
    return out


def main():
    if len(sys.argv) != 3:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    lines = names(sys.argv[1])
    with open(sys.argv[2], "w", encoding="utf-8", newline="\n") as f:
        f.write("# N11's synthetic card's names (tools/synthcard.py's plan, seed 1), made by names.py:\n")
        f.write(f"# {len(lines)} made-up names, one a line, in the plan's order.\n")
        for s in lines:
            f.write(s + "\n")
    print(f"names.py: {len(lines)} names, {os.path.getsize(sys.argv[2])} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
