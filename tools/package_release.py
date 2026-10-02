# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Package a core2 build for release: the release files, the install page and
the release notes. CI runs it after `pio run -e core2 -e core2-dio`; it runs
the same way locally (Python 3.8+, standard library only, and git):

    python tools/package_release.py                  # .pio/build/core2 + core2-dio -> dist/
    python tools/package_release.py --release --tag v0.5.0

Two builds of the same source: core2 (the flash in QIO at 80 MHz, the
firmware everything installs) and core2-dio (DIO at 40 MHz, as M5 ships the
Core2: the fallback for a unit that keeps restarting on QIO, released as
NAME-dio-full.bin).

It reads each build's own outputs: the version tools/version.py wrote
(generated/PlayerVersion.h) and the pieces and flash mode
tools/flash_guard.py checked and listed (firmware.parts.json). Before
writing anything it checks each is one build: the app description in
firmware.bin holds that version and firmware.elf's SHA-256, and the merged
image holds every piece. Then that the two are the same source (version,
tag, commit, release flag) in the flash modes their names say, and that the
fallback's git-pinned libraries were the same commits. With --release the
build must be a release build (RELEASE=1, every check in tools/version.py
passed: PLAYER_RELEASE), still the checkout's clean HEAD, and --tag's
version, if given.

The source tarball is what the binary was built from, kept beside it (GPLv3
section 6, LGPL-2.1 section 6): this repository's files (git ls-files: the
tracked ones and any untracked ones that aren't ignored), the git-pinned
lib_deps checkouts in .pio/libdeps/core2 (ESP8266Audio: GPL; ESP32-A2DP),
each checked against its platformio.ini pin, and the parts of the Arduino
core (LGPL) the build compiled, found from the build's .d files.

dist/release/  the files a GitHub Release carries (NAME = mstream-player-core2-<version>):
  NAME-full.bin       firmware.factory.bin, written at 0x0 (install or update)
  NAME-dio-full.bin   core2-dio's firmware.factory.bin, the same at 0x0: the
                      same firmware with the flash in DIO
  NAME-app.bin        firmware.bin, at 0x10000
  NAME-parts.zip      bootloader 0x1000, partitions 0x8000, boot_app0 0xe000,
                      firmware 0x10000, and flash_args.txt for
                      `esptool --chip esp32 write-flash @flash_args.txt`
  NAME-elf.zip        firmware.elf and firmware.map (decoding a backtrace)
  NAME-dio-elf.zip    the same for NAME-dio-full.bin (its own ELF)
  NAME-licenses.zip   LICENSE, THIRD-PARTY-NOTICES.md and LICENSES/
  NAME-source.tar.gz  the source (below): mstream-mp3-player/ (this
                      repository), lib_deps/<name>/, framework-arduinoespressif32/
  LICENSE, THIRD-PARTY-NOTICES.md
  SHA256SUMS          `sha256sum -c SHA256SUMS` checks every file above
dist/site/     the install page (GitHub Pages): site/index.html filled in,
               manifest.json for ESP Web Tools, firmware/<version>/NAME-full.bin
               (the QIO image only; same origin: release files have no CORS
               headers)
dist/release-notes.md  .github/release-notes.md filled in

The filesystem image (littlefs.bin, data/music: test audio, maybe excerpts
of someone's music) is never packaged. The zips and the tarball carry the
commit's time, so packaging the same build again gives the same bytes.

With $GITHUB_OUTPUT set (in Actions) it also writes the step outputs:
version, file_version, prerelease, title, artifact.
"""
import argparse
import calendar
import gzip
import hashlib
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = "IrosTheBeggar/mstream-mp3-player"
NAME = "mstream-player-core2"
MANIFEST_NAME = "mStream Player for M5Stack Core2"

# The same SemVer as tools/version.py (which refuses a release tag without it).
SEMVER = re.compile(r"^v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)"
                    r"(?:-((?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*)(?:\.(?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*))*))?"
                    r"(?:\+([0-9a-zA-Z-]+(?:\.[0-9a-zA-Z-]+)*))?$")
APP_DESC_MAGIC = 0xABCD5432
DESC_VERSION_MAX = 31
# What goes in the -parts.zip, by offset: the names flash_args.txt uses.
PART_NAMES = {0x1000: "bootloader.bin", 0x8000: "partitions.bin", 0xE000: "boot_app0.bin", 0x10000: "firmware.bin"}
# The flash mode of each build (platformio.ini; flash_guard records it in
# firmware.parts.json): the main image QIO, the fallback DIO.
MAIN_FLASH, FALLBACK_FLASH = "qio", "dio"
# A log string only a QIO bootloader links (tools/flash_guard.py's QIO_MARKER).
QIO_MARKER = b"not enabling QIO mode"
# What two builds of the same source share (read_version).
VERSION_KEYS = ("PLAYER_VERSION", "PLAYER_TAG", "PLAYER_RELEASE", "PLAYER_COMMIT", "PLAYER_COMMIT_DATE",
                "PLAYER_COMMIT_TIME")


class PackageError(Exception):
    pass


def env_name(build_dir):
    """The PlatformIO env a build directory belongs to (.pio/build/<env>)."""
    return os.path.basename(os.path.normpath(build_dir))


def read_version(build_dir):
    """PLAYER_VERSION, PLAYER_TAG, PLAYER_COMMIT and the commit's date and
    time from the header tools/version.py generated for this build."""
    path = os.path.join(build_dir, "generated", "PlayerVersion.h")
    try:
        with open(path, encoding="utf-8") as f:
            text = f.read()
    except OSError:
        raise PackageError(f"{path} is missing: build first (pio run -e {env_name(build_dir)})")
    defs = dict(re.findall(r'^#define (PLAYER_\w+) "((?:[^"\\]|\\.)*)"', text, re.M))
    defs.update(re.findall(r'^#define (PLAYER_RELEASE) ([01])\b', text, re.M))
    for key in VERSION_KEYS:
        if key not in defs:
            raise PackageError(f"{path} has no {key}: rebuild (pio run -e {env_name(build_dir)})")
    return defs


def git(*args):
    """git's output in this repository, stripped, or None."""
    try:
        out = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    return out.stdout.decode("utf-8", "replace").strip()


def check_build(build_dir, version, flash_mode):
    """The merged image, the parts list and the ELF's first 8 hex digits,
    after checking the build's files belong together and its flash mode is
    flash_mode."""
    env = env_name(build_dir)
    fw = os.path.join(build_dir, "firmware.bin")
    elf = os.path.join(build_dir, "firmware.elf")
    parts_path = os.path.join(build_dir, "firmware.parts.json")
    for p in (fw, elf, parts_path, os.path.join(build_dir, "firmware.map")):
        if not os.path.isfile(p):
            raise PackageError(f"{p} is missing: build first (pio run -e {env}; flash_guard writes the parts list)")
    with open(parts_path, encoding="utf-8") as f:
        layout = json.load(f)
    if "flash_mode" not in layout:
        raise PackageError(f"{parts_path} has no flash mode (an older flash_guard): rebuild (pio run -e {env})")
    if layout["flash_mode"] != flash_mode:
        raise PackageError(f"{env} was built with the flash in {layout['flash_mode'].upper()}, not "
                           f"{flash_mode.upper()} (platformio.ini)")

    with open(fw, "rb") as f:
        desc = f.read(0x20 + 256)[0x20:]
    with open(elf, "rb") as f:
        elf_sha = hashlib.sha256(f.read()).digest()
    in_image = desc[16:48].split(b"\0")[0].decode("ascii", "replace")
    if struct.unpack_from("<I", desc, 0)[0] != APP_DESC_MAGIC:
        raise PackageError("firmware.bin has no app description")
    if in_image != version[:DESC_VERSION_MAX]:
        raise PackageError(f"firmware.bin says {in_image!r} but the build's header says {version!r}: rebuild")
    if desc[144:176] != elf_sha:
        raise PackageError("firmware.bin's ELF SHA-256 isn't firmware.elf's: rebuild")

    with open(layout["merged"], "rb") as f:
        merged = f.read()
    parts = []
    for part in layout["parts"]:
        offset = int(part["offset"], 16)
        name = PART_NAMES.get(offset)
        if name is None:
            raise PackageError(f"an unexpected piece at {offset:#x} ({part['path']}): update PART_NAMES")
        with open(part["path"], "rb") as f:
            data = f.read()
        if merged[offset:offset + len(data)] != data:
            raise PackageError(f"the merged image at {offset:#x} isn't {part['path']}: rebuild")
        parts.append((offset, name, data))
    if sorted(o for o, _n, _d in parts) != sorted(PART_NAMES):
        raise PackageError(f"the parts list has {[hex(o) for o, _n, _d in parts]}, not {list(map(hex, PART_NAMES))}")
    bootloader = next(data for o, _n, data in parts if o == 0x1000)
    if (QIO_MARKER in bootloader) != (flash_mode == "qio"):
        raise PackageError(f"{env}'s bootloader {'has' if QIO_MARKER in bootloader else 'lacks'} the QIO code, "
                           f"for a {flash_mode.upper()} build: rebuild")
    return layout["merged"], parts, elf_sha.hex()[:8]


def zip_files(path, members, date, time):
    """A zip with fixed timestamps (the commit's) and mode, so the same build
    zips to the same bytes. members: [(name in the zip, bytes)]."""
    stamp = tuple(int(x) for x in date.split("-")) + tuple(int(x) for x in time.split(":"))
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, data in members:
            info = zipfile.ZipInfo(name, stamp)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, data, compresslevel=9)


def read(path):
    with open(path, "rb") as f:
        return f.read()


def licence_files():
    """[(relative path, bytes)]: LICENSE, THIRD-PARTY-NOTICES.md, LICENSES/*."""
    files = [("LICENSE", read(os.path.join(ROOT, "LICENSE"))),
             ("THIRD-PARTY-NOTICES.md", read(os.path.join(ROOT, "THIRD-PARTY-NOTICES.md")))]
    lic_dir = os.path.join(ROOT, "LICENSES")
    for name in sorted(os.listdir(lic_dir)):
        if os.path.isfile(os.path.join(lic_dir, name)):
            files.append((f"LICENSES/{name}", read(os.path.join(lic_dir, name))))
    return files


def tree_files(top):
    """[(path relative to top with "/", absolute path)]: every file under top,
    sorted, without .git (a directory, or a submodule's file)."""
    found = []
    for dirpath, dirs, files in os.walk(top):
        dirs[:] = sorted(d for d in dirs if d != ".git" and not os.path.islink(os.path.join(dirpath, d)))
        for n in sorted(files):
            p = os.path.join(dirpath, n)
            if n != ".git" and not os.path.islink(p):
                found.append((os.path.relpath(p, top).replace(os.sep, "/"), p))
    return found


def pinned_libdeps():
    """{checkout folder: commit} for every git lib_deps entry pinned by commit
    in platformio.ini (https://.../NAME.git#<40 hex>)."""
    with open(os.path.join(ROOT, "platformio.ini"), encoding="utf-8") as f:
        text = f.read()
    return {m.group(1): m.group(2).lower()
            for m in re.finditer(r"^\s*\S+/([^/\s#]+?)(?:\.git)?#([0-9a-fA-F]{40})\s*$", text, re.M)}


def git_head(checkout):
    """The commit a checkout's .git/HEAD names (detached, as PlatformIO leaves
    a pinned clone), without running git."""
    try:
        with open(os.path.join(checkout, ".git", "HEAD"), encoding="ascii") as f:
            head = f.read().strip()
    except OSError:
        return None
    if head.startswith("ref: "):
        try:
            with open(os.path.join(checkout, ".git", *head[5:].split("/")), encoding="ascii") as f:
                head = f.read().strip()
        except OSError:
            return None
    return head.lower()


def checked_libdeps(build_dir):
    """[(name, pin, checkout folder)]: the git-pinned lib_deps the build in
    build_dir used (.pio/libdeps/<env>), each checked at its pin."""
    env = env_name(build_dir)
    libdeps = os.path.join(ROOT, ".pio", "libdeps", env)
    found = []
    for name, pin in sorted(pinned_libdeps().items()):
        checkout = os.path.join(libdeps, name)
        head = git_head(checkout)
        if head != pin:
            raise PackageError(f"{checkout} is at {head or 'no git checkout'}, not platformio.ini's {pin}: "
                               f"rebuild (pio run -e {env} fetches the pinned commit)")
        found.append((name, pin, checkout))
    return found


def depfile_paths(build_dir):
    """Every file path the build's .d files list (its sources and headers)."""
    paths = set()
    for dirpath, _dirs, files in os.walk(build_dir):
        for n in files:
            if n.endswith(".d"):
                with open(os.path.join(dirpath, n), encoding="utf-8", errors="replace") as f:
                    text = f.read().replace("\\\n", " ")  # make's line continuations
                for tok in re.split(r"(?<!\\)\s+", text):  # "\ " is a space in a path
                    paths.add(tok.rstrip(":").replace("\\ ", " ").replace("\\", "/"))
    return paths


def source_files(build_dir, release, commit):
    """[(name in the tarball, absolute path)] and a description of what is in
    it (the tarball's SOURCES.txt)."""
    if release:
        if git("rev-parse", "HEAD") != commit:
            raise PackageError(f"--release: the checkout is no longer at the build's commit {commit[:7]}: rebuild")
        status = git("status", "--porcelain", "--untracked-files=normal")
        if status is None or status:
            raise PackageError("--release: the tree changed since the release build (git status: " +
                               "; ".join((status or "no git").splitlines()[:5]) + ")")
    # Tracked and untracked files, not ignored ones (.pio, dist, local.ini):
    # what a build compiles. For a release, exactly the tag's files.
    listed = git("ls-files", "-z", "--cached", "--others", "--exclude-standard")
    if listed is None:
        raise PackageError("git ls-files failed: the source tarball lists the repository's files with git")
    top = REPO.partition("/")[2]
    members = []
    for rel in sorted(set(n for n in listed.split("\0") if n)):
        p = os.path.join(ROOT, *rel.split("/"))
        if os.path.isfile(p) and not os.path.islink(p):
            members.append((f"{top}/{rel}", p))
    about = [f"{top}/: this repository's files at commit {commit or 'unknown'}" +
             ("" if release else " (as built: the working tree, uncommitted and untracked files included)")]

    for name, pin, checkout in checked_libdeps(build_dir):
        members += [(f"lib_deps/{name}/{rel}", p) for rel, p in tree_files(checkout)]
        about.append(f"lib_deps/{name}/: the checkout the build used, commit {pin} (platformio.ini)")

    # The Arduino core's parts the build compiled, located through the .d
    # files (its framework folder, whatever PLATFORMIO_CORE_DIR is): each
    # one whole (cores/esp32, the board's variant, each library compiled).
    units, framework = set(), None
    for p in depfile_paths(build_dir):
        m = re.match(r"^(.*/framework-arduinoespressif32)/(cores/[^/]+|variants/[^/]+|libraries/[^/]+)/", p)
        if m:
            framework = framework or m.group(1)
            units.add(m.group(2))
    if not framework or not os.path.isdir(framework):
        raise PackageError("the build's .d files name no framework-arduinoespressif32 sources: rebuild")
    with open(os.path.join(framework, "package.json"), encoding="utf-8") as f:
        fw_version = json.load(f).get("version", "unknown")
    members.append(("framework-arduinoespressif32/package.json", os.path.join(framework, "package.json")))
    for unit in sorted(units):
        members += [(f"framework-arduinoespressif32/{unit}/{rel}", p)
                    for rel, p in tree_files(os.path.join(framework, unit))]
    about.append(f"framework-arduinoespressif32/: Arduino-ESP32 {fw_version}, the parts the build compiled: " +
                 ", ".join(sorted(units)))
    return members, about


def tar_gz(path, members, about, mtime):
    """A gzipped tar with fixed metadata (the commit's time, mode 644, owner
    0, gzip time 0), sorted, so the same sources give the same bytes.
    members: [(name in the tarball, absolute path)]; about: SOURCES.txt."""
    readme = ("The source of the firmware this file was released with "
              "(tools/package_release.py).\n\n" + "".join(f"- {a}\n" for a in about)).encode()
    def info(name, size):
        ti = tarfile.TarInfo(name)
        ti.size, ti.mtime, ti.mode = size, mtime, 0o644
        ti.uid = ti.gid = 0
        ti.uname = ti.gname = ""
        return ti
    with open(path, "wb") as raw, \
            gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0, compresslevel=9) as gz, \
            tarfile.open(fileobj=gz, mode="w", format=tarfile.GNU_FORMAT) as tar:
        tar.addfile(info("SOURCES.txt", len(readme)), io.BytesIO(readme))
        for name, src in sorted(members):
            with open(src, "rb") as f:
                tar.addfile(info(name, os.fstat(f.fileno()).st_size), f)


def fill(template, values, what):
    out = template
    for key, value in values.items():
        out = out.replace("{{" + key + "}}", value)
    left = sorted(set(re.findall(r"\{\{(\w+)\}\}", out)))
    if left:
        raise PackageError(f"{what} has unknown placeholders: {', '.join(left)}")
    return out


def html_escape(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


def check_fallback(dio_dir, v, version):
    """core2-dio's merged image and ELF digits, after checking it is the same
    source as the main build (v) and a DIO build that belongs together."""
    dv = read_version(dio_dir)
    differ = [k for k in VERSION_KEYS if dv[k] != v[k]]
    if differ:
        raise PackageError(f"{env_name(dio_dir)} isn't the same source as the main build ({', '.join(differ)}: "
                           f"{dv['PLAYER_VERSION']!r} against {version!r}): build both from this checkout "
                           "(pio run -e core2 -e core2-dio)")
    merged_path, _parts, elf8 = check_build(dio_dir, version, FALLBACK_FLASH)
    checked_libdeps(dio_dir)
    return merged_path, elf8


def package(build_dir, dio_dir, out_dir, release, tag, repo, web_installer=False):
    v = read_version(build_dir)
    version, commit = v["PLAYER_VERSION"], v["PLAYER_COMMIT"]
    semver = SEMVER.match(version)
    # Built from exactly a tag with nothing changed, with RELEASE=1 and its
    # checks passed (tools/version.py): the tag is then this build's source
    # and its release's name. A plain build of a tag checked out with a
    # local.ini (gitignored, so still "clean") is not one.
    tagged = bool(semver) and v["PLAYER_TAG"] == version
    is_release = tagged and v["PLAYER_RELEASE"] == "1"
    if release:
        if not tagged:
            raise PackageError(f"--release: the build is {version!r}, not exactly a SemVer tag (RELEASE=1 builds one)")
        if not is_release:
            raise PackageError(f"--release: {version} wasn't built with RELEASE=1, so its release checks "
                               "(local flags, BT_SINK_NAME) never ran: rebuild with RELEASE=1")
        if tag and tag != version:
            raise PackageError(f"--release: the build is {version!r} but the tag is {tag!r}")
    merged_path, parts, elf8 = check_build(build_dir, version, MAIN_FLASH)
    dio_merged, dio_elf8 = check_fallback(dio_dir, v, version)
    sources, sources_about = source_files(build_dir, release, commit)

    file_version = re.sub(r"[^0-9A-Za-z._-]", "_", version)  # "+" (dev builds) out of names and URLs
    base = f"{NAME}-{file_version}"
    # v0.5.0-rc.1 (a SemVer pre-release) and every untagged build: pre-release.
    prerelease = not is_release or semver.group(4) is not None
    channel = "pre-release" if prerelease else "beta" if semver.group(1) == "0" else "release"
    repo_url = f"https://github.com/{repo}"
    owner, _, name = repo.partition("/")
    # A release's source is its tag; any other build's, its commit (a -dirty
    # build had changes on top that only its builder has).
    ref = version if is_release else commit or "HEAD"
    values = {
        "VERSION": version,
        "CHANNEL": channel,
        "COMMIT": commit[:7] or "unknown",
        "DATE": v["PLAYER_COMMIT_DATE"] or "unknown",
        "ELF": elf8,
        "DIO_ELF": dio_elf8,
        "REPO_URL": repo_url,
        "SOURCE_URL": f"{repo_url}/tree/{ref}",
        "LICENSE_URL": f"{repo_url}/blob/{ref}/LICENSE",
        "NOTICES_URL": f"{repo_url}/blob/{ref}/THIRD-PARTY-NOTICES.md",
        "RELEASE_URL": f"{repo_url}/releases/tag/{version}" if is_release else f"{repo_url}/releases",
        "INSTALL_URL": f"https://{owner.lower()}.github.io/{name}/",
        "FULL_BIN": f"{base}-full.bin",
        "DIO_FULL_BIN": f"{base}-dio-full.bin",
        "APP_BIN": f"{base}-app.bin",
        "PARTS_ZIP": f"{base}-parts.zip",
        "ELF_ZIP": f"{base}-elf.zip",
        "DIO_ELF_ZIP": f"{base}-dio-elf.zip",
        "LICENSES_ZIP": f"{base}-licenses.zip",
        "SOURCE_TAR": f"{base}-source.tar.gz",
    }
    # The web installer (GitHub Pages) is off unless the repository variable
    # WEB_INSTALLER is "true" (the workflow passes it on): installing is
    # planned in mstream-terminal instead.
    if web_installer and not prerelease:
        values["INSTALL_LINE"] = f"In Chrome or Edge on a computer: [the web installer]({values['INSTALL_URL']})."
    else:
        values["INSTALL_LINE"] = "Flash it with esptool (below)."

    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)
    rel_dir = os.path.join(out_dir, "release")
    site_dir = os.path.join(out_dir, "site")
    os.makedirs(rel_dir)
    date, time = v["PLAYER_COMMIT_DATE"] or "1980-01-01", v["PLAYER_COMMIT_TIME"] or "00:00:00"

    # --- the release files ---
    shutil.copyfile(merged_path, os.path.join(rel_dir, values["FULL_BIN"]))
    shutil.copyfile(dio_merged, os.path.join(rel_dir, values["DIO_FULL_BIN"]))
    app = next(data for off, _n, data in parts if off == 0x10000)
    with open(os.path.join(rel_dir, values["APP_BIN"]), "wb") as f:
        f.write(app)
    flash_args = "".join(f"{off:#x} {n}\n" for off, n, _d in sorted(parts))
    zip_files(os.path.join(rel_dir, values["PARTS_ZIP"]),
              [(n, data) for _o, n, data in sorted(parts)] + [("flash_args.txt", flash_args.encode())], date, time)
    zip_files(os.path.join(rel_dir, values["ELF_ZIP"]),
              [("firmware.elf", read(os.path.join(build_dir, "firmware.elf"))),
               ("firmware.map", read(os.path.join(build_dir, "firmware.map")))], date, time)
    zip_files(os.path.join(rel_dir, values["DIO_ELF_ZIP"]),
              [("firmware.elf", read(os.path.join(dio_dir, "firmware.elf"))),
               ("firmware.map", read(os.path.join(dio_dir, "firmware.map")))], date, time)
    licences = licence_files()
    zip_files(os.path.join(rel_dir, values["LICENSES_ZIP"]), licences, date, time)
    for rel, data in licences[:2]:  # LICENSE, THIRD-PARTY-NOTICES.md on their own too
        with open(os.path.join(rel_dir, rel), "wb") as f:
            f.write(data)
    commit_time = calendar.timegm(tuple(int(x) for x in date.split("-") + time.split(":")) + (0, 0, 0))
    tar_gz(os.path.join(rel_dir, values["SOURCE_TAR"]), sources, sources_about, commit_time)
    sums = "".join(f"{hashlib.sha256(read(os.path.join(rel_dir, n))).hexdigest()}  {n}\n"
                   for n in sorted(os.listdir(rel_dir)))
    with open(os.path.join(rel_dir, "SHA256SUMS"), "w", encoding="utf-8", newline="\n") as f:
        f.write(sums)

    # --- the install page: the main (QIO) image only ---
    fw_rel = f"firmware/{file_version}/{values['FULL_BIN']}"
    os.makedirs(os.path.join(site_dir, os.path.dirname(fw_rel)))
    shutil.copyfile(merged_path, os.path.join(site_dir, fw_rel))
    manifest = {
        "name": MANIFEST_NAME,
        "version": version,
        # The dialog asks whether to erase: yes for a first install over
        # other firmware, no for an update (the settings sit above the image).
        "new_install_prompt_erase": True,
        # No Improv (there is no WiFi to provision): don't probe for it, so
        # its packets never reach the serial console.
        "new_install_improv_wait_time": 0,
        "builds": [{"chipFamily": "ESP32", "parts": [{"path": fw_rel, "offset": 0}]}],
    }
    with open(os.path.join(site_dir, "manifest.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    page_src = os.path.join(ROOT, "site")
    for name in sorted(os.listdir(page_src)):
        src = os.path.join(page_src, name)
        if not os.path.isfile(src):
            continue
        if name.endswith(".html"):
            with open(src, encoding="utf-8") as f:
                page = fill(f.read(), {k: html_escape(x) for k, x in values.items()}, f"site/{name}")
            with open(os.path.join(site_dir, name), "w", encoding="utf-8", newline="\n") as f:
                f.write(page)
        else:
            shutil.copyfile(src, os.path.join(site_dir, name))

    # --- the release notes ---
    with open(os.path.join(ROOT, ".github", "release-notes.md"), encoding="utf-8") as f:
        notes = fill(f.read(), values, ".github/release-notes.md")
    with open(os.path.join(out_dir, "release-notes.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write(notes)

    # Nothing from the filesystem image, whatever changes above.
    for dirpath, _dirs, files in os.walk(out_dir):
        for n in files:
            if re.search(r"littlefs|spiffs|\.(mp3|flac|wav)$", n, re.I):
                raise PackageError(f"{os.path.join(dirpath, n)}: the filesystem image is never packaged")

    title = f"mStream Player {version}" + ("" if channel == "release" else f" ({channel})")
    outputs = {"version": version, "file_version": file_version, "prerelease": "true" if prerelease else "false",
               "title": title, "artifact": base}
    if os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as f:
            for k, x in outputs.items():
                f.write(f"{k}={x}\n")
    return outputs, manifest


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--build-dir", default=os.path.join(ROOT, ".pio", "build", "core2"),
                    help="the main build (QIO): pio run -e core2")
    ap.add_argument("--dio-build-dir", default=os.path.join(ROOT, ".pio", "build", "core2-dio"),
                    help="the fallback build of the same source (DIO): pio run -e core2-dio")
    ap.add_argument("--out", default=os.path.join(ROOT, "dist"))
    ap.add_argument("--release", action="store_true", help="require a clean SemVer tagged build")
    ap.add_argument("--tag", help="with --release: the tag the build must be")
    ap.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY") or REPO, help="owner/name on GitHub")
    ap.add_argument("--web-installer", action="store_true",
                    default=os.environ.get("WEB_INSTALLER", "").lower() == "true",
                    help="point the release notes at the GitHub Pages installer (env WEB_INSTALLER=true)")
    args = ap.parse_args()
    try:
        outputs, manifest = package(args.build_dir, args.dio_build_dir, args.out, args.release, args.tag, args.repo,
                                    args.web_installer)
    except PackageError as e:
        sys.stderr.write(f"package_release: ERROR: {e}\n")
        return 1
    print(f"package_release: {outputs['title']} -> {args.out}")
    for dirpath, _dirs, files in sorted(os.walk(args.out)):
        for n in sorted(files):
            p = os.path.join(dirpath, n)
            print(f"  {os.path.relpath(p, args.out).replace(os.sep, '/'):<64} {os.path.getsize(p):>10,}")
    print("manifest.json:")
    print(json.dumps(manifest, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
