#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""The card contract's shared fixtures (docs/METADATA.md 2.17, 2.18).

Writes test/fixtures/card/: the conformance vectors (vectors.json), the
mood filters' vectors (moods.json, made with tools/moodfilter.py), the
library descriptions (libraries/*.json), their golden files (golden/*), the
hardening files (hardening/*.bin, listed in hardening/index.json), each
broken in one way under valid CRCs, the play log's (plays/, made with
tools/playlog.py) and the listening stats' (stats/, made with
tools/statsref.py). The player's host tests
(test/test_card_contract, test/test_card_files, test/test_card_autodj,
test/test_card_waves, test/test_name_key) read them, as the transfer
software's tests in mstream-terminal will.

This is a second implementation of the contract, written from the spec and
kept apart from lib/core/CardContract*: the goldens come from here, so the
C++ writers matching them byte for byte is two implementations agreeing,
not one agreeing with itself. Every vector of 2.18 is recomputed and
checked before anything is written.

    python tools/card_fixtures.py          write the fixtures, and delete every
                                           other file in the folder (a retired
                                           fixture; .gitattributes is kept)
    python tools/card_fixtures.py --check  fail if a file on disk differs, or if
                                           a file there isn't one it makes

The fixtures are made-up libraries (no real names) and synthetic
embeddings, under CC0 (test/fixtures/card/README.md; part 7, U18).
"""
import argparse
import email.utils
import fnmatch
import hashlib
import json
import re
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cardnames  # noqa: E402  (the card path rules: METADATA 2.8.3-2.8.6)
import moodfilter  # noqa: E402  (the mood tail and word: METADATA 2.6.4, 3.4.3)
import mpdj  # noqa: E402  (every MPDJ fixture: AUTODJ-INDEX.md 13)
import mpwf  # noqa: E402  (every MPWF fixture: WAVEFORM-INDEX.md 13)
import playlog  # noqa: E402  (the play log's fixtures: METADATA 2.20)
import statsref  # noqa: E402  (the listening stats' fixtures: docs/LISTENING-STATS.md)

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "test" / "fixtures" / "card"

# ---------------------------------------------------------------------------
# Conventions (2.3)
# ---------------------------------------------------------------------------
FNV_BASIS = 0xCBF29CE484222325
FNV_PRIME = 0x00000100000001B3
M64 = (1 << 64) - 1


def fnv(data: bytes, h: int = FNV_BASIS) -> int:
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & M64
    return h


def crc(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def path_hash(rel: str) -> int:
    """2.3.3: '/music', then '/' and the path relative to it."""
    return fnv(b"/music" + (b"/" + rel.encode("utf-8") if rel else b""))


def normalise_url(url: str) -> str:
    """2.5.1: scheme and host lowercased; no userinfo, query, fragment; an
    empty or default port dropped; the path without its trailing slashes."""
    scheme, rest = url.split("://", 1)
    scheme = scheme.lower()
    end = len(rest)
    for c in "/?#":
        k = rest.find(c)
        if k >= 0:
            end = min(end, k)
    auth, tail = rest[:end], rest[end:]
    auth = auth.rsplit("@", 1)[-1]
    host, port = auth, ""
    if not auth.endswith("]") and ":" in auth:
        host, port = auth.rsplit(":", 1)
    host = host.lower()
    if port:
        p = int(port)
        port = "" if (scheme == "http" and p == 80) or (scheme == "https" and p == 443) else ":%d" % p
    path = tail
    for c in "?#":
        k = path.find(c)
        if k >= 0:
            path = path[:k]
    path = path.rstrip("/")
    return "%s://%s%s%s" % (scheme, host, port, path)


def server_url_key(url: str) -> int:
    return fnv(normalise_url(url).encode("utf-8"))


def qfp(data: bytes) -> int:
    n = len(data)
    part = data[:min(n, 4096)]
    if n > 4096:
        part += data[max(4096, n - 4096):]
    return fnv(struct.pack("<Q", n) + part)


def fat_time(y, mo, d, h, mi, s) -> int:
    return ((y - 1980) << 9 | mo << 5 | d) << 16 | (h << 11 | mi << 5 | s // 2)


def days_in(y, m):
    leap = (y % 4 == 0 and y % 100 != 0) or y % 400 == 0
    return [31, 29 if leap else 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31][m - 1]


def fat_fields(t):
    date, time = t >> 16, t & 0xFFFF
    return 1980 + (date >> 9), (date >> 5) & 15, date & 31, time >> 11, (time >> 5) & 63, time & 31


def fat_wall(t):
    """W of a valid non-zero stamp, else None."""
    if t == 0:
        return None
    y, mo, d, h, mi, s2 = fat_fields(t)
    if not (1 <= mo <= 12 and 1 <= d <= days_in(y, mo) and h < 24 and mi < 60 and s2 < 30):
        return None
    days = sum(366 if days_in(yy, 2) == 29 else 365 for yy in range(1980, y))
    days += sum(days_in(y, m) for m in range(1, mo)) + d - 1
    return days * 86400 + h * 3600 + mi * 60 + 2 * s2


def fat_from_wall(w):
    days, rem = divmod(w, 86400)
    y = 1980
    while days >= (366 if days_in(y, 2) == 29 else 365):
        days -= 366 if days_in(y, 2) == 29 else 365
        y += 1
    mo = 1
    while days >= days_in(y, mo):
        days -= days_in(y, mo)
        mo += 1
    return fat_time(y, mo, days + 1, rem // 3600, rem // 60 % 60, rem % 60)


def skew(pairs):
    """2.3.4's rule over (recorded, observed) pairs whose sizes match."""
    deltas = []
    for r, o in pairs:
        wr, wo = fat_wall(r), fat_wall(o)
        if wr is None or wo is None:
            continue
        deltas.append(wo - wr)
    counts = {}
    for d in deltas:
        if d:
            counts[d] = counts.get(d, 0) + 1
    if not counts:
        return 0
    best = sorted(counts, key=lambda d: (-counts[d], abs(d), d))[0]
    c = counts[best]
    if c < 8 or c * 2 < len(deltas) or abs(best) > 86400 or best % 900:
        return 0
    return best


def time_matches(r, o, d):
    wr, wo = fat_wall(r), fat_wall(o)
    if wr is None or wo is None:
        return False
    return wo - wr == 0 or (d != 0 and wo - wr == d)


# The number rule (5.3).
ASCII_WS = " \t\n\f\r"


def number(text, scale, gain=False):
    s = text.strip(ASCII_WS)
    if gain and len(s) >= 2 and s[-2:].lower() == "db":
        s = s[:-2].strip(ASCII_WS)
    sign = 1
    if s[:1] in "+-" and s:
        sign = -1 if s[0] == "-" else 1
        s = s[1:]
    if "." in s:
        ip, fp = s.split(".", 1)
    else:
        ip, fp = s, ""
    if not (ip + fp) or not all(c in "0123456789" for c in ip + fp):
        return None
    if not ip and not fp:
        return None
    digits = fp[:scale].ljust(scale, "0")
    mag = int(ip or "0") * 10 ** scale + int(digits or "0")
    if len(fp) > scale and fp[scale] >= "5":
        mag += 1
    return sign, mag


def bpm10(text):
    r = number(text, 0)
    if r is None:
        return None
    sign, m = r
    if sign < 0 and m:
        return None
    return m * 10 if 20 <= m <= 300 else None


def gain(text):
    r = number(text, 2, gain=True)
    if r is None:
        return None
    v = r[0] * r[1]
    return v if -32768 <= v <= 32767 else None


def peak(text):
    r = number(text, 4)
    if r is None:
        return None
    sign, m = r
    if sign < 0 and m:
        return None
    return min(m, 65535)


def r128(q):
    x = q + 1280
    m = (abs(x) * 25 + 32) // 64
    return -m if x < 0 else m


# Text (2.3.6).
def cut255(b: bytes, limit=255) -> bytes:
    if len(b) <= limit:
        return b
    k = limit
    while k > 0 and (b[k] & 0xC0) == 0x80:
        k -= 1
    return b[:k]


def build_field(values, is_list):
    """2.3.6's steps over one field's values: (bytes, truncated)."""
    out, total, truncated = [], 0, False
    for v in values:
        b = v.encode("utf-8") if isinstance(v, str) else bytes(v)
        if not b:
            continue
        b = bytes(0x20 if (c < 0x20 or c == 0x7F) else c for c in b)
        kept = cut255(b)
        if len(kept) < len(b):
            truncated = True
        if not kept:
            continue
        if not is_list:
            return kept, truncated
        if kept in out:
            continue
        grown = total + (1 if out else 0) + len(kept)
        if len(out) + 1 > 16 or grown > 1023:
            truncated = True
            break
        out.append(kept)
        total = grown
    return b"\x1f".join(out), truncated


FIELDS = ["title", "artist", "album", "albumArtist", "genre", "composer", "titleSort", "artistSort",
          "albumSort", "albumArtistSort", "mbAlbumId", "mbRecordingId"]
LIST_FIELDS = {"artist", "albumArtist", "genre", "composer"}


def encode_run(values: list) -> bytes:
    last = max((i for i, v in enumerate(values) if v), default=-1)
    if last < 0:
        return b""
    return bytes([last + 1]) + b"".join(v + b"\0" for v in values[:last + 1])


# Names and the canonical order (2.6.8).
def comps(rel: str):
    return [c.encode("utf-8") for c in rel.split("/")] if rel else []


def folder_key(rel: str):
    return comps(rel)


def file_key(rel: str):
    c = comps(rel)
    return [(1, x) for x in c[:-1]] + [(0, c[-1])]


def album_folder(rel: str, roots=()):
    root = ""
    for r in roots:
        if rel.startswith(r + "/") and len(r) > len(root):
            root = r
    below = rel[len(root) + 1:] if root else rel
    parts = below.split("/")
    folders = parts[:-1]
    if len(folders) <= 1:
        return rel.rsplit("/", 1)[0] if "/" in rel else ""
    return (root + "/" if root else "") + "/".join(folders[:2])


# ---------------------------------------------------------------------------
# The container (2.4)
# ---------------------------------------------------------------------------
REQUIRED = 1


def fourcc(s: str) -> bytes:
    return s.encode("ascii")


def build_container(magic, generation, card_id, type_header: bytes, sections, major=1, minor=0, hardening=False):
    """sections: [(type, flags, count, stride, data)] in order. `hardening`:
    a header of any length (its fixtures break that rule on purpose)."""
    header_bytes = 40 + len(type_header)
    assert hardening or header_bytes % 8 == 0
    n = len(sections)
    at = header_bytes + 32 * n
    offsets = []
    for s in sections:
        at = (at + 7) & ~7
        offsets.append(at)
        at += len(s[4])
    out = bytearray(at)
    for s, o in zip(sections, offsets):
        out[o:o + len(s[4])] = s[4]
    for i, (s, o) in enumerate(zip(sections, offsets)):
        struct.pack_into("<4sIIIIIII", out, header_bytes + 32 * i, fourcc(s[0]) if isinstance(s[0], str) else s[0],
                         s[1], o, len(s[4]), s[2], s[3], crc(bytes(s[4])), 0)
    struct.pack_into("<4sHHIIIIQII", out, 0, fourcc(magic), major, minor, header_bytes, n, at, generation, card_id, 0, 0)
    out[40:header_bytes] = type_header
    struct.pack_into("<I", out, 32, crc(bytes(out[:header_bytes + 32 * n])))
    return bytes(out)


def parse_container(b: bytes):
    magic, major, minor, hb, n, fb, gen, card = struct.unpack_from("<4sHHIIIIQ", b, 0)
    secs = []
    for i in range(n):
        t, fl, off, ln, cnt, st, _c, _r = struct.unpack_from("<4sIIIIIII", b, hb + 32 * i)
        secs.append([t.decode("ascii"), fl, cnt, st, bytearray(b[off:off + ln])])
    return dict(magic=magic.decode("ascii"), major=major, minor=minor, generation=gen, card=card,
                th=bytearray(b[40:hb]), sections=secs)


def rebuild(p):
    return build_container(p["magic"], p["generation"], p["card"], bytes(p["th"]),
                           [tuple(s) for s in p["sections"]], p["major"], p["minor"])


def section(p, t):
    for s in p["sections"]:
        if s[0] == t:
            return s
    raise KeyError(t)


class Strs:
    def __init__(self):
        self.b = bytearray(b"\0")

    def add(self, s) -> int:
        if isinstance(s, str):
            s = s.encode("utf-8")
        if not s:
            return 0
        off = len(self.b)
        self.b += s + b"\0"
        return off

    def raw(self, b: bytes) -> int:
        off = len(self.b)
        self.b += b
        return off


def reseal(b: bytearray) -> bytes:
    """Every section's CRC (where its directory entry lies inside the file)
    and the header's made right again, after a raw edit of the frame."""
    hb, n = struct.unpack_from("<II", b, 8)
    for i in range(n):
        off, ln = struct.unpack_from("<II", b, hb + 32 * i + 8)
        if off + ln <= len(b):
            struct.pack_into("<I", b, hb + 32 * i + 24, crc(bytes(b[off:off + ln])))
    struct.pack_into("<I", b, 32, 0)
    struct.pack_into("<I", b, 32, crc(bytes(b[:hb + 32 * n])))
    return bytes(b)


def u64(v):
    return int(v, 0) if isinstance(v, str) else int(v)


# ---------------------------------------------------------------------------
# MPTG (2.6)
# ---------------------------------------------------------------------------
FOLDER_OWNED, FOLDER_THUMB = 1, 2
TRUNCATED = 1 << 7


def parent_of(p):
    return p.rsplit("/", 1)[0] if "/" in p else ""


def last_name(p):
    return p.rsplit("/", 1)[-1]


def mptg(desc, order="canonical", rec_stride=None, extra_header=b"", extra_run_field=None, extra_sections=()):
    """A tags file from a library description (and the knobs the hardening
    files need).

    The mood tail (2.6.4) is written only for a description whose records
    carry "scores": the stride is then 80 unless given, a record whose
    scores are null (no server row) gets 8 zero bytes, and the minor is 1
    unless given. Without scores the stride is 72 unless given. Bytes past
    the tail (or past 72, with no scores) are 0xAB, a newer minor's."""
    recs = desc.get("records", [])
    moods = any("scores" in r for r in recs)
    # A writer writes a tail only when a record has scores (2.6.4).
    assert not moods or any(r.get("scores") is not None for r in recs), desc.get("file")
    if rec_stride is None:
        rec_stride = moodfilter.TAIL_STRIDE if moods else 72
    assert rec_stride >= (moodfilter.TAIL_STRIDE if moods else 72), rec_stride
    folders = {"": 0}
    for r in recs:
        a = parent_of(r["path"])
        while a:
            folders.setdefault(a, 0)
            a = parent_of(a)
    for f in desc.get("folders", []):
        assert f["path"] in folders or f.get("flags", 0) & FOLDER_OWNED, f
    for f in desc.get("folders", []):
        folders[f["path"]] = folders.get(f["path"], 0) | f.get("flags", 0)
        a = parent_of(f["path"])
        while a:
            folders.setdefault(a, 0)
            a = parent_of(a)
    fpaths = sorted(folders, key=folder_key)
    findex = {p: i for i, p in enumerate(fpaths)}
    if order == "canonical":
        recs = sorted(recs, key=lambda r: (findex[parent_of(r["path"])], last_name(r["path"]).encode("utf-8")))
    elif order == "path-string":
        recs = sorted(recs, key=lambda r: r["path"].encode("utf-8"))
    strs = Strs()
    producer = strs.add(desc.get("producer", ""))
    fold = bytearray()
    first = [0] * (len(fpaths) + 1)
    for r in recs:
        first[findex[parent_of(r["path"])] + 1] += 1
    for i in range(1, len(first)):
        first[i] += first[i - 1]
    if order != "canonical":
        # firstRecord stays "the records whose folder index is lower".
        pass
    for i, p in enumerate(fpaths):
        parent = 0xFFFFFFFF if i == 0 else findex[parent_of(p)]
        name = 0 if i == 0 else strs.add(last_name(p))
        fold += struct.pack("<IIII", parent, name, folders[p], first[i])
    rows, hidx, albums, artists = bytearray(), [], set(), set()
    for k, r in enumerate(recs):
        name = strs.add(last_name(r["path"]))
        vals, trunc = [], False
        tags = r.get("tags", {})
        for f in FIELDS:
            v, t = build_field(tags.get(f, []), f in LIST_FIELDS)
            vals.append(v)
            trunc = trunc or t
        if extra_run_field is not None and any(vals):
            vals.append(extra_run_field.encode("utf-8"))
        run = encode_run(vals)
        strings = strs.raw(run) if run else 0
        flags = r.get("flags", 0) | (TRUNCATED if trunc else 0)
        if vals[2]:
            albums.add(vals[2])
        for f in (1, 3):
            if vals[f]:
                artists.update(vals[f].split(b"\x1f"))
        pic = r.get("picture", {})
        row = struct.pack("<IIIIIIQIHHHHHHHhhHHBBIIBBBB",
                          findex[parent_of(r["path"])], name, strings, r.get("size", 0), u64(r.get("fatTime", 0)),
                          r.get("durationMs", 0), u64(r.get("qfp", 0)), u64(r.get("known", 0)), flags,
                          r.get("year", 0), r.get("track", 0), r.get("trackTotal", 0), r.get("disc", 0),
                          r.get("discTotal", 0), r.get("bpm10", 0), r.get("rgTrackGain", 0), r.get("rgAlbumGain", 0),
                          r.get("rgTrackPeak", 0), r.get("rgAlbumPeak", 0), r.get("container", 0), r.get("camelot", 0),
                          pic.get("offset", 0), pic.get("length", 0), pic.get("type", 0), pic.get("mime", 0),
                          pic.get("coding", 0), 0)
        assert len(row) == 72
        tail = moodfilter.tail_of(r.get("scores")) if moods else b""
        rows += row + tail + bytes([0xAB]) * (rec_stride - 72 - len(tail))
        hidx.append((path_hash(r["path"]), k))
    hidx.sort()
    sections = [("FOLD", REQUIRED, len(fpaths), 16, bytes(fold)),
                ("RECS", REQUIRED, len(recs), rec_stride, bytes(rows))]
    sections.append(("STRS", REQUIRED, 0, 0, bytes(strs.b)))
    if recs:
        sections.append(("HIDX", 0, len(recs), 12, b"".join(struct.pack("<QI", h, k) for h, k in hidx)))
    if desc["source"] in (2, 3) and recs:
        ostr = Strs()
        orig = bytearray()
        for r in recs:
            l = r.get("ledger", {})
            # Bytes 74-75: the row's server (2.6.6, 2.5.5), 0 the root's own.
            orig += struct.pack("<IIII16s16sQQIHBBHH4s", ostr.add(l.get("serverPath", "")), l.get("mstreamId", 0),
                                l.get("albumId", 0), l.get("artistId", 0), bytes.fromhex(l.get("audioHash", "00" * 16)),
                                bytes.fromhex(l.get("fileHash", "00" * 16)), l.get("serverModified", 0),
                                l.get("serverSize", 0), l.get("createdAt", 0), l.get("hashV", 0),
                                l.get("originFlags", 0), l.get("convertedTo", 0), l.get("convertKbps", 0),
                                l.get("server", 0), bytes(4))
        sections.append(("ORIG", 0, len(recs), 80, bytes(orig)))
        sections.append(("OSTR", 0, 0, 0, bytes(ostr.b)))
    sections += list(extra_sections)
    th = struct.pack("<BBHHHIIIIII", desc["source"], 0, desc.get("parserVersion", 0), desc.get("readRules", 1), 0,
                     len(recs), len(fpaths), len(albums), len(artists), producer, 0) + extra_header
    minor = desc.get("minor", 1 if moods else 0)
    assert not moods or minor >= 1, desc.get("file")
    return build_container("MPTG", desc["generation"], u64(desc.get("cardId", 0)), th, sections, minor=minor)


# ---------------------------------------------------------------------------
# MSMF (2.5), MSPD (2.12.5), MPTH (2.14.1), device.txt (2.15); MPDJ is
# tools/mpdj.py's (docs/AUTODJ-INDEX.md), MPWF tools/mpwf.py's
# (docs/WAVEFORM-INDEX.md)
# ---------------------------------------------------------------------------
COMPANION_STEMS = {"MPTG": "tags", "MPDJ": "autodj", "MPWF": "waves"}


def companion_name(kind, gen):
    return "%s-%08x.bin" % (COMPANION_STEMS[kind], gen)


PEER = 1  # SRVS flags bit 0 (2.5.5)


def uuid_bytes(s) -> bytes:
    """A UUID's 16 bytes in RFC 4122 order (2.3.1); absent: zeros, unknown."""
    return bytes.fromhex((s or "0" * 32).replace("-", ""))


def server_entry(s) -> dict:
    """One SRVS entry (2.5.5) from a description: the keys from the URLs
    given (2.5.1), 0 where none is."""
    return {"instance": uuid_bytes(s.get("serverInstance")),
            "serverUrlKey": server_url_key(s["serverUrl"]) if s.get("serverUrl") else 0,
            "parentKey": server_url_key(s["parentUrl"]) if s.get("parentUrl") else 0,
            "peerId": s.get("peerId", 0), "flags": s.get("flags", 0)}


def root_server(root) -> dict:
    """Server 0 of a root description (2.5.1): the header's, as an entry."""
    key = server_url_key(root["serverUrl"]) if root.get("serverUrl") else u64(root.get("serverUrlKey", 0))
    return {"instance": uuid_bytes(root.get("serverInstance")), "serverUrlKey": key, "parentKey": 0, "peerId": 0,
            "flags": 0}


def srvs_breaks(root: dict, entries) -> set:
    """The servers (i >= 1, entry i - 1) whose SRVS entry breaks a writer
    rule of 2.5.5, given server 0 (root_server). A reserved flag bit is a
    newer minor's (2.4.5), not a broken rule."""
    zero, out = bytes(16), set()
    for i, e in enumerate(entries, 1):
        if e["flags"] & PEER:
            fields = e["serverUrlKey"] == 0 and e["parentKey"] != 0 and e["peerId"] != 0
        else:
            fields = e["serverUrlKey"] != 0 and e["parentKey"] == 0 and e["peerId"] == 0
        roots = (e["instance"] != zero and e["instance"] == root["instance"]) or \
                (e["serverUrlKey"] != 0 and e["serverUrlKey"] == root["serverUrlKey"])
        if not fields or roots:
            out.add(i)
    # Each server once: both of two entries that name one server are broken.
    for name in ("instance", "serverUrlKey", "peer"):
        seen = {}
        for i, e in enumerate(entries, 1):
            v = (e["parentKey"], e["peerId"]) if name == "peer" else e[name]
            if (name == "peer" and not e["flags"] & PEER) or v in (zero, 0):
                continue
            seen.setdefault(v, []).append(i)
        for same in seen.values():
            if len(same) > 1:
                out.update(same)
    return out


def srvs_row(e: dict, stride=40) -> bytes:
    row = struct.pack("<16sQQII", e["instance"], e["serverUrlKey"], e["parentKey"], e["peerId"], e["flags"])
    return row + bytes([0xAB]) * (stride - 40)


def msmf_raw(desc, comps_rows, roots, minor=0, servers=(), srvs_stride=40, extra_sections=(), acks=None):
    """comps_rows: [(kind, generation, fileBytes, headerCrc, name, key)] as given;
    servers: SRVS's entries (server_entry), in index order (2.5.5); acks:
    ACKS's bytes as given (2.5.6), or None."""
    strs = Strs()
    producer = strs.add(desc.get("producer", ""))
    revision = strs.add(desc.get("serverRevision", ""))
    comp = bytearray()
    for kind, gen, fb, hc, name, key in comps_rows:
        comp += struct.pack("<4sIIIII16s", fourcc(kind), gen, fb, hc, strs.add(name), 0, key)
    libr = b"".join(struct.pack("<I", strs.add(r)) for r in roots)
    server = root_server(desc)
    th = struct.pack("<IIQ16sIIIIQ", desc.get("commitTime", 0), desc.get("flags", 0), u64(desc.get("commitId", 0)),
                     server["instance"], producer, revision, desc.get("baseGeneration", 0), 0, server["serverUrlKey"])
    sections = [("COMP", REQUIRED, len(comps_rows), 40, bytes(comp))]
    if roots:
        sections.append(("LIBR", 0, len(roots), 4, libr))
    sections.append(("STRS", REQUIRED, 0, 0, bytes(strs.b)))
    if servers:
        # Optional, never REQUIRED: the device doesn't know it (2.5.5).
        sections.append(("SRVS", 0, len(servers), srvs_stride, b"".join(srvs_row(e, srvs_stride) for e in servers)))
    if acks is not None:
        # Optional, never REQUIRED, after SRVS (2.5.3, 2.5.6).
        data, stride = acks if isinstance(acks, tuple) else (acks, playlog.ACK_STRIDE)
        sections.append(("ACKS", 0, len(data) // stride, stride, data))
    sections += list(extra_sections)
    return build_container("MSMF", desc["generation"], u64(desc.get("cardId", 0)), th, sections, minor=minor)


def comp_row(name: str, data: bytes):
    """A COMP entry for the companion file `name` holding `data` (2.5.3)."""
    p = parse_container(data)
    key = bytes(p["th"][24:40]) if p["magic"] == "MPDJ" else bytes(16)  # MPDJ: selectionSig, bytes 64-79
    assert name == companion_name(p["magic"], p["generation"]), name
    return p["magic"], p["generation"], len(data), struct.unpack_from("<I", data, 32)[0], name, key


def msmf(desc, golden: dict):
    rows = [comp_row(c["file"], golden[c["file"]]) for c in desc["companions"]]
    roots = sorted(desc.get("roots", []), key=lambda r: r.encode("utf-8"))
    servers = [server_entry(s) for s in desc.get("servers", [])]
    # 2.5.5's writer rules, which a description must keep (msmf_raw writes
    # whatever it is given, for the hardening files): each server once,
    # each entry's fields as its kind says, only the flag bits v1 defines.
    broken = srvs_breaks(root_server(desc), servers)
    assert not broken and all(e["flags"] in (0, PEER) for e in servers), (desc["file"], sorted(broken))
    # And with the tags file it names: SRVS lists exactly the servers its
    # rows name besides the root's, and every row names 0 or an entry.
    tags = golden[desc["companions"][0]["file"]]
    has_orig = any(s[0] == "ORIG" for s in parse_container(tags)["sections"])
    used = {server for _, server in ledger_servers(tags)} if has_orig else set()
    assert used - {0} == set(range(1, len(servers) + 1)), (desc["file"], sorted(used), len(servers))
    acks = None
    if desc.get("acks"):
        acks = playlog.acks_bytes([(a["deviceId"], a["epoch"], a["seq"]) for a in desc["acks"]])
    return msmf_raw(desc, rows, roots, desc.get("minor", 0), servers, acks=acks)


OPS = {"write": 1, "delete": 2, "folder": 3}
RANK = {2: 0, 3: 1, 1: 2}


def mspd_raw(desc, ops):
    """ops: [(op, path, expectedSize)] as given."""
    strs = Strs()
    pend = b"".join(struct.pack("<BBHIII", op, 0, 0, strs.add(path), size, 0) for op, path, size in ops)
    th = struct.pack("<QII", u64(desc.get("runId", 0)), desc.get("baseGeneration", 0), 0)
    return build_container("MSPD", desc["generation"], u64(desc.get("cardId", 0)), th,
                           [("PEND", REQUIRED, len(ops), 16, pend), ("STRS", REQUIRED, 0, 0, bytes(strs.b))])


def mspd(desc):
    ops = []
    for o in desc["ops"]:
        op = OPS[o["op"]]
        ops.append((op, o["path"], o.get("expectedSize", 0) if op == 1 else 0))
    ops.sort(key=lambda e: (RANK[e[0]], folder_key(e[1]) if e[0] == 3 else file_key(e[1])))
    return mspd_raw(desc, ops)


def rgb565be(r, g, b):
    v = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)
    return struct.pack(">H", v)


def thumb_pixels(size):
    """2.17's synthetic picture: r along x, g along y, b along the diagonal."""
    out = bytearray()
    for y in range(size):
        for x in range(size):
            out += rgb565be(x * 255 // (size - 1), y * 255 // (size - 1), (x + y) * 255 // (2 * size - 2))
    return bytes(out)


def mpth(desc):
    h = path_hash(desc["folder"])
    head = struct.pack("<4sHBBQII", b"MPTH", 1, 0, 0, h, desc.get("sourceBytes", 0), 40 | 96 << 16)
    return head + thumb_pixels(40) + thumb_pixels(96)


def device_txt(desc):
    keys = ["contract", "firmware", "read.msmf", "read.mptg", "read.mpdj", "read.mpwf", "read.mpth", "codecs",
            "extensions", "max_rate", "max_channels", "history"]
    return "".join("%s=%s\n" % (k, desc[k]) for k in keys if k in desc).encode("ascii")


# ---------------------------------------------------------------------------
# The software's private files (2.12.7) and the resumable partials (2.12.8)
# ---------------------------------------------------------------------------
STATE_FORMAT, RESUME_FORMAT, RUN_FORMAT = "mstream-card-state", "mstream-card-resume", "mstream-card-run"
PRIVATE_VERSION = 1                       # the version of the members this implementation knows
MAX_SEQ = (1 << 53) - 1
RESERVED = {"state.json": "reserved: the private state", "state.new": "reserved: its next write",
            "resume.json": "reserved: the partials kept", "resume.new": "reserved: its next write",
            "run.lock": "reserved: a run is writing the card"}
COMPANION_NAME = re.compile(r"^(tags|autodj|waves)-[0-9a-f]{8}\.bin$")  # 2.4.3's COMP names
PART_NAME = re.compile(r"^[0-9a-f]{8}\.part$")


def private_name(rel: str):
    """2.12.7: may the software keep a private file at /.mstream/<rel> ('/'
    between names)? (ok, why)."""
    if rel != rel.lower():
        return False, "not lowercase"
    parts = rel.split("/")
    if parts[0] == "resume" and len(parts) > 1:
        ok = len(parts) == 2 and PART_NAME.match(parts[1]) is not None
        return ok, "reserved: a partial (2.12.8)" if ok else "resume/ holds only 2.12.8's partials"
    if parts[0] in ("stage", "thumbs") and len(parts) > 1:
        return False, "inside %s/" % parts[0]
    if len(parts) == 1 and rel in ("stage", "thumbs", "resume"):
        return False, "a folder's name (2.2, 2.12.8)"
    name = parts[-1]
    if name.endswith(".tmp"):
        return False, "*.tmp: step 4 deletes it"
    if len(parts) == 1 and any(fnmatch.fnmatchcase(name, "%s-*.bin" % s) for s in COMPANION_STEMS.values()):
        return False, "a companion's name"
    if len(parts) == 1 and (name.startswith("manifest.") or name.startswith("pending.")):
        return False, "a contract file's name"
    return True, RESERVED.get(rel, "the software's own")


def _is_int(x):
    return isinstance(x, int) and not isinstance(x, bool)


def private_parse(data, fmt):
    """2.12.7's form: ("valid", obj), ("version", obj) for a version this
    software doesn't know (it stops: 2.4.5), ("invalid", None) or
    ("missing", None). `data`: the file's bytes or text, None: no file."""
    if data is None:
        return "missing", None
    raw = data.encode("utf-8") if isinstance(data, str) else bytes(data)
    if raw.startswith(b"\xef\xbb\xbf"):
        return "invalid", None

    def pairs(kv):
        names = [k for k, _ in kv]
        if len(set(names)) != len(names):
            raise ValueError("a member named twice")
        return dict(kv)

    def no_constant(c):
        raise ValueError(c)
    try:
        obj = json.loads(raw.decode("utf-8"), object_pairs_hook=pairs, parse_constant=no_constant)
    except (UnicodeDecodeError, ValueError):
        return "invalid", None
    if not isinstance(obj, dict) or obj.get("format") != fmt:
        return "invalid", None
    version, seq = obj.get("version"), obj.get("seq")
    if not _is_int(version) or version < 1:
        return "invalid", None
    if version != PRIVATE_VERSION:
        return "version", obj
    if not _is_int(seq) or not 1 <= seq <= MAX_SEQ:
        return "invalid", None
    return "valid", obj


def settle_private(stem, json_data, new_data):
    """2.12.7's read with its recovery, for X = stem ("state", "resume"):
    which file counts and what a run then does."""
    fmt = {"state": STATE_FORMAT, "resume": RESUME_FORMAT}[stem]
    a, b = private_parse(json_data, fmt), private_parse(new_data, fmt)
    out = {"pick": "none", "seq": None, "nextSeq": 1, "delete": [], "rename": False, "diskCheck": False,
           "stops": False}
    if "version" in (a[0], b[0]):
        return dict(out, stops=True)

    def raw(d):
        return d.encode("utf-8") if isinstance(d, str) else bytes(d)
    if json_data is not None and new_data is not None and raw(json_data) == raw(new_data):
        # One write seen twice: the rename cut between its two directory
        # writes. Either is used, neither deleted (2.12.1, "Cut renames").
        out.update(diskCheck=True)
        if a[0] == "valid":
            out.update(pick=stem + ".json", seq=a[1]["seq"], nextSeq=a[1]["seq"] + 1)
        return out
    if a[0] == "valid" and (b[0] != "valid" or a[1]["seq"] >= b[1]["seq"]):
        out.update(pick=stem + ".json", seq=a[1]["seq"], nextSeq=a[1]["seq"] + 1,
                   delete=[stem + ".new"] if new_data is not None else [])
    elif b[0] == "valid":
        out.update(pick=stem + ".new", seq=b[1]["seq"], nextSeq=b[1]["seq"] + 1,
                   delete=[stem + ".json"] if json_data is not None else [], rename=True)
    return out


def resume_parts(obj):
    """The partials a valid resume.json keeps: {name: size}. An entry that
    isn't one (a bad name, a size that isn't a count) keeps nothing; a name
    listed twice keeps nothing."""
    seen, out = set(), {}
    for p in obj.get("parts", []) if isinstance(obj.get("parts"), list) else []:
        ok = isinstance(p, dict) and isinstance(p.get("name"), str) and PART_NAME.match(p["name"]) is not None and \
            _is_int(p.get("size")) and p["size"] >= 0
        if not ok:
            continue
        if p["name"] in seen:
            out.pop(p["name"], None)
            continue
        seen.add(p["name"])
        out[p["name"]] = p["size"]
    return out


def cleanup(files, root_names, resume_json):
    """2.12.1, step 4, over /.mstream after steps 1-3: `files` {rel path:
    size}, `root_names` the companions the root names, `resume_json` the
    resume.json step 2 settled (text; None: none). The sorted paths it
    deletes; every other file stays, the private files among them."""
    st, obj = private_parse(resume_json, RESUME_FORMAT)
    keep = resume_parts(obj) if st == "valid" else {}
    named = {n.lower() for n in root_names}
    gone = []
    for rel, size in files.items():
        parts = rel.split("/")
        top, low = parts[0].lower(), rel.lower()
        if len(parts) > 1:
            if top == "stage":
                gone.append(rel)
            elif top == "thumbs" and low.endswith(".tmp"):
                gone.append(rel)
            elif top == "resume":
                name = parts[1].lower()
                if len(parts) > 2 or name not in keep or size > keep[name]:
                    gone.append(rel)
        elif low.endswith(".tmp") or (COMPANION_NAME.match(low) and low not in named):
            gone.append(rel)
    return sorted(gone, key=lambda r: r.encode("utf-8"))


def http_date(ms):
    """An If-Range date (2.12.8): RFC 9110's IMF-fixdate of a time in epoch
    ms, its seconds floored."""
    return email.utils.formatdate(ms // 1000, usegmt=True)


def resume_fetch(entry, part_size, server, status=None):
    """2.12.8 at fetch time. entry: resume.json's; part_size: the partial's
    size on the card; server: the server's entry for the file now
    {"serverPath", "file-size", "modified"}; status: the answer to the
    ranged GET, when there is one. -> {"from": the byte it goes on from, or
    None (start over), "headers": the request's, "then": what the answer
    does}. An entry whose `have` isn't an integer from 0 to `size`, or
    whose `etag` isn't a string, starts over (step 4 kept its file all the
    same: it looks at names and sizes only)."""
    sound = (_is_int(entry.get("have")) and _is_int(entry.get("size")) and 0 <= entry["have"] <= entry["size"] and
             isinstance(entry.get("etag"), str))
    same = sound and (server.get("serverPath") == entry["serverPath"] and server.get("file-size") == entry["size"]
                      and server.get("modified") == entry["modified"] and part_size <= entry["size"])
    if not same:
        return {"from": None, "headers": {}, "then": "start over"}
    at = min(part_size, entry["have"])
    validator = entry["etag"] if entry.get("etag") else http_date(entry["modified"])
    out = {"from": at, "headers": {"Range": "bytes=%d-" % at, "If-Range": validator}}
    if status is not None:
        out["then"] = "continue" if status == 206 else "start over"
    return out


# ---------------------------------------------------------------------------
# The library descriptions (made-up names only)
# ---------------------------------------------------------------------------
CARD_ID = "0x5EEDC0DE0A1B2C3D"
NFC_CAFE = "Caf\u00e9"
NFD_CAFE = "Cafe\u0301"
KNOWN_RULES1 = 0x1FFFF
RG = (1 << 2) | (1 << 3)


def h16(seed: str) -> str:
    """A made-up 16-byte digest, as hex."""
    return hashlib.md5(seed.encode("utf-8")).hexdigest()


def tags_transfer():
    t = fat_time(2026, 10, 7, 14, 30, 42)
    rec = []

    def add(path, **kw):
        r = {"path": path, "size": 1000 + 37 * len(rec), "fatTime": "0x%08X" % (t + 2 * len(rec)),
             "durationMs": 180000 + 1000 * len(rec), "qfp": "0x%016X" % fnv(path.encode("utf-8")),
             "known": KNOWN_RULES1, "container": 1}
        r.update(kw)
        r.setdefault("ledger", {"serverPath": "Music/" + path, "mstreamId": 100 + len(rec), "albumId": 7,
                                "artistId": 3, "audioHash": h16("a" + path), "fileHash": h16("f" + path),
                                "serverModified": 1790000000000 + len(rec), "serverSize": r["size"],
                                "createdAt": 1790000000, "hashV": 2, "originFlags": 2})
        rec.append(r)

    add("Artist/Album/01 - Title.mp3", note="every field; a list artist; ReplayGain; a JPEG front cover",
        tags={"title": ["Title"], "artist": ["Lantern Choir", "Guest Voice"], "album": ["Album"],
              "albumArtist": ["Lantern Choir"], "genre": ["Ambient", "Electronic"], "composer": ["Pale Ferns"],
              "titleSort": ["Title"], "artistSort": ["Choir, Lantern"], "albumSort": ["Album"],
              "albumArtistSort": ["Choir, Lantern"], "mbAlbumId": ["7a1c0d3e-1111-4222-8333-944455556666"],
              "mbRecordingId": ["0b2c3d4e-5555-4666-8777-988899990000"]},
        year=2019, track=1, trackTotal=10, disc=1, discTotal=2, bpm10=1210, camelot=8,
        rgTrackGain=-679, rgAlbumGain=-650, rgTrackPeak=9886, rgAlbumPeak=9990, flags=RG | 2,
        picture={"offset": 1024, "length": 50000, "type": 3, "mime": 1, "coding": 0})
    add("Artist/Album/02 - Other.mp3", note="repeats dropped; a TAB to a space; an ID3 unsynchronised picture",
        tags={"title": ["A\tB"], "artist": ["X", "X", "Y"], "album": ["Album"], "genre": ["Rock", "", "Rock"]},
        year=2019, track=2, trackTotal=10, disc=1, discTotal=2,
        picture={"offset": 600, "length": 4000, "type": 0, "mime": 1, "coding": 1})
    add("Artist/Album/cover.jpg", note="an owned non-audio file: container 255, no run, known 0",
        container=255, known=0, durationMs=0, ledger={"serverPath": "", "originFlags": 0})
    add("Artist/Album/CD1/01 - Disc One.flac", note="FLAC; compilation yes; the server's BPM; a PNG picture",
        container=2, tags={"title": ["Disc One"], "artist": ["Lantern Choir"], "album": ["Album"]},
        disc=1, discTotal=2, track=1, bpm10=1280, camelot=13, flags=1 | (1 << 8),
        picture={"offset": 300, "length": 9000, "type": 3, "mime": 2, "coding": 0})
    add("Artist/Album/CD2/01 - Disc Two.opus", note="Opus: an R128 gain; a picture across Ogg pages",
        container=3, tags={"title": ["Disc Two"], "artist": ["Lantern Choir"], "album": ["Album"]},
        disc=2, discTotal=2, track=1, rgTrackGain=r128(32), flags=(1 << 2) | (1 << 4),
        picture={"offset": 120, "length": 12000, "type": 3, "mime": 1, "coding": 2})
    add("Artist/x.mp3", note="UNREADABLE without FROM_API: known 0, no run",
        known=0, durationMs=0, flags=1 << 6)
    add("A/x.mp3", note="NO_TAGS: every known bit, no run", flags=1 << 5)
    add("A B/y.mp3", note="FROM_API: only the bits of the fields filled",
        tags={"title": ["Why"], "artist": ["Some Band, Other Band"], "album": ["Collected"], "genre": ["Pop"]},
        known=(1 << 0) | (1 << 1) | (1 << 2) | (1 << 4) | (1 << 8) | (1 << 9) | (1 << 10) | (1 << 11) | (1 << 14),
        year=2001, track=3, disc=1, rgTrackGain=-512, flags=(1 << 9) | (1 << 2))
    add("A-/z.flac", note="five 204-byte artist values: four kept, TRUNCATED",
        container=2, tags={"title": ["Zed"], "artist": [c * 204 for c in "PQRST"]})
    add("B/w.opus", note="values of 255, 255, 255, 250, 10 and 1 bytes: four kept",
        container=3, tags={"title": ["Double U"], "genre": ["g" * 255, "h" * 255, "i" * 255, "j" * 250, "k" * 10, "l"]})
    add("a/v.mp3", note="a 300-byte title cut to 255; 254 bytes and an e-acute cut to 254",
        tags={"title": ["t" * 300], "album": ["u" * 254 + "\u00e9"]})
    add("\u00c9/u.mp3", note="U+0080-U+009F kept; U+007F and U+001F to spaces",
        tags={"title": ["\u0080x\u009f"], "artist": ["Del\u007fName", "Unit\u001fSep"]})
    add(NFC_CAFE + "/Album/01 - Title.mp3", note="NFC: hashes as F1B24FC757494F2B",
        tags={"title": ["Title"], "artist": ["Caf\u00e9 Trio"]})
    add(NFD_CAFE + "/Album/01 - Title.mp3", note="NFD: another name, another hash (57017C0CC011BB1B)",
        tags={"title": ["Title"], "artist": ["Cafe\u0301 Trio"]})
    add("Loose Intro.mp3", note="a loose file at /music; a conversion; a sampled hash",
        tags={"title": ["Loose Intro"]},
        ledger={"serverPath": "Music/Loose Intro.flac", "mstreamId": 99, "audioHash": h16("loose"),
                "fileHash": h16("loose-f"), "serverModified": 1790000000999, "serverSize": 26214400,
                "createdAt": 1790000001, "hashV": 2, "originFlags": 1, "convertedTo": 1, "convertKbps": 320})
    return {
        "format": "MPTG", "file": "tags-0000002a.bin", "generation": 42, "cardId": CARD_ID, "minor": 0,
        "source": 2, "parserVersion": 1, "readRules": 1, "producer": "mstream-terminal 0.13.0",
        "note": "The transfer's tags file of the fixture card: tag values are raw (each writer applies "
                "2.3.6 and sets TRUNCATED); flags, known and the enums as given.",
        "folders": [{"path": "Artist/Album", "flags": FOLDER_THUMB},
                    {"path": "Owned Empty", "flags": FOLDER_OWNED},
                    {"path": "Owned Parent/Owned Child", "flags": FOLDER_OWNED},
                    {"path": "Artist/Album/CD1", "flags": FOLDER_OWNED}],
        "records": rec,
    }


def tags_device():
    t = fat_time(2025, 2, 28, 23, 59, 58)
    recs = [
        {"path": "Hand Copied/Album/01 - First.flac", "size": 4000, "fatTime": "0x%08X" % t, "durationMs": 200000,
         "known": KNOWN_RULES1, "container": 2, "track": 1,
         "tags": {"title": ["First"], "artist": ["Quiet Harbour"], "album": ["Album"]}},
        {"path": "Hand Copied/Album/02 - Second.flac", "size": 4100, "fatTime": "0x%08X" % t, "durationMs": 201000,
         "known": KNOWN_RULES1, "container": 2, "track": 2,
         "tags": {"title": ["Second"], "artist": ["Quiet Harbour"], "album": ["Album"]}},
        {"path": "Hand Copied/notags.mp3", "size": 300, "fatTime": 0, "durationMs": 1000, "known": KNOWN_RULES1,
         "container": 1, "flags": 1 << 5, "note": "NO_TAGS, an unknown time"},
        {"path": "Hand Copied/broken.mp3", "size": 10, "fatTime": "0x%08X" % t, "known": 0, "container": 1,
         "flags": 1 << 6, "note": "UNREADABLE"},
    ]
    return {"format": "MPTG", "file": "tags-device.bin", "generation": 0, "cardId": CARD_ID, "source": 1,
            "parserVersion": 3, "readRules": 1, "producer": "mstream-player 0.8.0",
            "note": "A device's /.player/tags.bin (source 1): no ledger.", "records": recs}


def tags_empty():
    return {"format": "MPTG", "file": "tags-00000001.bin", "generation": 1, "cardId": CARD_ID, "source": 2,
            "parserVersion": 1, "readRules": 1, "producer": "mstream-terminal 0.13.0",
            "note": "No records: FOLD holds /music and an OWNED empty folder; no HIDX, ORIG or OSTR.",
            "folders": [{"path": "Owned Empty", "flags": FOLDER_OWNED}], "records": []}


# MOOD-FILTERS.md 5.7's worked example, music/Artist A/song.flac on the server.
WORKED_SCORES = [0.7012, 0.3104, 0.5803, 0.7, 0.2999, None, 0.4999, 0.29]
WORKED_TAIL, WORKED_WORD, WORKED_FILTER = "03 3A 46 1D 31 FF 46 00", 0xB313, 0x127


def tags_moods():
    """A transfer's tags file with mood scores (2.6.4's tail, MPTG minor 1):
    MOOD-FILTERS.md 5.1's example tracks, 5.7's worked example, and the
    cases a writer could get wrong."""
    t = fat_time(2026, 11, 2, 18, 5, 10)
    rec = []

    def add(path, scores, **kw):
        r = {"path": path, "size": 3000 + 41 * len(rec), "fatTime": "0x%08X" % (t + 2 * len(rec)),
             "durationMs": 210000 + 1000 * len(rec), "qfp": "0x%016X" % fnv(b"moods " + path.encode("utf-8")),
             "known": KNOWN_RULES1, "container": 2, "scores": scores,
             "tags": {"title": [last_name(path).rsplit(".", 1)[0]], "artist": [path.split("/")[0]]}}
        r.update(kw)
        r.setdefault("ledger", {"serverPath": "music/" + path, "mstreamId": 300 + len(rec), "albumId": 20,
                                "artistId": 6, "audioHash": h16("ma" + path), "fileHash": h16("mf" + path),
                                "serverModified": 1792500000000 + len(rec), "serverSize": r["size"],
                                "createdAt": 1792500000, "hashV": 2, "originFlags": 2})
        rec.append(r)

    add("Artist A/scored.flac", [0.81, 0.29, 0, 0.7, 0.1234, 1, 0.57, 0.58],
        note="every head scored: 0 and 1 as integers; 0.29, 0.57 and 0.58 are 29, 57 and 58 (a plain floor "
             "gives 28, 56, 57)")
    add("Artist A/null-head.flac", [0.81, 0.29, None, 0.7, 0.1234, 1, 0.57, 0.58],
        note="sad NULL: valence 255, no order bit; the rest as scored.flac")
    add("Artist A/all-null.flac", [None] * 8, note="a row of NULLs: PRESENT, every value 255 (not no row)")
    add("Artist A/no-row.flac", None, note="no server row: 8 zero bytes")
    add("Artist A/song.flac", WORKED_SCORES, note="MOOD-FILTERS.md 5.7's worked example: " + WORKED_TAIL)
    add("Artist B/Album/01 - Ties.flac", [0.4, 0.6, 0.6, 0.55, 0.55, 0.3, 0.12345, 0.7],
        note="ties: happy = sad, relaxed = aggressive, so no order bit; a 5-place score (12)")
    add("Artist B/Album/02 - Order.flac", [0.1, 0.2, 0.9, 0.05, 0.95, 0, 0, 0.0001],
        note="sad beats happy, aggressive beats relaxed: flags 0x07")
    add("Artist B/Album/03 - Nulls.flac", [0.33, 0.12, 0.66, 0.8, None, 0.49, None, None],
        note="aggressive, acoustic and instrumental NULL: energy 255 and no bit 2; bit 1 kept")
    add("Artist B/Album/04 - Clamped.flac", [1.25, -0.5, 0.2, 0.9999, 0.0001, 1.5, -0.0001, 0.3],
        note="scores outside 0-1 clamp to 100 and 0; the order bits from the raw doubles")
    add("Artist B/Album/cover.jpg", None, note="an owned cover: no server track, so no row", container=255,
        known=0, durationMs=0, tags={}, ledger={"serverPath": "", "originFlags": 0})
    return {
        "format": "MPTG", "file": "tags-moods.bin", "generation": 44, "cardId": CARD_ID, "minor": 1,
        "source": 2, "parserVersion": 1, "readRules": 1, "producer": "mstream-terminal 0.13.0",
        "note": "A transfer's tags file with mood scores (2.6.4): RECS rows of 80 bytes, minor 1. A record's "
                "'scores' are mStream's eight in moods.json's 'heads' order, null where a head has none, or null "
                "for no server row; each writer derives the tail from them.",
        "records": rec,
    }


HOME_URL = "http://music.example:3000"  # ED901EA3EE763AC7, as 2.18's serverUrlKey vector
SECOND_URL = "https://second.example/mstream"
SECOND_INSTANCE = "8899aabb-ccdd-4eff-8011-223344556677"
FAR_INSTANCE = "0f1e2d3c-4b5a-4968-8796-a5b4c3d2e1f0"


def tags_servers():
    """A card filled from four servers (2.5.5, 2.6.6): the root's, a second
    server reached by its own URL, a peer of each."""
    t = fat_time(2026, 10, 9, 9, 15, 20)
    rec = []

    def add(path, server, server_path, **kw):
        r = {"path": path, "size": 2000 + 53 * len(rec), "fatTime": "0x%08X" % (t + 2 * len(rec)),
             "durationMs": 200000 + 1000 * len(rec), "qfp": "0x%016X" % fnv(b"servers " + path.encode("utf-8")),
             "known": KNOWN_RULES1, "container": 1}
        r.update(kw)
        r.setdefault("ledger", {"serverPath": server_path, "mstreamId": 100 + len(rec), "albumId": 5, "artistId": 2,
                                "audioHash": h16("sa" + server_path), "fileHash": h16("sf" + server_path),
                                "serverModified": 1790500000000 + len(rec), "serverSize": r["size"],
                                "createdAt": 1790500000, "hashV": 2, "originFlags": 2})
        r["ledger"]["server"] = server
        rec.append(r)

    add("Artist/Album/01 - Title.mp3", 0, "Music/Artist/Album/01 - Title.mp3", note="the root's own server: 0",
        tags={"title": ["Title"], "artist": ["Lantern Choir"], "album": ["Album"]})
    add("Artist/Album/cover.jpg", 0, "", note="an owned file with no server track: its album's server",
        container=255, known=0, durationMs=0)
    add("Second Artist/Album/01 - Song.flac", 1, "Library/Second Artist/Album/01 - Song.flac", container=2,
        note="server 1, reached by its own URL; mStream id 100 again (ids are each server's own)",
        tags={"title": ["Song"], "artist": ["Second Artist"], "album": ["Album"]})
    add("Both/One.mp3", 0, "Music/Both/One.mp3", note="one serverPath on two servers: the lower server keeps the name "
        "(2.8.3, step 6), though its mStream id is the higher", tags={"title": ["One"]})
    rec[-1]["ledger"]["mstreamId"] = 9
    add("Both/One (2).mp3", 1, "Music/Both/One.mp3", tags={"title": ["One"]})
    rec[-1]["ledger"]["mstreamId"] = 4
    add("Peer Artist/Peer Album/01 - Tune.opus", 2, "Shared/Peer Artist/Peer Album/01 - Tune.opus", container=3,
        note="server 2, a peer of the root's server: its own serverPath and ids, not its parent's",
        tags={"title": ["Tune"], "artist": ["Peer Artist"], "album": ["Peer Album"]})
    add("Peer Artist/Peer Album/cover.jpg", 2, "", note="an owned cover made for the peer's album: the peer's server",
        container=255, known=0, durationMs=0)
    add("Far Artist/Far Album/01 - Far.opus", 3, "Music/Far Artist/Far Album/01 - Far.flac", container=3,
        note="server 3, a peer of server 1, converted on the PC: not VERIFIED",
        tags={"title": ["Far"], "artist": ["Far Artist"], "album": ["Far Album"]})
    rec[-1]["ledger"].update(originFlags=0, convertedTo=3, convertKbps=192)
    for r in rec:
        if r["container"] == 255:
            r["ledger"] = {"serverPath": "", "originFlags": 0, "server": r["ledger"]["server"]}
    return {
        "format": "MPTG", "file": "tags-00000007.bin", "generation": 7, "cardId": CARD_ID, "minor": 0,
        "source": 2, "parserVersion": 1, "readRules": 1, "producer": "mstream-terminal 0.13.0",
        "note": "A card filled from several servers (2.5.5): each ledger row's 'server' is ORIG's server index, 0 "
                "the root's own; manifest-servers.json lists the others.",
        "folders": [{"path": "Peer Artist", "flags": FOLDER_OWNED},
                    {"path": "Peer Artist/Peer Album", "flags": FOLDER_OWNED}],
        "records": rec,
    }


def manifest_servers():
    return {"format": "MSMF", "file": "manifest-servers.bin", "generation": 7, "cardId": CARD_ID,
            "commitTime": 1791500000, "flags": 1, "commitId": "0x00000007C0DEC0DE",
            "producer": "mstream-terminal 0.13.0", "serverRevision": "\"r-5-104-1790500000004-0\"",
            "baseGeneration": 6, "serverUrl": HOME_URL,
            "note": "The root of a card filled from several servers: SRVS lists the servers other than the root's "
                    "(server i is entry i - 1; 2.5.5), each key from the URL given (2.5.1). The root's own "
                    "instance is unknown.",
            "companions": [{"file": "tags-00000007.bin"}], "roots": [],
            "servers": [
                {"serverInstance": SECOND_INSTANCE, "serverUrl": SECOND_URL,
                 "note": "server 1: another server, reached by its own URL, its instance known"},
                {"parentUrl": HOME_URL, "peerId": 3, "flags": PEER,
                 "note": "server 2: peer 3 of the root's server, its instance unknown"},
                {"serverInstance": FAR_INSTANCE, "parentUrl": SECOND_URL, "peerId": 1, "flags": PEER,
                 "note": "server 3: peer 1 of server 1, its instance known"},
            ]}


def manifest_main():
    return {"format": "MSMF", "file": "manifest.bin", "generation": 42, "cardId": CARD_ID, "commitTime": 1791400000,
            "flags": 1, "commitId": "0x0DDBA11CAFEF00D5", "serverInstance": "00112233-4455-6677-8899-aabbccddeeff",
            "producer": "mstream-terminal 0.13.0", "serverRevision": "\"r-412-99-1790000000999-300\"",
            "baseGeneration": 41, "serverUrl": "HTTP://Music.Example:3000/",
            "note": "The root of the fixture card: the tags file of this commit and an AutoDJ index (MPDJ v2, "
                    "docs/AUTODJ-INDEX.md) carried from the last (generation 41); the vpath layout's roots, given "
                    "unsorted.",
            "companions": [{"file": "tags-0000002a.bin"}, {"file": "autodj-00000029.bin"}],
            "roots": ["Lib B", "Lib A"]}


DEVICE_A, DEVICE_B = "00:00:5e:00:53:01", "00:00:5e:00:53:02"  # RFC 7042's documentation MACs
EPOCH_1, EPOCH_2, EPOCH_B = "0x1122334455667788", "0xF0E1D2C3B4A59687", "0x0A0B0C0D0E0F1011"


def manifest_acks():
    return {"format": "MSMF", "file": "manifest-acks.bin", "generation": 43, "cardId": CARD_ID,
            "commitTime": 1792000000, "flags": 1, "commitId": "0x0000002BACC5ACC5",
            "serverInstance": "00112233-4455-6677-8899-aabbccddeeff", "producer": "mstream-terminal 0.13.0",
            "serverRevision": "\"r-412-99-1790000000999-300\"", "baseGeneration": 42,
            "serverUrl": "HTTP://Music.Example:3000/",
            "note": "A root with ACKS (2.5.6): the plays of the device's log the software has, per device and epoch, "
                    "given out of order (the writer sorts them: by deviceId's bytes, then epoch as an unsigned "
                    "integer). Its companions are manifest.bin's.",
            "companions": [{"file": "tags-0000002a.bin"}, {"file": "autodj-00000029.bin"}],
            "roots": ["Lib B", "Lib A"],
            "acks": [{"deviceId": DEVICE_B, "epoch": EPOCH_B, "seq": 3},
                     {"deviceId": DEVICE_A, "epoch": EPOCH_2, "seq": 2},
                     {"deviceId": DEVICE_A, "epoch": EPOCH_1, "seq": 7}]}


def manifest_waves():
    return {"format": "MSMF", "file": "manifest-waves.bin", "generation": 43, "cardId": CARD_ID,
            "commitTime": 1792100000, "flags": 1, "commitId": "0x0000002BFACEC0DE",
            "serverInstance": "00112233-4455-6677-8899-aabbccddeeff", "producer": "mstream-terminal 0.13.0",
            "serverRevision": "\"r-412-99-1790000000999-300\"", "baseGeneration": 42,
            "serverUrl": "HTTP://Music.Example:3000/",
            "note": "A root that names all three companions (WAVEFORM-INDEX.md 7.2): its MPWF row after the MPTG and "
                    "MPDJ rows, its name after theirs in STRS, its key zeros. Its companions are manifest.bin's and a "
                    "waveform index carried from generation 42.",
            "companions": [{"file": "tags-0000002a.bin"}, {"file": "autodj-00000029.bin"},
                           {"file": "waves-0000002a.bin"}],
            "roots": ["Lib B", "Lib A"]}


def manifest_min():
    return {"format": "MSMF", "file": "manifest-min.bin", "generation": 1, "cardId": CARD_ID, "commitTime": 1791300000,
            "flags": 0, "commitId": "0x0000000000000001", "producer": "mstream-terminal 0.13.0",
            "serverRevision": "", "baseGeneration": 0, "serverUrlKey": "0",
            "note": "A checkpoint (FINAL clear): one companion, no LIBR, unknown server.",
            "companions": [{"file": "tags-00000001.bin"}], "roots": []}


def pending_main():
    return {"format": "MSPD", "file": "pending.bin", "generation": 43, "cardId": CARD_ID,
            "runId": "0x00C0FFEE00C0FFEE", "baseGeneration": 42,
            "note": "A run's plan, given out of order: the writer sorts it.",
            "ops": [{"op": "write", "path": "New Artist/New Album/01 - New.mp3", "expectedSize": 4321},
                    {"op": "folder", "path": "New Artist/New Album"},
                    {"op": "delete", "path": "Artist/x.mp3"},
                    {"op": "write", "path": "A B/y.mp3", "expectedSize": 77},
                    {"op": "folder", "path": "New Artist"},
                    {"op": "delete", "path": "A/x.mp3"},
                    {"op": "write", "path": "Loose New.mp3", "expectedSize": 12},
                    {"op": "write", "path": "A/z.mp3", "expectedSize": 5},
                    {"op": "folder", "path": "A B/Sub"}]}


def thumb_main():
    return {"format": "MPTH", "file": "B1F7E69F.565", "folder": "Artist/Album", "sourceBytes": 123456,
            "note": "The transfer thumbnail of /music/Artist/Album, its pixels the synthetic picture of "
                    "thumb_pixels(): pixel (x, y) of an n x n slot is RGB (x*255/(n-1), y*255/(n-1), "
                    "(x+y)*255/(2n-2)), integer division, as big-endian RGB565. The scaling is not pinned."}


def device_main():
    return {"format": "device.txt", "file": "device.txt", "contract": "1", "firmware": "0.12.0", "read.msmf": "1",
            "read.mptg": "1", "read.mpdj": "2", "read.mpwf": "1", "read.mpth": "1", "codecs": "mp3,flac,opus",
            "extensions": "mp3,flac,opus", "max_rate": "48000", "max_channels": "2", "history": "1"}


LIBRARIES = [tags_transfer, tags_device, tags_empty, manifest_min, manifest_main, pending_main, thumb_main,
             device_main, tags_servers, manifest_servers, manifest_acks, manifest_waves, tags_moods]


def build(desc, golden: dict):
    f = desc["format"]
    if f == "MPTG":
        return mptg(desc)
    if f == "MSMF":
        return msmf(desc, golden)
    if f == "MSPD":
        return mspd(desc)
    if f == "MPTH":
        return mpth(desc)
    if f == "device.txt":
        return device_txt(desc)
    raise ValueError(f)


# ---------------------------------------------------------------------------
# The play log (2.20; tools/playlog.py is its reference reader and writer)
# ---------------------------------------------------------------------------
def ledger_rows(b: bytes):
    """A tags file read back: per record, in record order, its path,
    pathHash, size, fatTime and durationMs and, with ORIG, its serverPath
    and server (2.6.4, 2.6.6)."""
    p = parse_container(b)
    strs = bytes(section(p, "STRS")[4])
    names = {s[0] for s in p["sections"]}

    def text(blob, off):
        return blob[off:blob.index(0, off)].decode("utf-8") if off else ""

    def join(a, n):
        return a + "/" + n if a else n

    fold, folders = section(p, "FOLD"), []
    for i in range(fold[2]):
        parent, name = struct.unpack_from("<II", fold[4], fold[3] * i)
        folders.append("" if i == 0 else join(folders[parent], text(strs, name)))
    recs, out = section(p, "RECS"), []
    for i in range(recs[2]):
        f, n, _, size, fat, dur = struct.unpack_from("<IIIIII", recs[4], recs[3] * i)
        path = join(folders[f], text(strs, n))
        out.append({"path": path, "pathHash": path_hash(path), "size": size, "fatTime": fat, "durationMs": dur})
    if "ORIG" in names:
        orig, ostr = section(p, "ORIG"), bytes(section(p, "OSTR")[4])
        for i, r in enumerate(out):
            r["serverPath"] = text(ostr, struct.unpack_from("<I", orig[4], orig[3] * i)[0])
            r["server"] = struct.unpack_from("<H", orig[4], orig[3] * i + 74)[0]
    return out


PLAYS_BOOT = (1791900000000, 1791986400000, 1792072800000)  # the script's boots, epoch ms
CLOCK = {"unset": 0, "rtc": 1, "host": 2}
NFC_TITLE, NFD_TITLE = NFC_CAFE + "/Album/01 - Title.mp3", NFD_CAFE + "/Album/01 - Title.mp3"
EPOCH_2_PATHS = ["B/w.opus", "A B/y.mp3", "A-/z.flac", "Artist/Album/CD2/01 - Disc Two.opus", "a/v.mp3", "É/u.mp3",
                 NFC_TITLE, NFD_TITLE, "Artist/Album/01 - Title.mp3"]


def plays_script(golden: dict):
    """The device's script for the play log's goldens (2.20): one Core2 and
    the fixture card (tags-0000002a.bin's files, generation 42), with a cap
    of 4 slots a file so the rotation shows. Every value a writer would
    otherwise choose is given."""
    rows = {r["path"]: r for r in ledger_rows(golden["tags-0000002a.bin"])}
    hand = {r["path"]: r for r in ledger_rows(golden["tags-device.bin"])}

    def play(path, outcome, source, played, uptime, boot, clock=True, pauses=0, duration=None, src=rows):
        r = src[path]
        return {"play": {"path": path, "pathHash": "0x%016X" % r["pathHash"], "size": r["size"],
                         "fatTime": "0x%08X" % r["fatTime"], "generation": 42, "outcome": outcome, "source": source,
                         "playedMs": played, "durationMs": r["durationMs"] if duration is None else duration,
                         "uptimeMs": uptime, "startedAt": PLAYS_BOOT[boot] + uptime if clock else 0,
                         "pauseCount": pauses}}
    events = [
        {"boot": {"clock": "unset"}, "note": "the first boot: NVS has no epoch, so EPOCH_1 is made; no clock"},
        play("Artist/Album/01 - Title.mp3", 1, 1, 180000, 5000, 0, clock=False),
        {"setClock": {}, "note": "a host's @time, during this boot"},
        play("Artist/Album/02 - Other.mp3", 2, 1, 12000, 190000, 0),
        play("Artist/Album/CD1/01 - Disc One.flac", 3, 3, 90500, 205000, 0, pauses=2),
        {"boot": {"clock": "rtc"}, "note": "the second boot: the RTC kept the time"},
        play("A/x.mp3", 1, 2, 186000, 8000, 1),
        play("Hand Copied/Album/01 - First.flac", 1, 2, 200000, 200000, 1, src=hand),
        play("Artist/x.mp3", 2, 1, 30000, 410000, 1, duration=0),
        play("Loose Intro.mp3", 1, 3, 194000, 450000, 1),
        {"snapshot": "sync-1", "note": "the card goes into a PC; the software resolves the seven plays"},
        {"acks": [{"deviceId": DEVICE_A, "epoch": EPOCH_1, "seq": 7}],
         "note": "the root of the sync's commit acknowledges them all (2.5.6)"},
        {"eraseNvs": {}, "note": "a flasher's install erases NVS"},
        {"boot": {"clock": "rtc"}, "note": "NVS has no epoch: EPOCH_2 is made; the acknowledged files go (2.20.2)"},
    ]
    for i, path in enumerate(EPOCH_2_PATHS):
        events.append(play(path, 1, (1, 2, 3)[i % 3], rows[path]["durationMs"], 10000 + 300000 * i, 2))
    events.append({"snapshot": "end", "note": "no sync since: the rotation dropped four plays, and said so"})
    return {"format": "MPPL", "file": "log.json", "cap": 4,
            "device": {"deviceId": DEVICE_A, "epochs": [EPOCH_1, EPOCH_2]},
            "note": "The play log's goldens (2.20): the events of one device with the fixture card. A play's file "
                    "is a record of tags-0000002a.bin (Hand Copied/...: of tags-device.bin, a file the ledger "
                    "doesn't hold); its fields are given. A snapshot is the log's files at that point, in "
                    "plays/golden/<name>/. The rotation's drop note takes the clock, startedAt, bootSeq and "
                    "uptimeMs of the play written after it (2.20.3).",
            "events": events}


def run_plays(desc):
    """The script through playlog.Device: {snapshot: {file name: bytes}},
    and the device's steps."""
    d = playlog.Device(desc["device"]["deviceId"], [u64(e) for e in desc["device"]["epochs"]], cap=desc["cap"])
    acks, snaps = None, {}
    for e in desc["events"]:
        if "boot" in e:
            d.boot(CLOCK[e["boot"]["clock"]], acks)
        elif "setClock" in e:
            d.set_clock()
        elif "eraseNvs" in e:
            d.erase_nvs()
        elif "acks" in e:
            acks = [(playlog.device_bytes(a["deviceId"]).hex(), u64(a["epoch"]), a["seq"]) for a in e["acks"]]
        elif "snapshot" in e:
            snaps[e["snapshot"]] = dict(d.files)
        else:
            p = e["play"]
            d.play(u64(p["pathHash"]), p["size"], u64(p["fatTime"]), p["generation"], p["playedMs"],
                   p["durationMs"], p["outcome"], p["source"], p["uptimeMs"], p["startedAt"], p["pauseCount"], acks)
    return snaps, d.ops


def play_slot(device, epoch, seq, **kw):
    """A play slot for the reader's cases."""
    s = {"kind": playlog.KIND_PLAY, "deviceId": device, "epoch": u64(epoch), "seq": seq, "outcome": 1, "source": 1,
         "clock": 1, "startedAt": 1792100000000 + 1000 * seq, "bootSeq": 1, "uptimeMs": 1000 * seq,
         "playedMs": 60000, "durationMs": 60000, "pathHash": path_hash("Artist/Album/01 - Title.mp3"), "size": 1000,
         "fatTime": fat_time(2026, 10, 7, 14, 30, 42), "generation": 42}
    s.update(kw)
    return playlog.encode(s)


def plays_read_cases(snaps):
    """The reader's cases (2.20.6): [(case, {file name: bytes}, ids, counts,
    extra, note)], the expected values typed from the spec."""
    a1 = lambda n: "core2-00005e005301-1122334455667788-%d" % n  # noqa: E731
    a2 = lambda n: "core2-00005e005301-f0e1d2c3b4a59687-%d" % n  # noqa: E731
    b = lambda n: "core2-00005e005302-0a0b0c0d0e0f1011-%d" % n  # noqa: E731
    s1, end = snaps["sync-1"], snaps["end"]
    zero = {k: 0 for k in ("slots", "damaged", "tail", "newer", "unknownKinds", "unusable", "twins", "conflicts",
                           "dropped")}
    c = lambda **kw: dict(zero, **kw)  # noqa: E731
    torn = bytearray(s1["plays.1.jnl"])
    torn[100:140] = bytes(40)
    newer = bytearray(play_slot(DEVICE_A, EPOCH_1, 9))
    newer[4] = 2
    struct.pack_into("<I", newer, 76, crc(bytes(newer[:76])))
    a2_other = playlog.read_log({"plays.1.jnl": s1["plays.1.jnl"]})["plays"][1]
    conflict = playlog.encode(dict(a2_other, playedMs=a2_other["playedMs"] + 1))
    a1_last = playlog.read_log({"plays.jnl": s1["plays.jnl"]})["plays"][-1]
    conflict_top = s1["plays.jnl"] + playlog.encode(dict(a1_last, playedMs=a1_last["playedMs"] + 1))
    dating = (play_slot(DEVICE_B, EPOCH_B, 3, startedAt=0, clock=0, bootSeq=4, uptimeMs=0xFFFFF000) +
              play_slot(DEVICE_B, EPOCH_B, 4, startedAt=1792100000000, clock=2, bootSeq=4, uptimeMs=0x1000) +
              play_slot(DEVICE_B, EPOCH_B, 5, startedAt=0, clock=0, bootSeq=5, uptimeMs=3000))
    return [
        ("sync-1", s1, [a1(n) for n in range(1, 8)], c(slots=7), {}, "the golden at the first sync"),
        ("end", end, [a2(n) for n in range(5, 10)], c(slots=6, dropped=4),
         {}, "the golden at the end: a new epoch; a drop note of 4 plays"),
        ("tail", dict(s1, **{"plays.jnl": s1["plays.jnl"] + b"\x5a" * 37}), [a1(n) for n in range(1, 8)],
         c(slots=7, tail=1), {}, "a cut append: 37 bytes after the last slot, ignored"),
        ("torn", dict(s1, **{"plays.1.jnl": bytes(torn)}), [a1(n) for n in (1, 3, 4, 5, 6, 7)],
         c(slots=7, damaged=1), {}, "a torn sector: the second slot's bytes 20-59 zeroed, its CRC fails"),
        ("newer", dict(s1, **{"plays.jnl": s1["plays.jnl"] + bytes(newer)}), [a1(n) for n in range(1, 8)],
         c(slots=8, newer=1), {}, "a slot of major 2, its CRC good: a newer firmware's, skipped and counted"),
        ("unknown-kind", dict(s1, **{"plays.jnl": s1["plays.jnl"] + play_slot(DEVICE_A, EPOCH_1, 8, kind=9)}),
         [a1(n) for n in range(1, 8)], c(slots=8, unknownKinds=1), {}, "a valid slot of kind 9: skipped"),
        ("twins", {"plays.jnl": s1["plays.1.jnl"], "plays.1.jnl": s1["plays.1.jnl"]}, [a1(n) for n in range(1, 5)],
         c(slots=8, twins=4), {}, "a rotation's rename cut between its two directory writes: both names, one chain"),
        ("conflict", {"plays.1.jnl": s1["plays.1.jnl"], "plays.jnl": conflict}, [a1(n) for n in (1, 3, 4)],
         c(slots=5, conflicts=1), {}, "one key, two different slots: neither is used"),
        ("conflict-top", dict(s1, **{"plays.jnl": conflict_top}), [a1(n) for n in range(1, 7)],
         c(slots=8, conflicts=1), {}, "a conflict at the log's end: seq 7's two slots differ, neither is used "
                                      "(the next seq at boot is still 8: vectors.json's plays.atBoot)"),
        ("unusable", {"plays.jnl": play_slot(DEVICE_A, 0, 1) + play_slot(DEVICE_A, EPOCH_1, 0) +
                      play_slot("00:00:00:00:00:00", EPOCH_1, 1)}, [], c(slots=3, unusable=3), {},
         "valid slots without an epoch, a seq, or a deviceId"),
        ("magic", {"plays.jnl": play_slot(DEVICE_A, EPOCH_1, 1) + bytes(80) + play_slot(DEVICE_A, EPOCH_1, 2)},
         [a1(1), a1(2)], c(slots=3, damaged=1), {}, "a zeroed slot between two plays: reading goes on"),
        ("two-devices", {"plays.jnl": play_slot(DEVICE_B, EPOCH_B, 2) + play_slot(DEVICE_A, EPOCH_1, 8) +
                         play_slot(DEVICE_B, EPOCH_B, 1)}, [a1(8), b(1), b(2)], c(slots=3), {},
         "two devices on one card: by deviceId's bytes, then epoch, then seq"),
        ("enums", {"plays.jnl": play_slot(DEVICE_A, EPOCH_1, 1, outcome=9, source=7, clock=5) +
                   play_slot(DEVICE_A, EPOCH_1, 2, outcome=0, source=0)}, [a1(1), a1(2)], c(slots=2),
         {"names": {a1(1): ["stopped", "other"], a1(2): ["stopped", "other"]}},
         "unknown values: outcome 9 and 0 read as stopped, source 7 as not said (other), clock 5 is ignored"),
        ("dating", {"plays.jnl": dating}, [b(3), b(4), b(5)], c(slots=3),
         {"dates": {b(3): 1792100000000 - 0x2000, b(4): 1792100000000, b(5): None}},
         "no clock: dated by the next dated play of its boot, the uptime wrapping (0xFFFFF000 to 0x1000 is 8,192 "
         "ms); with none in its boot, no start"),
    ]


def plays_files(golden: dict):
    """test/fixtures/card/plays: the script (log.json), the log files at each
    snapshot (golden/<snapshot>/), the reader's cases (read/<case>/, listed
    in index.json with what a reader takes from each), every case read back
    by tools/playlog.py first."""
    desc = plays_script(golden)
    snaps, ops = run_plays(desc)
    files = {"plays/log.json": (json.dumps(desc, indent=1, ensure_ascii=False) + "\n").encode("utf-8")}
    for snap, logs in snaps.items():
        for name, data in logs.items():
            files["plays/golden/%s/%s" % (snap, name)] = data
    known = {data: path[len("plays/"):] for path, data in files.items() if path.startswith("plays/golden/")}
    index = []
    for case, logs, ids, counts, extra, note in plays_read_cases(snaps):
        log = playlog.read_log(logs)
        got = [playlog.play_id(p) for p in log["plays"]]
        assert got == ids and log["counts"] == counts, (case, got, log["counts"])
        when = playlog.dates(log["plays"])
        for i, ms in extra.get("dates", {}).items():
            assert when[next(playlog.key(p) for p in log["plays"] if playlog.play_id(p) == i)] == ms, (case, i)
        for i, (o, s) in extra.get("names", {}).items():
            p = next(p for p in log["plays"] if playlog.play_id(p) == i)
            assert (playlog.outcome_name(p["outcome"]), playlog.source_name(p["source"])) == (o, s), (case, i)
        where = {}
        for name in sorted(logs, key=playlog.NAMES.index):
            # A golden file is named where it is; a case's own file is in
            # read/<case>/, under the name it has on the card.
            where[name] = known.get(logs[name], "read/%s/%s" % (case, name))
            if where[name].startswith("read/"):
                files["plays/" + where[name]] = logs[name]
        index.append(dict({"case": case, "files": where, "ids": ids, "counts": counts}, **extra, note=note))
    files["plays/index.json"] = (json.dumps(index, indent=1, ensure_ascii=False) + "\n").encode("utf-8")
    return files, snaps, ops


def play_target(root, server):
    """2.20.6, 2.5.5: where a play of a row of `server` goes, on the card
    whose root description is `root`: {"server": the card's server it is
    posted to, "peerId": a peer's id, else None}; None for an unknown
    server. A peer's goes to its parent, the card's server of parentKey
    (when the card has none: "server" None, the parent by its key)."""
    srv = card_server(root, server)
    if srv is None:
        return None
    if not srv["flags"] & PEER:
        return {"server": server, "peerId": None}
    return {"server": card_parent(root, srv["parentKey"]), "peerId": srv["peerId"]}


def history_majors(text):
    """2.15's history key: the play log's majors a firmware writes ([] none);
    text None: no device.txt."""
    if text is None:
        return []
    for line in text.replace("\r\n", "\n").split("\n"):
        k, _, v = line.partition("=")
        if k == "history":
            return sorted({int(x) for x in v.split(",") if x.strip().isdigit()})
    return []


def slot_json(f):
    """A slot's fields for the fixtures: the integers that may pass 2^53
    (epoch, pathHash) as hex strings."""
    out = dict(f)
    for k in ("epoch", "pathHash"):
        if k in out:
            out[k] = "0x%016X" % out[k]
    return out


def plays_vectors(golden: dict, snaps, ops):
    """vectors.json's plays group (2.20, 2.5.6)."""
    a = lambda n: "core2-00005e005301-1122334455667788-%d" % n  # noqa: E731
    a2 = lambda n: "core2-00005e005301-f0e1d2c3b4a59687-%d" % n  # noqa: E731
    first = snaps["sync-1"]["plays.1.jnl"][:80]
    note = snaps["end"]["plays.jnl"][:80]
    t1, t2, t3 = PLAYS_BOOT
    sync1 = [
        (a(1), True, "Music/Artist/Album/01 - Title.mp3", t1 + 190000 - 185000,
         "no clock at its start: dated by play 2 of its boot, 185,000 ms of uptime later"),
        (a(2), True, "Music/Artist/Album/02 - Other.mp3", t1 + 190000, "skipped"),
        (a(3), True, "Music/Artist/Album/CD1/01 - Disc One.flac", t1 + 205000, "stopped, 2 pauses, Auto DJ"),
        (a(4), True, "Music/A/x.mp3", t2 + 8000, "a second boot, the RTC's time"),
        (a(5), False, "file", None, "Hand Copied/...: no ledger row, the listener's file: counted, not sent"),
        (a(6), False, "file", None, "Artist/x.mp3: the ledger's stamp is invalid (sec2 31), so no time "
                                    "matches: counted, not sent"),
        (a(7), True, "Music/Loose Intro.flac", t2 + 450000, "a converted file: its ledger row's server path"),
    ]
    end = [(a2(n), True, "Music/" + EPOCH_2_PATHS[n - 1], t3 + 10000 + 300000 * (n - 1), "")
           for n in range(5, 10)]
    to = lambda server, peer=None: {"server": server, "peerId": peer}  # noqa: E731
    peers = [{"path": "Artist/Album/01 - Title.mp3", "server": 0, "to": to(0)},
             {"path": "Both/One (2).mp3", "server": 1, "to": to(1)},
             {"path": "Both/One.mp3", "server": 0, "to": to(0)},
             {"path": "Far Artist/Far Album/01 - Far.opus", "server": 3, "to": to(1, 1)},
             {"path": "Peer Artist/Peer Album/01 - Tune.opus", "server": 2, "to": to(0, 3)},
             {"path": "Second Artist/Album/01 - Song.flac", "server": 1, "to": to(1)}]
    rows2a = {r["path"]: r for r in ledger_rows(golden["tags-0000002a.bin"])}
    other = rows2a["Artist/Album/02 - Other.mp3"]
    hour_later = fat_from_wall(fat_wall(other["fatTime"]) + 3600)
    ack_rows = parse_container(golden["manifest-acks.bin"])
    acks = section(ack_rows, "ACKS")
    return {
        "who": "both",
        "note": ("2.20 and 2.5.6: the play log. slot: one play slot's fields and bytes; note: a drop note's. ids: "
                 "a play's id. names: what an outcome and a source read as. atBoot: the next seq and the next "
                 "bootSeq at boot from NVS's values, the card's log (the golden sync-1, or readCase's files in "
                 "plays/index.json), every valid slot counting, and the root's ACKS (acks; none when absent) "
                 "for the seq (2.20.4). deviceOps: what the device model did "
                 "with log.json's events (informative). files: plays/'s files (the goldens, the reader's cases in "
                 "plays/index.json). acks: ACKS's bytes, the golden root's entries, and the hardening files. "
                 "resolve: a golden log through a golden tags file (the software's): each new play, sent or not "
                 "(why: 'file' its file isn't a ledger row's, 'clock' no start), where to and its start; then the "
                 "posted marks once the server settled the sent ones. resolveCases: single plays. peers: where "
                 "a play of each row of the card of several servers goes. deviceTxt: the history key."),
        "slot": {"fields": slot_json(playlog.decode(first)[1]), "bytes": first.hex(" ").upper(),
                 "crc": "0x%08X" % struct.unpack_from("<I", first, 76)[0], "id": a(1)},
        "dropNote": {"fields": slot_json(playlog.decode(note)[1]), "bytes": note.hex(" ").upper()},
        "ids": [
            {"deviceId": DEVICE_A, "epoch": EPOCH_1, "seq": 42, "id": "core2-00005e005301-1122334455667788-42"},
            {"deviceId": DEVICE_A, "epoch": EPOCH_2, "seq": 1, "id": "core2-00005e005301-f0e1d2c3b4a59687-1"},
            {"deviceId": "FF:FF:FF:FF:FF:FE", "epoch": "0xFFFFFFFFFFFFFFFF", "seq": 4294967295,
             "id": "core2-fffffffffffe-ffffffffffffffff-4294967295", "note": "the longest: 46 characters"},
            {"deviceId": DEVICE_A, "epoch": "0x0000000000000001", "seq": 1,
             "id": "core2-00005e005301-0000000000000001-1",
             "note": "the epoch's leading zeros are written"}],
        "names": {"outcome": {"0": "stopped", "1": "completed", "2": "skipped", "3": "stopped", "4": "stopped"},
                  "source": {"0": "other", "1": "manual", "2": "shuffle", "3": "autodj", "4": "other"}},
        "atBoot": {"log": "plays/golden/sync-1", "cases": [
            {"nvsSeq": 0, "nvsBoots": 0, "deviceId": DEVICE_A, "epoch": EPOCH_1, "nextSeq": 8, "nextBootSeq": 3,
             "note": "NVS behind the card, for the seq and the boot count"},
            {"nvsSeq": 9, "nvsBoots": 5, "deviceId": DEVICE_A, "epoch": EPOCH_1, "nextSeq": 10, "nextBootSeq": 6,
             "note": "NVS ahead (seqs reserved, boots with no play)"},
            {"nvsSeq": 0, "nvsBoots": 0, "deviceId": DEVICE_A, "epoch": EPOCH_2, "nextSeq": 1, "nextBootSeq": 1,
             "note": "a new epoch"},
            {"nvsSeq": 3, "nvsBoots": 1, "deviceId": DEVICE_B, "epoch": EPOCH_1, "nextSeq": 4, "nextBootSeq": 2,
             "note": "another device's plays"},
            {"readCase": "conflict-top", "nvsSeq": 0, "nvsBoots": 0, "deviceId": DEVICE_A, "epoch": EPOCH_1,
             "nextSeq": 8, "nextBootSeq": 3,
             "note": "seq 7's two slots conflict, so a reader uses neither; the boot rule counts both"},
            {"readCase": "end", "nvsSeq": 3, "nvsBoots": 1, "deviceId": DEVICE_A, "epoch": EPOCH_1,
             "acks": [{"deviceId": DEVICE_A, "epoch": EPOCH_1, "seq": 7}], "nextSeq": 8, "nextBootSeq": 2,
             "note": "NVS put back from a backup (seq 3) behind a log already trimmed: EPOCH_1's slots are gone "
                     "(the golden at its end holds EPOCH_2's), and ACKS's seq 7 reserves 4-7, which a new play "
                     "would otherwise reuse and never be sent (0.14.0 took 4)"}]},
        "deviceOps": [list(o) for o in ops],
        "files": {"golden": sorted("plays/golden/%s/%s" % (s, n) for s in snaps for n in snaps[s])},
        "acks": {
            "entry": {"deviceId": DEVICE_A, "epoch": EPOCH_1, "seq": 7,
                      "stored": "00 00 5E 00 53 01 00 00 88 77 66 55 44 33 22 11 07 00 00 00 00 00 00 00"},
            "golden": {"file": "manifest-acks.bin", "stride": acks[3], "entries": [
                {"deviceId": "00005e005301", "epoch": EPOCH_1, "seq": 7},
                {"deviceId": "00005e005301", "epoch": EPOCH_2, "seq": 2},
                {"deviceId": "00005e005302", "epoch": EPOCH_B, "seq": 3}],
                "note": "EPOCH_2 has its top bit set: unsigned, it sorts after EPOCH_1"},
            "hardening": [
                {"file": "msmf-acks-damaged.bin", "acks": None, "root": "present", "note": "its CRC fails"},
                {"file": "msmf-acks-order.bin", "acks": None, "root": "present", "note": "two entries out of order"},
                {"file": "msmf-acks-short.bin", "acks": None, "root": "present", "note": "rows of 16 bytes"},
                {"file": "msmf-acks-minor-1.bin", "acks": [
                    {"deviceId": "00005e005301", "epoch": EPOCH_1, "seq": 7},
                    {"deviceId": "00005e005301", "epoch": EPOCH_2, "seq": 2},
                    {"deviceId": "00005e005302", "epoch": EPOCH_B, "seq": 3}],
                 "root": "present", "note": "rows of 32 bytes, read by their 24-byte prefix: the golden's entries"}]},
        "resolve": [
            {"log": "plays/golden/sync-1", "ledger": "tags-0000002a.bin", "skew": 0, "posted": [],
             "plays": [{"id": i, "send": s_, "filePath" if s_ else "why": w, "startedAt": t, "note": n}
                       for i, s_, w, t, n in sync1],
             "settled": [i for i, s_, _, _, _ in sync1 if s_],
             "marks": [{"deviceId": "00005e005301", "epoch": EPOCH_1, "seq": 7}]},
            {"log": "plays/golden/end", "ledger": "tags-0000002a.bin", "skew": 0,
             "posted": [{"deviceId": "00005e005301", "epoch": EPOCH_1, "seq": 7}],
             "plays": [{"id": i, "send": s_, "filePath": w, "startedAt": t} for i, s_, w, t, _ in end],
             "settled": [i for i, _, _, _, _ in end],
             "marks": [{"deviceId": "00005e005301", "epoch": EPOCH_1, "seq": 7},
                       {"deviceId": "00005e005301", "epoch": EPOCH_2, "seq": 9}],
             "dropped": 4, "note": "the epoch's plays 1-4 were dropped by the rotation before a sync read them"}],
        "resolveCases": [
            {"slot": {"size": other["size"] + 1}, "skew": 0, "send": False, "why": "file",
             "note": "Artist/Album/02 - Other.mp3, one byte longer: not the ledger's file"},
            {"slot": {"fatTime": "0x%08X" % hour_later}, "skew": 3600, "send": True,
             "note": "an hour later under T's skew of +3,600: the ledger's file"},
            {"slot": {"fatTime": "0x%08X" % hour_later}, "skew": 0, "send": False, "why": "file",
             "note": "the same with no skew"},
            {"slot": {"fatTime": "0x00000000"}, "skew": 0, "send": False, "why": "file", "note": "a 0 stamp"},
            {"slot": {"startedAt": 0, "clock": 0}, "skew": 0, "send": False, "why": "clock",
             "note": "no start, and no later dated play of its boot"},
            {"slot": {"seq": 3}, "posted": 3, "skew": 0, "send": None, "note": "at the posted mark: not new"}],
        "resolveCasesBase": {"deviceId": DEVICE_A, "epoch": EPOCH_1, "seq": 4, "path": "Artist/Album/02 - Other.mp3",
                             "ledger": "tags-0000002a.bin"},
        "peers": {"card": "manifest-servers.json", "ledger": "tags-00000007.bin", "rows": peers,
                  "note": "server 2 is peer 3 of the root's server (posted to server 0 with peerId 3), server 3 peer "
                          "1 of server 1"},
        "deviceTxt": [{"text": "contract=1\nhistory=1\n", "history": [1]},
                      {"text": "contract=1\nfirmware=0.11.0\n", "history": [], "note": "no key: no log"},
                      {"text": "contract=1\nhistory=\n", "history": [], "note": "empty: none"},
                      {"text": "contract=1\nhistory=1,2\n", "history": [1, 2]},
                      {"text": None, "history": [], "note": "no device.txt"}],
    }


# ---------------------------------------------------------------------------
# The vectors of 2.18 (the expected values typed from the spec; check()
# recomputes each one before vectors.json is written)
# ---------------------------------------------------------------------------
def pairs_for(groups):
    base = fat_wall(fat_time(2026, 10, 7, 14, 30, 42))
    out = []
    for g in groups:
        for i in range(g["count"]):
            rec = fat_from_wall(base + 2 * 86400 * len(out))
            obs = fat_from_wall(fat_wall(rec) + g.get("delta", 0))
            # A group may pin either stamp (a 0, or an invalid one).
            if "recorded" in g:
                rec = hexint(g["recorded"])
            if "observed" in g:
                obs = hexint(g["observed"])
            out.append((rec, obs))
    return out


ROOT_INSTANCE = "a0b1c2d3-e4f5-4a6b-8c7d-8e9f00112233"


def broken_server_cases():
    """2.5.5's readers: an SRVS entry that breaks a writer rule names an
    unknown server (2.11), though its fields alone would pass; the root
    stays valid, and the other entries keep their test (the controls)."""
    root = {"serverUrl": HOME_URL, "serverInstance": ROOT_INSTANCE}
    peer = {"parentUrl": HOME_URL, "peerId": 3, "flags": PEER}
    cases = [
        ([{"serverUrl": HOME_URL}], 1, {"url": HOME_URL}, "the root's own server again (its URL key)"),
        ([{"serverInstance": ROOT_INSTANCE, "serverUrl": SECOND_URL}], 1, {"url": SECOND_URL},
         "the root's own server again (its instance)"),
        ([{"serverInstance": SECOND_INSTANCE, "serverUrl": SECOND_URL}, {"serverInstance": SECOND_INSTANCE,
          "serverUrl": "http://192.0.2.20"}], 1, {"url": SECOND_URL, "serverInstance": SECOND_INSTANCE},
         "two entries with one instance: both"),
        ([{"serverUrl": SECOND_URL}, {"serverUrl": "HTTPS://Second.Example/mstream/"}], 2, {"url": SECOND_URL},
         "two entries with one URL key (the URLs normalised: 2.5.1)"),
        ([{"serverInstance": SECOND_INSTANCE}], 1, {"url": SECOND_URL, "serverInstance": SECOND_INSTANCE},
         "no PEER and no URL key, though its instance is the one read"),
        ([{"serverUrl": SECOND_URL, "peerId": 2}], 1, {"url": SECOND_URL}, "no PEER, and a peerId"),
        ([dict(peer, serverUrl=SECOND_URL)], 1, {"url": HOME_URL, "peerId": 3}, "a peer with a URL key"),
        ([{"serverInstance": FAR_INSTANCE, "peerId": 3, "flags": PEER}], 1,
         {"url": HOME_URL, "peerId": 3, "peerInstance": FAR_INSTANCE},
         "a peer with no parentKey, though its instance is the one read"),
        ([dict(peer, peerId=0)], 1, {"url": HOME_URL, "peerId": 0}, "a peer with no peerId"),
        ([peer, dict(peer)], 2, {"url": HOME_URL, "peerId": 3}, "two peers with one parentKey and peerId"),
    ]
    controls = [
        ([{"serverInstance": SECOND_INSTANCE, "serverUrl": SECOND_URL}, {"serverInstance": SECOND_INSTANCE,
          "serverUrl": "http://192.0.2.20"}, {"serverUrl": "http://198.51.100.7"}], 3,
         {"url": "http://198.51.100.7"}, "the other entries keep their test"),
        ([{"serverUrl": SECOND_URL, "flags": 2}], 1, {"url": SECOND_URL},
         "a reserved flag bit is a newer minor's, ignored (2.4.5): no broken rule"),
        ([{"parentUrl": SECOND_URL, "peerId": 1, "flags": PEER}], 1, {"url": SECOND_URL, "peerId": 1},
         "a peer whose parent has no rows on the card: parentKey by the key rule alone"),
    ]
    out = []
    for (servers, index, talk, note), verdict in [(c, "unknown") for c in cases] + [(c, "same") for c in controls]:
        card = dict(root, servers=servers)
        broken = index in srvs_breaks(root_server(card), [server_entry(s) for s in servers])
        # Each case is unknown by the rule alone (read without it, it is the
        # same), and each control breaks none.
        assert broken == (verdict == "unknown") and same_server(card, index, talk, rules=False) == "same", note
        out.append({"card": card, "index": index, "talk": talk, "verdict": verdict, "note": note})
    return out


def private_text(fmt, seq, version=PRIVATE_VERSION, **members):
    """A private file's text as the fixtures write it (2.12.7's form)."""
    return json.dumps(dict({"format": fmt, "version": version, "seq": seq}, **members), ensure_ascii=False) + "\n"


def private_vectors():
    """2.12.7 and 2.12.8 as cases: the names, the read with its recovery,
    step 4's clean-up and a partial's fetch. The expected values are typed
    from the spec; check_vectors() recomputes each one."""
    state = lambda seq, **m: private_text(STATE_FORMAT, seq, label="Card A", **m)  # noqa: E731
    torn = state(4)[:30]
    names = [
        ("state.json", True), ("state.new", True), ("resume.json", True), ("resume.new", True), ("run.lock", True),
        ("resume/0000002a.part", True), ("notes.txt", True), ("cache/index.bin", True), ("cache/old.tmp", False),
        ("State.json", False), ("state.tmp", False), ("tags-0000002a.bin", False), ("tags-backup.bin", False),
        ("autodj-old.bin", False), ("waves-0000002a.bin", False), ("waves-old.bin", False), ("manifest.json", False),
        ("pending.txt", False), ("stage/keep.bin", False),
        ("thumbs/state.json", False), ("resume/notes.txt", False), ("resume/0000002A.part", False),
        ("resume/old/0000002a.part", False), ("stage", False), ("thumbs", False), ("resume", False)]
    settle = [
        ("state", state(3), None, "state.json", 3, [], False, False, False, "only state.json"),
        ("state", None, state(4), "state.new", 4, [], True, False, False,
         "only state.new: a cut between the delete and the rename"),
        ("state", state(3), state(4), "state.new", 4, ["state.json"], True, False, False,
         "both, the new one higher: a cut before the delete"),
        ("state", state(3), torn, "state.json", 3, ["state.new"], False, False, False, "a torn state.new"),
        ("state", torn, state(4), "state.new", 4, ["state.json"], True, False, False, "a torn state.json"),
        ("state", state(4), state(4), "state.json", 4, [], False, True, False,
         "byte-equal: the rename cut between its two directory writes; neither deleted"),
        ("state", state(4), state(4, note="other"), "state.json", 4, ["state.new"], False, False, False,
         "equal seqs, different bytes: state.json"),
        ("state", state(5), state(4), "state.json", 5, ["state.new"], False, False, False, "the higher seq wins"),
        ("state", torn, torn[:20], "none", None, [], False, False, False, "neither valid: no private state"),
        ("state", private_text(STATE_FORMAT, 3, version=2), None, "none", None, [], False, False, True,
         "a version this software doesn't know: it stops (2.4.5)"),
        ("state", state(3), private_text(STATE_FORMAT, 4, version=2), "none", None, [], False, False, True,
         "an unknown version in state.new stops it too"),
        ("state", "﻿" + state(3), None, "none", None, [], False, False, False, "a byte-order mark: invalid"),
        ("state", state(3).replace('"seq": 3', '"seq": "3"'), None, "none", None, [], False, False, False,
         "seq a string: invalid"),
        ("state", state(3).replace('"seq": 3', '"seq": 0'), None, "none", None, [], False, False, False,
         "seq 0: invalid"),
        ("state", state(3).replace('"seq": 3', '"seq": 3.0'), None, "none", None, [], False, False, False,
         "seq not an integer: invalid"),
        ("state", state(3).replace('"seq": 3', '"seq": 3, "seq": 4'), None, "none", None, [], False, False, False,
         "a member named twice: invalid"),
        ("state", private_text(RESUME_FORMAT, 3), None, "none", None, [], False, False, False,
         "another format: invalid"),
        ("state", '[{"format": "mstream-card-state", "version": 1, "seq": 3}]\n', None, "none", None, [], False,
         False, False, "not an object: invalid"),
        ("state", state(3, gain=float("nan")), None, "none", None, [], False, False, False,
         "NaN is not JSON: invalid"),
        ("resume", private_text(RESUME_FORMAT, 2, parts=[]), private_text(RESUME_FORMAT, 3, parts=[]), "resume.new",
         3, ["resume.json"], True, False, False, "resume.json follows the same rule"),
    ]
    home = "0x%016X" % server_url_key(HOME_URL)
    part = {"name": "0000002a.part", "server": {"serverInstance": "", "serverUrlKey": "0x0", "parentKey": home,
                                                "peerId": 3},
            "serverPath": "Shared/Peer Artist/Peer Album/01 - Tune.flac", "size": 31457280,
            "etag": "W/\"1e00000-19a1b2c3d4e\"", "modified": 1790500000005, "have": 10485760}
    own = {"name": "0000002b.part", "server": {"serverInstance": "", "serverUrlKey": home, "parentKey": "0x0",
                                               "peerId": 0},
           "serverPath": "Music/Artist/Album/02 - Long.flac", "size": 52428800, "etag": "",
           "modified": 1790000000123, "have": 4194304}
    resume = private_text(RESUME_FORMAT, 7, parts=[part, own, dict(own, name="0000002c.part", size=100)])
    listing = {"manifest.bin": 480, "tags-0000002a.bin": 6432, "tags-00000029.bin": 6400, "autodj-00000029.bin": 9000,
               "manifest.tmp": 470, "pending.bin": 300, "stage/00000003.tmp": 5000, "thumbs/B/B1F7E69F.565": 21656,
               "thumbs/B/B1F7E69F.tmp": 21656, "state.json": 200, "resume.json": 900,
               "resume/0000002a.part": 10485760, "resume/0000002b.part": 52428801, "resume/0000002c.part": 100,
               "resume/0000002d.part": 4096, "resume/old/0000002e.part": 10, "run.lock": 120, "notes.txt": 5,
               "cache/index.bin": 64, "waves-0000002a.bin": 2048, "waves-00000029.bin": 1792}
    root = ["tags-0000002a.bin", "autodj-00000029.bin", "waves-0000002a.bin"]
    every = ["manifest.tmp", "resume/0000002a.part", "resume/0000002b.part", "resume/0000002c.part",
             "resume/0000002d.part", "resume/old/0000002e.part", "stage/00000003.tmp", "tags-00000029.bin",
             "thumbs/B/B1F7E69F.tmp", "waves-00000029.bin"]
    clean = [
        (listing, root, resume, [n for n in every if n not in ("resume/0000002a.part", "resume/0000002c.part")],
         "autodj-00000029.bin and waves-0000002a.bin are named, tags-00000029.bin and waves-00000029.bin aren't; "
         "step 1 left manifest.tmp; 0000002a and 0000002c "
         "are listed and no longer than their size, 0000002b is longer, 0000002d isn't listed, resume/old/ is a "
         "folder; the private files stay"),
        (listing, root, None, every, "no resume.json: every partial goes"),
        (listing, root, resume[:40], every, "a torn resume.json keeps nothing"),
        ({"resume/0000002a.part": 1, "resume/0000002b.part": 1}, root,
         private_text(RESUME_FORMAT, 2, parts=[dict(part, size=5), dict(part, size=9), dict(own, name="bad.part")]),
         ["resume/0000002a.part", "resume/0000002b.part"],
         "a name listed twice keeps nothing; an entry whose name isn't a partial's keeps nothing"),
    ]
    now = lambda e, **kw: dict({"serverPath": e["serverPath"], "file-size": e["size"],  # noqa: E731
                                "modified": e["modified"]}, **kw)
    by_etag = {"If-Range": part["etag"]}
    fetch = [
        (part, 10485760, now(part), 206, 10485760, by_etag, "continue",
         "the server's file unchanged: on from have, by its ETag"),
        (part, 12000000, now(part), 206, 10485760, by_etag, "continue",
         "the bytes past have were never flushed: cut back to have"),
        (part, 8000000, now(part), 200, 8000000, by_etag, "start over",
         "shorter than have; the server answers 200: its file changed"),
        (own, 4194304, now(own), 206, 4194304, {"If-Range": "Mon, 21 Sep 2026 14:13:20 GMT"}, "continue",
         "no ETag: If-Range carries the HTTP date of modified (1790000000123 ms)"),
        (part, 10485760, now(part, **{"file-size": part["size"] + 1}), None, None, None, "start over",
         "the server's size changed: start over without asking"),
        (part, 10485760, now(part, modified=part["modified"] + 1), None, None, None, "start over",
         "modified changed: start over"),
        (dict(part, have=-5), 10485760, now(part), None, None, None, "start over",
         "have below 0: not a count, so start over (step 4 kept the file by its name and size)"),
        (dict(part, have=part["size"] + 1), 10485760, now(part), None, None, None, "start over",
         "have past size: start over"),
        (dict(part, have=10485760.0), 10485760, now(part), None, None, None, "start over",
         "have not an integer: start over"),
        (dict(part, have="10485760"), 10485760, now(part), None, None, None, "start over",
         "have a string: start over"),
        (dict(part, etag=5), 10485760, now(part), None, None, None, "start over",
         "etag not a string: start over"),
    ]
    return {
        "who": "software",
        "note": ("2.12.7, 2.12.8: the software's private files in /.mstream. names: may a private file have this "
                 "path ('/' between names), and why. settle: the read with its recovery for X = stem, the two "
                 "files' text (null: no file): which counts, its seq and the next write's, what a run deletes, "
                 "whether it finishes the rename, whether step 3 stops for a disk check, whether an unknown "
                 "version stops the software. cleanup: step 4 over a /.mstream listing (path: size), the "
                 "companions the root names and the settled resume.json: the paths it deletes, every other file "
                 "staying. resumeFetch: a listed partial at fetch time, its size on the card, the server's entry "
                 "for the file now and the answer to the ranged GET: where it goes on from (null: it starts "
                 "over) and the request's headers. run: what run.lock holds (informational, never trusted)."),
        "names": [{"name": n, "ok": ok, "why": private_name(n)[1]} for n, ok in names],
        "settle": [{"stem": s, "json": j, "new": nw, "pick": p, "seq": q, "nextSeq": (q or 0) + 1, "delete": d,
                    "rename": r, "diskCheck": dc, "stops": stop, "note": note}
                   for s, j, nw, p, q, d, r, dc, stop, note in settle],
        "cleanup": [{"files": f, "root": r, "resume": rj, "delete": d, "note": n} for f, r, rj, d, n in clean],
        "resumeFetch": [{"entry": e, "partSize": size, "server": srv, "status": status, "from": at,
                         "headers": None if at is None else dict({"Range": "bytes=%d-" % at}, **h), "then": then,
                         "note": n} for e, size, srv, status, at, h, then, n in fetch],
        "resumeExample": json.loads(resume),
        "run": {"format": RUN_FORMAT, "runId": "0x00C0FFEE00C0FFEE", "started": 1791900000,
                "producer": "mstream-terminal 0.14.0"},
    }


def names_vectors():
    """2.8.3-2.8.6 as runs of tools/cardnames.py's namer: each a card (its
    layout, device.txt's extensions, what it lists) and the files one run
    names on it. The names come from the namer; the spec's own examples
    are asserted here first, typed from 2.8.3's arithmetic."""
    mk = cardnames.mark
    exts = "mp3,flac,opus"

    def f(server, id_, path, **kw):
        return dict({"server": server, "id": id_, "serverPath": path}, **kw)

    a300, euros, b200, c200 = "a" * 300, "€" * 100, "B" * 200, "c" * 200
    f250, t100, t200, u80, g247, l300 = "F" * 250, "t" * 100, "t" * 200, "u" * 80, "G" * 247, "L" * 300
    cut_album = "F" * 177 + mk(f250)            # ends 64 bytes short of 255: 7 + 2 + 182 = 191
    runs = [
        {"note": "steps 1-4 on an empty card: the vpath dropped, names made safe, the extension gated, "
                 "folders past the 8th joined",
         "files": [f(0, 1, "Music/Artist/Album/01 - Title.mp3"),
                   f(0, 2, "Music/a\"b*c:d<e>f?g\\h|i/x.mp3"),
                   f(0, 3, "Music/  Lead/Trail. . /.dot/x.flac"),
                   f(0, 4, "Music/CON/aux.mp3"),
                   f(0, 5, "Music/Café/x.opus"),
                   f(0, 6, "Music/tab\there/\u0001x.mp3"),
                   f(0, 7, "Music/1/2/3/4/5/6/7/8/9/10/x.mp3"),
                   f(0, 8, "Music/A/x.ogg", bytes="oggOpus"),
                   f(0, 9, "Music/A/y.ogg"),
                   f(0, 10, "Music/A/z.wav", bytes="opus"),
                   f(0, 11, "Music/A/w.m4a"),
                   f(0, 12, "Music/A/huge.flac", size=1 << 32),
                   f(0, 13, "Music"),
                   f(0, 14, "Music/A/cover.jpg", bytes="notAudio"),
                   f(0, 15, "Music/A/con.flac", bytes="mp3")],
         "spec": ["Artist/Album/01 - Title.mp3", "a_b_c_d_e_f_g_h_i/x.mp3", "Lead/Trail/_dot/x.flac",
                  "CON_/aux_.mp3", "Café/x.opus", "tab_here/_x.mp3", "1/2/3/4/5/6/7/8 - 9 - 10/x.mp3",
                  "A/x.opus", {"refused": "unlisted"}, "A/z.opus", {"refused": "unlisted"},
                  {"refused": "tooBig"}, {"refused": "noName"}, "A/cover.jpg", "A/con_.mp3"]},
        {"note": "a card with no device.txt: mp3 and flac only (2.15)", "extensions": "mp3,flac",
         "files": [f(0, 1, "V/A/x.opus"), f(0, 2, "V/A/x.ogg", bytes="oggOpus"), f(0, 3, "V/A/x.flac")],
         "spec": [{"refused": "unlisted"}, {"refused": "unlisted"}, "A/x.flac"]},
        {"note": "step 5: the file is the longest, its stem cut and marked with the stem's hash, to 255 bytes "
                 "with /music/ (at a code point boundary: 254 for 3-byte characters); a tie goes to the deepest",
         "files": [f(0, 1, "V/Artist/Album/%s.mp3" % a300), f(0, 2, "V/Artist/Album/%s.mp3" % euros),
                   f(0, 3, "V/%s/%s.mp3" % (b200, c200))],
         "spec": ["Artist/Album/%s%s.mp3" % ("a" * 226, mk(a300)), "Artist/Album/%s%s.mp3" % (euros[:75], mk(euros)),
                  "%s/%s%s.mp3" % (b200, "c" * 38, mk(c200))]},
        {"note": "step 5: the album folder is the longest: cut to end 64 bytes short of 255, so a sibling of 63 "
                 "bytes fits whole and a longer one is cut itself",
         "files": [f(0, 1, "V/A/%s/x.mp3" % f250), f(0, 2, "V/A/%s/%s.mp3" % (f250, "s" * 59)),
                   f(0, 3, "V/A/%s/%s.mp3" % (f250, t100))],
         "spec": ["A/%s/x.mp3" % cut_album, "A/%s/%s.mp3" % (cut_album, "s" * 59),
                  "A/%s/%s%s.mp3" % (cut_album, "t" * 54, mk(t100))]},
        {"note": "the same album first named by a long file: the folder's cut is the same",
         "files": [f(0, 1, "V/A/%s/%s.mp3" % (f250, t200))],
         "spec": ["A/%s/%s%s.mp3" % (cut_album, "t" * 54, mk(t200))]},
        {"note": "a folder still the longest name once 64 bytes short is cut again, as far as its file needs",
         "files": [f(0, 1, "V/A/%s/%s.mp3" % (f250, u80))],
         "spec": ["A/%s%s/%s.mp3" % ("F" * 156, mk(f250), u80)]},
        {"note": "a later run, the card listing the cut folder and no ledger row: its files join it",
         "card": [{"path": "A/%s/x.mp3" % cut_album}],
         "files": [f(0, 2, "V/A/%s/%s.mp3" % (f250, t200))],
         "spec": ["A/%s/%s%s.mp3" % (cut_album, "t" * 54, mk(t200))]},
        {"note": "a folder the card has that leaves no room below it: refused, not misnamed",
         "card": [{"path": g247, "folder": True}],
         "files": [f(0, 1, "V/%s/x.mp3" % g247)],
         "spec": [{"refused": "tooLong"}]},
        {"note": "step 6 in one run: by match key; the lower (server, mStream id) keeps the name, the lower "
                 "server first; an NFC and an NFD spelling are one name; folders merge",
         "files": [f(0, 7, "V/A/One.mp3"), f(0, 3, "V/A/one.MP3"), f(0, 5, "W/a/ONE.mp3"),
                   f(1, 1, "V/B/x.mp3"), f(0, 9, "V/B/x.mp3"),
                   f(0, 2, "V/Café.mp3"), f(0, 1, "V/Café.mp3")],
         "spec": ["A/One (3).mp3", "A/one.MP3", "A/ONE (2).mp3", "B/x (2).mp3", "B/x.mp3",
                  "Café (2).mp3", "Café.mp3"]},
        {"note": "step 6 against what the card lists: its folders keep their spelling and merge (an NFD one "
                 "too); a file named like the listener's file, a file named like a folder and a folder named "
                 "like a file each take ' (2)'",
         "card": [{"path": "Acme/Hits/01.mp3"}, {"path": "Mine/Song.mp3"}, {"path": "Mine/Live.mp3", "folder": True},
                  {"path": "Intro"}, {"path": "Café/a.mp3"}],
         "files": [f(0, 1, "Music/ACME/Hits/02.mp3"), f(0, 2, "Other/acme/HITS/03.mp3"),
                   f(0, 3, "V/Mine/song.mp3"), f(0, 4, "V/Mine/Live.mp3"), f(0, 5, "V/Intro/x.mp3"),
                   f(0, 6, "V/Intro/y.mp3"), f(0, 7, "V/Café/b.mp3")],
         "spec": ["Acme/Hits/02.mp3", "Acme/Hits/03.mp3", "Mine/song (2).mp3", "Mine/Live (2).mp3",
                  "Intro (2)/x.mp3", "Intro (2)/y.mp3", "Café/b.mp3"]},
        {"note": "the vpath layout (2.8.6): a root per library, in order of (server, vpath); the card's own "
                 "folder of a root's name becomes that root; another library of the same name takes ' (2)'; "
                 "the root is the first of the 8 levels",
         "layout": {"vpath": [{"server": 1, "vpath": "Music"}, {"server": 0, "vpath": "Music"},
                              {"server": 0, "vpath": "Audio Books"}, {"server": 0, "vpath": "a:b"}]},
         "card": [{"path": "music/mine.mp3"}],
         "files": [f(0, 1, "Music/A/x.mp3"), f(1, 1, "Music/A/x.mp3"), f(0, 2, "Audio Books/B/y.mp3"),
                   f(0, 3, "a:b/z.mp3"), f(2, 1, "Music/A/x.mp3"), f(0, 4, "Other/x.mp3"), f(0, 5, "Music"),
                   f(0, 6, "Music/1/2/3/4/5/6/7/8/9/10/x.mp3")],
         "spec": ["music/A/x.mp3", "Music (2)/A/x.mp3", "Audio Books/B/y.mp3", "a_b/z.mp3", {"refused": "noRoot"},
                  {"refused": "noRoot"}, {"refused": "noName"}, "music/1/2/3/4/5/6/7 - 8 - 9 - 10/x.mp3"]},
        {"note": "a vpath too long for a root: cut as a folder, to end 64 bytes short of 255, whatever is below",
         "layout": {"vpath": [{"server": 0, "vpath": l300}]},
         "files": [f(0, 1, "%s/A/x.mp3" % l300)],
         "spec": ["%s%s/A/x.mp3" % ("L" * 179, mk(l300))]},
    ]
    assert len("/music/" + runs[2]["spec"][0]) == 255 and len(("/music/" + runs[2]["spec"][1]).encode()) == 254
    assert len("/music/A/" + cut_album) == 255 - cardnames.ROOM_BELOW_CUT
    assert len("/music/" + runs[3]["spec"][2]) == 255 and len("/music/" + runs[5]["spec"][0]) == 255
    assert len("/music/" + runs[-1]["spec"][0].split("/")[0]) == 255 - cardnames.ROOM_BELOW_CUT
    out = []
    for r in runs:
        case = {"note": r["note"], "layout": r.get("layout", "default"), "extensions": r.get("extensions", exts)}
        if "card" in r:
            case["card"] = r["card"]
        case["files"] = r["files"]
        case["names"] = cardnames.run(case)
        assert case["names"] == r["spec"], (r["note"], case["names"])
        out.append(case)
    marks = [{"name": n, "component": c, "mark": mk(c)}
             for n, c in [("01 - Title.mp3", "01 - Title"), ("Album", "Album"), ("Café", "Café"),
                          ("8 - 9 - 10", "8 - 9 - 10")]]
    return {"who": "software",
            "note": ("2.8.3-2.8.6: card paths from mStream's. marks: step 5's mark of a name: the FNV-1a 64 of "
                     "the name as steps 2-4 left it, uncut (a file's stem, without its extension). runs: one run "
                     "on a card each: its layout ('default', or the vpath layout's libraries), device.txt's "
                     "extensions, what the card lists ('folder': a folder, else a file; relative to /music, as "
                     "stored), and the files the run names ('bytes': original when absent, oggOpus, a "
                     "conversion's format, or notAudio; 'size' 4,000,000 when absent), named in order of "
                     "(server, id). names: each file's path relative to /music, in the files' order, or "
                     "{'refused': why}: noName, noRoot, unlisted, tooBig, tooLong, folderFull. Made by "
                     "tools/cardnames.py."),
            "marks": marks, "runs": out}


def vectors(golden: dict):
    v = {}
    v["note"] = ("docs/METADATA.md 2.18 as data. 'who': both (every implementation), software (the transfer "
                 "software only), builder (the device's builder, milestone N2). Hex strings are integers; "
                 "byte strings are space-separated hex.")
    v["crc32"] = [{"ascii": "123456789", "crc": "0xCBF43926"}]
    v["fnv1a64"] = [{"utf8": s, "hash": h} for s, h in [
        ("", "0xCBF29CE484222325"), ("a", "0xAF63DC4C8601EC8C"), ("/music", "0x75DC8A6A38687865"),
        ("/music/Artist/Album", "0xB1F7E69FBD466B59"), ("/music/Artist/Album/01 - Title.mp3", "0xDA70B312760188D8"),
        ("/music/" + NFC_CAFE + "/Album/01 - Title.mp3", "0xF1B24FC757494F2B"),
        ("/music/" + NFD_CAFE + "/Album/01 - Title.mp3", "0x57017C0CC011BB1B"),
        ("/music/Artist", "0x4296E8541CC39B71"), ("/music/Artist/Album/CD1", "0xD9FA96D903A5A10C"),
        ("/Music", "0x359C71D0AA7DCAC5")]]
    v["pathHash"] = [{"rel": r, "hash": h, "note": n} for r, h, n in [
        ("", "0x75DC8A6A38687865", "/music itself; a card storing 'Music' still hashes as '/music'"),
        ("Artist/Album", "0xB1F7E69FBD466B59", ""),
        ("Artist/Album/01 - Title.mp3", "0xDA70B312760188D8", "")]]
    v["serverUrlKey"] = [{"url": "HTTP://Music.Example:3000/", "normalised": "http://music.example:3000",
                          "key": "0xED901EA3EE763AC7"},
                         {"url": "https://user:pw@Host.Example:443/mstream/?q=1#x",
                          "normalised": "https://host.example/mstream", "key": "0x%016X" % fnv(b"https://host.example/mstream")},
                         {"url": "http://10.0.0.5:80", "normalised": "http://10.0.0.5",
                          "key": "0x%016X" % fnv(b"http://10.0.0.5")}]
    v["qfp"] = [{"size": n, "qfp": q} for n, q in [
        (0, "0xA8C7F832281A39C5"), (100, "0xB708DC48BA0A842D"), (4096, "0x636A94FE9C19DC15"),
        (4097, "0xA76BF84EC13A75BC"), (5000, "0xC8E651224ADA889C"), (8192, "0xA9383C4532F6F525"),
        (8193, "0xD70E2B23544A7444"), (10000, "0xF17B194EF7F5F338")]]
    v["qfpNote"] = "files whose byte i is i & 0xFF"
    v["fatTime"] = [
        {"time": "2026-10-07 14:30:42", "fatTime": "0x5D4773D5"},
        {"time": "2026-10-07 15:30:42", "fatTime": "0x5D477BD5"},
        {"time": "2026-10-07 14:30:43", "fatTime": "0x5D4773D5"},
    ]
    v["fatTimeInvalid"] = ["0x00000000", "0x5C0773D5"]
    v["wallDelta"] = [{"a": "0x5D4773D5", "b": "0x5D477BD5", "delta": 3600}]
    v["skew"] = [
        {"groups": [{"count": 20, "delta": 3600}], "skew": 3600, "matching": 20},
        {"groups": [{"count": 7, "delta": 3600}], "skew": 0, "matching": 0},
        {"groups": [{"count": 10, "delta": 3600}, {"count": 20, "delta": 0}], "skew": 0, "matching": 20},
        {"groups": [{"count": 20, "delta": 2}], "skew": 0, "matching": 0},
        {"groups": [{"count": 10, "delta": 3600}, {"count": 10, "delta": -3600}], "skew": -3600, "matching": 10},
        # Left out of the pairs: 8 of 8 is a skew (counted, 8 of 18 wouldn't
        # be half), and the left-out pairs never match.
        {"groups": [{"count": 8, "delta": 3600}, {"count": 10, "recorded": "0x00000000"}], "skew": 3600,
         "matching": 8},
        {"groups": [{"count": 8, "delta": 3600}, {"count": 10, "observed": "0x00000000"}], "skew": 3600,
         "matching": 8},
        {"groups": [{"count": 8, "delta": 3600}, {"count": 10, "recorded": "0x00000000", "observed": "0x00000000"}],
         "skew": 3600, "matching": 8},
        {"groups": [{"count": 8, "delta": 3600}, {"count": 10, "observed": "0x5C0773D5"}], "skew": 3600,
         "matching": 8},
        {"groups": [{"count": 8, "delta": 3600}, {"count": 10, "recorded": "0x5C0773D5", "observed": "0x5C0773D5"}],
         "skew": 3600, "matching": 8},
    ]
    v["skewNote"] = ("pairs: recorded = 2026-10-07 14:30:42 + 2 days x i, observed = recorded + delta (0 when the "
                     "group gives none), unless the group pins 'recorded' or 'observed' to a stamp (0, or the "
                     "invalid 0x5C0773D5, month 0); 'matching' counts the pairs whose time matches under the skew. "
                     "A pair with a 0 or invalid stamp is left out of the pairs (so it doesn't count towards half) "
                     "and never matches, even when its two stamps are equal.")
    v["bytes"] = {
        "MPTG": "4D 50 54 47", "MPTGu32": "0x4754504D", "MPTHu32": "0x4854504D",
        "thumbPath": {"folder": "Artist/Album", "path": "/.mstream/thumbs/B/B1F7E69F.565", "bytes": 21656,
                      "head0to15": "4D 50 54 48 01 00 00 00 59 6B 46 BD 9F E6 F7 B1", "head20to23": "28 00 60 00"},
        "serverInstance": {"uuid": "00112233-4455-6677-8899-aabbccddeeff",
                           "stored": "00 11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF"},
        "audioHash": {"audioHash": "0123456789abcdef0123456789abcdef",
                      "stored": "01 23 45 67 89 AB CD EF 01 23 45 67 89 AB CD EF"},
        "selectionSigStored": " ".join("%02X" % b for b in golden["autodj-00000029.bin"][64:80]),
    }
    v["canonicalOrder"] = {
        "siblings": ["A", "A B", "A-", "B", "a", "\u00c9"],
        "files": ["A/x.mp3", "A/B/y.mp3", "A B/y.mp3"],
        "note": "siblings in byte order; files in the walk's order (a folder's files before its subfolders; "
                "'A' and its subfolders before 'A B')",
    }
    v["stringRuns"] = [
        {"fields": {"title": ["T"]}, "run": "01 54 00"},
        {"fields": {"artist": ["A"]}, "run": "02 00 41 00"},
        {"fields": {}, "run": "", "note": "no field: strings = 0"},
        {"fields": {"artist": ["X", "X", "Y"]}, "run": "02 00 58 1F 59 00"},
    ]
    v["fieldRules"] = [
        {"field": "title", "values": ["A\tB"], "stored": "A B", "truncated": False},
        {"field": "title", "values": ["t" * 300], "stored": "t" * 255, "truncated": True},
        {"field": "title", "values": ["t" * 254 + "\u00e9"], "stored": "t" * 254, "truncated": True},
        {"field": "artist", "values": [c * 204 for c in "PQRST"], "stored": "\u001f".join(c * 204 for c in "PQRS"),
         "truncated": True, "bytes": 819},
        {"field": "genre", "values": ["g" * 255, "h" * 255, "i" * 255, "j" * 250, "k" * 10, "l"],
         "stored": "\u001f".join(["g" * 255, "h" * 255, "i" * 255, "j" * 250]), "truncated": True, "bytes": 1018},
        {"field": "artist", "values": ["", "B", ""], "stored": "B", "truncated": False},
        {"field": "title", "values": ["", "Second", "Third"], "stored": "Second", "truncated": False,
         "note": "a single field: the first value left after the empty ones go"},
    ]
    v["numbers"] = [
        {"kind": "gain", "text": "-6.785 dB", "value": -679},
        {"kind": "gain", "text": "1.005 dB", "value": 101},
        {"kind": "gain", "text": "-6.5 DB", "value": -650},
        {"kind": "gain", "text": "-400 dB", "value": None},
        {"kind": "gain", "text": "inf", "value": None},
        {"kind": "gain", "text": "1e2", "value": None},
        {"kind": "r128", "q": -1312, "value": -13},
        {"kind": "r128", "q": 32, "value": 513},
        {"kind": "r128", "q": 0, "value": 500},
        {"kind": "r128", "q": -5888, "value": -1800},
        {"kind": "peak", "text": "0.988567", "value": 9886},
        {"kind": "peak", "text": "7", "value": 65535},
        {"kind": "bpm", "text": "120.5", "value": 1210},
        {"kind": "bpm", "text": "19.5", "value": 200},
        {"kind": "bpm", "text": "300.5", "value": None},
        {"kind": "bpm", "text": "0x78", "value": None},
        {"kind": "bpm", "text": "120 BPM", "value": None},
    ]
    v["hashSampled"] = [
        {"fileSize": 26214399, "hashV": 2, "sampled": False},
        {"fileSize": 26214400, "hashV": 2, "sampled": True},
        {"fileSize": 30000000, "hashV": 1, "sampled": False},
    ]
    v["rootElection"] = [
        {"bin": {"valid": True, "generation": 5, "commitId": 1, "headerCrc": 10},
         "tmp": {"valid": True, "generation": 6, "commitId": 2, "headerCrc": 20}, "pick": "tmp", "sameCommit": False},
        {"bin": {"valid": True, "generation": 5, "commitId": 1, "headerCrc": 10},
         "tmp": {"valid": True, "generation": 5, "commitId": 2, "headerCrc": 20}, "pick": "bin", "sameCommit": False},
        {"bin": {"valid": True, "generation": 5, "commitId": 9, "headerCrc": 77},
         "tmp": {"valid": True, "generation": 5, "commitId": 9, "headerCrc": 77}, "pick": "bin", "sameCommit": True,
         "note": "one commit seen twice (a cut rename): readers use either; the software deletes neither"},
        {"bin": {"valid": False}, "tmp": {"valid": True, "generation": 6, "commitId": 2, "headerCrc": 20},
         "pick": "tmp", "sameCommit": False},
        {"bin": {"valid": True, "generation": 6, "commitId": 1, "headerCrc": 10},
         "tmp": {"valid": True, "generation": 5, "commitId": 2, "headerCrc": 20}, "pick": "bin", "sameCommit": False},
        {"bin": {"valid": False}, "tmp": {"valid": False, "major": 2}, "pick": "none", "sameCommit": False,
         "softwareRefuses": True, "note": "no bin, a tmp of major 2"},
    ]
    v["albumFolder"] = [
        {"rel": "Artist/Album/CD1/01.flac", "album": "Artist/Album", "hash": "0xB1F7E69FBD466B59"},
        {"rel": "Artist/x.mp3", "album": "Artist", "hash": "0x4296E8541CC39B71"},
        {"rel": "Artist/Album/01.flac", "album": "Artist/Album", "hash": "0xB1F7E69FBD466B59"},
        {"rel": "x.mp3", "album": "", "hash": "0x75DC8A6A38687865"},
        {"rel": "Lib A/Artist/Album/CD1/01.flac", "roots": ["Lib A"], "album": "Lib A/Artist/Album",
         "hash": "0x%016X" % path_hash("Lib A/Artist/Album")},
    ]
    v["builderPrecedence"] = {"who": "builder", "cases": [
        "the walk (s, t) and T (s, t): T",
        "T (s, t - 3600) under a skew of +3600: T",
        "T (s, t - 2), no skew: qfp; equal: T, and the confirmation is saved",
        "T (s + 1, t) and a Scanned D record (s + 1, t): D",
        "T UNREADABLE without FROM_API, and the device reads the tags: D",
        "a Scanned D record of an older parserVersion, no T: path names, and the file goes Pending",
        "the walk's Acme/x.mp3 against T's ACME/x.mp3: no T match on the device"]}
    v["softwareMatching"] = {"who": "software", "cases": [
        "the ledger has ACME/Hits/01.mp3; the card lists Acme/Hits/01.mp3 with an equal size and time: unchanged, "
        "and the ledger takes the spelling Acme/...; no '01 (2).mp3'",
        "a plan's write target that is on the card but not in the ledger is replaced in place",
        "on macOS, a listed 'Cafe' + U+0301 is recorded in NFC (C3 A9), hash F1B24FC757494F2B",
        "an Opus stream the server stores as x.ogg goes on the card as x.opus, its bytes unchanged, convertedTo 0"]}
    v["names"] = names_vectors()
    other = "00112233-4455-4677-8899-aabbccddeeff"
    v["servers"] = {
        "who": "software",
        "note": ("2.5.5, 2.6.6, 2.11: a ledger row's server. origServer: ORIG's bytes 74-79 for a row's server "
                 "index. rows: the golden tags file's records in record order with their server. srvs: the golden "
                 "root's SRVS entries (entry i - 1 is server i), as stored. sameServer: 2.11's test of server "
                 "'index' of the card whose root description is 'card' against what the software reads: the "
                 "server at 'url' ('serverInstance': its instance, when known; 'confirmed': keys the listener has "
                 "called the same server as 'url'), or, with 'peerId', that peer of it ('peerInstance': the "
                 "peer's instance, when known). brokenEntries: the same test on cards whose SRVS breaks a writer "
                 "rule of 2.5.5, each 'card' a root description's server fields: the entry's server is unknown, "
                 "the root stays valid. The device reads neither ORIG nor SRVS."),
        "origServer": [{"server": 0, "bytes74to79": "00 00 00 00 00 00", "note": "a card filled from one server"},
                       {"server": 2, "bytes74to79": "02 00 00 00 00 00"},
                       {"server": 258, "bytes74to79": "02 01 00 00 00 00"}],
        "rows": {"file": "tags-00000007.bin", "rows": [
            {"path": "Artist/Album/01 - Title.mp3", "server": 0},
            {"path": "Artist/Album/cover.jpg", "server": 0},
            {"path": "Both/One (2).mp3", "server": 1},
            {"path": "Both/One.mp3", "server": 0},
            {"path": "Far Artist/Far Album/01 - Far.opus", "server": 3},
            {"path": "Peer Artist/Peer Album/01 - Tune.opus", "server": 2},
            {"path": "Peer Artist/Peer Album/cover.jpg", "server": 2},
            {"path": "Second Artist/Album/01 - Song.flac", "server": 1}]},
        "srvs": {"file": "manifest-servers.bin", "stride": 40, "entries": [
            {"server": 1, "stored": "88 99 AA BB CC DD 4E FF 80 11 22 33 44 55 66 77 6E 9D 52 CA 80 E7 CA 36 "
                                    "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"},
            {"server": 2, "stored": "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 "
                                    "C7 3A 76 EE A3 1E 90 ED 03 00 00 00 01 00 00 00"},
            {"server": 3, "stored": "0F 1E 2D 3C 4B 5A 49 68 87 96 A5 B4 C3 D2 E1 F0 00 00 00 00 00 00 00 00 "
                                    "6E 9D 52 CA 80 E7 CA 36 01 00 00 00 01 00 00 00"}]},
        "sameServer": {"card": "manifest-servers.json", "cases": [
            {"index": 0, "talk": {"url": HOME_URL}, "verdict": "same"},
            {"index": 0, "talk": {"url": "HTTP://Music.Example:3000/"}, "verdict": "same",
             "note": "the URL normalised first (2.5.1)"},
            {"index": 0, "talk": {"url": "http://192.0.2.10:3000"}, "verdict": "unknown",
             "note": "a second URL of the same server: another key"},
            {"index": 0, "talk": {"url": "http://192.0.2.10:3000",
                                  "confirmed": ["0x%016X" % server_url_key(HOME_URL)]},
             "verdict": "same", "note": "the listener has called the two the same server"},
            {"index": 0, "talk": {"url": HOME_URL, "peerId": 3}, "verdict": "unknown",
             "note": "a peer is not its parent"},
            {"index": 1, "talk": {"url": HOME_URL}, "verdict": "unknown"},
            {"index": 1, "talk": {"url": SECOND_URL}, "verdict": "same"},
            {"index": 1, "talk": {"url": SECOND_URL, "serverInstance": other}, "verdict": "different",
             "note": "both instances known and different, whatever the keys"},
            {"index": 1, "talk": {"url": "http://192.0.2.20", "serverInstance": SECOND_INSTANCE}, "verdict": "same",
             "note": "both instances known and equal, whatever the keys"},
            {"index": 2, "talk": {"url": HOME_URL, "peerId": 3}, "verdict": "same"},
            {"index": 2, "talk": {"url": HOME_URL, "peerId": 4}, "verdict": "unknown"},
            {"index": 2, "talk": {"url": HOME_URL}, "verdict": "unknown", "note": "the parent is not its peer"},
            {"index": 2, "talk": {"url": SECOND_URL, "peerId": 3}, "verdict": "unknown",
             "note": "peer 3 of another parent"},
            {"index": 2, "talk": {"url": "http://192.0.2.10:3000", "serverInstance": other, "peerId": 3},
             "verdict": "unknown", "note": "its parent at a new URL: the root's instance is unknown, so only a key "
                                           "the listener confirmed ties the URL to it"},
            {"index": 2, "talk": {"url": "http://192.0.2.10:3000", "peerId": 3,
                                  "confirmed": ["0x%016X" % server_url_key(HOME_URL)]}, "verdict": "same"},
            {"index": 3, "talk": {"url": SECOND_URL, "peerId": 1}, "verdict": "same"},
            {"index": 3, "talk": {"url": SECOND_URL, "peerId": 1, "peerInstance": other}, "verdict": "different"},
            {"index": 3, "talk": {"url": HOME_URL, "peerId": 1}, "verdict": "unknown"},
            {"index": 3, "talk": {"url": "http://192.0.2.20", "serverInstance": SECOND_INSTANCE, "peerId": 1},
             "verdict": "same", "note": "its parent at a new URL, known by its instance (server 1's)"},
            {"index": 3, "talk": {"url": SECOND_URL, "serverInstance": other, "peerId": 1}, "verdict": "unknown",
             "note": "the server at its parent's URL is another server now, by instance: not its parent"},
            {"index": 4, "talk": {"url": HOME_URL}, "verdict": "unknown", "note": "a server no SRVS entry names"},
            {"index": 65535, "talk": {"url": HOME_URL}, "verdict": "unknown"}]},
        "brokenEntries": {"cases": broken_server_cases()},
    }
    v["privateFiles"] = private_vectors()
    _, snaps, ops = plays_files(golden)
    v["plays"] = plays_vectors(golden, snaps, ops)
    v["deviceTxt"] = {"text": device_txt(device_main()).decode("ascii"),
                      "noFile": {"read.msmf": "1", "read.mptg": "1", "read.mpdj": "", "read.mpwf": "",
                                 "read.mpth": "1", "codecs": "mp3,flac", "extensions": "mp3,flac", "max_rate": "48000",
                                 "max_channels": "2"}}
    v["mpdj"] = mpdj.fixture_vectors(golden)
    v["mpwf"] = mpwf.fixture_vectors(golden)
    v["mpwf"]["rootRow"] = {
        "note": "WAVEFORM-INDEX.md 7.2: what a reader takes from a root's COMP. waves: the MPWF row's name, null for "
                "none; matches: the row against the golden file of that name. Each hardening file's root reads.",
        "golden": {"file": "manifest-waves.bin", "waves": "waves-0000002a.bin", "matches": True},
        "hardening": [{"file": name + ".bin", "waves": waves, "matches": matches, "note": note}
                      for name, (waves, matches, note) in WAVES_ROOTS.items()]}
    return v


def card_server(root, index, rules=True):
    """Server `index` of a root description (2.5.5): 0 the header's, i >= 1
    SRVS entry i - 1; None when no entry has that index, or when its entry
    breaks a writer rule (to a reader, that server is unknown; rules=False
    reads it anyway, for the vectors' own check)."""
    if index == 0:
        return root_server(root)
    entries = [server_entry(s) for s in root.get("servers", [])]
    if index > len(entries) or (rules and index in srvs_breaks(root_server(root), entries)):
        return None
    return entries[index - 1]


def card_parent(root, key, rules=True):
    """A peer's parent on the card (2.11): the index of the server reached by
    its own URL whose serverUrlKey is `key`, or None."""
    for i in range(len(root.get("servers", [])) + 1):
        s = card_server(root, i, rules)
        if key and s and not s["flags"] & PEER and s["serverUrlKey"] == key:
            return i
    return None


def same_server(root, index, talk, rules=True):
    """2.11's test of server `index` of a card (its root description) against
    what the software reads. talk: the server at "url" ("serverInstance": its
    instance, when known; "confirmed": keys the listener has called the same
    server as it), or, with "peerId", that peer of it ("peerInstance": the
    peer's instance, when known)."""
    srv = card_server(root, index, rules)
    if srv is None:
        return "unknown"
    peer = "peerId" in talk
    a, b = srv["instance"], uuid_bytes(talk.get("peerInstance" if peer else "serverInstance"))
    if a != bytes(16) and b != bytes(16):
        return "same" if a == b else "different"
    # Reading a server never makes its peers' rows the same, nor reading a
    # peer its parent's.
    if bool(srv["flags"] & PEER) != peer:
        return "unknown"
    via = {k: talk[k] for k in ("url", "serverInstance", "confirmed") if k in talk}
    if not peer:
        keys = {server_url_key(via["url"])} | {u64(k) for k in via.get("confirmed", [])}
        return "same" if srv["serverUrlKey"] in keys else "unknown"
    # A peer: its parent is the card's server of parentKey when the card has
    # one, tested as that server (instance, then key); else parentKey alone
    # by the key rule.
    parent = card_parent(root, srv["parentKey"], rules)
    if parent is not None:
        through = same_server(root, parent, via, rules) == "same"
    else:
        through = same_server({"serverUrlKey": srv["parentKey"]}, 0, via) == "same"
    return "same" if through and srv["peerId"] == talk["peerId"] else "unknown"


def ledger_servers(b: bytes):
    """A tags file read back: (path, ORIG's server) per record, in record
    order."""
    p = parse_container(b)
    strs = bytes(section(p, "STRS")[4])

    def text(off):
        return strs[off:strs.index(0, off)].decode("utf-8") if off else ""

    def join(a, n):
        return a + "/" + n if a else n

    fold, folders = section(p, "FOLD"), []
    for i in range(fold[2]):
        parent, name = struct.unpack_from("<II", fold[4], fold[3] * i)
        folders.append("" if i == 0 else join(folders[parent], text(name)))
    recs, orig = section(p, "RECS"), section(p, "ORIG")
    out = []
    for i in range(recs[2]):
        f, n = struct.unpack_from("<II", recs[4], recs[3] * i)
        out.append((join(folders[f], text(n)), struct.unpack_from("<H", orig[4], orig[3] * i + 74)[0]))
    return out


def elect(b, t):
    """2.5.4: (which root counts, whether the two are one commit)."""
    if b["valid"] and t["valid"]:
        same = b["commitId"] == t["commitId"] and b["headerCrc"] == t["headerCrc"]
        return ("tmp" if t["generation"] > b["generation"] else "bin"), same
    return ("bin" if b["valid"] else "tmp" if t["valid"] else "none"), False


def hexint(s):
    return int(s, 16)


def root_acks(b: bytes):
    """ACKS as a reader that uses it takes it from a root (2.5.6): its
    entries [(deviceId hex, epoch, seq)], or None when the root has none or
    the section breaks a rule (its CRC, stride, length or order): then it
    is ignored, and the root stays valid."""
    hb, n = struct.unpack_from("<II", b, 8)
    for i in range(n):
        t, _, off, ln, count, stride, c, _ = struct.unpack_from("<4sIIIIIII", b, hb + 32 * i)
        if t == b"ACKS":
            data = bytes(b[off:off + ln])
            return playlog.read_acks(data, count, stride) if crc(data) == c else None
    return None


# What a reader takes from each root that reads with a waves row out of the
# common (WAVEFORM-INDEX.md 7.2): (the row's name or None, whether it matches
# the golden file of that name, what the file shows).
WAVES_ROOTS = {
    "msmf-waves-no-mpdj": ("waves-0000002a.bin", True, "no MPDJ row: the root's second row"),
    "msmf-waves-between": ("waves-0000002a.bin", True, "between MPTG and MPDJ: readers don't check its place"),
    "msmf-waves-first": ("waves-0000002a.bin", True, "before MPTG: readers don't check its place"),
    "msmf-waves-name-upper": (None, None, "waves-0000002A.bin: uppercase hex"),
    "msmf-waves-name-generation": (None, None, "the name's digits aren't the row's generation"),
    "msmf-waves-name-stem": (None, None, "wave-0000002a.bin: another stem"),
    "msmf-waves-name-empty": (None, None, "the name's offset 0: the empty string"),
    "msmf-waves-two": (None, None, "two MPWF rows: neither counts"),
    "msmf-waves-key": ("waves-0000002a.bin", False, "the key isn't zeros: the row reads, the file doesn't match"),
    "msmf-waves-file-bytes": ("waves-0000002a.bin", False, "fileBytes one more: the file doesn't match"),
    "msmf-waves-header-crc": ("waves-0000002a.bin", False, "headerCrc wrong: the file doesn't match"),
}


def root_waves(b: bytes):
    """The MPWF row a reader takes from a root (WAVEFORM-INDEX.md 7.2):
    (name, generation, fileBytes, headerCrc, key), or None when the root has
    none, has two, or its name isn't the rule's: then the root reads with no
    waves entry. Its place in COMP isn't checked."""
    p = parse_container(b)
    comp, strs = section(p, "COMP"), bytes(section(p, "STRS")[4])
    rows = []
    for i in range(comp[2]):
        kind, gen, fb, hc, name, _, key = struct.unpack_from("<4sIIIII16s", comp[4], comp[3] * i)
        if kind == b"MPWF":
            rows.append((strs[name:strs.index(0, name)].decode("utf-8"), gen, fb, hc, key))
    return rows[0] if len(rows) == 1 and rows[0][0] == mpwf.file_name(rows[0][1]) else None


def waves_matches(row, golden: dict):
    """A root's MPWF row against the golden file of its name."""
    return mpwf.companion_matches(golden[row[0]], *row[1:])


def check_plays(pv, golden: dict, want):
    """The plays group recomputed (2.20, 2.5.6)."""
    snaps, _ = run_plays(plays_script(golden))

    def log_of(path):
        return snaps[path.rsplit("/", 1)[1]]

    def hexbytes(s):
        return bytes.fromhex(s.replace(" ", ""))

    def unjson(f):
        return {k: (u64(x) if k in ("epoch", "pathHash") else x) for k, x in f.items()}
    slot = hexbytes(pv["slot"]["bytes"])
    want(playlog.decode(slot) == ("ok", unjson(pv["slot"]["fields"])) and
         playlog.encode(pv["slot"]["fields"]) == slot and
         crc(slot[:76]) == hexint(pv["slot"]["crc"]) and playlog.play_id(pv["slot"]["fields"]) == pv["slot"]["id"],
         "plays slot")
    note = hexbytes(pv["dropNote"]["bytes"])
    want(playlog.decode(note) == ("ok", unjson(pv["dropNote"]["fields"])) and
         playlog.encode(pv["dropNote"]["fields"]) == note, "plays drop note")
    for e in pv["ids"]:
        got = playlog.play_id({"deviceId": e["deviceId"], "epoch": e["epoch"], "seq": e["seq"]})
        want(got == e["id"] and len(got) <= 46, "play id " + e["id"])
    for o, n in pv["names"]["outcome"].items():
        want(playlog.outcome_name(int(o)) == n, "outcome %s" % o)
    for o, n in pv["names"]["source"].items():
        want(playlog.source_name(int(o)) == n, "source %s" % o)
    cases = {c[0]: c[1] for c in plays_read_cases(snaps)}
    for e in pv["atBoot"]["cases"]:
        slots = playlog.valid_slots(cases[e["readCase"]] if "readCase" in e else log_of(pv["atBoot"]["log"]))
        acks = [(a["deviceId"], u64(a["epoch"]), a["seq"]) for a in e.get("acks", [])]
        want(playlog.boot_seq(e["nvsSeq"], slots, e["deviceId"], e["epoch"], acks) == e["nextSeq"] and
             playlog.boot_count(e["nvsBoots"], slots, e["deviceId"], e["epoch"]) == e["nextBootSeq"], "atBoot %r" % e)
    a = pv["acks"]
    want(playlog.acks_bytes([(a["entry"]["deviceId"], a["entry"]["epoch"], a["entry"]["seq"])]) ==
         hexbytes(a["entry"]["stored"]), "acks entry")
    want(root_acks(golden[a["golden"]["file"]]) ==
         [(e["deviceId"], u64(e["epoch"]), e["seq"]) for e in a["golden"]["entries"]], "acks golden")
    for r in pv["resolve"]:
        logs = log_of(r["log"])
        log = playlog.read_log(logs)
        posted = {(e["deviceId"], u64(e["epoch"])): e["seq"] for e in r["posted"]}
        got = playlog.resolve(log["plays"], ledger_rows(golden[r["ledger"]]), r["skew"], posted)
        want([e["id"] for e in got] == [e["id"] for e in r["plays"]], "resolve ids " + r["log"])
        for g, e in zip(got, r["plays"]):
            ok = g["send"] == e["send"]
            if e["send"]:
                ok = ok and g["filePath"] == e["filePath"] and g["startedAt"] == e["startedAt"]
            else:
                ok = ok and g["why"].startswith({"file": "not", "clock": "no clock"}[e["why"]])
            want(ok, "resolve %s: %r" % (e["id"], g))
        marks = playlog.next_mark(log["plays"], got, posted, r["settled"])
        want(marks == {(e["deviceId"], u64(e["epoch"])): e["seq"] for e in r["marks"]}, "marks " + r["log"])
        want(log["counts"]["dropped"] == r.get("dropped", 0), "dropped " + r["log"])
    base = pv["resolveCasesBase"]
    rows = {x["path"]: x for x in ledger_rows(golden[base["ledger"]])}
    rec = rows[base["path"]]
    for e in pv["resolveCases"]:
        p = {"kind": 1, "deviceId": playlog.device_bytes(base["deviceId"]).hex(), "epoch": u64(base["epoch"]),
             "seq": base["seq"], "outcome": 1, "source": 1, "pauseCount": 0, "clock": 1, "startedAt": 1792200000000,
             "bootSeq": 1, "uptimeMs": 1000, "playedMs": 1000, "durationMs": 0, "pathHash": rec["pathHash"],
             "size": rec["size"], "fatTime": rec["fatTime"], "generation": 42}
        p.update({k: u64(x) for k, x in e["slot"].items()})
        posted = {(p["deviceId"], p["epoch"]): e["posted"]} if "posted" in e else None
        got = playlog.resolve([p], list(rows.values()), e["skew"], posted)
        if e["send"] is None:
            want(got == [], "resolveCase " + e["note"])
        else:
            want(len(got) == 1 and got[0]["send"] == e["send"] and (e["send"] or got[0]["why"].startswith(
                {"file": "not", "clock": "no clock"}[e["why"]])), "resolveCase " + e["note"])
    card = manifest_servers()
    rows7 = [x for x in ledger_rows(golden[pv["peers"]["ledger"]]) if x["serverPath"]]
    want([{"path": x["path"], "server": x["server"], "to": play_target(card, x["server"])} for x in rows7] ==
         pv["peers"]["rows"], "peers")
    for e in pv["deviceTxt"]:
        want(history_majors(e["text"]) == e["history"], "history %r" % e["text"])


def check_waves(w, golden: dict, want):
    """The mpwf group recomputed (docs/WAVEFORM-INDEX.md 13.3), and the fixture
    card's waveform index against its tags file."""
    for e in w["names"]:
        want(mpwf.file_name(e["generation"]) == e["name"], "mpwf name " + e["name"])
    for e in w["sizes"]:
        want(mpwf.size(e["rows"]) == e["fileBytes"], "mpwf size %d" % e["rows"])
    for g in w["barRanges"]:
        for e in g["cases"]:
            want(mpwf.bar_range(e["i"], g["m"]) == (e["start"], e["end"]), "mpwf range %r" % e)
    for e in w["bars"]:
        peaks = list(bytes.fromhex(e["peaks"])) if "peaks" in e else [0] * e["m"]
        bars = list(bytes.fromhex(e["bars"])) if "bars" in e else [0] * mpwf.BAR_COUNT
        for i, b in e.get("peaksAt", {}).items():
            peaks[int(i)] = b
        for i, b in e.get("barsAt", {}).items():
            bars[int(i)] = b
        want(len(peaks) == e["m"] and list(mpwf.bars_from_peaks(peaks)) == bars, "mpwf bars " + e["note"])
    for sc in (w["slot"], w["slotCrc"]):
        raw = mpwf.Reader(golden[sc["file"]]).slot_raw(sc["row"])
        want(mpwf.slot_crc(raw, sc["row"]) == hexint(sc["crc"]) and mpwf.crc32(raw[:-4]) == hexint(sc["unseeded"]),
             "mpwf slotCrc " + sc["file"])
    want(mpwf.Reader(golden[w["slot"]["file"]]).slot_raw(0).hex() == w["slot"]["bytes"], "mpwf slot bytes")
    for e in w["sectionCrc"]:
        r = mpwf.Reader(golden[e["file"]])
        want(r.rows == e["rows"] and r.section.crc == hexint(e["crc"]), "mpwf section crc " + e["file"])
    for e in w["pick"]:
        want(mpwf.probe_row(u64(e["h"]), e["lo"], e["hi"], u64(e["loH"]), u64(e["hiH"]), e["probes"]) == e["row"],
             "mpwf pick " + e["note"])
    for name, cases in w["search"]["files"].items():
        r = mpwf.Reader(golden[name])
        for e in cases:
            got = r.find(u64(e["hash"]))
            want(got.read == e["read"] and got.probes == e["probes"] and got.row == e.get("row"),
                 "mpwf search %s %s" % (name, e["hash"]))
    for e in w["deviceTxt"]["cases"]:
        want(mpwf.writes_waves(mpwf.device_text(e)) == e["writes"], "mpwf deviceTxt %r" % e)
    for e in w["commit"]["cases"]:
        got = mpwf.commit_outcome(mpwf.device_text(e), golden.get(e["old"]), mpwf.commit_rows(e, golden))
        want(got == e["outcome"], "mpwf commit " + e["note"])
    # Every slot of the fixture card's index is a track of its tags file, its
    # lengthMs that record's durationMs (0 where the record has none).
    records = {x["pathHash"]: x for x in ledger_rows(golden["tags-0000002a.bin"])}
    r = mpwf.Reader(golden[mpwf.G1_FILE])
    for i in range(r.rows):
        slot = r.slot(i)[2]
        want(slot.path_hash in records and records[slot.path_hash]["durationMs"] == slot.length_ms,
             "mpwf slot %d against the tags file" % i)
    rr = w["rootRow"]
    row = root_waves(golden[rr["golden"]["file"]])
    want(row is not None and row[0] == rr["golden"]["waves"] and waves_matches(row, golden) == rr["golden"]["matches"],
         "mpwf root row")


def check_vectors(v, golden: dict):
    """Recomputes every vector; raises on a mismatch."""
    bad = []

    def want(cond, what):
        if not cond:
            bad.append(what)

    want(crc(b"123456789") == hexint(v["crc32"][0]["crc"]), "crc32")
    for e in v["fnv1a64"]:
        want(fnv(e["utf8"].encode("utf-8")) == hexint(e["hash"]), "fnv " + e["utf8"])
    for e in v["pathHash"]:
        want(path_hash(e["rel"]) == hexint(e["hash"]), "pathHash " + e["rel"])
    for e in v["serverUrlKey"]:
        want(normalise_url(e["url"]) == e["normalised"], "url " + e["url"])
        want(server_url_key(e["url"]) == hexint(e["key"]), "urlkey " + e["url"])
    for e in v["qfp"]:
        want(qfp(bytes(i & 0xFF for i in range(e["size"]))) == hexint(e["qfp"]), "qfp %d" % e["size"])
    for e in v["fatTime"]:
        d, t = e["time"].split()
        y, mo, dd = map(int, d.split("-"))
        h, mi, s = map(int, t.split(":"))
        want(fat_time(y, mo, dd, h, mi, s) == hexint(e["fatTime"]), "fat " + e["time"])
    for e in v["fatTimeInvalid"]:
        want(fat_wall(hexint(e)) is None, "invalid " + e)
    for e in v["wallDelta"]:
        want(fat_wall(hexint(e["b"])) - fat_wall(hexint(e["a"])) == e["delta"], "wall delta")
    for e in v["skew"]:
        pairs = pairs_for(e["groups"])
        d = skew(pairs)
        want(d == e["skew"], "skew %r" % e["groups"])
        want(sum(time_matches(r, o, d) for r, o in pairs) == e["matching"], "skew matching %r" % e["groups"])
    want(fat_wall(0) is None and not time_matches(0, 0, 0), "zero stamps")
    m = v["mpdj"]
    want(bytes.fromhex(m["signature"]["sig"]).hex(" ").upper() == v["bytes"]["selectionSigStored"], "sig bytes")
    want(mpdj.signature(m["signature"]["text"]).hex() == m["signature"]["sig"], "mpdj signature")
    want(mpdj.prepare(next(d for d in mpdj.fixture_libraries() if d["file"] == m["signature"]["file"])).sig_text
         == m["signature"]["text"], "mpdj signature text")
    for e in m["levels"]:
        want(mpdj.level(e["cos"]) == e["level"], "mpdj level %r" % e["cos"])
    for e in m["floors"]["cases"]:
        want(mpdj.floor_level(e["percent"]) == e["level"], "mpdj floor %r" % e["percent"])
    for e in m["slotBytes"]:
        want(mpdj.slot_bytes_for(e["k"]) == e["slotBytes"], "mpdj slotBytes %r" % e["k"])
    for e in m["headerBytes"]:
        want(mpdj.header_bytes_for(e["sectionCount"], e["slotBytes"], e["minimum"]) == e["headerBytes"],
             "mpdj headerBytes %r" % e)
    for e in m["stridedRanks"]:
        ranks = list(range(e["k"])) if e["m"] <= e["k"] else mpdj.strided_ranks(e["k"], e["head"], e["m"])
        want(ranks == e["ranks"] if "ranks" in e else all(ranks[q["j"]] == q["rank"] for q in e["pairs"]),
             "mpdj stridedRanks %r" % ((e["k"], e["head"], e["m"]),))
    sc = m["slotCrc"]
    raw = mpdj.Reader(golden[sc["file"]]).slot_raw(sc["row"])
    want(mpdj.slot_crc(raw, sc["row"]) == hexint(sc["crc"]) and mpdj.crc32(raw[:-4]) == hexint(sc["unseeded"]),
         "mpdj slotCrc")
    check_waves(v["mpwf"], golden, want)
    sib = v["canonicalOrder"]["siblings"]
    want(sorted(sib, key=lambda s: s.encode("utf-8")) == sib, "siblings")
    files = v["canonicalOrder"]["files"]
    want(sorted(files, key=file_key) == files, "files")
    for e in v["stringRuns"]:
        vals = [build_field(e["fields"].get(f, []), f in LIST_FIELDS)[0] for f in FIELDS]
        want(encode_run(vals).hex(" ").upper() == e["run"], "run %r" % e["fields"])
    for e in v["fieldRules"]:
        got, t = build_field(e["values"], e["field"] in LIST_FIELDS)
        want(got == e["stored"].encode("utf-8") and t == e["truncated"], "field %s" % e["field"])
        if "bytes" in e:
            want(len(got) == e["bytes"], "field bytes")
    for e in v["numbers"]:
        k = e["kind"]
        got = (gain(e["text"]) if k == "gain" else peak(e["text"]) if k == "peak" else bpm10(e["text"])
               if k == "bpm" else r128(e["q"]))
        want(got == e["value"], "number %r" % e)
    for e in v["rootElection"]:
        want(elect(e["bin"], e["tmp"]) == (e["pick"], e["sameCommit"]), "election %r" % e)
    for e in v["hashSampled"]:
        want((e["hashV"] >= 2 and e["fileSize"] >= 26214400) == e["sampled"], "sampled")
    for e in v["albumFolder"]:
        a = album_folder(e["rel"], e.get("roots", []))
        want(a == e["album"] and path_hash(a) == hexint(e["hash"]), "album " + e["rel"])
    tb = v["bytes"]["thumbPath"]
    th = mpth({"folder": tb["folder"]})
    want(len(th) == tb["bytes"] and th[:16].hex(" ").upper() == tb["head0to15"] and
         th[20:24].hex(" ").upper() == tb["head20to23"], "thumb bytes")
    want("/.mstream/thumbs/%X/%08X.565" % (path_hash(tb["folder"]) >> 60, path_hash(tb["folder"]) >> 32) ==
         tb["path"], "thumb path")
    want(bytes.fromhex(v["bytes"]["serverInstance"]["uuid"].replace("-", "")).hex(" ").upper() ==
         v["bytes"]["serverInstance"]["stored"], "uuid")
    want(bytes.fromhex(v["bytes"]["audioHash"]["audioHash"]).hex(" ").upper() == v["bytes"]["audioHash"]["stored"],
         "audioHash")
    sv = v["servers"]
    for e in sv["origServer"]:
        want(struct.pack("<H4s", e["server"], bytes(4)).hex(" ").upper() == e["bytes74to79"], "origServer %r" % e)
    want(ledger_servers(golden[sv["rows"]["file"]]) == [(e["path"], e["server"]) for e in sv["rows"]["rows"]],
         "server rows")
    ps = parse_container(golden[sv["srvs"]["file"]])
    srvs = section(ps, "SRVS")
    want(srvs[3] == sv["srvs"]["stride"] and [bytes(srvs[4][srvs[3] * i:srvs[3] * (i + 1)]).hex(" ").upper()
                                              for i in range(srvs[2])] == [e["stored"] for e in sv["srvs"]["entries"]],
         "srvs entries")
    want([e["server"] for e in sv["srvs"]["entries"]] == list(range(1, srvs[2] + 1)), "srvs indexes")
    card = manifest_servers()
    want(sv["sameServer"]["card"] == card["file"].rsplit(".", 1)[0] + ".json", "sameServer card")
    for e in sv["sameServer"]["cases"]:
        want(same_server(card, e["index"], e["talk"]) == e["verdict"], "sameServer %r" % e)
    for e in sv["brokenEntries"]["cases"]:
        want(same_server(e["card"], e["index"], e["talk"]) == e["verdict"], "brokenEntries %r" % e)
    pf = v["privateFiles"]
    for e in pf["names"]:
        want(private_name(e["name"]) == (e["ok"], e["why"]), "private name " + e["name"])
    want(all(private_name(n)[0] for n in RESERVED), "the reserved names are private names")
    for e in pf["settle"]:
        got = settle_private(e["stem"], e["json"], e["new"])
        exp = {k: e[k] for k in ("pick", "seq", "nextSeq", "delete", "rename", "diskCheck", "stops")}
        if exp["pick"] == "none":
            exp["nextSeq"] = 1
        want(got == exp, "settle %s: got %r" % (e["note"], got))
    for e in pf["cleanup"]:
        want(cleanup(e["files"], e["root"], e["resume"]) == e["delete"], "cleanup " + e["note"])
    for e in pf["resumeFetch"]:
        got = resume_fetch(e["entry"], e["partSize"], e["server"], e["status"])
        want(got["from"] == e["from"] and (got["headers"] or None) == e["headers"] and
             got.get("then", "start over") == e["then"], "resumeFetch " + e["note"])
    want(private_parse(json.dumps(pf["resumeExample"]), RESUME_FORMAT)[0] == "valid" and
         len(resume_parts(pf["resumeExample"])) == len(pf["resumeExample"]["parts"]), "resume example")
    for e in v["names"]["marks"]:
        want(e["mark"] == "~%04X" % (fnv(e["component"].encode("utf-8")) & 0xFFFF), "name mark " + e["name"])
    for e in v["names"]["runs"]:
        want(cardnames.run(e) == e["names"], "names " + e["note"])
        want(all(len(("/music/" + n).encode("utf-8")) <= 255 for n in e["names"] if isinstance(n, str)),
             "names within 255 bytes: " + e["note"])
    check_plays(v["plays"], golden, want)
    if bad:
        raise SystemExit("vectors that don't match: " + ", ".join(bad))


# ---------------------------------------------------------------------------
# Mood filters (moods.json: MOOD-FILTERS.md 4.1, 5.3, 5.4; tools/moodfilter.py)
# ---------------------------------------------------------------------------
def spaced(b: bytes) -> str:
    """Bytes as the fixtures write them: space-separated hex."""
    return b.hex(" ").upper()


def broken_tail(i, v):
    """5.7's worked tail with byte 72 + i set to v."""
    t = bytearray.fromhex(WORKED_TAIL)
    t[i] = v
    return spaced(bytes(t))


# Tails a reader must refuse, one kind each: the record then has no scores
# (2.6.4). Each is 5.7's worked tail broken in one way, but the last.
BAD_TAILS = [("bit 0 clear, an order bit set", broken_tail(0, 0x02)),
             ("bit 0 clear, the values set", broken_tail(0, 0x00))]
BAD_TAILS += [("reserved flag bit %d set" % k, broken_tail(0, 0x03 | 1 << k)) for k in range(3, 8)]
BAD_TAILS += [("byte 79 not 0", broken_tail(7, 1))]
BAD_TAILS += [("byte %d at %d: neither 0-100 nor 255" % (72 + i, v), broken_tail(i, v))
              for i, v in zip(range(1, 7), (101, 254) * 3)]
BAD_TAILS += [("mptg-minor-1's 0xAB fill (flag bits 0xA8)", spaced(bytes([0xAB]) * 8))]

# Tails a reader takes: two no writer makes (an order bit beside a 255),
# and three edges a writer can.
ODD_TAILS = [("an order bit beside a 255: ignored, valence NULL", "03 FF 46 1D 31 FF 46 00"),
             ("bit 2 beside a 255: ignored, energy NULL", "05 3A FF 1D 31 FF 46 00"),
             ("an order bit beside a value under 30: bucket 0 stores 0, no side", "03 1D 46 1D 31 FF 46 00"),
             ("every value 100, both order bits", "07 64 64 64 64 64 64 00"),
             ("every value 0, PRESENT alone: analysed, nothing holds but neutral and vocals",
              "01 00 00 00 00 00 00 00")]

# The rows: tags-moods.bin's records first, then the bars' edges. The
# NULL rows each take one head from MOOD_BASE, which passes 5.7's filter.
MOOD_BASE = [0.6, 0.2, 0.55, 0.6, 0.35, 0.65, 0.75, 0.15]


def mood_rows():
    """moods.json's rows, as (note, scores)."""
    rows = [("tags-moods.bin's %s: %s" % (r["path"], r["note"]), r["scores"]) for r in tags_moods()["records"]]
    rows += [("every head at %s%s" % (x, what), [x] * 8) for x, what in [
        (0.3, ", Loose's bar: ties, so happy and relaxed"), (0.2999, ", a step under Loose's bar"),
        (0.5, ", Standard's bar"), (0.4999, ", a step under Standard's bar"), (0.7, ", Strict's bar"),
        (0.6999, ", a step under Strict's bar"), (0, ""), (1, "")]]
    rows += [
        ("happy at Strict's bar, sad a step under", [0.5, 0.7, 0.6999, 0.5, 0.5, 0.5, 0.5, 0.5]),
        ("sad at Strict's bar, happy a step under", [0.5, 0.6999, 0.7, 0.5, 0.5, 0.5, 0.5, 0.5]),
        ("aggressive beats relaxed by a step at Standard's bar", [0.5, 0.2, 0.1, 0.5, 0.5001, 0.5, 0.5, 0.5]),
        ("happy and neutral at once at Loose (on 0.3, mirror 0.7)", [0.4, 0.65, 0.2, 0.25, 0.1, 0.2, 0.2, 0.2]),
        ("sad at 0.7 over happy at 0.6: sad at every level, never neutral", [0.5, 0.6, 0.7, 0.3, 0.6, 0.5, 0.5, 0.5]),
        ("neutral at Strict: every head under 0.3", [0.1, 0.25, 0.2999, 0.0001, 0.29, 0.1, 0.1, 0.1]),
        ("instrumental at 0.3: vocals at Standard, not at Strict", [0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.3]),
        ("instrumental at 0.69: vocals at Loose", [0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.69]),
        ("the tags at the three bars: acoustic 0.3, party 0.5, danceable 0.7",
         [0.7, 0.4, 0.4, 0.4, 0.4, 0.5, 0.3, 0.4]),
        ("the float traps on every head: 0.29, 0.57, 0.58", [0.58, 0.29, 0.57, 0.57, 0.58, 0.29, 0.58, 0.29]),
        ("5-place scores beside the bars", [0.70001, 0.29999, 0.49999, 0.69999, 0.30001, 0.50001, 0.29999, 0.49999]),
        ("sad, relaxed, vocals and danceable at every level", [0.9, 0.1, 0.8, 0.75, 0.05, 0.2, 0.3, 0.1]),
        ("0x127 at its bars: sad 0.5 over 0.4999, relaxed tied at 0.5, instrumental 0.4999, danceable 0.5",
         [0.5, 0.4999, 0.5, 0.5, 0.5, 0.1, 0.1, 0.4999]),
        ("sad and relaxed only at Loose", [0.3, 0.2, 0.3, 0.3, 0.2999, 0, 0, 0.6999]),
    ]
    for i, head in enumerate(moodfilter.MOOD_COLUMNS):
        rows.append(("%s NULL: it fails only its own scale" % head, [None if j == i else x
                                                                    for j, x in enumerate(MOOD_BASE)]))
    return rows


# Filters beside the 33 one-group ones of `holds`: 5.7's, groups ANDed at
# each level, a level alone, and words that read as 0.
MOOD_FILTERS = [
    (WORKED_FILTER, "MOOD-FILTERS.md 5.7: Sad, Relaxed, Vocals, Danceable at Standard"),
    (moodfilter.pack("happy", "relaxed", "instrumental", level="strict"), "Happy, Relaxed, Instrumental at Strict"),
    (moodfilter.pack("neutral", "neutral", level="loose"), "Neutral and Neutral at Loose"),
    (moodfilter.pack(energy="aggressive", tags=["party"], level="loose"), "Aggressive and Party at Loose"),
    (moodfilter.pack(tags=["acoustic", "party", "danceable"]), "every tag at Standard"),
    (moodfilter.pack("sad", voice="vocals", level="strict"), "Sad and Vocals at Strict"),
    (0, "no filter: every track, analysed or not"),
    (moodfilter.pack(level="strict"), "a level alone filters nothing"),
    (moodfilter.pack(level="loose"), "a level alone filters nothing"),
    (0x800, "bit 11: invalid, reads as 0"),
    (0x8001, "bit 15: invalid, reads as 0"),
    (0x30, "voice 3: invalid, reads as 0"),
    (0x600, "level 3: invalid, reads as 0"),
    (0x627, "5.7's groups at level 3: invalid, reads as 0, so no filter"),
]


# The spec's own literals, which nothing here computes: check_moods holds
# the code to them. The 11 predicates' bits and each level's code, need and
# under (MOOD-FILTERS.md 5.4), and its bars (4.1).
SPEC_PREDICATES = ["0x001", "0x002", "0x003", "0x004", "0x008", "0x00C", "0x010", "0x020", "0x040", "0x080",
                   "0x100"]
SPEC_LEVELS = [("strict", 1, 3, 1, 0.7, 0.3), ("standard", 0, 2, 2, 0.5, 0.5), ("loose", 2, 1, 3, 0.3, 0.7)]
# Hand-made tails, one word field each, and the words 5.4's bit table gives
# them: analysed 15, voice NULL 14, valence 13-11, energy 10-8 (side << 2 |
# bucket, 4 NULL), instrumental 7-6, acoustic 5-4, party 3-2, danceable 1-0.
SPEC_WORDS = [
    ("01 00 00 00 00 00 00 00", 0x8000, "analysed alone: bit 15"),
    ("01 00 00 FF 00 00 00 00", 0xC000, "instrumental NULL: bit 14, its bucket 0"),
    ("03 46 00 00 00 00 00 00", 0xB800, "valence 70, sad's side: 7 in bits 13-11"),
    ("01 FF 00 00 00 00 00 00", 0xA000, "valence NULL: 4 in bits 13-11"),
    ("05 00 1E 00 00 00 00 00", 0x8500, "energy 30, aggressive's side: 5 in bits 10-8"),
    ("01 00 32 00 00 00 00 00", 0x8200, "energy 50, relaxed's side: 2 in bits 10-8"),
    ("01 00 FF 00 00 00 00 00", 0x8400, "energy NULL: 4 in bits 10-8"),
    ("01 00 00 46 00 00 00 00", 0x80C0, "instrumental 70: 3 in bits 7-6"),
    ("01 00 00 00 32 00 00 00", 0x8020, "acoustic 50: 2 in bits 5-4"),
    ("01 00 00 00 00 1E 00 00", 0x8004, "party 30: 1 in bits 3-2"),
    ("01 00 00 00 00 00 64 00", 0x8003, "danceable 100: 3 in bits 1-0"),
    ("01 00 00 00 00 00 1D 00", 0x8000, "danceable 29: bucket 0"),
    ("01 00 00 00 00 00 31 00", 0x8001, "danceable 49: bucket 1"),
    ("01 00 00 00 00 00 45 00", 0x8002, "danceable 69: bucket 2"),
    ("01 00 00 00 00 FF 00 00", 0x8000, "party NULL: 0, which never holds"),
]


def mood_albums(rows):
    """Albums made of rows (5.7's majority): (note, q, row indexes)."""
    q = WORKED_FILTER
    words = [moodfilter.word_of(s) for _, s in rows]
    yes = [i for i, w in enumerate(words) if w and moodfilter.matches(w, q)]
    no = [i for i, w in enumerate(words) if w and not moodfilter.matches(w, q)]
    none = [i for i, w in enumerate(words) if not w]
    all_null = [i for i, (_, s) in enumerate(rows) if s is not None and all(x is None for x in s)]
    return [
        ("5 of 9 analysed pass: 10 > 9, it matches", q, yes[:5] + no[:4]),
        ("2 of 4: 4 > 4 fails", q, yes[:2] + no[:2]),
        ("only tracks with no row: 0 of 0, never a match", q, none[:2]),
        ("5 of 9 and three with no row, which don't count", q, yes[:5] + no[:4] + none[:3]),
        ("1 pass and a row of NULLs: 1 of 2, the NULL row in the denominator", q, yes[:1] + all_null[:1]),
        ("1 pass alone: 1 of 1", q, yes[:1]),
        ("1 pass and a track with no row: still 1 of 1", q, yes[:1] + none[:1]),
        ("no track passes: 0 of 3", q, no[:3]),
    ]


def moods(golden: dict):
    m = {}
    m["note"] = ("Mood filters as data (MOOD-FILTERS.md 4.1, 5.3, 5.4; METADATA 2.6.4's mood tail, 3.4.3's mood "
                 "word), made with tools/moodfilter.py. Hex strings are integers; byte strings are space-separated "
                 "hex; rows are counted from 0. A row's 'holds' lists, for each level, the predicates that hold for "
                 "it, in 'predicates' order; one not listed fails. A predicate at a level is the filter of its 'q' "
                 "with the level's 'code' in bits 9-10. A filter that sets no group (a level alone, or an invalid "
                 "word, which reads as 0) matches every row, one with no scores too.")
    m["heads"] = list(moodfilter.MOOD_COLUMNS)
    m["values"] = [{"score": x, "value": moodfilter.value(x), "bucket": moodfilter.bucket(moodfilter.value(x))}
                   for x in [0, 1, 0.29, 0.57, 0.58, 0.2999, 0.3, 0.4999, 0.5, 0.6999, 0.7, 0.12345, 0.99999, 1.25,
                             -0.5, -0.0001]]
    m["values"].append({"score": None, "value": moodfilter.NULL, "note": "NULL: no bucket"})
    m["levels"] = [{"level": lv, "code": moodfilter.LEVELS[lv]["code"], "need": moodfilter.LEVELS[lv]["need"],
                    "under": moodfilter.LEVELS[lv]["under"]} for lv in ("strict", "standard", "loose")]
    m["bars"] = [{"level": lv, "on": moodfilter.LEVELS[lv]["on"], "mirror": moodfilter.LEVELS[lv]["mirror"]}
                 for lv in ("strict", "standard", "loose")]
    m["predicates"] = [{"name": n, "q": "0x%03X" % q} for n, q in moodfilter.PREDICATES]
    rows = mood_rows()
    m["rows"] = []
    for note, scores in rows:
        tail = moodfilter.tail_of(scores)
        word = moodfilter.word_of_tail(tail)
        m["rows"].append({"note": note, "scores": scores, "tail": spaced(tail), "word": "0x%04X" % word,
                          "holds": moodfilter.holds(word)})
    m["tails"] = [{"note": "no row", "tail": spaced(moodfilter.NO_ROW)}]
    m["tails"] += [{"note": n, "tail": spaced(moodfilter.tail_of(s))} for n, s in [
        ("MOOD-FILTERS.md 5.7's worked example", WORKED_SCORES), ("a row of NULLs", [None] * 8)]]
    m["tails"] += [{"note": n, "tail": t} for n, t in ODD_TAILS + BAD_TAILS]
    for e in m["tails"]:
        t = bytes.fromhex(e["tail"])
        e["valid"] = moodfilter.tail_valid(t)
        e["word"] = "0x%04X" % moodfilter.word_of_tail(t)
    words = [moodfilter.word_of(s) for _, s in rows]
    m["filters"] = []
    for q, note in MOOD_FILTERS:
        f = moodfilter.unpack(q)
        m["filters"].append({"note": note, "q": "0x%03X" % q, "valid": moodfilter.q_valid(q),
                             "reads": "0x%03X" % moodfilter.read_q(q), "active": moodfilter.active(q),
                             "level": f["level"], "valence": f["valence"], "energy": f["energy"],
                             "voice": f["voice"], "tags": f["tags"],
                             "matches": [i for i, w in enumerate(words) if moodfilter.matches(w, q)]})
    m["albums"] = []
    for note, q, members in mood_albums(rows):
        passed, analysed = moodfilter.album_counts([words[i] for i in members], q)
        m["albums"].append({"note": note, "q": "0x%03X" % q, "rows": members, "pass": passed, "analysed": analysed,
                            "matches": moodfilter.album_matches([words[i] for i in members], q)})
    return m


def check_moods(m, golden: dict):
    """moods.json against the spec's own numbers, the word against 4.1's
    rules on the raw scores, and tags-moods.bin's tails against its
    description."""
    bad = []

    def want(cond, what):
        if not cond:
            bad.append(what)

    mf = moodfilter
    # 5.7, step by step, and the traps a plain floor falls into (5.3).
    want(spaced(mf.tail_of(WORKED_SCORES)) == WORKED_TAIL, "5.7 tail")
    want(mf.word_of(WORKED_SCORES) == WORKED_WORD, "5.7 word")
    want(mf.pack("sad", "relaxed", "vocals", ["danceable"]) == WORKED_FILTER, "5.7 filter")
    want(mf.matches(WORKED_WORD, WORKED_FILTER) and mf.matches_scores(WORKED_SCORES, WORKED_FILTER), "5.7 holds")
    for x, plain in ((0.29, 28), (0.57, 56), (0.58, 57)):
        want(int(x * 100) == plain and mf.value(x) == plain + 1, "float trap %s" % x)
    want(not mf.tail_valid(bytes([0xAB]) * 8), "mptg-minor-1's tail is no tail")
    # The spec's literals (SPEC_*): the predicates' bits, the levels' codes,
    # readings and bars, two packed words, and each word field's place.
    want([p["q"] for p in m["predicates"]] == SPEC_PREDICATES, "predicate bits")
    want([(lv["level"], lv["code"], lv["need"], lv["under"], b["on"], b["mirror"])
          for lv, b in zip(m["levels"], m["bars"])] == SPEC_LEVELS, "levels")
    want(mf.pack("happy", "relaxed", "instrumental", level="strict") == 0x215, "pack at Strict")
    want(mf.pack("neutral", "neutral", level="loose") == 0x40A, "pack at Loose")
    for tail, word, note in SPEC_WORDS:
        want(mf.word_of_tail(bytes.fromhex(tail)) == word, "word field: " + note)
    # Every row: its word's 33 checks are 4.1's on its raw scores, as are
    # every filter's.
    for r in m["rows"]:
        word = int(r["word"], 16)
        want(spaced(mf.tail_of(r["scores"])) == r["tail"] and mf.word_of_tail(bytes.fromhex(r["tail"])) == word,
             "row word " + r["note"])
        raw = {lv: [n for n, _ in mf.PREDICATES if mf.matches_scores(r["scores"], mf.predicate_q(n, lv))]
               for lv in r["holds"]}
        want(raw == r["holds"] == mf.holds(word), "row holds " + r["note"])
    for f in m["filters"]:
        q = int(f["q"], 16)
        raw = [i for i, r in enumerate(m["rows"]) if mf.matches_scores(r["scores"], q)]
        want(raw == f["matches"], "filter " + f["q"])
        want(mf.pack(f["valence"], f["energy"], f["voice"], f["tags"], f["level"]) == int(f["reads"], 16),
             "filter pack " + f["q"])
    for t in m["tails"]:
        want(t["valid"] == (t["word"] != "0x0000"), "tail " + t["note"])
    want(all(not t["valid"] for t in m["tails"] if (t["note"], t["tail"]) in BAD_TAILS), "bad tails")
    # The albums: the counts their notes give.
    counts = [(5, 9, True), (2, 4, False), (0, 0, False), (5, 9, True), (1, 2, False), (1, 1, True), (1, 1, True),
              (0, 3, False)]
    want([(a["pass"], a["analysed"], a["matches"]) for a in m["albums"]] == counts, "albums")
    # tags-moods.bin: each record's bytes 72-79 its description's tail.
    p = parse_container(golden["tags-moods.bin"])
    s = section(p, "RECS")
    by_path = {r["path"]: r for r in tags_moods()["records"]}
    paths = [e["path"] for e in ledger_rows(golden["tags-moods.bin"])]
    want(p["minor"] == 1 and s[3] == 80 and len(paths) == s[2] == len(by_path), "tags-moods.bin shape")
    for k, path in enumerate(paths):
        want(mf.record_tail(s[4][s[3] * k:s[3] * (k + 1)], s[3]) == mf.tail_of(by_path[path]["scores"]),
             "tags-moods.bin " + path)
    if bad:
        raise SystemExit("moods.json doesn't match: " + ", ".join(bad))


# ---------------------------------------------------------------------------
# Hardening: each file breaks one check under valid CRCs
# ---------------------------------------------------------------------------
def hardening(golden: dict):
    out = []  # (name, format, uses, expect, why, check, bytes)

    def bad(name, fmt, why, check, data, uses="all"):
        out.append((name, fmt, uses, "absent", why, check, data))

    def good(name, fmt, check, data, uses="all", same=None):
        out.append((name, fmt, uses, "present", "ok", check, data, same))

    tags = golden["tags-0000002a.bin"]
    desc = tags_transfer()

    def edit(fn, src=tags):
        p = parse_container(src)
        fn(p)
        return rebuild(p)

    def folds(p):
        s = section(p, "FOLD")
        return s, [list(struct.unpack_from("<IIII", s[4], 16 * i)) for i in range(s[2])]

    def put_folds(s, rows):
        s[4] = bytearray(b"".join(struct.pack("<IIII", *r) for r in rows))

    def recs(p):
        s = section(p, "RECS")
        return s

    def set_fold(i, field, value):
        def fn(p):
            s, rows = folds(p)
            rows[i][field] = value
            put_folds(s, rows)
        return fn

    bad("mptg-fold-root-parent", "MPTG", "foldRoot", "FOLD: folder 0's parent isn't 0xFFFFFFFF",
        edit(set_fold(0, 0, 0)))
    bad("mptg-fold-root-name", "MPTG", "foldRoot", "FOLD: folder 0's name isn't offset 0", edit(set_fold(0, 1, 1)))
    bad("mptg-fold-self-parent", "MPTG", "foldParent", "FOLD: a folder that is its own parent",
        edit(set_fold(3, 0, 3)))

    def not_preorder(p):
        s, rows = folds(p)
        # A folder whose parent is lower but not the folder before it nor one
        # of that folder's ancestors.
        for i in range(2, len(rows)):
            anc, j = set(), i - 1
            while j != 0xFFFFFFFF:
                anc.add(j)
                j = rows[j][0]
            for cand in range(i):
                if cand not in anc:
                    rows[i][0] = cand
                    put_folds(s, rows)
                    return
        raise AssertionError("no folder to break")

    bad("mptg-fold-not-preorder", "MPTG", "foldParent", "FOLD: a parent that isn't on the pre-order stack",
        edit(not_preorder))

    def sibling_names(p):
        # Two siblings of one length swap their names' bytes in STRS.
        s, rows = folds(p)
        strs = section(p, "STRS")[4]

        def name(off):
            return bytes(strs[off:strs.index(0, off)])

        for i in range(1, len(rows)):
            for j in range(i + 1, len(rows)):
                if rows[i][0] == rows[j][0] and len(name(rows[i][1])) == len(name(rows[j][1])):
                    a, b = name(rows[i][1]), name(rows[j][1])
                    strs[rows[i][1]:rows[i][1] + len(a)] = b
                    strs[rows[j][1]:rows[j][1] + len(b)] = a
                    return
        raise AssertionError("no siblings")

    bad("mptg-fold-sibling-order", "MPTG", "foldOrder", "FOLD: siblings' names not strictly increasing",
        edit(sibling_names))

    def first_record(p):
        s, rows = folds(p)
        for r in rows:
            if r[3]:
                r[3] += 1
                break
        put_folds(s, rows)

    bad("mptg-first-record", "MPTG", "firstRecord", "FOLD: a wrong firstRecord", edit(first_record))

    def recs_folder(p):
        s = recs(p)
        n = s[2]
        struct.pack_into("<I", s[4], 72 * (n - 1), section(p, "FOLD")[2])

    bad("mptg-recs-folder-range", "MPTG", "recsFolder", "RECS: a folder index not below folderCount",
        edit(recs_folder))
    bad("mptg-recs-path-string-order", "MPTG", "firstRecord",
        "RECS: records in path-string order (A B/y.mp3 before A/x.mp3; the first check it breaks is firstRecord)",
        mptg(desc, order="path-string"))

    def dup_name(p):
        s = recs(p)
        strs = section(p, "STRS")[4]
        rows = [struct.unpack_from("<II", s[4], 72 * i) for i in range(s[2])]
        for i in range(len(rows) - 1):
            (fa, na), (fb, nb) = rows[i], rows[i + 1]
            a, b = bytes(strs[na:strs.index(0, na)]), bytes(strs[nb:strs.index(0, nb)])
            if fa == fb and len(a) == len(b):
                strs[nb:nb + len(b)] = a
                return
        raise AssertionError("no pair")

    bad("mptg-recs-duplicate-name", "MPTG", "recsOrder", "RECS: two records of one folder with one name",
        edit(dup_name))

    def rec_name(i, value):
        def fn(p):
            s = recs(p)
            if value is None:
                struct.pack_into("<I", s[4], 72 * i + 4, 0)
                return
            strs = section(p, "STRS")[4]
            off = struct.unpack_from("<I", s[4], 72 * i + 4)[0]
            strs[off] = ord(value)
        return fn

    bad("mptg-name-empty", "MPTG", "name", "RECS: a record named by offset 0 (the empty string)",
        edit(rec_name(2, None)))
    bad("mptg-name-slash", "MPTG", "name", "RECS: a name with a '/'", edit(rec_name(2, "/")))

    def folder_named(new):
        def fn(p):
            s, rows = folds(p)
            strs = section(p, "STRS")[4]
            for r in rows[1:]:
                off = r[1]
                if strs.index(0, off) - off == len(new):
                    strs[off:off + len(new)] = new.encode("ascii")
                    return
            raise AssertionError("no folder of that length")
        return fn

    bad("mptg-name-dot", "MPTG", "name", "FOLD: a folder named '.'", edit(folder_named(".")))
    bad("mptg-name-dotdot", "MPTG", "name", "FOLD: a folder named '..'", edit(folder_named("..")))

    def strings_at(value):
        def fn(p):
            s = recs(p)
            strs = section(p, "STRS")[4]
            for i in range(s[2]):
                if struct.unpack_from("<I", s[4], 72 * i + 8)[0]:
                    struct.pack_into("<I", s[4], 72 * i + 8, len(strs) if value == "end" else value)
                    return
        return fn

    bad("mptg-string-out-of-range", "MPTG", "string", "RECS: a run's offset at STRS's end", edit(strings_at("end")))

    def no_nul(p):
        strs = section(p, "STRS")[4]
        strs[-1] = ord("x")

    bad("mptg-string-no-nul", "MPTG", "string", "STRS: the last string has no NUL", edit(no_nul))

    def swap_runs(p):
        s = recs(p)
        withrun = [i for i in range(s[2]) if struct.unpack_from("<I", s[4], 72 * i + 8)[0]]
        a, b = withrun[0], withrun[1]
        ra = struct.unpack_from("<I", s[4], 72 * a + 8)[0]
        rb = struct.unpack_from("<I", s[4], 72 * b + 8)[0]
        struct.pack_into("<I", s[4], 72 * a + 8, rb)
        struct.pack_into("<I", s[4], 72 * b + 8, ra)

    bad("mptg-string-order", "MPTG", "stringOrder", "STRS: two records' runs swapped (each string still valid)",
        edit(swap_runs))

    def header_field(off, value, fmt="<I"):
        def fn(p):
            struct.pack_into(fmt, p["th"], off - 40, value)
        return fn

    bad("mptg-counts", "MPTG", "counts", "header: recordCount isn't RECS's count",
        edit(header_field(48, len(desc["records"]) + 1)))
    bad("mptg-source-0", "MPTG", "enum", "header: source 0", edit(header_field(40, 0, "<B")))
    bad("mptg-source-4", "MPTG", "enum", "header: source 4", edit(header_field(40, 4, "<B")))

    def hidx_edit(kind):
        def fn(p):
            s = section(p, "HIDX")
            e = [list(struct.unpack_from("<QI", s[4], 12 * i)) for i in range(s[2])]
            if kind == "swap":
                e[0], e[1] = e[1], e[0]
            elif kind == "hash":
                assert e[0][0] + 1 < e[1][0]
                e[0][0] += 1
            elif kind == "twice":
                e[1][1] = e[0][1]
            elif kind == "count":
                e = e[:-1]
                s[2] -= 1
            s[4] = bytearray(b"".join(struct.pack("<QI", *x) for x in e))
        return fn

    bad("mptg-hidx-unsorted", "MPTG", "hidx", "HIDX: two entries swapped", edit(hidx_edit("swap")))
    bad("mptg-hidx-wrong-hash", "MPTG", "hidx", "HIDX: a hash that isn't its record's", edit(hidx_edit("hash")))
    bad("mptg-hidx-record-twice", "MPTG", "hidx", "HIDX: a record twice (and one missing)", edit(hidx_edit("twice")))
    bad("mptg-hidx-count", "MPTG", "hidx", "HIDX: one entry fewer than recordCount", edit(hidx_edit("count")))
    good("mptg-hidx-ignored", "MPTG", "HIDX broken, read by a reader that doesn't use it (the device's builder)",
         edit(hidx_edit("swap")), uses="device")

    def orig_short(p):
        s = section(p, "ORIG")
        s[2] -= 1
        s[4] = s[4][:-80]

    bad("mptg-orig-count", "MPTG", "orig", "ORIG: its count isn't recordCount", edit(orig_short))
    good("mptg-orig-ignored", "MPTG", "ORIG broken, read by the device (which ignores the ledger)",
         edit(orig_short), uses="device")

    def unknown_required(p):
        p["sections"].append(["ZZZZ", REQUIRED, 0, 0, bytearray(b"future")])

    bad("mptg-unknown-required", "MPTG", "unknownRequired", "a REQUIRED section this reader doesn't know",
        edit(unknown_required))

    def unknown_optional(p):
        p["sections"].append(["XTRA", 0, 0, 0, bytearray(b"a newer writer's")])

    good("mptg-unknown-optional", "MPTG", "an unknown section without REQUIRED: skipped",
         edit(unknown_optional), same="tags-0000002a.bin")

    def major(p):
        p["major"] = 2

    bad("mptg-major-2", "MPTG", "major", "a newer major", edit(major))

    gap = bytearray(tags)
    pc = parse_container(tags)
    n = len(pc["sections"])
    hb = 40 + len(pc["th"])
    # Move the last section 8 bytes on (a gap of 8 or more): the directory's
    # offset and fileBytes change, the header's CRC is made again.
    last = hb + 32 * (n - 1)
    off, ln = struct.unpack_from("<II", gap, last + 8)
    gap = gap[:off] + bytes(8) + gap[off:]
    struct.pack_into("<I", gap, last + 8, off + 8)
    struct.pack_into("<I", gap, 16, len(gap))
    struct.pack_into("<I", gap, 32, 0)
    struct.pack_into("<I", gap, 32, crc(bytes(gap[:hb + 32 * n])))
    bad("mptg-layout-gap", "MPTG", "layout", "the directory: a gap of 8 bytes before a section", bytes(gap))

    def shape(p):
        s = recs(p)
        s[3] = 64
        s[4] = bytearray(b"".join(s[4][72 * i:72 * i + 64] for i in range(s[2])))

    bad("mptg-shape", "MPTG", "shape", "RECS: a stride below v1's 72", edit(shape))

    def no_folders(p):
        s = section(p, "FOLD")
        s[2] = 0
        s[4] = bytearray()
        struct.pack_into("<I", p["th"], 52 - 40, 0)

    bad("mptg-no-folders", "MPTG", "foldRoot", "FOLD empty: folder 0 is always there", edit(no_folders))
    def one_path(rel):
        return mptg({"format": "MPTG", "generation": 1, "cardId": CARD_ID, "source": 2, "producer": "x",
                     "records": [{"path": rel, "known": 0}]})

    bad("mptg-path-too-long", "MPTG", "pathLength", "a record whose path makes '/music/' + path 256 bytes",
        one_path("L" * 120 + "/" + "M" * 124 + ".mp3"))
    good("mptg-path-longest", "MPTG", "a record whose path makes '/music/' + path exactly 255 bytes",
         one_path("L" * 120 + "/" + "M" * 123 + ".mp3"))
    good("mptg-minor-1", "MPTG",
         "a newer minor: 8 more header bytes, RECS rows of 80 bytes whose bytes 72-79 (0xAB, flag bits 3-7 set) "
         "are no valid mood tail, so no record has scores (2.6.4), a 13th run field, an unknown section",
         mptg(dict(desc, minor=1), rec_stride=80, extra_header=b"newminor", extra_run_field="a newer field",
              extra_sections=[("XTRA", 0, 0, 0, b"new")]), same="tags-0000002a.bin")

    # The mood tail (2.6.4): a reader that doesn't know it reads each record
    # by its 72-byte prefix, as for any newer minor; one that does takes a
    # tail only from a stride of 80 or more, and only a valid one, and
    # never counts a bad tail against the record or the file.
    good("mptg-moods-longer", "MPTG", "a newer minor: RECS rows of 88 bytes, the mood tail at 72-79 and 0xAB from "
         "80, read by their 80-byte prefix: tags-moods.bin's records, their scores too",
         mptg(dict(tags_moods(), minor=2), rec_stride=88), same="tags-moods.bin")

    def tails_on(stride, tails):
        def fn(p):
            s = recs(p)
            s[4] = bytearray(b"".join(bytes(s[4][72 * i:72 * i + 72]) + tails[i] for i in range(s[2])))
            s[3] = stride
            p["minor"] = 1
        return fn

    bad_tails = [bytes.fromhex(t) for _, t in BAD_TAILS]
    assert len(bad_tails) == len(desc["records"])
    good("mptg-tail-invalid", "MPTG", "RECS rows of 80 bytes, each record's tail broken in its own way (bit 0 clear, "
         "a flag bit of 3-7 set, byte 79 not 0, a value of 101 or 254): no record has scores, and every record "
         "reads (2.6.4)", edit(tails_on(80, bad_tails)), same="tags-0000002a.bin")
    head = bytes.fromhex(WORKED_TAIL)[:4]
    good("mptg-stride-76", "MPTG", "RECS rows of 76 bytes whose bytes 72-75 begin a valid tail (%s, valid if read "
         "with zeros after it): a stride under 80 has no tail, so no record has scores (2.6.4)" % spaced(head),
         edit(tails_on(76, [head] * len(desc["records"]))), same="tags-0000002a.bin")

    # The rest of 2.4.3's frame and directory, one file each (the tags file
    # otherwise whole, its CRCs right).
    def with_header(th_bytes):
        p = parse_container(tags)
        return build_container("MPTG", p["generation"], p["card"], th_bytes, [tuple(s) for s in p["sections"]],
                               hardening=True)

    pt = parse_container(tags)
    bad("mptg-header-bytes-odd", "MPTG", "headerBytes", "header: headerBytes 76, not a multiple of 8",
        with_header(bytes(pt["th"]) + bytes(4)))
    bad("mptg-header-bytes-short", "MPTG", "headerBytes", "header: headerBytes 64, below MPTG v1's 72",
        with_header(bytes(pt["th"][:24])))

    def many_sections(p):
        # 61 empty optional sections more: 65 in all (each type once, the
        # layout right).
        for i in range(65 - len(p["sections"])):
            p["sections"].append(["X%03d" % i, 0, 0, 0, bytearray()])

    bad("mptg-section-count", "MPTG", "sectionCount", "header: 65 sections (at most 64)", edit(many_sections))
    bad("mptg-file-bytes", "MPTG", "fileBytes", "header: fileBytes 8 bytes short of the file's length",
        tags + bytes(8))

    def raw_last(fn):
        # An edit of the bytes and of the last directory entry, resealed.
        b = bytearray(tags)
        hb, n = struct.unpack_from("<II", b, 8)
        fn(b, hb + 32 * (n - 1), hb + 32 * (n - 2))
        struct.pack_into("<I", b, 16, len(b))
        return reseal(b)

    def misaligned(b, last, _prev):
        off = struct.unpack_from("<I", b, last + 8)[0]
        b[off:off] = bytes(4)
        struct.pack_into("<I", b, last + 8, off + 4)

    def overlap(b, last, prev):
        # The last section starts 8 bytes into the one before it (and runs
        # to the file's end).
        poff = struct.unpack_from("<I", b, prev + 8)[0]
        struct.pack_into("<II", b, last + 8, poff + 8, len(b) - poff - 8)

    def short_end(b, _last, _prev):
        b += bytes(8)

    bad("mptg-layout-misaligned", "MPTG", "layout", "the directory: a section at an offset not a multiple of 8",
        raw_last(misaligned))
    bad("mptg-layout-overlap", "MPTG", "layout", "the directory: a section that starts inside the one before it",
        raw_last(overlap))
    bad("mptg-layout-end", "MPTG", "layout", "the directory: the last section ends 8 bytes before fileBytes",
        raw_last(short_end))

    def type_twice(p):
        p["sections"].append(list(section(p, "OSTR")))

    bad("mptg-layout-type-twice", "MPTG", "layout", "the directory: OSTR twice", edit(type_twice))

    def drop_section(t):
        def fn(p):
            p["sections"] = [s for s in p["sections"] if s[0] != t]
        return fn

    bad("mptg-missing-strs", "MPTG", "missing", "the directory: no STRS (a REQUIRED section)",
        edit(drop_section("STRS")))
    bad("mptg-hidx-absent", "MPTG", "missing", "HIDX absent while recordCount > 0 (2.6.2)",
        edit(drop_section("HIDX")))
    good("mptg-hidx-absent-device", "MPTG", "HIDX absent, read by a reader that doesn't use it (the device's builder)",
         edit(drop_section("HIDX")), uses="device")

    def blob_count(p):
        section(p, "STRS")[2] = 1

    bad("mptg-blob-count", "MPTG", "shape", "STRS (a blob) with a count of 1", edit(blob_count))

    def array_bytes(p):
        section(p, "HIDX")[4] += bytes(4)

    bad("mptg-array-bytes", "MPTG", "shape", "HIDX: bytes 4 more than count x stride", edit(array_bytes))
    bad("mptg-folder-count", "MPTG", "counts", "header: folderCount isn't FOLD's count",
        edit(header_field(52, section(pt, "FOLD")[2] + 1)))

    def strs_first(p):
        section(p, "STRS")[4][0] = ord("x")

    bad("mptg-strs-first-byte", "MPTG", "string", "STRS: byte 0 isn't NUL (offset 0 must be the empty string)",
        edit(strs_first))

    def descending(p):
        # Two records of one folder with names of one length swap their
        # names' bytes: the names now decrease. HIDX follows (each entry's
        # record is the one whose path now has its hash), so only the order
        # is wrong.
        s = recs(p)
        strs = section(p, "STRS")[4]
        rows = [struct.unpack_from("<II", s[4], 72 * i) for i in range(s[2])]
        for i in range(len(rows) - 1):
            (fa, na), (fb, nb) = rows[i], rows[i + 1]
            a, b = bytes(strs[na:strs.index(0, na)]), bytes(strs[nb:strs.index(0, nb)])
            if fa == fb and len(a) == len(b):
                strs[na:na + len(a)] = b
                strs[nb:nb + len(b)] = a
                h = section(p, "HIDX")
                e = [list(struct.unpack_from("<QI", h[4], 12 * k)) for k in range(h[2])]
                for x in e:
                    x[1] = i + 1 if x[1] == i else i if x[1] == i + 1 else x[1]
                h[4] = bytearray(b"".join(struct.pack("<QI", *x) for x in e))
                return
        raise AssertionError("no pair")

    bad("mptg-recs-descending", "MPTG", "recsOrder", "RECS: two records of one folder with their names decreasing",
        edit(descending))

    # MSMF.
    man = golden["manifest.bin"]
    md = manifest_main()
    t_row = comp_row("tags-0000002a.bin", golden["tags-0000002a.bin"])
    d_row = comp_row("autodj-00000029.bin", golden["autodj-00000029.bin"])
    roots = ["Lib A", "Lib B"]
    bad("msmf-comp-order", "MSMF", "comp", "COMP: the MPDJ before the MPTG", msmf_raw(md, [d_row, t_row], roots))
    bad("msmf-comp-two-mptg", "MSMF", "comp", "COMP: two MPTG", msmf_raw(md, [t_row, t_row], roots))
    bad("msmf-comp-no-mptg", "MSMF", "comp", "COMP: no MPTG", msmf_raw(md, [d_row], roots))
    bad("msmf-comp-name-upper", "MSMF", "comp", "COMP: tags-0000002A.bin (uppercase hex)",
        msmf_raw(md, [t_row[:4] + ("tags-0000002A.bin", t_row[5]), d_row], roots))
    bad("msmf-comp-name-generation", "MSMF", "comp", "COMP: a name whose digits aren't the entry's generation",
        msmf_raw(md, [t_row[:4] + ("tags-0000002b.bin", t_row[5]), d_row], roots))
    good("msmf-comp-unknown-kind", "MSMF", "COMP: an unknown kind between them, ignored",
         msmf_raw(md, [t_row, ("ZZZZ", 1, 2, 3, "zzzz.bin", bytes(16)), d_row], roots))
    bad("msmf-libr-unsorted", "MSMF", "libr", "LIBR: roots not in byte order", msmf_raw(md, [t_row, d_row],
                                                                                      ["Lib B", "Lib A"]))
    bad("msmf-libr-absolute", "MSMF", "libr", "LIBR: an absolute root", msmf_raw(md, [t_row, d_row], ["/Lib A"]))
    bad("msmf-libr-dot", "MSMF", "libr", "LIBR: a root through '.'", msmf_raw(md, [t_row, d_row], ["Lib A/."]))
    bad("msmf-libr-dotdot", "MSMF", "libr", "LIBR: a root through '..'", msmf_raw(md, [t_row, d_row], ["Lib A/.."]))
    bad("msmf-libr-empty-name", "MSMF", "libr", "LIBR: a root with an empty name ('Lib A//Inner')",
        msmf_raw(md, [t_row, d_row], ["Lib A//Inner"]))
    bad("msmf-libr-too-long", "MSMF", "libr", "LIBR: a root that makes '/music/' + root 256 bytes",
        msmf_raw(md, [t_row, d_row], ["L" * 120 + "/" + "M" * 128]))
    good("msmf-libr-longest", "MSMF", "LIBR: a root that makes '/music/' + root exactly 255 bytes",
         msmf_raw(md, [t_row, d_row], ["L" * 120 + "/" + "M" * 127]))
    bad("msmf-major-2", "MSMF", "major", "a newer major", edit(major, man))

    # A server per ledger row (2.5.5, 2.6.6): the device reads neither ORIG
    # nor SRVS, so neither can make its tags file or its root absent.
    def orig_servers(p):
        s = section(p, "ORIG")
        for i, server in ((1, 1), (3, 2), (s[2] - 1, 0xFFFF)):
            struct.pack_into("<H", s[4], s[3] * i + 74, server)

    good("mptg-orig-server", "MPTG", "ORIG: rows naming servers 1, 2 and 65,535 (2.6.6): the same records; a server "
         "no SRVS entry names is the guard's unknown (2.11), not damage", edit(orig_servers), same="tags-0000002a.bin")

    def srvs_short(p):
        s = section(p, "SRVS")
        s[4] = bytearray(b"".join(s[4][s[3] * i:s[3] * i + 32] for i in range(s[2])))
        s[3] = 32

    good("msmf-srvs-ignored", "MSMF", "SRVS broken (rows of 32 bytes, below v1's 40), read by the device, which never "
         "reads SRVS (to a reader that uses it, the root is absent: 2.4.3)",
         edit(srvs_short, golden["manifest-servers.bin"]), uses="device")
    sd = manifest_servers()
    good("msmf-minor-1", "MSMF", "a newer minor: SRVS rows of 48 bytes, read by their 40-byte prefix (the servers of "
         "manifest-servers.bin), and an unknown section",
         msmf_raw(sd, [comp_row(c["file"], golden[c["file"]]) for c in sd["companions"]], [], 1,
                  [server_entry(s) for s in sd["servers"]], srvs_stride=48, extra_sections=[("XTRA", 0, 0, 0, b"new")]))
    rule_breaks = [{"serverUrl": HOME_URL}, {"serverInstance": SECOND_INSTANCE, "serverUrl": SECOND_URL},
                   {"serverInstance": SECOND_INSTANCE, "parentUrl": SECOND_URL, "flags": PEER}]
    assert srvs_breaks(root_server(sd), [server_entry(s) for s in rule_breaks]) == {1, 2, 3}
    good("msmf-srvs-rules", "MSMF", "SRVS: entries that break 2.5.5's writer rules (the root's own URL key, one "
         "instance twice, a peer with no peerId): not damage, the root reads; their rows' servers are unknown (2.11)",
         msmf_raw(sd, [comp_row(c["file"], golden[c["file"]]) for c in sd["companions"]], [], 0,
                  [server_entry(s) for s in rule_breaks]))

    # MSMF's ACKS (2.5.6): whatever it holds, the root reads; a reader that
    # uses ACKS ignores one that breaks its rules.
    ad = manifest_acks()
    acks_comps = [comp_row(c["file"], golden[c["file"]]) for c in ad["companions"]]
    roots = sorted(ad["roots"], key=lambda r: r.encode("utf-8"))
    entries = [(x["deviceId"], x["epoch"], x["seq"]) for x in ad["acks"]]
    rows = playlog.acks_bytes(entries)

    damaged = bytearray(golden["manifest-acks.bin"])
    hb, n = struct.unpack_from("<II", damaged, 8)
    for i in range(n):
        t, _, off, ln = struct.unpack_from("<4sIII", damaged, hb + 32 * i)
        if t == b"ACKS":
            damaged[off + 16] ^= 1  # a seq's byte: the section's CRC now fails
    good("msmf-acks-damaged", "MSMF", "ACKS: its CRC fails: the section is ignored, never the root (2.5.6)",
         bytes(damaged))
    swapped = rows[24:48] + rows[:24] + rows[48:]
    order = msmf_raw(ad, acks_comps, roots, acks=swapped)
    good("msmf-acks-order", "MSMF", "ACKS: two entries out of order, its CRC good: ignored, the root reads", order)
    short = msmf_raw(ad, acks_comps, roots, acks=(b"".join(rows[24 * i:24 * i + 16] for i in range(3)), 16))
    good("msmf-acks-short", "MSMF", "ACKS: rows of 16 bytes, below v1's 24: ignored, the root reads", short)
    longer = msmf_raw(ad, acks_comps, roots, minor=1,
                      acks=(b"".join(rows[24 * i:24 * (i + 1)] + bytes([0xAB]) * 8 for i in range(3)), 32))
    good("msmf-acks-minor-1", "MSMF", "ACKS: rows of 32 bytes, a newer minor's, read by their 24-byte prefix", longer)
    full = root_acks(golden["manifest-acks.bin"])
    assert full and [root_acks(b) for b in (bytes(damaged), order, short, longer)] == [None, None, None, full]

    # MSMF's MPWF row (WAVEFORM-INDEX.md 7.2): a bad one costs only itself. The
    # root reads, with or without a waves entry; its name is read in its place
    # (2.4.4), as for any kind, so only a string rule can cost the root.
    wd = manifest_waves()
    wave_comps = [comp_row(c["file"], golden[c["file"]]) for c in wd["companions"]]
    w_row = wave_comps[2]
    roots = sorted(wd["roots"], key=lambda r: r.encode("utf-8"))

    def named(row, name, **fields):
        kind, gen, fb, hc, _, key = row
        return (kind, fields.get("gen", gen), fields.get("fb", fb), fields.get("hc", hc), name,
                fields.get("key", key))

    waves_cases = {
        "msmf-waves-no-mpdj": [t_row, w_row],
        "msmf-waves-between": [t_row, w_row, d_row],
        "msmf-waves-first": [w_row, t_row, d_row],
        "msmf-waves-name-upper": [t_row, d_row, named(w_row, "waves-0000002A.bin")],
        "msmf-waves-name-generation": [t_row, d_row, named(w_row, "waves-0000002b.bin")],
        "msmf-waves-name-stem": [t_row, d_row, named(w_row, "wave-0000002a.bin")],
        "msmf-waves-name-empty": [t_row, d_row, named(w_row, "")],
        "msmf-waves-two": [t_row, d_row, w_row, named(w_row, "waves-00000029.bin", gen=41)],
        "msmf-waves-key": [t_row, d_row, named(w_row, w_row[4], key=bytes([0xA5]) * 16)],
        "msmf-waves-file-bytes": [t_row, d_row, named(w_row, w_row[4], fb=w_row[2] + 1)],
        "msmf-waves-header-crc": [t_row, d_row, named(w_row, w_row[4], hc=w_row[3] ^ 1)],
    }
    assert list(waves_cases) == list(WAVES_ROOTS)
    for name, rows_ in waves_cases.items():
        data = msmf_raw(wd, rows_, roots)
        waves, matches, note = WAVES_ROOTS[name]
        row = root_waves(data)
        assert (row[0] if row else None) == waves and (waves_matches(row, golden) if row else None) == matches, name
        good(name, "MSMF", "COMP's MPWF row: " + note, data)

    def waves_name_offset(fn):
        p = parse_container(msmf_raw(wd, wave_comps, roots))
        comp = section(p, "COMP")
        struct.pack_into("<I", comp[4], 2 * comp[3] + 16, fn(p, comp))
        return rebuild(p)

    bad("msmf-waves-name-range", "MSMF", "string", "COMP: the MPWF row's name offset at STRS's end: a string rule, "
        "which costs the root for any kind", waves_name_offset(lambda p, c: len(section(p, "STRS")[4])))
    bad("msmf-waves-name-order", "MSMF", "stringOrder", "COMP: the MPWF row's name offset back at the MPTG row's: a "
        "string rule, which costs the root for any kind",
        waves_name_offset(lambda p, c: struct.unpack_from("<I", c[4], 16)[0]))
    # MSPD.
    pd = pending_main()
    bad("mspd-order", "MSPD", "pendOrder", "PEND: a write before a delete",
        mspd_raw(pd, [(1, "A B/y.mp3", 77), (2, "A/x.mp3", 0)]))
    bad("mspd-group-order", "MSPD", "pendOrder", "PEND: two deletes out of canonical order",
        mspd_raw(pd, [(2, "A B/y.mp3", 0), (2, "A/x.mp3", 0)]))
    bad("mspd-absolute", "MSPD", "pendPath", "PEND: an absolute path", mspd_raw(pd, [(2, "/A/x.mp3", 0)]))
    bad("mspd-dotdot", "MSPD", "pendPath", "PEND: a path through '..'", mspd_raw(pd, [(2, "../x.mp3", 0)]))
    bad("mspd-op-0", "MSPD", "pendOp", "PEND: an op of 0", mspd_raw(pd, [(0, "A/x.mp3", 0)]))
    bad("mspd-path-empty", "MSPD", "pendPath", "PEND: a path at offset 0 (the empty string)",
        mspd_raw(pd, [(2, "", 0)]))
    bad("mspd-path-too-long", "MSPD", "pathLength", "PEND: a path that makes '/music/' + path 256 bytes",
        mspd_raw(pd, [(2, "L" * 120 + "/" + "M" * 124 + ".mp3", 0)]))
    good("mspd-path-longest", "MSPD", "PEND: a path that makes '/music/' + path exactly 255 bytes",
         mspd_raw(pd, [(2, "L" * 120 + "/" + "M" * 123 + ".mp3", 0)]))
    good("mspd-op-9", "MSPD", "PEND: an op above 3: the plan reads, and stops the software",
         mspd_raw(pd, [(2, "A/x.mp3", 0), (9, "Z/z.mp3", 0), (1, "A B/y.mp3", 77)]))
    # MPDJ (docs/AUTODJ-INDEX.md 13.6): tools/mpdj.py's, each already read back.
    out += mpdj.fixture_hardening(golden)
    # MPWF (docs/WAVEFORM-INDEX.md 13.5): tools/mpwf.py's, each already read back.
    out += mpwf.fixture_hardening(golden)
    return out


# ---------------------------------------------------------------------------
# Writing it all
# ---------------------------------------------------------------------------
README = """\
# Card contract fixtures

SPDX-License-Identifier: CC0-1.0. To the extent possible under law, the
authors have waived all copyright and related or neighboring rights to the
files in this folder (the CC0 1.0 Universal dedication,
https://creativecommons.org/publicdomain/zero/1.0/), so the player,
mstream-terminal and mStream can all copy them (docs/METADATA.md part 7,
U18, proposed). The player's code stays GPL-3.0-or-later.

The shared fixtures of docs/METADATA.md 2.17, made by
`tools/card_fixtures.py` (a second implementation of the contract, apart
from `lib/core/CardContract*`) and frozen: `python tools/card_fixtures.py
--check` fails if a file here differs from what it makes, or if a file
here isn't one it makes. Writing them (`python tools/card_fixtures.py`)
deletes any such file, `.gitattributes` excepted, so keep work in progress
elsewhere. Made-up names only, synthetic embeddings.

- `vectors.json`: the vectors of 2.18 as data. `who` says which side a
  group binds when it isn't both.
- `libraries/*.json`: library descriptions, the input of a writer: every
  value a writer would otherwise choose is given (ids, times,
  generations). The tags descriptions' tag values are raw lists: each
  writer applies 2.3.6 and sets TRUNCATED; flags, `known` and the enums are
  as given. The AutoDJ descriptions' titles and artists are instead an MPTG
  record's values as stored, 2.3.6 already applied: an MPDJ writer cuts,
  replaces and drops nothing in them (AUTODJ-INDEX.md 13.2). Integers above
  2^53 are hex strings. A manifest names its companions by file; their
  generation, length, header CRC and (MPDJ) selection signature come from
  the golden companion. A manifest's `servers` are its SRVS entries in
  order (server i is entry i - 1), each key computed from the URL given
  (`serverUrl`, `parentUrl`: 2.5.1), 0 where none is; a ledger's `server`
  is ORIG's server index, 0 (the root's own server) when absent (2.5.5,
  2.6.6).
- `golden/`: what a writer MUST produce from each description, byte for
  byte (2.17, item 2). The MPTH pixels are the synthetic picture the
  description names: the scaling filter isn't pinned.
- `hardening/`: files broken in one way each, with valid CRCs (2.17, item
  4), at least one for every check of 2.4.3, so a reader that skips a
  check fails on its file. `index.json` lists each with the reader's uses
  (`all`: every section; `device`: MPTG's FOLD, RECS and STRS, MPDJ's
  DJRF, DJPH and STRS, MSMF's COMP, LIBR and STRS, MPWF's frame: the device
  never reads ORIG, OSTR or SRVS, and a slot of DJSL or WVSL only when it
  needs one), whether it must read as absent or present, the check it
  breaks, and the player's reason code. A `present` file with
  `same` reads as the same records as that golden file (MPDJ: every row's
  filter record and used slot entries, every DJPH hash's row, the five
  strings; MPWF: every slot's hash, length and bars). An MPDJ file's `slot`
  says how one slot reads when the device reads it alone (`damaged`: its
  CRC fails; `broken`: a good CRC over a bad list). An MPWF file's `slot` is
  one lookup as the device makes it: the hash, what it reads (`hit`, `miss`,
  `damaged`, `broken`) and how many slots it reads.
- `moods.json`, `golden/tags-moods.bin` and the hardening files
  `mptg-moods-longer`, `mptg-tail-invalid` and `mptg-stride-76`: mood
  filters (docs/MOOD-FILTERS.md 5.3, 5.4; METADATA 2.6.4's mood tail and
  3.4.3's mood word), made with `tools/moodfilter.py`, which
  card_fixtures.py imports. A tags description's record `scores` are
  mStream's eight mood scores in `moods.json`'s `heads` order (null where a
  head has none), or null for no server row: each writer derives the
  record's tail from them (RECS rows of 80 bytes, minor 1). An MPTG file's
  `same` covers its records' scores too. In `moods.json`: `values` (a
  score's value byte and bucket), `levels` (each level's code in a filter,
  its `need` and `under`), `bars` (its `on` and `mirror`), `predicates` (the
  eleven of MOOD-FILTERS.md 4.1, each a one-group filter's bits at
  Standard), `rows` (scores, the tail a writer makes of them, the word a
  builder makes of the tail, and `holds`: for each level, `strict`,
  `standard` and `loose`, the predicates that hold, in `predicates` order;
  any other fails), `tails` (whether a reader takes each, and its word),
  `filters` (a filter's bits, what they read as, and the rows it
  `matches`, by index from 0) and `albums` (rows as one album's tracks
  under a filter: `pass` of `analysed`, and whether 2 × pass > analysed).
  `tools/test_mood_vectors.py` checks every `holds` and `matches` against
  mStream's own predicate, run by SQLite.

The AutoDJ index (MPDJ v2) is specified apart, in docs/AUTODJ-INDEX.md;
its fixtures (section 13 there) come from `tools/mpdj.py`, which
card_fixtures.py imports. `libraries/autodj-mid.f32` holds the vectors
that `autodj-mid.json` and `autodj-mid-default.json` name (raw
little-endian f32, 16 a row, in the descriptions' row order).

The waveform index (MPWF v1) is specified apart, in
docs/WAVEFORM-INDEX.md; its fixtures (section 13 there) come from
`tools/mpwf.py`, which card_fixtures.py imports. A description's tracks
hold the server's peak bars as given, and a writer computes the bars.
`golden/manifest-waves.bin` is a root that names all three companions. The
`msmf-waves-*` hardening files are roots that read whatever their MPWF row
holds (`vectors.json`'s `mpwf.rootRow` says what a reader takes from each),
but for the two whose name breaks a string rule, as any kind's would.

The play log (docs/METADATA.md 2.20) has its fixtures in `plays/`, made
with `tools/playlog.py`, its reference reader and writer, which
card_fixtures.py imports. `plays/log.json` is one device's script on the
fixture card (every value a writer would otherwise choose given; a cap of
4 slots a file, so the rotation shows). `plays/golden/<snapshot>/` holds
the log's files at each of its snapshots: what a writer MUST make of the
script, byte for byte. `plays/index.json` lists the reader's cases, each
a card's log files (a golden one by its path, a case's own in
`plays/read/<case>/`) with what a reader takes from them: the plays' ids
in order, the counts of what it skipped, and where given the plays'
starts and what their enums read as. `golden/manifest-acks.bin` is a root
with ACKS (2.5.6); the `msmf-acks-*` hardening files are roots that
read, whatever their ACKS holds (`vectors.json`'s `plays.acks` says what
a reader that uses ACKS takes from each). So a reader takes ACKS outside
its container's check of known sections, whose stride and length rules
would make the root absent: it finds ACKS by its type in the directory
and checks it on its own.

The listening stats (docs/LISTENING-STATS.md: `/.player/stats.bin` and
its base file, 5.2-5.6) have their fixtures in `stats/`, made with
`tools/statsref.py`, their reference, which card_fixtures.py imports.
`stats/index.json` lists the scenarios. Each is a card's `/.player` before
its first boot (the log's files, and a `stats.bin` and its base where an
earlier boot left them), the booting device (`device`: its deviceId and
epoch, which only the boot rules use: the stats fold every device's
plays) and its boots in turn. A boot gives the files it starts from (`in`)
and leaves (`out`), each name mapped to the folder that holds its bytes (a
file is stored once, in the first folder with those bytes; files changed
between two boots, a play the device wrote, are named in `added` and
stored in `<boot>-in`); its inputs (`acks`: the root's ACKS rows; `now`:
the board's clock at the save in Unix ms, null while unset, and `today`,
its UTC day counted from 2026-01-01; `commitTime`: the root's, Unix s;
`playing` and `safePoint`, for the compaction's rule; `saveFails`: a full
card; `unread`: the scan's card faults, each log file's name mapped to the
slots read before a read failed, or null for an open that failed); and
what a device MUST make of them: `out` byte for byte, what the read found
(`read`: loaded, absent, corrupt, newer or unread, and `why`, the check
that failed by CardContainer's name), the slots folded, met a second time,
and missing under ACKS (`folded`, `twice`, `gap`), whether the scan was
partial (`partial`: no save follows), whether `stats.bin` was written
(`saved`) and the base compacted, the files removed and the log files
trimmed, and, read back where it reads, HEAD and MARK (`stats`), the
shelves' lists (`lists`, by path hash, each its cap plus 16: `mostPlayed`
per window [hash, plays in it]; `newFinds` [hash, plays, firstAt, its
source]; `oldFavourites` [hash, plays, lastAt]; `recentlyPlayed` [hash,
plays, lastAt, lastOrd]; `notPlayed` by album) and the hub's numbers
(`view`). Each log file keeps one device and epoch's seqs in order, as a
device writes them (5.6's ranges rely on it). `stats/library.json` is the
made-up library behind Not played yet: its albums in A-Z order, each
track's path and hash. `stats/sessions.json` lists the sessions: players'
boots and plays in turn on one card (`in`, under `stats/sessions/`), the
play log written as tools/playlog.py's Device writes it and the stats kept
as a device that keeps them does (the live fold of each play written, the
rotation's save before plays.1.jnl leaves the log's names, `Hs`, the
orderly end's flush, another player's `stats.bin` refusing a session's
later saves, a 0.14.0 boot, NVS put back or erased, a log file deleted by
hand, a library update's give-back and the re-read after it). Each player
gives its deviceId, the epochs a fresh NVS draws and
its NVS as the session starts (null: erased); each step its own fields (a
boot's as a boot's above, and `stats` false for a 0.14.0 boot; a play's
slot fields, `now` at a save it brings, `saveFails`, and `ending` with the
budget `left` for the end's flush) and what a device MUST make of it: the
card after it (`out`), a boot's facts as above, a play's seq and its
rotation's save (`save`: null when none is asked, else whether it landed),
Hs's `ok`, a give-back's save (`saved`: null when nothing was new) and the
re-read's facts (`read`, `same`: the copy that held every slot the session
held, the ring's plays `folded` and `covered`, those `over` it, and whether
the session is `behind` its log, every later save refused). `slots` and `lost`: the slots that count the card ever held and
those the stats lose (5.5's undercount, to the slot). Nothing but the
device reads these files: a mirror of this folder MAY leave `stats/` out
(docs/METADATA.md 2.17, item 10).

The player's tests: `test/test_card_contract` (the vectors),
`test/test_card_files` (the goldens, round trips, the hardening files,
truncation at every byte and every flipped bit), `test/test_card_autodj`
(the AutoDJ index's own rules), `test/test_card_waves` (the waveform
index's own rules) and `test/test_name_key` (the keys the AutoDJ
descriptions give). The play log's fixtures are for the
firmware's log and the transfer software's reader, neither built yet;
`tools/test_playlog.py` reads them today. The stats' fixtures are for the
firmware's stats (docs/LISTENING-STATS.md S1-S2), not built yet;
`tools/test_statsref.py` checks them today.
"""


def generate():
    files = {}
    descs = [f() for f in LIBRARIES]
    dj_descs = mpdj.fixture_libraries()
    wf_descs = mpwf.fixture_libraries()
    # The AutoDJ and waveform goldens first (each read back and checked
    # against its rule by tools/mpdj.py and tools/mpwf.py), then the other
    # companions: a root reads their generation, length, header CRC and key
    # from them.
    golden = dict(mpdj.fixture_goldens())
    golden.update(mpwf.fixture_goldens())
    for d in sorted(descs, key=lambda d: d["format"] == "MSMF"):
        golden[d["file"]] = build(d, golden)
    for d in descs + dj_descs + wf_descs:
        files["libraries/" + d["file"].rsplit(".", 1)[0] + ".json"] = (
            json.dumps(d, indent=1, ensure_ascii=False) + "\n").encode("utf-8")
    for name, data in mpdj.fixture_sidecars().items():
        files["libraries/" + name] = data
    for name, data in golden.items():
        files["golden/" + name] = data
    files.update(plays_files(golden)[0])
    files.update(statsref.fixture_files())
    v = vectors(golden)
    check_vectors(v, golden)
    files["vectors.json"] = (json.dumps(v, indent=1, ensure_ascii=False) + "\n").encode("utf-8")
    m = moods(golden)
    check_moods(m, golden)
    files["moods.json"] = (json.dumps(m, indent=1, ensure_ascii=False) + "\n").encode("utf-8")
    index = []
    for entry in hardening(golden):
        name, fmt, uses, expect, why, check, data = entry[:7]
        same = entry[7] if len(entry) > 7 else None
        slot = entry[8] if len(entry) > 8 else None
        files["hardening/" + name + ".bin"] = data
        e = {"file": name + ".bin", "format": fmt, "uses": uses, "expect": expect, "why": why, "check": check}
        if same:
            e["same"] = same
        if slot:
            e["slot"] = slot
        index.append(e)
    files["hardening/index.json"] = (json.dumps(index, indent=1, ensure_ascii=False) + "\n").encode("utf-8")
    files["README.md"] = README.encode("utf-8")
    return files


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="compare with the files on disk; write nothing")
    args = ap.parse_args()
    files = generate()
    # A file in the folder that generate() doesn't make (a retired fixture)
    # is stale too; .gitattributes is the folder's own.
    extra = sorted(q.relative_to(OUT).as_posix() for q in OUT.rglob("*")
                   if q.is_file() and q.relative_to(OUT).as_posix() not in files and q.name != ".gitattributes")
    if args.check:
        stale = [n for n, d in files.items() if not (OUT / n).is_file() or (OUT / n).read_bytes() != d]
        if stale or extra:
            sys.exit("card fixtures differ from tools/card_fixtures.py: " + ", ".join(sorted(stale)) +
                     ("; not made by it: " + ", ".join(extra) if extra else ""))
        print("card fixtures: %d files up to date" % len(files))
        return
    for n in extra:
        (OUT / n).unlink()
    for n, d in files.items():
        path = OUT / n
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(d)
    print("card fixtures: wrote %d files to %s%s" % (len(files), OUT.relative_to(ROOT).as_posix(),
                                                       ", deleted %d stale" % len(extra) if extra else ""))


if __name__ == "__main__":
    main()
