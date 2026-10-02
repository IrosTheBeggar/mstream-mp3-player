# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""PlatformIO pre-script: the firmware's version, from git.

Runs `git describe --tags --match "v[0-9]*" --always --dirty` and writes
$BUILD_DIR/generated/PlayerVersion.h (PLAYER_VERSION, the tag when the build
is exactly one, the commit and its date; tools/package_release.py reads it
too). Only src/app/Version.cpp includes it, so a new commit recompiles that
one file and relinks; a global -D would rebuild everything, libraries too.
The header is rewritten only when its text changes. The date is the commit's
(UTC), never the build's: building the same commit again names it the same.

The version reads:
- "v0.5.0": a release, built from its tag;
- "v0.5.0-3-gabc1234-dirty": 3 commits past v0.5.0, with uncommitted changes
  (git describe; "-dirty" only for tracked files);
- "v0.6.0-dev+abc1234[-dirty]": no v* tag reachable (before the first
  release, or a clone without its tags), so the next release's dev build.

With RELEASE=1 in the environment (CI sets it when it builds a tag), the build
fails unless HEAD is exactly a SemVer v* tag, the tree is clean (untracked
files count: they would be built but aren't in the tag), BT_SINK_NAME is
empty, and no local*.ini adds build flags. A release is then exactly the
tagged source, the Corresponding Source its GPL offer names. RELEASE_TAG
(CI sets it to the tag that triggered the run) names the tag: it must be
HEAD, and it is the version, whichever tag git describe would prefer (an
annotated v0.5.0-rc.1 on the same commit wins over a lightweight v0.5.0).
The header's PLAYER_RELEASE is 1 only for such a build:
tools/package_release.py --release packages nothing else.

After every build, this also checks that the app image (firmware.bin) carries
the version in its app description (esp_app_desc, defined in Version.cpp: what
a future OTA update compares) and the ELF's SHA-256 (what About shows, for
crash reports).
"""
import configparser
import datetime
import glob
import hashlib
import os
import re
import struct
import subprocess
import sys

from SCons.Script import COMMAND_LINE_TARGETS  # pylint: disable=import-error

Import("env")  # noqa: F821  (provided by PlatformIO)

# The next release: builds with no v* tag reachable read "v<this>-dev+<hash>".
# Set it to the version about to be tagged before tagging it (a release build
# warns when they differ); once that tag exists, git describe names builds
# after it and this only matters in clones without tags.
NEXT_RELEASE = "0.6.0"

# SemVer 2.0.0 with a "v": v0.5.0, v1.2.3-beta.1, v1.2.3+build.5.
SEMVER = re.compile(r"^v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)"
                    r"(?:-((?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*)(?:\.(?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*))*))?"
                    r"(?:\+([0-9a-zA-Z-]+(?:\.[0-9a-zA-Z-]+)*))?$")
# esp_app_desc_t's version field: 32 bytes with its NUL.
DESC_VERSION_MAX = 31

PROJECT_DIR = env.subst("$PROJECT_DIR")  # noqa: F821
GEN_DIR = os.path.join(env.subst("$BUILD_DIR"), "generated")  # noqa: F821
HEADER = os.path.join(GEN_DIR, "PlayerVersion.h")


def git(*args):
    """git's output, stripped, or None (no git, not a repository, no match)."""
    try:
        out = subprocess.run(["git", *args], cwd=PROJECT_DIR, capture_output=True, text=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    return out.stdout.strip()


def tag_commit(tag):
    """The commit a tag points to, or None (no such tag)."""
    return git("rev-parse", "--verify", "--quiet", f"refs/tags/{tag}^{{commit}}")


def describe(release_tag):
    """(version, tag, commit, commit date, commit time): from git, or a
    placeholder without it. tag is the v* tag HEAD is exactly, with no tracked
    file changed (so the version is the tag), else "". A release_tag that is
    HEAD is both (release_errors refuses one that isn't, or a dirty tree)."""
    commit = git("rev-parse", "HEAD")
    if not commit:
        return f"v{NEXT_RELEASE}-dev+nogit", "", "", "", ""
    stamp = datetime.datetime.fromtimestamp(int(git("log", "-1", "--format=%ct")), datetime.timezone.utc)
    if release_tag and tag_commit(release_tag) == commit:
        return release_tag, release_tag, commit, stamp.strftime("%Y-%m-%d"), stamp.strftime("%H:%M:%S")
    found = git("describe", "--tags", "--match", "v[0-9]*", "--always", "--dirty")
    if not found.startswith("v"):  # --always: only a hash, no v* tag reachable
        short, dirty = found.partition("-dirty")[:2]
        found = f"v{NEXT_RELEASE}-dev+{short}" + ("-dirty" if dirty else "")
    tag = git("describe", "--tags", "--exact-match", "--match", "v[0-9]*", "HEAD") or ""
    return found, (tag if tag == found else ""), commit, stamp.strftime("%Y-%m-%d"), stamp.strftime("%H:%M:%S")


def flag_values(name):
    """Every value given to -D<name> in this environment's build flags (and
    PLATFORMIO_BUILD_FLAGS); "1" for a bare -D<name>."""
    flags = list(env.GetProjectOption("build_flags", []))  # noqa: F821  (local.ini's [local] included)
    flags += [str(f) for f in env.get("BUILD_FLAGS", [])]  # noqa: F821
    flags.append(os.environ.get("PLATFORMIO_BUILD_FLAGS", ""))
    values = [("1" if m.group(1) is None else m.group(1))
              for m in re.finditer(rf"-D\s*{name}(?:=(\S*))?(?=\s|$)", " ".join(flags))]
    return list(dict.fromkeys(values))  # the same flag reaches both lists


def local_flags():
    """(file, section, option) for every non-empty *flags option in a local*.ini."""
    found = []
    for path in sorted(glob.glob(os.path.join(PROJECT_DIR, "local*.ini"))):
        ini = configparser.ConfigParser(inline_comment_prefixes=(";", "#"), interpolation=None, strict=False)
        ini.read(path, encoding="utf-8")
        for section in ini.sections():
            for option, value in ini.items(section):
                if option.endswith("flags") and value.strip():
                    found.append((os.path.basename(path), section, option))
    return found


def release_errors(version, release_tag):
    errors = []
    head = git("rev-parse", "HEAD")
    if not head:
        return ["not a git checkout: a release is built from its tag"]
    tag = None
    if release_tag:
        target = tag_commit(release_tag)
        if not target:
            errors.append(f"RELEASE_TAG {release_tag} isn't a tag in this checkout")
        elif target != head:
            errors.append(f"RELEASE_TAG {release_tag} is commit {target[:7]}, but HEAD is {head[:7]}")
        else:
            tag = release_tag
    else:
        tag = git("describe", "--tags", "--exact-match", "--match", "v[0-9]*", "HEAD")
        if not tag:
            errors.append(f"HEAD isn't tagged (it describes as {version}): a release is built from a v* tag")
    if not tag:
        pass  # what is wrong is said above
    elif not SEMVER.match(tag):
        errors.append(f"the tag {tag} isn't SemVer (vMAJOR.MINOR.PATCH, optionally -prerelease)")
    elif len(tag) > DESC_VERSION_MAX:
        errors.append(f"the tag {tag} is longer than the app description's {DESC_VERSION_MAX} characters")
    elif tag != f"v{NEXT_RELEASE}":
        print(f"version: WARNING: the tag is {tag} but NEXT_RELEASE (tools/version.py) is {NEXT_RELEASE}")
    status = git("status", "--porcelain", "--untracked-files=normal")
    if status is None or status:
        changed = (status or "").splitlines()
        errors.append("the tree isn't clean (git status: " + "; ".join(c.strip() for c in changed[:5]) +
                      ("; ..." if len(changed) > 5 else "") + ")")
    for value in flag_values("BT_SINK_NAME"):
        if value.replace("\\", "").strip("\"'"):
            errors.append(f"BT_SINK_NAME is set ({value}): a release scans for no headphones by name")
    for name, section, option in local_flags():
        errors.append(f"{name} sets [{section}] {option}: a release builds with the repository's flags only")
    return errors


def c_string(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_header(version, tag, commit, date, time, release):
    text = "\n".join([
        "// Generated by tools/version.py on every build: don't edit, don't commit.",
        "#pragma once",
        f"#define PLAYER_VERSION {c_string(version)}",
        # What esp_app_desc_t's 32 bytes hold: the whole version when it fits.
        f"#define PLAYER_VERSION_DESC {c_string(version[:DESC_VERSION_MAX])}",
        # The tag this build is exactly ("" past it, or dirty): what
        # tools/package_release.py names the source and the release after.
        f"#define PLAYER_TAG {c_string(tag)}",
        # 1: built with RELEASE=1, every release check passed (a build that
        # failed one stopped before this was written).
        f"#define PLAYER_RELEASE {1 if release else 0}",
        f"#define PLAYER_COMMIT {c_string(commit)}",
        f"#define PLAYER_COMMIT_DATE {c_string(date)}  // UTC",
        f"#define PLAYER_COMMIT_TIME {c_string(time)}",
        "",
    ])
    os.makedirs(GEN_DIR, exist_ok=True)
    try:
        with open(HEADER, encoding="utf-8") as f:
            if f.read() == text:
                return
    except OSError:
        pass
    with open(HEADER, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def add_include(node_env, node):
    """The generated header's directory on the include path of the project's
    own sources only (the libraries' command lines stay as they were)."""
    if not os.path.normcase(node.srcnode().get_abspath()).startswith(
            os.path.normcase(os.path.join(env.subst("$PROJECT_SRC_DIR"), ""))):  # noqa: F821
        return node
    return node_env.Object(node, CPPPATH=node_env.get("CPPPATH", []) + [GEN_DIR])


def check_image(source, target, env):  # pylint: disable=redefined-outer-name
    """The built app image's esp_app_desc (right after the image and first
    segment headers): its magic, our version, and the ELF's SHA-256."""
    bin_path = str(source[0])
    elf_path = os.path.splitext(bin_path)[0] + ".elf"
    with open(bin_path, "rb") as f:
        desc = f.read(0x20 + 256)[0x20:]
    magic = struct.unpack_from("<I", desc, 0)[0]
    in_image = desc[16:48].split(b"\0")[0].decode("ascii", "replace")
    with open(elf_path, "rb") as f:
        elf_sha = hashlib.sha256(f.read()).digest()
    errors = []
    if magic != 0xABCD5432:
        errors.append(f"no app description at the image's start (magic {magic:#x})")
    elif in_image != VERSION[:DESC_VERSION_MAX]:
        errors.append(f"the app description's version is \"{in_image}\", not \"{VERSION}\" "
                      "(the framework's esp_app_desc was linked instead of src/app/Version.cpp's)")
    if desc[144:176] != elf_sha:
        errors.append("the app description's ELF SHA-256 isn't firmware.elf's")
    if errors:
        for e in errors:
            sys.stderr.write(f"version: ERROR: {e}\n")
        return 1
    print(f"version: the image says {in_image}, ELF {elf_sha.hex()[:8]}")
    return 0


RELEASE = os.environ.get("RELEASE", "") not in ("", "0")
RELEASE_TAG = os.environ.get("RELEASE_TAG", "").strip() if RELEASE else ""
VERSION, TAG, COMMIT, DATE, TIME = describe(RELEASE_TAG)
if RELEASE:
    problems = release_errors(VERSION, RELEASE_TAG)
    if problems:
        for p in problems:
            sys.stderr.write(f"version: RELEASE: {p}\n")
        sys.stderr.write("version: RELEASE=1 refuses this build (tools/version.py)\n")
        env.Exit(1)  # noqa: F821
write_header(VERSION, TAG, COMMIT, DATE, TIME, RELEASE)
env.AddBuildMiddleware(add_include)  # noqa: F821
if not {"buildfs", "uploadfs", "uploadfsota", "nobuild"} & set(COMMAND_LINE_TARGETS):
    # On every build and before every upload, after firmware.bin (as
    # flash_guard does: a post-action added here, before the platform's
    # builder exists, would be lost).
    check_action = env.Action(check_image)  # noqa: F821
    check_action.strfunction = lambda target, source, env: ""
    # PROGNAME is still unset here: the platform's builder names it as below.
    progname = env.get("PROGNAME", "program")  # noqa: F821
    image = os.path.join(env.subst("$BUILD_DIR"), ("firmware" if progname == "program" else progname) + ".bin")  # noqa: F821
    check = env.AlwaysBuild(env.Alias("version_check", image, check_action))  # noqa: F821
    env.Default(check)  # noqa: F821
    env.Depends("upload", check)  # noqa: F821
print(f"version: {VERSION} ({COMMIT[:7] or 'no commit'}, {DATE or 'no date'})" + (", release" if RELEASE else ""))
