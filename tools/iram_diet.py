# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""PlatformIO post-script: move PSRAM-workaround libc functions out of IRAM.

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
nothing of libc is pinned any more, this script and its line in platformio.ini
can go. Names that no longer match while others do are left alone with a
warning: check the IRAM figure in the link's memory summary.
"""
import re
import sys
from pathlib import Path

Import("env")  # noqa: F821  (provided by PlatformIO)

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

build_dir.mkdir(parents=True, exist_ok=True)
(build_dir / "sections.ld").write_text(script, encoding="utf-8")
env.Prepend(LIBPATH=[str(build_dir)])  # noqa: F821  (found before the package's copy)
print(f"iram_diet: {len(moved)} of {len(MOVE_TO_FLASH)} libc objects moved to flash "
      f"(sections.ld {before} -> {len(script)} bytes)")
