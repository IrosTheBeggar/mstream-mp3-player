# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""PlatformIO post-script: check the flash layout after every firmware build.

The partition table can never change after release (an OTA update rewrites an
app slot, not the table), and a single-file install at 0x0 must keep the
settings. So once firmware.bin and pioarduino's merged firmware.factory.bin are
built, this fails the build (and any upload) if:

- firmware.factory.bin is missing or older than firmware.bin (pioarduino's
  merge step only prints a message when esptool fails), or doesn't hold this
  build's partition table and app where the table says;
- the table isn't the one the firmware expects: otadata where the merged image
  writes boot_app0.bin, ota_0 where it writes the app, ota_1 the same size, no
  factory app, the "nvs" and "spiffs" labels (Preferences, LittleFS.begin(),
  uploadfs), a coredump partition, nothing overlapping or past the flash's end;
- nvs isn't above everything a merged image reaches (its own length, and an
  app as big as ota_0);
- the app fills more than FAIL_AT of its slot (a warning from WARN_AT): the
  slots can't grow later, so a release needs room for the next one;
- a piece of the merged image (bootloader, table, boot_app0, app) isn't the
  file a release also ships on its own.

Once the check passes it writes firmware.parts.json next to the image: each
piece's offset and file, which tools/package_release.py packages.

The table is read from the build's partitions.bin (made from partitions.csv),
not written down here a second time; this file only names what the firmware
relies on. The check runs on every build, even with nothing rebuilt, and it
makes firmware.bin (whose post-action is the merge) depend on the merged
pieces, so a table-only change remerges the image.
"""
import json
import os
import struct
import sys

from SCons.Script import COMMAND_LINE_TARGETS  # pylint: disable=import-error

Import("env")  # noqa: F821  (provided by PlatformIO)

WARN_AT = 0.80
FAIL_AT = 0.90

# ESP-IDF's partition types and subtypes (esp_partition.h).
APP, DATA = 0x00, 0x01
APP_FACTORY, APP_OTA_0, APP_OTA_1 = 0x00, 0x10, 0x11
DATA_OTA, DATA_NVS, DATA_COREDUMP, DATA_SPIFFS = 0x00, 0x02, 0x03, 0x82

ENTRY = struct.Struct("<HBBII16sI")  # magic, type, subtype, offset, size, label, flags
ENTRY_MAGIC = 0x50AA
MD5_MAGIC = 0xEBEB
TABLE_MAX = 0xC00  # the table's sector (3 KB of entries)


def parse_table(blob):
    """Partition entries from a binary table: [{label, type, subtype, offset, size}]."""
    parts = []
    for pos in range(0, min(len(blob), TABLE_MAX) - ENTRY.size + 1, ENTRY.size):
        magic, ptype, subtype, offset, size, label, _flags = ENTRY.unpack_from(blob, pos)
        if magic == MD5_MAGIC:
            continue
        if magic != ENTRY_MAGIC:
            break  # 0xFFFF: the end
        parts.append({
            "label": label.rstrip(b"\0").decode("ascii", "replace"),
            "type": ptype,
            "subtype": subtype,
            "offset": offset,
            "size": size,
        })
    return parts


def mb(n):
    return f"{n / (1024 * 1024):.2f} MB"


def check(env, fw_path, factory_path, table_path):
    """Returns (errors, warnings, summary)."""
    errors, warnings = [], []

    # --- the merged image exists and is this build's ---
    if not os.path.isfile(factory_path):
        return [f"{os.path.basename(factory_path)} is missing: pioarduino's merge step failed "
                "(see 'esptool merge-bin' above)"], [], ""
    if os.path.getmtime(factory_path) + 1 < os.path.getmtime(fw_path):
        errors.append(f"{os.path.basename(factory_path)} is older than {os.path.basename(fw_path)}: "
                      "the merge step failed and left the last build's image")
    with open(fw_path, "rb") as f:
        fw = f.read()
    with open(factory_path, "rb") as f:
        factory = f.read()
    with open(table_path, "rb") as f:
        table_bin = f.read()

    # Where the merge put each extra image (bootloader, table, boot_app0).
    extras = [(int(str(off), 0), env.subst(path)) for off, path in env.get("FLASH_EXTRA_IMAGES", [])]
    table_offset = next((off for off, p in extras if os.path.basename(p) == "partitions.bin"), None)
    boot_app0 = next(((off, p) for off, p in extras if os.path.basename(p) == "boot_app0.bin"), None)
    app_offset = int(env.subst("$ESP32_APP_OFFSET") or "0x10000", 0)

    parts = parse_table(table_bin)
    if not parts:
        return [f"{table_path} holds no partition entries"], [], ""
    if table_offset is None:
        errors.append("the merged image has no partitions.bin (FLASH_EXTRA_IMAGES)")
    elif factory[table_offset:table_offset + len(table_bin)] != table_bin:
        errors.append(f"the merged image's table at {table_offset:#x} isn't this build's partitions.bin")
    if factory[app_offset:app_offset + len(fw)] != fw:
        errors.append(f"the merged image's app at {app_offset:#x} isn't this build's firmware.bin")
    for off, p in extras:
        with open(p, "rb") as f:
            piece = f.read()
        if factory[off:off + len(piece)] != piece:
            errors.append(f"the merged image at {off:#x} isn't {os.path.basename(p)} (the pieces a release "
                          "also ships separately)")

    # --- the table is the one the firmware expects ---
    def find(ptype, subtype, label=None):
        found = [p for p in parts if p["type"] == ptype and p["subtype"] == subtype
                 and (label is None or p["label"] == label)]
        return found[0] if found else None

    otadata = find(DATA, DATA_OTA)
    ota_0 = find(APP, APP_OTA_0)
    ota_1 = find(APP, APP_OTA_1)
    nvs = find(DATA, DATA_NVS, "nvs")
    fs = find(DATA, DATA_SPIFFS, "spiffs")
    for part, what in ((otadata, "otadata (data, ota)"), (ota_0, "ota_0 (app)"), (ota_1, "ota_1 (app)"),
                       (nvs, 'nvs (data, nvs, label "nvs": Preferences)'),
                       (fs, 'spiffs (data, spiffs, label "spiffs": LittleFS.begin(), uploadfs)'),
                       (find(DATA, DATA_COREDUMP), "coredump (data, coredump)")):
        if part is None:
            errors.append(f"no {what} partition")
    if find(APP, APP_FACTORY):
        errors.append("a factory app partition: the bootloader would boot it instead of the OTA slots")

    flash_size = int(env.BoardConfig().get("upload.flash_size", "4MB").rstrip("MB")) * 1024 * 1024
    ordered = sorted(parts, key=lambda p: p["offset"])
    for a, b in zip(ordered, ordered[1:]):
        if a["offset"] + a["size"] > b["offset"]:
            errors.append(f"{a['label']} overlaps {b['label']}")
    last = ordered[-1]
    if last["offset"] + last["size"] > flash_size:
        errors.append(f"{last['label']} ends past the {mb(flash_size)} flash")

    if ota_0 and ota_0["offset"] != app_offset:
        errors.append(f"ota_0 is at {ota_0['offset']:#x} but the merged image and upload write the app at "
                      f"{app_offset:#x}")
    if ota_0 and ota_1 and ota_0["size"] != ota_1["size"]:
        errors.append("ota_0 and ota_1 differ in size: an update must fit either slot")
    if otadata:
        if otadata["size"] != 0x2000:
            errors.append(f"otadata is {otadata['size']:#x} bytes, not 0x2000")
        if boot_app0 is None:
            errors.append("the merged image has no boot_app0.bin (FLASH_EXTRA_IMAGES)")
        elif boot_app0[0] != otadata["offset"]:
            errors.append(f"boot_app0.bin lands at {boot_app0[0]:#x} but otadata is at {otadata['offset']:#x}")

    # --- nvs is above anything a merged image reaches ---
    image_end = len(factory)
    if nvs:
        reach = [("firmware.factory.bin", image_end)]
        if ota_0:
            reach.append(("an app filling ota_0", ota_0["offset"] + ota_0["size"]))
        for off, p in extras:
            if os.path.isfile(p):
                reach.append((os.path.basename(p), off + os.path.getsize(p)))
        for what, end in reach:
            if end > nvs["offset"]:
                errors.append(f"{what} reaches {end:#x}, over nvs at {nvs['offset']:#x}: a single-file install "
                              "at 0x0 would wipe the settings")

    # --- the app fits its slot with room to spare ---
    summary = ""
    slots = [p["size"] for p in (ota_0, ota_1) if p]
    if slots:
        slot = min(slots)
        used = len(fw) / slot
        summary = (f"app {mb(len(fw))} = {used:.0%} of the {mb(slot)} slot; firmware.factory.bin ends at "
                   f"{image_end:#x}" + (f", nvs at {nvs['offset']:#x}" if nvs else ""))
        if used > FAIL_AT:
            errors.append(f"the app fills {used:.0%} of its slot (limit {FAIL_AT:.0%}): the slots can never grow")
        elif used > WARN_AT:
            warnings.append(f"the app fills {used:.0%} of its slot (fails over {FAIL_AT:.0%})")
    return errors, warnings, summary


def flash_guard(source, target, env):
    build_dir = env.subst("$BUILD_DIR")
    fw_path = env.subst("$BUILD_DIR/${PROGNAME}.bin")
    factory_path = env.subst("$BUILD_DIR/${PROGNAME}.factory.bin")
    table_path = os.path.join(build_dir, "partitions.bin")
    errors, warnings, summary = check(env, fw_path, factory_path, table_path)
    for w in warnings:
        print(f"flash_guard: WARNING: {w}")
    if errors:
        for e in errors:
            sys.stderr.write(f"flash_guard: ERROR: {e}\n")
        sys.stderr.write("flash_guard: the flash layout check failed (partitions.csv, tools/flash_guard.py)\n")
        return 1
    write_parts(env, fw_path, factory_path)
    print(f"flash_guard: ok: {summary}")
    return 0


def write_parts(env, fw_path, factory_path):
    """$BUILD_DIR/${PROGNAME}.parts.json: where each piece of the merged image
    goes, for tools/package_release.py (boot_app0.bin lives in the framework,
    not the build directory). Written only once the pieces are checked."""
    parts = [{"offset": f"{int(str(off), 0):#x}", "path": os.path.abspath(env.subst(p))}
             for off, p in env.get("FLASH_EXTRA_IMAGES", [])]
    parts.append({"offset": f"{int(env.subst('$ESP32_APP_OFFSET') or '0x10000', 0):#x}",
                  "path": os.path.abspath(fw_path)})
    text = json.dumps({"merged": os.path.abspath(factory_path),
                       "parts": sorted(parts, key=lambda p: int(p["offset"], 16))}, indent=2) + "\n"
    out = os.path.splitext(fw_path)[0] + ".parts.json"
    try:
        with open(out, encoding="utf-8") as f:
            if f.read() == text:
                return
    except OSError:
        pass
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


FS_TARGETS = {"buildfs", "uploadfs", "uploadfsota", "nobuild"}
if not FS_TARGETS & set(COMMAND_LINE_TARGETS):
    firmware = "$BUILD_DIR/${PROGNAME}.bin"
    # pioarduino merges firmware.factory.bin in a post-action of firmware.bin,
    # which is only rebuilt when the app's code changes: after a table-only
    # change the merged image kept the old table. Rebuilding firmware.bin
    # when any merged piece changes (bootloader, table, boot_app0) reruns it.
    env.Depends(firmware, [env.subst(path) for _off, path in env.get("FLASH_EXTRA_IMAGES", [])])  # noqa: F821
    # The check itself runs on every build and before every upload (even with
    # nothing rebuilt), after firmware.bin and so after the merge.
    guard_action = env.Action(flash_guard)  # noqa: F821
    guard_action.strfunction = lambda target, source, env: ""
    guard = env.AlwaysBuild(env.Alias("flash_guard", firmware, guard_action))  # noqa: F821
    env.Default(guard)  # noqa: F821
    env.Depends("upload", guard)  # noqa: F821
