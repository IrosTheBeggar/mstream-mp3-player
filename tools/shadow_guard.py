# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""PlatformIO pre-script: a local that shadows a local fails our own build.

GCC's -Wshadow=local warns when a local, a parameter or a lambda's parameter
hides a local or a parameter of an enclosing scope; this makes it an error,
-Werror=shadow=local, in the code this repository owns: src/, lib/core and
test/ (the host tests). 0.14.0's Now Playing redrew its whole empty state on
every lit pass because of one: a second `noCard`, an always-true result, hid
the card's test inside NowPlayingPage::update(). (The flag also covers the
compatible-local class that one was.)

Only this class. The full -Wshadow is left off: on src/ and lib/core it gave
28 warnings when this was added, 17 of them a parameter or local named as a
member (a constructor's `alloc` and `release`, say) and 5 as a global, not
read one by one (docs/ARCHITECTURE.md, "Build notes").

The framework, the libraries in .pio/libdeps and lib/SD (the Arduino core's
SD library, copied) keep their own flags: their code isn't ours to rename.
Their headers are checked all the same where our files include them: they
come in with -I, not -isystem, so an inline function or a template in one
(M5GFX's, M5Unified's, the IDF's) is compiled with the flag. None trips it
today. One that an upgrade brings goes between pragmas around its include:
`#pragma GCC diagnostic push`, `ignored "-Wshadow=local"`, `ignored
"-Wshadow=compatible-local"` (both: ignoring "-Wshadow" leaves them on) and
`pop`.

The flag goes on an environment, not a file. In native the project's
environment compiles each suite's own files and PlatformIO's generated
unity_config.c, which has it too, as has FatFs (test/support/fatfs, which a
suite's fatfs_unit.c includes): FatFsBuild.c takes it out for FatFs with
those pragmas.

How: build_src_flags would reach src/ (and test/) only, and lib/core is a
library built in an environment of its own; a library.json for it would
change the builder PlatformIO picks for it. So, as tools/no_psram_fix.py
does, this registers a build middleware. Every environment that collects
sources (the project's, each library's, the Arduino core's) calls it for
each file before it compiles anything, and an environment collecting one of
our files gets the flag added to its CCFLAGS in place: the project's
(src/, or test/ in native) and lib/core's (the core folder in the project's
lib_dir). An earlier middleware may have turned the file into an object
already (tools/version.py's, for src/): its source is then the object's.

It says what it reached, as a guard that matched nothing would pass in
silence: on every build (the default targets, which pio test's build
includes, and before an upload) it prints the files it gave the flag by
folder, core2's "shadow_guard: -Werror=shadow=local on 219 of our files
(src/ 70, lib/core 149)" with main 4454316 merged in (each new file of src/
or lib/core adds one), and none at all fails the build.
"""
import os
import sys

from SCons.Node.FS import File  # pylint: disable=import-error
from SCons.Script import COMMAND_LINE_TARGETS  # pylint: disable=import-error

Import("env")  # noqa: F821  (provided by PlatformIO)

FLAG = "-Werror=shadow=local"


def _ours():
    """(folder, prefix) for each of ours, the prefix as source_path() gives
    paths."""
    core = os.path.join(env.GetProjectConfig().get("platformio", "lib_dir"), "core")  # noqa: F821
    if not os.path.isdir(core):
        raise SystemExit(f"shadow_guard: no core folder in the project's lib_dir ({core})")
    dirs = (("src/", env.subst("$PROJECT_SRC_DIR")), ("lib/core", core), ("test/", env.subst("$PROJECT_TEST_DIR")))  # noqa: F821
    return tuple((name, os.path.normcase(os.path.join(os.path.abspath(d), ""))) for name, d in dirs)


OURS = _ours()
GUARDED = {name: set() for name, _ in OURS}  # the files given the flag, by folder


def source_path(node):
    """The source file a node compiles: a file in the variant directory, or
    an object an earlier middleware made of one."""
    if not isinstance(node, File):
        try:
            node = node[0].sources[0]
        except (IndexError, TypeError, AttributeError):
            raise SystemExit(f"shadow_guard: can't tell which source {node!r} is") from None
    return os.path.normcase(node.srcnode().get_abspath())


def guard(build_env, node):
    path = source_path(node)
    for name, prefix in OURS:
        if path.startswith(prefix):
            build_env.AppendUnique(CCFLAGS=[FLAG])
            GUARDED[name].add(path)
            break
    return node


def report(target, source, env):  # pylint: disable=unused-argument
    """Run after every SConscript is read, so after every file is collected."""
    total = sum(len(files) for files in GUARDED.values())
    if not total:
        sys.stderr.write(f"shadow_guard: {FLAG} reached no file of src/, lib/core or test/: the guard checks "
                         "nothing (tools/shadow_guard.py)\n")
        return 1
    folders = ", ".join(f"{name} {len(files)}" for name, files in GUARDED.items() if files)
    print(f"shadow_guard: {FLAG} on {total} of our files ({folders})")
    return 0


env.AddBuildMiddleware(guard)  # noqa: F821
if not {"buildfs", "uploadfs", "uploadfsota", "nobuild"} & set(COMMAND_LINE_TARGETS):
    # An alias with no source, as nothing it reads is a file: an action
    # added here to the program would be lost (tools/version.py), and its
    # name isn't known yet.
    _action = env.Action(report)  # noqa: F821
    _action.strfunction = lambda target, source, env: ""
    _report = env.AlwaysBuild(env.Alias("shadow_guard", [], _action))  # noqa: F821
    env.Default(_report)  # noqa: F821
    env.Depends("upload", _report)  # noqa: F821
