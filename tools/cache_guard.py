# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""PlatformIO post-script (and a command-line check): the MP3 synth loop's flash-cache sets.

The ESP32 reads its flash code (0x400Dxxxx) and flash data (0x3F4xxxxx)
through one cache per CPU: 32 KB, 2-way, 32-byte lines. So an address's set
is (address mod 16 KB) / 32, for code and data alike, and a set keeps two
lines. libmad's synthesis runs 36 passes a frame (dct32 for each channel,
the window D), and between two passes AudioGeneratorMP3 hands the output 32
samples one at a time (GetOneSample, loop, the output's ConsumeSample through
its vtable). Every line of that loop is read on every pass: a set that holds
three of them misses about 3 times a pass, ~100 line fills a frame. In
0.7.0-dev (the seek bar) and the Opus branch 13-14 sets held dct32, D and
GetOneSample/loop: One More Time fell from 4.8x realtime to 3.8x, and the
same image with 10 KB of unused rodata added read 5.0x (docs/RESAMPLER.md
section 10e).

So tools/iram_diet.py pins the loop's hot code at the front of .flash.text
and its tables at the front of .flash.rodata. Both sections start at fixed
addresses in every build (0x400D0020 and 0x3F400120: memory.ld and the
256-byte app descriptor), each block is under 16 KB, so a set holds at most
one line of each, whatever the rest of the firmware does. This check runs
after every link and fails the build when:

- any cache set holds 3 or more distinct lines of the loop's hot set, on
  either path below (it prints which sets, what shares them, and where each
  item is);
- (in the build) the pin isn't in the link (its _mp3_hot_* symbols are
  missing), or a hot item isn't inside it: then this layout's result is
  luck, not layout independence;
- a hot item's symbol isn't in the ELF (a library update renamed it): update
  HOT below.

It warns, in the build's log too, when it reads a hot function only in part:
bytes its branches don't reach from the entry (a switch's jump table's
cases, an exception's landing pad) or an instruction it can't decode. The
literals those load aren't counted, nor is a jump table's own rodata: look
at the function before trusting the result.

The hot set, one table for both this check and the pin (HOT, PATHS):

- libmad's synthesis: dct32, synth_full, mad_synth_frame_onens (or
  mad_synth_frame), and D, its window table (2,176 B);
- ESP8266Audio's per-sample code: AudioGeneratorMP3::GetOneSample, ::loop;
- the bench's sink (the console's b<n>): CountingOutput::ConsumeSample and the
  word of its vtable that loop() reads;
- playback's: RingOutput::ConsumeSample (RingFeed::consume inlined),
  TrimFeed::consumeTrimmed and the out-of-line RingFeed::consume (a track
  with a LAME tag's padding to hold back: most MP3s, the whole track), and
  RingOutput's vtable word.

A function counts with every literal it loads (Xtensa's L32R reads a pool
that the linker puts apart from the code), found by following its code from
its entry. Code or data in IRAM or internal DRAM isn't cached and doesn't
count. Left out: the
PSRAM, also cached, where libmad's state sits at one fixed address in every
build (RESAMPLER.md section 10d: its lower 2 MB measured insensitive to
where); and a track at another rate's converter (one block per pass), FLAC
and Opus, which have other loops.

Command line (no PlatformIO needed; Python 3.8+, the standard library):

    python tools/cache_guard.py [--require-pin] firmware.elf [more.elf ...]

prints each ELF's result and exits 1 if any fails. Tests:
python -m unittest discover -s tools -p "test_cache_guard.py".
"""
import argparse
import struct
import sys
from dataclasses import dataclass

WAY = 16 * 1024  # one way of the 32 KB, 2-way cache
LINE = 32
WAYS = 2
SETS = WAY // LINE

# The MMU's windows onto the flash: what goes through the cache (and is modelled).
CACHED = (
    (0x3F400000, 0x3F800000, "flash data"),
    (0x400C2000, 0x40C00000, "flash code"),
)

# The pin's bounds, defined by tools/iram_diet.py's linker-script lines.
PIN_CODE = ("_mp3_hot_text_start", "_mp3_hot_text_end")
PIN_DATA = ("_mp3_hot_rodata_start", "_mp3_hot_rodata_end")

SYNTH_O = "*libESP8266Audio.a:synth.c.o"
MP3_O = "*libESP8266Audio.a:AudioGeneratorMP3.cpp.o"
BACKEND_O = "*Core2AudioBackend.cpp.o"
COUNTING_CS = "_ZN12_GLOBAL__N_114CountingOutput13ConsumeSampleEPs"
RING_CS = "_ZN10RingOutput13ConsumeSampleEPs"


@dataclass(frozen=True)
class Hot:
    """One item of the loop's hot set.

    label: what the report calls it. names: its symbol (mangled); the first
    one the ELF has is used. kind: "code" (its bytes and the literals it
    loads), "data" (its bytes) or "slot" (the word of this vtable that holds
    `of`, a function's symbol). file: a local symbol's source file (the ELF's
    STT_FILE entry), to tell it from others of that name. obj: the input
    files that hold it, for the pin's linker-script line ("*": any, for an
    inline function or a vtable emitted wherever it is used).
    """
    label: str
    names: tuple
    kind: str = "code"
    file: str = None
    obj: str = "*"
    of: str = None


HOT = (
    Hot("dct32", ("dct32",), file="synth.c", obj=SYNTH_O),
    Hot("synth_full", ("synth_full",), file="synth.c", obj=SYNTH_O),
    Hot("mad_synth_frame_onens", ("mad_synth_frame_onens", "mad_synth_frame"), obj=SYNTH_O),
    Hot("D", ("D",), kind="data", file="synth.c", obj=SYNTH_O),
    Hot("AudioGeneratorMP3::GetOneSample", ("_ZN17AudioGeneratorMP312GetOneSampleEPs",), obj=MP3_O),
    Hot("AudioGeneratorMP3::loop", ("_ZN17AudioGeneratorMP34loopEv",), obj=MP3_O),
)

PATHS = (
    ("bench", (
        Hot("CountingOutput::ConsumeSample", (COUNTING_CS,), file="Core2AudioBackend.cpp", obj=BACKEND_O),
        Hot("CountingOutput's vtable", ("_ZTVN12_GLOBAL__N_114CountingOutputE",), kind="slot",
            file="Core2AudioBackend.cpp", obj=BACKEND_O, of=COUNTING_CS),
    )),
    ("playback", (
        Hot("RingOutput::ConsumeSample", (RING_CS,)),
        Hot("TrimFeed::consumeTrimmed", ("_ZN8TrimFeed14consumeTrimmedEPKs",)),
        Hot("RingFeed::consume", ("_ZN8RingFeed7consumeEPKs",)),
        Hot("RingOutput's vtable", ("_ZTV10RingOutput",), kind="slot", of=RING_CS),
    )),
)


def all_hot():
    """Every hot item once, in the pin's order: HOT, then each path's."""
    return HOT + tuple(h for _name, items in PATHS for h in items)


def pin_lines():
    """tools/iram_diet.py's input-section lines: (code, data). A function's
    literal pool goes with it (.literal.<name>), so the block is contiguous."""
    code, data = [], []
    for h in all_hot():
        if h.kind == "code":
            code.append(f"{h.obj}({' '.join(f'.literal.{n} .text.{n}' for n in h.names)})")
        else:
            data.append(f"{h.obj}({' '.join(f'.rodata.{n}' for n in h.names)})")
    return code, data


# ---------------------------------------------------------------------------
# The ELF (32-bit little-endian), read directly: no pyelftools, no toolchain.
# ---------------------------------------------------------------------------
SHT_SYMTAB, SHT_NOBITS, SHF_ALLOC = 2, 8, 0x2
STT_OBJECT, STT_FUNC, STT_FILE = 1, 2, 4


@dataclass(frozen=True)
class Symbol:
    name: str
    addr: int
    size: int
    kind: int  # STT_*
    file: str  # the STT_FILE before it, for a local; None for a global or weak one


class Elf:
    """The symbols and the loaded bytes of an ELF."""

    def __init__(self, path):
        with open(path, "rb") as f:
            d = f.read()
        if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
            raise ValueError(f"{path}: not a 32-bit little-endian ELF")
        self._d = d
        shoff, = struct.unpack_from("<I", d, 0x20)
        shentsize, shnum = struct.unpack_from("<HH", d, 0x2E)
        # (type, flags, addr, offset, size, link)
        self._sections = [struct.unpack_from("<4xIIIIII", d, shoff + i * shentsize) for i in range(shnum)]
        self.symbols = {}  # name -> [Symbol]
        self.addresses = {}  # name -> address, every defined symbol (the pin's bounds have no size)
        for stype, _flags, _addr, off, size, link in self._sections:
            if stype != SHT_SYMTAB:
                continue
            str_off = self._sections[link][3]
            file = None
            for name_off, value, sym_size, info, _other, shndx in struct.iter_unpack("<IIIBBH", d[off:off + size]):
                end = d.index(b"\0", str_off + name_off)
                name = d[str_off + name_off:end].decode("utf-8", "replace")
                kind, local = info & 0xF, info >> 4 == 0
                if kind == STT_FILE:
                    file = name
                    continue
                if shndx == 0 or not name:
                    continue
                self.addresses.setdefault(name, value)
                if sym_size and kind in (STT_OBJECT, STT_FUNC):
                    self.symbols.setdefault(name, []).append(
                        Symbol(name, value, sym_size, kind, file if local else None))

    def read(self, addr, size):
        """The bytes at [addr, addr + size), or None if no loaded section holds them."""
        for stype, flags, saddr, off, ssize, _link in self._sections:
            if flags & SHF_ALLOC and stype != SHT_NOBITS and saddr <= addr and addr + size <= saddr + ssize:
                return self._d[off + addr - saddr:off + addr - saddr + size]
        return None


def _signed(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


def _step(code, i, addr):
    """The instruction at code[i]: (length, the offsets it can go on to, its
    L32R target or None). Xtensa as the ESP32 has it (the density option, no
    FLIX): op0, the low 4 bits, 8-13 is a 2-byte instruction, 0-7 a 3-byte
    one. Only what decides the flow is decoded: the branches (their targets
    are pc + 4 + offset), J, the returns, JX, ILL. A call goes on after itself."""
    b0 = code[i]
    op0 = b0 & 0xF
    if op0 >= 14:
        raise ValueError(f"no ESP32 instruction at {addr + i:#x} (op0 {op0})")
    n = 2 if op0 >= 8 else 3
    if i + n > len(code):
        raise ValueError(f"the instruction at {addr + i:#x} runs past the function's end")
    on = [i + n]
    if n == 2:
        w = b0 | code[i + 1] << 8
        t, r = (w >> 4) & 0xF, (w >> 12) & 0xF
        if op0 == 12 and t & 8:  # BEQZ.N, BNEZ.N: a 6-bit forward offset
            return n, on + [i + 4 + ((t & 3) << 4 | r)], None
        if op0 == 13 and r == 15 and t in (0, 1, 6):  # RET.N, RETW.N, ILL.N
            return n, [], None
        return n, on, None
    w = b0 | code[i + 1] << 8 | code[i + 2] << 16
    t, r, op1, op2 = (w >> 4) & 0xF, (w >> 12) & 0xF, (w >> 16) & 0xF, w >> 20
    if op0 == 1:  # L32R: ((pc + 3) & ~3) + (0xFFFF0000 | imm16) * 4
        return n, on, (((addr + i + 3) & ~3) + ((w >> 8) - 0x10000) * 4) & 0xFFFFFFFF
    if op0 == 0 and op1 == 0 and op2 == 0 and (r == 3 or (r == 0 and t in (0, 8, 9, 10))):
        return n, [], None  # RFE and co., ILL, RET, RETW, JX (a jump table's targets aren't known)
    if op0 == 6:
        sub, m = (w >> 4) & 3, (w >> 6) & 3
        if sub == 0:  # J
            return n, [i + 4 + _signed(w >> 6, 18)], None
        if sub == 1:  # BEQZ, BNEZ, BLTZ, BGEZ
            return n, on + [i + 4 + _signed(w >> 12, 12)], None
        if sub == 2 or m >= 2 or (m == 1 and r in (0, 1)):  # BEQI..BGEI, BLTUI, BGEUI, BF, BT
            return n, on + [i + 4 + _signed(w >> 16, 8)], None
        if m == 1 and r in (8, 9, 10):  # LOOP, LOOPNEZ, LOOPGTZ: the loop's end
            return n, on + [i + 4 + (w >> 16)], None
        return n, on, None  # ENTRY
    if op0 == 7:  # the two-register branches
        return n, on + [i + 4 + _signed(w >> 16, 8)], None
    return n, on, None


def literals(code, addr):
    """(the addresses an Xtensa function at `addr` loads with L32R: its
    literals, the bytes of it not reached). Followed from its entry along its
    branches, so the alignment padding the assembler leaves after a jump is
    never read as code. Bytes not reached that aren't 0 are code only an
    indirect jump (a switch's table) reaches: its literals aren't counted."""
    todo, seen, found = [0], set(), set()
    reached = bytearray(len(code))
    while todo:
        i = todo.pop()
        if i in seen or not 0 <= i < len(code):
            continue  # (a jump out of the function: a tail call)
        seen.add(i)
        n, on, lit = _step(code, i, addr)
        reached[i:i + n] = b"\1" * n
        if lit is not None:
            found.add(lit)
        todo += on
    return sorted(found), sum(1 for i, b in enumerate(code) if b and not reached[i])


# ---------------------------------------------------------------------------
# The model
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class Piece:
    """Bytes the loop reads on every pass: `label`'s, at [addr, addr + size)."""
    label: str
    addr: int
    size: int


def cached(addr):
    return any(lo <= addr < hi for lo, hi, _what in CACHED)


def lines_of(p):
    return range(p.addr // LINE, (p.addr + p.size - 1) // LINE + 1)


class Notes:
    """What a check found besides the sets: errors fail it, warnings don't."""

    def __init__(self):
        self.errors, self.warnings = [], []


def find(image, hot, notes):
    """The Symbol of `hot` in `image`, or None (with an error)."""
    for name in hot.names:
        found = {(s.addr, s.size): s for s in image.symbols.get(name, ())
                 if hot.file is None or s.file == hot.file}
        if len(found) == 1:
            return next(iter(found.values()))
        if len(found) > 1:
            notes.errors.append(f"{hot.label}: {len(found)} symbols named {name}"
                                + (f" in {hot.file}" if hot.file else "") + ": which one is hot?")
            return None
    notes.errors.append(f"{hot.label}: no symbol {' or '.join(hot.names)}"
                        + (f" from {hot.file}" if hot.file else "")
                        + " in the ELF (a library update renamed it?): update HOT/PATHS in tools/cache_guard.py")
    return None


def pieces_of(image, hot, notes):
    """The Pieces `hot` reads on every pass."""
    sym = find(image, hot, notes)
    if sym is None:
        return []
    if hot.kind == "data":
        return [Piece(hot.label, sym.addr, sym.size)]
    blob = image.read(sym.addr, sym.size)
    if blob is None:
        notes.errors.append(f"{hot.label}: its bytes at {sym.addr:#x} aren't in the ELF's loaded sections")
        return []
    if hot.kind == "slot":
        words = struct.unpack(f"<{len(blob) // 4}I", blob[:len(blob) // 4 * 4])
        target = image.addresses.get(hot.of)
        if target not in words:
            notes.errors.append(f"{hot.label}: no word of it points at {hot.of} (update PATHS in "
                                "tools/cache_guard.py)")
            return []
        return [Piece(hot.label, sym.addr + 4 * words.index(target), 4)]
    out = [Piece(hot.label, sym.addr, sym.size)]
    try:
        found, unreached = literals(blob, sym.addr)
    except ValueError as e:
        notes.warnings.append(f"{hot.label}: its literals aren't counted: {e}")
        return out
    if unreached:
        notes.warnings.append(f"{hot.label}: {unreached} B of it aren't reached from its entry (a switch's "
                              "jump table?): the literals they load aren't counted")
    return out + [Piece(f"{hot.label}'s literals", a, 4) for a in found]


def collect(image, notes):
    """(the pieces every path reads, {path: its own pieces}) for an image:
    anything with `symbols` ({name: [Symbol]}), `addresses` ({name: address})
    and `read(addr, size)`: the Elf above, or a test's made-up one."""
    common = [p for h in HOT for p in pieces_of(image, h, notes)]
    return common, {name: [p for h in items for p in pieces_of(image, h, notes)] for name, items in PATHS}


def occupancy(pieces):
    """{set: {line: [labels]}} of the cached pieces."""
    sets = {}
    for p in pieces:
        if not cached(p.addr):
            continue
        for line in lines_of(p):
            labels = sets.setdefault(line % SETS, {}).setdefault(line, [])
            if p.label not in labels:
                labels.append(p.label)
    return sets


def pinned_blocks(image):
    """[(what, start, end)] of the pin, or None when the link has no pin."""
    blocks = []
    for what, (lo_name, hi_name) in (("code", PIN_CODE), ("data", PIN_DATA)):
        lo, hi = image.addresses.get(lo_name), image.addresses.get(hi_name)
        if lo is None or hi is None:
            return None
        blocks.append((what, lo, hi))
    return blocks


def check_pin(blocks, pieces, notes):
    """Errors for a pinned block over one way, or a hot piece out of the pin."""
    for what, lo, hi in blocks:
        if hi - lo > WAY:
            notes.errors.append(f"the pinned {what} is {hi - lo} B, over one way of the cache ({WAY} B): "
                                "it can set against itself")
    out = {}  # item -> its lowest address out of the pin (its code, data or a literal)
    for p in pieces:
        if cached(p.addr) and not any(lo <= p.addr and p.addr + p.size <= hi for _what, lo, hi in blocks):
            item = p.label[:-len("'s literals")] if p.label.endswith("'s literals") else p.label
            out[item] = min(out.get(item, p.addr), p.addr)
    if out:
        notes.errors.append("out of the pin (their input sections aren't the ones tools/iram_diet.py pins: "
                            "HOT/PATHS in tools/cache_guard.py): "
                            + ", ".join(f"{item} (at {addr:#x})" for item, addr in out.items()))


def ranges(numbers):
    """'3-9, 12' for [3, 4, ..., 9, 12]."""
    out = []
    for n in sorted(numbers):
        if out and n == out[-1][1] + 1:
            out[-1][1] = n
        else:
            out.append([n, n])
    return ", ".join(f"{a}-{b}" if a != b else f"{a}" for a, b in out)


def explain(path, bad, pieces):
    """A path's conflicting sets: which, what shares them, and where each item
    is (its offset into the 16 KB way is what puts it in those sets)."""
    out = [f"  {path}: {len(bad)} set{'s' if len(bad) != 1 else ''}:"]
    groups = {}
    for s, lines in bad.items():
        groups.setdefault(tuple(sorted({label for labels in lines.values() for label in labels})), []).append(s)
    for key, sets in sorted(groups.items(), key=lambda kv: min(kv[1])):
        out.append(f"    set{'s' if len(sets) > 1 else ''} {ranges(sets)}: {' + '.join(key)}")
    involved = {label for key in groups for label in key}
    for p in sorted(pieces, key=lambda p: p.addr):
        if p.label in involved and cached(p.addr):
            first, last = p.addr // LINE % SETS, (p.addr + p.size - 1) // LINE % SETS
            out.append(f"      {p.label}: {p.addr:#010x}, {p.size} B: {p.addr % WAY:#06x} into the way, "
                       f"sets {first}-{last}")
    return out


def check(image, require_pin=False):
    """(ok, the report's lines) for an image (see collect())."""
    notes = Notes()
    common, paths = collect(image, notes)
    blocks = pinned_blocks(image)
    if blocks is not None:
        check_pin(blocks, common + [p for ps in paths.values() for p in ps], notes)
    elif require_pin:
        notes.errors.append("the hot set isn't pinned (no _mp3_hot_* symbols in the link): tools/iram_diet.py "
                            "didn't pin it, so whatever this layout gives is luck")
    worst, bad = {}, {}
    for path, ps in paths.items():
        sets = occupancy(common + ps)
        worst[path] = max((len(lines) for lines in sets.values()), default=0)
        bad[path] = {s: lines for s, lines in sets.items() if len(lines) > WAYS}
    report = [line for path, ps in paths.items() if bad[path] for line in explain(path, bad[path], common + ps)]
    if report:
        notes.errors.insert(0, "cache sets with 3 or more of the MP3 synth loop's hot lines: "
                               + ", ".join(f"{len(b)} ({p})" for p, b in bad.items()))
        report[:0] = [
            f"  The flash cache is {WAYS}-way, {WAY // 1024} KB a way, {LINE} B lines: a set is (address mod "
            f"{WAY // 1024} KB) / {LINE}, for code and flash data alike.",
            "  Each set below misses on every pass of the loop, 36 a frame (docs/RESAMPLER.md section 10e).",
            "  tools/iram_diet.py pins the hot set (HOT/PATHS in tools/cache_guard.py): an item out of the pin,",
            "  or a new one, is the usual cause.",
        ]
    ok = not notes.errors
    pin = ("pinned: " + ", ".join(f"{what} {lo:#x}-{hi:#x} ({hi - lo} B)" for what, lo, hi in blocks)
           if blocks is not None else "not pinned")
    head = (f"{'ok' if ok else 'ERROR'}: the MP3 synth loop's hot lines, at most "
            + ", ".join(f"{n} in a set ({p})" for p, n in worst.items()) + f"; {pin}")
    return ok, ([head] + [f"ERROR: {e}" for e in notes.errors] + report
                + [f"warning: {w}" for w in notes.warnings])


def main(argv=None):
    ap = argparse.ArgumentParser(description="The MP3 synth loop's flash-cache sets (see the docstring).")
    ap.add_argument("elf", nargs="+")
    ap.add_argument("--require-pin", action="store_true",
                    help="fail unless tools/iram_diet.py's pin is in the link and holds the whole hot set")
    args = ap.parse_args(argv)
    failed = 0
    for path in args.elf:
        ok, lines = check(Elf(path), args.require_pin)
        print(f"{path}:\n  " + "\n  ".join(lines))
        failed += not ok
    return 1 if failed else 0


# ---------------------------------------------------------------------------
# PlatformIO: run on every build, after the link (as tools/flash_guard.py).
# SCons gives an extra script Import(); tools/iram_diet.py imports this
# module for HOT, and the command line runs main().
# ---------------------------------------------------------------------------
if "Import" in globals():
    from SCons.Script import COMMAND_LINE_TARGETS  # pylint: disable=import-error

    Import("env")  # noqa: F821  (provided by PlatformIO)

    def cache_guard(source, target, env):
        ok, lines = check(Elf(env.subst("$BUILD_DIR/${PROGNAME}.elf")), require_pin=True)
        if ok:  # the result, and any warning: a hot item the model reads only in part
            for line in lines:
                print(f"cache_guard: {line}")
            return 0
        for line in lines:
            sys.stderr.write(f"cache_guard: {line}\n")
        sys.stderr.write("cache_guard: the MP3 synth loop's cache check failed (tools/cache_guard.py)\n")
        return 1

    if not {"buildfs", "uploadfs", "uploadfsota", "nobuild"} & set(COMMAND_LINE_TARGETS):
        _action = env.Action(cache_guard)  # noqa: F821
        _action.strfunction = lambda target, source, env: ""
        _guard = env.AlwaysBuild(env.Alias("cache_guard", "$BUILD_DIR/${PROGNAME}.elf", _action))  # noqa: F821
        env.Default(_guard)  # noqa: F821
        env.Depends("upload", _guard)  # noqa: F821
elif __name__ == "__main__":
    sys.exit(main())
