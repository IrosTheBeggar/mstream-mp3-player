#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Card paths from mStream's paths (docs/METADATA.md 2.8.3-2.8.6): the
reference namer, written from the spec.

The algorithm is RECOMMENDED (2.8.3), so the transfer software and a
future device sync (2.12.4) name new files alike; the ledger, not this, is
authoritative for a file already on a card. card_fixtures.py imports it
for vectors.json's `names` group, which the transfer software's namer is
tested against. Every step is here in its plainest form, with its number:

1. split at `/`: the vpath dropped, or kept as a library root (2.8.6);
2. each component a safe name (`clean`);
3. the extension gated by device.txt's `extensions` (2.15);
4. folders past the 8th level joined with ` - `;
5. the path cut to 255 bytes (`fit`), each cut name marked `~XXXX`;
6. collisions by the match key (`match_key`): folders merge, and of two
   files the lower (server, mStream id) keeps the name.

Step 7 (the names read back) is the writer's. Standard library only.
"""
import unicodedata

MUSIC = len("/music/")
MAX_PATH = 255               # bytes of UTF-8 from the volume root (2.8.2)
MAX_NAME_UNITS = 255         # FAT's longest name, in UTF-16 code units
MAX_DEPTH = 8                # folder levels below /music (Library.cpp's kMaxDepth)
ROOM_BELOW_CUT = 64          # a cut folder ends its path this much short of MAX_PATH (step 5)
MAX_FILE_BYTES = (1 << 32) - 1
MAX_DIR_ENTRIES = 65536
JOIN = " - "
ILLEGAL = set('"*/:<>?\\|')
CONTAINER_EXT = {"mp3": "mp3", "flac": "flac", "opus": "opus"}  # a conversion's format: its extension


def fnv(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x00000100000001B3) & ((1 << 64) - 1)
    return h


# ---------------------------------------------------------------------------
# Names (step 2) and the match key (step 6)
# ---------------------------------------------------------------------------
def nfc(text: str) -> str:
    return unicodedata.normalize("NFC", text)


def device_name_end(name: str):
    """Where a Windows device name ends when `name`'s stem is one, any case
    (`CON PRN AUX NUL COM1-COM9 LPT1-LPT9`); None otherwise. The stem runs
    to the first dot, its trailing spaces aside."""
    stem = name.split(".")[0].rstrip(" ")
    up = "".join(c.upper() if c < "\x80" else c for c in stem)
    numbered = len(up) == 4 and up[:3] in ("COM", "LPT") and up[3] in "123456789"
    return len(stem) if up in ("CON", "PRN", "AUX", "NUL") or numbered else None


def clean(raw: str) -> str:
    """Step 2: NFC; U+0000-U+001F, U+007F and `" * / : < > ? \\ |` to `_`;
    leading spaces, and trailing spaces and dots, dropped; leading dots to
    `_`; `_` for nothing left; `_` after a Windows device name's stem."""
    swapped = "".join("_" if c in ILLEGAL or ord(c) < 0x20 or ord(c) == 0x7F else c for c in nfc(raw))
    trimmed = swapped.lstrip(" ").rstrip(" .")
    undotted = trimmed.lstrip(".")
    name = "_" * (len(trimmed) - len(undotted)) + undotted or "_"
    at = device_name_end(name)
    return name if at is None else name[:at] + "_" + name[at:]


def simple_upper(c: str) -> str:
    """A code point's Unicode simple upper case. str.upper() gives the full
    mapping (SpecialCasing.txt's first); where that is several code points
    the simple mapping is the code point itself, but for the Greek vowels
    with ypogegrammeni, whose simple upper case is their title case."""
    u = c.upper()
    if len(u) == 1:
        return u
    o = ord(c)
    if 0x1F80 <= o <= 0x1F87 or 0x1F90 <= o <= 0x1F97 or 0x1FA0 <= o <= 0x1FA7:
        return chr(o + 8)
    if o in (0x1FB3, 0x1FC3, 0x1FF3):
        return chr(o + 9)
    return c


def match_key(name: str) -> str:
    """Step 6: NFC, then each code point's simple upper case."""
    return "".join(simple_upper(c) for c in nfc(name))


def units(name: str) -> int:
    return len(name.encode("utf-16-le")) // 2


def dir_entries(name: str) -> int:
    """A name's FAT directory entries (2.8.4): 1 + ceil(UTF-16 length / 13)."""
    return 1 + -(-units(name) // 13)


def mark(component: str) -> str:
    """Step 5's mark: `~` and the low 16 bits of the FNV-1a 64 of the name's
    bytes as steps 2-4 left it, uncut: a folder's whole name, a file's stem
    without its extension."""
    return "~%04X" % (fnv(component.encode("utf-8")) & 0xFFFF)


# ---------------------------------------------------------------------------
# Step 5: the cut
# ---------------------------------------------------------------------------
class Part:
    """One name of a path being fitted: a folder to make, or the file."""

    def __init__(self, body: str, ext: str = ""):
        self.body = body.encode("utf-8")   # the clean name, uncut; the file's stem
        self.ext = ext                     # the file's extension with its dot; "" for a folder
        self.kept = None                   # bytes of body kept, once cut
        self.mark = mark(body)
        self.n = 0                         # step 6's " (n)", from 2

    def cuttable(self) -> int:
        """The bytes that count as the name's length, and that can be cut."""
        return len(self.body) if self.kept is None else self.kept

    def cut(self, by: int):
        """The name `by` bytes shorter, mark included (a little more, to stop
        at a code point boundary; at most down to its mark alone)."""
        keep = max(self.cuttable() - (by + (len(self.mark) if self.kept is None else 0)), 0)
        while keep and (self.body[keep] & 0xC0) == 0x80:
            keep -= 1
        self.kept = keep

    def name(self) -> str:
        base = self.body.decode("utf-8") if self.kept is None else self.body[:self.kept].decode("utf-8") + self.mark
        return base + (" (%d)" % self.n if self.n >= 2 else "") + self.ext

    def bump(self):
        self.n = max(self.n, 1) + 1


def fit(prefix: int, parts) -> bool:
    """Step 5 for `parts` below a folder `prefix` bytes long (relative to
    /music): while the path is over 255 bytes, or a name over 255 UTF-16
    units, cut the longest name (by its cuttable bytes; ties: the deepest)
    and mark it. A folder whose path ends past byte 255 - 64 is cut to end
    there; the file, or a folder ending there or sooner, by as much as the
    path is over. False when nothing is left to cut."""
    above = MUSIC + prefix + (1 if prefix else 0)
    while True:
        lens = [len(p.name().encode("utf-8")) for p in parts]
        total = above + sum(lens) + len(lens) - 1
        if total <= MAX_PATH and all(units(p.name()) <= MAX_NAME_UNITS for p in parts):
            return True
        cands = [i for i, p in enumerate(parts) if p.cuttable() > 0]
        if not cands:
            return False
        i = max(cands, key=lambda k: (parts[k].cuttable(), k))
        over = max(total - MAX_PATH, 1)
        end = above + sum(lens[:i + 1]) + i
        to_room = max(end - (MAX_PATH - ROOM_BELOW_CUT), 0)
        parts[i].cut(to_room if i + 1 < len(parts) and to_room > 0 else over)


def root_name(vpath: str) -> str:
    """The vpath layout's root folder for a library: its vpath as a step-2
    name, cut as a folder when it ends past byte 255 - 64."""
    root = Part(clean(vpath))
    over = MUSIC + len(root.body) - (MAX_PATH - ROOM_BELOW_CUT)
    if over > 0:
        root.cut(over)
    return root.name()


# ---------------------------------------------------------------------------
# The namer: one run on one card
# ---------------------------------------------------------------------------
class Folder:
    def __init__(self, path: str):
        self.path = path            # relative to /music
        self.entries = {}           # match key -> [(name, folder index or None)]
        self.dir_entries = 2        # `.` and `..`

    def exact(self, name):
        return next((e for e in self.entries.get(match_key(name), []) if e[0] == name), None)

    def merge_with(self, key, name):
        """The folder a new folder `name` merges into: the card's one spelled
        alike, else the first by bytes of those with its key."""
        folders = [e for e in self.entries.get(key, []) if e[1] is not None]
        exact = [e for e in folders if e[0] == name]
        pick = exact[0] if exact else min(folders, default=None, key=lambda e: e[0].encode("utf-8"))
        return None if pick is None else pick[1]

    def add(self, name, folder):
        self.dir_entries += dir_entries(name)
        self.entries.setdefault(match_key(name), []).append((name, folder))


class Namer:
    """Names a run's new files on one card. `libraries`: None for the
    default layout, else the vpath layout's [(server, vpath)]. `extensions`:
    device.txt's list (2.15)."""

    def __init__(self, extensions: str, libraries=None):
        self.extensions = {e.strip().lower() for e in extensions.split(",") if e.strip()}
        self.vpath_layout = libraries is not None
        self.libraries = sorted((s, v) for s, v in libraries or [])
        assert len(self.libraries) <= 16 and len(set(self.libraries)) == len(self.libraries)
        self.folders = [Folder("")]
        self.known = {}             # (parent, key of the clean uncut name) -> folder
        self.roots = {}             # (server, vpath) -> folder

    def seed(self, rel: str, folder: bool):
        """An entry the card lists (relative to /music, as stored)."""
        names = [n for n in rel.split("/") if n]
        if not names:
            return
        at = 0
        for name in names[:-1]:
            at = self._seed_folder(at, name)
        if folder:
            self._seed_folder(at, names[-1])
        elif self.folders[at].exact(names[-1]) is None:
            self.folders[at].add(names[-1], None)

    def _seed_folder(self, at, name):
        e = self.folders[at].exact(name)
        if e is not None and e[1] is not None:
            return e[1]
        if e is not None:   # listed as a file too: FAT holds one, the folder stands
            f = self._below(at, name)
            lst = self.folders[at].entries[match_key(name)]
            lst[lst.index(e)] = (name, f)
            return f
        return self._new_folder(at, name)

    def _below(self, parent, name):
        above = self.folders[parent].path
        self.folders.append(Folder(name if not above else above + "/" + name))
        return len(self.folders) - 1

    def _new_folder(self, parent, name):
        f = self._below(parent, name)
        self.folders[parent].add(name, f)
        return f

    def _free_name(self, at, name):
        n = 1
        while True:
            cand = name if n == 1 else "%s (%d)" % (name, n)
            if match_key(cand) not in self.folders[at].entries:
                return cand
            n += 1

    def _give_roots(self):
        for lib in self.libraries:
            if lib in self.roots:
                continue
            name = root_name(lib[1])
            taken = set(self.roots.values())
            free = [(n, f) for n, f in self.folders[0].entries.get(match_key(name), [])
                    if f is not None and f not in taken]
            if free:
                root = min(free, key=lambda e: (e[0] != name, e[0].encode("utf-8")))[1]
            else:
                root = self._new_folder(0, self._free_name(0, name))
            self.roots[lib] = root

    def _split(self, server_path):
        comps = server_path.split("/")
        vpath, rest = comps[0], comps[1:]
        if not rest:
            return None
        folders = [clean(c) for c in rest[:-1]]
        levels = MAX_DEPTH - (1 if self.vpath_layout else 0)
        if len(folders) > levels:
            folders = folders[:levels - 1] + [JOIN.join(folders[levels - 1:])]
        return vpath, folders, clean(rest[-1])

    def _extension(self, file, bytes_):
        """Step 3: (stem, extension with its dot), or None (unlisted)."""
        stem, dot, ext = file.rpartition(".")
        if not dot:
            stem, ext = file, None
        if bytes_ == "notAudio":
            return stem, "." + ext if ext is not None else ""
        if bytes_ == "oggOpus" and ext is not None and ext.lower() in ("ogg", "oga"):
            ext = "opus"
        elif bytes_ in CONTAINER_EXT:
            ext = CONTAINER_EXT[bytes_]
        elif bytes_ not in ("original", "oggOpus"):
            return None
        if ext is None or ext.lower() not in self.extensions:
            return None
        return stem, "." + ext

    def place(self, files):
        """files: [{server, id, serverPath, size?, bytes?}], named in order of
        (server, id): each a path relative to /music, or a refusal's code."""
        self._give_roots()
        out = [None] * len(files)
        for i in sorted(range(len(files)), key=lambda k: (files[k].get("server", 0), files[k]["id"])):
            out[i] = self._place_one(files[i])
        return out

    def _place_one(self, w):
        if w.get("size", 4_000_000) > MAX_FILE_BYTES:
            return {"refused": "tooBig"}
        split = self._split(w["serverPath"])
        if split is None:
            return {"refused": "noName"}
        vpath, folders, file = split
        at = 0
        if self.vpath_layout:
            at = self.roots.get((w.get("server", 0), vpath))
            if at is None:
                return {"refused": "noRoot"}
        se = self._extension(file, w.get("bytes", "original"))
        if se is None:
            return {"refused": "unlisted"}
        rest = list(folders)
        while rest:
            key = match_key(rest[0])
            f = self.known.get((at, key))
            if f is None:
                f = self.folders[at].merge_with(key, rest[0])
            if f is None:
                break
            at, rest = f, rest[1:]
        parts = [Part(c) for c in rest] + [Part(*se)]
        while True:
            if not fit(len(self.folders[at].path.encode("utf-8")), parts):
                return {"refused": "tooLong"}
            name = parts[0].name()
            key = match_key(name)
            if len(parts) > 1:
                f = self.folders[at].merge_with(key, name)
                if f is not None:   # the name it was cut to is a folder's already: they merge
                    part = parts.pop(0)
                    self.known.setdefault((at, match_key(part.body.decode("utf-8"))), f)
                    at = f
                    continue
            if key in self.folders[at].entries:
                parts[0].bump()
                continue
            break
        if self.folders[at].dir_entries + dir_entries(parts[0].name()) > MAX_DIR_ENTRIES:
            return {"refused": "folderFull"}
        for part in parts[:-1]:
            f = self._new_folder(at, part.name())
            self.known[(at, match_key(part.body.decode("utf-8")))] = f
            at = f
        name = parts[-1].name()
        above = self.folders[at].path
        self.folders[at].add(name, None)
        return name if not above else above + "/" + name


def run(case):
    """One of vectors.json's `names` runs: its names, in its files' order."""
    n = Namer(case["extensions"], None if case["layout"] == "default" else
              [(lib["server"], lib["vpath"]) for lib in case["layout"]["vpath"]])
    for e in case.get("card", []):
        n.seed(e["path"], e.get("folder", False))
    return n.place(case["files"])
