#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Compiles a dancer (dancer.ini plus a PNG sheet of frames) into an MPDN v1
pack for the player's Dance tab.

    python tools/dancepack.py check DIR [--strict]        # validate, report, write nothing
    python tools/dancepack.py build DIR [-o FILE] [--strict]
    python tools/dancepack.py info FILE                   # read a .dnc back
    python tools/dancepack.py builtin [DIR --out FILE]    # the built-in packs and vectors
    python tools/dancepack.py --check                     # exit 1 if they are stale

DIR is a dancer folder: dancer.ini and its sheet. Its name is the pack's id.
`build` writes <id>.dnc in the current directory unless -o names a file;
copy it to /dancers/ on the player's card. `check` prints, per move, a
status line with its budget light, a table of its steps and a coverage
report (which pace each move is danced at for each dance tempo, and which
moves fit at the stage's edge spots). `--strict` turns a skipped move into a
failure and holds the licence to the built-in allowlist.

`builtin` and `--check` need the repository: they regenerate the registry
(BUILTINS: the test dancer's pack test/support/dance/TestPack.h) and the
shared test vectors (EligibilityVectors.h, FoldVectors.h, BudgetVectors.h).
A registry folder may keep its sheet as text, sheet.txt, which `builtin`
renders to sheet.png and `--check` compares pixel for pixel. `--check` also
checks the embedded TEXTFOLD and FONT_COVERAGE tables against
lib/core/TextFold.cpp and src/ui/VlwFonts.cpp.

docs/DANCE-SPRITES.md is the specification: Appendix A the MPDN v1 bytes,
Appendix B the integer algorithms this file shares with the firmware, and
Appendix C this tool's contract (commands, messages, generated files and
the importable API). Standard library only, Python 3.8 or newer; every
computation is in integers, so the output is the same on every platform.
"""
import argparse
import bisect
import collections
import configparser
import difflib
import io
import math
import os
import re
import struct
import sys
import zlib
from fractions import Fraction
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# ---- the format and the stage (Appendix A, section 6)
FORMAT = 1
MIN_READER = 1
MAX_PACK = 131072
STAGE_W, STAGE_H, GROUND_Y = 320, 164, 153
SPOTS = (64, 160, 256)
EASES = ("hold", "linear", "in", "out", "inout")
LICENCE_ALLOWLIST = ("GPL-3.0-or-later", "GPL-2.0-or-later", "CC0-1.0", "CC-BY-4.0",
                     "CC-BY-SA-4.0", "MIT", "Apache-2.0")

# The registry: (dancer folder, generated file), relative to ROOT with '/'.
BUILTINS = [("tools/art/dancers/test", "test/support/dance/TestPack.h")]
TEST_DANCER = "tools/art/dancers/test"
ELIGIBILITY_H = "test/support/dance/EligibilityVectors.h"
FOLD_H = "test/support/dance/FoldVectors.h"
BUDGET_H = "test/support/dance/BudgetVectors.h"

MAX_FRAMES = 256
MAX_MOVES = 64
MAX_STEPS = 32
MAX_CHUNKS = 16
MAX_COLOURS = 15
MAX_POSE_KEYS = 255
MAX_CELLS = 256
MAX_SHEET = 1024
CHUNKS = ("META", "GEOM", "PALS", "THMB", "FTAB", "STRS", "MOVS", "STEP", "FDAT")

BEAT, IDLE = 0, 1
FLAG_MIRROR, FLAG_FREEZE, FLAG_PLACE_START, FLAG_PLACE_END, FLAG_HEAVY, FLAG_SNAP = 1, 2, 4, 8, 16, 32
NEEDS_SONG_BPM = 1
WHEN = ("", "quiet", "listening", "any")           # 1, 2, 3
START_AT = ("any", "centre", "side", "left", "right")
FACES = ("none", "right", "left")
GREEN_PX, AMBER_PX = 16000, 20000
FPS_PERIODS = ((24, 42), (30, 33))                  # dancerate::periodMs
BACKGROUND = (0x08, 0x0C, 0x10)                    # 0x0862
TOAST_ROWS = 36                                     # stage rows 0-35 sit under a toast
RUNS_WARN = 300

DANCER_KEYS = ("name", "author", "license", "sheet", "cell", "scale", "anchor", "rest", "shadow",
               "format", "key", "palette", "idle_palette", "faces", "url", "version")
MOVE_KEYS = ("row", "frames", "beats", "bpm", "timing", "offset", "ease", "mirror", "place",
             "weight", "from", "to", "repeat", "freeze", "heavy", "snap", "sway", "bounce", "travel",
             "travel_ease", "start_at", "go", "genre", "when", "song_bpm")
BEAT_ONLY = ("beats", "place", "repeat", "freeze", "sway", "bounce", "travel", "travel_ease",
             "start_at", "go", "genre", "song_bpm")
NEED_TRAVEL = ("travel_ease", "start_at", "go")
BOOLEANS = {"1": True, "yes": True, "true": True, "on": True,
            "0": False, "no": False, "false": False, "off": False}

_ID = re.compile(r"[a-z0-9][a-z0-9-]{0,22}")       # pack ids, move names and pose keys
# C.3's numbers: any leading zeros, then at most 9 digits (so int() never meets a
# string long enough for Python's int-string limit, and every version agrees);
# a decimal's part after the point is at most 9 digits.
_N = r"(?:0*[1-9][0-9]{0,8}|0+)"
_FRAC = r"(?:" + _N + r")?\.[0-9]{1,9}"
_INT = re.compile(_N)
_SINT = re.compile(r"[+-]?" + _N)
_RANGE = re.compile(r"(" + _N + r")\s*-\s*(" + _N + r")")
_HEX = re.compile(r"#[0-9a-fA-F]{6}")
_CELL = re.compile(r"([A-Za-z]+)([0-9]+)")
_BEAT_TIME = re.compile(_N + "|" + _FRAC + "|" + _N + "/" + _N)
_IDLE_TIME = re.compile(r"(" + _N + "|" + _FRAC + r")(s|ms)")
_LONG_BEAT_TIME = re.compile(r"[0-9]+|[0-9]*\.[0-9]+|[0-9]+/[0-9]+")   # the shapes, any length
_LONG_IDLE_TIME = re.compile(r"([0-9]+|[0-9]*\.[0-9]+)(s|ms)")
_CONTROL = re.compile("[\x00-\x1f\x7f-\x9f]")
_WORD = re.compile(r"[a-z0-9]+")
# A section header (C.3): configparser 3.10+'s rule on every version, `[`, then
# everything up to the line's last `]` (3.8 and 3.9 stop at the first `]`).
_SECTCRE = re.compile(r"\[(?P<header>.+)\]")
_COMMENT_IN_VALUE = re.compile(r"(?:^|(?<=\s))#(?=\s|$)", re.M)


# ---- records (Appendix C.9)
class DanceError(ValueError):
    """args[0] is a message of Appendix C.6 (without its location or
    "(skipped)"), or a reader's reason of Appendix A.15."""


Geom = collections.namedtuple(
    "Geom", "scale faces cell_w cell_h anchor_x anchor_y shadow_w frame_count rest_frame")
Box = collections.namedtuple("Box", "x0 y0 w h")
Step = collections.namedtuple("Step", "frame mirrored dur dx dy ease go")
Move = collections.namedtuple(
    "Move", "name kind beats pace_lo pace_hi weight10 when flags repeat needs travel start_at "
            "travel_ease bounce sway from_key to_key from_key_m to_key_m genres song_lo song_hi steps")
Pack = collections.namedtuple(
    "Pack", "size crc id name author licence url version declared_moves geom palette idle_palette "
            "thumb boxes grids pose_keys moves ignored")
Pose = collections.namedtuple("Pose", "frame mirrored root_x root_y box shadow rect")
Case = collections.namedtuple("Case", "fps pace mirrored spot frames median peak")
Budget = collections.namedtuple("Budget", "cases median peak light")
Result = collections.namedtuple("Result", "ok data pack messages report skipped budgets")


# ---- small helpers (Appendix B.0, B.9, B.10)
def fnv1a32(data):
    """FNV-1a 32 of bytes (B.10)."""
    h = 0x811C9DC5
    for b in bytes(data):
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def crc32(data):
    """zlib's CRC-32 (A.2)."""
    return zlib.crc32(bytes(data)) & 0xFFFFFFFF


def _rhu(a, b):
    """a / b rounded half up, b > 0 (B.0)."""
    return (2 * a + b) // (2 * b)


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def idle_colour(rgb):
    """The derived idle colour (B.9): half way to its luma, then x0.7."""
    r, g, b = rgb
    y = (77 * r + 150 * g + 29 * b + 128) >> 8
    return tuple((7 * ((c + y + 1) >> 1) + 5) // 10 for c in (r, g, b))


def near_background(rgb):
    """True when every channel is within 15 of the stage's #080C10 (B.9)."""
    return all(abs(c - k) < 16 for c, k in zip(rgb, BACKGROUND))


def _hex(rgb):
    return "#%02x%02x%02x" % tuple(rgb)


def _parse_hex(text):
    return int(text[1:3], 16), int(text[3:5], 16), int(text[5:7], 16)


def _n(n):
    """A count of px or bytes: commas from 1,000 (C.5)."""
    return f"{n:,}"


def _s(n, word):
    return f"{n} {word}" if n == 1 else f"{n} {word}s"


def _slash(path):
    return str(path).replace("\\", "/")


def _valid_text(text):
    return not _CONTROL.search(text)


def _esc(text):
    """Printed text with its C0, DEL and C1 control characters as \\xNN (C.5),
    so echoing dancer.ini or a path can't send escape sequences to a terminal."""
    return _CONTROL.sub(lambda m: "\\x%02x" % ord(m.group(0)), str(text))


def _int(text):
    """An integer C.3's grammar matched (_INT, _SINT, _RANGE...): its sign and
    leading zeros stripped first, so int() sees at most 9 digits."""
    t = text.strip()
    sign = -1 if t[:1] == "-" else 1
    return sign * int(t.lstrip("+-").lstrip("0") or "0")


def _fraction(text):
    """A timing number C.3's grammar matched: an integer, a decimal or a/b."""
    if "/" in text:
        a, b = text.split("/")
        return Fraction(_int(a), _int(b))
    if "." in text:
        a, b = text.split(".")
        return Fraction(_int(a or "0") * 10 ** len(b) + int(b), 10 ** len(b))
    return Fraction(_int(text))


def _strerror(e):
    return e.strerror or str(e)


def _string_ok(raw):
    """A string's bytes: strict UTF-8 with no control character (A.1)."""
    try:
        text = bytes(raw).decode("utf-8")
    except UnicodeDecodeError:
        return None
    return text if _valid_text(text) else None


def _covered(cp):
    i = bisect.bisect_right(_COVERAGE_FIRSTS, cp) - 1
    return i >= 0 and cp <= FONT_COVERAGE[i][1]


def _ucodes(cps):
    return ", ".join("U+%04X" % cp for cp in cps)


# ---- genre folding and pose keys (B.8, A.11)
def textfold_full(text):
    """textfold's Full folding: ASCII stays, the rest becomes its replacement."""
    return "".join(ch if ord(ch) < 0x80 else TEXTFOLD.get(ord(ch), "?") for ch in text)


def fold_genre(tag):
    """B.8: the folded key, "" when the tag folds to nothing."""
    s = textfold_full(tag)
    s = "".join(chr(ord(c) + 32) if "A" <= c <= "Z" else c for c in s)
    s = s.replace("&", " and ").replace("+", " and ")
    words = _WORD.findall(s)
    return "".join("and" if w == "n" and 0 < i < len(words) - 1 else w for i, w in enumerate(words))


def genre_key(tag):
    """The genre key, FNV-1a 32 of the folded tag, or None when it folds to nothing."""
    key = fold_genre(tag)
    return fnv1a32(key.encode("ascii")) if key else None


def pose_twin(key):
    """A pose key's mirror twin: left and right swapped in its '-'-parts."""
    return "-".join({"left": "right", "right": "left"}.get(p, p) for p in key.split("-"))


# ---- timing (B.1)
def parse_beat_time(text):
    """A beat move's timing value -> (ticks, snapped). Raises DanceError."""
    bad = f'timing "{text}" isn\'t a number of beats, like 1/4, 0.5 or 2'
    if not _BEAT_TIME.fullmatch(text):
        if _LONG_BEAT_TIME.fullmatch(text):
            raise DanceError(_too_long(text))
        raise DanceError(bad)
    try:
        value = _fraction(text)
    except ZeroDivisionError:
        raise DanceError(bad)
    t = 12 * value
    n = math.floor(t + Fraction(1, 2))
    if t == n:
        return n, False
    if abs(t - n) <= Fraction(1, 4):
        return n, True
    raise DanceError(f"timing {text} is off the 1/12-beat grid by more than 1/48 beat")


def _too_long(text):
    return f'timing "{text}" has a number of more than 9 digits'


def _idle_form(text):
    m = _IDLE_TIME.fullmatch(text.lower())     # the unit in any case, as C.3's words
    if not m:
        if _LONG_IDLE_TIME.fullmatch(text.lower()):
            raise DanceError(_too_long(text))
        raise DanceError(f'timing "{text}" needs s or ms, like 2s or 400ms')
    return m


def parse_idle_time(text):
    """A no-BPM move's timing value -> 10 ms units. Raises DanceError."""
    m = _idle_form(text)
    v = _fraction(m.group(1))
    ms = v * 1000 if m.group(2) == "s" else v
    if ms.denominator != 1 or ms.numerator % 10:
        raise DanceError(f"timing {text} isn't a whole number of 10 ms")
    return ms.numerator // 10


def beats_text(ticks):
    """Ticks as beats: '1 beat', '2 beats', '1/4 beat', '3 3/4 beats' (C.5)."""
    whole, rest = divmod(ticks, 12)
    if ticks == 12:
        return "1 beat"
    if rest == 0:
        return f"{whole} beats"
    f = Fraction(rest, 12)
    if whole == 0:
        return f"{f.numerator}/{f.denominator} beat"
    return f"{whole} {f.numerator}/{f.denominator} beats"


def even_split(frames, beats):
    """12*beats ticks shared evenly by the frames (a count or a list). Raises DanceError."""
    n = frames if isinstance(frames, int) else len(frames)
    total = 12 * beats
    if n <= 0 or total % n:
        raise DanceError(f"{n} frames can't share {beats_text(total)} evenly on the 1/12 grid: "
                         "give timing =")
    return [total // n] * n


# ---- motion (B.2, B.3)
def _ease_code(ease):
    return EASES.index(ease.lower()) if isinstance(ease, str) else int(ease)


def ease_q16(ease, f):
    """An ease (a code 0-4 or its name) at f, 0..65536 (Q16). hold is 0 until
    f reaches 65536 (B.2): a step never does, a finished travel does."""
    code = _ease_code(ease)
    if code == 0:
        return 65536 if f >= 65536 else 0
    if code == 1:
        return f
    if code == 2:
        return (f * f) >> 16
    if code == 3:
        g = 65536 - f
        return 65536 - ((g * g) >> 16)
    if code == 4:
        return (f * f * (196608 - 2 * f)) >> 32
    raise ValueError(f"no ease {ease}")


def hop_q16(phi):
    """The bounce's hop at phase phi of a move beat (Q16): the crab's curve."""
    if phi < 7864:
        return 0
    a = ((phi - 7864) * 65536) // 57672
    return (4 * a * (65536 - a)) >> 16


def sway_q16(beat, phi):
    """The sway at move beat `beat` (since the run started), phase phi (Q16)."""
    s = 1 if beat % 2 else -1
    return s * (65536 - 2 * ease_q16(4, phi))


# ---- pace (B.7)
def _x(t10, k):
    return t10 * 2 ** (k + 1)


def eligible_k(t10, lo, hi, prev_k, beats, travel=0):
    """The octave a move is danced at for dance tempo t10 (tenths of a BPM), or
    None. A travel move needs D = beats * 2^-k >= 2 (section 8), so it skips
    double time under 4 beats, as reach() does."""
    if lo == 0 and hi == 0:
        return 0
    order = []
    for k in ((prev_k,) if prev_k is not None else ()) + (0, -1, 1):
        if k not in order:
            order.append(k)
    for k in order:
        if k == -1 and beats > 4:
            continue
        if k == 1 and (t10 > 900 or (travel and beats < 4)):
            continue
        if 20 * lo <= _x(t10, k) <= 20 * hi:
            return k
    return None


def pace_holds(t10, k, lo, hi):
    """The running move's tolerance: its pace within 0.97*lo .. 1.03*hi, and
    double time only while T10 <= 900, as eligible_k: never past 180 (B.7)."""
    if lo == 0 and hi == 0:
        return True
    if k == 1 and t10 > 900:
        return False
    return 1940 * lo <= 100 * _x(t10, k) <= 2060 * hi


_BANDS = {-1: (36, 88), 0: (72, 176), 1: (144, 180)}


def _band(lo, hi, beats, travel):
    ranged = not (lo == 0 and hi == 0)
    ks = []
    if not (travel and beats < 2):
        ks.append(0)
    if ranged and beats <= 4:
        ks.append(-1)
    if ranged and (not travel or beats >= 4):
        ks.append(1)
    if not ks:
        return None
    return min(_BANDS[k][0] for k in ks), max(_BANDS[k][1] for k in ks)


def reach(lo, hi, beats, travel=0):
    """The paces a move can be danced at, (rlo, rhi), or None."""
    band = _band(lo, hi, beats, travel)
    if band is None:
        return None
    if lo == 0 and hi == 0:
        return band
    rlo, rhi = max(lo, band[0]), min(hi, band[1])
    return (rlo, rhi) if rlo <= rhi else None


def _reach_error(lo, hi, beats, travel):
    band = _band(lo, hi, beats, travel) or (72, 176)
    if lo > band[1]:
        return f"range {lo}-{hi} can't be reached: the fastest pace is {band[1]}"
    return f"range {lo}-{hi} can't be reached: the slowest pace is {band[0]}"


def _light_text(b):
    """A status line's light (C.5): `green: median 4 px, peak 6 of 16,000 px`."""
    if b.light == "red":
        return f"red (heavy): median {_n(b.median)} px, peak {_n(b.peak)} px, over {_n(AMBER_PX)}"
    limit = GREEN_PX if b.light == "green" else AMBER_PX
    return f"{b.light}: median {_n(b.median)} px, peak {_n(b.peak)} of {_n(limit)} px"


def _what(move):
    """A status line's move (C.5): `2 beats, pace 90-130` or `no BPM, when any, 4000 ms`."""
    if move.kind == BEAT:
        pace = "any" if (move.pace_lo, move.pace_hi) == (0, 0) else f"{move.pace_lo}-{move.pace_hi}"
        return f"{beats_text(12 * move.beats)}, pace {pace}"
    return f"no BPM, when {WHEN[move.when]}, {10 * sum(s.dur for s in move.steps)} ms"


# ---- frameAt, rects and the budget (B.4-B.6, B.11)
def _rect(r):
    return r if r is not None and r[2] > r[0] and r[3] > r[1] else None


def _union(a, b):
    a, b = _rect(a), _rect(b)
    if a is None:
        return b
    if b is None:
        return a
    return (min(a[0], b[0]), min(a[1], b[1]), max(a[2], b[2]), max(a[3], b[3]))


def _area(r):
    r = _rect(r)
    return 0 if r is None else (r[2] - r[0]) * (r[3] - r[1])


def dirty_px(prev_rect, rect):
    """The px a frame dirties (B.5): one union, or two rects when the union is
    more than 1.3x their sum. prev_rect None: area(rect)."""
    a, b = _rect(prev_rect), _rect(rect)
    u = _union(a, b)
    if 10 * _area(u) > 13 * (_area(a) + _area(b)):
        return _area(a) + _area(b)
    return _area(u)


def _spot_x(spot):
    if spot not in (-1, 0, 1):
        raise ValueError(f"no spot {spot}")
    return SPOTS[spot + 1]


def _starts(move):
    starts, acc = [], 0
    for s in move.steps:
        starts.append(acc)
        acc += s.dur
    return starts, acc


def frame_at(geom, boxes, move, pos16, mirrored, spot, e16=65536):
    """B.4: where move draws at pos16 (Q16 steps since its run started, >= 0)."""
    if pos16 < 0:
        raise ValueError(f"pos16 {pos16} is negative: B.4 needs pos16 >= 0")
    steps = move.steps
    n = len(steps)
    starts, total = _starts(move)
    period = total * 65536
    loop = pos16 // period
    q = pos16 - loop * period
    i = 0
    for k in range(n):
        if starts[k] * 65536 <= q:
            i = k
    st = steps[i]
    f = (q - starts[i] * 65536) // st.dur
    drawn = bool(st.mirrored) != bool(mirrored)
    nxt = steps[(i + 1) % n]
    e = ease_q16(st.ease, f)
    kx = st.dx * 65536 + (nxt.dx - st.dx) * e
    ky = st.dy * 65536 + (nxt.dy - st.dy) * e
    by = sx = 0
    if move.kind == BEAT:
        b = pos16 // 786432
        phi = (pos16 % 786432) // 12
        by = -move.bounce * hop_q16(phi)
        sx = move.sway * sway_q16(b, phi)
    sgn = -1 if mirrored else 1
    x16 = sgn * (kx + sx)
    y16 = ky + by
    scale = geom.scale
    if move.flags & FLAG_SNAP:
        X = scale * _rhu(e16 * x16, 1 << 32)
        Y = scale * _rhu(e16 * y16, 1 << 32)
    else:
        X = _rhu(scale * e16 * x16, 1 << 32)
        Y = _rhu(scale * e16 * y16, 1 << 32)
    start_x = _spot_x(spot)
    travel_px = 0
    if move.kind == BEAT and move.travel:
        end_x = _spot_x(spot + move.travel * sgn)
        go = [k for k in range(n) if steps[k].go] or list(range(n))
        gt = sum(steps[k].dur for k in go)
        if loop >= 1:
            p = 65536
        else:
            g = sum(steps[k].dur * 65536 for k in go if k < i)
            if i in go:
                g += q - starts[i] * 65536
            p = g // gt
        travel_px = _rhu((end_x - start_x) * ease_q16(move.travel_ease, p), 65536)
    root_x = start_x + travel_px + X
    root_y = GROUND_Y + Y
    box = boxes[st.frame]
    rect_box = None
    if box.w > 0 and box.h > 0:
        if drawn:
            left = root_x + (geom.anchor_x - box.x0 - box.w + 1) * scale
        else:
            left = root_x + (box.x0 - geom.anchor_x) * scale
        top = root_y + (box.y0 - geom.anchor_y) * scale
        right, bottom = left + box.w * scale, top + box.h * scale
        cx = -left if left < 0 else (STAGE_W - right if right > STAGE_W else 0)
        cy = -top if top < 0 else (STAGE_H - bottom if bottom > STAGE_H else 0)
        root_x += cx
        root_y += cy
        rect_box = (left + cx, top + cy, right + cx, bottom + cy)
    shadow = None
    sw = geom.shadow_w - 2 * max(0, -Y)
    if sw > 0:
        x0 = root_x - sw // 2
        shadow = _rect((max(x0, 0), GROUND_Y - 1, min(x0 + sw, STAGE_W), GROUND_Y + 2))
    return Pose(st.frame, drawn, root_x, root_y, rect_box, shadow, _union(rect_box, shadow))


def extent(geom, boxes, move, mirrored):
    """B.11: (lo, hi), the screen columns from the root x the move can draw
    in, or None when every box is empty."""
    sgn = -1 if mirrored else 1
    scale = geom.scale
    sway = move.sway if move.kind == BEAT else 0
    n = len(move.steps)
    lo = hi = None
    for i, st in enumerate(move.steps):
        box = boxes[st.frame]
        if box.w == 0 or box.h == 0:
            continue
        if bool(st.mirrored) != bool(mirrored):
            left = (geom.anchor_x - box.x0 - box.w + 1) * scale
        else:
            left = (box.x0 - geom.anchor_x) * scale
        right = left + box.w * scale
        a = sgn * st.dx * scale
        c = sgn * move.steps[(i + 1) % n].dx * scale
        l_i = left + min(a, c, 0) - sway * scale
        h_i = right + max(a, c, 0) + sway * scale
        lo = l_i if lo is None else min(lo, l_i)
        hi = h_i if hi is None else max(hi, h_i)
    return None if lo is None else (lo, hi)


def fits(geom, boxes, move, mirrored, spot):
    ext = extent(geom, boxes, move, mirrored)
    if ext is None:
        return True
    x = _spot_x(spot)
    return x + ext[0] >= 0 and x + ext[1] <= STAGE_W


def _start_code(start_at):
    return START_AT.index(start_at) if isinstance(start_at, str) else int(start_at)


def start_spots(geom, boxes, move, mirrored):
    """B.11: the spots a travel move can start from in an orientation. A move
    without travel can play at every spot: [-1, 0, 1]."""
    sgn = -1 if mirrored else 1
    t = move.travel * sgn if move.kind == BEAT else 0
    if t == 0:
        return [-1, 0, 1]
    code = _start_code(move.start_at)
    allowed = {0: (-1, 0, 1), 1: (0,), 2: (-1, 1),
               3: (1,) if mirrored else (-1,), 4: (-1,) if mirrored else (1,)}.get(code, ())
    return [s for s in (-1, 0, 1) if -1 <= s + t <= 1 and s in allowed
            and fits(geom, boxes, move, mirrored, s) and fits(geom, boxes, move, mirrored, s + t)]


def _period(fps):
    return dict(FPS_PERIODS).get(fps, (1000 + fps // 2) // fps)


def _pos16(move, n, period, pace):
    if move.kind == BEAT:
        return (n * period * pace * 65536) // 5000
    return (n * period * 65536) // 10


def _run(geom, boxes, move, period, pace, mirrored, spot):
    if move.kind == BEAT and pace <= 0:
        raise ValueError("a beat move needs a pace above 0")
    end = _starts(move)[1] * 65536
    out, prev, n = [], None, 0
    while True:
        p = _pos16(move, n, period, pace)
        pose = frame_at(geom, boxes, move, p, mirrored, spot)
        out.append((n, p, pose, None if n == 0 else dirty_px(prev, pose.rect)))
        prev = pose.rect
        if n >= 1 and p >= end:
            return out
        n += 1


def _case_specs(geom, boxes, move):
    if move.kind == IDLE:
        paces = [0]
    else:
        r = reach(move.pace_lo, move.pace_hi, move.beats, move.travel)
        paces = [] if r is None else sorted({r[0], (r[0] + r[1]) // 2, r[1]})
    orients = [False, True] if move.flags & FLAG_MIRROR else [False]
    for fps, period in FPS_PERIODS:
        for pace in paces:
            for m in orients:
                travels = move.kind == BEAT and move.travel
                for spot in (start_spots(geom, boxes, move, m) if travels else [0]):
                    yield fps, period, pace, m, spot


def _light(peak):
    return "green" if peak <= GREEN_PX else "amber" if peak <= AMBER_PX else "red"


def budget(geom, boxes, move):
    """B.6: the move's dirty px over one play in every case, and its light."""
    cases = []
    for fps, period, pace, m, spot in _case_specs(geom, boxes, move):
        run = _run(geom, boxes, move, period, pace, m, spot)
        values = sorted(d for _, _, _, d in run[1:])
        n = len(values)
        cases.append(Case(fps, pace, m, spot, n, values[(n - 1) // 2], values[-1]))
    median = max((c.median for c in cases), default=0)
    peak = max((c.peak for c in cases), default=0)
    return Budget(tuple(cases), median, peak, _light(peak))


def trace(geom, boxes, move, fps, pace, mirrored, spot):
    """One budget case frame by frame: [(n, pos16, Pose, dirty or None), ...]."""
    return _run(geom, boxes, move, _period(fps), pace, mirrored, spot)


# ---- frames (A.8-A.10, A.16)
def _tight_box(grid):
    ys = [y for y, row in enumerate(grid) if any(row)]
    if not ys:
        return Box(0, 0, 0, 0)
    xs = [x for row in grid for x, v in enumerate(row) if v]
    x0, y0 = min(xs), ys[0]
    return Box(x0, y0, max(xs) - x0 + 1, ys[-1] - y0 + 1)


def _row_tokens(row):
    """A box row's canonical (skip, tokens), tokens as (run, index)."""
    nz = [i for i, v in enumerate(row) if v]
    if not nz:
        return 0, []
    skip, last = nz[0], nz[-1]
    tokens = []
    i = skip
    while i <= last:
        j = i
        while j <= last and row[j] == row[i]:
            j += 1
        run = j - i
        while run > 0:
            take = min(16, run)
            tokens.append((take, row[i]))
            run -= take
        i = j
    return skip, tokens


def encode_frame(grid, box):
    """A.10: the frame's FDAT bytes, canonical."""
    out = bytearray()
    for y in range(box.y0, box.y0 + box.h):
        skip, tokens = _row_tokens(list(grid[y][box.x0:box.x0 + box.w]))
        out += bytes((skip, len(tokens)))
        out += bytes(((run - 1) << 4) | index for run, index in tokens)
    return bytes(out)


def _runs(grid, box):
    return sum(1 for y in range(box.y0, box.y0 + box.h)
               for _, index in _row_tokens(list(grid[y][box.x0:box.x0 + box.w]))[1] if index)


def thumbnail(grid, box):
    """A.8: the 40x40 4-bit thumbnail, 800 bytes."""
    px = [[0] * 40 for _ in range(40)]
    w, h = box.w, box.h
    if w and h:
        big = max(w, h)
        if big <= 40:
            m = 40 // big
            tw, th = w * m, h * m
            ox, oy = (40 - tw) // 2, (40 - th) // 2
            for j in range(th):
                for i in range(tw):
                    px[oy + j][ox + i] = grid[box.y0 + j // m][box.x0 + i // m]
        else:
            s = (big + 39) // 40
            tw, th = (w + s - 1) // s, (h + s - 1) // s
            ox, oy = (40 - tw) // 2, (40 - th) // 2
            for j in range(th):
                for i in range(tw):
                    counts = [0] * 16
                    for y in range(j * s, min(j * s + s, h)):
                        for x in range(i * s, min(i * s + s, w)):
                            counts[grid[box.y0 + y][box.x0 + x]] += 1
                    best = 0
                    for v in range(1, 16):
                        if counts[v] > (counts[best] if best else 0):
                            best = v
                    px[oy + j][ox + i] = best
    return bytes((row[2 * i] << 4) | row[2 * i + 1] for row in px for i in range(20))


def frame_hash(grid):
    """A.16: FNV-1a 32 over the stored cell grid, row-major."""
    return fnv1a32(b"".join(bytes(row) for row in grid))


def _mirror_grid(grid, anchor_x):
    """The cell mirrored about the anchor column (x -> 2*anchor_x - x), as
    drawing a frame mirrored places it (B.4); None when a pixel would leave
    the cell."""
    w = len(grid[0])
    out = []
    for row in grid:
        new = bytearray(w)
        for x, v in enumerate(row):
            if v:
                m = 2 * anchor_x - x
                if not 0 <= m < w:
                    return None
                new[m] = v
        out.append(bytes(new))
    return tuple(out)


# ---- PNG (C.4)
_PNG_SIG = b"\x89PNG\r\n\x1a\n"
_DEPTHS = {0: (1, 2, 4, 8, 16), 2: (8, 16), 3: (1, 2, 4, 8), 4: (8, 16), 6: (8, 16)}
_CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def _damaged(detail):
    return DanceError(f"damaged PNG ({detail})")


def _tag_text(tag):
    return "".join(chr(b) if 0x20 <= b <= 0x7E else "?" for b in tag)


def _unfilter(raw, width, height, bits):
    rowbytes = (width * bits + 7) // 8
    bpp = max(1, bits // 8)
    rows, prev = [], bytearray(rowbytes)
    for y in range(height):
        base = y * (rowbytes + 1)
        ft = raw[base]
        line = bytearray(raw[base + 1:base + 1 + rowbytes])
        if ft == 1:
            for i in range(bpp, rowbytes):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif ft == 2:
            for i in range(rowbytes):
                line[i] = (line[i] + prev[i]) & 255
        elif ft == 3:
            for i in range(rowbytes):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 255
        elif ft == 4:
            for i in range(rowbytes):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 255
        elif ft != 0:
            raise _damaged(f"bad filter {ft}")
        rows.append(line)
        prev = line
    return rows


def _samples(line, depth, count):
    if depth == 8:
        return list(line[:count])
    if depth == 16:
        return [(line[2 * i] << 8) | line[2 * i + 1] for i in range(count)]
    per, mask = 8 // depth, (1 << depth) - 1
    out = []
    for b in line:
        for k in range(per):
            out.append((b >> (8 - depth * (k + 1))) & mask)
    return out[:count]


def read_png(data):
    """C.4: a PNG -> (width, height, rows), rows[y][x] = (r, g, b, a) in 8 bits.
    Raises DanceError."""
    data = bytes(data)
    if data[:8] != _PNG_SIG:
        raise DanceError("not a PNG")
    pos, ihdr, plte, trns, idat = 8, None, None, None, []
    while True:
        if pos + 12 > len(data):
            raise _damaged("truncated")
        length, ctype = struct.unpack(">I4s", data[pos:pos + 8])
        if pos + 12 + length > len(data):
            raise _damaged("truncated")
        body = data[pos + 8:pos + 8 + length]
        (crc,) = struct.unpack(">I", data[pos + 8 + length:pos + 12 + length])
        if zlib.crc32(ctype + body) & 0xFFFFFFFF != crc:
            raise _damaged(f"bad CRC in {_tag_text(ctype)}")
        pos += 12 + length
        if ihdr is None:
            if ctype != b"IHDR":
                raise _damaged("no IHDR")
            if length != 13:
                raise _damaged("bad IHDR")
            ihdr = struct.unpack(">IIBBBBB", body)
            width, height, depth, colour, comp, filt, inter = ihdr
            if width == 0 or height == 0 or comp != 0 or filt != 0 or inter > 1:
                raise _damaged("bad IHDR")
            if depth not in _DEPTHS.get(colour, ()):
                raise DanceError(f"colour type {colour} at {depth} bits isn't valid PNG")
            if inter == 1:
                raise DanceError("save without interlacing")
            if width > MAX_SHEET or height > MAX_SHEET:
                raise DanceError(f"{width}x{height}: at most {MAX_SHEET}x{MAX_SHEET}")
            continue
        if ctype == b"IEND":
            break
        if ctype == b"IHDR":
            raise _damaged("bad IHDR")
        if ctype == b"PLTE":
            plte = body
        elif ctype == b"IDAT":
            idat.append(body)
        elif ctype == b"tRNS":
            trns = body
        elif not ctype[0] & 0x20:
            raise _damaged(f"unknown critical chunk {_tag_text(ctype)}")
    width, height, depth, colour = ihdr[:4]
    if colour == 3 and plte is None:
        raise _damaged("no PLTE")
    if plte is not None and (len(plte) % 3 or not 3 <= len(plte) <= 768):
        raise _damaged("bad PLTE")
    if not idat:
        raise _damaged("no IDAT")
    channels = _CHANNELS[colour]
    bits = depth * channels
    want = height * ((width * bits + 7) // 8 + 1)
    # Inflate at most one byte more than the image needs: a small file can
    # hold gigabytes of zeros. No flush(), which would inflate the rest.
    try:
        d = zlib.decompressobj()
        raw = d.decompress(b"".join(idat), want + 1)
        if len(raw) > want or d.unconsumed_tail:
            raise _damaged(f"more than {want} bytes of pixels")
        if not d.eof:
            raise zlib.error("incomplete")
    except zlib.error:
        raise _damaged("bad zlib data")
    if len(raw) != want:
        raise _damaged(f"{len(raw)} bytes of pixels, not {want}")
    lines = _unfilter(raw, width, height, bits)
    hi = (lambda v: v >> 8) if depth == 16 else (lambda v: v)
    grey = (lambda v: v * 255 // ((1 << depth) - 1)) if depth < 8 else hi
    key = None
    if trns is not None and colour == 0 and len(trns) == 2:
        key = struct.unpack(">H", trns)[0]
    elif trns is not None and colour == 2 and len(trns) == 6:
        key = struct.unpack(">HHH", trns)
    pal = [tuple(plte[i:i + 3]) for i in range(0, len(plte), 3)] if plte else []
    alphas = bytes(trns) if trns is not None and colour == 3 else b""
    rows = []
    for line in lines:
        s = _samples(line, depth, width * channels)
        row = []
        if colour == 0:
            for v in s:
                g = grey(v)
                row.append((g, g, g, 0 if v == key else 255))
        elif colour == 2:
            for x in range(width):
                v = tuple(s[3 * x:3 * x + 3])
                row.append((hi(v[0]), hi(v[1]), hi(v[2]), 0 if v == key else 255))
        elif colour == 3:
            for v in s:
                if v >= len(pal):
                    raise _damaged("bad PLTE")
                r, g, b = pal[v]
                row.append((r, g, b, alphas[v] if v < len(alphas) else 255))
        elif colour == 4:
            for x in range(width):
                g = hi(s[2 * x])
                row.append((g, g, g, hi(s[2 * x + 1])))
        else:
            for x in range(width):
                row.append(tuple(hi(v) for v in s[4 * x:4 * x + 4]))
        rows.append(row)
    return width, height, rows


def _png_chunk(tag, body):
    return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)


def _write_png(width, height, rows):
    """An RGB 8-bit PNG (colour type 2) from rows[y][x] = (r, g, b)."""
    raw = bytearray()
    for row in rows:
        raw.append(0)
        for r, g, b in row:
            raw += bytes((r, g, b))
    return (_PNG_SIG + _png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + _png_chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + _png_chunk(b"IEND", b""))


# ---- cells
def _col_name(col):
    s, n = "", col + 1
    while n:
        n, r = divmod(n - 1, 26)
        s = chr(65 + r) + s
    return s


def _cell_name(cell):
    return f"{_col_name(cell[0])}{cell[1] + 1}"


def _parse_cell(text):
    """'B4' -> (1, 3), zero-based (column, row); None when it isn't a cell name.
    A sheet is at most 1024 cells either way (AMJ, 1024), so longer letters or
    digits are no cell, and are never converted (C.4)."""
    m = _CELL.fullmatch(text)
    if not m:
        return None
    letters, digits = m.group(1), m.group(2).lstrip("0")
    if len(letters) > 3 or not digits or len(digits) > 4:
        return None
    col = 0
    for ch in letters.upper():
        col = col * 26 + ord(ch) - 64
    return col - 1, int(digits) - 1


def _cells_text(cells):
    names = [_cell_name(c) for c in sorted(cells, key=lambda c: (c[1], c[0]))]
    return ", ".join(names[:8]) + (" …" if len(names) > 8 else "")


# ---- dancer.ini (C.3)
def _parser():
    # default_section "" can't name a section, so [DEFAULT] is an ordinary
    # (unknown) section rather than defaults for every other one. SECTCRE is
    # pinned (configparser documents overriding it), so a header reads the
    # same on Python 3.8 and 3.12.
    cp = configparser.ConfigParser(interpolation=None, strict=True, comment_prefixes=("#", ";"),
                                   inline_comment_prefixes=(";",), default_section="")
    cp.SECTCRE = _SECTCRE
    return cp


def _ini_layout(text):
    """Where configparser finds each section and key: ([(header, line,
    {key: line})] in file order, wraps). It follows RawConfigParser._read's
    rules for comments, continuation lines and headers, so the lines match the
    values. wraps lists (line, key) for each indented whole-line comment made
    only of #rrggbb tokens inside [dancer]'s palette or idle_palette: the
    colours an artist wrapped onto the next line, which configparser drops."""
    sect = _SECTCRE
    opt = configparser.ConfigParser.OPTCRE
    nonspace = configparser.ConfigParser.NONSPACECRE
    out, wraps, cur, optname, indent = [], [], None, None, 0
    for lineno, line in enumerate(io.StringIO(text), start=1):
        comment_start = sys.maxsize
        inline = {";": -1}
        while comment_start == sys.maxsize and inline:
            nxt = {}
            for prefix, index in inline.items():
                index = line.find(prefix, index + 1)
                if index == -1:
                    continue
                nxt[prefix] = index
                if index == 0 or (index > 0 and line[index - 1].isspace()):
                    comment_start = min(comment_start, index)
            inline = nxt
        for prefix in ("#", ";"):
            if line.strip().startswith(prefix):
                comment_start = 0
                break
        if comment_start == sys.maxsize:
            comment_start = None
        value = line[:comment_start].strip()
        m = nonspace.search(line)
        cur_indent = m.start() if m else 0
        if (not value and comment_start == 0 and cur is not None and cur_indent > indent
                and optname in ("palette", "idle_palette") and cur[0].strip().lower() == "dancer"):
            tokens = re.split(r"\s;", line, 1)[0].split()
            if tokens and all(_HEX.fullmatch(t) for t in tokens):
                wraps.append((lineno, optname))
        if not value:
            continue
        if cur is not None and optname and cur_indent > indent:
            continue
        indent = cur_indent
        mo = sect.match(value)
        if mo:
            cur = (mo.group("header"), lineno, {})
            out.append(cur)
            optname = None
        elif cur is not None:
            mo = opt.match(value)
            if mo:
                optname = mo.group("option").rstrip().lower()
                cur[2].setdefault(optname, lineno)
    return out, wraps


def _near(key, known):
    """(K, strong) for the known word K nearest to key (C.3): an adjacent
    transposition is strong; otherwise the highest difflib ratio of at least
    0.6, the earlier word on a tie, strong from 0.75. (None, False) for none."""
    for k in known:
        if len(k) == len(key) and any(key[:i] + key[i + 1] + key[i] + key[i + 2:] == k
                                      for i in range(len(key) - 1)):
            return k, True
    best, best_ratio = None, 0.0
    for k in known:
        ratio = difflib.SequenceMatcher(None, key, k).ratio()
        if ratio >= 0.6 and ratio > best_ratio:
            best, best_ratio = k, ratio
    return best, best_ratio >= 0.75


def _typo(key, known):
    """The known word key is surely a typo of (an error or a skip), or None."""
    k, strong = _near(key, known)
    return k if strong else None


def _hint(key, known):
    """The known word a weaker match suggests (a warning's hint), or None."""
    return _near(key, known)[0]


class _Fail(Exception):
    """A pack-level error, already reported."""


class _Skip(Exception):
    def __init__(self, line, text):
        Exception.__init__(self, text)
        self.line, self.text = line, text


class _Section:
    def __init__(self, header, line, keys, values):
        self.header, self.line, self.lines, self.values = header, line, keys, values

    def get(self, key):
        return self.values.get(key)

    def at(self, key):
        return self.lines.get(key, self.line)


class _Compiler:
    """One compile of a dancer folder (C.5's order). `items` holds every
    printed line as (kind, text): "msg" (what build prints), "echo" (the step
    tables) and "cov" (the coverage report)."""

    def __init__(self, path, strict):
        self.path = Path(path)
        self.dir_text = _slash(path)
        self.strict = strict
        self.items = []
        self.data = self.pack = None
        self.skipped, self.budgets, self.kept = [], {}, []

    # -- output
    def msg(self, text):
        self.items.append(("msg", _esc(text)))

    def fail(self, text):
        self.msg(text)
        raise _Fail()

    def ini(self, line, text):
        return f"dancer.ini:{line}: {text}" if line else f"dancer.ini: {text}"

    def run(self):
        try:
            self._start()
            self._read_ini()
            self._dancer()
            self._sheet()
            self._moves()
            self._frames()
            self._write()
            self._coverage()
            ok = self._strict()
            self._summary()
        except _Fail:
            ok = False
        if not ok:
            self.data = self.pack = None
        return ok

    def result(self, ok):
        return Result(ok, self.data, self.pack, [t for k, t in self.items if k == "msg"],
                      [t for _, t in self.items], tuple(self.skipped), dict(self.budgets))

    # -- the folder and dancer.ini
    def _start(self):
        ini = self.path / "dancer.ini"
        if not ini.is_file():
            self.fail(f"{self.dir_text}: error: no dancer.ini")
        # abspath, not resolve(): robo, robo/ and ./robo agree, and the name stays as
        # given even where the file system ignores case (Windows, macOS)
        self.id = Path(os.path.abspath(str(self.path))).name
        if not _ID.fullmatch(self.id):
            self.fail(f'{self.dir_text}: error: the folder name "{self.id}" can\'t be an id: use 1-23 '
                      "of a-z, 0-9 and -, starting with a letter or digit")
        try:
            raw = ini.read_bytes()
        except OSError as e:
            self.fail(f"dancer.ini: error: can't read it ({_strerror(e)})")
        if raw.startswith(b"\xef\xbb\xbf"):
            raw = raw[3:]
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError as e:
            before = raw[:e.start].replace(b"\r\n", b"\n").replace(b"\r", b"\n")
            self.fail(self.ini(before.count(b"\n") + 1, "error: not UTF-8: save it as UTF-8"))
        self.text = text.replace("\r\n", "\n").replace("\r", "\n")

    def _read_ini(self):
        cp = _parser()
        lines = self.text.split("\n")
        try:
            cp.read_string(self.text)
        except configparser.DuplicateSectionError as e:
            self.fail(self.ini(e.lineno, f"error: duplicate section [{e.section}]"))
        except configparser.DuplicateOptionError as e:
            self.fail(self.ini(e.lineno, f'error: duplicate key "{e.option}" in [{e.section}]'))
        except configparser.MissingSectionHeaderError as e:
            self.fail(self.ini(e.lineno, f'error: "{lines[e.lineno - 1].strip()}" is outside any section'))
        except configparser.ParsingError as e:
            lineno = e.errors[0][0]
            self.fail(self.ini(lineno, f'error: can\'t read "{lines[lineno - 1].strip()}": not key = value'))
        self.dancer, self.moves_in, self.near_in, names = None, [], [], set()
        layout, wraps = _ini_layout(self.text)
        for header, line, keys in layout:
            values = dict(cp.items(header, raw=True))
            sec = _Section(header, line, keys, values)
            words = header.split(None, 1)
            kind = words[0].lower() if words else ""
            if kind == "dancer" and len(words) == 1:
                if self.dancer is not None:
                    self.fail(self.ini(line, f"error: duplicate section [{header}]"))
                self.dancer, known = sec, DANCER_KEYS
            elif kind == "move":
                sec.name = words[1].strip() if len(words) > 1 else ""
                if not sec.name:
                    self.fail(self.ini(line, "error: a move needs a name: [move <name>]"))
                if sec.name in names:
                    self.fail(self.ini(line, f'error: duplicate move "{sec.name}"'))
                names.add(sec.name)
                self.moves_in.append(sec)
                known = MOVE_KEYS
            else:
                self._unknown_section(sec, words, kind)
                continue
            for key, value in values.items():
                if key.startswith("x-"):
                    continue
                if key not in known:
                    if not _typo(key, known):      # a sure typo is an error or a skip, later
                        hint = _hint(key, known)
                        tail = f" (did you mean {hint}?)" if hint else ""
                        self.msg(self.ini(sec.at(key), f'warning: unknown key "{key}" in [{header}] ignored{tail}'))
                    continue
                m = _COMMENT_IN_VALUE.search(value)
                if m:
                    rest = value[m.start():].split("\n", 1)[0].strip()
                    self.msg(self.ini(sec.at(key), f'warning: {key}: "{rest}" is part of the value; a comment '
                                                   "after a value starts with ;"))
        if wraps:
            line, key = wraps[0]
            self.fail(self.ini(line, f"error: an indented line starting with # is a comment, not more colours: "
                                     f"keep {key} on one line"))
        if self.dancer is None:
            self.fail("dancer.ini: error: no [dancer] section")

    def _unknown_section(self, sec, words, kind):
        """A section that is neither [dancer] nor [move <name>] (C.3): a near
        miss of either is an error or a skipped move; anything else a warning.
        `move` and a non-letter is checked first, so [move-1] skips move 1. A
        skipped near miss joins near_in: its cells are used cells (C.4)."""
        header, line = sec.header, sec.line
        if re.match(r"move[^a-z]", kind):
            near, name = "move", header.strip()[5:].strip()
        else:
            near, name = _typo(kind, ("dancer", "move")) if kind else None, ""
            if near == "move":
                name = words[1].strip() if len(words) > 1 else ""
        if near == "dancer":
            self.fail(self.ini(line, f"error: unknown section [{header}]: did you mean [dancer]?"))
        if near == "move" and not name:
            self.fail(self.ini(line, f"error: unknown section [{header}]: did you mean [move <name>]?"))
        if near == "move":
            self.msg(self.ini(line, f'move "{name}": unknown section [{header}]: did you mean [move {name}]? '
                                    "(skipped)"))
            self.skipped.append(name)
            self.near_in.append(sec)
            return
        self.msg(self.ini(line, f"warning: unknown section [{header}] ignored"))

    # -- [dancer]
    def _text(self, key, lo, hi, default=None):
        d = self.dancer
        v = d.get(key)
        if v is None:
            return default
        line = d.at(key)
        if lo and not v:
            self.fail(self.ini(line, f"error: {key} is empty"))
        size = len(v.encode("utf-8"))
        if size > hi:
            self.fail(self.ini(line, f"error: {key} is {size} bytes: at most {hi}"))
        if not _valid_text(v):
            self.fail(self.ini(line, f"error: {key} has a control character"))
        lack = sorted({ord(ch) for ch in v if not _covered(ord(ch))})
        if lack:
            self.msg(self.ini(line, f"warning: {key}: {len(lack)} character{'s' if len(lack) != 1 else ''} "
                                    f'the device fonts lack will show as a look-alike or "?": {_ucodes(lack)}'))
        return v

    def _dancer(self):
        d = self.dancer
        if d.get("format") is not None:
            v = d.get("format")
            if _INT.fullmatch(v) and _int(v) > FORMAT:
                self.fail(self.ini(d.at("format"), f"error: format {_int(v)} is newer than this dancepack.py "
                                                   f"knows ({FORMAT}): update dancepack.py"))
            if not _INT.fullmatch(v) or _int(v) != FORMAT:
                self.fail(self.ini(d.at("format"), "error: format must be 1"))
        for key in d.values:
            if not key.startswith("x-") and key not in DANCER_KEYS:
                k = _typo(key, DANCER_KEYS)
                if k:
                    self.fail(self.ini(d.at(key), f'error: unknown key "{key}" in [dancer]: did you mean {k}?'))
        for key in ("name", "author", "license", "cell"):
            if d.get(key) is None:
                self.fail(self.ini(d.line, f"error: [dancer] needs {key} ="))
        self.name = self._text("name", 1, 24)
        self.author = self._text("author", 1, 40)
        self.licence = self._text("license", 1, 40)
        self.sheet = d.get("sheet") if d.get("sheet") is not None else "sheet.png"
        self._sheet_path(d.at("sheet"))
        m = re.fullmatch(r"(" + _N + r")[xX](" + _N + r")", d.get("cell"))
        if not m or _int(m.group(1)) < 1 or _int(m.group(2)) < 1:
            self.fail(self.ini(d.at("cell"), "error: cell must be WxH, like 64x76"))
        self.cw, self.ch = _int(m.group(1)), _int(m.group(2))
        self.scale = 1
        if d.get("scale") is not None:
            v = d.get("scale")
            if not _INT.fullmatch(v) or not 1 <= _int(v) <= 4:
                self.fail(self.ini(d.at("scale"), "error: scale must be 1, 2, 3 or 4"))
            self.scale = _int(v)
        s = self.scale
        if self.cw * s > 160 or self.ch * s > 160:
            self.fail(self.ini(d.at("cell"), f"error: cell {self.cw}x{self.ch} at scale {s} is "
                                             f"{self.cw * s}x{self.ch * s} on screen: at most 160x160"))
        self.ax, self.ay = self.cw // 2, self.ch
        if d.get("anchor") is not None:
            m = re.fullmatch(r"(" + _N + r")\s*,\s*(" + _N + r")", d.get("anchor"))
            if not m:
                self.fail(self.ini(d.at("anchor"), "error: anchor must be x,y"))
            self.ax, self.ay = _int(m.group(1)), _int(m.group(2))
            if not (self.ax < self.cw and 1 <= self.ay <= self.ch):
                self.fail(self.ini(d.at("anchor"), f"error: anchor {self.ax},{self.ay} is outside the "
                                                   f"{self.cw}x{self.ch} cell"))
        if self.ay * s > GROUND_Y:
            self.fail(self.ini(d.at("anchor"), f"error: anchor y {self.ay} at scale {s} puts the cell's top "
                                               f"{self.ay * s} px above the ground: at most {GROUND_Y}"))
        if (self.ch - self.ay) * s > 10:
            self.fail(self.ini(d.at("anchor"), f"error: the cell reaches {(self.ch - self.ay) * s} screen px "
                                               "below the ground: at most 10"))
        self.rest_text = d.get("rest") if d.get("rest") is not None else "A1"
        self.shadow = 0
        if d.get("shadow") is not None:
            v = d.get("shadow")
            if not _INT.fullmatch(v) or _int(v) > 160:
                self.fail(self.ini(d.at("shadow"), "error: shadow must be 0-160 (screen px)"))
            self.shadow = _int(v)
        self.key = None
        if d.get("key") is not None:
            if not _HEX.fullmatch(d.get("key")):
                self.fail(self.ini(d.at("key"), "error: key must be #rrggbb"))
            self.key = _parse_hex(d.get("key"))
        self.listed = self._colours("palette")
        if self.listed is not None:
            if len(self.listed) > MAX_COLOURS:
                self.fail(self.ini(d.at("palette"), f"error: palette has {len(self.listed)} colours: at most 15"))
            seen = {}
            for c in self.listed:
                k = rgb565(*c)
                if k in seen:
                    self.fail(self.ini(d.at("palette"), f"error: palette lists {_hex(seen[k])} and {_hex(c)}, "
                                                        "both RGB565 0x%04X" % k))
                seen[k] = c
        self.idle_listed = self._colours("idle_palette")
        self.faces = 0
        if d.get("faces") is not None:
            v = d.get("faces").lower()
            if v not in FACES:
                self.fail(self.ini(d.at("faces"), "error: faces must be none, right or left"))
            self.faces = FACES.index(v)
        self.url = self._text("url", 0, 80, "")
        self.version = self._text("version", 0, 16, "")

    def _sheet_path(self, line):
        """C.3's sheet: a relative path with / inside the folder, each name
        matched exactly as written, so it reads alike on every file system."""
        v = self.sheet
        parts = v.split("/")
        if not v or v.startswith("/") or "\\" in v or ":" in v or any(p in ("", "..") for p in parts):
            hint = ": use / between folders" if "\\" in v else ""
            self.fail(self.ini(line, f"error: sheet must be a path inside the dancer's folder{hint}"))
        for p in parts:
            if p != "." and p != p.rstrip(". "):
                self.fail(self.ini(line, f'error: sheet: "{p}" ends in a dot or a space, which Windows drops: '
                                         "rename it"))
        real, cur, exact = [], self.path, True
        for p in parts:
            if p == ".":
                continue
            try:
                names = sorted(os.listdir(str(cur)))
            except OSError:
                names = []
            if p not in names:
                folded = [n for n in names if n.lower() == p.lower()]
                if not folded:
                    self.fail(self.ini(line, f"error: no sheet at {v}"))
                p, exact = folded[0], False
            real.append(p)
            cur = cur / p
        if not cur.is_file():
            self.fail(self.ini(line, f"error: no sheet at {v}"))
        if not exact:
            self.fail(self.ini(line, f"error: no sheet at {v} (the file is {'/'.join(real)}: names are "
                                     "case-sensitive)"))

    def _colours(self, key):
        v = self.dancer.get(key)
        if v is None:
            return None
        tokens = v.split()
        if not tokens:
            self.fail(self.ini(self.dancer.at(key), f"error: {key} is empty"))
        for t in tokens:
            if not _HEX.fullmatch(t):
                self.fail(self.ini(self.dancer.at(key), f'error: {key}: "{t}" isn\'t #rrggbb'))
        return [_parse_hex(t) for t in tokens]

    # -- the sheet and the palette
    def _sheet(self):
        d, sheet = self.dancer, self.sheet
        try:
            data = (self.path / sheet).read_bytes()
        except OSError as e:
            self.fail(f"{sheet}: error: can't read it ({_strerror(e)})")
        try:
            w, h, rows = read_png(data)
        except DanceError as e:
            self.fail(f"{sheet}: error: {e}")
        if w % self.cw or h % self.ch:
            self.fail(f"{sheet}: error: {w}x{h} isn't a whole number of {self.cw}x{self.ch} cells")
        self.px = rows
        self.cols, self.rows = w // self.cw, h // self.ch
        self.grid_text = f"{self.cols}x{self.rows}"
        self._blank = {}
        rest = _parse_cell(self.rest_text)
        if rest is None or not self._on_sheet(rest):
            self.fail(self.ini(d.at("rest"), f"error: rest {self.rest_text} isn't a cell of the "
                                             f"{self.grid_text} sheet"))
        if self.blank(rest):
            self.fail(self.ini(d.at("rest"), f"error: the rest frame {self.rest_text} is empty"))
        self.rest = rest
        used = {rest}
        for sec in self.moves_in + self.near_in:       # C.4: skipped moves too, near misses included
            used.update(c for c, _ in self._named_cells(sec))
        if len(used) > MAX_CELLS:
            self.fail(f"dancer.ini: error: {len(used)} cells used: at most {MAX_CELLS}")
        used = sorted(used, key=lambda c: (c[1], c[0]))
        if all(all(self._opaque(p) for p in self._pixels(c)) for c in used):
            corner = next(self._pixels(used[0]))[:3]
            self.fail(f"{sheet}: error: every cell is fully opaque: export with transparency, or set key = "
                      f"{_hex(corner)} (the top-left pixel of {_cell_name(used[0])})")
        partial, partial_cells = 0, []
        for c in used:
            k = sum(1 for p in self._pixels(c) if 0 < p[3] < 255)
            if k:
                partial += k
                partial_cells.append(c)
        if partial:
            self.msg(f"{sheet}: warning: {_n(partial)} px with partial alpha in {_cells_text(partial_cells)}: "
                     "alpha 128-254 counts as opaque, 1-127 as transparent")
        # colours, in scan order
        counts, cells_of, order = {}, {}, []
        for c in used:
            for p in self._pixels(c):
                if self._opaque(p):
                    rgb = p[:3]
                    if rgb not in counts:
                        counts[rgb] = 0
                        cells_of[rgb] = []
                        order.append(rgb)
                    counts[rgb] += 1
                    if not cells_of[rgb] or cells_of[rgb][-1] != c:
                        cells_of[rgb].append(c)
        if self.listed is None:
            groups, first, notes = {}, {}, []
            for rgb in order:
                k = rgb565(*rgb)
                if k in first:
                    notes.append(f"{sheet}: note: {_hex(rgb)} merges with {_hex(first[k])} (both RGB565 0x%04X)" % k)
                    groups[k].append(rgb)
                else:
                    first[k] = rgb
                    groups[k] = [rgb]
            if len(groups) > MAX_COLOURS:
                # C.6's <list>: the 15 most used, then the rest summed up (no merge notes)
                entries = []
                for i, k in enumerate(groups):
                    n = sum(counts[c] for c in groups[k])
                    cells = {c for rgb in groups[k] for c in cells_of[rgb]}
                    entries.append((-n, i, k, n, cells))
                entries.sort()
                shown, rest = entries[:MAX_COLOURS], entries[MAX_COLOURS:]
                listing = ", ".join(f"{_hex(first[k])} {_n(n)} px" + (f" in {_cells_text(cells)}" if n < 16 else "")
                                    for _, _, k, n, cells in shown)
                each = " each" if len(rest) > 1 else ""
                more = (f", and {_s(len(rest), 'more colour')} of at most {_n(max(e[3] for e in rest))} px{each}, "
                        f"in {_cells_text(set().union(*(e[4] for e in rest)))}")
                self.fail(f"{sheet}: error: {len(groups)} colours: at most 15. By pixel count: {listing}{more} "
                          "(smoothing or anti-aliasing in the export adds colours: export at 100 % with no smoothing)")
            for note in notes:
                self.msg(note)
            self.palette = [first[k] for k in groups]
        else:
            self.palette = list(self.listed)
            index = {rgb565(*c): c for c in self.listed}
            for rgb in order:
                k = rgb565(*rgb)
                if k not in index:
                    self.fail(f"{sheet}: error: colour {_hex(rgb)} (in {_cells_text(cells_of[rgb])}) isn't in palette =")
            for rgb in order:
                k = rgb565(*rgb)
                if rgb != index[k]:
                    self.msg(f"{sheet}: note: {_hex(rgb)} merges with {_hex(index[k])} (both RGB565 0x%04X)" % k)
        self.index = {rgb565(*c): i + 1 for i, c in enumerate(self.palette)}
        if self.idle_listed is not None:
            if len(self.idle_listed) != len(self.palette):
                have = _s(len(self.idle_listed), "colour")
                self.fail(self.ini(d.at("idle_palette"), f"error: idle_palette has {have}; the palette has "
                                                         f"{len(self.palette)}"))
            self.idle = list(self.idle_listed)
        else:
            self.idle = [idle_colour(c) for c in self.palette]
        for i, c in enumerate(self.palette, 1):
            if near_background(c):
                self.msg(f"{sheet}: warning: colour {i} {_hex(c)} is close to the background #080c10: it will vanish")
        for i, c in enumerate(self.idle, 1):
            if near_background(c):
                self.msg(self.ini(d.at("idle_palette"), f"warning: idle colour {i} {_hex(c)} is close to the "
                                                        "background #080c10: it will vanish while idle"))
        if self.listed is not None:
            seen = {rgb565(*rgb) for rgb in order}
            for c in self.listed:
                if rgb565(*c) not in seen:
                    self.msg(self.ini(d.at("palette"), f"warning: palette colour {_hex(c)} isn't used"))
        self.grids = {}
        self._rest_on_ground()
        self._runs_on(used)

    def _rest_on_ground(self):
        """C.6: the rest frame's lowest drawn row should be anchor y - 1."""
        d, ax, ay, s = self.dancer, self.ax, self.ay, self.scale
        box = _tight_box(self.grid(self.rest))
        bottom = box.y0 + box.h
        if bottom == ay:
            return
        way, gap = ("above", ay - bottom) if bottom < ay else ("below", bottom - ay)
        if bottom * s <= GROUND_Y and (self.ch - bottom) * s <= 10:
            fix = f"set anchor = {ax},{bottom} to stand it on the ground"
        else:
            fix = f"draw it {gap} native px {'lower' if bottom < ay else 'higher'}"
        self.msg(self.ini(d.at("anchor"), f"warning: the rest frame {_cell_name(self.rest)}'s feet end on row "
                                          f"{bottom - 1}, {gap} native px {way} the ground row (anchor y {ay}): {fix}"))

    def _runs_on(self, used):
        """C.6: drawings cut by a cell width that is too small. A used cell
        runs on when an opaque pixel of its last column has an opaque pixel
        beside it in the next cell's first column; at least half of the used,
        non-blank cells doing so is the warning."""
        drawn = [c for c in used if not self.blank(c)]
        hits = []
        for c, r in drawn:
            if c + 1 >= self.cols:
                continue
            x = c * self.cw + self.cw - 1
            if any(self._opaque(self.px[y][x]) and self._opaque(self.px[y][x + 1])
                   for y in range(r * self.ch, (r + 1) * self.ch)):
                hits.append((c, r))
        if hits and 2 * len(hits) >= len(drawn):
            c, r = hits[0]
            self.msg(f"{self.sheet}: warning: the drawing in {_cell_name((c, r))} runs on into "
                     f"{_cell_name((c + 1, r))}: is cell = {self.cw}x{self.ch} right?")

    def _on_sheet(self, cell):
        return cell[0] < self.cols and cell[1] < self.rows

    def _pixels(self, cell):
        x0, y0 = cell[0] * self.cw, cell[1] * self.ch
        for y in range(y0, y0 + self.ch):
            row = self.px[y]
            for x in range(x0, x0 + self.cw):
                yield row[x]

    def _opaque(self, p):
        return p[3] >= 128 and (self.key is None or p[:3] != self.key)

    def blank(self, cell):
        if cell not in self._blank:
            self._blank[cell] = not any(self._opaque(p) for p in self._pixels(cell))
        return self._blank[cell]

    def grid(self, cell):
        if cell not in self.grids:
            rows, it = [], self._pixels(cell)
            for _ in range(self.ch):
                rows.append(bytes(self.index[rgb565(*p[:3])] if self._opaque(p) else 0
                                  for p in (next(it) for _ in range(self.cw))))
            self.grids[cell] = tuple(rows)
        return self.grids[cell]

    def _row_cells(self, r):
        last = -1
        for c in range(self.cols):
            if not self.blank((c, r)):
                last = c
        return [((c, r), _cell_name((c, r))) for c in range(last + 1)]

    def _named_cells(self, sec):
        out = []
        v = sec.get("row")
        if v is not None and _INT.fullmatch(v.strip()) and 1 <= _int(v) <= self.rows:
            out += self._row_cells(_int(v) - 1)
        v = sec.get("frames")
        if v is not None:
            for t in v.split():
                c = _parse_cell(t)
                if c is not None and self._on_sheet(c):
                    out.append((c, t))
        return out

    # -- moves
    def _moves(self):
        self.freeze_move = None
        for sec in self.moves_in:
            try:
                mv = self._move(sec)
            except _Skip as e:
                self.msg(self.ini(e.line, f'move "{sec.name}": {e.text} (skipped)'))
                self.skipped.append(sec.name)
                continue
            self.kept.append(mv)
            self.budgets[mv["name"]] = mv["budget"]
            self._report_move(mv)

    def _move(self, sec):
        g, at, name = sec.get, sec.at, sec.name
        if not _ID.fullmatch(name):
            raise _Skip(sec.line, "its name must be 1-23 of a-z, 0-9 and -, starting with a letter or digit")
        for key in sec.values:
            if not key.startswith("x-") and key not in MOVE_KEYS:
                k = _typo(key, MOVE_KEYS)
                if k:
                    raise _Skip(at(key), f'unknown key "{key}": did you mean {k}?')
        if g("bpm") is None:
            raise _Skip(sec.line, "needs bpm = (a range like 90-130, any, or none)")
        bpm = g("bpm").strip().lower()
        m = _RANGE.fullmatch(bpm)
        if bpm not in ("any", "none") and not m:
            raise _Skip(at("bpm"), "bpm must be lo-hi, any or none")
        kind = IDLE if bpm == "none" else BEAT
        lo, hi = (_int(m.group(1)), _int(m.group(2))) if m else (0, 0)
        present = sorted((at(k), k) for k in sec.values if k in MOVE_KEYS)
        if kind == IDLE:
            for line, k in present:
                if k in BEAT_ONLY:
                    raise _Skip(line, f"{k} applies to beat moves only")
        elif g("when") is not None:
            raise _Skip(at("when"), "when applies to no-BPM moves only")
        tv = g("travel")
        no_travel = tv is None or (_SINT.fullmatch(tv.strip()) and _int(tv) == 0)
        if no_travel:
            for line, k in present:
                if k in NEED_TRAVEL:
                    raise _Skip(line, f"{k} needs travel =")
        # cells
        if g("row") is None and g("frames") is None:
            raise _Skip(sec.line, "give row = or frames =")
        if g("row") is not None and g("frames") is not None:
            raise _Skip(at("row"), "give row = or frames =, not both")
        if g("row") is not None:
            v = g("row").strip()
            if not _INT.fullmatch(v) or not 1 <= _int(v) <= self.rows:
                raise _Skip(at("row"), f"row {v} isn't a row of the {self.grid_text} sheet")
            cells = self._row_cells(_int(v) - 1)
            if not cells:
                raise _Skip(at("row"), f"row {_int(v)} is empty")
            cells_at = at("row")
        else:
            tokens = g("frames").split()
            if not tokens:
                raise _Skip(sec.line, "give row = or frames =")
            cells = []
            for t in tokens:
                c = _parse_cell(t)
                if c is None or not self._on_sheet(c):
                    hint = ": separate cells with spaces" if "," in t else ""
                    raise _Skip(at("frames"), f'frames: "{t}" isn\'t a cell of the {self.grid_text} sheet{hint}')
                cells.append((c, t))
            cells_at = at("frames")
        n = len(cells)
        if n > MAX_STEPS:
            raise _Skip(cells_at, f"{n} frames: at most 32 steps")
        beats = 0
        if kind == BEAT:
            if g("beats") is None:
                raise _Skip(sec.line, "needs beats = 1, 2, 4 or 8")
            v = g("beats").strip()
            if not _INT.fullmatch(v) or _int(v) not in (1, 2, 4, 8):
                raise _Skip(at("beats"), "beats must be 1, 2, 4 or 8")
            beats = _int(v)
        elif g("timing") is None:
            raise _Skip(sec.line, "a no-BPM move needs timing = (s or ms)")
        snaps = []
        if g("timing") is not None:
            tokens = g("timing").split()
            if len(tokens) != n:
                raise _Skip(at("timing"), f"timing has {_s(len(tokens), 'value')} for {_s(n, 'step')}: give one "
                                          "per step")
            try:
                if kind == BEAT:
                    for t in tokens:
                        if not _BEAT_TIME.fullmatch(t):
                            parse_beat_time(t)
                    durs = []
                    for t in tokens:
                        ticks, snapped = parse_beat_time(t)
                        durs.append(ticks)
                        if snapped:
                            snaps.append((t, ticks))
                else:
                    for t in tokens:
                        _idle_form(t)
                    durs = [parse_idle_time(t) for t in tokens]
            except DanceError as e:
                raise _Skip(at("timing"), str(e))
        else:
            try:
                durs = even_split(n, beats)
            except DanceError as e:
                raise _Skip(cells_at, str(e))
        if kind == BEAT:
            for i, d in enumerate(durs, 1):
                if d < 3:
                    raise _Skip(at("timing") if g("timing") is not None else cells_at,
                                f"step {i} lasts {d} ticks: at least 3 (1/4 beat)")
            if sum(durs) != 12 * beats:
                t = sum(durs)
                raise _Skip(at("timing"), f"steps last {t} ticks ({beats_text(t)}); a {beats}-beat move "
                                          f"needs {12 * beats}")
        else:
            for i, d in enumerate(durs, 1):
                if not 20 <= d <= 500:
                    raise _Skip(at("timing"), f"step {i} lasts {10 * d} ms: 200-5,000 ms")
            if not 50 <= sum(durs) <= 1200:
                raise _Skip(at("timing"), f"the steps last {10 * sum(durs)} ms in all: 500-12,000 ms")
        # offsets and eases
        offsets = [(0, 0)] * n
        if g("offset") is not None:
            tokens = g("offset").split()
            if len(tokens) != n:
                raise _Skip(at("offset"), f"offset has {_s(len(tokens), 'value')} for {_s(n, 'step')}: give one "
                                          "per step")
            offsets = []
            for t in tokens:
                m = re.fullmatch(r"([+-]?" + _N + r"),([+-]?" + _N + r")", t)
                if not m:
                    raise _Skip(at("offset"), f'offset "{t}" isn\'t x,y')
                offsets.append((_int(m.group(1)), _int(m.group(2))))
            for x, y in offsets:
                if max(abs(x), abs(y)) * self.scale > 48:
                    raise _Skip(at("offset"), f"offset {x},{y} moves {max(abs(x), abs(y)) * self.scale} "
                                              "screen px: at most 48")
        eases = [0] * n
        if g("ease") is not None:
            tokens = g("ease").split()
            for t in tokens:
                if t.lower() not in EASES:
                    raise _Skip(at("ease"), f'ease "{t}" isn\'t hold, linear, in, out or inout')
        travel_ease = None
        if g("travel_ease") is not None:
            t = g("travel_ease").strip()
            if t.lower() not in EASES:
                raise _Skip(at("travel_ease"), f'ease "{t}" isn\'t hold, linear, in, out or inout')
            travel_ease = EASES.index(t.lower())
        if g("ease") is not None:
            tokens = g("ease").split()
            if len(tokens) not in (1, n):
                raise _Skip(at("ease"), f"ease has {_s(len(tokens), 'value')} for {_s(n, 'step')}: give one, or "
                                        "one per step")
            eases = [EASES.index(t.lower()) for t in (tokens * n if len(tokens) == 1 else tokens)]
        # flags and labels
        flags = 0
        bools = {}
        for key, bit in (("mirror", FLAG_MIRROR), ("freeze", FLAG_FREEZE), ("heavy", FLAG_HEAVY),
                         ("snap", FLAG_SNAP)):
            if g(key) is not None:
                v = g(key).strip().lower()
                if v not in BOOLEANS:
                    raise _Skip(at(key), f"{key} must be yes or no")
                bools[key] = BOOLEANS[v]
                if BOOLEANS[v]:
                    flags |= bit
        if g("place") is not None:
            v = g("place").strip().lower()
            if v not in ("start", "end"):
                raise _Skip(at("place"), "place must be start or end")
            flags |= FLAG_PLACE_START if v == "start" else FLAG_PLACE_END
        weight10 = 10
        if g("weight") is not None:
            v = g("weight").strip()
            m = re.fullmatch(r"(" + _N + r")?(?:\.([0-9]))?", v)
            w10 = None
            if m and (m.group(1) or m.group(2)):
                w10 = _int(m.group(1) or "0") * 10 + _int(m.group(2) or "0")
            if w10 is None or not 1 <= w10 <= 100:
                raise _Skip(at("weight"), "weight must be 0.1-10 in steps of 0.1")
            weight10 = w10
        keys = {}
        for key in ("from", "to"):
            if g(key) is not None:
                v = g(key).strip()
                if not _ID.fullmatch(v):
                    raise _Skip(at(key), f'{key} "{v}": pose keys are 1-23 of a-z, 0-9 and -')
                twin = pose_twin(v)
                if len(twin) > 23:      # ASCII: characters are bytes
                    raise _Skip(at(key), f'{key} "{v}": its mirror twin "{twin}" is {len(twin)} bytes: at most 23')
                keys[key] = v
        repeat = 0
        if g("repeat") is not None:
            v = g("repeat").strip()
            if not _INT.fullmatch(v) or _int(v) > 8:
                raise _Skip(at("repeat"), "repeat must be 0-8")
            repeat = _int(v)
            if repeat * beats > 16:
                raise _Skip(at("repeat"), f"repeat {repeat} x {beats} beats is {repeat * beats} beats: at most 16")
        if flags & FLAG_FREEZE and self.freeze_move is not None:
            raise _Skip(at("freeze"), f'freeze is already set on move "{self.freeze_move}"')
        amounts = {}
        for key in ("sway", "bounce"):
            amounts[key] = 0
            if g(key) is not None:
                v = g(key).strip()
                if not _INT.fullmatch(v) or _int(v) > 16:
                    raise _Skip(at(key), f"{key} must be 0-16 (native px)")
                amounts[key] = _int(v)
        travel = 0
        if tv is not None:
            v = tv.strip()
            if not _SINT.fullmatch(v) or not -2 <= _int(v) <= 2:
                raise _Skip(at("travel"), "travel must be -2 to 2 (spots)")
            travel = _int(v)
            if travel and beats < 2:
                raise _Skip(at("travel"), "a travel move needs at least 2 beats")
        start_at = 0
        if g("start_at") is not None:
            v = g("start_at").strip().lower()
            if v not in START_AT:
                hint = ": did you mean centre?" if v == "center" else ""
                raise _Skip(at("start_at"), "start_at must be any, centre, side, left or right" + hint)
            start_at = START_AT.index(v)
        go = set()
        if g("go") is not None:
            for t in [t for t in re.split(r"[\s,]+", g("go").strip()) if t]:
                m = re.fullmatch(r"(" + _N + r")(?:-(" + _N + r"))?", t)
                a = _int(m.group(1)) if m else 0
                b = _int(m.group(2)) if m and m.group(2) else a
                if not m or not 1 <= a <= b <= n:
                    raise _Skip(at("go"), f'go: "{t}" isn\'t a step (1-{n}) or a range of them')
                go.update(range(a - 1, b))
        genres = []
        if g("genre") is not None:
            genres = [t.strip() for t in g("genre").split(",")]
            if any(not t for t in genres):
                raise _Skip(at("genre"), "empty genre tag")
            if len(genres) > 8:
                raise _Skip(at("genre"), f"more than 8 genre tags ({len(genres)})")
            for t in genres:
                if len(t.encode("utf-8")) > 63:
                    raise _Skip(at("genre"), f'genre tag "{t}" is {len(t.encode("utf-8"))} bytes: at most 63')
            for t in genres:
                if not _valid_text(t):
                    raise _Skip(at("genre"), f'genre tag "{t}" has a control character')
            for t in genres:
                if not fold_genre(t):
                    raise _Skip(at("genre"), f'genre tag "{t}" folds to nothing: use letters or digits Full '
                                             "folding can spell")
        when = 0
        if kind == IDLE:
            when = 3
            if g("when") is not None:
                v = g("when").strip().lower()
                if v not in WHEN[1:]:
                    raise _Skip(at("when"), "when must be quiet, listening or any")
                when = WHEN.index(v)
        song = None
        if g("song_bpm") is not None:
            m = _RANGE.fullmatch(g("song_bpm").strip())
            if not m or not 1 <= _int(m.group(1)) <= _int(m.group(2)) <= 255:
                raise _Skip(at("song_bpm"), "song_bpm must be lo-hi, 1-255")
            song = (_int(m.group(1)), _int(m.group(2)))
        if kind == BEAT and (lo, hi) != (0, 0):
            if not (36 <= lo <= 200 and 36 <= hi <= 200):
                raise _Skip(at("bpm"), f"bpm {lo}-{hi}: each bound must be 36-200")
            if lo > hi:
                raise _Skip(at("bpm"), f"bpm {lo}-{hi}: lo is above hi")
        if kind == BEAT and reach(lo, hi, beats, travel) is None:
            raise _Skip(at("bpm"), _reach_error(lo, hi, beats, travel))
        # the move as the engine sees it: frame i is step i's cell, drawn as on the sheet
        boxes = [_tight_box(self.grid(c)) for c, _ in cells]
        steps = tuple(Step(i, False, durs[i], offsets[i][0], offsets[i][1], eases[i], i in go)
                      for i in range(n))
        if travel_ease is None:
            travel_ease = 4 if travel else 0
        move = Move(name, kind, beats, lo, hi, weight10, when, flags, repeat,
                    NEEDS_SONG_BPM if song else 0, travel, start_at if travel else 0,
                    travel_ease if travel else 0, amounts["bounce"], amounts["sway"],
                    keys.get("from"), keys.get("to"),
                    pose_twin(keys["from"]) if "from" in keys else None,
                    pose_twin(keys["to"]) if "to" in keys else None,
                    tuple(genres), song[0] if song else 0, song[1] if song else 0, steps)
        geom = self._geom(0)
        if travel:
            orients = [False, True] if flags & FLAG_MIRROR else [False]
            if not any(start_spots(geom, boxes, move, o) for o in orients):
                raise _Skip(at("travel"), "can't travel from any spot: too wide there, or start_at rules "
                                          "every spot out")
        b = budget(geom, boxes, move)
        if b.light == "red" and not flags & FLAG_HEAVY:
            raise _Skip(sec.line, f"budget red: peak {_n(b.peak)} px is over 20,000; set heavy = yes to keep it")
        if flags & FLAG_FREEZE:
            self.freeze_move = name
        return {"name": name, "sec": sec, "move": move, "cells": cells, "boxes": boxes, "budget": b,
                "snaps": snaps, "cells_at": cells_at}

    def _geom(self, frames):
        return Geom(self.scale, self.faces, self.cw, self.ch, self.ax, self.ay, self.shadow, frames, 0)

    def _report_move(self, mv):
        sec, move, b = mv["sec"], mv["move"], mv["budget"]
        name = mv["name"]
        self.msg(self.ini(sec.line, f'move "{name}": {_what(move)}; {_light_text(b)}'))
        # the echo table
        unit = "ticks" if move.kind == BEAT else "ms"
        rows = [("step", "cell", unit, "offset", "ease", "")]
        for i, (st, (cell, text)) in enumerate(zip(move.steps, mv["cells"]), 1):
            dur = st.dur if move.kind == BEAT else 10 * st.dur
            rows.append((str(i), text + (" (blank)" if self.blank(cell) else ""), str(dur), f"{st.dx},{st.dy}",
                         EASES[st.ease], "go" if st.go and move.travel else ""))
        widths = [max(len(r[k]) for r in rows) for k in range(6)]
        for r in rows:
            line = "    " + "  ".join(r[k].ljust(widths[k]) for k in range(6))
            self.items.append(("echo", line.rstrip()))
        at = sec.at
        timing_at = at("timing") if sec.get("timing") is not None else sec.line
        for text, ticks in mv["snaps"]:
            self.msg(self.ini(at("timing"), f'warning: move "{name}": timing {text} snapped to {beats_text(ticks)}'))
        if move.kind == BEAT:
            starts, _ = _starts(move)
            for beat in range(2, move.beats + 1):
                tick = 12 * (beat - 1)
                for i, st in enumerate(move.steps):
                    if starts[i] < tick < starts[i] + st.dur:
                        self.msg(self.ini(timing_at, f'warning: move "{name}": beat {beat} lands inside step '
                                                     f"{i + 1} ({mv['cells'][i][1]}): no step starts on it; fine "
                                                     "for a hold, otherwise set timing = so one does"))
        if move.kind == BEAT:
            first = move.steps[0]
            if (first.dx, first.dy) != (0, 0):
                self.msg(self.ini(at("offset"), f'warning: move "{name}": the first key is {first.dx},{first.dy}, '
                                                "not 0,0: the dancer jumps when another move follows"))
        if not move.travel:
            # C.6's feet drift: the feet of each standing step at its key, and
            # where a glide (an ease other than hold, to a key on the ground)
            # carries them by the step's end
            cs, n = [], len(move.steps)
            for i, ((c, _), st) in enumerate(zip(mv["cells"], move.steps)):
                grid = self.grid(c)
                box = _tight_box(grid)
                if st.dy or not (box.h and box.y0 + box.h == self.ay):
                    continue
                row = grid[self.ay - 1]
                xs = [x for x, v in enumerate(row) if v]
                cs.append(xs[0] + xs[-1] + 2 * st.dx)
                nxt = move.steps[(i + 1) % n]
                if st.ease != 0 and nxt.dx != st.dx and nxt.dy == 0:
                    cs.append(xs[0] + xs[-1] + 2 * nxt.dx)
            if cs and max(cs) - min(cs) > 2:
                self.msg(self.ini(sec.line, f'warning: move "{name}": the feet drift {(max(cs) - min(cs) + 1) // 2} '
                                            "native px across its standing frames"))
        if (self.faces == 1 and move.travel < 0) or (self.faces == 2 and move.travel > 0):
            way, faces = ("left", "right") if move.travel < 0 else ("right", "left")
            self.msg(self.ini(at("travel"), f'warning: move "{name}": travels {way} while the art faces {faces}'))
        if move.needs & NEEDS_SONG_BPM:
            self.msg(self.ini(at("song_bpm"), f'warning: move "{name}": song_bpm needs track BPM data: skipped '
                                              "in this version"))
        if b.light == "red":
            self.msg(self.ini(at("heavy"), f'warning: move "{name}": budget red (peak {_n(b.peak)} px), kept '
                                           "because heavy = yes"))
        for i, ((cell, text), st) in enumerate(zip(mv["cells"], move.steps), 1):
            if self.blank(cell):
                how_long = beats_text(st.dur) if move.kind == BEAT else f"{10 * st.dur} ms"
                self.msg(self.ini(mv["cells_at"], f'note: move "{name}": step {i} ({text}) is blank: the dancer '
                                                  f"disappears for {how_long}"))
        if move.genres:
            self.msg(self.ini(at("genre"), f'note: move "{name}": genre tags keep it dormant until tracks '
                                           "carry genres"))

    # -- frames (A.14) and the pack
    def _frames(self):
        index, self.frame_grids, self.frame_cells = {}, [], []

        def place(cell):
            grid = self.grid(cell)
            if grid in index:
                f, m = index[grid], False
            else:
                mg = _mirror_grid(grid, self.ax)
                if mg is not None and mg in index:
                    f, m = index[mg], True
                else:
                    f, m = len(self.frame_grids), False
                    index[grid] = f
                    self.frame_grids.append(grid)
                    self.frame_cells.append([])
            if (cell, m) not in self.frame_cells[f]:
                self.frame_cells[f].append((cell, m))
            return f, m

        place(self.rest)
        self.pose_keys = []
        for mv in self.kept:
            move = mv["move"]
            steps = []
            for (cell, _), st in zip(mv["cells"], move.steps):
                f, m = place(cell)
                steps.append(st._replace(frame=f, mirrored=m))
            mv["move"] = move._replace(steps=tuple(steps))
            for k in (move.from_key, move.to_key, move.from_key_m, move.to_key_m):
                if k is not None and k not in self.pose_keys:
                    self.pose_keys.append(k)
        self.boxes = [_tight_box(g) for g in self.frame_grids]
        for f, (grid, box) in enumerate(zip(self.frame_grids, self.boxes)):
            cell = _cell_name(self.frame_cells[f][0][0])
            if box.h and box.y0 + box.h == self.ay:
                top = GROUND_Y + (box.y0 - self.ay) * self.scale
                if top < TOAST_ROWS:
                    self.msg(f"{self.sheet}: warning: frame {cell}: standing, it reaches stage row {top}, under "
                             "the toast (rows 0-35)")
            runs = _runs(grid, box)
            if runs > RUNS_WARN:
                self.msg(f"{self.sheet}: warning: frame {cell}: {runs} runs, over 300: slow to draw")
        if len(self.frame_grids) > MAX_FRAMES:
            self.fail(f"error: {len(self.frame_grids)} frames: at most {MAX_FRAMES}")
        if len(self.kept) > MAX_MOVES:
            self.fail(f"error: {len(self.kept)} moves: at most {MAX_MOVES}")
        if len(self.pose_keys) > MAX_POSE_KEYS:
            self.fail(f"error: {len(self.pose_keys)} pose keys: at most {MAX_POSE_KEYS}")

    def _write(self):
        meta = (_string(self.id) + _string(self.name) + _string(self.author) + _string(self.licence)
                + _string(self.url) + _string(self.version) + bytes((len(self.kept),)))
        geom = struct.pack("<8BHH", self.scale, self.faces, self.cw, self.ch, self.ax, self.ay, self.shadow, 0,
                           len(self.frame_grids), 0)
        pals = bytearray(96)
        for i, (c, ic) in enumerate(zip(self.palette, self.idle), 1):
            pals[3 * i:3 * i + 3] = bytes(c)
            pals[48 + 3 * i:48 + 3 * i + 3] = bytes(ic)
        thmb = thumbnail(self.frame_grids[0], self.boxes[0])
        ftab, fdat = bytearray(), bytearray()
        for grid, box in zip(self.frame_grids, self.boxes):
            data = encode_frame(grid, box) if box.w else b""
            ftab += struct.pack("<4BIHH", box.x0, box.y0, box.w, box.h, len(fdat) if data else 0, len(data), 0)
            fdat += data
        strs = bytearray((len(self.pose_keys), 0, 0, 0))
        for k in self.pose_keys:
            strs += _string(k)
        name_at, run_at = [], {}
        for mv in self.kept:
            name_at.append(len(strs))
            strs += _string(mv["name"])
        for mv in self.kept:
            run = mv["move"].genres
            if run and run not in run_at:
                run_at[run] = len(strs)
                for tag in run:
                    strs += _string(tag)
        key_id = {k: i + 1 for i, k in enumerate(self.pose_keys)}
        movs = bytearray((len(self.kept), 32, 0, 0))
        step = bytearray()
        first = 0
        for mv, name_off in zip(self.kept, name_at):
            m = mv["move"]
            movs += struct.pack("<H9Bb8BHBBHBBI", name_off, m.kind, m.beats, m.pace_lo, m.pace_hi, m.weight10,
                                m.when, m.flags, m.repeat, m.needs, m.travel, m.start_at, m.travel_ease, m.bounce,
                                m.sway, key_id.get(m.from_key, 0), key_id.get(m.to_key, 0),
                                key_id.get(m.from_key_m, 0), key_id.get(m.to_key_m, 0),
                                run_at.get(m.genres, 0), len(m.genres), len(m.steps), first, m.song_lo,
                                m.song_hi, 0)
            for st in m.steps:
                step += struct.pack("<HHbbBB", st.frame | (0x8000 if st.mirrored else 0), st.dur, st.dx, st.dy,
                                    st.ease, 1 if st.go else 0)
            first += len(m.steps)
        data = _assemble([("META", meta), ("GEOM", geom), ("PALS", bytes(pals)), ("THMB", thmb),
                          ("FTAB", bytes(ftab)), ("STRS", bytes(strs)), ("MOVS", bytes(movs)),
                          ("STEP", bytes(step)), ("FDAT", bytes(fdat))])
        if len(data) > MAX_PACK:
            self.fail(f"error: the pack is {_n(len(data))} bytes: at most 131,072")
        self.data = data
        try:
            self.pack = read_pack(data)
        except DanceError as e:     # a compiler bug: report it rather than raise (compile_dir never raises)
            self.fail(f"error: internal: the pack doesn't read back ({e}): please report it")

    # -- the coverage report (C.5)
    def _coverage(self):
        beat = [mv["move"] for mv in self.kept if mv["move"].kind == BEAT]
        idle = [mv["move"] for mv in self.kept if mv["move"].kind == IDLE]
        rows = [m for m in beat if not m.needs]
        tempos = (72, 80, 90, 100, 110, 120, 130, 140, 150, 160, 176)
        labels = [m.name + ("*" if m.genres else "") for m in rows]
        width = max([len("T")] + [len(t) for t in labels]) + 2
        cov = ["coverage: pace at dance tempo T (h = half time, d = double time, . = not eligible)",
               "  " + "T".ljust(width) + "".join(str(t).rjust(5) for t in tempos)]
        for m, label in zip(rows, labels):
            cells = []
            for t in tempos:
                k = eligible_k(10 * t, m.pace_lo, m.pace_hi, None, m.beats, m.travel)
                if k is None:
                    cells.append(".")
                else:
                    pace = Fraction(t) * Fraction(2) ** k
                    p = str(pace.numerator) if pace.denominator == 1 else "%.1f" % float(pace)
                    cells.append(p + {0: "", -1: "h", 1: "d"}[k])
            cov.append("  " + label.ljust(width) + "".join(c.rjust(5) for c in cells))
        if any(m.genres for m in rows):
            cov.append("  * genre-tagged: dormant until tracks carry genres")
        names = {w: [m.name for m in idle if m.when in (w, 3)] for w in (1, 2)}
        parts = []
        for w, other in ((1, 2), (2, 1)):
            if names[w]:
                parts.append(f"{WHEN[w]}: " + ", ".join(names[w]))
            elif names[other]:
                parts.append(f"{WHEN[w]}: (the {WHEN[other]} idles)")
            else:
                parts.append(f"{WHEN[w]}: auto-idle")
        cov.append("idles: " + "; ".join(parts))
        geom, boxes = self._geom(len(self.frame_grids)), self.boxes
        edges = []
        for m in beat:
            orients = [False, True] if m.flags & FLAG_MIRROR else [False]
            if m.travel:
                def spots(o):
                    s = start_spots(geom, boxes, m, o)
                    return " and ".join("%+d" % x if x else "0" for x in s) if s else "no spot"
                text = f"travels from {spots(False)}" + (f"; mirrored from {spots(True)}" if len(orients) > 1 else "")
                edges.append(f"  {m.name}: {text}")
            else:
                left = not all(fits(geom, boxes, m, o, -1) for o in orients)
                right = not all(fits(geom, boxes, m, o, 1) for o in orients)
                if left and right:
                    edges.append(f"  {m.name}: clamped at both edge spots")
                elif left or right:
                    edges.append(f"  {m.name}: clamped at spot {'-1' if left else '+1'}")
        if edges:
            cov += ["edges:"] + edges
        else:
            cov.append("edges: every move fits at both edge spots")
        for line in cov:
            self.items.append(("cov", line))
        if not self.kept:
            self.msg("warning: no moves: only the engine's auto-bounce and auto-idle will play")
            return
        plain = [m for m in beat if not m.genres and not m.needs]
        gap = None
        for t in range(72, 177):
            none = not any(eligible_k(10 * t, m.pace_lo, m.pace_hi, None, m.beats, m.travel) is not None
                           for m in plain)
            if none and gap is None:
                gap = t
            if gap is not None and (not none or t == 176):
                last = t if none else t - 1
                span = str(gap) if last == gap else f"{gap}-{last}"
                self.msg(f"warning: no beat move at dance tempo {span}: auto-bounce plays there")
                gap = None

    def _strict(self):
        if not self.strict:
            return True
        ok = True
        if self.licence not in LICENCE_ALLOWLIST:
            self.msg(self.ini(self.dancer.at("license"), f'error: license "{self.licence}" isn\'t on the built-in '
                                                         "allowlist: " + ", ".join(LICENCE_ALLOWLIST)))
            ok = False
        if self.skipped:
            self.msg(f"error: --strict: {_s(len(self.skipped), 'move')} skipped")
            ok = False
        return ok

    def _summary(self):
        written = len(self.kept) + len(self.skipped)
        self.msg(f"{self.id}: {_s(len(self.kept), 'move')} ({len(self.skipped)} of {written} skipped), "
                 f"{_s(len(self.frame_grids), 'frame')}, {_n(len(self.data))} bytes")


def _string(text):
    raw = text.encode("utf-8")
    return bytes((len(raw),)) + raw + b"\0"


def _assemble(chunks):
    """A.2-A.4: the header, the directory and the chunks, 4-byte aligned."""
    head = 32 + 12 * len(chunks)
    body = bytearray(head)
    directory = []
    for tag, data in chunks:
        body += bytes(-len(body) % 4)
        directory.append(struct.pack("<4sII", tag.encode("ascii"), len(body), len(data)))
        body += data
    body += bytes(-len(body) % 4)
    body[32:head] = b"".join(directory)
    body[16:20] = struct.pack("<HH", len(chunks), 0)
    body[0:12] = struct.pack("<4sHHI", b"MPDN", FORMAT, MIN_READER, len(body))
    body[12:16] = struct.pack("<I", crc32(body[16:]))
    return bytes(body)


def _compile(path, strict=False):
    c = _Compiler(path, strict)
    ok = c.run()
    return c, c.result(ok)


def compile_dir(path, strict=False):
    """Compiles a dancer folder (C.5): a Result, never an exception."""
    return _compile(path, strict)[1]


# ---- the reader (A.15)
def _read_string(data, p, end):
    """(text, next offset) of the string at p, or None, checked in A.1's order:
    no byte is read before a check puts it inside [p, end)."""
    if p + 2 > end:                 # the length byte and the NUL's place
        return None
    n = data[p]
    if n > end - p - 2 or data[p + 1 + n] != 0:
        return None
    raw = data[p + 1:p + 1 + n]
    if 0 in raw:
        return None
    text = _string_ok(raw)
    return None if text is None else (text, p + 2 + n)


def _decode_frame(data, off, length, box, cell_w, cell_h):
    """A.10's dry-run decode into a cell grid, or None when it fails."""
    grid = [bytearray(cell_w) for _ in range(cell_h)]
    p, end = off, off + length
    for y in range(box.h):
        if p + 2 > end:
            return None
        skip, n = data[p], data[p + 1]
        p += 2
        if p + n > end or skip > box.w:     # skip + the runs <= w, even for a row with no tokens
            return None
        x = skip
        for t in data[p:p + n]:
            run, index = (t >> 4) + 1, t & 15
            if x + run > box.w:
                return None
            if index:
                for i in range(x, x + run):
                    grid[box.y0 + y][box.x0 + i] = index
            x += run
        p += n
    if p != end:
        return None
    return tuple(bytes(r) for r in grid)


def read_pack(data, check_crc=True):
    """Reads and validates an MPDN pack as the device does for a card pack
    (A.15, the budget included). Raises DanceError(reason) for a rejected
    pack; dropped moves are listed in Pack.ignored as (MOVS index, reason)."""
    data = bytes(data)
    size = len(data)
    if size < 32:
        raise DanceError("too short")
    if data[:4] != b"MPDN":
        raise DanceError("not an MPDN pack")
    _, min_reader, total, crc = struct.unpack_from("<HHII", data, 4)
    if min_reader > MIN_READER:
        raise DanceError("needs a newer player")
    if total != size:
        raise DanceError(f"size {total} isn't the file's {size}")
    if total > MAX_PACK:
        raise DanceError(f"too big ({total} bytes)")
    if check_crc and crc32(data[16:]) != crc:
        raise DanceError("bad checksum")
    count = struct.unpack_from("<H", data, 16)[0]
    if count == 0 or count > MAX_CHUNKS or 32 + 12 * count > total:
        raise DanceError(f"bad chunk count {count}")
    dir_end = 32 + 12 * count
    chunks, seen = {}, []
    for i in range(count):
        tag, off, length = struct.unpack_from("<4sII", data, 32 + 12 * i)
        t = _tag_text(tag)
        if off % 4:
            raise DanceError(f"chunk {t}: misaligned")
        # A.3's order, which can't wrap in 32-bit arithmetic: off first, then size
        if off < dir_end or off > total or length > total - off:
            raise DanceError(f"chunk {t}: out of bounds")
        if tag in chunks:
            raise DanceError(f"chunk {t} twice")
        for ptag, poff, plen in seen:
            if length and plen and off < poff + plen and poff < off + length:
                raise DanceError(f"chunks {_tag_text(ptag)} and {t} overlap")
        chunks[tag] = (off, length)
        seen.append((tag, off, length))
    for name in CHUNKS:
        if name.encode("ascii") not in chunks:
            raise DanceError(f"no {name} chunk")

    def chunk(name):
        return chunks[name.encode("ascii")]

    # META
    off, length = chunk("META")
    p, end, meta = off, off + length, []
    for field, lo, hi in (("id", 1, 23), ("name", 1, 24), ("author", 1, 40), ("licence", 1, 40),
                          ("url", 0, 80), ("version", 0, 16)):
        s = _read_string(data, p, end)
        if s is None or not lo <= len(s[0].encode("utf-8")) <= hi or (field == "id" and not _ID.fullmatch(s[0])):
            raise DanceError(f"META: bad {field}")
        meta.append(s[0])
        p = s[1]
    if p >= end:
        raise DanceError("META: bad count")
    declared = data[p]
    # GEOM
    off, length = chunk("GEOM")
    if length < 12:
        raise DanceError("GEOM: too short")
    scale, faces, cw, ch, ax, ay, shadow, _, frames, rest = struct.unpack_from("<8BHH", data, off)
    for field, good in (("scale", 1 <= scale <= 4), ("faces", faces <= 2),
                        ("cellW", cw >= 1 and cw * scale <= 160), ("cellH", ch >= 1 and ch * scale <= 160),
                        ("anchorX", ax < cw),
                        ("anchorY", 1 <= ay <= ch and ay * scale <= GROUND_Y and (ch - ay) * scale <= 10),
                        ("shadowW", shadow <= 160), ("frame count", 1 <= frames <= MAX_FRAMES),
                        ("rest frame", (rest & 0x7FFF) < frames)):
        if not good:
            raise DanceError(f"GEOM: bad {field}")
    geom = Geom(scale, faces, cw, ch, ax, ay, shadow, frames, rest)
    off, length = chunk("PALS")
    if length != 96:
        raise DanceError(f"PALS: {length} bytes, not 96")
    pals = data[off:off + 96]
    palette = tuple(tuple(pals[3 * i:3 * i + 3]) for i in range(16))
    idle_palette = tuple(tuple(pals[48 + 3 * i:48 + 3 * i + 3]) for i in range(16))
    off, length = chunk("THMB")
    if length != 800:
        raise DanceError(f"THMB: {length} bytes, not 800")
    thumb = data[off:off + 800]
    off, length = chunk("FTAB")
    if length != 12 * frames:
        raise DanceError(f"FTAB: {length} bytes for {frames} frames")
    fdat_off, fdat_len = chunk("FDAT")
    boxes, grids = [], []
    for i in range(frames):
        x0, y0, w, h, foff, flen, _ = struct.unpack_from("<4BIHH", data, off + 12 * i)
        if (w == 0) != (h == 0) or (w == 0) != (flen == 0) or x0 + w > cw or y0 + h > ch:
            raise DanceError(f"frame {i}: bad box")
        if foff > fdat_len or flen > fdat_len - foff:      # A.9: off first, so nothing wraps
            raise DanceError(f"frame {i}: data out of bounds")
        box = Box(x0, y0, w, h)
        grid = _decode_frame(data, fdat_off + foff, flen, box, cw, ch) if w else \
            tuple(bytes(cw) for _ in range(ch))
        if grid is None:
            raise DanceError(f"frame {i}: bad data")
        boxes.append(box)
        grids.append(grid)
    if boxes[rest & 0x7FFF].w == 0:
        raise DanceError("the rest frame is empty")
    soff, slen = chunk("STRS")
    if slen < 4:
        raise DanceError("STRS: too short")
    send = soff + slen
    pcount = data[soff]
    pose_keys, p = [], soff + 4
    for i in range(1, pcount + 1):
        s = _read_string(data, p, send)
        if s is None or not _ID.fullmatch(s[0]):
            raise DanceError(f"STRS: bad pose key {i}")
        pose_keys.append(s[0])
        p = s[1]
    moff, mlen = chunk("MOVS")
    if mlen < 4:
        raise DanceError("MOVS: too short")
    mcount, rlen = data[moff], data[moff + 1]
    if mcount > MAX_MOVES:
        raise DanceError(f"MOVS: {mcount} moves")
    if rlen < 32:
        raise DanceError(f"MOVS: record length {rlen}")
    if mlen < 4 + mcount * rlen:
        raise DanceError("MOVS: too short")
    stoff, stlen = chunk("STEP")
    if stlen % 8:
        raise DanceError(f"STEP: {stlen} bytes")

    def strs_string(o, check):
        if o < 4:
            return None
        s = _read_string(data, soff + o, send)
        return s if s is not None and check(s[0]) else None

    moves, ignored = [], []
    for i in range(mcount):
        rec = struct.unpack_from("<H9Bb8BHBBHBBI", data, moff + 4 + i * rlen)
        move = _decode_move(data, pose_keys, strs_string, soff, stoff, stlen, rec)
        moves.append(move)
        try:
            _check_move(geom, boxes, pose_keys, strs_string, soff, stlen, rec, move)
        except DanceError as e:
            ignored.append((i, str(e)))
    return Pack(size, crc, meta[0], meta[1], meta[2], meta[3], meta[4], meta[5], declared, geom, palette,
                idle_palette, bytes(thumb), tuple(boxes), tuple(grids), tuple(pose_keys), tuple(moves),
                tuple(ignored))


def _genre_run(strs_string, soff, first, count):
    """The genre run's tags, or None when one isn't a good tag (A.11)."""
    tags, p = [], first
    for _ in range(count):
        s = strs_string(p, lambda t: 1 <= len(t.encode("utf-8")) <= 63 and fold_genre(t))
        if s is None:
            return None
        tags.append(s[0])
        p = s[1] - soff
    return tags


def _decode_move(data, pose_keys, strs_string, soff, stoff, stlen, rec):
    """A MOVS record as a Move, as far as it decodes: a bad name is None, steps
    past STEP's end are (), a bad genre run is (), a bad pose key id is None."""
    (name_off, kind, beats, lo, hi, weight10, when, flags, repeat, needs, travel, start_at, travel_ease,
     bounce, sway, fk, tk, fkm, tkm, gfirst, gcount, scount, sfirst, song_lo, song_hi, _) = rec
    s = strs_string(name_off, lambda t: _ID.fullmatch(t))
    steps = []
    if sfirst + scount <= stlen // 8:
        for k in range(scount):
            frame, dur, dx, dy, ease, sflags = struct.unpack_from("<HHbbBB", data, stoff + 8 * (sfirst + k))
            steps.append(Step(frame & 0x7FFF, bool(frame & 0x8000), dur, dx, dy, ease, bool(sflags & 1)))
    genres = _genre_run(strs_string, soff, gfirst, gcount) if gcount <= 8 else None

    def key(i):
        return pose_keys[i - 1] if 0 < i <= len(pose_keys) else None

    return Move(s[0] if s else None, kind, beats, lo, hi, weight10, when, flags, repeat, needs, travel, start_at,
                travel_ease, bounce, sway, key(fk), key(tk), key(fkm), key(tkm), tuple(genres or ()), song_lo,
                song_hi, tuple(steps))


def _check_move(geom, boxes, pose_keys, strs_string, soff, stlen, rec, move):
    """A.15's move checks, in order: DanceError(reason) at the first failure."""
    (name_off, kind, beats, lo, hi, weight10, when, flags, repeat, needs, travel, start_at, travel_ease,
     bounce, sway, fk, tk, fkm, tkm, gfirst, gcount, scount, sfirst, song_lo, song_hi, _) = rec
    if move.name is None:
        raise DanceError("bad name")
    if kind > 1:
        raise DanceError("bad kind")
    if scount == 0 or scount > MAX_STEPS or sfirst + scount > stlen // 8:
        raise DanceError("bad steps")
    steps = move.steps
    for st in steps:
        if st.frame >= geom.frame_count:
            raise DanceError(f"frame {st.frame} out of range")
    durs = [st.dur for st in steps]
    if kind == BEAT:
        if beats not in (1, 2, 4, 8):
            raise DanceError(f"{beats} beats")
        for k, d in enumerate(durs, 1):
            if d < 3:
                raise DanceError(f"step {k}: {d} ticks")
        if sum(durs) != 12 * beats:
            raise DanceError(f"{sum(durs)} of {12 * beats} ticks")
    else:
        for k, d in enumerate(durs, 1):
            if not 20 <= d <= 500:
                raise DanceError(f"step {k}: {10 * d} ms")
        if not 50 <= sum(durs) <= 1200:
            raise DanceError(f"{10 * sum(durs)} ms in all")
        if (beats or lo or hi or repeat or bounce or sway or travel or gcount
                or flags & (FLAG_FREEZE | FLAG_PLACE_START | FLAG_PLACE_END)):
            raise DanceError("beat fields on a no-BPM move")
    if (kind == BEAT and when != 0) or (kind == IDLE and not 1 <= when <= 3):
        raise DanceError("bad when")
    if kind == BEAT and (lo, hi) != (0, 0):
        if not (36 <= lo <= 200 and 36 <= hi <= 200) or lo > hi:
            raise DanceError(f"pace {lo}-{hi}")
        if reach(lo, hi, beats, travel) is None:
            raise DanceError(f"range {lo}-{hi} unreachable")
    if weight10 == 0 or weight10 > 100:
        raise DanceError(f"weight {weight10}")
    if repeat > 8 or repeat * beats > 16:
        raise DanceError(f"repeat {repeat}")
    if bounce > 16:
        raise DanceError(f"bounce {bounce}")
    if sway > 16:
        raise DanceError(f"sway {sway}")
    if not -2 <= travel <= 2 or (travel and beats < 2):
        raise DanceError(f"travel {travel}")
    if start_at > 4:
        raise DanceError("bad start_at")
    if travel_ease > 4 or any(st.ease > 4 for st in steps):
        raise DanceError("bad ease")
    if flags & FLAG_PLACE_START and flags & FLAG_PLACE_END:
        raise DanceError("bad place")
    for st in steps:
        if abs(st.dx) * geom.scale > 48 or abs(st.dy) * geom.scale > 48:
            raise DanceError(f"offset {st.dx},{st.dy}")
    if max(fk, tk, fkm, tkm) > len(pose_keys):
        raise DanceError("bad pose key")
    if gcount > 8 or (gcount and _genre_run(strs_string, soff, gfirst, gcount) is None):
        raise DanceError("bad genre")
    if needs & NEEDS_SONG_BPM:
        raise DanceError("song_bpm needs track BPM data: skipped in this version")
    if needs:
        raise DanceError("needs 0x%02X" % needs)
    if kind == BEAT and travel:
        orients = [False, True] if flags & FLAG_MIRROR else [False]
        if not any(start_spots(geom, boxes, move, o) for o in orients):
            raise DanceError("travels from no spot")
    b = budget(geom, boxes, move)
    if b.light == "red" and not flags & FLAG_HEAVY:
        raise DanceError(f"budget red ({b.peak} px)")
    return move


# ---- generated files (C.8)
def _head(licence, holder, source):
    return [f"// SPDX-License-Identifier: {licence}", f"// Copyright (C) {holder}",
            f"// Generated by tools/dancepack.py from {source}. Do not edit:",
            "// change the source and run `python tools/dancepack.py builtin`."]


def _byte_lines(data, indent="    "):
    return [indent + ", ".join("0x%02X" % b for b in data[i:i + 16]) + "," for i in range(0, len(data), 16)]


_PIXEL = ".123456789abcdef"


def _pack_source(comp, source, stem, cpp):
    """A compiled pack as a .h (static) or a .cpp (extern), symbols k<stem>."""
    pack, data = comp.pack, comp.data
    sym = "k" + stem
    hashes = [frame_hash(g) for g in pack.grids]
    out = _head(pack.licence, pack.author, source)
    out += ["//",
            f'// The dancer "{pack.name}" (id {pack.id}) by {pack.author}, {pack.licence}:',
            f"// {_s(len(pack.moves), 'move')}, {_s(len(pack.grids), 'frame')}, {_n(len(data))} bytes; cell "
            f"{pack.geom.cell_w}x{pack.geom.cell_h} at scale {pack.geom.scale}, anchor "
            f"{pack.geom.anchor_x},{pack.geom.anchor_y}, shadow {pack.geom.shadow_w}.",
            "// The moves, in MOVS order:"]
    names = ", ".join(m.name for m in pack.moves)
    while names:
        cut = len(names) if len(names) <= 74 else names.rfind(", ", 0, 74) + 1
        out.append("//   " + names[:cut].strip())
        names = names[cut:].strip()
    for i, reason in pack.ignored:
        out.append(f"// A v1 reader ignores move {i} ({pack.moves[i].name}): {reason}.")
    out.append("// Each frame: the cells it stores (m: drawn mirrored), its box, its pixels.")
    for f, (grid, box) in enumerate(zip(pack.grids, pack.boxes)):
        cells = ", ".join(_cell_name(c) + (" (m)" if m else "") for c, m in comp.frame_cells[f])
        out.append("//")
        out.append(f"// Frame {f}: {cells}; box {box.x0},{box.y0} {box.w}x{box.h}; hash 0x%08X" % hashes[f])
        for row in grid:
            out.append("//   " + "".join(_PIXEL[v] for v in row))
    if not cpp:
        out += ["#pragma once"]
    out += ["#include <cstddef>", "#include <cstdint>", "", "namespace dance {", ""]
    if cpp:
        out += [f"extern const uint8_t {sym}[{len(data)}];", f"extern const size_t {sym}Size;",
                f"extern const uint32_t {sym}FrameHash[{len(hashes)}];", f"extern const int {sym}FrameCount;", ""]
        q = "const"
    else:
        q = "static const"
    out.append(f"alignas(4) {q} uint8_t {sym}[{len(data)}] = {{")
    out += _byte_lines(data)
    out += ["};", f"{q} size_t {sym}Size = {len(data)};", "",
            "// FNV-1a 32 of each frame's cell grid (A.16).", f"{q} uint32_t {sym}FrameHash[{len(hashes)}] = {{"]
    for i in range(0, len(hashes), 6):
        out.append("    " + ", ".join("0x%08Xu" % h for h in hashes[i:i + 6]) + ",")
    out += ["};", f"{q} int {sym}FrameCount = {len(hashes)};", "", "}  // namespace dance", ""]
    return "\n".join(out)


ELIGIBILITY_T10 = (720, 800, 870, 880, 900, 901, 957, 1200, 1600, 1740, 1760)
ELIGIBILITY_RANGES = ((0, 0), (36, 60), (36, 200), (60, 100), (88, 88), (90, 130), (100, 160), (110, 180),
                      (144, 180), (150, 200), (170, 180), (177, 200))
ELIGIBILITY_PREV = (None, -1, 0, 1)
ELIGIBILITY_BEATS = (1, 2, 4, 8)
ELIGIBILITY_TRAVEL = (0, 1)        # travel 1 only with 2 beats or more (a travel move has them)
PACE_T10 = (720, 800, 870, 880, 900, 901, 957, 969, 970, 1030, 1031, 1200, 1600, 1740, 1760)
PACE_RANGES = ELIGIBILITY_RANGES + ((100, 100),)
FOLD_TAGS = ("Hip Hop", "hip-hop", "HIP_HOP", "Drum & Bass", "drum n bass", "Drum ’n’ Bass", "Drum&n&Bass",
             "R&B", "r+b", "d&b", "rock'n'roll", "n", "a n", "n n n", "&", "C++", "Électro", "Straße",
             "Æther", "Œuvre", "Łódź", "Ñandú", "½ step", "©",
             "Synth–Pop", "K-Pop", "90s", "Música Popular Brasileira", "日本", "  ", "")


def _vector_head(source, what):
    return _head("GPL-3.0-or-later", "2026 IrosTheBeggar", source) + [
        "//", f"// {what}", "// docs/DANCE-SPRITES.md, Appendices B and C.8. test_dancepack.py and the",
        "// C++ suites pin the same values.",
        "#pragma once", "#include <cstdint>", "", "namespace dancevec {", ""]


def _k(k):
    return "kNoK" if k is None else str(k)


def _eligibility_h():
    out = _vector_head("its own tables", "EligibilityVectors.h: B.7's eligible_k on a grid of dance tempos "
                                         "(T10), ranges,\n// previous octaves, move lengths and travel; and "
                                         "pace_holds on a grid of its own.")
    out += ["constexpr int8_t kNoK = 127;  // prevK: no previous ranged move; k: not eligible",
            "struct Eligibility { uint16_t t10; uint8_t lo, hi; int8_t prevK; uint8_t beats; int8_t travel; "
            "int8_t k; };",
            "static const Eligibility kEligibility[] = {"]
    n = 0
    for t10 in ELIGIBILITY_T10:
        for lo, hi in ELIGIBILITY_RANGES:
            for prev in ELIGIBILITY_PREV:
                for beats in ELIGIBILITY_BEATS:
                    for travel in ELIGIBILITY_TRAVEL if beats >= 2 else (0,):
                        k = eligible_k(t10, lo, hi, prev, beats, travel)
                        out.append(f"    {{{t10}, {lo}, {hi}, {_k(prev)}, {beats}, {travel}, {_k(k)}}},")
                        n += 1
    out += ["};", f"static const int kEligibilityCount = {n};", "",
            "struct PaceHolds { uint16_t t10; int8_t k; uint8_t lo, hi; uint8_t holds; };  // holds: 1 or 0",
            "static const PaceHolds kPaceHolds[] = {"]
    m = 0
    for t10 in PACE_T10:
        for k in (-1, 0, 1):
            for lo, hi in PACE_RANGES:
                out.append(f"    {{{t10}, {k}, {lo}, {hi}, {int(pace_holds(t10, k, lo, hi))}}},")
                m += 1
    out += ["};", f"static const int kPaceHoldsCount = {m};", "", "}  // namespace dancevec", ""]
    return "\n".join(out)


def _c_bytes(text):
    out = []
    for b in text.encode("utf-8"):
        if b >= 0x80:
            out.append("\\%03o" % b)
        elif b in (0x22, 0x5C):
            out.append("\\" + chr(b))
        else:
            out.append(chr(b))
    return '"' + "".join(out) + '"'


def _fold_h():
    out = _vector_head("its own tables", "FoldVectors.h: B.8's fold_genre and its FNV-1a 32 key.")
    out += ["struct Fold { const char* tag; const char* key; uint32_t hash; };  "
            "// key \"\" and hash 0: folds to nothing",
            "static const Fold kFold[] = {"]
    for tag in FOLD_TAGS:
        key = fold_genre(tag)
        out.append(f"    {{{_c_bytes(tag)}, {_c_bytes(key)}, 0x%08Xu}}," % (genre_key(tag) or 0))
    out += ["};", f"static const int kFoldCount = {len(FOLD_TAGS)};", "", "}  // namespace dancevec", ""]
    return "\n".join(out)


def _budget_h(pack):
    out = _vector_head(TEST_DANCER, "BudgetVectors.h: B.6's budget on every move of kTestPack (TestPack.h),\n"
                                    "// `move` being its MOVS index, and one frame trace per move (B.4, B.5).")
    geom, boxes = pack.geom, pack.boxes
    cases, moves, traces, frames = [], [], [], []
    for i, move in enumerate(pack.moves):
        b = budget(geom, boxes, move)
        for c in b.cases:
            cases.append(f"    {{{i}, {c.fps}, {c.pace}, {int(c.mirrored)}, {c.spot}, {c.frames}, {c.median}, "
                         f"{c.peak}}},")
        moves.append(f"    {{{i}, {b.median}, {b.peak}, {('green', 'amber', 'red').index(b.light)}}},")
        if move.kind == BEAT:
            r = reach(move.pace_lo, move.pace_hi, move.beats, move.travel)
            mid = (r[0] + r[1]) // 2
        else:
            mid = 0
        c = next(c for c in b.cases if c.fps == 30 and c.pace == mid)
        run = trace(geom, boxes, move, 30, mid, c.mirrored, c.spot)
        traces.append(f"    {{{i}, 30, {mid}, {int(c.mirrored)}, {c.spot}, {len(frames)}, {len(run)}}},")
        for n, pos16, pose, dirty in run:
            rect = pose.rect or (0, 0, 0, 0)
            frames.append(f"    {{{n}, {pos16}, {pose.frame | (0x8000 if pose.mirrored else 0)}, {pose.root_x}, "
                          f"{pose.root_y}, {rect[0]}, {rect[1]}, {rect[2]}, {rect[3]}, {dirty or 0}}},")
    out += ["struct BudgetCase { uint8_t move, fps, pace, mirrored; int8_t spot; uint16_t frames; "
            "uint32_t median, peak; };",
            "static const BudgetCase kBudgetCases[] = {  // every case, in B.6's order"] + cases + [
        "};", f"static const int kBudgetCasesCount = {len(cases)};", "",
        "struct MoveBudget { uint8_t move; uint32_t median, peak; uint8_t light; };  // 0 green, 1 amber, 2 red",
        "static const MoveBudget kMoveBudgets[] = {"] + moves + [
        "};", f"static const int kMoveBudgetsCount = {len(moves)};", "",
        "struct TraceCase { uint8_t move, fps, pace, mirrored; int8_t spot; uint16_t first, count; };",
        "static const TraceCase kTraceCases[] = {  // one per move: its first case at 30 fps and the middle pace"
    ] + traces + [
        "};", f"static const int kTraceCasesCount = {len(traces)};", "",
        "struct TraceFrame { uint16_t n; uint32_t pos16; uint16_t frame;  // bit 15: drawn mirrored",
        "                    int16_t rootX, rootY, x0, y0, x1, y1;        // the character rect; 0s when empty",
        "                    uint32_t dirty; };                           // 0 for n = 0",
        "static const TraceFrame kTrace[] = {  // frames 0..N of each trace case"] + frames + [
        "};", f"static const int kTraceCount = {len(frames)};", "", "}  // namespace dancevec", ""]
    return "\n".join(out)


def _generate():
    """Every registry output and vector header: ([(path, text)], failures),
    failures being the compile reports of registry packs that failed."""
    files, failures, test = [], [], None
    for src, dst in BUILTINS:
        comp, res = _compile(ROOT / src, strict=True)
        if not res.ok:
            failures.append((src, res.messages))
            continue
        files.append((dst, _pack_source(comp, src, Path(dst).stem, dst.endswith(".cpp"))))
        if src == TEST_DANCER:
            test = res.pack
    files.append((ELIGIBILITY_H, _eligibility_h()))
    files.append((FOLD_H, _fold_h()))
    if test is not None:
        files.append((BUDGET_H, _budget_h(test)))
    return files, failures


def generated_files():
    """Every registry output and vector header as [(path, text)]. Raises
    DanceError when a registry pack doesn't compile under --strict."""
    files, failures = _generate()
    if failures:
        raise DanceError("; ".join(f"{src}: " + " / ".join(m for m in msgs if "error" in m)
                                   for src, msgs in failures))
    return files


# ---- sheets kept as text (sheet.txt -> sheet.png)
def _render_sheet_txt(text):
    """sheet.txt -> (width, height, rows of (r, g, b)). The format:
    '#' comment lines; `sheet <cols>x<rows> cells of <w>x<h>`; one line per
    colour, `<char> #rrggbb` ('.' is the background, required); then cells,
    each its name (A1) on a line of its own followed by <h> lines of <w>
    characters. Cells not given are all background."""
    lines = [ln.rstrip() for ln in text.split("\n")]
    lines = [ln for ln in lines if not ln.startswith("#")]
    it = iter(enumerate(lines, 1))
    size, colours, cells = None, {}, {}
    for _, ln in it:
        if not ln.strip():
            continue
        m = re.fullmatch(r"sheet ([0-9]+)x([0-9]+) cells of ([0-9]+)x([0-9]+)", ln)
        if m:
            size = tuple(int(v) for v in m.groups())
            continue
        m = re.fullmatch(r"(\S) (#[0-9a-fA-F]{6})", ln)
        if m:
            colours[m.group(1)] = _parse_hex(m.group(2))
            continue
        cell = _parse_cell(ln.strip())
        if cell is None or size is None:
            raise DanceError(f"sheet.txt: can't read {ln!r}")
        rows = []
        for _ in range(size[3]):
            _, row = next(it, (0, None))
            if row is None or len(row) != size[2] or any(ch not in colours for ch in row):
                raise DanceError(f"sheet.txt: cell {ln.strip()} needs {size[3]} rows of {size[2]} known characters")
            rows.append(row)
        cells[cell] = rows
    if size is None or "." not in colours:
        raise DanceError("sheet.txt: needs a sheet line and the '.' colour")
    cols, nrows, cw, ch = size
    out = [[colours["."]] * (cols * cw) for _ in range(nrows * ch)]
    for (c, r), rows in cells.items():
        if c >= cols or r >= nrows:
            raise DanceError(f"sheet.txt: {_cell_name((c, r))} is off the sheet")
        for y, row in enumerate(rows):
            for x, chr_ in enumerate(row):
                out[r * ch + y][c * cw + x] = colours[chr_]
    return cols * cw, nrows * ch, out


def _sheet_sources():
    """(sheet.txt, sheet.png) of every registry folder that keeps its sheet as text."""
    out = []
    for src, _ in BUILTINS:
        txt = ROOT / src / "sheet.txt"
        if txt.is_file():
            out.append((txt, ROOT / src / "sheet.png"))
    return out


def _sheet_current(txt, png):
    w, h, rows = _render_sheet_txt(txt.read_text(encoding="utf-8"))
    if png.is_file():
        try:
            pw, ph, prows = read_png(png.read_bytes())
        except DanceError:
            pw = None
        if (pw, ph if pw else None) == (w, h) and all(
                tuple(p[:3]) == q and p[3] == 255 for prow, row in zip(prows, rows) for p, q in zip(prow, row)):
            return True, (w, h, rows)
    return False, (w, h, rows)


# ---- the embedded tables' sources (C.2)
def _c_unescape(body):
    out, i = [], 0
    while i < len(body):
        ch = body[i]
        if ch != "\\":
            out.append(ch)
            i += 1
            continue
        nxt = body[i + 1]
        if nxt in "01234567":
            j = i + 1
            while j < len(body) and j < i + 4 and body[j] in "01234567":
                j += 1
            out.append(chr(int(body[i + 1:j], 8)))
            i = j
        else:
            out.append({"n": "\n", "t": "\t", "0": "\0"}.get(nxt, nxt))
            i += 2
    return "".join(out)


def parse_textfold_cpp(text):
    """TEXTFOLD rebuilt from lib/core/TextFold.cpp's source: {code point:
    Full replacement} for every code point whose replacement isn't '?'."""
    def table(fn):
        m = re.search(r"const char\* " + fn + r"\(uint32_t cp\) \{(.*?)\n\}", text, re.S)
        out, pending = {}, []
        if m:
            for t in re.finditer(r'case (0x[0-9A-Fa-f]+):|return "((?:[^"\\]|\\.)*)";', m.group(1)):
                if t.group(1):
                    pending.append(int(t.group(1), 16))
                else:
                    for cp in pending:
                        out[cp] = _c_unescape(t.group(2))
                    pending = []
        return out

    def literal(name):
        m = re.search(r"constexpr char " + name + r"\[[^\]]*\] =(.*?);", text, re.S)
        return "".join(_c_unescape(s) for s in re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))) if m else ""

    punct, multi, sym = table("punctuation"), table("latin1Multi"), table("symbols")
    latin1, ext_a = literal("kLatin1"), literal("kLatinExtA")

    def single(c):
        return c if ("A" <= c <= "Z") or ("a" <= c <= "z") else "?"

    out = {}
    for cp in sorted(set(punct) | set(multi) | set(sym) | set(range(0xC0, 0x180))):
        if cp < 0x80:
            continue
        if cp in punct:
            r = punct[cp]
        elif cp in multi:
            r = multi[cp]
        elif cp in sym:
            r = sym[cp]
        elif 0xC0 <= cp <= 0xFF and cp - 0xC0 < len(latin1) and latin1[cp - 0xC0] != "\0":
            r = single(latin1[cp - 0xC0])
        elif 0x100 <= cp <= 0x17F and cp - 0x100 < len(ext_a):
            r = single(ext_a[cp - 0x100])
        else:
            r = "?"
        if r != "?":
            out[cp] = r
    return out


def parse_vlw_coverage(text):
    """FONT_COVERAGE rebuilt from src/ui/VlwFonts.cpp: the code points every
    kVlw<Name> array holds, as sorted (first, last) ranges."""
    sets = []
    for m in re.finditer(r"const uint8_t kVlw\w+\[([0-9]+)\] = \{(.*?)\};", text, re.S):
        raw = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(2)))
        if len(raw) < 24:
            continue
        n = int.from_bytes(raw[0:4], "big")
        sets.append({int.from_bytes(raw[24 + 28 * i:28 + 28 * i], "big") for i in range(n)
                     if 28 + 28 * i <= len(raw)})
    common = set.intersection(*sets) if sets else set()
    ranges = []
    for cp in sorted(common):
        if ranges and ranges[-1][1] == cp - 1:
            ranges[-1] = (ranges[-1][0], cp)
        else:
            ranges.append((cp, cp))
    return ranges


def _first_difference(a, b):
    keys = set(a) | set(b)
    diff = [k for k in keys if a.get(k) != b.get(k)]
    return min(diff) if diff else None


def _check_tables():
    out = []
    path = ROOT / "lib" / "core" / "TextFold.cpp"
    if not path.is_file():
        out.append("stale: tools/dancepack.py (no lib/core/TextFold.cpp to check TEXTFOLD against)")
    else:
        cp = _first_difference(TEXTFOLD, parse_textfold_cpp(path.read_text(encoding="utf-8")))
        if cp is not None:
            out.append("stale: tools/dancepack.py (TEXTFOLD differs from lib/core/TextFold.cpp at U+%04X)" % cp)
    path = ROOT / "src" / "ui" / "VlwFonts.cpp"
    if not path.is_file():
        out.append("stale: tools/dancepack.py (no src/ui/VlwFonts.cpp to check FONT_COVERAGE against)")
    else:
        def points(ranges):
            return {cp: True for a, b in ranges for cp in range(a, b + 1)}
        cp = _first_difference(points(FONT_COVERAGE), points(parse_vlw_coverage(path.read_text(encoding="utf-8"))))
        if cp is not None:
            out.append("stale: tools/dancepack.py (FONT_COVERAGE differs from src/ui/VlwFonts.cpp at U+%04X)" % cp)
    return out


# ---- the command line (C.2)
def _write_text(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def _write_failed(path, e):
    """C.6's write failure: `<FILE>: error: can't write it (<reason>)`, exit 1."""
    print(f"{_esc(_slash(path))}: error: can't write it ({_strerror(e)})")
    return 1


def _read_text(path):
    try:
        with open(path, encoding="utf-8") as f:   # universal newlines: CRLF checkouts compare equal
            return f.read()
    except (OSError, UnicodeDecodeError):
        return None


def _in_repository():
    """True when dancepack.py sits in the repository (C.1): `--check` and
    `builtin` regenerate files there; a downloaded copy has no lib/core."""
    return (ROOT / "lib" / "core").is_dir()


def _print_failures(failures):
    for _, messages in failures:
        for line in messages:
            print(line)


def _cmd_check_all():
    stale = []
    for txt, png in _sheet_sources():
        try:
            current, _ = _sheet_current(txt, png)
        except DanceError as e:
            print(e)
            return 1
        if not current:
            stale.append(_slash(png.relative_to(ROOT)))
    files, failures = _generate()
    for path, text in files:
        if _read_text(ROOT / path) != text:
            stale.append(path)
    for path in stale:
        print(f"stale: {path} (run python tools/dancepack.py builtin)")
    if failures:
        _print_failures(failures)
        return 1
    tables = _check_tables()
    for line in tables:
        print(line)
    return 1 if stale or tables else 0


def _cmd_builtin_all():
    for txt, png in _sheet_sources():
        try:
            current, (w, h, rows) = _sheet_current(txt, png)
        except DanceError as e:
            print(e)
            return 1
        if not current:
            try:
                png.write_bytes(_write_png(w, h, rows))
            except OSError as e:
                return _write_failed(png.relative_to(ROOT), e)
            print(f"wrote {_slash(png.relative_to(ROOT))}")
    files, failures = _generate()
    if failures:
        _print_failures(failures)
        return 1
    for path, text in files:
        if _read_text(ROOT / path) != text:
            try:
                _write_text(ROOT / path, text)
            except OSError as e:
                return _write_failed(path, e)
            print(f"wrote {path}")
    return 0


def _cmd_builtin_one(ap, src, out):
    stem = Path(out).stem
    if not (out.endswith(".cpp") or out.endswith(".h")) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", stem):
        ap.error(f"--out {out}: a .cpp or .h file whose name is a C++ identifier (it names the symbols)")
    if not Path(src).is_dir():
        print(f"{_esc(_slash(src))}: error: no dancer.ini")
        return 1
    comp, res = _compile(src, strict=True)
    for line in res.messages:
        print(line)
    if not res.ok:
        return 1
    try:
        _write_text(Path(out), _pack_source(comp, _slash(src), stem, out.endswith(".cpp")))
    except OSError as e:
        return _write_failed(out, e)
    print(f"wrote {_esc(_slash(out))}")
    return 0


def _cmd_info(path):
    shown = _esc(_slash(path))
    if Path(path).is_dir():
        print(f"{shown}: error: a folder, not a .dnc pack")
        return 1
    try:
        data = Path(path).read_bytes()
    except FileNotFoundError:
        print(f"{shown}: error: no such file")
        return 1
    except OSError as e:
        print(f"{shown}: error: can't read it ({_strerror(e)})")
        return 1
    try:
        pack = read_pack(data)
    except DanceError as e:
        print(f"{shown}: error: {e}")
        return 1
    fmt, min_reader = struct.unpack_from("<HH", data, 4)
    g = pack.geom
    print(f"{shown}: MPDN v{fmt} (reader {min_reader}), {_n(pack.size)} bytes, CRC ok")
    print(f'  id {pack.id}: "{pack.name}" by {pack.author}, licence {pack.licence}')
    if pack.url or pack.version:
        print(f"  url {pack.url or '-'}, version {pack.version or '-'}")
    print(f"  cell {g.cell_w}x{g.cell_h} at scale {g.scale}, anchor {g.anchor_x},{g.anchor_y}, shadow {g.shadow_w}, "
          f"faces {FACES[g.faces]}, {_s(g.frame_count, 'frame')}, rest frame {g.rest_frame & 0x7FFF}")
    count = struct.unpack_from("<H", data, 16)[0]
    entries = [struct.unpack_from("<4sII", data, 32 + 12 * i) for i in range(count)]
    print("  chunks: " + ", ".join(f"{_tag_text(t)} {off}+{size}" for t, off, size in entries))
    if pack.pose_keys:
        print("  pose keys: " + ", ".join(pack.pose_keys))
    ignored = dict(pack.ignored)
    print(f"  {_s(len(pack.moves), 'move')} ({len(pack.ignored)} ignored), {pack.declared_moves} declared")
    for i, m in enumerate(pack.moves):
        if i in ignored:
            label = f'"{m.name}"' if m.name else "(bad name)"
            print(f"  move {i} {label}: ignored: {ignored[i]}")
            continue
        b = budget(g, pack.boxes, m)
        print(f'  move {i} "{m.name}": {_what(m)}, {_s(len(m.steps), "step")}; {_light_text(b)}')
    return 0


def main(argv=None):
    """The command line (C.2): returns the exit code; usage errors raise SystemExit(2)."""
    try:
        sys.stdout.reconfigure(errors="backslashreplace")
    except (AttributeError, ValueError, OSError):
        pass
    ap = argparse.ArgumentParser(prog="dancepack.py", description=__doc__.split("\n\n")[0].replace("\n", " "))
    ap.add_argument("--check", action="store_true",
                    help="in the repository: exit 1 if the built-in packs, the test vectors or the embedded tables "
                         "are stale (to validate a dancer folder, use: check DIR)")
    sub = ap.add_subparsers(dest="cmd", metavar="COMMAND")
    p = sub.add_parser("check", help="validate a dancer folder and print the full report")
    p.add_argument("dir", metavar="DIR")
    p.add_argument("--strict", action="store_true", help="fail on a skipped move or a licence off the allowlist")
    p = sub.add_parser("build", help="compile a dancer folder to a .dnc pack")
    p.add_argument("dir", metavar="DIR")
    p.add_argument("-o", "--out", metavar="FILE", help="the pack to write (default: <id>.dnc here)")
    p.add_argument("--strict", action="store_true", help="fail on a skipped move or a licence off the allowlist")
    p = sub.add_parser("info", help="read a .dnc pack and print what it holds")
    p.add_argument("file", metavar="FILE")
    p = sub.add_parser("builtin", help="regenerate the built-in packs and test vectors, or one pack as C++")
    p.add_argument("dir", metavar="DIR", nargs="?")
    p.add_argument("--out", metavar="FILE", help="the .cpp or .h to write (with DIR)")
    args = ap.parse_args(argv)
    outside = ("--check and builtin work only in the mstream-mp3-player repository; to check a dancer use: "
               "dancepack.py check DIR")
    if args.check:
        if args.cmd:
            ap.error("--check takes no command")
        if not _in_repository():
            ap.error(outside)
        return _cmd_check_all()
    if args.cmd is None:
        ap.error("give a command (check, build, info or builtin) or --check")
    if args.cmd == "info":
        return _cmd_info(args.file)
    if args.cmd == "builtin":
        if (args.dir is None) != (args.out is None):
            ap.error("builtin takes both DIR and --out FILE, or neither")
        if args.dir is None and not _in_repository():
            ap.error(outside)
        return _cmd_builtin_all() if args.dir is None else _cmd_builtin_one(ap, args.dir, args.out)
    comp, res = _compile(args.dir, strict=args.strict)
    if args.cmd == "check":
        for line in res.report:
            print(line)
        return 0 if res.ok else 1
    for line in res.messages:
        print(line)
    if not res.ok:
        return 1
    out = args.out if args.out else f"{comp.id}.dnc"
    try:
        Path(out).parent.mkdir(parents=True, exist_ok=True)
        Path(out).write_bytes(res.data)
    except OSError as e:
        return _write_failed(out, e)
    print(f"wrote {_esc(_slash(out))}")
    return 0


# ---- the embedded tables (C.2: `--check` verifies them)
# textfold::replacement(cp, Mode::Full) from lib/core/TextFold.cpp, for every
# code point whose replacement isn't "?".
TEXTFOLD = {
    0x00A0: " ", 0x00A1: "!", 0x00A2: "c", 0x00A3: "L", 0x00A5: "Y", 0x00A6: "|",
    0x00A7: "S", 0x00A8: "", 0x00A9: "(c)", 0x00AA: "a", 0x00AB: "\"", 0x00AC: "-",
    0x00AD: "", 0x00AE: "(R)", 0x00AF: "", 0x00B0: "o", 0x00B1: "+-", 0x00B2: "2",
    0x00B3: "3", 0x00B4: "'", 0x00B5: "u", 0x00B6: "P", 0x00B7: ".", 0x00B8: ",",
    0x00B9: "1", 0x00BA: "o", 0x00BB: "\"", 0x00BC: "1/4", 0x00BD: "1/2",
    0x00BE: "3/4", 0x00C0: "A", 0x00C1: "A", 0x00C2: "A", 0x00C3: "A", 0x00C4: "A",
    0x00C5: "A", 0x00C6: "AE", 0x00C7: "C", 0x00C8: "E", 0x00C9: "E", 0x00CA: "E",
    0x00CB: "E", 0x00CC: "I", 0x00CD: "I", 0x00CE: "I", 0x00CF: "I", 0x00D0: "D",
    0x00D1: "N", 0x00D2: "O", 0x00D3: "O", 0x00D4: "O", 0x00D5: "O", 0x00D6: "O",
    0x00D7: "x", 0x00D8: "O", 0x00D9: "U", 0x00DA: "U", 0x00DB: "U", 0x00DC: "U",
    0x00DD: "Y", 0x00DE: "Th", 0x00DF: "ss", 0x00E0: "a", 0x00E1: "a", 0x00E2: "a",
    0x00E3: "a", 0x00E4: "a", 0x00E5: "a", 0x00E6: "ae", 0x00E7: "c", 0x00E8: "e",
    0x00E9: "e", 0x00EA: "e", 0x00EB: "e", 0x00EC: "i", 0x00ED: "i", 0x00EE: "i",
    0x00EF: "i", 0x00F0: "d", 0x00F1: "n", 0x00F2: "o", 0x00F3: "o", 0x00F4: "o",
    0x00F5: "o", 0x00F6: "o", 0x00F7: "/", 0x00F8: "o", 0x00F9: "u", 0x00FA: "u",
    0x00FB: "u", 0x00FC: "u", 0x00FD: "y", 0x00FE: "th", 0x00FF: "y", 0x0100: "A",
    0x0101: "a", 0x0102: "A", 0x0103: "a", 0x0104: "A", 0x0105: "a", 0x0106: "C",
    0x0107: "c", 0x0108: "C", 0x0109: "c", 0x010A: "C", 0x010B: "c", 0x010C: "C",
    0x010D: "c", 0x010E: "D", 0x010F: "d", 0x0110: "D", 0x0111: "d", 0x0112: "E",
    0x0113: "e", 0x0114: "E", 0x0115: "e", 0x0116: "E", 0x0117: "e", 0x0118: "E",
    0x0119: "e", 0x011A: "E", 0x011B: "e", 0x011C: "G", 0x011D: "g", 0x011E: "G",
    0x011F: "g", 0x0120: "G", 0x0121: "g", 0x0122: "G", 0x0123: "g", 0x0124: "H",
    0x0125: "h", 0x0126: "H", 0x0127: "h", 0x0128: "I", 0x0129: "i", 0x012A: "I",
    0x012B: "i", 0x012C: "I", 0x012D: "i", 0x012E: "I", 0x012F: "i", 0x0130: "I",
    0x0131: "i", 0x0132: "IJ", 0x0133: "ij", 0x0134: "J", 0x0135: "j", 0x0136: "K",
    0x0137: "k", 0x0138: "k", 0x0139: "L", 0x013A: "l", 0x013B: "L", 0x013C: "l",
    0x013D: "L", 0x013E: "l", 0x013F: "L", 0x0140: "l", 0x0141: "L", 0x0142: "l",
    0x0143: "N", 0x0144: "n", 0x0145: "N", 0x0146: "n", 0x0147: "N", 0x0148: "n",
    0x0149: "n", 0x014A: "N", 0x014B: "n", 0x014C: "O", 0x014D: "o", 0x014E: "O",
    0x014F: "o", 0x0150: "O", 0x0151: "o", 0x0152: "OE", 0x0153: "oe", 0x0154: "R",
    0x0155: "r", 0x0156: "R", 0x0157: "r", 0x0158: "R", 0x0159: "r", 0x015A: "S",
    0x015B: "s", 0x015C: "S", 0x015D: "s", 0x015E: "S", 0x015F: "s", 0x0160: "S",
    0x0161: "s", 0x0162: "T", 0x0163: "t", 0x0164: "T", 0x0165: "t", 0x0166: "T",
    0x0167: "t", 0x0168: "U", 0x0169: "u", 0x016A: "U", 0x016B: "u", 0x016C: "U",
    0x016D: "u", 0x016E: "U", 0x016F: "u", 0x0170: "U", 0x0171: "u", 0x0172: "U",
    0x0173: "u", 0x0174: "W", 0x0175: "w", 0x0176: "Y", 0x0177: "y", 0x0178: "Y",
    0x0179: "Z", 0x017A: "z", 0x017B: "Z", 0x017C: "z", 0x017D: "Z", 0x017E: "z",
    0x017F: "s", 0x02BC: "'", 0x2000: " ", 0x2001: " ", 0x2002: " ", 0x2003: " ",
    0x2004: " ", 0x2005: " ", 0x2006: " ", 0x2007: " ", 0x2008: " ", 0x2009: " ",
    0x200A: " ", 0x200B: "", 0x200C: "", 0x200D: "", 0x2010: "-", 0x2011: "-",
    0x2012: "-", 0x2013: "-", 0x2014: "-", 0x2015: "-", 0x2018: "'", 0x2019: "'",
    0x201A: "'", 0x201B: "'", 0x201C: "\"", 0x201D: "\"", 0x201E: "\"", 0x201F: "\"",
    0x2022: "*", 0x2026: "...", 0x202F: " ", 0x2032: "'", 0x2033: "\"", 0x2035: "'",
    0x2039: "<", 0x203A: ">", 0x2044: "/", 0x205F: " ", 0x2060: "", 0x20AC: "EUR",
    0x2122: "TM", 0x2212: "-", 0x3000: " ", 0xFEFF: "",
}

# The code points all four VLW fonts in src/ui/VlwFonts.cpp hold.
FONT_COVERAGE = (
    (0x0020, 0x007E), (0x00A0, 0x00FF), (0x0104, 0x0107), (0x010C, 0x010D),
    (0x0118, 0x011B), (0x0131, 0x0131), (0x0141, 0x0144), (0x0147, 0x0148),
    (0x0150, 0x0153), (0x0158, 0x015B), (0x0160, 0x0161), (0x0164, 0x0165),
    (0x016E, 0x0171), (0x0178, 0x017E), (0x2010, 0x2015), (0x2018, 0x2022),
    (0x2026, 0x2026), (0x2030, 0x2030), (0x2032, 0x2033), (0x2039, 0x203A),
    (0x20AC, 0x20AC), (0x2122, 0x2122),
)
_COVERAGE_FIRSTS = [a for a, _ in FONT_COVERAGE]


if __name__ == "__main__":
    sys.exit(main())
