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
"""
import re
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
for name in MOVE_TO_FLASH:
    obj = re.escape(f"*libc.a:libc_a-{name}.*")
    # The object's own placement lines: code in .iram0.text, rodata in .dram0.data.
    script = re.sub(rf"^\s*{obj}\([^)]*\)\s*\n", "", script, flags=re.M)
    # ...and its entries in the flash sections' EXCLUDE_FILE lists.
    script = re.sub(rf"\s{obj}(?=[\s)])", "", script)

build_dir.mkdir(parents=True, exist_ok=True)
(build_dir / "sections.ld").write_text(script, encoding="utf-8")
env.Prepend(LIBPATH=[str(build_dir)])  # noqa: F821  (found before the package's copy)
print(f"iram_diet: {len(MOVE_TO_FLASH)} libc objects moved to flash "
      f"(sections.ld {before} -> {len(script)} bytes)")
