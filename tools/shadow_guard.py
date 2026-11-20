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

How: build_src_flags would reach src/ (and test/) only, and lib/core is a
library built in an environment of its own; a library.json for it would
change the builder PlatformIO picks for it. So, as tools/no_psram_fix.py
does, this registers a build middleware. Every environment that collects
sources (the project's, each library's, the Arduino core's) calls it for
each file before it compiles anything, and an environment collecting one of
our files gets the flag added to its CCFLAGS in place: the project's
(src/, or test/ in native) and lib/core's. An earlier middleware may have
turned the file into an object already (tools/version.py's, for src/): its
source is then the object's.
"""
import os

from SCons.Node.FS import File  # pylint: disable=import-error

Import("env")  # noqa: F821  (provided by PlatformIO)

FLAG = "-Werror=shadow=local"


def _ours():
    project = env.subst("$PROJECT_DIR")  # noqa: F821
    dirs = (env.subst("$PROJECT_SRC_DIR"), os.path.join(project, "lib", "core"), env.subst("$PROJECT_TEST_DIR"))  # noqa: F821
    return tuple(os.path.normcase(os.path.join(os.path.abspath(d), "")) for d in dirs)


OURS = _ours()


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
    if source_path(node).startswith(OURS):
        build_env.AppendUnique(CCFLAGS=[FLAG])
    return node


env.AddBuildMiddleware(guard)  # noqa: F821
print(f"shadow_guard: {FLAG} on src/, lib/core and test/")
