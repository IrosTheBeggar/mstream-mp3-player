#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Makes the UI's strings (lib/core/UiStringIds.h, lib/core/UiStringsData.cpp)
and the width tests' view of the catalogue (test/test_ui_strings/catalogue.h)
from the catalogue, i18n/<lang>.json (docs/I18N.md, phase 3).

    python tools/gen_ui_strings.py            # validate, regenerate the three
    python tools/gen_ui_strings.py --check    # exit 1 if one differs (or a catalogue is wrong)
    python tools/gen_ui_strings.py --report   # each language's coverage
    python tools/gen_ui_strings.py --dump en  # every message of a language, one a line

The catalogue (i18n/README.md says it for translators):

  en.json is the source: every message, by id ("coach.title"), and after it
  "@<id>", what a translator needs: its context, its room (the font, the
  pixels, the lines), its placeholders and what fills them at worst, the
  placeholder that is a plural message's count. The other files hold only
  their messages ("<id>": text), and "@<id>": {"note": ...} if they like.
  "@@locale" is the file's tag and "@@autonym" the language's name in
  itself (AUTONYMS', the apps' spelling).

  A message is a string, or a plural message's forms by CLDR category
  ({"one": ..., "other": ...}). {0} to {3} are its placeholders, the only
  special characters; '\\n' a line break the layout keeps (its room's lines).
  A translation's "@<id>" may also say "overflow": true: a text the width
  test lets be in that language until its layout is fixed (as en.json's).

Validation fails the run (and --check) on: a file that isn't UTF-8; an id
English hasn't; a plural message given as a string or the other way round;
a category the language hasn't, or one it needs missing (one, other; one,
few, many in pl and ru; other in ja and zh: "many" in es, fr, it, pt and
"other" in pl, ru are optional, as no count here reaches them);
placeholders that differ from English's (a form whose category is n = 1
alone may leave out the count); more lines than the room has; a stray
brace, a control character or a lone surrogate; a character the device
can't draw as itself (one the four VLW fonts, src/ui/VlwFonts.cpp, and the
CJK store, lib/core/CjkGlyphs16.cpp, both lack, or one TextFold always
folds: the fullwidth forms U+FF01-FF5E, the halfwidth katakana, the minus
sign U+2212), every one in a file named at once with its code point; a
text with no placeholders longer than its "bytes" (or a line longer than
its "line_bytes"); an @@autonym that isn't the language's name in itself.
A message a translation leaves out is English on the device (--report
counts them).

"bytes" in a message's metadata is the smallest buffer (its NUL included)
the text goes through on the device, filled; "line_bytes" the same for
each line, where the code copies the text a line at a time. The generator
checks a text without placeholders; test_ui_strings checks every text
filled at worst. A placeholder's "or" lists the other messages that may
fill it (each at its own worst): the width test tries each in the UI's
language, and keeps the widest (the byte test the longest).

Out: per language, one blob of the messages (UTF-8, NUL after each form,
the same text once) and an offset per id (0xFFFF: none, English is used);
a bit per id for the plural ones. All ten languages are in the firmware's
list (the Language setting offers each, by its name in itself): one with
no catalogue yet has no blob, and draws English. Plus the pseudo-locale
en-XA (English, accented and a third longer, in brackets), compiled where
UISTR_PSEUDO is defined (the host tests): what the width tests run every
room with, to show where a longer language won't fit. Standard library
only.
"""
import argparse
import json
import math
import re
import struct
import sys
import unicodedata
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CATALOGUE = ROOT / "i18n"
OUT_IDS = ROOT / "lib" / "core" / "UiStringIds.h"
OUT_DATA = ROOT / "lib" / "core" / "UiStringsData.cpp"
OUT_TEST = ROOT / "test" / "test_ui_strings" / "catalogue.h"
# What the device draws text with: the four VLW fonts (the same characters
# each) and the CJK glyph store behind them.
VLW = ROOT / "src" / "ui" / "VlwFonts.cpp"
STORE = ROOT / "lib" / "core" / "CjkGlyphs16.cpp"

CATEGORIES = ["zero", "one", "two", "few", "many", "other"]

# The ten languages (the mobile app's and the terminal player's), CLDR 46's
# cardinal categories each: (all, needed). lib/core/UiStrings.cpp has the
# rules; test_ui_strings checks the two agree. A category no count here
# reaches is optional: es, fr, it and pt's "many" (a million's multiples),
# pl and ru's "other" (fractions).
LANGS = {
    "en": ("one other", "one other"),
    "de": ("one other", "one other"),
    "es": ("one many other", "one other"),
    "fr": ("one many other", "one other"),
    "it": ("one many other", "one other"),
    "ja": ("other", "other"),
    "pl": ("one few many other", "one few many"),
    "pt": ("one many other", "one other"),
    "ru": ("one few many other", "one few many"),
    "zh": ("other", "other"),
}
ORDER = list(LANGS)
PSEUDO = "en-XA"
# Each language's name in itself, as the mobile app's Language setting and
# the terminal player's picker write it (mstream_music's
# lib/l10n/language_names.dart; mstream-terminal-player's LANGS): never
# translated. A catalogue's @@autonym must be the same.
AUTONYMS = {
    "en": "English",
    "de": "Deutsch",
    "es": "Español",
    "fr": "Français",
    "it": "Italiano",
    "ja": "日本語",
    "pl": "Polski",
    "pt": "Português",
    "ru": "Русский",
    "zh": "中文",
    PSEUDO: "[Éñĝĺíšĥ]",
}
# A category that holds exactly one whole number (1): its form may say the
# count in words ("Added", "1 played track made way") instead of {N}.
ONLY_ONE = {("en", "one"), ("de", "one"), ("es", "one"), ("it", "one"), ("pl", "one")}

FONTS = ["Body", "Small", "Bold", "Title"]
LAYOUTS = ["line", "wrap", "toast"]
FILLS = ["grouped", "digits", "decimal", "text", "message"]
META_KEYS = {"context", "room", "placeholders", "count", "translate", "overflow", "note", "bytes", "line_bytes"}
ROOM_KEYS = {"font", "else", "px", "lines", "layout", "const", "two_line_px", "optional"}
PH_KEYS = {"means", "type", "worst", "places", "or"}
TRANSLATE = {"yes", "maybe", "dev"}
MAX_OR = 20  # a placeholder's other fills (catalogue.h's Placeholder::alt)

ID_RE = re.compile(r"^[a-z][a-z0-9_]*(\.[a-z0-9_]+)+$")
PH_RE = re.compile(r"\{([0-3])\}")


class Bad(Exception):
    pass


def placeholders(text):
    return set(int(m) for m in PH_RE.findall(text))


def check_text(where, text, max_lines):
    """A message's text: no stray brace, no control character but '\\n',
    no lone surrogate (JSON's "\\ud800")."""
    if not isinstance(text, str):
        raise Bad(f"{where}: not a string")
    rest = PH_RE.sub("", text)
    if "{" in rest or "}" in rest:
        raise Bad(f"{where}: a brace that isn't {{0}}..{{3}}: {text!r}")
    for ch in text:
        if (ord(ch) < 0x20 and ch != "\n") or 0x7F <= ord(ch) < 0xA0:
            raise Bad(f"{where}: control character U+{ord(ch):04X}")
        if 0xD800 <= ord(ch) <= 0xDFFF:
            raise Bad(f"{where}: a lone surrogate U+{ord(ch):04X} (not a character)")
    if text.count("\n") + 1 > max_lines:
        raise Bad(f"{where}: {text.count(chr(10)) + 1} lines, the room has {max_lines}")


def check_bytes(where, text, meta):
    """A text with no placeholders against its buffers (meta's "bytes" and
    "line_bytes", a NUL included); a filled one is test_ui_strings'."""
    if placeholders(text):
        return
    n = len(text.encode("utf-8"))
    if "bytes" in meta and n + 1 > meta["bytes"]:
        raise Bad(f"{where}: {n} bytes of UTF-8, its buffer holds {meta['bytes'] - 1}")
    for line in text.split("\n"):
        n = len(line.encode("utf-8"))
        if "line_bytes" in meta and n + 1 > meta["line_bytes"]:
            raise Bad(f"{where}: a line of {n} bytes of UTF-8, its buffer holds {meta['line_bytes'] - 1}")


# ---- what the device draws ----

_drawn = {}


def vlw_codepoints(path=VLW):
    """The code points the VLW fonts draw (tools/vlw_font.py's arrays: a
    glyph count, then 28 B of metrics a glyph from byte 24, the code point
    first). The four fonts have the same characters (test_ui_strings
    checks): the first's are read."""
    text = path.read_text(encoding="utf-8")
    m = re.search(r"const uint8_t \w+\[\d+\] = \{", text)
    if not m:
        raise Bad(f"{path.name}: no VLW font")
    head = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", text[m.end():m.end() + 6 * 24]))
    count = struct.unpack(">I", head[:4])[0]
    need = 24 + 28 * count
    raw = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", text[m.end():m.end() + 7 * need]))[:need]
    return {struct.unpack(">I", raw[24 + 28 * i:28 + 28 * i])[0] for i in range(count)}


def store_codepoints(path=STORE):
    """The code points of the CJK glyph store (tools/glyph_store.py's
    format: its PAGE and PMAP sections, a presence bit a code point)."""
    text = path.read_text(encoding="utf-8")
    m = re.search(r"const uint32_t \w+\[\d+\] = \{(.*?)\};", text, re.S)
    if not m:
        raise Bad(f"{path.name}: no glyph store")
    words = [int(w, 16) for w in re.findall(r"0x([0-9a-f]{8})", m.group(1))]
    store = struct.pack(f"<{len(words)}I", *words)
    if store[:4] != b"GLYS":
        raise Bad(f"{path.name}: not a glyph store")
    nsec = struct.unpack_from("<H", store, 6)[0]
    npages = struct.unpack_from("<I", store, 24)[0]
    sec = {}
    for i in range(nsec):
        tag, at, length = struct.unpack_from("<4sII", store, 32 + 12 * i)
        sec[tag] = store[at:at + length]
    pages = struct.unpack(f"<{npages}H", sec[b"PAGE"])
    out = set()
    for p, page in enumerate(pages):
        bitmap = struct.unpack_from("<8I", sec[b"PMAP"], 48 * p)
        out |= {page << 8 | bit for bit in range(256) if bitmap[bit >> 5] >> (bit & 31) & 1}
    return out


def drawn(cp):
    """Whether the device draws `cp` as itself: in the VLW fonts, else in
    the CJK store (read only when a text needs it)."""
    if "vlw" not in _drawn:
        _drawn["vlw"] = vlw_codepoints()
    if cp in _drawn["vlw"]:
        return True
    if "store" not in _drawn:
        _drawn["store"] = store_codepoints()
    return cp in _drawn["store"]


def why_not_drawn(cp):
    """What the device does with a code point it can't draw as itself, and
    what to write instead."""
    ch = chr(cp)
    if 0xFF01 <= cp <= 0xFF5E:
        return (f"fullwidth {ch!r}: drawn as the ASCII {chr(cp - 0xFEE0)!r} (TextFold folds U+FF01-FF5E): write "
                "that instead, or a sign the CJK store has (、 。 「 」)")
    if 0xFF61 <= cp <= 0xFF9F:
        return f"halfwidth katakana {ch!r}: drawn as the fullwidth kana: write that"
    if cp == 0x2212:
        return "the minus sign: drawn as '-' (TextFold): write that"
    return (f"{ch!r} is in neither the fonts nor the CJK store (drawn as '?' or a look-alike): write another, or "
            "add it to the store (tools/glyph_store.py takes the characters of i18n/*.json; the font is needed)")


def check_glyphs(name, texts):
    """Every character of a catalogue's texts ((id, text) pairs) drawn as
    itself (composed, as TextFit draws: NFC): one failure naming each code
    point the device can't draw, and the ids that use it."""
    missing = {}
    for mid, text in texts:
        for ch in unicodedata.normalize("NFC", text):
            if ch != "\n" and not drawn(ord(ch)):
                missing.setdefault(ord(ch), []).append(mid)
    if missing:
        lines = [f"U+{cp:04X} {why_not_drawn(cp)}; in {', '.join(sorted(set(ids)))}"
                 for cp, ids in sorted(missing.items())]
        raise Bad(f"{name}: {len(missing)} character(s) the device can't draw:\n  " + "\n  ".join(lines))


def load(path):
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f, object_pairs_hook=pairs)
    except UnicodeDecodeError as e:
        raise Bad(f"{path.name}: not UTF-8 ({e})") from None
    except (json.JSONDecodeError, Bad) as e:
        raise Bad(f"{path.name}: {e}") from None
    if not isinstance(data, dict):
        raise Bad(f"{path.name}: not an object")
    return data


def pairs(items):
    d = {}
    for k, v in items:
        if k in d:
            raise Bad(f"key {k!r} twice")
        d[k] = v
    return d


def header(where, data, tag):
    if data.get("@@locale") != tag:
        raise Bad(f"{where}: @@locale is {data.get('@@locale')!r}, the file says {tag!r}")
    if not isinstance(data.get("@@autonym"), str) or not data["@@autonym"]:
        raise Bad(f"{where}: no @@autonym (the language's name in itself)")
    if data["@@autonym"] != AUTONYMS.get(tag):
        raise Bad(f"{where}: @@autonym is {data['@@autonym']!r}; {tag}'s name in itself is {AUTONYMS.get(tag)!r}")


def rooms_of(meta):
    """A message's rooms: none, one, or a list (every place it is drawn)."""
    room = meta.get("room")
    if room is None:
        return []
    return room if isinstance(room, list) else [room]


def max_lines(meta):
    """The lines a message may have: its rooms' fewest (line breaks only
    where every room places its lines one by one)."""
    lines = [r.get("lines", 1) if r.get("layout", "line") == "line" else 1 for r in rooms_of(meta)]
    return min(lines) if lines else 1


class Message:
    def __init__(self, mid, value, meta):
        self.id = mid
        self.value = value
        self.meta = meta
        self.plural = isinstance(value, dict)

    def forms(self):
        return self.value if self.plural else {"": self.value}

    def all_placeholders(self):
        s = set()
        for t in self.forms().values():
            s |= placeholders(t)
        return s


def check_english(data):
    """en.json: the messages and their metadata, in file order."""
    header("en.json", data, "en")
    msgs = {}
    order = []
    for key, value in data.items():
        if key.startswith("@@"):
            continue
        if key.startswith("@"):
            if key[1:] not in data:
                raise Bad(f"en.json: {key} describes no message")
            continue
        where = f"en.json {key}"
        if not ID_RE.match(key):
            raise Bad(f"{where}: an id is <screen>.<snake_case>")
        meta = data.get("@" + key)
        if not isinstance(meta, dict):
            raise Bad(f"{where}: no @{key} (its context and room)")
        unknown = set(meta) - META_KEYS
        if unknown:
            raise Bad(f"{where}: unknown metadata {sorted(unknown)}")
        if not isinstance(meta.get("context"), str) or not meta["context"]:
            raise Bad(f"{where}: no context")
        room = meta.get("room", "missing")
        if isinstance(room, list):
            if not room:
                raise Bad(f"{where}: an empty list of rooms")
            for r in room:
                check_room(where, r)
        else:
            check_room(where, room)
        if meta.get("translate", "yes") not in TRANSLATE:
            raise Bad(f"{where}: translate is one of {sorted(TRANSLATE)}")
        for k in ("bytes", "line_bytes"):
            if k in meta and (not isinstance(meta[k], int) or isinstance(meta[k], bool) or meta[k] < 2):
                raise Bad(f"{where}: {k} is a buffer's size (a whole number, 2 or more)")
        m = Message(key, value, meta)
        lines = max_lines(meta)
        if m.plural:
            if set(value) != {"one", "other"}:
                raise Bad(f"{where}: English's forms are one and other")
            for c, t in value.items():
                check_text(f"{where} [{c}]", t, lines)
                check_bytes(f"{where} [{c}]", t, meta)
        else:
            check_text(where, value, lines)
            check_bytes(where, value, meta)
        used = m.all_placeholders()
        phs = meta.get("placeholders", {})
        if not isinstance(phs, dict):
            raise Bad(f"{where}: placeholders is an object")
        described = set(int(k) for k in phs)
        if used != described:
            raise Bad(f"{where}: placeholders used {sorted(used)}, described {sorted(described)}")
        if used and max(used) + 1 != len(used):
            raise Bad(f"{where}: placeholders are {{0}} on, none skipped")
        for k, ph in phs.items():
            check_placeholder(f"{where} {{{k}}}", ph)
        count = meta.get("count")
        if m.plural:
            if count is not None and int(count) not in used:
                raise Bad(f"{where}: its count {{{count}}} isn't a placeholder")
            if placeholders(value["other"]) != used:
                raise Bad(f"{where}: the other form has every placeholder")
        elif count is not None:
            raise Bad(f"{where}: a count, and it isn't plural")
        mkey = m.id.replace(".", "_")
        if any(o.replace(".", "_") == mkey for o in order):
            raise Bad(f"{where}: its C++ name {mkey} is another id's")
        msgs[key] = m
        order.append(key)
    for key, meta in data.items():
        if key.startswith("@") and not key.startswith("@@") and not isinstance(meta, dict):
            raise Bad(f"en.json: {key} is an object")
    for m in msgs.values():
        for k, ph in m.meta.get("placeholders", {}).items():
            if ph.get("type") == "message" and ph.get("worst") not in msgs:
                raise Bad(f"en.json {m.id} {{{k}}}: worst names no message")
            for other in ph.get("or", []):
                if other not in msgs:
                    raise Bad(f"en.json {m.id} {{{k}}}: or names {other!r}, no message")
                if other == m.id:
                    raise Bad(f"en.json {m.id} {{{k}}}: or names the message itself")
    check_glyphs("en.json", [(mid, t) for mid in order for t in msgs[mid].forms().values()])
    return msgs, order


def check_room(where, room):
    if room is None:
        return  # a fragment another message holds: no room of its own
    if isinstance(room, str) or not isinstance(room, dict):
        raise Bad(f"{where}: no room (null for a fragment that has none)")
    unknown = set(room) - ROOM_KEYS
    if unknown:
        raise Bad(f"{where}: unknown room keys {sorted(unknown)}")
    if room.get("font") not in FONTS:
        raise Bad(f"{where}: room font is one of {FONTS}")
    if "else" in room and room["else"] not in FONTS:
        raise Bad(f"{where}: room else is one of {FONTS}")
    if not isinstance(room.get("px"), int) or room["px"] <= 0:
        raise Bad(f"{where}: room px")
    if not isinstance(room.get("lines", 1), int) or room.get("lines", 1) < 1:
        raise Bad(f"{where}: room lines")
    if room.get("layout", "line") not in LAYOUTS:
        raise Bad(f"{where}: room layout is one of {LAYOUTS}")
    if room.get("layout") == "toast" and not isinstance(room.get("two_line_px"), int):
        raise Bad(f"{where}: a toast's room has two_line_px")


def check_placeholder(where, ph):
    if not isinstance(ph, dict):
        raise Bad(f"{where}: an object")
    unknown = set(ph) - PH_KEYS
    if unknown:
        raise Bad(f"{where}: unknown keys {sorted(unknown)}")
    if not isinstance(ph.get("means"), str):
        raise Bad(f"{where}: means")
    if ph.get("type") not in FILLS:
        raise Bad(f"{where}: type is one of {FILLS}")
    w = ph.get("worst")
    if ph["type"] in ("grouped", "digits") and not isinstance(w, int):
        raise Bad(f"{where}: worst is a whole number")
    if ph["type"] == "decimal" and not isinstance(w, (int, float)):
        raise Bad(f"{where}: worst is a number")
    if ph["type"] in ("text", "message") and not isinstance(w, str):
        raise Bad(f"{where}: worst is a string")
    if "or" in ph:
        alts = ph["or"]
        if ph["type"] not in ("text", "message"):
            raise Bad(f"{where}: or is for a text or a message")
        if not isinstance(alts, list) or not alts or not all(isinstance(a, str) for a in alts):
            raise Bad(f"{where}: or is a list of message ids")
        if len(alts) > MAX_OR or len(set(alts)) != len(alts) or w in alts:
            raise Bad(f"{where}: or holds up to {MAX_OR} other messages, each once")


def check_translation(tag, data, english, overflows=None):
    """A translation: its messages, checked against English's. The ids its
    "@<id>" marks "overflow" go into `overflows` (a set), if given."""
    where0 = f"{tag}.json"
    header(where0, data, tag)
    have, need = (set(s.split()) for s in LANGS[tag])
    out = {}
    for key, value in data.items():
        if key.startswith("@@"):
            continue
        if key.startswith("@"):
            if key[1:] not in english:
                raise Bad(f"{where0}: {key} describes no message")
            if not isinstance(value, dict) or set(value) - {"note", "overflow"}:
                raise Bad(f"{where0}: {key} holds a note and an overflow mark only")
            if "overflow" in value:
                if value["overflow"] is not True:
                    raise Bad(f"{where0}: {key}'s overflow is true, or left out")
                if key[1:] not in data:
                    raise Bad(f"{where0}: {key} marks an overflow, and the file hasn't the text")
                if overflows is not None:
                    overflows.add(key[1:])
            continue
        where = f"{where0} {key}"
        en = english.get(key)
        if en is None:
            raise Bad(f"{where}: no such id in en.json")
        lines = max_lines(en.meta)
        want = en.all_placeholders()
        count = en.meta.get("count")
        if en.plural:
            if not isinstance(value, dict):
                raise Bad(f"{where}: plural in English, so forms by category")
            extra = set(value) - have
            if extra:
                raise Bad(f"{where}: {sorted(extra)} isn't a category of {tag} ({' '.join(sorted(have))})")
            missing = need - set(value)
            if missing:
                raise Bad(f"{where}: {sorted(missing)} missing")
            for c, t in value.items():
                check_text(f"{where} [{c}]", t, lines)
                got = placeholders(t)
                ok = got == want or (count is not None and (tag, c) in ONLY_ONE and got == want - {int(count)})
                if not ok:
                    raise Bad(f"{where} [{c}]: placeholders {sorted(got)}, English has {sorted(want)}")
                check_bytes(f"{where} [{c}]", t, en.meta)
        else:
            if isinstance(value, dict):
                raise Bad(f"{where}: not plural in English, so a string")
            check_text(where, value, lines)
            if placeholders(value) != want:
                raise Bad(f"{where}: placeholders {sorted(placeholders(value))}, English has {sorted(want)}")
            check_bytes(where, value, en.meta)
        out[key] = value
    texts = [(mid, t) for mid, v in out.items() for t in (v.values() if isinstance(v, dict) else [v])]
    check_glyphs(where0, texts + [("@@autonym", data["@@autonym"])])
    return out


# ---- the pseudo-locale ----

ACCENTS = dict(zip("acdeghijklnorstuwyzACDEGHIJKLNORSTUWYZ",
                   "áçďéĝĥíĵķĺñöŕšţüŵýžÅÇĎÉĜĤÍĴĶĹÑÖŔŠŢÜŴÝŽ"))
FILLER = "lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod tempor "


def pseudo_line(text):
    """[Ţĥé ţĥŕéé ŕéð ðöţš ĺöŕéɱ íƥšüɱ]: accented, a third longer, in
    brackets; placeholders kept."""
    parts = PH_RE.split(text)
    out, n = [], 0
    for i, p in enumerate(parts):
        if i % 2:
            out.append("{" + p + "}")
        else:
            out.append("".join(ACCENTS.get(c, c) for c in p))
            n += len(p)
    extra = math.ceil(n * 0.35)
    filler = "".join(ACCENTS.get(c, c) for c in (FILLER * 4)[:extra]).rstrip()
    return "[" + "".join(out) + (" " + filler if filler else "") + "]"


def pseudo(text):
    return "\n".join(pseudo_line(t) for t in text.split("\n"))


# ---- the tables ----

def forms_of(tag, msg_value, plural):
    """A message's bytes as stored: its forms, each with a NUL, in the
    order of the language's categories (an empty one: left out)."""
    if not plural:
        return msg_value.encode("utf-8") + b"\0"
    cats = [c for c in CATEGORIES if c in LANGS.get(tag, LANGS["en"])[0].split()]
    return b"".join(msg_value.get(c, "").encode("utf-8") + b"\0" for c in cats)


def build_table(tag, messages, english, order):
    blob = bytearray()
    seen = {}
    offs = []
    for mid in order:
        if mid not in messages:
            offs.append(0xFFFF)
            continue
        b = forms_of(tag, messages[mid], english[mid].plural)
        if b not in seen:
            seen[b] = len(blob)
            blob += b
        offs.append(seen[b])
    if len(blob) >= 0xFFFF:
        raise Bad(f"{tag}: {len(blob)} bytes of messages, over the 16-bit offsets")
    return bytes(blob), offs


def c_bytes(b):
    """A C string literal of bytes (with the NULs as \\0), lines of ~100.
    A hex escape followed by a hex digit, or \\0 by an octal one, would read
    on into it: the literal is split there."""
    lines, cur = [], ""
    prev = None  # "hex" or "nul" after such an escape
    for byte in b:
        kind = None
        if byte == 0:
            s, kind = "\\0", "nul"
        elif byte == 0x22:
            s = '\\"'
        elif byte == 0x5C:
            s = "\\\\"
        elif byte == 0x0A:
            s = "\\n"
        elif 0x20 <= byte < 0x7F:
            s = chr(byte)
            if (prev == "hex" and s in "0123456789abcdefABCDEF") or (prev == "nul" and s in "01234567"):
                s = '" "' + s
            if s == "?" and cur.endswith("?"):
                s = '" "?'  # no trigraphs
        else:
            s, kind = f"\\x{byte:02X}", "hex"
        cur += s
        prev = kind
        if byte == 0 and len(cur) > 90:
            lines.append(f'    "{cur}"')
            cur = ""
            prev = None
    if cur:
        lines.append(f'    "{cur}"')
    return "\n".join(lines) if lines else '    ""'


def cname(mid):
    return mid.replace(".", "_")


def lang_enum(tag):
    return "".join(p.capitalize() if p.islower() else p for p in tag.split("-"))


LICENCE = "// SPDX-License-Identifier: GPL-3.0-or-later\n// Copyright (C) 2026 IrosTheBeggar\n"


def gen_ids(order, english, langs):
    o = [LICENCE, "// Generated by tools/gen_ui_strings.py from i18n/*.json: don't edit.\n",
         "#pragma once\n#include <cstdint>\n\nnamespace uistr {\n\n",
         "// The UI's messages, in i18n/en.json's order (the English after each).\n",
         "enum class S : uint16_t {\n"]
    for mid in order:
        en = english[mid]
        text = en.value["other"] if en.plural else en.value
        shown = text.replace("\n", "\\n").replace("*/", "* /")
        if len(shown) > 70:
            shown = shown[:67] + "..."
        o.append(f"  {cname(mid)},  // {shown}\n")
    o.append("};\n")
    o.append(f"inline constexpr unsigned kStrings = {len(order)};\n\n")
    o.append("// The languages, English first: the mobile app's ten (one without a\n")
    o.append("// catalogue yet draws English), and the pseudo-locale where UISTR_PSEUDO\n")
    o.append("// is defined.\n")
    o.append("enum class Lang : uint8_t {\n")
    for tag in ORDER:
        o.append(f"  {lang_enum(tag)},  // {tag}\n")
    o.append("#ifdef UISTR_PSEUDO\n")
    o.append(f"  {lang_enum(PSEUDO)},  // {PSEUDO}\n")
    o.append("#endif\n};\n")
    o.append("#ifdef UISTR_PSEUDO\n")
    o.append(f"inline constexpr unsigned kLangs = {len(ORDER) + 1};\n")
    o.append("#else\n")
    o.append(f"inline constexpr unsigned kLangs = {len(ORDER)};\n")
    o.append("#endif\n\n}  // namespace uistr\n")
    return "".join(o)


def gen_data(order, english, tables, pseudo_table):
    o = [LICENCE, "// Generated by tools/gen_ui_strings.py from i18n/*.json: don't edit.\n",
         "// The messages of each language: a blob of UTF-8 (NUL after each form, each\n",
         "// text once) and an offset per id (0xFFFF: none, English's is used).\n\n",
         '#include "UiStrings.h"\n\nnamespace uistr {\nnamespace detail {\n\nnamespace {\n\n']

    def one(tag, blob, offs):
        v = cname(tag.replace("-", "_"))
        o.append(f"// {tag}: {len(blob)} bytes, {sum(1 for x in offs if x != 0xFFFF)} of {len(offs)} messages.\n")
        o.append(f"const char kBlob_{v}[] =\n{c_bytes(blob)};\n")
        o.append(f"const uint16_t kOff_{v}[kStrings] = {{\n")
        for i in range(0, len(offs), 12):
            o.append("    " + ", ".join(str(x) for x in offs[i:i + 12]) + ",\n")
        o.append("};\n\n")

    for tag, (blob, offs) in tables.items():
        one(tag, blob, offs)
    o.append("#ifdef UISTR_PSEUDO\n")
    one(PSEUDO, *pseudo_table)
    o.append("#endif\n\n}  // namespace\n\n")
    o.append("// A language with no catalogue yet has no blob: every message English's.\n")
    o.append("const Table kTables[kLangs] = {\n")
    for tag in ORDER:
        v = cname(tag.replace("-", "_"))
        if tag in tables:
            o.append(f'    {{"{tag}", "{c_escape(AUTONYMS[tag])}", kBlob_{v}, kOff_{v}}},\n')
        else:
            o.append(f'    {{"{tag}", "{c_escape(AUTONYMS[tag])}", nullptr, nullptr}},\n')
    o.append("#ifdef UISTR_PSEUDO\n")
    o.append(f'    {{"{PSEUDO}", "{c_escape(AUTONYMS[PSEUDO])}", kBlob_en_XA, kOff_en_XA}},\n')
    o.append("#endif\n};\n\n")
    bits = [0] * ((len(order) + 7) // 8)
    for i, mid in enumerate(order):
        if english[mid].plural:
            bits[i // 8] |= 1 << (i % 8)
    o.append("const uint8_t kPluralBits[(kStrings + 7) / 8] = {\n")
    for i in range(0, len(bits), 16):
        o.append("    " + ", ".join(f"0x{b:02X}" for b in bits[i:i + 16]) + ",\n")
    o.append("};\n\n}  // namespace detail\n}  // namespace uistr\n")
    return "".join(o)


def c_escape(s):
    out = ""
    prev_hex = False
    for byte in s.encode("utf-8"):
        if byte in (0x22, 0x5C):
            out += "\\" + chr(byte)
            prev_hex = False
        elif byte == 0x0A:
            out += "\\n"
            prev_hex = False
        elif 0x20 <= byte < 0x7F:
            c = chr(byte)
            out += ('" "' + c) if prev_hex and c in "0123456789abcdefABCDEF" else c
            prev_hex = False
        else:
            out += f"\\x{byte:02X}"
            prev_hex = True
    return out


def gen_test(order, english, langs, overflows):
    o = [LICENCE, "// Generated by tools/gen_ui_strings.py from i18n/en.json: don't edit.\n",
         "// The catalogue as test_ui_strings checks it: each message's English and\n",
         "// room (the font, the pixels, the lines; what fills its placeholders at\n",
         "// worst), and the plural categories the generator holds each language to.\n",
         "#pragma once\n#include <cstdint>\n\n#include \"UiStrings.h\"\n\nnamespace catalogue {\n\n",
         "enum class Font : uint8_t { None, Body, Small, Bold, Title };\n",
         "enum class Layout : uint8_t { Line, Wrap, Toast };\n",
         "enum class Fill : uint8_t { None, Grouped, Digits, Decimal, Text, Message };\n",
         "struct Placeholder {\n  Fill type;\n  const char* text;  // Text\n  double value;      // Grouped, Digits, Decimal\n",
         "  int places;        // Decimal\n  uistr::S message;  // Message\n",
         f"  int alts;          // \"or\": the other messages that may fill it\n  uistr::S alt[{MAX_OR}];\n}};\n",
         "struct Entry {\n  uistr::S id;\n  const char* name;\n  bool plural;\n",
         "  const char* one;    // the English (a plural's one form)\n  const char* other;  // a plural's other form\n",
         "  Font font, elseFont;  // None: no room of its own (a fragment)\n  int px, lines;\n",
         "  Layout layout;\n  int twoLinePx;  // a toast's lines when it splits at \": \"\n",
         "  int count;  // the placeholder that is the count (-1: none)\n",
         "  bool knownOverflow;  // too wide in English already (\"overflow\" in en.json)\n",
         "  bool optional;  // drawn only when it fits (another message else): never an overflow\n",
         "  bool dev;  // \"translate\": \"dev\"\n",
         "  int bytes, lineBytes;  // its buffers on the device, a NUL included (0: none)\n",
         "  Placeholder ph[4];\n};\n\n"]

    def lit(s):
        return '"' + c_escape(s) + '"'

    o.append("inline const Entry kEntries[] = {\n")
    for mid in order:
        m = english[mid]
        one_t = m.value["one"] if m.plural else m.value
        other_t = m.value["other"] if m.plural else ""
        count = m.meta.get("count")
        phs = []
        for k in range(4):
            ph = m.meta.get("placeholders", {}).get(str(k))
            if not ph:
                phs.append("{Fill::None, nullptr, 0, 0, uistr::S{}, 0, {}}")
                continue
            t = ph["type"]
            alts = ph.get("or", [])
            alt = f"{len(alts)}, {{{', '.join('uistr::S::' + cname(a) for a in alts)}}}"
            if t == "text":
                phs.append(f"{{Fill::Text, {lit(ph['worst'])}, 0, 0, uistr::S{{}}, {alt}}}")
            elif t == "message":
                phs.append(f"{{Fill::Message, nullptr, 0, 0, uistr::S::{cname(ph['worst'])}, {alt}}}")
            else:
                phs.append(f"{{Fill::{t.capitalize()}, nullptr, {ph['worst']}, {ph.get('places', 0)}, uistr::S{{}}, "
                           f"{alt}}}")
        # An entry a room: a message drawn in several places is measured in each.
        for room in rooms_of(m.meta) or [{}]:
            font = "Font::" + room["font"] if room else "Font::None"
            elsef = "Font::" + room["else"] if room.get("else") else "Font::None"
            layout = "Layout::" + room.get("layout", "line").capitalize()
            o.append(f"    {{uistr::S::{cname(mid)}, {lit(mid)}, {'true' if m.plural else 'false'}, {lit(one_t)}, "
                     f"{lit(other_t)}, {font}, {elsef}, {room.get('px', 0)}, {room.get('lines', 1)}, {layout}, "
                     f"{room.get('two_line_px', 0)}, {int(count) if count is not None else -1}, "
                     f"{'true' if m.meta.get('overflow') else 'false'}, "
                     f"{'true' if room.get('optional') else 'false'}, "
                     f"{'true' if m.meta.get('translate') == 'dev' else 'false'}, "
                     f"{m.meta.get('bytes', 0)}, {m.meta.get('line_bytes', 0)},\n"
                     f"     {{{', '.join(phs)}}}}},\n")
    o.append("};\n\n")
    o.append("// Each language's categories (bit 1 << uistr::Plural), as the generator\n")
    o.append("// checks the catalogues against them: uistr::pluralSet() must agree.\n")
    o.append("struct LangSet {\n  const char* tag;\n  uint8_t set;\n};\n")
    o.append("inline constexpr LangSet kLangSets[] = {\n")
    for tag in ORDER + [PSEUDO]:
        cats = LANGS.get(tag, LANGS["en"])[0].split()
        mask = sum(1 << CATEGORIES.index(c) for c in cats)
        o.append(f'    {{"{tag}", 0x{mask:02X}}},\n')
    o.append("};\n\n")
    o.append("// The languages with a catalogue (English, then each translation), and the\n")
    o.append("// texts a translation marks \"overflow\" (too wide in it until a layout\n")
    o.append("// fix: the width test lets them be, and fails a mark that fits).\n")
    tags = ", ".join('"' + t + '"' for t in langs)
    o.append(f"inline constexpr const char* kCatalogues[] = {{{tags}}};\n")
    o.append("struct LangOverflow {\n  const char* tag;\n  uistr::S id;\n};\n")
    o.append("inline const LangOverflow kLangOverflows[] = {\n")
    marks = [(t, mid) for t in langs[1:] for mid in order if mid in overflows.get(t, set())]
    for t, mid in marks:
        o.append(f'    {{"{t}", uistr::S::{cname(mid)}}},\n')
    if not marks:
        o.append('    {"", uistr::S{}},  // (none yet)\n')
    o.append("};\n\n")
    # The constants a room says it is (uitext's): the test checks each.
    consts = [(mid, r["const"], r["px"]) for mid in order for r in rooms_of(english[mid].meta) if r.get("const")]
    o.append("// Rooms that are a uitext constant (\"const\" in en.json): the same number.\n")
    o.append("struct Const {\n  const char* name;\n  int px;\n  int code;\n};\n")
    o.append("inline const Const kConsts[] = {\n")
    for mid, c, px in consts:
        o.append(f'    {{"{mid}", {px}, {c}}},\n')
    if not consts:
        o.append('    {"", 0, 0},\n')
    o.append("};\n\n}  // namespace catalogue\n")
    return "".join(o)


def load_all():
    if not (CATALOGUE / "en.json").exists():
        raise Bad("no i18n/en.json")
    en_data = load(CATALOGUE / "en.json")
    english, order = check_english(en_data)
    translations = {}
    overflows = {}
    for path in sorted(CATALOGUE.glob("*.json")):
        tag = path.stem
        if tag == "en":
            continue
        if tag not in LANGS:
            raise Bad(f"{path.name}: {tag} isn't one of the languages ({', '.join(ORDER)})")
        data = load(path)
        overflows[tag] = set()
        translations[tag] = check_translation(tag, data, english, overflows[tag])
    return english, order, translations, overflows


def generate():
    english, order, translations, overflows = load_all()
    langs = ["en"] + [t for t in ORDER if t in translations]
    tables = {}
    en_msgs = {mid: english[mid].value for mid in order}
    tables["en"] = build_table("en", en_msgs, english, order)
    for tag in langs[1:]:
        tables[tag] = build_table(tag, translations[tag], english, order)
    ps = {}
    for mid in order:
        v = english[mid].value
        ps[mid] = {c: pseudo(t) for c, t in v.items()} if english[mid].plural else pseudo(v)
    pseudo_table = build_table(PSEUDO, ps, english, order)
    return {
        OUT_IDS: gen_ids(order, english, langs),
        OUT_DATA: gen_data(order, english, tables, pseudo_table),
        OUT_TEST: gen_test(order, english, langs, overflows),
    }, english, order, translations, tables


def report(english, order, translations):
    total = len(order)
    yes = [m for m in order if english[m].meta.get("translate", "yes") != "dev"]
    print(f"en: {total} messages ({total - len(yes)} developer-only)")
    for tag in ORDER[1:]:
        if tag not in translations:
            print(f"{tag}: no catalogue (English everywhere)")
            continue
        got = translations[tag]
        missing = [m for m in yes if m not in got]
        print(f"{tag}: {len(got)} of {total}; missing {len(missing)}" + (f" ({', '.join(missing[:8])}...)" if missing else ""))


def dump(tag, english, order, translations):
    for mid in order:
        if tag == "en":
            v = english[mid].value
        elif tag == PSEUDO:
            v = english[mid].value
            v = {c: pseudo(t) for c, t in v.items()} if isinstance(v, dict) else pseudo(v)
        else:
            v = translations.get(tag, {}).get(mid)
            if v is None:
                continue
        if isinstance(v, dict):
            for c in CATEGORIES:
                if c in v:
                    print(f"{mid}[{c}]\t{json.dumps(v[c], ensure_ascii=False)}")
        else:
            print(f"{mid}\t{json.dumps(v, ensure_ascii=False)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true", help="fail if a generated file is out of date")
    ap.add_argument("--report", action="store_true", help="each language's coverage")
    ap.add_argument("--dump", metavar="TAG", help="print every message of a language")
    args = ap.parse_args()
    for stream in (sys.stdout, sys.stderr):  # (the code points' characters, on any console)
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    try:
        files, english, order, translations, tables = generate()
    except Bad as e:
        print(f"gen_ui_strings: {e}", file=sys.stderr)
        return 1
    if args.report:
        report(english, order, translations)
        return 0
    if args.dump:
        dump(args.dump, english, order, translations)
        return 0
    stale = []
    for path, text in files.items():
        old = path.read_text(encoding="utf-8") if path.exists() else None
        if old != text:
            stale.append(path)
            if not args.check:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding="utf-8", newline="\n")
    if args.check:
        for p in stale:
            print(f"gen_ui_strings: {p.relative_to(ROOT)} is out of date", file=sys.stderr)
        return 1 if stale else 0
    sizes = ", ".join(f"{t} {len(b)} B" for t, (b, _) in tables.items())
    print(f"{len(order)} messages; {sizes}; " + (", ".join(str(p.relative_to(ROOT)) for p in stale) or "nothing changed"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
