#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""The AutoDJ index, autodj-<gen>.bin (MPDJ v2, docs/AUTODJ-INDEX.md): the
reference writer and reader, and the maker of its test vectors.

Every rule a second writer must reproduce byte for byte is here in its
plainest form, with the spec section it implements. card_fixtures.py
imports it for every MPDJ fixture in test/fixtures/card, and test_mpdj.py
tests it. Standard library only: numpy, when it is installed, is an
optional fast path for library-sized inputs (an f64 matrix product, every
pair near a rounding boundary recomputed by the rule, 12.1), which gives
the same bytes; nothing here needs it.

    python tools/mpdj.py build DESC.json OUT.bin [--f32] [--fast|--no-fast]
    python tools/mpdj.py check FILE.bin [--uses all|device]   exit 1 when absent; prints the reason
    python tools/mpdj.py dump FILE.bin [--slot R] [--hex]
    python tools/mpdj.py signature DESC.json                  the selection signature's text and digest
    python tools/mpdj.py restamp OLD.bin DESC.json OUT.bin    a new file around OLD's lists (7.3)
    python tools/mpdj.py size ROWS [--paths P] [--k K] [--strings S]

`build` uses the fast path when numpy is there and the description has
1,000 rows or more (--fast / --no-fast force it). `--f32` builds with the
levels of a float32 sum instead of the rule: a deliberately wrong writer,
for the float32 trap (12.3).

The description (DESC.json; AUTODJ-INDEX.md 13.2) is the writer's whole
input, every value a writer would otherwise choose given:

    {"format": "MPDJ", "file": "<golden name>.bin",
     "major": 2, "minor": 0,                          optional, these defaults
     "generation": 41, "cardId": "0x5EEDC0DE0A1B2C3D", "builtTime": 1790000000,
     "tagsGeneration": 41,
     "k": 4, "selKind": 1, "selHead": 1, "selFloor": 100,
                                                      selKind 0: selHead and selFloor
                                                      left out (written as k and 0)
     "modelId": "synthetic-embedding", "modelVersion": "1", "metric": "cosine",
     "license": "CC0-1.0", "attribution": "synthetic test vectors",
     "dim": 8,
     "vectors": "<file>.f32",                         optional: raw little-endian f32,
                                                      dim per row, rows in the
                                                      description's order; else each
                                                      row's "embedding"
     "note": "...",
     "rows": [{"hash": "<32 lowercase hex>",
               "files": [{"path": "<relative to /music, as the card stores it>",
                          "artists": ["raw value", ...],   the MPTG record's artist list
                          "title": "raw",                   absent: no title field
                          "bpm10": 0, "camelot": 0,
                          "bpmAnalysed": false}],           optional, these defaults
               "embedding": [...],                 numbers that are exactly f32 values
               "primaryPath": "...", "artistNameKey": "...",
               "songKey": "..." | null,            given for checking: the writer
                                                   computes them and they must agree
               "note": "..."}]}

Rows may be listed in any order; a row's files are distinct paths. The
file values are the primary file's MPTG record (METADATA 2.6.4-2.6.5):
string-run fields 0 (title) and 1 (artists), bpm10, camelot and flags bit
8 (BPM_ANALYSED). The title and artists are the record's values as
stored, so already through METADATA 2.3.6: this writer cuts, replaces and
drops nothing in them. Embedding numbers are read as f64 and rounded to
the nearest f32 (5.2).
"""
import argparse
import functools
import hashlib
import importlib.util
import json
import math
import random
import struct
import sys
import unicodedata
import zlib
from pathlib import Path

# ---------------------------------------------------------------------------
# Constants (AUTODJ-INDEX.md 4)
# ---------------------------------------------------------------------------
MAGIC = b"MPDJ"
MAJOR = 2
MIN_HEADER_BYTES = 104       # minor 0's header: bytes 0-103
REQUIRED = 1
DJSL, DJRF, DJPH, STRS = b"DJSL", b"DJRF", b"DJPH", b"STRS"
KNOWN = {DJSL: 256, DJRF: 8, DJPH: 10, STRS: 0}   # v2's strides (0: a blob)
ORDER = (DJSL, DJRF, DJPH, STRS)
FILTER_STRIDE, PATH_STRIDE = 8, 10
MIN_K, MAX_K, MAX_SMALL_K = 2, 169, 84
SMALL_SLOT, LARGE_SLOT = 256, 512
UNUSED = 0xFFFF
INDEX_BYTES = 2
SCORE_COSINE = 1
SEL_TOP, SEL_STRIDED = 0, 1
DEFAULT_K, DEFAULT_HEAD, DEFAULT_FLOOR = 169, 84, 140
FILTER_BPM_ANALYSED = 1      # DJRF flags bit 0 (the record's flags bit 8)
MAX_ROWS = 65535
MAX_STRING = 255
STRING_FIELDS = ("modelId", "modelVersion", "metric", "license", "attribution")
# The type header: rowCount, pathCount, k, slotBytes, indexBytes, scoreKind,
# selKind, selHead, builtTime, tagsGeneration, selectionSig, the five string
# offsets, selFloor, 3 reserved bytes (40-103).
TYPE_HEADER = struct.Struct("<IIHHBBBBII16sIIIIIB3x")
assert 40 + TYPE_HEADER.size == MIN_HEADER_BYTES
COMMON = struct.Struct("<4sHHIIIIQII")
DIR_ENTRY = struct.Struct("<4sIIIIIII")


class WriterError(ValueError):
    """A description no writer may turn into a file (the writer refuses)."""


# ---------------------------------------------------------------------------
# Conventions (2.2-2.4)
# ---------------------------------------------------------------------------
def crc32(data, value=0):
    """CRC-32 (ISO-HDLC, zlib's): check value 0xCBF43926."""
    return zlib.crc32(bytes(data), value) & 0xFFFFFFFF


FNV_BASIS = 0xCBF29CE484222325
FNV_PRIME = 0x00000100000001B3
M64 = (1 << 64) - 1


def fnv1a64(data, h=FNV_BASIS):
    for b in bytes(data):
        h = ((h ^ b) * FNV_PRIME) & M64
    return h


def path_hash(rel):
    """METADATA 2.3.3: FNV-1a 64 of "/music" + "/" + the path relative to it,
    as the card stores it (UTF-8)."""
    return fnv1a64(b"/music" + (b"/" + rel.encode("utf-8") if rel else b""))


_F32 = struct.Struct("<f")


def f32(v):
    """The nearest f32 to v, as a Python float."""
    return _F32.unpack(_F32.pack(v))[0]


# Unicode White_Space (PropList.txt): Rust's char::is_whitespace.
WHITE_SPACE = frozenset(map(chr, [0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x20, 0x85, 0xA0, 0x1680, *range(0x2000, 0x200B),
                                  0x2028, 0x2029, 0x202F, 0x205F, 0x3000]))
_FOLD = {**{c: "'" for c in "‘’‚‛′"},
         **{c: '"' for c in "“”„‟″"},
         **{c: "-" for c in "‐‑‒–—―−"}}


def name_key(s):
    """2.4 (METADATA 5.4): White_Space runs to one space, trimmed; the quotes
    and dashes folded; the default full lowercase with Final_Sigma. Python's
    str.lower() is that mapping in its own Unicode version (Rust's 17.0 is
    the contract's): the fixtures' names are ones every version agrees on,
    and their keys are given in the descriptions and checked."""
    out, space = [], False
    for ch in s:
        if ch in WHITE_SPACE:
            space = True
            continue
        if space and out:
            out.append(" ")
        space = False
        out.append(_FOLD.get(ch, ch))
    return "".join(out).lower()


def is_blank(s):
    """Empty or White_Space only (None too: no value)."""
    return s is None or all(ch in WHITE_SPACE for ch in s)


def trim(s):
    i, j = 0, len(s)
    while i < j and s[i] in WHITE_SPACE:
        i += 1
    while j > i and s[j - 1] in WHITE_SPACE:
        j -= 1
    return s[i:j]


def artist_display(artists):
    """The values trimmed, blank ones dropped; one as is; several
    deduplicated by nameKey (the first kept) and joined with ", "."""
    vals = [trim(v) for v in artists if not is_blank(v)]
    if len(vals) <= 1:
        return vals[0] if vals else ""
    seen, keep = set(), []
    for v in vals:
        k = name_key(v)
        if k not in seen:
            seen.add(k)
            keep.append(v)
    return ", ".join(keep)


def artist_name_key(artists):
    """nameKey of the first non-blank artist value; "" when none."""
    for v in artists:
        if not is_blank(v):
            return name_key(v)
    return ""


def artist_key(artists):
    """DJRF's artistKey: the low 32 bits of FNV-1a 64 of the UTF-8 nameKey of
    the first non-blank artist value; 0 when none."""
    k = artist_name_key(artists)
    return fnv1a64(k.encode("utf-8")) & 0xFFFFFFFF if k else 0


def song_key(artists, title):
    """None when the title is absent or blank (the row has no song key and
    is never "the same song"); else nameKey(artist display) + "|" +
    nameKey(title)."""
    if is_blank(title):
        return None
    return name_key(artist_display(artists)) + "|" + name_key(title)


# ---------------------------------------------------------------------------
# The cosine and the level (5.2)
# ---------------------------------------------------------------------------
def cos_rule(a, b):
    """The sum, in index order, of the products of two rows' f32 components,
    each product and partial sum in f64 (a product of two f32 is exact)."""
    s = 0.0
    for x, y in zip(a, b):
        s += x * y
    return s


def cos_f32(a, b):
    """What a float32 writer gets: s = 0f; for each i, s = f32(s + f32(a_i b_i)).
    (An f64 operation rounded to f32 is the f32 operation: 53 >= 2 x 24 + 2.)"""
    s = 0.0
    for x, y in zip(a, b):
        s = f32(s + f32(x * y))
    return s


def round_half_away(x):
    """C's round(): half away from zero (Python's round() goes to even, and
    floor(x + 0.5) is wrong for the largest double below 0.5)."""
    a = abs(x)
    f = math.floor(a)
    r = f + 1 if a - f >= 0.5 else f
    return r if x >= 0 else -r


def level(cos):
    """clamp(round(cos x 255), 0, 255): one f64 multiply, rounded half away
    from zero; a negative value gives 0."""
    r = round_half_away(cos * 255.0)
    return 0 if not r > 0 else 255 if r > 255 else int(r)


def level_of(a, b):
    return level(cos_rule(a, b))


def level_f32(a, b):
    return level(cos_f32(a, b))


def writes_v2(device_txt):
    """7.4: whether a writer writes MPDJ v2 for a card whose
    /.player/device.txt holds `device_txt` (None: no file): only when its
    read.mpdj lists 2. No file, or no read.mpdj key, reads as none."""
    if device_txt is None:
        return False
    for line in device_txt.splitlines():
        key, eq, value = line.partition("=")
        if eq and key == "read.mpdj":
            return "2" in [v.strip() for v in value.split(",")]
    return False


def floor_level(percent):
    """Informative (11.4): the least level a slider of `percent` hundredths
    passes, ceil(255 p / 100) in integers."""
    return (255 * percent + 99) // 100


# ---------------------------------------------------------------------------
# The geometry (3.3, 4.3)
# ---------------------------------------------------------------------------
def slot_bytes_for(k):
    """256 for k 2-84, 512 for 85-169; 0 for any other k (invalid)."""
    if MIN_K <= k <= MAX_SMALL_K:
        return SMALL_SLOT
    if MAX_SMALL_K < k <= MAX_K:
        return LARGE_SLOT
    return 0


def header_bytes_for(section_count, slot_bytes, minimum=MIN_HEADER_BYTES):
    """3.3: the smallest multiple of 8, at least the header's size, that ends
    the directory on a slot boundary (so DJSL, the first section, starts on
    one)."""
    hb = (minimum + 7) & ~7
    while (hb + 32 * section_count) % slot_bytes:
        hb += 8
    return hb


def size(rows, paths=None, k=DEFAULT_K, strings=134):
    """10.2: a minor 0 file's length: rows x (slotBytes + 8) + paths x 10,
    the header and directory (slotBytes bytes), the padding before STRS, and
    STRS (`strings` bytes; mStream's strings take 134)."""
    p = rows if paths is None else paths
    sb = slot_bytes_for(k)
    if not sb:
        raise ValueError("k must be %d-%d" % (MIN_K, MAX_K))
    at = header_bytes_for(4, sb) + 4 * 32 + rows * sb + FILTER_STRIDE * rows + PATH_STRIDE * p
    return ((at + 7) & ~7) + strings


# ---------------------------------------------------------------------------
# The selection (5.3-5.4)
# ---------------------------------------------------------------------------
def strided_ranks(k, head, m):
    """The ranks a strided list takes when m > k: 0..H-1, then
    H + floor((j - H)(m - 1 - H) / (k - 1 - H)) for H <= j < k."""
    if not (1 <= head <= k - 2 and m > k):
        raise ValueError("strided ranks need 1 <= H <= k - 2 and m > k")
    return [j if j < head else head + (j - head) * (m - 1 - head) // (k - 1 - head) for j in range(k)]


def order_candidates(levels, a, keys):
    """Row a's candidates, ordered by (level desc, index asc): every other
    row but those whose song key equals a's (both must have one). One
    256-bucket counting sort (12.2). `levels[b]` is level(a, b)."""
    buckets = [[] for _ in range(256)]
    ka = keys[a]
    for b, lv in enumerate(levels):
        if b == a or (ka is not None and keys[b] == ka):
            continue
        buckets[lv].append(b)
    return [(b, lv) for lv in range(255, -1, -1) for b in buckets[lv]]


def select(cands, kind, k, head, floor):
    """A row's entries from its ordered candidates: kind 0 the top k; kind 1
    strided by rank when more than k candidates reach the floor, else the
    top k too."""
    if kind == SEL_STRIDED:
        m = 0
        while m < len(cands) and cands[m][1] >= floor:
            m += 1
        if m > k:
            return [cands[r] for r in strided_ranks(k, head, m)]
    return cands[:k]


def level_matrix(emb, f32_sum=False):
    """Every pair's level by the rule (or a float32 sum's), as rows of
    ints. The rule's sum is symmetric (the same products in the same order),
    so each pair is computed once."""
    n = len(emb)
    fn = level_f32 if f32_sum else level_of
    m = [[0] * n for _ in range(n)]
    for a in range(n):
        ea, ma = emb[a], m[a]
        for b in range(a + 1, n):
            ma[b] = m[b][a] = fn(ea, emb[b])
    return m


def lists_rule(emb, keys, kind, k, head, floor, f32_sum=False):
    """Every row's entries [(row, level)] by the rule, as written (no
    shortcut): the reference the fast path is checked against."""
    lv = level_matrix(emb, f32_sum)
    return [select(order_candidates(lv[a], a, keys), kind, k, head, floor) for a in range(len(emb))]


def have_numpy():
    return importlib.util.find_spec("numpy") is not None


def song_ids(keys):
    """Equal ids for equal song keys; a row with none gets an id of its own."""
    ids, out = {}, []
    for i, key in enumerate(keys):
        out.append(ids.setdefault(key, len(ids)) if key is not None else -1 - i)
    return out


def lists_fast(E32, keys, kind, k, head, floor, f32_sum=False, block=512, log=None):
    """The same entries at library scale (12.1-12.2), with numpy: the levels
    from an f64 matrix product in any summation order, every pair within a
    guard band of a rounding boundary recomputed by the rule; then one
    stable 16-bit sort per row. For f32 vectors of norm about 1 and dim
    1,280 any f64 order is within 7.3e-11 of the rule after x 255; the band
    is at least 1e-9 and widened with the norms and dim.

    E32: (n, dim) float32 rows in row order. Returns (idx uint16 (n, k),
    level uint8 (n, k), entries used per row, pairs recomputed). With
    `f32_sum`, a float32 matrix product instead (the wrong writer)."""
    import numpy as np
    E32 = np.ascontiguousarray(E32, dtype=np.float32)
    n, dim = E32.shape
    E = E32.astype(np.float64)
    norm = float(np.sqrt((E * E).sum(1)).max()) if n else 0.0
    band = max(1e-9, 4.0 * 2 * dim * 2.0 ** -53 * 255.0 * norm * norm)
    ids = np.asarray(song_ids(keys), dtype=np.int64)
    idx_out = np.full((n, k), UNUSED, dtype=np.uint16)
    lv_out = np.zeros((n, k), dtype=np.uint8)
    used = np.zeros(n, dtype=np.int64)
    recomputed = 0
    j = np.arange(k, dtype=np.int64)
    for i0 in range(0, n, block):
        i1 = min(n, i0 + block)
        if f32_sum:
            V = (E32[i0:i1] @ E32.T).astype(np.float64) * 255.0
        else:
            V = (E[i0:i1] @ E.T) * 255.0
            F = np.floor(V)
            near = np.nonzero(np.abs((V - F) - 0.5) < band)
            for ra, b in zip(*near):
                a = i0 + int(ra)
                V[ra, b] = cos_rule(E32[a].tolist(), E32[int(b)].tolist()) * 255.0
            recomputed += len(near[0])
        F = np.floor(V)
        R = F + ((V - F) >= 0.5)
        L = np.where(V >= 0, R, 0).clip(0, 255).astype(np.int64)
        excl = ids[i0:i1, None] == ids[None, :]
        excl[np.arange(i1 - i0), np.arange(i0, i1)] = True
        key = (255 - L).astype(np.uint16)
        key[excl] = 256
        order = np.argsort(key, axis=1, kind="stable")
        ks = np.take_along_axis(key, order, 1).astype(np.int64)
        lvs = 255 - ks                                  # -1 for the excluded, last
        ncand = (~excl).sum(1)
        top = np.where(j[None, :] < ncand[:, None], j[None, :], -1)
        r = top
        if kind == SEL_STRIDED:
            m = (lvs >= floor).sum(1)
            big = m > k
            if big.any():
                strided = np.where(j[None, :] < head, j[None, :],
                                   head + ((j[None, :] - head) * (m[:, None] - 1 - head)) // (k - 1 - head))
                r = np.where(big[:, None], strided, top)
        rc = np.where(r < 0, 0, r)
        idx = np.take_along_axis(order, rc, 1)
        q = np.take_along_axis(lvs, rc, 1)
        idx_out[i0:i1] = np.where(r < 0, UNUSED, idx).astype(np.uint16)
        lv_out[i0:i1] = np.where(r < 0, 0, q).astype(np.uint8)
        used[i0:i1] = (r >= 0).sum(1)
        if log:
            log("rows %d/%d, pairs recomputed by the rule %d" % (i1, n, recomputed))
    return idx_out, lv_out, used, recomputed


# ---------------------------------------------------------------------------
# A slot (4.3, 8.4)
# ---------------------------------------------------------------------------
def slot_crc(slot, row):
    """CRC-32 over the 4 bytes of the row as u32 LE, then the slot's bytes
    before its CRC."""
    return crc32(bytes(slot[:len(slot) - 4]), crc32(struct.pack("<I", row)))


def encode_slot(row, entries, k, slot_bytes):
    """index[k] (u16, 0xFFFF unused), level[k] (u8, 0 unused), zero
    padding, the CRC seeded with the row."""
    if len(entries) > k:
        raise ValueError("more entries than k")
    buf = bytearray(slot_bytes)
    for i in range(k):
        struct.pack_into("<H", buf, 2 * i, entries[i][0] if i < len(entries) else UNUSED)
    for i, (_, lv) in enumerate(entries):
        buf[2 * k + i] = lv
    struct.pack_into("<I", buf, slot_bytes - 4, slot_crc(buf, row))
    return bytes(buf)


OK, DAMAGED, BROKEN = "ok", "damaged", "broken"


def check_slot(slot, k, row, row_count):
    """8.4, in order, each its own pass (so every reader gives the same
    reason): S1 the CRC (a failure: DAMAGED, only this slot); then, under a
    good CRC, the structure (a failure: BROKEN, a broken writer): S2 no used
    entry after an unused one; S3 every used index below rowCount and not
    the row; S4 levels never rise, and a tie has its indexes increasing.
    Returns (read, why, entries)."""
    sb = len(slot)
    if slot_crc(slot, row) != struct.unpack_from("<I", slot, sb - 4)[0]:
        return DAMAGED, "djSlotCrc", None
    idx = list(struct.unpack_from("<%dH" % k, slot, 0))
    c = idx.index(UNUSED) if UNUSED in idx else k
    if any(i != UNUSED for i in idx[c:]):
        return BROKEN, "djSlotUnused", None
    if any(i >= row_count or i == row for i in idx[:c]):
        return BROKEN, "djSlotIndex", None
    lv = slot[2 * k:2 * k + c]
    for j in range(1, c):
        if lv[j] > lv[j - 1] or (lv[j] == lv[j - 1] and idx[j] <= idx[j - 1]):
            return BROKEN, "djSlotOrder", None
    return OK, "ok", list(zip(idx[:c], lv))


# ---------------------------------------------------------------------------
# The selection signature (6)
# ---------------------------------------------------------------------------
def vec_digest(rows_f32, dim):
    """SHA-256 of every row's dim f32 values, little-endian, in row order."""
    h = hashlib.sha256()
    fmt = struct.Struct("<%df" % dim)
    for v in rows_f32:
        h.update(fmt.pack(*v))
    return h.hexdigest()


def same_groups(keys):
    """Rows sharing a song key, two or more (rows without one never group):
    members ascending, groups by their first member."""
    by = {}
    for i, key in enumerate(keys):
        if key is not None:
            by.setdefault(key, []).append(i)
    return sorted(g for g in by.values() if len(g) > 1)


def signature_text(model_id, model_version, metric, k, kind, head, floor, dim, vec_sha256, hashes, groups):
    lines = ["MPDJ-SEL 2", "model %s %s" % (model_id, model_version), "metric %s" % metric, "k %d" % k,
             "sel top" if kind == SEL_TOP else "sel strided %d %d" % (head, floor),
             "vec %d %s" % (dim, vec_sha256)]
    lines += list(hashes)
    lines += ["same " + " ".join(str(i) for i in g) for g in groups]
    return "".join(line + "\n" for line in lines)


def signature(text):
    return hashlib.sha256(text.encode("utf-8")).digest()[:16]


# ---------------------------------------------------------------------------
# A description, ready to write (5.1, 4.4-4.6)
# ---------------------------------------------------------------------------
def u64(v):
    return int(v, 0) if isinstance(v, str) else int(v)


def _string(desc, field, default=""):
    s = desc.get(field, default)
    if not isinstance(s, str):
        raise WriterError("%s is not a string" % field)
    b = s.encode("utf-8")
    if b"\0" in b or len(b) > MAX_STRING:
        raise WriterError("%s: at most %d bytes of UTF-8, no NUL" % (field, MAX_STRING))
    return s


class Prepared:
    """Everything of a file but its lists: the header values, the rows in
    row order with their filter records, the path map, the signature."""


def _load_vectors(desc, sidecars, base):
    name = desc["vectors"]
    if sidecars is not None and name in sidecars:
        return sidecars[name]
    if base is None:
        raise WriterError("the vectors file %s: no folder to read it from" % name)
    return Path(base, name).read_bytes()


def prepare(desc, vectors=None, sidecars=None, base=None):
    """Validates a description (the writer's refusals) and derives every
    value but the lists. `vectors`: a (rows, dim) float32 numpy array in the
    description's row order, instead of the embeddings or the sidecar (the
    synthetic card's 99 MB of vectors don't go through JSON)."""
    if desc.get("format", "MPDJ") != "MPDJ":
        raise WriterError("not an MPDJ description")
    p = Prepared()
    if int(desc.get("major", MAJOR)) != MAJOR:
        raise WriterError("this writer writes major %d" % MAJOR)
    p.minor = int(desc.get("minor", 0))
    p.generation = int(desc["generation"])
    p.card_id = u64(desc.get("cardId", 0))
    p.built_time = int(desc.get("builtTime", 0))
    p.tags_generation = int(desc.get("tagsGeneration", 0))
    if not (0 <= p.minor <= 0xFFFF and 0 <= p.generation <= 0xFFFFFFFF and 0 <= p.card_id <= M64
            and 0 <= p.built_time <= 0xFFFFFFFF and 0 <= p.tags_generation <= 0xFFFFFFFF):
        raise WriterError("a header value out of range")
    p.k = int(desc.get("k", DEFAULT_K))
    p.slot_bytes = slot_bytes_for(p.k)
    if not p.slot_bytes:
        raise WriterError("k must be %d-%d" % (MIN_K, MAX_K))
    p.kind = int(desc.get("selKind", SEL_STRIDED))
    if p.kind == SEL_TOP:
        p.head, p.floor = int(desc.get("selHead", p.k)), int(desc.get("selFloor", 0))
        if (p.head, p.floor) != (p.k, 0):
            raise WriterError("selKind 0: selHead is k and selFloor 0")
    elif p.kind == SEL_STRIDED:
        p.head, p.floor = int(desc.get("selHead", DEFAULT_HEAD)), int(desc.get("selFloor", DEFAULT_FLOOR))
        if not 1 <= p.head <= p.k - 2:
            raise WriterError("selKind 1: selHead must be 1 to k - 2")
        if not 1 <= p.floor <= 255:
            raise WriterError("selKind 1: selFloor must be 1-255")
    else:
        raise WriterError("selKind %d: this writer knows 0 and 1" % p.kind)
    p.strings = [_string(desc, f, "cosine" if f == "metric" else "") for f in STRING_FIELDS]
    p.dim = int(desc["dim"])
    if p.dim < 1:
        raise WriterError("dim must be at least 1")
    rows = desc["rows"]
    if not 1 <= len(rows) <= MAX_ROWS:
        raise WriterError("rowCount must be 1-%d" % MAX_ROWS)

    # The embeddings, in the description's order.
    np_vectors = None
    if vectors is not None:
        np_vectors = vectors
        if tuple(vectors.shape) != (len(rows), p.dim):
            raise WriterError("vectors: %d x %d expected" % (len(rows), p.dim))
        emb = None
    elif "vectors" in desc:
        raw = _load_vectors(desc, sidecars, base)
        if len(raw) != 4 * p.dim * len(rows):
            raise WriterError("the vectors file holds %d bytes, not rows x dim x 4" % len(raw))
        flat = struct.unpack("<%df" % (p.dim * len(rows)), raw)
        emb = [list(flat[i * p.dim:(i + 1) * p.dim]) for i in range(len(rows))]
    else:
        emb = []
        for r in rows:
            e = r.get("embedding")
            if not isinstance(e, list) or len(e) != p.dim:
                raise WriterError("row %s: an embedding of dim %d values" % (r.get("hash"), p.dim))
            try:
                emb.append([f32(float(x)) for x in e])
            except (OverflowError, TypeError, ValueError):
                raise WriterError("row %s: an embedding value isn't an f32 number" % r.get("hash"))
    if emb is not None:
        for e in emb:
            if not all(math.isfinite(x) for x in e):
                raise WriterError("an embedding value isn't finite")

    # Rows in canonical-hash order (5.1).
    seen = set()
    for r in rows:
        h = r.get("hash", "")
        if (not isinstance(h, str) or len(h) != 32 or any(c not in "0123456789abcdef" for c in h)):
            raise WriterError("a hash isn't 32 lowercase hex digits: %r" % (h,))
        if h in seen:
            raise WriterError("the hash %s twice" % h)
        seen.add(h)
    perm = sorted(range(len(rows)), key=lambda i: bytes.fromhex(rows[i]["hash"]))
    p.hashes = [rows[i]["hash"] for i in perm]
    p.desc_rows = [rows[i] for i in perm]
    if np_vectors is not None:
        import numpy as np
        p.E32 = np.ascontiguousarray(np_vectors[perm], dtype=np.float32)
        if not np.isfinite(p.E32).all():
            raise WriterError("an embedding value isn't finite")
        p.emb = None
        p.vec_sha256 = hashlib.sha256(p.E32.astype("<f4").tobytes()).hexdigest()
    else:
        p.E32 = None
        p.emb = [emb[i] for i in perm]
        p.vec_sha256 = vec_digest(p.emb, p.dim)

    # Each row's card files, its primary file and record (5.3), its filter
    # record (4.4) and keys (2.4).
    p.filters, p.keys, p.primary, p.name_keys = [], [], [], []
    owner = {}
    files_by_hash = {}
    for i, r in enumerate(p.desc_rows):
        files = r.get("files") or []
        if not files:
            raise WriterError("row %s has no card file" % r["hash"])
        paths = [f["path"] for f in files]
        if len(set(paths)) != len(paths):
            raise WriterError("row %s lists a path twice" % r["hash"])
        for f in files:
            if not isinstance(f.get("path"), str) or not f["path"]:
                raise WriterError("row %s: a file without a path" % r["hash"])
            if f["path"] in owner:
                raise WriterError("the card file %r in two rows" % f["path"])
            owner[f["path"]] = i
            files_by_hash.setdefault(path_hash(f["path"]), []).append((f["path"], i))
        primary = min(files, key=lambda f: (path_hash(f["path"]), f["path"].encode("utf-8")))
        artists = primary.get("artists", [])
        title = primary.get("title")
        if not isinstance(artists, list) or not all(isinstance(a, str) for a in artists) or not (
                title is None or isinstance(title, str)):
            raise WriterError("row %s: artists is a list of strings, title a string" % r["hash"])
        bpm10 = int(primary.get("bpm10", 0))
        camelot = int(primary.get("camelot", 0))
        if not (0 <= bpm10 <= 0xFFFF and 0 <= camelot <= 255):
            raise WriterError("row %s: bpm10 or camelot out of range" % r["hash"])
        flags = FILTER_BPM_ANALYSED if primary.get("bpmAnalysed", False) else 0
        p.filters.append((bpm10, camelot if camelot <= 24 else 0, flags, artist_key(artists)))
        p.keys.append(song_key(artists, title))
        p.primary.append(primary["path"])
        p.name_keys.append(artist_name_key(artists))
        for field, got in (("primaryPath", p.primary[-1]), ("artistNameKey", p.name_keys[-1]),
                           ("songKey", p.keys[-1])):
            if field in r and r[field] != got:
                raise WriterError("row %s: %s is %r, the description gives %r" % (r["hash"], field, got, r[field]))
    # DJPH (4.5): every card file once, by path hash; a hash held by two or
    # more card files is left out.
    p.paths = sorted((h, fs[0][1]) for h, fs in files_by_hash.items() if len(fs) == 1)
    p.groups = same_groups(p.keys)
    p.sig_text = signature_text(p.strings[0], p.strings[1], p.strings[2], p.k, p.kind, p.head, p.floor, p.dim,
                                p.vec_sha256, p.hashes, p.groups)
    p.sig = signature(p.sig_text)
    return p


def compute_lists(p, fast=None, f32_sum=False, log=None):
    """The DJSL bytes: each row's entries by 5.3-5.4, in row order."""
    n, k, sb = len(p.hashes), p.k, p.slot_bytes
    if fast is None:
        fast = have_numpy() and (n >= 1000 or p.E32 is not None)
    if (fast or p.E32 is not None) and not have_numpy():
        raise WriterError("the fast path needs numpy")
    if fast or p.E32 is not None:           # vectors given as an array take the fast path
        import numpy as np
        E32 = p.E32 if p.E32 is not None else np.asarray(p.emb, dtype=np.float32)
        idx, lv, used, _ = lists_fast(E32, p.keys, p.kind, k, p.head, p.floor, f32_sum, log=log)
        slots = np.zeros((n, sb), dtype=np.uint8)
        slots[:, :2 * k] = idx.astype("<u2").view(np.uint8).reshape(n, 2 * k)
        slots[:, 2 * k:3 * k] = lv
        out = bytearray(slots.tobytes())
        for r in range(n):
            o = r * sb
            struct.pack_into("<I", out, o + sb - 4, slot_crc(memoryview(out)[o:o + sb], r))
        return bytes(out)
    lists = lists_rule(p.emb, p.keys, p.kind, k, p.head, p.floor, f32_sum)
    return b"".join(encode_slot(r, e, k, sb) for r, e in enumerate(lists))


# ---------------------------------------------------------------------------
# The container (3) and the file (4)
# ---------------------------------------------------------------------------
def pack_type_header(rows, paths, k, slot_bytes, kind, head, floor, built_time, tags_generation, sig, strings,
                     index_bytes=INDEX_BYTES, score_kind=SCORE_COSINE):
    return TYPE_HEADER.pack(rows, paths, k, slot_bytes, index_bytes, score_kind, kind, head, built_time,
                            tags_generation, sig, *strings, floor)


def strs_section(strings):
    """4.6: a NUL, then the non-empty strings in header-field order, each
    once with its NUL; returns (bytes, offsets), offset 0 for an empty one."""
    b, offs = bytearray(b"\0"), []
    for s in strings:
        if not s:
            offs.append(0)
            continue
        offs.append(len(b))
        b += s.encode("utf-8") + b"\0"
    return bytes(b), offs


def frame(generation, card_id, type_header, sections, header_bytes, major=MAJOR, minor=0, magic=MAGIC):
    """3.1-3.2: sections [(type, flags, count, stride, bytes)] in directory
    order; the first at headerBytes + 32 x count, each next at the previous
    end rounded up to 8 (zeros between); the header CRC last. The type
    header is written from byte 40 and zero-padded to headerBytes (cut
    there when it is longer: a hardening file's short header)."""
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
        DIR_ENTRY.pack_into(out, header_bytes + 32 * i, s[0], s[1], o, len(s[4]), s[2], s[3], crc32(s[4]), 0)
    COMMON.pack_into(out, 0, magic, major, minor, header_bytes, n, at, generation, card_id, 0, 0)
    th = bytes(type_header)[:max(0, header_bytes - 40)]
    out[40:40 + len(th)] = th
    struct.pack_into("<I", out, 32, crc32(out[:header_bytes + 32 * n]))
    return bytes(out)


def write(p, djsl, built_time=None):
    """The file around DJSL's bytes (4): DJSL, DJRF, DJPH, STRS, headerBytes
    by the alignment rule."""
    n = len(p.hashes)
    djrf = b"".join(struct.pack("<HBBI", *f) for f in p.filters)
    djph = b"".join(struct.pack("<QH", h, r) for h, r in p.paths)
    strs, offs = strs_section(p.strings)
    th = pack_type_header(n, len(p.paths), p.k, p.slot_bytes, p.kind, p.head, p.floor,
                          p.built_time if built_time is None else built_time, p.tags_generation, p.sig, offs)
    sections = [(DJSL, REQUIRED, n, p.slot_bytes, djsl), (DJRF, REQUIRED, n, FILTER_STRIDE, djrf),
                (DJPH, REQUIRED, len(p.paths), PATH_STRIDE, djph), (STRS, REQUIRED, 0, 0, strs)]
    return frame(p.generation, p.card_id, th, sections, header_bytes_for(4, p.slot_bytes), minor=p.minor)


def build(desc, vectors=None, sidecars=None, base=None, fast=None, f32_sum=False, log=None):
    """A file from a description: the lists by the rule, then the file; the
    writer's own check of every slot before it hands the bytes over."""
    p = prepare(desc, vectors, sidecars, base)
    data = write(p, compute_lists(p, fast, f32_sum, log))
    why = check(data, "all")
    if why != "ok":
        raise AssertionError("the writer's own file reads as absent: " + why)
    return data


def restamp(old, desc, vectors=None, sidecars=None, base=None):
    """7.3: the description's new file (generation, cardId, tagsGeneration,
    DJRF, DJPH, STRS from it) around the old file's DJSL bytes, unchanged,
    and its builtTime; refused when the signatures differ (the lists must
    be computed again)."""
    p = prepare(desc, vectors, sidecars, base)
    r = Reader(old)
    if r.why != "ok":
        raise WriterError("the old file is absent: " + r.why)
    if r.sig != p.sig:
        raise WriterError("the selection signatures differ: compute the lists again")
    s = r.sections[DJSL]
    return write(p, bytes(old[s.offset:s.offset + s.bytes]), built_time=r.built_time)


# ---------------------------------------------------------------------------
# The reader (8)
# ---------------------------------------------------------------------------
class Section:
    def __init__(self, index, type_, flags, offset, nbytes, count, stride, crc):
        self.index, self.type, self.flags, self.offset = index, type_, flags, offset
        self.bytes, self.count, self.stride, self.crc = nbytes, count, stride, crc


class Reader:
    """Opens a file (8.1, 8.2): `why` is "ok" or the first check it fails.
    check() goes on (8.3, 8.4), slot() reads one slot as the device does."""

    def __init__(self, data):
        self.b = bytes(data)
        self.sections = {}
        self.why = self._open()

    def _open(self):
        b, size = self.b, len(self.b)
        if size < 40:
            return "short"
        (magic, self.major, self.minor, self.header_bytes, self.section_count, self.file_bytes, self.generation,
         self.card_id, self.header_crc, _) = COMMON.unpack_from(b, 0)
        if magic != MAGIC:
            return "magic"
        if self.major != MAJOR:
            return "major"
        if self.header_bytes % 8 or self.header_bytes < MIN_HEADER_BYTES:
            return "headerBytes"
        if self.section_count > 64:
            return "sectionCount"
        dir_end = self.header_bytes + 32 * self.section_count
        if dir_end > size:
            return "short"
        if self.file_bytes != size:
            return "fileBytes"
        if crc32(b[:32] + bytes(4) + b[36:dir_end]) != self.header_crc:
            return "headerCrc"
        end, types = dir_end, []
        for i in range(self.section_count):
            t, flags, off, nb, count, stride, c, _ = DIR_ENTRY.unpack_from(b, self.header_bytes + 32 * i)
            if off % 8 or off < end or off - end >= 8 or off + nb > self.file_bytes or t in types:
                return "layout"
            types.append(t)
            end = off + nb
            if t not in KNOWN:
                if flags & REQUIRED:
                    return "unknownRequired"
                continue
            if KNOWN[t] == 0:
                if count or stride:
                    return "shape"
            elif stride < KNOWN[t] or count * stride != nb:
                return "shape"
            self.sections[t] = Section(i, t, flags, off, nb, count, stride, c)
        if end != self.file_bytes:
            return "layout"
        if any(t not in self.sections for t in ORDER):
            return "missing"
        # The header (8.2).
        (self.rows, self.path_count, self.k, self.slot_bytes, self.index_bytes, self.score_kind, self.kind,
         self.head, self.built_time, self.tags_generation, self.sig, *offs, self.floor) = TYPE_HEADER.unpack_from(b, 40)
        self.string_offsets = offs
        if not 1 <= self.rows <= MAX_ROWS:
            return "enum"
        if not MIN_K <= self.k <= MAX_K:
            return "enum"
        if self.slot_bytes != slot_bytes_for(self.k):
            return "enum"
        if self.index_bytes != INDEX_BYTES or self.score_kind != SCORE_COSINE:
            return "enum"
        if self.kind == SEL_TOP:
            if self.head != self.k or self.floor != 0:
                return "djSelection"
        elif self.kind == SEL_STRIDED:
            if not (1 <= self.head <= self.k - 2 and 1 <= self.floor <= 255):
                return "djSelection"
        elif self.head > self.k:      # a later kind: only selHead and selFloor are used
            return "djSelection"
        sl, rf, ph = self.sections[DJSL], self.sections[DJRF], self.sections[DJPH]
        if sl.count != self.rows or rf.count != self.rows or ph.count != self.path_count:
            return "counts"
        if sl.stride != self.slot_bytes:
            return "shape"
        pos = [self.sections[t].index for t in ORDER]
        if sl.index != 0 or sl.offset % self.slot_bytes or pos != sorted(pos):
            return "djLayout"
        return "ok"

    def section_bytes(self, t):
        s = self.sections[t]
        return self.b[s.offset:s.offset + s.bytes]

    def check(self, uses="all"):
        """8.6's order: STRS always; DJRF, DJPH and DJSL as `uses` says
        ("all": every one; "device": DJRF and DJPH, the join, its slots
        checked as they are read)."""
        if self.why != "ok":
            return self.why
        use = {"all": ("filters", "paths", "slots"), "device": ("filters", "paths")}.get(uses, uses)
        why = self._check_strings()
        if why == "ok" and "filters" in use:
            why = self._crc(DJRF)
        if why == "ok" and "paths" in use:
            why = self._check_paths()
        if why == "ok" and "slots" in use:
            for r in range(self.rows):
                read, why, _ = self.slot(r)
                if read != OK:
                    return why
            why = self._crc(DJSL)
        return why

    def _crc(self, t):
        return "ok" if crc32(self.section_bytes(t)) == self.sections[t].crc else "sectionCrc"

    def _check_strings(self):
        s = self.section_bytes(STRS)
        if not s or s[0] != 0:
            return "string"
        pos = 1
        for off in self.string_offsets:
            if off == 0:
                continue
            if off >= len(s):
                return "string"
            if off < pos:
                return "stringOrder"
            nul = s.find(b"\0", off)
            if nul < 0:
                return "string"
            pos = nul + 1
        return self._crc(STRS)

    def _check_paths(self):
        s = self.sections[DJPH]
        prev = None
        for i in range(s.count):
            h, r = struct.unpack_from("<QH", self.b, s.offset + i * s.stride)
            if r >= self.rows or (prev is not None and h <= prev):
                return "djPaths"
            prev = h
        return self._crc(DJPH)

    # Values (the caller has opened and checked what it uses).
    def strings(self):
        """The five strings; None for an offset outside STRS or with no NUL
        after it (a file that fails 8.3), bad UTF-8 replaced."""
        s, out = self.section_bytes(STRS), []
        for o in self.string_offsets:
            nul = s.find(b"\0", o) if 0 < o < len(s) else -1
            out.append("" if o == 0 else None if nul < 0 else s[o:nul].decode("utf-8", "replace"))
        return out

    def filter(self, r):
        """(bpm10, camelot, BPM_ANALYSED, artistKey): camelot above 24 reads as
        0; flag bits other than bit 0 are ignored."""
        s = self.sections[DJRF]
        bpm10, camelot, flags, ak = struct.unpack_from("<HBBI", self.b, s.offset + r * s.stride)
        return bpm10, camelot if camelot <= 24 else 0, bool(flags & FILTER_BPM_ANALYSED), ak

    def paths(self):
        s = self.sections[DJPH]
        return [struct.unpack_from("<QH", self.b, s.offset + i * s.stride) for i in range(s.count)]

    def row_of(self, h):
        for ph, r in self.paths():
            if ph == h:
                return r
        return None

    def slot_raw(self, r):
        s = self.sections[DJSL]
        return self.b[s.offset + r * self.slot_bytes:s.offset + (r + 1) * self.slot_bytes]

    def slot(self, r):
        """One slot as the device reads it: (read, why, entries)."""
        return check_slot(self.slot_raw(r), self.k, r, self.rows)


def read(data):
    """A file opened (8.1-8.2): a Reader, its `why` "ok" or the reason."""
    return Reader(data)


def check(data, uses="all"):
    """8.6: "ok", or the first check the file fails (it is then absent)."""
    return Reader(data).check(uses)


def same_content(a, b):
    """Two files read the same (13.6): every row's filter record (its 8
    known bytes, as read) and used slot entries (the c of 4.3; not the
    unused entries' bytes), every DJPH hash's row, the five strings.
    Returns None, or what differs."""
    ra, rb = Reader(a), Reader(b)
    if ra.why != "ok" or rb.why != "ok":
        return "not both present"
    if ra.rows != rb.rows:
        return "rowCount"
    for r in range(ra.rows):
        if ra.filter(r) != rb.filter(r):
            return "filter of row %d" % r
        if ra.slot(r) != rb.slot(r):
            return "slot %d" % r
    if ra.paths() != rb.paths():
        return "DJPH"
    if ra.strings() != rb.strings():
        return "strings"
    return None


# ---------------------------------------------------------------------------
# Printing
# ---------------------------------------------------------------------------
def hexdump(b):
    """As `hexdump -C`: a line equal to the one before is folded into one '*';
    the length last."""
    out, prev, star = [], None, False
    for o in range(0, len(b), 16):
        line = bytes(b[o:o + 16])
        if line == prev:
            if not star:
                out.append("*")
                star = True
            continue
        star, prev = False, line
        hx = ["%02x" % x for x in line] + ["  "] * (16 - len(line))
        text = "".join(chr(x) if 0x20 <= x < 0x7F else "." for x in line)
        out.append("%08x  %s  %s  |%s|" % (o, " ".join(hx[:8]), " ".join(hx[8:]), text))
    out.append("%08x" % len(b))
    return "\n".join(out)


def dump(data, slot=None, hex_=False, out=None):
    r = Reader(data)
    w = (out or sys.stdout).write
    if r.why in ("short", "magic"):
        w("absent: %s\n" % r.why)
        return
    w("MPDJ %d.%d headerBytes %d sections %d fileBytes %d generation %d cardId 0x%016X headerCrc 0x%08X\n" % (
        r.major, r.minor, r.header_bytes, r.section_count, r.file_bytes, r.generation, r.card_id, r.header_crc))
    if r.why != "ok":
        w("absent: %s\n" % r.why)
        if hex_:
            w(hexdump(data) + "\n")
        return
    w("rows %d paths %d k %d slotBytes %d selKind %d selHead %d selFloor %d builtTime %d tagsGeneration %d\n" % (
        r.rows, r.path_count, r.k, r.slot_bytes, r.kind, r.head, r.floor, r.built_time, r.tags_generation))
    w("selectionSig %s\n" % r.sig.hex())
    for t in ORDER:
        s = r.sections[t]
        w("%s entry %d offset %d bytes %d count %d stride %d crc 0x%08X\n" % (
            t.decode(), s.index, s.offset, s.bytes, s.count, s.stride, s.crc))
    for name, value in zip(STRING_FIELDS, r.strings()):
        w("%s %s\n" % (name, "(not a string: no NUL inside STRS)" if value is None
                       else json.dumps(value, ensure_ascii=False)))
    w("check (all): %s\n" % r.check("all"))
    rows = range(r.rows) if slot is None else [slot]
    if slot is None:
        for i in rows:
            bpm10, cam, analysed, ak = r.filter(i)
            w("filter %d bpm10 %d camelot %d analysed %d artistKey 0x%08X\n" % (i, bpm10, cam, analysed, ak))
        for h, row in r.paths():
            w("path 0x%016X row %d\n" % (h, row))
    for i in rows:
        read, why, ent = r.slot(i)
        raw = r.slot_raw(i)
        crc = struct.unpack_from("<I", raw, len(raw) - 4)[0]
        if read == OK:
            w("slot %d crc 0x%08X: %s\n" % (i, crc, " ".join("%d/%d" % e for e in ent) or "(none)"))
        else:
            w("slot %d crc 0x%08X: %s (%s)\n" % (i, crc, read, why))
        if slot is not None and hex_:
            w(hexdump(raw) + "\n")
    if hex_ and slot is None:
        w(hexdump(data) + "\n")


# ---------------------------------------------------------------------------
# The test vectors (13): descriptions, goldens, hardening files, vectors.json
# ---------------------------------------------------------------------------
CARD_ID = "0x5EEDC0DE0A1B2C3D"
NFC_CAFE = "Café"
NFD_CAFE = "Café"
assert unicodedata.normalize("NFC", NFD_CAFE) == NFC_CAFE
FIXTURE_STRINGS = {"modelId": "synthetic-embedding", "modelVersion": "1", "metric": "cosine",
                   "license": "CC0-1.0", "attribution": "synthetic test vectors"}
_ABSENT = object()


def _file(path, artists, title=_ABSENT, bpm10=0, camelot=0, analysed=False):
    f = {"path": path, "artists": list(artists)}
    if title is not _ABSENT:
        f["title"] = title
    if bpm10:
        f["bpm10"] = bpm10
    if camelot:
        f["camelot"] = camelot
    if analysed:
        f["bpmAnalysed"] = True
    return f


def _header(file, generation, k, kind, head=None, floor=None, dim=None, note=""):
    d = {"format": "MPDJ", "file": file, "generation": generation, "cardId": CARD_ID, "builtTime": 1790000000,
         "tagsGeneration": generation, "k": k, "selKind": kind}
    if kind != SEL_TOP:
        d["selHead"], d["selFloor"] = head, floor
    d.update(FIXTURE_STRINGS)
    d["dim"] = dim
    d["note"] = note
    return d


def _given(row, path, name_key_, song_key_):
    row["primaryPath"], row["artistNameKey"], row["songKey"] = path, name_key_, song_key_
    return row


# G1's components: integers / 16, each vector of length exactly 1, so every
# cosine is a multiple of 1/256.
G1_INT = [[2, 2, 6, 3, 9, 0, 11, 1], [-2, 12, 3, 2, 9, -1, -2, -3], [6, 0, 3, 1, 7, 6, 10, -5],
          [9, -1, 5, 9, -1, 7, 3, 3], [7, 1, 12, 4, 0, -1, 6, -3], [4, -3, 9, 2, 0, 4, 11, 3]]
G1_DOTS = {(0, 1): 100, (0, 2): 201, (0, 3): 100, (0, 4): 163, (0, 5): 186, (1, 2): 51, (1, 3): -28, (1, 4): 40,
           (1, 5): -48, (2, 3): 128, (2, 4): 151, (2, 5): 172, (3, 4): 160, (3, 5): 172, (4, 5): 194}
SONG_03 = "generated artist 03|sample title 03"


def g1_rows():
    e = [[x / 16 for x in v] for v in G1_INT]
    cafe = "Generated Artist 02/Sample Album 02/01 - Sample %s.flac"
    return [
        _given({"hash": "0123456789abcdef0123456789abcdef",
                "files": [_file("Generated Artist 01/Sample Album 01/01 - Sample Title 01.mp3", ["Generated Artist 01"],
                                "Sample Title 01", 1210, 8)],
                "embedding": e[0],
                "note": "m 5 > k: strided ranks 0, 1, 2, 4; rows 1 and 3 tie at level 100 = Lf, and rank 4, the "
                        "last at or above Lf, is row 3"},
               "Generated Artist 01/Sample Album 01/01 - Sample Title 01.mp3", "generated artist 01",
               "generated artist 01|sample title 01"),
        _given({"hash": "1c3e5a7092b4d6f81c3e5a7092b4d6f8",
                "files": [_file(cafe % NFC_CAFE, [], "Sample Café"),
                          _file(cafe % NFD_CAFE, [], "Sample Café", 1000)],
                "embedding": e[1],
                "note": "one recording at two paths (NFC and NFD names: two DJPH entries, one row); the primary "
                        "file is the smaller path hash's, the NFC one (bpm10 0); no artist: artistKey 0"},
               cafe % NFC_CAFE, "", "|sample café"),
        _given({"hash": "5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a",
                "files": [_file("Generated Artist 04/Sample Album 04/03 - Sample Title 04.mp3",
                                ["Generated Artist 04", "Generated Artist 05", "generated  artist 04"],
                                "Sample Title 04", 1280, 13, True)],
                "embedding": e[2],
                "note": "three artist values: display 'Generated Artist 04, Generated Artist 05'; cosine to row 3 "
                        "exactly 0.5 (127.5: level 128); m 4 = k: the top k"},
               "Generated Artist 04/Sample Album 04/03 - Sample Title 04.mp3", "generated artist 04",
               "generated artist 04, generated artist 05|sample title 04"),
        _given({"hash": "9e1f00000000000000000000000000a1",
                "files": [_file("Generated Artist 03/Sample Album 03/01 - Sample Title 03.mp3", ["Generated Artist 03"],
                                "Sample Title 03", 954, 21, True)],
                "embedding": e[3],
                "note": "rows 3, 4 and 5 are one song: none lists another, so each list has an unused entry"},
               "Generated Artist 03/Sample Album 03/01 - Sample Title 03.mp3", "generated artist 03", SONG_03),
        _given({"hash": "b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0",
                "files": [_file("Generated Artist 03/Sample Album 06/07 - SAMPLE TITLE 03.flac",
                                ["GENERATED ARTIST 03"], "SAMPLE TITLE 03")],
                "embedding": e[4], "note": "the same song in capitals"},
               "Generated Artist 03/Sample Album 06/07 - SAMPLE TITLE 03.flac", "generated artist 03", SONG_03),
        _given({"hash": "fedcba9876543210fedcba9876543210",
                "files": [_file("Generated Artist 03/Sample Album 07/12 - Sample Title 03.opus",
                                ["Generated Artist 03"], " Sample Title 03", 960, 21)],
                "embedding": e[5],
                "note": "the same song again: a no-break space between the artist's words, a leading space in "
                        "the title"},
               "Generated Artist 03/Sample Album 07/12 - Sample Title 03.opus", "generated artist 03", SONG_03),
    ]


G1_NOTE = ("The hand-checkable file (AUTODJ-INDEX.md 13.4), the manifest fixture's AutoDJ companion: components "
           "are multiples of 1/16 with |v| = 1 exactly, so every cosine is a multiple of 1/256. k 4, strided "
           "with H 1 and Lf 100. artistNameKey, songKey and primaryPath are given for checking.")

# 9.3's expected lists, typed from the plan and checked against the rule.
G1_LISTS = [[(2, 200), (5, 185), (4, 162), (3, 100)], [(0, 100), (2, 51), (4, 40), (3, 0)],
            [(0, 200), (5, 171), (4, 150), (3, 128)], [(2, 128), (0, 100), (1, 0)], [(0, 162), (2, 150), (1, 40)],
            [(0, 185), (2, 171), (1, 0)]]
G2_LISTS = [[(2, 200), (5, 185), (4, 162), (1, 100)]] + G1_LISTS[1:]
G3_LISTS = [[(2, 200), (5, 185), (4, 162), (1, 100), (3, 100)], [(0, 100), (2, 51), (4, 40), (3, 0), (5, 0)],
            [(0, 200), (5, 171), (4, 150), (3, 128), (1, 51)]] + G1_LISTS[3:]


def g1():
    d = _header("autodj-00000029.bin", 41, 4, SEL_STRIDED, 1, 100, 8, G1_NOTE)
    d["rows"] = g1_rows()
    return d


def g2():
    d = _header("autodj-small-top.bin", 41, 4, SEL_TOP, dim=8,
                note="autodj-00000029's rows as a plain top-k table (selKind 0: selHead k, selFloor 0): row 0 "
                     "keeps row 1, the lower index of the two at level 100.")
    d["rows"] = g1_rows()
    return d


def g3():
    d = _header("autodj-small-default.bin", 41, 169, SEL_STRIDED, 84, 140, 8,
                "autodj-00000029's rows at the writer's defaults (k 169, H 84, Lf 140): 512-byte slots and their "
                "padding byte at 507; every row has m <= k, so every list is the top k, down to level 0.")
    d["rows"] = g1_rows()
    return d


# G4's titles are one song only under the full lowercase with Final_Sigma
# (2.4): U+0130 lowercases to "i" + U+0307 (a per-character mapping gives
# "i"), and a capital sigma at a word's end to "ς" (a per-character one
# gives "σ"). A writer whose lowercase is per character makes two songs of
# them, and so lists that aren't empty.
G4_TITLES = ("Sample Ti̇tle 31 ΤΙΤΛΟΣ",     # "Sample Ti̇tle 31 ΤΙΤΛΟΣ"
             "SAMPLE TİTLE 31 τιτλος")       # "SAMPLE TİTLE 31 τιτλος"
G4_SONG = "generated artist 31|sample ti̇tle 31 τιτλος"


def g4():
    d = _header("autodj-tiny.bin", 7, 2, SEL_TOP, dim=2,
                note="Two rows of one song: every list is empty (c 0). The titles are one song only under the "
                     "full lowercase with Final_Sigma (AUTODJ-INDEX.md 2.4): row cc's has 'i' + U+0307 where row "
                     "dd's has U+0130, and a capital sigma ending a word where row dd's has the final sigma. The "
                     "embedding values are f32(0.6) and f32(0.8), written as their shortest decimals; the second "
                     "row's two files share one record.")
    d["rows"] = [
        _given({"hash": "cc" + "00" * 15,
                "files": [_file("Generated Artist 31/01 - Sample Title 31.mp3", ["Generated Artist 31"],
                                G4_TITLES[0])],
                "embedding": [f32(0.6), f32(0.8)]},
               "Generated Artist 31/01 - Sample Title 31.mp3", "generated artist 31", G4_SONG),
        _given({"hash": "dd" + "00" * 15,
                "files": [_file("Generated Artist 31/Live/01 - Sample Title 31.mp3", ["generated artist 31"],
                                G4_TITLES[1]),
                          _file("Generated Artist 31/Copy/01 - Sample Title 31.mp3", ["generated artist 31"],
                                G4_TITLES[1])],
                "embedding": [1.0, 0.0]},
               None, "generated artist 31", G4_SONG),
    ]
    if (name_key(G4_TITLES[0]) != name_key(G4_TITLES[1]) or G4_SONG.split("|")[1] != name_key(G4_TITLES[0])
            or "".join(map(str.lower, G4_TITLES[0])) == G4_TITLES[0].lower() or "İ".lower() != "i̇"):
        raise AssertionError("G4's titles: one song only by the full lowercase with Final_Sigma")
    d["rows"][1]["primaryPath"] = min(
        (f["path"] for f in d["rows"][1]["files"]), key=path_hash)
    return d


def g5():
    d = _header("autodj-pair.bin", 7, 3, SEL_TOP, dim=2,
                note="Two title-less rows of one artist: neither has a song key, so each lists the other (a "
                     "title taken from the file name would have made them one song). Each list: one entry, at "
                     "level 0 (cosine -1).")
    d["rows"] = [
        _given({"hash": "aa" + "00" * 15,
                "files": [_file("Generated Artist 32/01 - Sample Title 32.mp3", ["Generated Artist 32"], "   ")],
                "embedding": [1.0, 0.0], "note": "a title of three spaces: kept by METADATA 2.3.6, blank here"},
               "Generated Artist 32/01 - Sample Title 32.mp3", "generated artist 32", None),
        _given({"hash": "bb" + "00" * 15,
                "files": [_file("Generated Artist 32/02 - Sample Title 32.mp3", ["Generated Artist 32"])],
                "embedding": [-1.0, 0.0], "note": "no title field"},
               "Generated Artist 32/02 - Sample Title 32.mp3", "generated artist 32", None),
    ]
    return d


# --- G6 / G7: the generated mid-size vector -------------------------------
MID_SEED = 20261013
MID_N, MID_DIM = 200, 16
MID_OUTLIERS = 14             # g 186-199: a group of their own, away from the cone
MID_SCALE = 1 << 9            # integer components below 2^23: fine steps for the trap


def _unit_f32(v):
    """An integer vector, normalised in f64 (the sum of squares exact, the
    square root and the division correctly rounded), each value rounded to
    f32: the same bytes on any IEEE machine and any Python 3."""
    s = sum(x * x for x in v)
    if not 0 < s < 1 << 53:
        raise ValueError("the sum of squares must be exact in f64")
    norm = math.sqrt(float(s))
    return [f32(x / norm) for x in v]


def _mid_names(g):
    """g: the generation index. Names follow g (artist g // 10 + 1)."""
    return "Generated Artist %02d" % (g // 10 + 1), "Sample Album %02d" % (g // 5 + 1), "Sample Title %03d" % g


@functools.lru_cache(maxsize=None)
def tv_mid():
    """autodj-mid's rows (13.5): a description (without the header) and its
    vectors, from random.Random(20261013): integer components from
    getrandbits, so the bytes are the same on any Python. A main cone (most
    pairs at cos >= 0.55, so most rows have m > k at k 24 and at k 169), an
    outlier group (low m), planted near-duplicates for the song-key rules,
    and the float32 trap: a pair at level 140 = Lf whose float32 level is
    139, found by moving one component of one row a step at a time. Returns
    (rows, ints, emb, keys, facts), in generation order: the rows' files,
    the integer vectors, their f32 values, the song keys, and the trap's
    pair."""
    rng = random.Random(MID_SEED)

    def bits(n):
        return rng.getrandbits(n)

    centre = [700 + bits(10) for _ in range(MID_DIM)]                       # positive: the cone's axis
    other = [bits(12) - 2048 for _ in range(MID_DIM)]                       # the outliers' axis
    ints = []
    for g in range(MID_N):
        if g < MID_N - MID_OUTLIERS:
            v = [4 * c + 3 * (bits(12) - 2048) for c in centre]
        else:
            v = [2 * o + bits(11) - 1024 for o in other]
        ints.append([x * MID_SCALE for x in v])
    # Near-duplicates: (e) 151 copies 150; (f) 161 and 162 copy 160.
    for dup, src in ((151, 150), (161, 160), (162, 160)):
        ints[dup] = [x + (bits(8) - 128) * 64 for x in ints[src]]
    hashes = set()
    rows = []
    for g in range(MID_N):
        while True:
            h = "%032x" % bits(128)
            if h not in hashes:
                break
        hashes.add(h)
        artist, album, title = _mid_names(g)
        artists = [artist]
        if g % 37 == 5:
            artists = []                                                   # (h) no artist
        if g == 7:
            artists = [artist, "Generated Artist 21", artist.lower()]       # (h) three values, one repeated
        bpm10 = 0 if g % 13 == 4 else 700 + (g * 37) % 1100
        camelot = 25 if g == 11 else g % 25                                # 25: a record's out-of-range key
        f = _file("%s/%s/%02d - %s.mp3" % (artist, album, g % 5 + 1, title), artists, title, bpm10, camelot,
                  g % 3 == 0)
        files = [f]
        if g == 150:                                                       # (e) title-less, same artist as 151
            del f["title"]
        if g == 151:
            f["title"] = _mid_names(150)[2]
            f["path"] = "%s/%s/%02d - %s.flac" % (artist, album, 2, _mid_names(150)[2])
        if g in (160, 161, 162):                                           # (f) one song, three spellings
            f["artists"] = [["Generated Artist 17"], ["GENERATED ARTIST 17"], ["Generated  Artist 17 "]][g - 160]
            f["title"] = ["Sample Title 160", "SAMPLE TITLE 160", "Sample  Title 160"][g - 160]
        if g == 170:                                                       # (g) one recording at two paths
            files = [_file("%s/%s/01 - Sample %s %03d.mp3" % (artist, album, NFC_CAFE, g), artists, title, 1000, 3),
                     _file("%s/%s/01 - Sample %s %03d.mp3" % (artist, album, NFD_CAFE, g), artists, title, 0, 0)]
        rows.append({"hash": h, "files": files})
    keys = [song_key(r["files"][0]["artists"], r["files"][0].get("title")) for r in rows]

    # The trap (d): row A, the cone row with the most candidates at or above
    # Lf, and row B, the cone row whose level to A is nearest 170, moved
    # away from A (B - t A / 2^16, t found by halving) until cos(A, B) x 255
    # is just above 139.5, then one component a step at a time until the
    # float32 sum falls below it.
    emb = [_unit_f32(v) for v in ints]
    lv = level_matrix(emb)
    plain = [g for g in range(MID_N - MID_OUTLIERS) if keys[g] is not None and g not in (150, 151, 160, 161, 162, 170)]

    def m_of(a):
        return sum(1 for _, x in order_candidates(lv[a], a, keys) if x >= DEFAULT_FLOOR)

    a_g = max(plain, key=lambda g: (m_of(g), -g))
    A = ints[a_g]

    def x255(v):
        return cos_rule(emb[a_g], _unit_f32(v)) * 255.0

    def search(base):
        """B - t A / 2^16 with x255 just above 139.5 (t by halving), then each
        component in turn a step at a time (about 1e-5 of a level a step)
        across 139.5: the first point whose float32 level is 139."""
        def moved(t):
            return [b - ((t * a) >> 16) for a, b in zip(A, base)]

        target = 139.5 + 1e-4
        lo, hi = 0, 1 << 16                # B - A: past orthogonal
        if not x255(moved(lo)) > target > x255(moved(hi)):
            return None
        while hi - lo > 1:
            mid = (lo + hi) // 2
            if x255(moved(mid)) > target:
                lo = mid
            else:
                hi = mid
        for j in sorted(range(MID_DIM), key=lambda i: (-A[i], i)):
            v = moved(lo)
            for _ in range(2000):
                v[j] -= 1
                e = _unit_f32(v)
                x = cos_rule(emb[a_g], e) * 255.0
                if x < 139.5 or x > target + 0.01:
                    break
                if level(x / 255.0) == DEFAULT_FLOOR and level_f32(emb[a_g], e) == DEFAULT_FLOOR - 1:
                    return v
        return None

    found = None
    for trap_g in sorted((g for g in plain if g != a_g), key=lambda g: (abs(lv[a_g][g] - 170), g)):
        found = search(list(ints[trap_g]))
        if found:
            break
    if found is None:
        raise AssertionError("no float32 trap found")
    ints[trap_g] = found
    emb[trap_g] = _unit_f32(found)

    # No other pair may have a float32 level unlike the rule's, so a float32
    # build differs from the rule's only through the trap: a row of any other
    # such pair (not A or B) moves its first component by one, until none is
    # left.
    lv = level_matrix(emb)
    lv32 = level_matrix(emb, f32_sum=True)
    for _ in range(1000):
        bad = next(((a, b) for a in range(MID_N) for b in range(a + 1, MID_N)
                    if lv[a][b] != lv32[a][b] and {a, b} != {a_g, trap_g}), None)
        if bad is None:
            break
        g = next(x for x in (bad[1], bad[0]) if x not in (a_g, trap_g))
        ints[g][0] += 1
        emb[g] = _unit_f32(ints[g])
        for b in range(MID_N):
            if b != g:
                lv[g][b] = lv[b][g] = level_of(emb[g], emb[b])
                lv32[g][b] = lv32[b][g] = level_f32(emb[g], emb[b])
    else:
        raise AssertionError("float32 differences left")
    facts = {"trapG": (a_g, trap_g)}
    return rows, ints, emb, keys, facts


def _mid_vectors_bytes():
    rows, ints, emb, keys, facts = tv_mid()
    return b"".join(struct.pack("<%df" % MID_DIM, *e) for e in emb)


def _mid_rows_described():
    """The rows with their given keys, in generation order."""
    rows, ints, emb, keys, facts = tv_mid()
    out = []
    for r, key in zip(rows, keys):
        primary = min(r["files"], key=lambda f: (path_hash(f["path"]), f["path"].encode("utf-8")))
        out.append(_given(json.loads(json.dumps(r)), primary["path"], artist_name_key(primary["artists"]),
                          song_key(primary["artists"], primary.get("title"))))
    return out


@functools.lru_cache(maxsize=None)
def _mid_facts(k, head):
    """What (a)-(h) say about one build of the mid rows, by row index (rule
    and float32 lists both computed)."""
    rows, ints, emb, keys, facts = tv_mid()
    perm = sorted(range(MID_N), key=lambda g: bytes.fromhex(rows[g]["hash"]))
    row_of = {g: i for i, g in enumerate(perm)}
    E = [emb[g] for g in perm]
    K = [keys[g] for g in perm]
    lv = level_matrix(E)
    lv32 = level_matrix(E, f32_sum=True)
    cand = [order_candidates(lv[a], a, K) for a in range(MID_N)]
    rule = [select(c, SEL_STRIDED, k, head, DEFAULT_FLOOR) for c in cand]
    wrong = [select(order_candidates(lv32[a], a, K), SEL_STRIDED, k, head, DEFAULT_FLOOR) for a in range(MID_N)]
    m = [sum(1 for _, x in c if x >= DEFAULT_FLOOR) for c in cand]
    f = {}
    big = [r for r in range(MID_N) if m[r] > k]
    f["a"] = [r for r in big if (m[r] - 1 - head) % (k - 1 - head)]
    f["b"] = [r for r in range(MID_N) if m[r] <= k and any(x < DEFAULT_FLOOR for _, x in rule[r])]
    f["c"] = [r for r in big if sum(1 for _, x in cand[r] if x == DEFAULT_FLOOR) >= 2
              and rule[r][k - 1] == max((b, x) for b, x in cand[r] if x == DEFAULT_FLOOR)]
    a_g, b_g = facts["trapG"]
    ra, rb = row_of[a_g], row_of[b_g]
    rank_b = [b for b, _ in cand[ra]].index(rb)
    f["d"] = {"rowA": ra, "rowB": rb, "rankB": rank_b, "level": lv[ra][rb], "levelF32": lv32[ra][rb],
              "slotsDiffer": [r for r in range(MID_N) if rule[r] != wrong[r]]}
    e1, e2 = row_of[150], row_of[151]
    f["e"] = (e1, e2, e2 in [b for b, _ in rule[e1]] and e1 in [b for b, _ in rule[e2]])
    grp = [row_of[g] for g in (160, 161, 162)]
    f["f"] = (grp, not any(b in grp for r in grp for b, _ in rule[r]))
    f["g"] = row_of[170]
    f["h"] = ([row_of[g] for g in range(MID_N) if not rows[g]["files"][0]["artists"]], row_of[7])
    f["m"] = m
    f["rule"], f["wrong"] = rule, wrong
    return f


def _mid_assert(k, head):
    """(a)-(h) of 13.5, asserted for one build."""
    f = _mid_facts(k, head)
    d = f["d"]
    if not f["a"]:
        raise AssertionError("mid k %d: (a) no row with m > k and a stride remainder" % k)
    if not f["b"]:
        raise AssertionError("mid k %d: (b) no row with m <= k listing below Lf" % k)
    if not f["c"]:
        raise AssertionError("mid k %d: (c) no row whose last entry a tie at Lf decides" % k)
    if not (d["rankB"] >= head and d["level"] == DEFAULT_FLOOR and d["levelF32"] == DEFAULT_FLOOR - 1
            and d["slotsDiffer"] and set(d["slotsDiffer"]) <= {d["rowA"], d["rowB"]}):
        raise AssertionError("mid k %d: (d) the float32 trap: %r" % (k, d))
    if not f["e"][2]:
        raise AssertionError("mid k %d: (e) the title-less pair doesn't list each other" % k)
    if not f["f"][1]:
        raise AssertionError("mid k %d: (f) the same-song group lists itself" % k)
    return f


def _mid_note(k, head, f):
    d = f["d"]
    return ("Generated by tools/mpdj.py tv_mid() (random.Random(%d), stdlib): %d rows, dim %d, a main cone and %d "
            "outliers; k %d, strided (H %d, Lf 140). Row indexes below are row order. (a) m > k with a stride "
            "remainder: rows %s; (b) m <= k with entries below Lf: rows %s; (c) the last entry decided by a tie at "
            "Lf: rows %s; (d) the float32 trap: rows %d and %d, rank %d, level 140 by the rule and 139 by a float32 "
            "sum, so a float32 writer gets the slots of rows %s wrong (vectors.json mpdj.trap); (e) row %d has no "
            "title and row %d, by the same artist, has the title %d's file name carries: each lists the other; "
            "(f) rows %s are one song (case, extra spaces) and none lists another; (g) row %d has two paths (NFC, "
            "NFD); (h) rows %s have no artist, row %d three values; a record's camelot of 25 is written as 0."
            % (MID_SEED, MID_N, MID_DIM, MID_OUTLIERS, k, head, _few(f["a"]), _few(f["b"]), _few(f["c"]),
               d["rowA"], d["rowB"], d["rankB"], _few(d["slotsDiffer"]), f["e"][0], f["e"][1], f["e"][0],
               _few(f["f"][0]), f["g"], _few(f["h"][0]), f["h"][1]))


def _few(rows, n=6):
    rows = list(rows)
    if len(rows) > n:
        return ", ".join(map(str, rows[:n])) + " and %d more" % (len(rows) - n)
    return ", ".join(map(str, rows[:-1])) + " and %d" % rows[-1] if len(rows) > 1 else str(rows[0])


def g_mid(default=False):
    k, head = (DEFAULT_K, DEFAULT_HEAD) if default else (24, 8)
    f = _mid_assert(k, head)
    d = _header("autodj-mid-default.bin" if default else "autodj-mid.bin", 9, k, SEL_STRIDED, head, DEFAULT_FLOOR,
                MID_DIM, _mid_note(k, head, f))
    d["vectors"] = "autodj-mid.f32"
    d["rows"] = _mid_rows_described()
    return d


def fixture_libraries():
    """Every MPDJ description of test/fixtures/card/libraries, in order."""
    return [g1(), g2(), g3(), g4(), g5(), g_mid(), g_mid(default=True)]


def fixture_sidecars():
    """The vectors files the descriptions name: {name: bytes}."""
    return {"autodj-mid.f32": _mid_vectors_bytes()}


EXPECTED_LISTS = {"autodj-00000029.bin": G1_LISTS, "autodj-small-top.bin": G2_LISTS,
                  "autodj-small-default.bin": G3_LISTS, "autodj-tiny.bin": [[], []],
                  "autodj-pair.bin": [[(1, 0)], [(0, 0)]]}


def fixture_goldens():
    """{file: bytes} for every description, each checked: its lists against
    the rule as written (and 13.4's typed lists where there are), its
    keys, a read back."""
    side = fixture_sidecars()
    out = {}
    for d in fixture_libraries():
        data = build(d, sidecars=side, fast=False)
        p = prepare(d, sidecars=side)
        want = lists_rule(p.emb, p.keys, p.kind, p.k, p.head, p.floor)
        r = Reader(data)
        got = [r.slot(i)[2] for i in range(r.rows)]
        if got != want:
            raise AssertionError("%s: the lists read back aren't the rule's" % d["file"])
        if d["file"] in EXPECTED_LISTS and got != EXPECTED_LISTS[d["file"]]:
            raise AssertionError("%s: the lists aren't 13.4's: %r" % (d["file"], got))
        out[d["file"]] = data
    _check_g1(out["autodj-00000029.bin"])
    return out


def _check_g1(data):
    """13.4's typed facts about autodj-00000029.bin."""
    e = [[x / 16 for x in v] for v in G1_INT]
    for (a, b), dot in G1_DOTS.items():
        if cos_rule(e[a], e[b]) * 256 != dot:
            raise AssertionError("G1 dot %d-%d" % (a, b))
    r = Reader(data)
    if (r.header_bytes, r.slot_bytes, len(data), r.path_count) != (128, 256, 1973, 7):
        raise AssertionError("G1 geometry: %r" % ((r.header_bytes, r.slot_bytes, len(data), r.path_count),))
    if r.filter(1)[0] != 0 or r.filter(2)[1:] != (13, True, artist_key(["Generated Artist 04"])):
        raise AssertionError("G1 filters")


# --- Hardening (13.6) -------------------------------------------------------
class Parts:
    """A file taken apart for editing: the frame's fields, the type header
    (bytes 40 to headerBytes) and the sections in directory order."""

    def __init__(self, data):
        (_, self.major, self.minor, self.header_bytes, n, _, self.generation, self.card_id, _,
         _) = COMMON.unpack_from(data, 0)
        self.head = bytearray(data[40:self.header_bytes])
        self.sections = []
        for i in range(n):
            t, flags, off, nb, count, stride, _, _ = DIR_ENTRY.unpack_from(data, self.header_bytes + 32 * i)
            self.sections.append([t, flags, count, stride, bytearray(data[off:off + nb])])

    def section(self, t):
        return next(s for s in self.sections if s[0] == t)

    def put(self, fmt, off, value):
        """A type-header field at its file offset."""
        struct.pack_into(fmt, self.head, off - 40, value)

    def slot(self, r):
        sb = struct.unpack_from("<H", self.head, 10)[0]
        return self.section(DJSL)[4], r * sb, sb

    def reseal_slot(self, r):
        data, o, sb = self.slot(r)
        struct.pack_into("<I", data, o + sb - 4, slot_crc(data[o:o + sb], r))

    def build(self, header_bytes=None, realign=False):
        """The file again, every CRC made right. `realign`: headerBytes by
        the alignment rule for this many sections (a fixture that adds,
        drops or reorders them); else as it was."""
        hb = self.header_bytes if header_bytes is None else header_bytes
        if realign:
            sb = struct.unpack_from("<H", self.head, 10)[0]
            hb = header_bytes_for(len(self.sections), sb, max(MIN_HEADER_BYTES, 40 + len(self.head.rstrip(b"\0"))))
        return frame(self.generation, self.card_id, bytes(self.head), [tuple(s) for s in self.sections], hb,
                     self.major, self.minor)


def fixture_hardening(golden):
    """13.6's files: [(name, format, uses, expect, why, check, bytes, same,
    slot)], each made from autodj-00000029.bin (or -small-default) by
    breaking one rule, every other CRC made right; each one's check() and,
    for a present file, its content against `same`, asserted here."""
    g1b = golden["autodj-00000029.bin"]
    out = []

    def add(name, why, text, data, uses="all", same=None, slot=None):
        expect = "present" if why == "ok" else "absent"
        got = check(data, uses)
        if got != why:
            raise AssertionError("%s: reads as %s, not %s" % (name, got, why))
        if same is not None:
            diff = same_content(data, golden[same])
            if diff:
                raise AssertionError("%s: not the same as %s (%s)" % (name, same, diff))
        if slot is not None:
            r = Reader(data)
            if r.why != "ok" or r.check("device") not in ("ok",) or r.slot(slot[0])[0] != slot[1]:
                raise AssertionError("%s: slot %d doesn't read %s" % (name, slot[0], slot[1]))
        out.append((name, "MPDJ", uses, expect, why, text, data, same,
                    {"row": slot[0], "read": slot[1]} if slot else None))

    def edit(fn, src=g1b, realign=False, header_bytes=None):
        p = Parts(src)
        fn(p)
        return p.build(header_bytes, realign)

    def field(fmt, off, value):
        return lambda p: p.put(fmt, off, value)

    def frame_field(off, fmt, value):
        b = bytearray(g1b)
        struct.pack_into(fmt, b, off, value)
        struct.pack_into("<I", b, 32, 0)
        hb, n = struct.unpack_from("<II", b, 8)
        struct.pack_into("<I", b, 32, crc32(b[:hb + 32 * n]))
        return bytes(b)

    # The frame (8.1).
    add("mpdj-major-1", "major", "major 1: the retired v1 draft, an older major (unknownMajor false)",
        frame_field(4, "<H", 1))
    add("mpdj-major-3", "major", "major 3: a newer major", frame_field(4, "<H", 3))
    add("mpdj-header-bytes-short", "headerBytes", "headerBytes 96, below minor 0's 104",
        edit(lambda p: None, header_bytes=96))
    add("mpdj-misaligned", "djLayout", "headerBytes 136: DJSL at 264, not on a slot boundary",
        edit(lambda p: None, header_bytes=136))

    def reorder(order):
        def fn(p):
            p.sections = [p.section(t) for t in order]
        return fn

    add("mpdj-djsl-not-first", "djLayout", "DJRF before DJSL: DJSL isn't the first directory entry",
        edit(reorder((DJRF, DJSL, DJPH, STRS)), realign=True))
    add("mpdj-section-order", "djLayout", "STRS before DJPH", edit(reorder((DJSL, DJRF, STRS, DJPH)), realign=True))
    # The header (8.2).
    add("mpdj-rows-zero", "enum", "rowCount 0", edit(field("<I", 40, 0)))
    add("mpdj-k-one", "enum", "k 1 (2-169)", edit(field("<H", 48, 1)))
    add("mpdj-k-170", "enum", "k 170 (2-169)", edit(field("<H", 48, 170)))
    add("mpdj-slot-bytes", "enum", "slotBytes 512 with k 4 (k 2-84 takes 256)", edit(field("<H", 50, 512)))
    add("mpdj-index-bytes", "enum", "indexBytes 4", edit(field("<B", 52, 4)))
    add("mpdj-score-kind", "enum", "scoreKind 2", edit(field("<B", 53, 2)))

    def sel(kind, head, floor):
        def fn(p):
            p.put("<B", 54, kind)
            p.put("<B", 55, head)
            p.put("<B", 100, floor)
        return fn

    add("mpdj-sel-kind0-head", "djSelection", "selKind 0 with selHead 3, not k", edit(sel(0, 3, 0)))
    add("mpdj-sel-kind0-floor", "djSelection", "selKind 0 with selFloor 140, not 0", edit(sel(0, 4, 140)))
    add("mpdj-sel-head-zero", "djSelection", "selKind 1 with selHead 0", edit(sel(1, 0, 100)))
    add("mpdj-sel-head-big", "djSelection", "selKind 1 with selHead 3 = k - 1", edit(sel(1, 3, 100)))
    add("mpdj-sel-floor-zero", "djSelection", "selKind 1 with selFloor 0", edit(sel(1, 1, 0)))
    add("mpdj-sel-unknown-head", "djSelection", "an unknown selKind 7 with selHead 5, above k", edit(sel(7, 5, 9)))
    add("mpdj-row-count", "counts", "rowCount 7, DJSL and DJRF hold 6", edit(field("<I", 40, 7)))
    add("mpdj-path-count", "counts", "pathCount 8, DJPH holds 7", edit(field("<I", 44, 8)))

    def djrf_five(p):
        s = p.section(DJRF)
        s[2], s[4] = 5, s[4][:40]

    add("mpdj-djrf-count", "counts", "DJRF of 5 records for 6 rows", edit(djrf_five))

    def slot_stride(p):
        s = p.section(DJSL)
        s[3] = 512
        s[4] = bytearray(b"".join(bytes(s[4][256 * i:256 * (i + 1)]) + bytes(256) for i in range(s[2])))

    add("mpdj-slot-stride", "shape", "DJSL's stride 512, slotBytes 256", edit(slot_stride))

    def drop(t):
        def fn(p):
            p.sections = [s for s in p.sections if s[0] != t]
        return fn

    add("mpdj-missing-djrf", "missing", "no DJRF (a REQUIRED section)", edit(drop(DJRF), realign=True))

    def insert_zz(flags):
        def fn(p):
            p.sections.insert(3, [b"DJZZ", flags, 0, 0, bytearray(b"a later minor's section")])
        return fn

    add("mpdj-unknown-required", "unknownRequired", "an unknown REQUIRED section DJZZ before STRS",
        edit(insert_zz(REQUIRED), realign=True))

    def no_paths(stride):
        def fn(p):
            s = p.section(DJPH)
            s[2], s[3], s[4] = 0, stride, bytearray()
            p.put("<I", 44, 0)
        return fn

    add("mpdj-paths-empty-stride-zero", "shape", "pathCount 0 with DJPH's stride 0: an empty array keeps its "
        "stride (10)", edit(no_paths(0)))

    # Sums and products that wrap in 32 bits (8.1: computed without
    # overflow): a reader that wraps reads each of these wrongly.
    def wrap_header_bytes():
        b = bytearray(g1b)
        struct.pack_into("<I", b, 8, 0xFFFFFFF8)
        struct.pack_into("<I", b, 32, 0)
        struct.pack_into("<I", b, 32, crc32(b[:(0xFFFFFFF8 + 32 * 4) & 0xFFFFFFFF]))
        return bytes(b)

    add("mpdj-header-bytes-wrap", "short", "headerBytes 0xFFFFFFF8: headerBytes + 32 x sectionCount is past the "
        "file (wrapped to 32 bits it is 120, and the header CRC is right for that)", wrap_header_bytes())

    def wrap_section_end():
        # DJZY (8 zero bytes) and an empty DJZZ, both optional, before STRS;
        # then DJZZ claims 0xFFFFFFF8 bytes. Its end wrapped to 32 bits is
        # DJZY's start, where this STRS starts (DJZY's zeros, then the
        # strings, their offsets moved by 8): a reader that wraps finds
        # every other check passing.
        p = Parts(g1b)
        offs = struct.unpack_from("<5I", p.head, 80 - 40)
        struct.pack_into("<5I", p.head, 80 - 40, *[o + 8 if o else 0 for o in offs])
        p.sections[3:3] = [[b"DJZY", 0, 0, 0, bytearray(8)], [b"DJZZ", 0, 0, 0, bytearray()]]
        b = bytearray(p.build(realign=True))
        hb, n = struct.unpack_from("<II", b, 8)
        zy, zz, st = (hb + 32 * i for i in (3, 4, 5))
        at = struct.unpack_from("<I", b, zy + 8)[0]
        struct.pack_into("<I", b, zz + 12, 0xFFFFFFF8)
        struct.pack_into("<II", b, st + 8, at, len(b) - at)
        struct.pack_into("<I", b, st + 24, crc32(b[at:]))
        struct.pack_into("<I", b, 32, 0)
        struct.pack_into("<I", b, 32, crc32(b[:hb + 32 * n]))
        return bytes(b)

    add("mpdj-section-end-wrap", "layout", "an optional section of 0xFFFFFFF8 bytes before STRS: its end is past "
        "the file (wrapped to 32 bits it is the start of STRS, placed there)", wrap_section_end())

    def wrap_count(p):
        s = p.section(DJPH)
        s[2] += 1 << 31
        p.put("<I", 44, s[2])

    add("mpdj-array-bytes-wrap", "shape", "DJPH's count and pathCount 7 + 2^31: count x stride isn't its 70 bytes "
        "(wrapped to 32 bits it is)", edit(wrap_count))
    # STRS, DJPH (8.3).

    def strs_edit(fn):
        def inner(p):
            fn(p.section(STRS)[4], p)
        return inner

    add("mpdj-strs-first-byte", "string", "STRS: byte 0 isn't NUL",
        edit(strs_edit(lambda s, p: s.__setitem__(0, ord("x")))))
    add("mpdj-string-no-nul", "string", "STRS: the last string has no NUL",
        edit(strs_edit(lambda s, p: s.__setitem__(len(s) - 1, ord("x")))))
    add("mpdj-string-out-of-range", "string", "attribution's offset at STRS's end",
        edit(strs_edit(lambda s, p: p.put("<I", 96, len(s)))))

    def swap_strings(s, p):
        a, b = struct.unpack_from("<II", p.head, 80 - 40)
        struct.pack_into("<II", p.head, 80 - 40, b, a)

    add("mpdj-string-order", "stringOrder", "modelId's and modelVersion's offsets swapped (each string valid)",
        edit(strs_edit(swap_strings)))

    def paths(kind):
        def fn(p):
            s = p.section(DJPH)[4]
            if kind == "swap":
                s[0:10], s[10:20] = s[10:20], s[0:10]
            elif kind == "equal":
                s[10:18] = s[0:8]
            else:
                struct.pack_into("<H", s, 8, 6)
        return fn

    add("mpdj-paths-unsorted", "djPaths", "DJPH: two entries swapped", edit(paths("swap")))
    add("mpdj-paths-equal", "djPaths", "DJPH: two equal hashes", edit(paths("equal")))
    add("mpdj-paths-row-range", "djPaths", "DJPH: a row of 6 (rowCount 6)", edit(paths("range")))

    # Slots (8.4): each edit reseals the slot's CRC but the CRC's own fault.
    def slot_edit(r, fn, reseal=True):
        def inner(p):
            data, o, sb = p.slot(r)
            fn(data, o)
            if reseal:
                p.reseal_slot(r)
        return inner

    def bad_crc(data, o):
        data[o + 252] ^= 0x01

    add("mpdj-slot-crc", "djSlotCrc", "slot 2's CRC wrong", edit(slot_edit(2, bad_crc, reseal=False)),
        slot=(2, DAMAGED))

    def swap_slots(p):
        data, o1, sb = p.slot(1)
        _, o2, _ = p.slot(2)
        data[o1:o1 + sb], data[o2:o2 + sb] = data[o2:o2 + sb], data[o1:o1 + sb]

    add("mpdj-slot-wrong-row", "djSlotCrc", "slots 1 and 2 swapped, each CRC right for its own row",
        edit(swap_slots), slot=(1, DAMAGED))

    def swap_entries(data, o):      # row 3: 2/128, 0/100, 1/0, unused -> the unused before row 1
        data[o + 4:o + 6], data[o + 6:o + 8] = data[o + 6:o + 8], data[o + 4:o + 6]
        data[o + 8 + 2], data[o + 8 + 3] = data[o + 8 + 3], data[o + 8 + 2]

    add("mpdj-slot-unused", "djSlotUnused", "slot 3: an unused entry (0xFFFF) before a used one",
        edit(slot_edit(3, swap_entries)), slot=(3, BROKEN))
    add("mpdj-slot-index-range", "djSlotIndex", "slot 0: entry 3's index 6 = rowCount",
        edit(slot_edit(0, lambda d, o: struct.pack_into("<H", d, o + 6, 6))), slot=(0, BROKEN))
    add("mpdj-slot-self", "djSlotIndex", "slot 0: entry 3's index 0, the row itself",
        edit(slot_edit(0, lambda d, o: struct.pack_into("<H", d, o + 6, 0))), slot=(0, BROKEN))
    add("mpdj-slot-level-rises", "djSlotOrder", "slot 0: entry 1's level 201 after entry 0's 200",
        edit(slot_edit(0, lambda d, o: d.__setitem__(o + 8 + 1, 201))), slot=(0, BROKEN))
    add("mpdj-slot-tie-order", "djSlotOrder", "slot 2: entry 2's level 171, so rows 5 then 4 tie with the index "
        "falling", edit(slot_edit(2, lambda d, o: d.__setitem__(o + 8 + 2, 171))), slot=(2, BROKEN))

    # Present (9).
    def minor1(p):
        p.minor = 1
        s = p.section(STRS)[4]
        later = len(s)
        s += b"a later minor's string\0"
        p.head = bytearray(p.head[:64]) + struct.pack("<II", 0x0C0FFEE0, later)
        for t, tail in ((DJRF, 4), (DJPH, 2)):
            sec = p.section(t)
            st = sec[3]
            sec[4] = bytearray(b"".join(bytes(sec[4][st * i:st * (i + 1)]) + bytes([0xA5]) * tail
                                        for i in range(sec[2])))
            sec[3] = st + tail
        p.sections.insert(3, [b"DJZZ", 0, 0, 0, bytearray(b"a later minor's section")])

    add("mpdj-minor-1", "ok", "minor 1: a 112-byte header (a field at 104, a string offset at 108 after "
        "attribution), DJRF and DJPH strides 12 with non-zero tails, an unknown optional section before STRS",
        edit(minor1, header_bytes=352), same="autodj-00000029.bin")
    add("mpdj-unknown-optional", "ok", "an unknown optional section DJZZ before STRS",
        edit(insert_zz(0), realign=True), same="autodj-00000029.bin")
    add("mpdj-header-bigger", "ok", "headerBytes 384 on 256-byte slots (not the smallest; still aligned)",
        edit(lambda p: None, header_bytes=384), same="autodj-00000029.bin")
    add("mpdj-paths-empty", "ok", "pathCount 0 (every card file's hash in a collision, 4.5): DJPH empty, count 0 "
        "and stride 10; the rows and their lists stay", edit(no_paths(10)))
    add("mpdj-sel-unknown-kind", "ok", "an unknown selKind 7 (selHead 2, selFloor 9): readers use only selHead "
        "and selFloor", edit(sel(7, 2, 9)), same="autodj-00000029.bin")
    add("mpdj-slot-unused-level", "ok", "slot 3: the unused entry's level byte 7 (not checked; CRC made right)",
        edit(slot_edit(3, lambda d, o: d.__setitem__(o + 8 + 3, 7))), same="autodj-00000029.bin")
    add("mpdj-slot-padding", "ok", "autodj-small-default's slot 0: padding byte 507 set (not checked; CRC made "
        "right)", edit(slot_edit(0, lambda d, o: d.__setitem__(o + 507, 0x5A)), src=golden["autodj-small-default.bin"]),
        same="autodj-small-default.bin")
    add("mpdj-uses-device-slot-crc", "ok", "mpdj-slot-crc read by the device: the join's sections are good; the "
        "slot fails when read", edit(slot_edit(2, bad_crc, reseal=False)), uses="device", slot=(2, DAMAGED))
    return out


# --- vectors.json "mpdj" (13.3) ----------------------------------------------
def fixture_vectors(golden):
    """The "mpdj" group of vectors.json, each value recomputed here."""
    v = {"note": "AUTODJ-INDEX.md 13.3. 'who': both unless said. Byte strings are lowercase hex; integers in hex "
                 "strings are 0x-prefixed."}
    levels = [{"cos": c, "level": level(c)} for c in (1.0, 0.75, 0.5, 0.390625, 0.25, 0.0, -0.5)]
    want = [255, 191, 128, 100, 64, 0, 0]
    if [e["level"] for e in levels] != want:
        raise AssertionError("levels")
    f = _mid_facts(24, 8)
    rows, ints, emb, keys, facts = tv_mid()
    a_g, b_g = facts["trapG"]
    cos = cos_rule(emb[a_g], emb[b_g])
    c32 = cos_f32(emb[a_g], emb[b_g])
    levels += [{"cos": cos, "level": level(cos), "note": "the float32 trap pair, by the rule"},
               {"cos": c32, "level": level(c32), "note": "the float32 trap pair, by a float32 sum"}]
    v["levels"] = levels
    floors = [(20, 51), (30, 77), (45, 115), (55, 141), (65, 166), (75, 192), (85, 217), (90, 230)]
    for p, lv in floors:
        if floor_level(p) != lv or lv != math.ceil(255 * p / 100 - 1e-9) or lv * 100 < 255 * p:
            raise AssertionError("floor %d" % p)
    v["floors"] = {"who": "informative", "cases": [{"percent": p, "level": lv} for p, lv in floors]}
    v["slotBytes"] = [{"k": k, "slotBytes": slot_bytes_for(k)} for k in (1, 2, 84, 85, 169, 170)]
    if [e["slotBytes"] for e in v["slotBytes"]] != [0, 256, 256, 512, 512, 0]:
        raise AssertionError("slotBytes")
    hb = [(4, 256, 104, 128), (4, 512, 104, 384), (5, 256, 104, 352), (5, 512, 104, 352), (8, 512, 104, 256),
          (8, 256, 104, 256), (4, 256, 112, 128)]
    for n, sb, mn, want_hb in hb:
        if header_bytes_for(n, sb, mn) != want_hb:
            raise AssertionError("headerBytes %r" % ((n, sb, mn),))
    v["headerBytes"] = [{"sectionCount": n, "slotBytes": sb, "minimum": mn, "headerBytes": h} for n, sb, mn, h in hb]
    sr = [{"k": 4, "head": 1, "m": 5, "ranks": strided_ranks(4, 1, 5)},
          {"k": 24, "head": 8, "m": 100, "ranks": strided_ranks(24, 8, 100)},
          {"k": 169, "head": 84, "m": 170, "ranks": strided_ranks(169, 84, 170), "note": "rank 168 skipped"},
          {"k": 169, "head": 84, "m": 2258,
           "pairs": [{"j": j, "rank": strided_ranks(169, 84, 2258)[j]} for j in (0, 83, 84, 85, 126, 168)]},
          {"k": 169, "head": 84, "m": 169, "ranks": list(range(169)), "note": "m <= k: the top k"}]
    if (sr[0]["ranks"] != [0, 1, 2, 4] or sr[1]["ranks"] != list(range(8)) + list(range(8, 93, 6)) + [99]
            or sr[2]["ranks"] != list(range(168)) + [169]
            or [p["rank"] for p in sr[3]["pairs"]] != [0, 83, 84, 109, 1170, 2257]):
        raise AssertionError("stridedRanks")
    v["stridedRanks"] = sr
    r = Reader(golden["autodj-00000029.bin"])
    raw = r.slot_raw(3)
    v["slotCrc"] = {"file": "autodj-00000029.bin", "row": 3, "crc": "0x%08X" % struct.unpack_from("<I", raw, 252)[0],
                    "unseeded": "0x%08X" % crc32(raw[:252]),
                    "note": "CRC-32 over the row as u32 LE, then the slot's first slotBytes - 4 bytes; 'unseeded' "
                            "is the CRC of those bytes alone"}
    p = prepare(g1())
    v["signature"] = {"file": "autodj-00000029.bin", "text": p.sig_text, "sig": p.sig.hex()}
    if r.sig != p.sig:
        raise AssertionError("signature")
    v["vec"] = {"file": "autodj-00000029.bin", "dim": 8, "sha256": p.vec_sha256}
    v["deviceTxt"] = {"who": "software", "note": "a writer writes MPDJ v2 only when /.player/device.txt's read.mpdj "
                                                 "lists 2; no file, or no read.mpdj key, reads as none",
                      "cases": [{"readMpdj": "", "writesV2": False}, {"readMpdj": "1", "writesV2": False},
                                {"readMpdj": "2", "writesV2": True}, {"readMpdj": "1,2", "writesV2": True},
                                {"noFile": True, "writesV2": False}, {"keyMissing": True, "writesV2": False}]}
    d = f["d"]
    v["trap"] = {"file": "autodj-mid.bin", "rowA": d["rowA"], "rowB": d["rowB"], "level": d["level"],
                 "levelF32": d["levelF32"], "cos": cos, "cosF32": c32,
                 "note": "the rule's cosine x 255 is just above 139.5; a float32 sum's is just below it"}
    if level(cos) != DEFAULT_FLOOR or level(c32) != DEFAULT_FLOOR - 1:
        raise AssertionError("trap")
    return v


# ---------------------------------------------------------------------------
# The command line
# ---------------------------------------------------------------------------
def _load_desc(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def main(argv=None):
    ap = argparse.ArgumentParser(description="The AutoDJ index (MPDJ v2, docs/AUTODJ-INDEX.md): the reference "
                                             "writer and reader.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="a file from a description")
    b.add_argument("desc")
    b.add_argument("out")
    b.add_argument("--f32", action="store_true", help="levels from a float32 sum: a wrong writer, for the trap")
    g = b.add_mutually_exclusive_group()
    g.add_argument("--fast", dest="fast", action="store_true", default=None, help="numpy's fast path")
    g.add_argument("--no-fast", dest="fast", action="store_false", help="the rule as written, stdlib only")
    c = sub.add_parser("check", help="validate a file as a reader does (8)")
    c.add_argument("file")
    c.add_argument("--uses", choices=("all", "device"), default="all")
    d = sub.add_parser("dump", help="what a file holds")
    d.add_argument("file")
    d.add_argument("--slot", type=int)
    d.add_argument("--hex", action="store_true")
    s = sub.add_parser("signature", help="a description's selection signature")
    s.add_argument("desc")
    r = sub.add_parser("restamp", help="a new file around an old file's lists (same signature)")
    r.add_argument("old")
    r.add_argument("desc")
    r.add_argument("out")
    z = sub.add_parser("size", help="a file's length (minor 0)")
    z.add_argument("rows", type=int)
    z.add_argument("--paths", type=int)
    z.add_argument("--k", type=int, default=DEFAULT_K)
    z.add_argument("--strings", type=int, default=134)
    a = ap.parse_args(argv)
    try:
        if a.cmd == "build":
            desc = _load_desc(a.desc)
            if a.f32:
                p = prepare(desc, base=Path(a.desc).parent)
                data = write(p, compute_lists(p, a.fast, f32_sum=True))
            else:
                data = build(desc, base=Path(a.desc).parent, fast=a.fast)
            Path(a.out).write_bytes(data)
            print("%s: %d bytes, headerCrc 0x%08X, selectionSig %s" % (
                a.out, len(data), struct.unpack_from("<I", data, 32)[0], data[64:80].hex()))
        elif a.cmd == "check":
            why = check(Path(a.file).read_bytes(), a.uses)
            print("%s: %s" % (a.file, "present" if why == "ok" else "absent (%s)" % why))
            return 0 if why == "ok" else 1
        elif a.cmd == "dump":
            dump(Path(a.file).read_bytes(), a.slot, a.hex)
        elif a.cmd == "signature":
            p = prepare(_load_desc(a.desc), base=Path(a.desc).parent)
            sys.stdout.write(p.sig_text)
            print("selectionSig %s" % p.sig.hex())
        elif a.cmd == "restamp":
            data = restamp(Path(a.old).read_bytes(), _load_desc(a.desc), base=Path(a.desc).parent)
            Path(a.out).write_bytes(data)
            print("%s: %d bytes, headerCrc 0x%08X" % (a.out, len(data), struct.unpack_from("<I", data, 32)[0]))
        elif a.cmd == "size":
            n = size(a.rows, a.paths, a.k, a.strings)
            print("%d bytes (%.2f MB)" % (n, n / 1e6))
    except WriterError as e:
        print("refused: %s" % e, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
