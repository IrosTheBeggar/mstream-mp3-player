#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""The card contract's shared fixtures (docs/METADATA.md 2.17, 2.18).

Writes test/fixtures/card/: the conformance vectors (vectors.json), the
library descriptions (libraries/*.json), their golden files (golden/*), and
the hardening files (hardening/*.bin, listed in hardening/index.json), each
broken in one way under valid CRCs. The player's host tests
(test/test_card_contract, test/test_card_files) read them, as the transfer
software's tests in mstream-terminal will.

This is a second implementation of the contract, written from the spec and
kept apart from lib/core/CardContract*: the goldens come from here, so the
C++ writers matching them byte for byte is two implementations agreeing,
not one agreeing with itself. Every vector of 2.18 is recomputed and
checked before anything is written.

    python tools/card_fixtures.py          write the fixtures
    python tools/card_fixtures.py --check  fail if a file on disk differs

The fixtures are made-up libraries (no real names) and synthetic
embeddings, under CC0 (test/fixtures/card/README.md; part 7, U18).
"""
import argparse
import hashlib
import json
import math
import struct
import sys
import unicodedata
import zlib
from pathlib import Path

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


def build_container(magic, generation, card_id, type_header: bytes, sections, major=1, minor=0):
    """sections: [(type, flags, count, stride, data)] in order."""
    header_bytes = 40 + len(type_header)
    assert header_bytes % 8 == 0
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


def mptg(desc, order="canonical", rec_stride=72, extra_header=b"", extra_run_field=None, extra_sections=()):
    """A tags file from a library description (and the knobs the hardening
    files need)."""
    recs = desc.get("records", [])
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
        rows += row + bytes([0xAB]) * (rec_stride - 72)
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
            orig += struct.pack("<IIII16s16sQQIHBBH6s", ostr.add(l.get("serverPath", "")), l.get("mstreamId", 0),
                                l.get("albumId", 0), l.get("artistId", 0), bytes.fromhex(l.get("audioHash", "00" * 16)),
                                bytes.fromhex(l.get("fileHash", "00" * 16)), l.get("serverModified", 0),
                                l.get("serverSize", 0), l.get("createdAt", 0), l.get("hashV", 0),
                                l.get("originFlags", 0), l.get("convertedTo", 0), l.get("convertKbps", 0), bytes(6))
        sections.append(("ORIG", 0, len(recs), 80, bytes(orig)))
        sections.append(("OSTR", 0, 0, 0, bytes(ostr.b)))
    sections += list(extra_sections)
    th = struct.pack("<BBHHHIIIIII", desc["source"], 0, desc.get("parserVersion", 0), desc.get("readRules", 1), 0,
                     len(recs), len(fpaths), len(albums), len(artists), producer, 0) + extra_header
    return build_container("MPTG", desc["generation"], u64(desc.get("cardId", 0)), th, sections,
                           minor=desc.get("minor", 0))


# ---------------------------------------------------------------------------
# MSMF (2.5), MSPD (2.12.5), MPDJ (2.13), MPTH (2.14.1), device.txt (2.15)
# ---------------------------------------------------------------------------
def companion_name(kind, gen):
    return ("tags-%08x.bin" if kind == "MPTG" else "autodj-%08x.bin") % gen


def msmf_raw(desc, comps_rows, roots, minor=0):
    """comps_rows: [(kind, generation, fileBytes, headerCrc, name, key)] as given."""
    strs = Strs()
    producer = strs.add(desc.get("producer", ""))
    revision = strs.add(desc.get("serverRevision", ""))
    comp = bytearray()
    for kind, gen, fb, hc, name, key in comps_rows:
        comp += struct.pack("<4sIIIII16s", fourcc(kind), gen, fb, hc, strs.add(name), 0, key)
    libr = b"".join(struct.pack("<I", strs.add(r)) for r in roots)
    instance = bytes.fromhex(desc.get("serverInstance", "0" * 32).replace("-", ""))
    key = server_url_key(desc["serverUrl"]) if desc.get("serverUrl") else u64(desc.get("serverUrlKey", 0))
    th = struct.pack("<IIQ16sIIIIQ", desc.get("commitTime", 0), desc.get("flags", 0), u64(desc.get("commitId", 0)),
                     instance, producer, revision, desc.get("baseGeneration", 0), 0, key)
    sections = [("COMP", REQUIRED, len(comps_rows), 40, bytes(comp))]
    if roots:
        sections.append(("LIBR", 0, len(roots), 4, libr))
    sections.append(("STRS", REQUIRED, 0, 0, bytes(strs.b)))
    return build_container("MSMF", desc["generation"], u64(desc.get("cardId", 0)), th, sections, minor=minor)


def comp_row(name: str, data: bytes):
    """A COMP entry for the companion file `name` holding `data` (2.5.3)."""
    p = parse_container(data)
    key = bytes(p["th"][16:32]) if p["magic"] == "MPDJ" else bytes(16)
    assert name == companion_name(p["magic"], p["generation"]), name
    return p["magic"], p["generation"], len(data), struct.unpack_from("<I", data, 32)[0], name, key


def msmf(desc, golden: dict):
    rows = [comp_row(c["file"], golden[c["file"]]) for c in desc["companions"]]
    roots = sorted(desc.get("roots", []), key=lambda r: r.encode("utf-8"))
    return msmf_raw(desc, rows, roots, desc.get("minor", 0))


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


def f32(v):
    return struct.unpack("<f", struct.pack("<f", float(v)))[0]


def round_half_away(v):
    a = abs(v)
    f = math.floor(a)
    r = f + 1 if a - f >= 0.5 else f
    return r if v >= 0 else -r


def dj_score(a, b):
    s = 0.0
    for x, y in zip(a, b):
        s += x * y
    return max(0, min(255, round_half_away(s * 255.0)))


def selection_signature(model_id, model_version, metric, k, hashes):
    text = "MPDJ-SEL 1\nmodel %s %s\nmetric %s\nk %d\n" % (model_id, model_version, metric, k)
    text += "".join(h + "\n" for h in sorted(hashes))
    return hashlib.sha256(text.encode("utf-8")).digest()[:16]


def mpdj(desc):
    k = desc["k"]
    rows = sorted(desc["rows"], key=lambda r: bytes.fromhex(r["hash"]))
    n = len(rows)
    ib = 2 if n <= 65535 else 4
    unused = (1 << (8 * ib)) - 1
    emb = [[f32(x) for x in r["embedding"]] for r in rows]
    djrw, paths = bytearray(), []
    for i, r in enumerate(rows):
        ph = sorted(path_hash(p) for p in r["paths"])
        paths += [(h, i) for h in ph]
        ak = fnv(r["artistNameKey"].encode("utf-8")) & 0xFFFFFFFF if r.get("artistNameKey") else 0
        djrw += struct.pack("<Q8sHBBI", ph[0], bytes.fromhex(r["hash"])[:8], r.get("bpm10", 0), r.get("camelot", 0),
                            r.get("flags", 0), ak)
    paths.sort()
    djnb = bytearray()
    for a in range(n):
        cand = sorted((-dj_score(emb[a], emb[b]), b) for b in range(n)
                      if b != a and rows[b]["songKey"] != rows[a]["songKey"])
        for j in range(k):
            if j < len(cand):
                djnb += cand[j][1].to_bytes(ib, "little") + bytes([-cand[j][0]])
            else:
                djnb += unused.to_bytes(ib, "little") + b"\0"
    sig = selection_signature(desc["modelId"], desc["modelVersion"], desc["metric"], k, [r["hash"] for r in rows])
    strs = Strs()
    th = struct.pack("<IIHBBI16sIIIIII", n, len(paths), k, ib, 1, desc.get("builtTime", 0), sig,
                     strs.add(desc["modelId"]), strs.add(desc["modelVersion"]), strs.add(desc["metric"]),
                     desc.get("tagsGeneration", 0), strs.add(desc.get("license", "")),
                     strs.add(desc.get("attribution", "")))
    sections = [("DJRW", REQUIRED, n, 24, bytes(djrw)), ("DJNB", REQUIRED, n, k * (ib + 1), bytes(djnb)),
                ("DJPH", REQUIRED, len(paths), 12, b"".join(struct.pack("<QI", h, r) for h, r in paths)),
                ("STRS", REQUIRED, 0, 0, bytes(strs.b))]
    return build_container("MPDJ", desc["generation"], u64(desc.get("cardId", 0)), th, sections,
                           minor=desc.get("minor", 0))


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
    keys = ["contract", "firmware", "read.msmf", "read.mptg", "read.mpdj", "read.mpth", "codecs", "extensions",
            "max_rate", "max_channels"]
    return "".join("%s=%s\n" % (k, desc[k]) for k in keys).encode("ascii")


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


def autodj_main():
    half = [0.5, 0.5, 0.5, 0.5]
    rows = [
        {"hash": "0123456789abcdef0123456789abcdef", "paths": ["Artist/Album/01 - Title.mp3"],
         "artist": "Lantern Choir", "title": "Title", "artistNameKey": "lantern choir", "songKey": "lantern choir|title",
         "bpm10": 1210, "camelot": 8, "embedding": half},
        {"hash": "11111111111111111111111111111111", "paths": ["Artist/Album/02 - Other.mp3"],
         "artist": "X", "title": "A B", "artistNameKey": "x", "songKey": "x, y|a b", "embedding": [0.5, 0.5, 0.5, -0.5]},
        {"hash": "22222222222222222222222222222222",
         "paths": [NFC_CAFE + "/Album/01 - Title.mp3", NFD_CAFE + "/Album/01 - Title.mp3"],
         "artist": "Caf\u00e9 Trio", "title": "Title", "artistNameKey": "caf\u00e9 trio",
         "songKey": "caf\u00e9 trio|title", "note": "one recording at two paths: one row",
         "embedding": [0.625, 0.5, 0.375, 0.25]},
        {"hash": "33333333333333333333333333333333", "paths": ["A B/y.mp3"], "artist": "Some Band, Other Band",
         "title": "Why", "artistNameKey": "some band, other band", "songKey": "some band, other band|why",
         "bpm10": 1280, "camelot": 13, "flags": 1, "embedding": [0.5, -0.5, 0.5, 0.5]},
        {"hash": "44444444444444444444444444444444", "paths": ["B/w.opus"], "artist": "", "title": "Double U",
         "artistNameKey": "", "songKey": "|double u", "embedding": [-0.5, 0.5, 0.5, 0.5]},
        {"hash": "fedcba9876543210fedcba9876543210", "paths": ["Loose Intro.mp3"], "artist": "Lantern Choir",
         "title": "Title", "artistNameKey": "lantern choir", "songKey": "lantern choir|title",
         "note": "the same song as the first row: never its neighbour", "embedding": half},
    ]
    return {"format": "MPDJ", "file": "autodj-00000029.bin", "generation": 41, "cardId": CARD_ID, "k": 3,
            "builtTime": 1790000000, "tagsGeneration": 41, "modelId": "synthetic-embedding", "modelVersion": "1",
            "metric": "cosine", "license": "CC0-1.0", "attribution": "synthetic fixture vectors", "dim": 4,
            "note": "Synthetic embeddings (multiples of 1/8, exact in f32 and in f64 products): a tie of three at "
                    "the K boundary for the first row, a same-song pair, a recording at two paths. songKey and "
                    "artistNameKey are nameKey's output (5.4), given.",
            "rows": rows}


def autodj_small():
    rows = [
        {"hash": "aa000000000000000000000000000000", "paths": ["A/x.mp3"], "artistNameKey": "",
         "songKey": "|x", "embedding": [1.0, 0.0]},
        {"hash": "bb000000000000000000000000000000", "paths": ["a/v.mp3"], "artistNameKey": "",
         "songKey": "|v", "embedding": [-1.0, 0.0]},
    ]
    return {"format": "MPDJ", "file": "autodj-small.bin", "generation": 7, "cardId": CARD_ID, "k": 3,
            "builtTime": 1790000000, "tagsGeneration": 7, "modelId": "synthetic-embedding", "modelVersion": "1",
            "metric": "cosine", "license": "CC0-1.0", "attribution": "synthetic fixture vectors", "dim": 2,
            "note": "Fewer rows than K: unused slots; a negative cosine scores 0.", "rows": rows}


def manifest_main():
    return {"format": "MSMF", "file": "manifest.bin", "generation": 42, "cardId": CARD_ID, "commitTime": 1791400000,
            "flags": 1, "commitId": "0x0DDBA11CAFEF00D5", "serverInstance": "00112233-4455-6677-8899-aabbccddeeff",
            "producer": "mstream-terminal 0.13.0", "serverRevision": "\"r-412-99-1790000000999-300\"",
            "baseGeneration": 41, "serverUrl": "HTTP://Music.Example:3000/",
            "note": "The root of the fixture card: the tags file of this commit and an AutoDJ table carried from "
                    "the last (generation 41); the vpath layout's roots, given unsorted.",
            "companions": [{"file": "tags-0000002a.bin"}, {"file": "autodj-00000029.bin"}],
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
    return {"format": "device.txt", "file": "device.txt", "contract": "1", "firmware": "0.8.0", "read.msmf": "1",
            "read.mptg": "1", "read.mpdj": "1", "read.mpth": "1", "codecs": "mp3,flac,opus",
            "extensions": "mp3,flac,opus", "max_rate": "48000", "max_channels": "2"}


LIBRARIES = [tags_transfer, tags_device, tags_empty, autodj_main, autodj_small, manifest_min, manifest_main,
             pending_main, thumb_main, device_main]


def build(desc, golden: dict):
    f = desc["format"]
    if f == "MPTG":
        return mptg(desc)
    if f == "MSMF":
        return msmf(desc, golden)
    if f == "MPDJ":
        return mpdj(desc)
    if f == "MSPD":
        return mspd(desc)
    if f == "MPTH":
        return mpth(desc)
    if f == "device.txt":
        return device_txt(desc)
    raise ValueError(f)


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
            obs = fat_from_wall(fat_wall(rec) + g["delta"])
            out.append((rec, obs))
    return out


def vectors():
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
    ]
    v["skewNote"] = ("pairs: recorded = 2026-10-07 14:30:42 + 2 days x i, observed = recorded + delta; 'matching' "
                     "counts the pairs whose time matches under the skew. A pair with a 0 or invalid stamp is "
                     "left out and never matches.")
    v["bytes"] = {
        "MPTG": "4D 50 54 47", "MPTGu32": "0x4754504D", "MPTHu32": "0x4854504D",
        "thumbPath": {"folder": "Artist/Album", "path": "/.mstream/thumbs/B/B1F7E69F.565", "bytes": 21656,
                      "head0to15": "4D 50 54 48 01 00 00 00 59 6B 46 BD 9F E6 F7 B1", "head20to23": "28 00 60 00"},
        "serverInstance": {"uuid": "00112233-4455-6677-8899-aabbccddeeff",
                           "stored": "00 11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF"},
        "hashPrefix": {"audioHash": "0123456789abcdef0123456789abcdef", "stored": "01 23 45 67 89 AB CD EF"},
        "selectionSigStored": "9F 0A E9 1F 22 0E 2E 76 EC 5A 40 E4 9F C8 0B 2D",
    }
    v["selectionSignature"] = [
        {"modelId": "discogs-effnet", "modelVersion": "1", "metric": "cosine", "k": 100,
         "hashes": ["0123456789abcdef0123456789abcdef", "fedcba9876543210fedcba9876543210"],
         "sig": "9f0ae91f220e2e76ec5a40e49fc80b2d"},
        {"modelId": "discogs-effnet", "modelVersion": "1", "metric": "cosine", "k": 100, "hashes": [],
         "sig": "efdb6ffa02a177c18593a3ed34f7016b"},
    ]
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
    v["deviceTxt"] = {"text": device_txt(device_main()).decode("ascii"),
                      "noFile": {"read": "1", "codecs": "mp3,flac", "extensions": "mp3,flac", "max_rate": "48000",
                                 "max_channels": "2"}}
    return v


def elect(b, t):
    """2.5.4: (which root counts, whether the two are one commit)."""
    if b["valid"] and t["valid"]:
        same = b["commitId"] == t["commitId"] and b["headerCrc"] == t["headerCrc"]
        return ("tmp" if t["generation"] > b["generation"] else "bin"), same
    return ("bin" if b["valid"] else "tmp" if t["valid"] else "none"), False


def hexint(s):
    return int(s, 16)


def check_vectors(v):
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
    for e in v["selectionSignature"]:
        want(selection_signature(e["modelId"], e["modelVersion"], e["metric"], e["k"], e["hashes"]).hex() == e["sig"],
             "sig")
    want(" ".join("%02X" % b for b in bytes.fromhex(v["selectionSignature"][0]["sig"])) == v["bytes"][
        "selectionSigStored"], "sig bytes")
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
    want(bytes.fromhex(v["bytes"]["hashPrefix"]["audioHash"])[:8].hex(" ").upper() == v["bytes"]["hashPrefix"][
        "stored"], "prefix")
    if bad:
        raise SystemExit("vectors that don't match: " + ", ".join(bad))


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
         "a newer minor: 8 more header bytes, RECS rows of 80 bytes, a 13th run field, an unknown section",
         mptg(dict(desc, minor=1), rec_stride=80, extra_header=b"newminor", extra_run_field="a newer field",
              extra_sections=[("XTRA", 0, 0, 0, b"new")]), same="tags-0000002a.bin")

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
    bad("msmf-major-2", "MSMF", "major", "a newer major", edit(major, man))

    # MPDJ.
    dj = golden["autodj-00000029.bin"]
    bad("mpdj-index-bytes", "MPDJ", "enum", "header: indexBytes 4 for 6 rows", edit(header_field(50, 4, "<B"), dj))
    bad("mpdj-k-zero", "MPDJ", "enum", "header: k 0", edit(header_field(48, 0, "<H"), dj))
    bad("mpdj-score-kind", "MPDJ", "enum", "header: scoreKind 2", edit(header_field(51, 2, "<B"), dj))

    def dj_rows(p):
        s = section(p, "DJRW")
        s[4][0:24], s[4][24:48] = s[4][24:48], s[4][0:24]

    bad("mpdj-rows-unsorted", "MPDJ", "djRows", "DJRW: rows out of hashPrefix order", edit(dj_rows, dj))

    def dj_paths(kind):
        def fn(p):
            s = section(p, "DJPH")
            if kind == "swap":
                s[4][0:12], s[4][12:24] = s[4][12:24], s[4][0:12]
            else:
                struct.pack_into("<I", s[4], 8, 6)
        return fn

    bad("mpdj-paths-unsorted", "MPDJ", "djPaths", "DJPH: entries out of order", edit(dj_paths("swap"), dj))
    bad("mpdj-paths-row-range", "MPDJ", "djPaths", "DJPH: a row out of range", edit(dj_paths("range"), dj))

    def dj_nb(p):
        s = section(p, "DJNB")
        struct.pack_into("<H", s[4], 0, 6)

    bad("mpdj-neighbour-range", "MPDJ", "djNeighbours", "DJNB: an index out of range (not the unused all-ones)",
        edit(dj_nb, dj))

    # MSPD.
    pd = pending_main()
    bad("mspd-order", "MSPD", "pendOrder", "PEND: a write before a delete",
        mspd_raw(pd, [(1, "A B/y.mp3", 77), (2, "A/x.mp3", 0)]))
    bad("mspd-group-order", "MSPD", "pendOrder", "PEND: two deletes out of canonical order",
        mspd_raw(pd, [(2, "A B/y.mp3", 0), (2, "A/x.mp3", 0)]))
    bad("mspd-absolute", "MSPD", "pendPath", "PEND: an absolute path", mspd_raw(pd, [(2, "/A/x.mp3", 0)]))
    bad("mspd-dotdot", "MSPD", "pendPath", "PEND: a path through '..'", mspd_raw(pd, [(2, "../x.mp3", 0)]))
    bad("mspd-op-0", "MSPD", "pendOp", "PEND: an op of 0", mspd_raw(pd, [(0, "A/x.mp3", 0)]))
    good("mspd-op-9", "MSPD", "PEND: an op above 3: the plan reads, and stops the software",
         mspd_raw(pd, [(2, "A/x.mp3", 0), (9, "Z/z.mp3", 0), (1, "A B/y.mp3", 77)]))
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
--check` fails if a file here differs from what it makes. Made-up names
only, synthetic embeddings.

- `vectors.json`: the vectors of 2.18 as data. `who` says which side a
  group binds when it isn't both.
- `libraries/*.json`: library descriptions, the input of a writer: every
  value a writer would otherwise choose is given (ids, times,
  generations). Tag values are raw lists: each writer applies 2.3.6 and
  sets TRUNCATED; flags, `known` and the enums are as given. Integers above
  2^53 are hex strings. A manifest names its companions by file; their
  generation, length, header CRC and (MPDJ) selection signature come from
  the golden companion.
- `golden/`: what a writer MUST produce from each description, byte for
  byte (2.17, item 2). The MPTH pixels are the synthetic picture the
  description names: the scaling filter isn't pinned.
- `hardening/`: files broken in one way each, with valid CRCs (2.17, item
  4). `index.json` lists each with the reader's uses (`all`: every
  section; `device`: FOLD, RECS and STRS only), whether it must read as
  absent or present, the check it breaks, and the player's reason code.
  A `present` file with `same` reads as the same records as that golden
  file.

The player's tests: `test/test_card_contract` (the vectors) and
`test/test_card_files` (the goldens, round trips, the hardening files,
truncation at every byte and every flipped bit).
"""


def generate():
    files = {}
    descs = [f() for f in LIBRARIES]
    golden = {}
    # Companions first: a root reads their generation, length, header CRC
    # and key from them.
    for d in sorted(descs, key=lambda d: d["format"] == "MSMF"):
        golden[d["file"]] = build(d, golden)
    for d in descs:
        files["libraries/" + d["file"].rsplit(".", 1)[0] + ".json"] = (
            json.dumps(d, indent=1, ensure_ascii=False) + "\n").encode("utf-8")
    for name, data in golden.items():
        files["golden/" + name] = data
    v = vectors()
    check_vectors(v)
    files["vectors.json"] = (json.dumps(v, indent=1, ensure_ascii=False) + "\n").encode("utf-8")
    index = []
    for entry in hardening(golden):
        name, fmt, uses, expect, why, check, data = entry[:7]
        same = entry[7] if len(entry) > 7 else None
        files["hardening/" + name + ".bin"] = data
        e = {"file": name + ".bin", "format": fmt, "uses": uses, "expect": expect, "why": why, "check": check}
        if same:
            e["same"] = same
        index.append(e)
    files["hardening/index.json"] = (json.dumps(index, indent=1, ensure_ascii=False) + "\n").encode("utf-8")
    files["README.md"] = README.encode("utf-8")
    return files


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="compare with the files on disk; write nothing")
    args = ap.parse_args()
    files = generate()
    if args.check:
        stale = [n for n, d in files.items() if not (OUT / n).is_file() or (OUT / n).read_bytes() != d]
        if stale:
            sys.exit("card fixtures differ from tools/card_fixtures.py: " + ", ".join(sorted(stale)))
        print("card fixtures: %d files up to date" % len(files))
        return
    for n, d in files.items():
        path = OUT / n
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(d)
    print("card fixtures: wrote %d files to %s" % (len(files), OUT.relative_to(ROOT)))


if __name__ == "__main__":
    main()
