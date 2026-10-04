# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Tests for tools/cache_guard.py (the MP3 synth loop's flash-cache check).

    python -m unittest discover -s tools -p "test_cache_guard.py"

No ELF: each test makes up a symbol table and the bytes behind it (code of
NOPs, a few L32Rs, vtables), as the linker would have laid them out.
"""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cache_guard as cg  # noqa: E402

NOP = bytes.fromhex("f02000")    # nop
NOP_N = bytes.fromhex("3df0")    # nop.n
RETW_N = bytes.fromhex("1df0")   # retw.n


def fill(size):
    """`size` bytes of NOPs (size >= 2, not 1)."""
    k, rest = divmod(size, 3)
    if rest == 1:
        return NOP * (k - 1) + NOP_N * 2
    return NOP * k + NOP_N * (rest // 2)


def l32r(pc, target, reg=8):
    """L32R a<reg>, target (target below pc, 4-aligned)."""
    imm16 = ((target - ((pc + 3) & ~3)) >> 2) & 0xFFFF
    return bytes([reg << 4 | 0x1, imm16 & 0xFF, imm16 >> 8])


def j(pc, target):
    w = ((target - (pc + 4)) & 0x3FFFF) << 6 | 0x6
    return bytes([w & 0xFF, w >> 8 & 0xFF, w >> 16])


def bnez_n(pc, target, reg=2):
    imm6 = target - (pc + 4)
    assert 0 <= imm6 < 64
    w = 0xC | (0b1100 | imm6 >> 4) << 4 | reg << 8 | (imm6 & 0xF) << 12
    return bytes([w & 0xFF, w >> 8])


class FakeImage:
    """What cache_guard reads from an ELF: symbols, addresses, bytes."""

    def __init__(self):
        self.symbols, self.addresses, self.blobs = {}, {}, []

    def add(self, name, addr, size, kind=cg.STT_FUNC, file=None, blob=None):
        self.symbols.setdefault(name, []).append(cg.Symbol(name, addr, size, kind, file))
        self.addresses.setdefault(name, addr)
        if blob is not None:
            assert len(blob) == size, name
            self.blobs.append((addr, blob))

    def add_code(self, name, addr, size, literals=(), file=None):
        code = b"".join(l32r(addr + 3 * i, t) for i, t in enumerate(literals))
        self.add(name, addr, size, cg.STT_FUNC, file, code + fill(size - len(code) - 2) + RETW_N)

    def read(self, addr, size):
        for start, blob in self.blobs:
            if start <= addr and addr + size <= start + len(blob):
                return blob[addr - start:addr - start + size]
        return None


# A rebuild of 9fd02e6 (the seek bar's merge), from its ELF: 13
# sets of dct32 + D + GetOneSample/loop on both paths.
SEEK_BAR = {
    "dct32": 0x40128F5C, "synth_full": 0x4012A070, "mad_synth_frame_onens": 0x4012A9D4, "D": 0x3F46D450,
    "gos": 0x40121760, "loop": 0x4012181C, "counting": 0x4022DFA4, "ring": 0x400DE7BC,
    "trimmed": 0x4014A0EC, "feed": 0x401435B4, "ring_vt": 0x3F42E448, "counting_vt": 0x3F42E490,
}
SIZES = {
    "dct32": 4372, "synth_full": 1188, "mad_synth_frame_onens": 126, "D": 2176, "gos": 187, "loop": 225,
    "counting": 34, "ring": 205, "trimmed": 213, "feed": 163, "ring_vt": 60, "counting_vt": 60,
}


def vtable(target, slot=8, words=15):
    return struct.pack(f"<{words}I", *[(target if i == slot else 0x400D0000 + 4 * i) for i in range(words)])


def image(at=SEEK_BAR, literals=None, pin=None, drop=()):
    """A made-up image with the hot set at `at` ({key: address}); `literals`:
    {key: [targets]}; `pin`: ((code start, end), (data start, end))."""
    literals = literals or {}
    img = FakeImage()
    code = {
        "dct32": ("dct32", "synth.c"), "synth_full": ("synth_full", "synth.c"),
        "mad_synth_frame_onens": ("mad_synth_frame_onens", None),
        "gos": ("_ZN17AudioGeneratorMP312GetOneSampleEPs", None), "loop": ("_ZN17AudioGeneratorMP34loopEv", None),
        "counting": (cg.COUNTING_CS, "Core2AudioBackend.cpp"), "ring": (cg.RING_CS, None),
        "trimmed": ("_ZN8TrimFeed14consumeTrimmedEPKs", None), "feed": ("_ZN8RingFeed7consumeEPKs", None),
    }
    for key, (name, file) in code.items():
        if key not in drop:
            img.add_code(name, at[key], SIZES[key], literals.get(key, ()), file)
    if "D" not in drop:
        img.add("D", at["D"], SIZES["D"], cg.STT_OBJECT, "synth.c")
    img.add("_ZTV10RingOutput", at["ring_vt"], 60, cg.STT_OBJECT, None, vtable(at["ring"]))
    img.add("_ZTVN12_GLOBAL__N_114CountingOutputE", at["counting_vt"], 60, cg.STT_OBJECT, "Core2AudioBackend.cpp",
            vtable(at["counting"], slot=6))
    if pin:
        (c0, c1), (d0, d1) = pin
        img.addresses.update({cg.PIN_CODE[0]: c0, cg.PIN_CODE[1]: c1, cg.PIN_DATA[0]: d0, cg.PIN_DATA[1]: d1})
    return img


def pinned(text=0x400D0020, rodata=0x3F400120):
    """The hot set as tools/iram_diet.py lays it out: code from the start of
    .flash.text, data from the start of .flash.rodata."""
    at, pos = {}, text
    for key in ("dct32", "synth_full", "mad_synth_frame_onens", "gos", "loop", "counting", "ring", "trimmed", "feed"):
        at[key] = pos
        pos = (pos + SIZES[key] + 3) & ~3
    code_end, pos = pos, rodata
    for key in ("D", "counting_vt", "ring_vt"):
        at[key] = pos
        pos += SIZES[key]
    return at, ((text, code_end), (rodata, pos))


class DecoderTest(unittest.TestCase):
    def test_l32r_targets(self):
        base = 0x40100000
        code = l32r(base, base - 0x40) + l32r(base + 3, base - 0x1000)
        found, unreached = cg.literals(code + RETW_N, base)
        self.assertEqual(found, [base - 0x1000, base - 0x40])
        self.assertEqual(unreached, 0)

    def test_padding_after_a_jump_is_not_code(self):
        # j over two bytes of zero padding; the padding, read as code, would
        # take the next instruction's bytes for a 3-byte one (the case
        # objdump's linear sweep gets wrong in TrimFeed::consumeTrimmed).
        base = 0x40100000
        lit = base - 0x100
        code = j(base, base + 5) + b"\0\0" + l32r(base + 5, lit) + RETW_N
        self.assertEqual(cg.literals(code, base), ([lit], 0))

    def test_branches_are_followed(self):
        base = 0x40100000
        a, b = base - 0x20, base - 0x80
        # bnez.n to the second L32R, the first L32R, retw.n, then the target.
        code = bnez_n(base, base + 7) + l32r(base + 2, a) + RETW_N + l32r(base + 7, b) + RETW_N
        self.assertEqual(cg.literals(code, base), (sorted([a, b]), 0))

    def test_unreached_bytes_are_reported_not_decoded(self):
        base = 0x40100000
        code = RETW_N + l32r(base + 2, base - 0x40)  # after the return: only a jump table would get there
        self.assertEqual(cg.literals(code, base), ([], 3))

    def test_an_impossible_opcode_raises(self):
        with self.assertRaises(ValueError):
            cg.literals(b"\x0e\x00\x00", 0x40100000)


class ModelTest(unittest.TestCase):
    def test_the_seek_bar_layout_fails_on_both_paths(self):
        ok, lines = cg.check(image())
        self.assertFalse(ok)
        text = "\n".join(lines)
        self.assertIn("13 (bench), 13 (playback)", text)
        self.assertIn("sets 187-191: AudioGeneratorMP3::GetOneSample + D + dct32", text)
        self.assertIn("sets 193-199: AudioGeneratorMP3::loop + D + dct32", text)
        self.assertIn("D: 0x3f46d450, 2176 B: 0x1450 into the way, sets 162-230", text)

    def test_ten_kb_of_rodata_more_passes_unpinned(self):
        at = dict(SEEK_BAR, D=SEEK_BAR["D"] + 10240, ring_vt=SEEK_BAR["ring_vt"] + 10240,
                  counting_vt=SEEK_BAR["counting_vt"] + 10240)
        ok, lines = cg.check(image(at))
        self.assertTrue(ok, lines)
        self.assertIn("not pinned", lines[0])
        ok, lines = cg.check(image(at), require_pin=True)  # the build's check: the pin must be there
        self.assertFalse(ok)
        self.assertIn("isn't pinned", "\n".join(lines))

    def test_the_trimmed_playback_path_counts(self):
        # The seek work's layout (67dd141, from its ELF): the bench is clean,
        # but loop, dct32 and consumeTrimmed (a track with a LAME tag) share 6 sets.
        at = {"dct32": 0x4012874C, "synth_full": 0x40129860, "mad_synth_frame_onens": 0x4012A1C4,
              "D": 0x3F46D1B8, "gos": 0x40120F50, "loop": 0x4012100C, "counting": 0x4022CE98,
              "ring": 0x400DE6C0, "trimmed": 0x4014904C, "feed": 0x40142958, "ring_vt": 0x3F42E1C8,
              "counting_vt": 0x3F42E210}
        ok, lines = cg.check(image(at))
        self.assertFalse(ok)
        text = "\n".join(lines)
        self.assertIn("0 (bench), 6 (playback)", text)
        self.assertIn("sets 130-135: AudioGeneratorMP3::loop + TrimFeed::consumeTrimmed + dct32", text)

    def test_code_in_iram_is_out_but_a_literal_is_a_line(self):
        # The seek bar's layout with GetOneSample and loop in IRAM (not
        # cached): D and dct32 share sets 162-230, two lines each: fine. Then
        # dct32 loads a literal that sits in set 170: a third line there.
        at = dict(SEEK_BAR, gos=0x40090000, loop=0x40090100)
        ok, lines = cg.check(image(at))
        self.assertTrue(ok, lines)
        lit = 0x40110000 + 170 * cg.LINE
        ok, lines = cg.check(image(at, literals={"dct32": [lit]}))
        self.assertFalse(ok)
        text = "\n".join(lines)
        self.assertIn("1 (bench), 1 (playback)", text)
        self.assertIn("set 170: D + dct32 + dct32's literals", text)

    def test_the_vtable_word_is_the_one_read(self):
        notes = cg.Notes()
        pieces = cg.pieces_of(image(), cg.PATHS[1][1][3], notes)
        self.assertEqual(notes.errors, [])
        self.assertEqual(pieces, [cg.Piece("RingOutput's vtable", SEEK_BAR["ring_vt"] + 4 * 8, 4)])

    def test_a_missing_symbol_fails(self):
        ok, lines = cg.check(image(drop=("dct32",)))
        self.assertFalse(ok)
        self.assertIn("dct32: no symbol dct32 from synth.c", "\n".join(lines))

    def test_a_local_symbol_is_told_apart_by_its_file(self):
        img = image(dict(SEEK_BAR, D=SEEK_BAR["D"] + 10240, ring_vt=SEEK_BAR["ring_vt"] + 10240,
                         counting_vt=SEEK_BAR["counting_vt"] + 10240))
        img.add("D", 0x3F450000, 64, cg.STT_OBJECT, "other.c")
        self.assertTrue(cg.check(img)[0])
        img.add("D", 0x3F460000, 64, cg.STT_OBJECT, "synth.c")
        ok, lines = cg.check(img)
        self.assertFalse(ok)
        self.assertIn("2 symbols named D in synth.c", "\n".join(lines))


class PinTest(unittest.TestCase):
    def test_pinned_passes_wherever_the_rest_lands(self):
        at, pin = pinned()
        ok, lines = cg.check(image(at, pin=pin), require_pin=True)
        self.assertTrue(ok, lines)
        self.assertIn("at most 2 in a set (bench), 2 in a set (playback); pinned: code 0x400d0020-", lines[0])

    def test_pinned_holds_for_every_offset_of_the_blocks(self):
        # The blocks are at fixed addresses, but even if both moved, one way
        # each can't make three: try every 32-byte step of the data block.
        at, ((c0, c1), (d0, d1)) = pinned()
        for step in range(0, cg.WAY, cg.LINE):
            moved = {k: (v + step if v < 0x40000000 else v) for k, v in at.items()}
            ok, lines = cg.check(image(moved, pin=((c0, c1), (d0 + step, d1 + step))), require_pin=True)
            self.assertTrue(ok, (step, lines))

    def test_an_item_out_of_the_pin_fails(self):
        at, pin = pinned()
        at["trimmed"] = 0x4014A0EC  # where it was before: its pattern didn't match
        ok, lines = cg.check(image(at, pin=pin), require_pin=True)
        self.assertFalse(ok)
        self.assertIn("TrimFeed::consumeTrimmed at 0x4014a0ec is out of the pin", "\n".join(lines))

    def test_a_block_over_one_way_fails(self):
        at, ((c0, _c1), data) = pinned()
        ok, lines = cg.check(image(at, pin=((c0, c0 + cg.WAY + 4), data)), require_pin=True)
        self.assertFalse(ok)
        self.assertIn("over one way of the cache", "\n".join(lines))

    def test_pin_lines_name_every_item_once(self):
        code, data = cg.pin_lines()
        self.assertEqual(len(code) + len(data), len(cg.all_hot()))
        self.assertIn("*libESP8266Audio.a:synth.c.o(.literal.dct32 .text.dct32)", code)
        self.assertIn("*libESP8266Audio.a:synth.c.o(.rodata.D)", data)
        self.assertIn("*(.rodata._ZTV10RingOutput)", data)


class FormatTest(unittest.TestCase):
    def test_ranges(self):
        self.assertEqual(cg.ranges([5, 3, 4, 9, 12, 13]), "3-5, 9, 12-13")
        self.assertEqual(cg.ranges([]), "")


if __name__ == "__main__":
    unittest.main()
