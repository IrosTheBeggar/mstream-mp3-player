#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""The play log, /.player/plays.jnl (MPPL v1, docs/METADATA.md 2.20): the
reference reader and writer, and the rules a reader applies after it.

Every rule a second implementation must reproduce is here in its plainest
form, with the section it implements: the 80-byte slot (2.20.3), the files
and their rotation (2.20.2), the device's ids and its seq at boot (2.20.4),
the reader's verdicts (2.20.6), the dating of plays made before the clock
was set, the play's server path through the ledger, and MSMF's ACKS
section (2.5.6). The firmware's writer and the transfer software's
reader (neither built yet) are to be tested against what it makes:
card_fixtures.py imports it for the play log's fixtures in
test/fixtures/card/plays and for vectors.json's plays group, and
test_playlog.py tests it. Standard library only.

    python tools/playlog.py dump FILE...        every slot, one JSON object a line
    python tools/playlog.py read FILE...        the log as a reader takes it (2.20.6)

`Device` models the firmware's side: its NVS (epoch, seq, boot count),
the files on the card, a play's slot appended at its end, the rotation
under a cap, the trim by a root's ACKS, and a power cut inside the
rotation with the twins a cut rename leaves. Its cap is a parameter (the
contract's is 6,553 slots a file; the fixtures use 4 to show a rotation).
"""
import argparse
import json
import struct
import sys
import zlib

# ---------------------------------------------------------------------------
# The slot (2.20.3)
# ---------------------------------------------------------------------------
MAGIC = b"MPPL"
MAJOR = 1
SLOT = 80
KIND_PLAY, KIND_DROP = 1, 2
OUTCOMES = {1: "completed", 2: "skipped", 3: "stopped"}
SOURCES = {0: "other", 1: "manual", 2: "shuffle", 3: "autodj"}
CLOCKS = {0: "unset", 1: "rtc", 2: "host"}
FILE_SLOTS = 6553                       # a file's cap: 524,240 bytes, under 512 KiB (2.20.2)
LOG_BYTES = 1 << 20                     # the two files together, for the software's room on the card
NAMES = ["plays.jnl", "plays.1.jnl"] + ["plays.%d.jnl" % n for n in range(2, 10)]  # what a reader reads
TWINS = ["plays.xl%d" % n for n in range(1, 10)]  # a cut rename's twins (2.20.2): the device's; nobody reads them
ID_PREFIX = "core2"
M32, M64 = (1 << 32) - 1, (1 << 64) - 1

# offset 0 magic, 4 major, 5 kind, 6 outcome, 7 source, 8 seq, 12 pauseCount, 14 clock, 15 reserved,
# 16 epoch, 24 deviceId (6 bytes), 30 reserved, 32 startedAt, 40 bootSeq, 44 uptimeMs,
# 48 playedMs (a drop note: dropped), 52 durationMs, 56 pathHash, 64 size, 68 fatTime,
# 72 generation, 76 crc
LAYOUT = struct.Struct("<4sBBBBIHBBQ6sHQIIIIQIII")
assert LAYOUT.size == SLOT - 4

PLAY_FIELDS = ("outcome", "source", "seq", "pauseCount", "clock", "epoch", "deviceId", "startedAt", "bootSeq",
               "uptimeMs", "playedMs", "durationMs", "pathHash", "size", "fatTime", "generation")


def crc32(b: bytes) -> int:
    """CRC-32/ISO-HDLC (2.3.2)."""
    return zlib.crc32(b) & 0xFFFFFFFF


def device_bytes(device_id) -> bytes:
    """A deviceId as its 6 bytes: from 12 hex digits (any case, ':' or '-'
    between pairs allowed) or bytes."""
    if isinstance(device_id, (bytes, bytearray)):
        b = bytes(device_id)
    else:
        b = bytes.fromhex(device_id.replace(":", "").replace("-", ""))
    if len(b) != 6:
        raise ValueError("a deviceId is 6 bytes")
    return b


def u64(v) -> int:
    return int(v, 0) if isinstance(v, str) else int(v)


def encode(slot: dict) -> bytes:
    """One slot's 80 bytes. slot: {"kind": 1 or 2, ...}; a play's fields as
    PLAY_FIELDS names them, a drop note's deviceId, epoch, clock, startedAt,
    bootSeq, uptimeMs and dropped. Missing fields are 0. Values are written
    as given (a test may write an unknown enum value)."""
    kind = slot.get("kind", KIND_PLAY)
    g = lambda k: u64(slot.get(k, 0))  # noqa: E731
    dev = device_bytes(slot["deviceId"]) if "deviceId" in slot else bytes(6)
    if kind == KIND_PLAY:
        body = LAYOUT.pack(slot.get("magic", MAGIC), slot.get("major", MAJOR), kind, g("outcome"), g("source"),
                           g("seq"), g("pauseCount"), g("clock"), 0, g("epoch"), dev, 0, g("startedAt"),
                           g("bootSeq"), g("uptimeMs"), g("playedMs"), g("durationMs"), g("pathHash"), g("size"),
                           g("fatTime"), g("generation"))
    else:
        body = LAYOUT.pack(slot.get("magic", MAGIC), slot.get("major", MAJOR), kind, 0, 0, 0, 0, g("clock"), 0,
                           g("epoch"), dev, 0, g("startedAt"), g("bootSeq"), g("uptimeMs"), g("dropped"), 0, 0, 0, 0,
                           0)
    return body + struct.pack("<I", crc32(body))


def decode(b: bytes):
    """2.20.6's verdict on one slot: (why, slot). why: "ok", "short" (a
    tail under 80 bytes), "magic", "newer" (the magic, another major: a
    newer firmware's), "crc", or "kind" (a valid slot of a kind this reader
    doesn't know). slot: the fields, for "ok" (and "kind": its kind)."""
    if len(b) < SLOT:
        return "short", None
    (magic, major, kind, outcome, source, seq, pauses, clock, _r1, epoch, dev, _r2, started, boot, up, played,
     duration, path_hash, size, fat, gen) = LAYOUT.unpack_from(b, 0)
    if magic != MAGIC:
        return "magic", None
    if major != MAJOR:
        return "newer", {"major": major}
    if crc32(b[:SLOT - 4]) != struct.unpack_from("<I", b, SLOT - 4)[0]:
        return "crc", None
    if kind == KIND_PLAY:
        return "ok", {"kind": kind, "outcome": outcome, "source": source, "seq": seq, "pauseCount": pauses,
                      "clock": clock, "epoch": epoch, "deviceId": dev.hex(), "startedAt": started, "bootSeq": boot,
                      "uptimeMs": up, "playedMs": played, "durationMs": duration, "pathHash": path_hash,
                      "size": size, "fatTime": fat, "generation": gen}
    if kind == KIND_DROP:
        return "ok", {"kind": kind, "clock": clock, "epoch": epoch, "deviceId": dev.hex(), "startedAt": started,
                      "bootSeq": boot, "uptimeMs": up, "dropped": played}
    return "kind", {"kind": kind}


def outcome_name(v: int) -> str:
    """2.20.3: an unknown outcome (0, or above 3) reads as stopped."""
    return OUTCOMES.get(v, "stopped")


def source_name(v: int) -> str:
    """2.20.3: an unknown source reads as 0, not said: mStream's 'other'."""
    return SOURCES.get(v, "other")


def play_id(p: dict) -> str:
    """2.20.4: core2-<deviceId, 12 hex>-<epoch, 16 hex>-<seq, decimal>."""
    return "%s-%s-%016x-%d" % (ID_PREFIX, device_bytes(p["deviceId"]).hex(), u64(p["epoch"]), p["seq"])


def key(p: dict):
    """A play's identity: (deviceId bytes, epoch, seq), its sort order."""
    return device_bytes(p["deviceId"]), u64(p["epoch"]), p["seq"]


# ---------------------------------------------------------------------------
# Reading (2.20.6)
# ---------------------------------------------------------------------------
def read_file(data: bytes, limit=FILE_SLOTS * SLOT):
    """Every slot of one file: [(offset, why, slot, raw)], a tail shorter
    than a slot as one "short" entry. The bytes past `limit`, the cap, are
    no device's (2.20.6, item 1): one "past" entry, its slot {"slots": the
    80-byte slots, or parts of one, they hold}, unread. None reads all."""
    out = []
    end = len(data) if limit is None else min(len(data), limit)
    for off in range(0, end, SLOT):
        raw = bytes(data[off:min(off + SLOT, end)])
        why, slot = decode(raw)
        out.append((off, why, slot, raw))
    if end < len(data):
        out.append((end, "past", {"slots": -(-(len(data) - end) // SLOT)}, b""))
    return out


def valid_slots(files):
    """Every valid slot, play or drop note, of the log's files (2.20.2's
    names) in {name: bytes}: twins and conflicting slots each counted,
    since the boot rules (2.20.4) take them all."""
    return [s for name in sorted(files) if name in NAMES for _, why, s, _ in read_file(files[name])
            if why == "ok" and s["kind"] in (KIND_PLAY, KIND_DROP)]


def usable(p: dict) -> bool:
    """A valid play slot the software may use: an epoch, a seq, a deviceId."""
    return p["epoch"] != 0 and p["seq"] >= 1 and p["deviceId"] != "000000000000"


def read_log(files: dict):
    """The log as the software takes it from {name: bytes} (any of NAMES;
    the order of the files doesn't matter): the plays, each once, by
    (deviceId, epoch, seq), and the counts of what it skipped."""
    seen, notes = {}, set()
    counts = {"slots": 0, "damaged": 0, "tail": 0, "newer": 0, "unknownKinds": 0, "unusable": 0, "twins": 0,
              "conflicts": 0, "dropped": 0}
    conflicted = set()
    for name in sorted(files, key=lambda n: (NAMES.index(n) if n in NAMES else len(NAMES), n)):
        for _, why, slot, raw in read_file(files[name]):
            if why == "short":
                counts["tail"] += 1
                continue
            if why == "past":
                # Past the cap: counted as damaged slots, not read (2.20.6, item 1).
                counts["slots"] += slot["slots"]
                counts["damaged"] += slot["slots"]
                continue
            counts["slots"] += 1
            if why in ("magic", "crc"):
                counts["damaged"] += 1
            elif why == "newer":
                counts["newer"] += 1
            elif why == "kind":
                counts["unknownKinds"] += 1
            elif slot["kind"] == KIND_DROP:
                if raw in notes:
                    counts["twins"] += 1
                else:
                    notes.add(raw)
                    counts["dropped"] += slot["dropped"]
            elif not usable(slot):
                counts["unusable"] += 1
            else:
                k = key(slot)
                if k in conflicted:
                    continue
                if k in seen:
                    if seen[k][1] == raw:
                        counts["twins"] += 1
                    else:
                        # One key, two different slots: neither is used, so a
                        # play is never credited wrongly.
                        del seen[k]
                        conflicted.add(k)
                        counts["conflicts"] += 1
                    continue
                seen[k] = (slot, raw)
    plays = [seen[k][0] for k in sorted(seen)]
    return {"plays": plays, "counts": counts}


def _mine(slots, device_id, epoch):
    d, e = device_bytes(device_id), u64(epoch)
    return [s for s in slots if device_bytes(s["deviceId"]) == d and u64(s["epoch"]) == e]


def last_seq(slots, device_id, epoch) -> int:
    """The highest seq of one device and epoch among valid play slots (0:
    none). slots: valid_slots()'s, or read_log()'s plays."""
    return max((s["seq"] for s in _mine(slots, device_id, epoch) if s.get("kind", KIND_PLAY) == KIND_PLAY),
               default=0)


def boot_seq(nvs_seq: int, slots, device_id, epoch) -> int:
    """2.20.4: the next seq at boot, max(NVS's, the card's last for this
    device and epoch) + 1, over every valid play slot (valid_slots(): a
    conflicting pair's seq is used, though a reader drops both)."""
    return max(nvs_seq, last_seq(slots, device_id, epoch)) + 1


def boot_count(nvs_boots: int, slots, device_id, epoch) -> int:
    """2.20.4: this boot's bootSeq, max(NVS's boot count, the highest
    bootSeq of a valid slot of this device and epoch, a play's or a drop
    note's) + 1: NVS behind the card for one is behind for the other."""
    return max([nvs_boots] + [s["bootSeq"] for s in _mine(slots, device_id, epoch)]) + 1


def dates(plays):
    """2.20.6: each play's start in epoch ms, or None. A play the clock
    didn't date takes it from the first dated play after it of the same
    device, epoch and boot: that one's startedAt less the uptime between
    them (mod 2^32). {key: ms or None}."""
    out = {}
    by_boot = {}
    for p in plays:
        by_boot.setdefault((device_bytes(p["deviceId"]), u64(p["epoch"]), p["bootSeq"]), []).append(p)
    for group in by_boot.values():
        group.sort(key=lambda p: p["seq"])
        later = None
        for p in reversed(group):
            if p["startedAt"]:
                out[key(p)] = p["startedAt"]
                later = p
            elif later is not None:
                out[key(p)] = later["startedAt"] - ((later["uptimeMs"] - p["uptimeMs"]) & M32)
            else:
                out[key(p)] = None
    return out


# ---------------------------------------------------------------------------
# A play's server file (2.20.6), through the ledger of the root that counts
# ---------------------------------------------------------------------------
def wall(t):
    """2.3.4's W of a valid non-zero FAT stamp, else None."""
    if t == 0:
        return None
    date, tm = t >> 16, t & 0xFFFF
    y, mo, d, h, mi, s2 = 1980 + (date >> 9), (date >> 5) & 15, date & 31, tm >> 11, (tm >> 5) & 63, tm & 31
    leap = lambda yy: (yy % 4 == 0 and yy % 100 != 0) or yy % 400 == 0  # noqa: E731
    month_days = [31, 29 if leap(y) else 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
    if not (1 <= mo <= 12 and 1 <= d <= month_days[mo - 1] and h < 24 and mi < 60 and s2 < 30):
        return None
    days = sum(366 if leap(yy) else 365 for yy in range(1980, y)) + sum(month_days[:mo - 1]) + d - 1
    return days * 86400 + h * 3600 + mi * 60 + 2 * s2


def same_file(play, rec, skew=0) -> bool:
    """The file the device played is the ledger's: the same size, and valid
    stamps that are equal or differ by the tags file's skew (2.3.4). A 0 or
    an invalid stamp never matches: a slot has no qfp to decide."""
    a, b = wall(rec["fatTime"]), wall(play["fatTime"])
    if play["size"] != rec["size"] or a is None or b is None:
        return False
    return b == a or (skew != 0 and b - a == skew)


def resolve(plays, ledger, skew=0, posted=None):
    """2.20.6 over plays (read_log's) and the ledger of the root that counts
    (each record {"pathHash", "size", "fatTime", "serverPath", "server"};
    server: ORIG's index, 2.5.5). posted: {(deviceId hex, epoch): seq} from
    state.json; plays at or below it are done. Returns one entry per new
    play, in order: {"id", "send": bool, "why", the fields mStream takes}.
    Where it goes ("server") is the row's server index; the caller maps a
    peer's to its parent with the peer's id (2.5.5)."""
    by_hash = {}
    for r in ledger:
        by_hash.setdefault(r["pathHash"], []).append(r)
    when = dates(plays)
    out = []
    for p in plays:
        mark = (posted or {}).get((p["deviceId"], u64(p["epoch"])), 0)
        if p["seq"] <= mark:
            continue
        e = {"id": play_id(p)}
        cands = [r for r in by_hash.get(p["pathHash"], []) if r.get("serverPath")]
        if not cands:
            out.append(dict(e, send=False, why="not in the ledger: the listener's file"))
            continue
        if len(cands) > 1:
            out.append(dict(e, send=False, why="two ledger records share the path hash"))
            continue
        r = cands[0]
        if not same_file(p, r, skew):
            out.append(dict(e, send=False, why="not the ledger's file: its size or time differ"))
            continue
        t = when[key(p)]
        if t is None:
            out.append(dict(e, send=False, why="no clock: played before it was ever set"))
            continue
        e.update(send=True, why="ok", server=r["server"], filePath=r["serverPath"], startedAt=t,
                 playedMs=p["playedMs"], outcome=outcome_name(p["outcome"]), source=source_name(p["source"]),
                 pauseCount=p["pauseCount"])
        if p["durationMs"]:
            e["durationMs"] = p["durationMs"]
        out.append(e)
    return out


def next_mark(plays, resolved, posted, settled):
    """The posted mark per (deviceId hex, epoch) once the server has
    settled the ids in `settled` (accepted, duplicates and rejected alike):
    for each device and epoch, the seq up to which every play on the card
    is settled or isn't sent. A play still waiting (no answer yet, or for
    another server) holds it back; a seq missing from the card doesn't."""
    done = {e["id"] for e in resolved if not e["send"]} | set(settled)
    marks = dict(posted or {})
    groups = {}
    for p in plays:
        groups.setdefault((p["deviceId"], u64(p["epoch"])), []).append(p)
    for k, group in groups.items():
        m = marks.get(k, 0)
        for p in sorted(group, key=lambda q: q["seq"]):
            if p["seq"] <= m:
                continue
            if play_id(p) not in done:
                break
            m = p["seq"]
        if m:
            marks[k] = m
    return marks


# ---------------------------------------------------------------------------
# ACKS, MSMF's section of the plays the software has (2.5.6)
# ---------------------------------------------------------------------------
ACK = struct.Struct("<6sHQII")
ACK_STRIDE = 24


def acks_bytes(entries) -> bytes:
    """ACKS's rows from [(deviceId, epoch, seq)], in its order: by the
    deviceId's bytes, then the epoch as an unsigned integer, each pair once."""
    rows = sorted(((device_bytes(d), u64(e), s) for d, e, s in entries), key=lambda r: (r[0], r[1]))
    assert len({(d, e) for d, e, _ in rows}) == len(rows), "a (deviceId, epoch) twice"
    return b"".join(ACK.pack(d, 0, e, s, 0) for d, e, s in rows)


def read_acks(data: bytes, count: int, stride: int):
    """ACKS read back: [(deviceId hex, epoch, seq)], or None when it breaks
    2.5.6's rules (a reader then ignores the section, never the root)."""
    if stride < ACK_STRIDE or len(data) != count * stride:
        return None
    rows = []
    for i in range(count):
        d, _, e, s, _ = ACK.unpack_from(data, i * stride)
        rows.append((d, e, s))
    if any((rows[i][0], rows[i][1]) >= (rows[i + 1][0], rows[i + 1][1]) for i in range(len(rows) - 1)):
        return None
    return [(d.hex(), e, s) for d, e, s in rows]


def acknowledged(slot, acks) -> bool:
    a = {(d, e): s for d, e, s in acks or []}
    return slot["seq"] <= a.get((slot["deviceId"], slot["epoch"]), 0)


# ---------------------------------------------------------------------------
# The device's side (2.20.2, 2.20.4, 2.20.5): a model of the writer
# ---------------------------------------------------------------------------
def unread(data: bytes, acks) -> int:
    """What a rotation's step 1 counts in the file it removes (2.20.2): its
    valid play slots ACKS doesn't acknowledge, and the `dropped` of each
    drop note no acknowledged play follows in it (a note the software
    hasn't read: its count goes on into the next note). Saturating."""
    lost = pending = 0
    for _, why, s, _ in read_file(data):
        if why != "ok":
            continue
        if s["kind"] == KIND_DROP:
            pending += s["dropped"]
        elif s["kind"] == KIND_PLAY:
            if acknowledged(s, acks):
                pending = 0
            else:
                lost += 1
    return min(lost + pending, M32)


def trimmable(data: bytes, acks) -> bool:
    """2.20.2's trim, for one file's bytes: ACKS acknowledges every valid
    play slot, there is at least one, and a play comes after the last drop
    note (the software read each note with a play after it); no slot the
    device can't judge (a newer major's, a kind it doesn't know)."""
    entries = [(why, s) for _, why, s, _ in read_file(data)]
    if any(why in ("newer", "kind", "past") for why, _ in entries):
        return False
    valid = [s for why, s in entries if why == "ok"]
    plays = [s for s in valid if s["kind"] == KIND_PLAY]
    return bool(plays) and valid[-1]["kind"] == KIND_PLAY and all(acknowledged(s, acks) for s in plays)


class Device:
    """The firmware's play log, step by step, with the card's files in
    `files` ({name: bytes}, the log's and its twins') and each one's first
    cluster in `chains`: a cut inside a rename leaves two names on one
    chain, and the model asserts no delete ever frees a chain another name
    uses (2.17, item 6). NVS holds epoch, seq and the boot count; the
    epochs a fresh NVS makes come from `epochs` in order (the fixtures give
    them: the firmware draws them from its RNG)."""

    def __init__(self, device_id, epochs, cap=FILE_SLOTS):
        self.device_id = device_bytes(device_id).hex()
        self.epochs = list(epochs)
        self.cap = cap
        self.files = {}
        self.chains = {}            # {name: first cluster}; a file a test puts in `files` gets its own
        self.chain = 0
        self.nvs = None             # {"epoch", "seq", "boots"}; None: erased
        self.clock = 0
        self.down = False           # a power cut: nothing more until the next boot
        self.blocked = False        # no twin name free: no slot is written until a disk check frees one
        self.ops = []

    def plays_on_card(self):
        return read_log({n: b for n, b in self.files.items() if n in NAMES})["plays"]

    def erase_nvs(self):
        """The firmware's NVS recovery, or a flasher's erase."""
        self.nvs = None

    def disk_check(self):
        """A PC's disk check: cross-linked chains copied apart."""
        for name in sorted(self.files):
            self.chain += 1
            self.chains[name] = self.chain

    # The file system, as far as the cut-rename rule needs it.
    def _first(self, name):
        if name not in self.chains:
            self.chain += 1
            self.chains[name] = self.chain
        return self.chains[name]

    def _shared(self, name):
        c = self._first(name)
        return any(n != name and self._first(n) == c for n in self.files)

    def _create(self, name, data):
        self.chain += 1
        self.files[name], self.chains[name] = data, self.chain

    def _delete(self, name, why):
        assert not self._shared(name), "a delete would free a chain %s shares" % name
        del self.files[name]
        self.chains.pop(name, None)
        self.ops.append(("delete", name, why))

    def _rename(self, a, b):
        self._first(a)
        self.files[b], self.chains[b] = self.files.pop(a), self.chains.pop(a)

    def _to_twin(self, name):
        """name renamed to the first free twin name (a rename frees
        nothing); False, and blocked, with none free."""
        t = next((t for t in TWINS if t not in self.files), None)
        if t is None:
            self.blocked = True
            self.ops.append(("blocked", name))
            return False
        self._rename(name, t)
        self.ops.append(("twin", name, t))
        return True

    def _cut(self, where):
        self.down = True
        self.ops.append(("cut", where))
        return False

    def settle(self):
        """2.20.2 at a mount: plays.jnl and plays.1.jnl on one chain (a cut
        inside the rotation's rename) sends plays.jnl to a twin name, so the
        next play starts a new one; a twin no other name shares goes."""
        if "plays.jnl" in self.files and "plays.1.jnl" in self.files and \
                self._first("plays.jnl") == self._first("plays.1.jnl"):
            self._to_twin("plays.jnl")
        for t in TWINS:
            if t in self.files and not self._shared(t):
                self._delete(t, "twin")

    def boot(self, clock=0, acks=None):
        """A boot with this card: NVS (a new epoch when it has none), the
        settling of a cut rename, the boot rules (2.20.4); the trim by the
        root's ACKS."""
        self.down = self.blocked = False
        if self.nvs is None:
            self.nvs = {"epoch": self.epochs.pop(0), "seq": 0, "boots": 0}
            self.ops.append(("epoch", "%016x" % self.nvs["epoch"]))
        self.settle()
        slots = valid_slots(self.files)
        self.nvs["boots"] = boot_count(self.nvs["boots"], slots, self.device_id, self.nvs["epoch"])
        self.nvs["seq"] = boot_seq(self.nvs["seq"], slots, self.device_id, self.nvs["epoch"]) - 1
        self.clock = clock
        if acks:
            self.trim(acks)

    def set_clock(self):
        """A host's @time during this boot."""
        self.clock = 2

    def trim(self, acks):
        """2.20.2: a log file trimmable() says may go, and no twin shares
        (the device here deletes every such file)."""
        for name in sorted(n for n in self.files if n in NAMES):
            if trimmable(self.files[name], acks) and not self._shared(name):
                self._delete(name, "acknowledged")

    def append(self, raw, acks=None, cut=None):
        """One slot at plays.jnl's end, after the rotation when it would
        pass the cap. cut: where a power cut stops the rotation ("delete":
        after step 1; "rename": inside step 2's rename, its new entry
        written and its old one not yet removed; "renamed": after step 2).
        False when the slot isn't written."""
        cur = self.files.get("plays.jnl", b"")
        if len(cur) // SLOT + 1 > self.cap:
            # The rotation: plays.1.jnl goes (what it holds unread counted),
            # plays.jnl becomes plays.1.jnl, a new plays.jnl starts, with a
            # drop note first when plays were lost.
            lost = 0
            if "plays.1.jnl" in self.files:
                lost = unread(self.files["plays.1.jnl"], acks)
                if not self._shared("plays.1.jnl"):
                    self._delete("plays.1.jnl", "rotation")
                elif not self._to_twin("plays.1.jnl"):
                    return False
            if cut == "delete":
                return self._cut(cut)
            if cut == "rename":
                self.files["plays.1.jnl"], self.chains["plays.1.jnl"] = cur, self._first("plays.jnl")
                return self._cut(cut)
            self._rename("plays.jnl", "plays.1.jnl")
            self.ops.append(("rename", "plays.jnl", "plays.1.jnl"))
            if cut == "renamed":
                return self._cut(cut)
            note = b""
            if lost:
                note = encode({"kind": KIND_DROP, "deviceId": self.device_id, "epoch": self.nvs["epoch"],
                               "clock": self.clock, "startedAt": self._now, "bootSeq": self.nvs["boots"],
                               "uptimeMs": self._up, "dropped": lost})
                self.ops.append(("note", lost))
            self._create("plays.jnl", note + raw)
        elif "plays.jnl" in self.files:
            self.files["plays.jnl"] = cur + raw
        else:
            self._create("plays.jnl", raw)
        return True

    def play(self, path_hash, size, fat_time, generation, played_ms, duration_ms, outcome, source, uptime_ms,
             started_at=0, pause_count=0, acks=None, cut=None):
        """One play, written at its end: the seq taken (NVS first), the slot
        appended. started_at: the wall time it began, 0 while the clock is
        unset. cut: append()'s. The slot, or None when it is lost (a cut,
        or no twin name free)."""
        assert not self.down, "a power cut: the device boots first"
        if self.clock == 0:
            started_at = 0
        self.nvs["seq"] += 1
        self._now, self._up = started_at, uptime_ms
        slot = {"kind": KIND_PLAY, "outcome": outcome, "source": source, "seq": self.nvs["seq"],
                "pauseCount": min(pause_count, 0xFFFF), "clock": self.clock, "epoch": self.nvs["epoch"],
                "deviceId": self.device_id, "startedAt": started_at, "bootSeq": self.nvs["boots"],
                "uptimeMs": uptime_ms & M32, "playedMs": played_ms, "durationMs": duration_ms,
                "pathHash": path_hash, "size": size, "fatTime": fat_time, "generation": generation}
        if self.blocked or not self.append(encode(slot), acks, cut):
            self.ops.append(("lost", self.nvs["seq"]))
            return None
        self.ops.append(("play", self.nvs["seq"]))
        return slot


# ---------------------------------------------------------------------------
# The command line
# ---------------------------------------------------------------------------
def _named(paths):
    out = {}
    for p in paths:
        name = p.replace("\\", "/").rsplit("/", 1)[-1]
        with open(p, "rb") as fh:
            out[name if name in NAMES else p] = fh.read()
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description="The play log (docs/METADATA.md 2.20): the reference reader.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("dump", "read"):
        s = sub.add_parser(name)
        s.add_argument("files", nargs="+")
    a = ap.parse_args(argv)
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass
    if a.cmd == "dump":
        for p in a.files:
            with open(p, "rb") as fh:
                data = fh.read()
            for off, why, slot, _ in read_file(data):
                print(json.dumps({"file": p, "offset": off, "why": why, "slot": slot}))
        return 0
    log = read_log(_named(a.files))
    for p in log["plays"]:
        print(json.dumps(dict(p, id=play_id(p), outcomeName=outcome_name(p["outcome"]),
                              sourceName=source_name(p["source"]))))
    print(json.dumps({"counts": log["counts"]}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
