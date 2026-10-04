# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""PlatformIO post-script: the build's own copy of the linker script (sections.ld).

It does two things to a copy of the framework's sections.ld: moves the
PSRAM-workaround libc functions out of IRAM (below), and pins the MP3 synth
loop's hot code and tables at the front of flash ("The MP3 hot set", further
down).

The prebuilt Arduino-ESP32 libs are built with CONFIG_SPIRAM_CACHE_WORKAROUND
(for rev-1 ESP32s) and the CONFIG_SPIRAM_CACHE_LIB*_IN_IRAM options, which link
~125 libc objects from libc instead of ROM and pin their code in IRAM (and
their rodata in internal DRAM). With Bluetooth, M5Unified and the speaker that
overflows the ESP32's 128 KB of IRAM. The Core2's rev-3 chip doesn't need the
workaround, so this does what those options set to "n" would: it writes a copy
of the framework's sections.ld into the build directory without the IRAM/DRAM
placements for libc's time, stdio, number-parsing, rand and strdup code, and
puts that directory first on the linker's search path (the same technique
pioarduino's relinker uses). The framework package itself is not modified.

mem*/str*, setjmp, locks and the syscall stubs stay in IRAM: those are the ones
an interrupt handler might call while the flash cache is off. The rest must
not be called then, which nothing here does.

When it moves nothing: each name in MOVE_TO_FLASH must match a placement line
"*libc.a:libc_a-<name>.*(...)" in the framework's sections.ld (its .iram0.text
and .dram0.data rules). If none does, the build stops with an error: a
toolchain or framework update renamed the objects, changed how sections.ld
lists them, or stopped pinning them (the SPIRAM workaround options turned off
upstream). Open the sections.ld the error names, search for "libc_a-", and
update MOVE_TO_FLASH (or the patterns below) to what it places in IRAM now; if
nothing of libc is pinned any more, the IRAM half of this script can go.
Names that no longer match while others do are left alone with a warning:
check the IRAM figure in the link's memory summary.

The MP3 hot set: the code libmad's synthesis and AudioGeneratorMP3 run on
every one of the loop's 36 passes a frame (dct32, synth_full, the per-sample
GetOneSample and loop, the outputs' ConsumeSample and the trim and feed
behind it), each with its literal pool, goes first in .flash.text; the
tables it reads (libmad's window D, the two outputs' vtables) first in
.flash.rodata. Both sections start at the same address in every build
(0x400D0020, 0x3F400120), so these lines sit in the same cache sets whatever
else changes, at most one of each block in a set: the 2-way flash cache keeps
them all. Unpinned, the layout decided it: 13-14 sets held three of them in
the seek bar's build and Opus's, and MP3 decoding lost a fifth of its speed
(docs/RESAMPLER.md section 10e). The list is HOT/PATHS in
tools/cache_guard.py, which checks the result after the link; it costs no
RAM and no flash. If an anchor line below isn't in the framework's
sections.ld any more, the build stops here: find where .flash.text and
.flash.rodata start their contents and update PIN_ANCHORS.
"""
import re
import sys
from pathlib import Path

Import("env")  # noqa: F821  (provided by PlatformIO)

sys.path.insert(0, str(Path(env.subst("$PROJECT_DIR")) / "tools"))  # noqa: F821
import cache_guard  # noqa: E402  (the hot set: HOT, PATHS)

MOVE_TO_FLASH = """
    asctime asctime_r ctime ctime_r gettzinfo gmtime gmtime_r lcltime lcltime_r
    mktime month_lengths strftime strptime time timelocal tzcalc_limits tzlock
    tzset tzset_r tzvars
    fclose fflush findfp fputwc fvwrite fwalk fwrite makebuf refill sccl stdio
    ungetc wbuf wsetup wcrtomb wctomb_r
    atoi atol itoa quorem rshift strtol strtoul utoa
    rand rand_r srand strdup strdup_r strndup strndup_r
""".split()

platform = env.PioPlatform()  # noqa: F821
mcu = env.BoardConfig().get("build.mcu")  # noqa: F821
original = Path(platform.get_package_dir("framework-arduinoespressif32-libs")) / mcu / "ld" / "sections.ld"
build_dir = Path(env.subst("$BUILD_DIR"))  # noqa: F821

script = original.read_text(encoding="utf-8")
before = len(script)
moved, missing = [], []
for name in MOVE_TO_FLASH:
    obj = re.escape(f"*libc.a:libc_a-{name}.*")
    # The object's own placement lines: code in .iram0.text, rodata in .dram0.data.
    script, placed = re.subn(rf"^\s*{obj}\([^)]*\)\s*\n", "", script, flags=re.M)
    # ...and its entries in the flash sections' EXCLUDE_FILE lists.
    script = re.sub(rf"\s{obj}(?=[\s)])", "", script)
    (moved if placed else missing).append(name)

if not moved:
    # Nothing matched: the framework's sections.ld no longer names these
    # objects as it did (a toolchain or framework update). Linking on would
    # overflow IRAM at best, or hide that the diet stopped working.
    sys.stderr.write(
        f"\niram_diet: ERROR: moved nothing: none of the {len(MOVE_TO_FLASH)} libc objects is placed in\n"
        f"  {original}\n"
        "  the way this script expects (a toolchain or framework update changed the objects or\n"
        "  their names). Read the docstring of tools/iram_diet.py, 'When it moves nothing', and\n"
        "  update MOVE_TO_FLASH or the patterns.\n\n")
    env.Exit(1)  # noqa: F821
if missing:
    print(f"iram_diet: warning: {len(missing)} of the objects aren't placed in IRAM any more, left alone: "
          f"{' '.join(missing)} (see the docstring, 'When it moves nothing')")

# --- The MP3 hot set, pinned at the front of flash (the docstring) ---
# Each block goes right after the line that opens its output section's
# contents, before the framework's own placements: (anchor, start symbol,
# end symbol, input-section lines).
code_lines, data_lines = cache_guard.pin_lines()
PIN_ANCHORS = (
    (r"^[ \t]*_text_start = ABSOLUTE\(\.\);[ \t]*\n", *cache_guard.PIN_CODE, code_lines),
    (r"^[ \t]*_flash_rodata_start = ABSOLUTE\(\.\);[ \t]*\n", *cache_guard.PIN_DATA, data_lines),
)
for anchor, start, end, lines in PIN_ANCHORS:
    block = "".join(f"    {line}\n" for line in
                    ["/* tools/iram_diet.py: the MP3 synth loop's hot set (tools/cache_guard.py) */",
                     f"{start} = ABSOLUTE(.);", *lines, f"{end} = ABSOLUTE(.);"])
    script, found = re.subn(anchor, lambda m, block=block: m.group(0) + block, script, flags=re.M)
    if found != 1:
        sys.stderr.write(
            f"\niram_diet: ERROR: can't pin the MP3 hot set: {found} lines match {anchor!r} in\n"
            f"  {original}\n"
            "  (one expected). Read the docstring of tools/iram_diet.py, 'The MP3 hot set'.\n\n")
        env.Exit(1)  # noqa: F821

build_dir.mkdir(parents=True, exist_ok=True)
(build_dir / "sections.ld").write_text(script, encoding="utf-8")
env.Prepend(LIBPATH=[str(build_dir)])  # noqa: F821  (found before the package's copy)
print(f"iram_diet: {len(moved)} of {len(MOVE_TO_FLASH)} libc objects moved to flash, the MP3 hot set "
      f"pinned ({len(code_lines)} code and {len(data_lines)} data lines; sections.ld {before} -> {len(script)} bytes)")
