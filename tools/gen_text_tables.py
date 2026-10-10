#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Generates TextFold's Unicode tables (lib/core/TextFoldTables.cpp; the
declarations are in TextFold.h) and test_text_fold's composition cases
(test/test_text_fold/compose_cases.h), from Python's unicodedata.

    python tools/gen_text_tables.py            # regenerate both
    python tools/gen_text_tables.py --check    # exit 1 if either checked-in file differs

docs/I18N.md, phase 0. The tables:

  kPairs, kPairMarks  NFC's canonical composition pairs (its primary
        composites: two-code-point canonical decompositions that NFC
        composes again, so no singleton and no exclusion) whose composite is
        Latin (U+0000-024F, U+1E00-1EFF), Greek (U+0370-03FF, U+1F00-1FFF),
        Cyrillic (U+0400-052F) or kana (U+3040-30FF): 839 pairs, 31 second
        code points (the combining marks, and the kana voicing marks). An
        entry is first << 18 | its mark's index << 13 | the composite's low
        13 bits, sorted; the composite is those bits | (first & 0x2000)
        (the generator checks that every pair keeps to it).
  kCombiningClass  the Canonical_Combining_Class of U+0300-036F (the
        composer's other marks, U+0483-0487 and U+3099-309A, are 230 and
        8: TextFold.cpp; checked here).
  kLatinExtB, kLatinExtAdditional, kFolds  Full folding of Latin
        Extended-B (U+0180-024F) and Latin Extended Additional
        (U+1E00-1EFF): one base letter a code point (kFolds for the ones that
        fold to two, and for the IPA letters that are the other case of a
        folded Extended-B one: ɓ ɔ ə ɛ ʒ ...), '\\0' for none. A letter folds
        to its canonical decomposition's base letter (ș -> s, ỹ -> y), else
        its compatibility decomposition's letters (ǆ -> dz), else to the
        letter its name is built on (ƀ "B WITH STROKE" -> b, Ɔ "OPEN O" ->
        O, ǝ "TURNED E" -> e, ȸ "DB DIGRAPH" -> db), else to EXPLICIT's.
        The rest (ƍ, Ʃ, Ȝ, the tone letters, the clicks) stays '?'.
  kCp1252     U+0080-009F as their Windows-1252 characters (0: one
        cp1252 leaves undefined): docs/METADATA.md 5.2 lets a device draw
        them so.
  kDigest     FNV-1a 64 of what the tables say (every composite of
        U+0000-30FF with each mark, the classes, the folds of U+0180-02AF
        and U+1E00-1EFF, the cp1252 map), as test_text_fold recomputes it
        through TextFold's own functions.

The cases: 600 strings in the tables' range, made by a seeded random.Random
(so the same Python makes the same file): NFD text and NFC text (NFC's
result expected of both), and base letters with up to four combining
marks in any order. test_text_fold composes each with textfold::Composer
and compares with unicodedata.normalize("NFC", ...).

These blocks have been complete since Unicode 5.1 and canonical
compositions are stable, so any Python 3 makes the same bytes (this was
made with Python 3.10's Unicode 13.0.0). Standard library only.
"""
import argparse
import random
import re
import sys
import unicodedata as ud
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "lib" / "core" / "TextFoldTables.cpp"
CASES = ROOT / "test" / "test_text_fold" / "compose_cases.h"

# The composites the pair table keeps: Latin, Greek, Cyrillic and kana.
PAIR_RANGES = ((0x0000, 0x024F), (0x1E00, 0x1EFF), (0x0370, 0x03FF), (0x1F00, 0x1FFF), (0x0400, 0x052F),
               (0x3040, 0x30FF))
EXT_B = (0x0180, 0x024F)
EXT_ADDITIONAL = (0x1E00, 0x1EFF)
# Folds the rules below don't find (a letter with no decomposition and a
# name that isn't "<letter> WITH ..."): the common reading of each.
EXPLICIT = {
    0x018F: "E",   # Ə schwa (Azerbaijani; its small ə is IPA's U+0259)
    0x01B7: "Z",   # Ʒ ezh
    0x01BA: "z",   # ƺ ezh with tail
    0x01BF: "w",   # ƿ wynn
    0x01F7: "W",   # Ƿ wynn
    0x1E9E: "SS",  # ẞ capital sharp s
}
# A precomposed letter's base that isn't ASCII but folds (Latin-1's own
# multi-letter folds, TextFold.cpp's latin1Multi).
KNOWN = {0x00C6: "AE", 0x00E6: "ae", 0x00D8: "O", 0x00F8: "o"}
NAME_RULE = re.compile(r"^LATIN (CAPITAL|SMALL) LETTER (?:(?:OPEN|TURNED|REVERSED|AFRICAN|DOTLESS|MIDDLE-WELSH|LONG|SMALL) )?"
                       r"([A-Z]{1,2})(?: WITH .*| BAR| DIGRAPH)?$")
OTHER_MARKS = {0x0483: 230, 0x0484: 230, 0x0485: 230, 0x0486: 230, 0x0487: 230, 0x3099: 8, 0x309A: 8}


def in_ranges(cp, ranges):
    return any(lo <= cp <= hi for lo, hi in ranges)


def pairs():
    """{(first, second): composite} for NFC's primary composites in PAIR_RANGES."""
    out = {}
    for cp in range(0x110000):
        if not in_ranges(cp, PAIR_RANGES):
            continue
        d = ud.decomposition(chr(cp))
        if not d or d.startswith("<"):
            continue
        parts = [int(x, 16) for x in d.split()]
        if len(parts) != 2:
            continue  # a singleton: never composed
        if ud.normalize("NFC", chr(parts[0]) + chr(parts[1])) != chr(cp):
            continue  # an exclusion (or a non-starter decomposition)
        out[(parts[0], parts[1])] = cp
    return out


def fold_of(cp):
    """The ASCII letters `cp` folds to, or None."""
    if cp in EXPLICIT:
        return EXPLICIT[cp]
    if cp in KNOWN:
        return KNOWN[cp]
    ch = chr(cp)
    try:
        name = ud.name(ch)
    except ValueError:
        return None
    base = ud.normalize("NFD", ch)[0]
    if base.isascii() and base.isalpha():
        return base
    compat = "".join(c for c in ud.normalize("NFKD", ch) if not ud.combining(c))
    if compat and compat.isascii() and compat.isalpha():
        return compat
    if ord(base) != cp:
        f = fold_of(ord(base))
        if f:
            return f
    m = NAME_RULE.match(name)
    if m:
        return m.group(2) if m.group(1) == "CAPITAL" else m.group(2).lower()
    return None


def folds():
    """{cp: letters} for the two blocks, and the other-case partners of the
    folded Extended-B letters that sit outside the folded blocks (IPA's)."""
    out = {}
    for lo, hi in (EXT_B, EXT_ADDITIONAL):
        for cp in range(lo, hi + 1):
            f = fold_of(cp)
            if f:
                out[cp] = f
    for cp, f in list(out.items()):
        if not in_ranges(cp, (EXT_B,)):
            continue
        for partner, case in ((chr(cp).lower(), str.lower), (chr(cp).upper(), str.upper)):
            if len(partner) != 1:
                continue
            p = ord(partner)
            if p < 0x0180 or in_ranges(p, (EXT_B, EXT_ADDITIONAL)) or p in out:
                continue
            out[p] = case(f)
    return out


def cp1252():
    out = []
    for b in range(0x80, 0xA0):
        try:
            out.append(ord(bytes([b]).decode("cp1252")))
        except UnicodeDecodeError:
            out.append(0)
    return out


class Fnv:
    def __init__(self):
        self.h = 0xCBF29CE484222325

    def add(self, data):
        for b in data:
            self.h ^= b
            self.h = (self.h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF


def digest(pair_map, marks, ccc, fold_map, c1):
    """What test_text_fold recomputes (test_tables_digest)."""
    h = Fnv()
    for first in range(0x3100):
        for m in marks:
            h.add(pair_map.get((first, m), 0).to_bytes(4, "little"))
    for cp in list(range(0x0300, 0x0370)) + list(range(0x0483, 0x0488)) + [0x3099, 0x309A]:
        h.add(bytes([ccc(cp)]))
    for cp in list(range(0x0180, 0x02B0)) + list(range(0x1E00, 0x1F00)):
        h.add(fold_map.get(cp, "?").encode("ascii") + b"\0")
    for i, cp in enumerate(c1):
        h.add((cp or 0x80 + i).to_bytes(2, "little"))
    return h.h


def c_char(c):
    return "\\0" if c is None else c


def generate():
    pair_map = pairs()
    marks = sorted({m for _, m in pair_map})
    assert len(marks) <= 32, marks
    for (first, m), comp in pair_map.items():
        assert ud.combining(chr(m)) > 0, hex(m)  # a mark: the composer only looks at marks
        assert first < 0x4000 and comp < 0x4000, hex(comp)
        assert (comp & 0x2000) == (first & 0x2000), (hex(first), hex(comp))
        assert ud.combining(chr(first)) == 0, hex(first)
    for cp, c in OTHER_MARKS.items():
        assert ud.combining(chr(cp)) == c, hex(cp)
    for m in marks:
        assert 0x0300 <= m <= 0x036F or m in OTHER_MARKS, hex(m)
    entries = sorted(first << 18 | marks.index(m) << 13 | (comp & 0x1FFF) for (first, m), comp in pair_map.items())

    fold_map = folds()
    ext_b = [fold_map.get(cp) for cp in range(EXT_B[0], EXT_B[1] + 1)]
    ext_add = [fold_map.get(cp) for cp in range(EXT_ADDITIONAL[0], EXT_ADDITIONAL[1] + 1)]
    multi = sorted((cp, f) for cp, f in fold_map.items()
                   if len(f) > 1 or not in_ranges(cp, (EXT_B, EXT_ADDITIONAL)))
    for _, f in multi:
        assert 1 <= len(f) <= 2 and f.isascii() and f.isalpha(), f
    c1 = cp1252()

    def ccc(cp):
        return ud.combining(chr(cp))

    dig = digest(pair_map, marks, ccc, fold_map, c1)

    def block(table):
        s = "".join(c_char(t if t and len(t) == 1 else None) for t in table)
        # 16 code points a line, as kLatinExtA.
        lines = []
        for i in range(0, len(table), 16):
            lines.append('    "' + "".join(c_char(t if t and len(t) == 1 else None) for t in table[i:i + 16]) + '"')
        assert s
        return "\n".join(lines)

    out = []
    out.append("""// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar
// Generated by tools/gen_text_tables.py from Python's unicodedata. Do not
// edit: change the generator and run `python tools/gen_text_tables.py`.
// TextFold's composition pairs, combining classes, the Latin Extended-B and
// Extended Additional folds and the cp1252 map (docs/I18N.md, phase 0).

#include "TextFold.h"

namespace textfold {
namespace tables {
""")
    out.append(f"// {len(entries)} pairs: first << 18 | mark index << 13 | the composite's low 13 bits,\n"
               "// sorted (the composite: those bits | (first & 0x2000)).\n")
    out.append("const uint32_t kPairs[] = {\n")
    for i in range(0, len(entries), 6):
        out.append("    " + " ".join(f"0x{e:08X}," for e in entries[i:i + 6]) + "\n")
    out.append("};\nconst uint32_t kPairCount = sizeof(kPairs) / sizeof(kPairs[0]);\n\n")
    out.append(f"// The pairs' second code points, by index ({len(marks)}).\n")
    out.append("const uint16_t kPairMarks[] = {\n")
    for i in range(0, len(marks), 8):
        out.append("    " + " ".join(f"0x{m:04X}," for m in marks[i:i + 8]) + "\n")
    out.append("};\nconst uint32_t kPairMarkCount = sizeof(kPairMarks) / sizeof(kPairMarks[0]);\n\n")
    out.append("// Canonical_Combining_Class of U+0300..U+036F.\n")
    out.append("const uint8_t kCombiningClass[0x70] = {\n")
    for i in range(0x300, 0x370, 16):
        out.append("    " + " ".join(f"{ccc(cp)}," for cp in range(i, i + 16)) + "\n")
    out.append("};\n\n")
    out.append("// U+0180..U+024F: the base letter, '\\0' for none or more than one (kFolds).\n")
    out.append("const char kLatinExtB[0xD0 + 1] =\n" + block(ext_b) + ";\n\n")
    out.append("// U+1E00..U+1EFF: the base letter, '\\0' for none or more than one (kFolds).\n")
    out.append("const char kLatinExtAdditional[0x100 + 1] =\n" + block(ext_add) + ";\n\n")
    out.append("// Two letters, or a letter outside the two blocks (the other case of a\n"
               "// folded Extended-B letter), by code point.\n")
    out.append("const Fold kFolds[] = {\n")
    for cp, f in multi:
        out.append(f'    {{0x{cp:04X}, "{f}"}},  // {chr(cp)} {ud.name(chr(cp)).lower()}\n')
    out.append("};\nconst uint32_t kFoldCount = sizeof(kFolds) / sizeof(kFolds[0]);\n\n")
    out.append("// U+0080..U+009F as Windows-1252 has them (0: undefined there).\n")
    out.append("const uint16_t kCp1252[32] = {\n")
    for i in range(0, 32, 8):
        out.append("    " + " ".join(f"0x{c:04X}," for c in c1[i:i + 8]) + "\n")
    out.append("};\n\n")
    out.append(f"const uint64_t kDigest = 0x{dig:016X}ull;\n\n}}  // namespace tables\n}}  // namespace textfold\n")
    return "".join(out), cases(pair_map)


def c_string(s):
    """A C string literal: ASCII as it is (", \\ escaped), the rest \\u/\\U."""
    out = []
    for c in s:
        o = ord(c)
        if c in '"\\':
            out.append("\\" + c)
        elif 0x20 <= o < 0x7F:
            out.append(c)
        elif o > 0xFFFF:
            out.append(f"\\U{o:08X}")
        else:
            assert o >= 0xA0, hex(o)
            out.append(f"\\u{o:04X}")
    return '"' + "".join(out) + '"'


def cases(pair_map):
    rng = random.Random(20261010)
    assigned = []
    for lo, hi in PAIR_RANGES:
        for cp in range(max(lo, 0x41), hi + 1):
            ch = chr(cp)
            if ud.category(ch)[0] == "L" and ud.combining(ch) == 0:
                assigned.append(ch)
    # Base letters: no decomposition at all (so no singleton), not a mark.
    bases = [c for c in assigned if ud.decomposition(c) == ""]
    marks = sorted({m for _, m in pair_map}) + [0x0315, 0x031A, 0x0334, 0x0338, 0x0345, 0x0360, 0x0483]
    hangul = [chr(0xAC00 + rng.randrange(11172)) for _ in range(64)]
    pool = assigned + hangul + list("  -'0123456789")

    def text(n):
        s = "".join(rng.choice(pool) for _ in range(n))
        if rng.random() < 0.3:  # a stray mark somewhere
            i = rng.randrange(len(s) + 1)
            s = s[:i] + chr(rng.choice(marks)) + s[i:]
        return s

    out = []
    seen = set()

    def add(kind, s):
        nfc = ud.normalize("NFC", s)
        if (s, nfc) in seen:
            return
        seen.add((s, nfc))
        out.append((kind, s, nfc))

    while len(out) < 250:
        add("nfd", ud.normalize("NFD", text(rng.randint(1, 5))))
    while len(out) < 400:
        add("nfc", ud.normalize("NFC", text(rng.randint(1, 5))))
    while len(out) < 600:
        s = rng.choice(bases) + "".join(chr(rng.choice(marks)) for _ in range(rng.randint(1, 4)))
        add("marks", s)
    lines = ["""// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar
// Generated by tools/gen_text_tables.py from Python's unicodedata. Do not
// edit. Random strings in the composition tables' range (Latin, Greek,
// Cyrillic, kana, Hangul) and what unicodedata.normalize("NFC", ...) makes
// of them: NFD text, NFC text, and base letters with up to four combining
// marks in any order.
#pragma once

namespace composecases {

struct Case {
  const char* in;
  const char* nfc;
};

const Case kCases[] = {
"""]
    for kind, s, nfc in out:
        lines.append(f"    {{{c_string(s)}, {c_string(nfc)}}},  // {kind}\n")
    lines.append("};\n\n}  // namespace composecases\n")
    return "".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="fail if a generated file is out of date")
    args = ap.parse_args()
    tables, case_text = generate()
    targets = ((OUT, tables), (CASES, case_text))
    if args.check:
        stale = [str(p.relative_to(ROOT)) for p, text in targets
                 if not p.exists() or p.read_text(encoding="utf-8") != text]
        if stale:
            print("gen_text_tables: out of date: " + ", ".join(stale), file=sys.stderr)
            return 1
        print("gen_text_tables: up to date")
        return 0
    for p, text in targets:
        p.write_text(text, encoding="utf-8", newline="\n")
        print(f"gen_text_tables: wrote {p.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
