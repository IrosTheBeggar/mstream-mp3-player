# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""PlatformIO pre-script: compile our code without the rev-1 PSRAM workaround.

The prebuilt Arduino-ESP32 libs are built with CONFIG_SPIRAM_CACHE_WORKAROUND,
and their build script adds its compiler flags to every compile and link:
-mfix-esp32-psram-cache-issue -mfix-esp32-psram-cache-strategy=memw. They work
around a rev-1 ESP32 cache bug by putting a `memw` after every 8- and 16-bit
store (RESAMPLER.md), which the decoders, the rate converter and above all the
UI (M5GFX writes its pixels and glyphs into the PSRAM canvases as 16-bit
stores) pay for on every one.

The Core2 is an ESP32-D0WDQ6-V3 (rev 3.x): ECO3 fixed the bug in silicon, and
IDF needs the workaround only below rev 3 (its Kconfig option depends on
ESP32_REV_MIN_FULL < 300). The flags apply per object, and on rev 3 silicon
objects built with and without them mix safely. So this drops them from
everything PlatformIO compiles: src/, lib/core, the libraries (M5GFX,
ESP8266Audio, ESP32-A2DP) and the Arduino core's sources.

It keeps them in LINKFLAGS: they pick the toolchain's libc/libgcc multilib, so
the link uses the same libc/libgcc as before and the prebuilt IDF libs (still
built with the workaround) and their IRAM layout don't move; only our code
shrinks. Turning the workaround off in the prebuilt libs too (and freeing the
libc copies they pin in IRAM) needs the framework rebuilt with a custom
sdkconfig: docs/ENERGY.md section 5, P3b.

The shipped sdkconfig doesn't refuse an older chip (ESP32_REV_MIN is 0), so
setup() checks the revision first and halts below 3 (lib/core/ChipRevision.h).

How: the framework's flags are added after this script runs (pre: scripts
come first), so it registers a build middleware. Every environment that
collects sources (the project's, each library's, the Arduino core's) calls
it before it compiles anything, and it filters the flags out of that
environment's CCFLAGS, CFLAGS, CXXFLAGS, ASFLAGS and ASPPFLAGS. LINKFLAGS
belong to the program's environment, which compiles nothing itself.

`custom_psram_cache_fix = keep` in the environment (a local.ini can set it for
[env:core2]) leaves the flags alone: the build as it was, for an A/B.
"""
Import("env")  # noqa: F821  (provided by PlatformIO)

FLAGS = ("-mfix-esp32-psram-cache-issue", "-mfix-esp32-psram-cache-strategy=memw")
SCOPES = ("CCFLAGS", "CFLAGS", "CXXFLAGS", "ASFLAGS", "ASPPFLAGS")

mode = env.GetProjectOption("custom_psram_cache_fix", "drop").strip().lower()  # noqa: F821
if mode not in ("drop", "keep"):
    raise SystemExit(f"no_psram_fix: custom_psram_cache_fix must be drop or keep, not {mode!r}")


def without_fix(build_env, node):
    """Drops the flags from the environment collecting this file, in place
    (once: later calls find nothing to drop). In place rather than an
    env.Object() with overrides: tools/version.py's middleware already
    returns the project's sources as objects, and its override environment
    reads everything else from this one when the command runs."""
    for scope in SCOPES:
        flags = build_env.get(scope)
        if not flags:
            continue
        flat = build_env.Flatten(flags)
        kept = [f for f in flat if f not in FLAGS]
        if len(kept) != len(flat):
            build_env.Replace(**{scope: kept})
    return node


if mode == "drop":
    env.AddBuildMiddleware(without_fix)  # noqa: F821
    print("no_psram_fix: compiling without -mfix-esp32-psram-cache-issue (rev 3 silicon; kept for the link)")
else:
    print("no_psram_fix: custom_psram_cache_fix = keep: the rev-1 PSRAM workaround stays in every compile")
