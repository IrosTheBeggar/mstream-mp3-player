# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Tests for tools/synthcard.py (the synthetic big card, docs/METADATA.md N11).

    python -m unittest discover -s tools -p "test_synthcard.py"

- The shape: the default plan's statistics against the measured library's.
- Determinism: one seed, one tree (the plan and the bytes).
- The stubs: a spread of every container variant written to a temporary
  folder and read back three ways: by this file's own parsers (ID3v2 frame
  by frame, FLAC blocks and every frame's CRCs, Ogg pages and their CRCs,
  OpusHead and OpusTags); by ffprobe when it is installed; and by the
  repo's parsers on the host (tools/synthcard_probe.cpp, built with g++
  when there is one): the firmware's MP3 length and LAME header, the FLAC
  STREAMINFO length, oggopus::Reader's open, and, when lib/core/TagScan
  (N6) builds, the tag record it reads, field by field.
- --copy-to's refusals and its copy, on a fake drive (no real drive is
  ever asked or written).
"""
import base64
import hashlib
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import synthcard as sc  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
CORE = ROOT / "lib" / "core"

_PLAN = None


def default_plan():
    global _PLAN
    if _PLAN is None:
        _PLAN = sc.make_plan()
    return _PLAN


# ---------------------------------------------------------------------------
# Readers of our own (a second look at the bytes, independent of the writers)
# ---------------------------------------------------------------------------


def unsync_undo(b):
    return b.replace(b"\xff\x00", b"\xff")


def ss(b):
    return (b[0] << 21) | (b[1] << 14) | (b[2] << 7) | b[3]


def decode_text(enc, raw):
    """An ID3 text frame's values (after the encoding byte)."""
    if enc == 0:
        parts = raw.split(b"\0")
        vals = [p.decode("latin-1") for p in parts]
    elif enc == 3:
        vals = [p.decode("utf-8") for p in raw.split(b"\0")]
    else:
        units = [raw[i:i + 2] for i in range(0, len(raw) - 1, 2)]
        vals, cur = [], bytearray()
        for u in units:
            if u == b"\0\0":
                vals.append(bytes(cur))
                cur = bytearray()
            else:
                cur += u
        vals.append(bytes(cur))
        out = []
        for v in vals:
            if v[:2] == b"\xff\xfe":
                out.append(v[2:].decode("utf-16-le"))
            elif v[:2] == b"\xfe\xff":
                out.append(v[2:].decode("utf-16-be"))
            else:
                out.append(v.decode("utf-16-be" if enc == 2 else "utf-16-le"))
        vals = out
    while vals and vals[-1] == "":
        vals.pop()
    return vals


def read_id3v2(d):
    """{frame id (v2.3 names): [values]} and the picture's (offset, stored length)."""
    assert d[:3] == b"ID3", "no ID3v2 tag"
    ver, flags = d[3], d[5]
    size = ss(d[6:10])
    end = 10 + size
    at = 10
    frames = {}
    pic = None
    names22 = {v: k for k, v in sc.Id3.V22.items()}
    while at < end:
        if ver == 2:
            if d[at] == 0:
                break
            fid = names22.get(d[at:at + 3].decode("latin-1"), d[at:at + 3].decode("latin-1"))
            n = int.from_bytes(d[at + 3:at + 6], "big")
            body_at, fflags = at + 6, 0
        else:
            if d[at] == 0:
                break
            fid = d[at:at + 4].decode("latin-1")
            n = ss(d[at + 4:at + 8]) if ver == 4 else int.from_bytes(d[at + 4:at + 8], "big")
            fflags = d[at + 9]
            body_at = at + 10
        body = d[body_at:body_at + n]
        stored_at = body_at
        if ver == 4 and fflags & 0x01:  # a data length indicator
            body = body[4:]
            stored_at += 4
        if ver == 4 and fflags & 0x02:
            body = unsync_undo(body)
        if fid == "APIC":
            if ver == 2:
                pre = 1 + 3 + 1 + body[5:].index(b"\0") + 1
            else:
                mime_end = body.index(b"\0", 1)
                pre = mime_end + 1 + 1 + body[mime_end + 2:].index(b"\0") + 1
            if pic is None:
                stored_len = n - (4 if (ver == 4 and fflags & 0x01) else 0) - pre
                pic = (stored_at + pre, stored_len, body[pre:])
        elif fid == "TXXX":
            enc = body[0]
            vals = decode_text(enc, body[1:])
            frames.setdefault("TXXX:" + vals[0], []).extend(vals[1:])
        elif fid.startswith("T"):
            frames.setdefault(fid, []).extend(decode_text(body[0], body[1:]))
        at = body_at + n
    return ver, flags, frames, pic, end


def mp3_frames(d, start, end):
    """Every Layer III header from start to end: (count, ok)."""
    rates = {0: 44100, 1: 48000, 2: 32000}
    kbps = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320]
    n, at = 0, start
    while at + 4 <= end:
        h = d[at:at + 4]
        if h[0] != 0xFF or (h[1] & 0xFE) != 0xFA:
            return n, False
        br = kbps[h[2] >> 4]
        rate = rates[(h[2] >> 2) & 3]
        pad = (h[2] >> 1) & 1
        at += 144 * br * 1000 // rate + pad
        n += 1
    return n, at == end


def read_flac(d):
    assert d[:4] == b"fLaC"
    at, blocks = 4, []
    while True:
        last, typ = d[at] & 0x80, d[at] & 0x7F
        n = int.from_bytes(d[at + 1:at + 4], "big")
        blocks.append((typ, at + 4, d[at + 4:at + 4 + n]))
        at += 4 + n
        if last:
            break
    si = blocks[0][2]
    v = int.from_bytes(si[10:18], "big")
    total = v & ((1 << 36) - 1)
    rate = v >> 44
    comments, pics = [], []
    for typ, off, body in blocks:
        if typ == 4:
            vl = struct.unpack("<I", body[:4])[0]
            p = 4 + vl
            cnt = struct.unpack("<I", body[p:p + 4])[0]
            p += 4
            for _ in range(cnt):
                ln = struct.unpack("<I", body[p:p + 4])[0]
                comments.append(body[p + 4:p + 4 + ln].decode("utf-8"))
                p += 4 + ln
        if typ == 6:
            ml = struct.unpack(">I", body[4:8])[0]
            dl = struct.unpack(">I", body[8 + ml:12 + ml])[0]
            q = 12 + ml + dl + 16
            n = struct.unpack(">I", body[q:q + 4])[0]
            pics.append((off + q + 4, n))
    # The frames: each header's CRC-8 and each frame's CRC-16, to the end.
    frames, ok = 0, True
    while at < len(d):
        if d[at:at + 2] != b"\xff\xf8":
            ok = False
            break
        hl = 4
        b = d[at + 4]
        hl += 1 if b < 0x80 else 2 if b < 0xE0 else 3
        if d[at + 2] >> 4 == 0x7:
            hl += 2
        if sc.crc8(d[at:at + hl]) != d[at + hl]:
            ok = False
            break
        fl = hl + 1 + 6 + 2
        if sc.crc16(d[at:at + fl - 2]) != int.from_bytes(d[at + fl - 2:at + fl], "big"):
            ok = False
            break
        frames += 1
        at += fl
    return {"total": total, "rate": rate, "comments": comments, "pictures": pics, "frames": frames,
            "frames_ok": ok and at == len(d)}


def read_ogg(d):
    at, pages, packets, cur = 0, [], [], bytearray()
    while at < len(d):
        assert d[at:at + 4] == b"OggS", f"no page at {at}"
        nseg = d[at + 26]
        lace = d[at + 27:at + 27 + nseg]
        hl = 27 + nseg
        bl = sum(lace)
        page = bytearray(d[at:at + hl + bl])
        crc = struct.unpack("<I", page[22:26])[0]
        page[22:26] = b"\0\0\0\0"
        granule = struct.unpack("<q", d[at + 6:at + 14])[0]
        pages.append({"at": at, "flags": d[at + 5], "granule": granule, "crc_ok": sc.ogg_crc(bytes(page)) == crc})
        p = at + hl
        for ln in lace:
            cur += d[p:p + ln]
            p += ln
            if ln < 255:
                packets.append(bytes(cur))
                cur = bytearray()
        at += hl + bl
    return pages, packets


def vorbis_list(raw):
    vl = struct.unpack("<I", raw[:4])[0]
    p = 4 + vl
    n = struct.unpack("<I", raw[p:p + 4])[0]
    p += 4
    out = []
    for _ in range(n):
        ln = struct.unpack("<I", raw[p:p + 4])[0]
        out.append(raw[p + 4:p + 4 + ln].decode("utf-8"))
        p += 4 + ln
    return out


def multimap(comments):
    m = {}
    for c in comments:
        k, _, v = c.partition("=")
        m.setdefault(k.upper(), []).append(v)
    return m


# ---------------------------------------------------------------------------


class ShapeTests(unittest.TestCase):
    """The default plan against the measured library (metascan; aggregates)."""

    @classmethod
    def setUpClass(cls):
        cls.plan = default_plan()
        cls.s = sc.summary(cls.plan)

    def near(self, got, want, tol, what):
        self.assertTrue(abs(got - want) <= tol, f"{what}: {got}, {want} +- {tol}")

    def test_counts(self):
        s, m = self.s, sc.MEASURED
        self.assertEqual(s["audio_files"], m["audio_files"])
        self.assertEqual(s["artists"], m["artists"])
        self.near(s["folders"], m["folders"], 30, "folders")
        self.near(s["audio_folders"], m["audio_folders"], 80, "folders with audio")
        self.near(s["by_kind"]["mp3"], m["mp3"], 400, "MP3 files")
        self.near(s["by_kind"]["flac"], m["flac"], 60, "FLAC files")
        self.assertGreater(s["by_kind"]["opus"], 100)
        for k, v in m["other_audio"].items():
            self.assertEqual(s["by_kind"][k], v, k)
        self.assertEqual(s["compilation_folders"], 2)

    def test_depths_and_albums(self):
        s, m = self.s, sc.MEASURED
        self.assertEqual(s["depth"].get(1), m["depth"][1])
        self.assertEqual(s["depth"].get(4), m["depth"][4])
        self.near(s["depth"].get(3, 0), m["depth"][3], 150, "depth 3")
        pf = s["per_folder"]
        self.near(pf["mean"], 10.9, 0.6, "tracks per folder, mean")
        self.near(pf["p50"], 11, 1, "p50")
        self.near(pf["p90"], 17, 2, "p90")
        self.near(pf["p99"], 28, 5, "p99")
        self.assertLessEqual(pf["max"], 43)
        self.near(pf["single"], 104, 30, "single-file folders")

    def test_name_shapes(self):
        s = self.s
        self.near(s["artist_in_name"], 24, 3, "the artist in the file name")
        self.near(s["no_number"], 8.9, 2.5, "no number")
        sh = s["name_shapes_counts"]
        self.near(sh.get("D-NN Title", 0), sc.MEASURED["disc_track_names"], 40, "1-01 names")
        self.near(sh.get("DNN - Title", 0), sc.MEASURED["three_digit_names"], 40, "101 names")
        self.assertGreater(sh.get("restart", 0), 300)
        for shape, _ in sc.NAME_SHAPES:
            self.assertGreater(sh.get(shape, 0), 0, shape)

    def test_disagreement_rates(self):
        s = self.s
        t = s["title_vs_name"]
        self.near(t["same"], 61, 4, "title same")
        self.near(t["inside"], 6, 1.5, "title inside the name")
        self.near(t["case"], 4, 1.5, "title case")
        a = s["album_folders_vs_tag"]
        self.near(a["same"], 38, 4, "album same")
        self.near(a["year"], 35, 4, "album year")
        self.near(a["suffix"], 16, 3, "album suffix")
        self.near(a["other"], 4.5, 2, "album other")
        r = s["artist_folders_vs_tag"]
        self.near(r["same"], 88, 3, "artist same")
        self.near(r["case"], 9, 3, "artist case")
        self.near(r["other"], 3, 1.5, "artist other")

    def test_tag_presence(self):
        t = self.s["tags"]
        self.near(t["title"], 99.4, 1, "title")
        self.near(t["artist"], 99.7, 1, "artist")
        self.near(t["track"], 98, 1.5, "track")
        self.near(t["year"], 95.2, 2, "year")
        self.near(t["genre"], 78.7, 4, "genre")
        self.near(t["disc"], 24.2, 4, "disc")
        self.near(t["disc_gt1"], 2.1, 1.2, "disc > 1")
        self.near(t["albumartist"], 35.7, 6, "album artist")
        self.near(t["compilation"], 0.8, 0.8, "compilation")
        self.near(t["picture_mp3"], 22, 4, "MP3 picture")
        self.near(t["picture_flac"], 47, 12, "FLAC picture")
        self.near(t["non_latin_title"], 0.3, 0.2, "non-Latin titles")
        self.assertGreater(t["sort"], 0.5)
        self.assertGreater(t["multi_artist"], 2)
        self.assertGreater(t["multi_genre"], 0.5)
        mc = self.s["mp3_containers"]
        self.near(mc["id3v2.3"], 79, 5, "v2.3")
        self.near(mc["id3v2.4"], 17, 4, "v2.4")
        self.near(mc["id3v1"], 60, 6, "ID3v1")
        self.near(mc["unsync"], 9, 3, "unsync")
        self.near(mc["xing"], 76, 5, "Xing/Info")

    def test_fat_layout(self):
        f, m = self.s["fat"], sc.MEASURED
        self.near(f["music_entries"], m["music_fat_entries"], 100, "/music entries")
        self.near(f["music_sectors"], m["music_sectors"], 6, "/music sectors")
        self.near(f["open_sectors_mean"], m["open_sectors_mean"], 4, "sectors per open")
        self.near(f["dir_entries"]["mean"], m["dir_entries"]["mean"], 5, "entries per folder")

    def test_images_and_other_files(self):
        s = self.s
        self.near(s["images"]["jpg"] + s["images"]["png"], sc.MEASURED["images"], 250, "images")
        self.near(s["images"]["named_cover_folders"], 647, 80, "named covers")
        self.near(s["other_files"]["text"] + s["other_files"]["blob"], sc.MEASURED["other_files"], 250, "other files")
        self.near(s["other_files"]["hidden"], sc.MEASURED["hidden"], 40, "hidden files")

    def test_probes(self):
        p = self.s["probes"]
        self.assertEqual([x["wanted"] for x in p], list(sc.PROBE_POSITIONS))
        for x in p:
            self.assertLessEqual(abs(x["position"] - x["wanted"]), 10)
        self.assertEqual(p[0]["sector"], 0)
        self.assertGreaterEqual(p[2]["sector"], 95)
        # Plain: MP3 only, no pictures, nothing else in the folder.
        for x in p:
            d = f"music/{x['artist']}/{x['album']}/"
            inside = [f for f in self.plan.files if f["rel"].startswith(d)]
            self.assertTrue(inside)
            self.assertTrue(all(f["kind"] == "mp3" and not f.get("pic") for f in inside), d)

    def test_names(self):
        """Every name is one the card can hold: no FAT-forbidden character, no
        trailing dot or space, no Windows device name, unique in its folder
        case-insensitively, paths within the walk's 255 bytes."""
        seen = set()
        for rel in [f["rel"] for f in self.plan.files] + self.plan.folders:
            for part in rel.split("/"):
                self.assertFalse(any(c in sc.FAT_BAD for c in part), rel)
                self.assertFalse(part.endswith((" ", ".")), rel)
                self.assertNotIn(part.split(".")[0].upper(), sc.WIN_RESERVED, rel)
            self.assertLessEqual(len(("/" + rel).encode("utf-8")), 255, rel)
            key = sc.fold(rel)
            self.assertNotIn(key, seen, rel)
            seen.add(key)

    def test_no_real_names_shape(self):
        """Artist names come from the made-up syllables (or the two generic
        compilation labels); no word list entry is an artist name on its own."""
        for a in self.plan.artists:
            if a["va"]:
                self.assertIn(a["tag"], ("Various Artists",))
                continue
            words = re.findall(r"[A-Za-z]+", a["tag"])
            self.assertTrue(words or not a["tag"].isascii(), a["tag"])


class DeterminismTests(unittest.TestCase):
    def digest(self, plan):
        h = hashlib.sha256()
        for f in plan.files:
            h.update(json.dumps([f["rel"], f["kind"], f.get("tags"), f.get("mtime"), f.get("pic")], sort_keys=True,
                                ensure_ascii=False).encode("utf-8"))
        for d in plan.folders:
            h.update(d.encode("utf-8"))
        return h.hexdigest()

    def test_same_seed_same_tree(self):
        a = sc.make_plan(3000, 7)
        b = sc.make_plan(3000, 7)
        self.assertEqual(self.digest(a), self.digest(b))
        for fa, fb in zip(sc.sample_files(a, 40), sc.sample_files(b, 40)):
            self.assertEqual(sc.file_bytes(fa)[0], sc.file_bytes(fb)[0], fa["rel"])

    def test_another_seed_another_tree(self):
        self.assertNotEqual(self.digest(sc.make_plan(3000, 7)), self.digest(sc.make_plan(3000, 8)))

    def test_counts_scale(self):
        p = sc.make_plan(2000, 3)
        s = sc.summary(p)
        self.assertEqual(s["audio_files"], 2000)
        self.assertEqual(s["artists"], round(705 * 2000 / 19519))


class StubTests(unittest.TestCase):
    """A spread of the stubs (every container variant), read back."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="synthcard_test_")
        cls.plan = default_plan()
        cls.files = sc.sample_files(cls.plan, 160)
        cls.rows = {}
        for i, f in enumerate(cls.files):
            data, ms, pic, written = sc.file_bytes(f)
            # (ASCII names: the harness opens them with the C library's fopen)
            p = os.path.join(cls.tmp, f"{i:03d}.{f['kind']}")
            with open(p, "wb") as fh:
                fh.write(data)
            cls.rows[p] = (f, data, ms, pic, written)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_variants_covered(self):
        kinds = {(f["kind"], f["style"].get("id3"), bool(f["style"].get("unsync")), bool(f.get("pic")))
                 for f, *_ in self.rows.values()}
        for want in (("mp3", "2.2", False, False), ("mp3", "2.3", False, True), ("mp3", "2.4", True, True),
                     ("flac", None, False, True), ("opus", None, False, False)):
            self.assertIn(want, kinds)
        self.assertTrue(any(f["style"].get("v1only") for f, *_ in self.rows.values()))
        self.assertTrue(any(f.get("pic") == "png" for f, *_ in self.rows.values()))
        self.assertTrue(any(f["kind"] == "opus" and f.get("pic") for f, *_ in self.rows.values()))

    def test_sizes_small(self):
        for p, (f, data, *_) in self.rows.items():
            self.assertLess(len(data), 12000, f["rel"])

    def test_mp3_own_reader(self):
        for p, (f, data, ms, pic, written) in self.rows.items():
            if f["kind"] != "mp3":
                continue
            st, tags = f["style"], f["tags"]
            start, end = 0, len(data)
            if written.get("v1"):
                self.assertEqual(data[-128:-125], b"TAG")
                end -= 128
            if written.get("ape"):
                self.assertEqual(data[end - 32:end - 24], b"APETAGEX")
                size = struct.unpack("<I", data[end - 20:end - 16])[0]
                end -= size + 32
            if written.get("id3"):
                ver, flags, frames, pic_seen, tag_end = read_id3v2(data)
                self.assertEqual(f"2.{ver}", st["id3"])
                start = tag_end
                if tags.get("title"):
                    self.assertEqual(frames["TIT2"][0], tags["title"], p)
                if tags.get("album"):
                    self.assertEqual(frames["TALB"][0], tags["album"], p)
                arts = frames.get("TPE1", [])
                if ver == 4:
                    self.assertEqual(arts, tags["artist"])
                else:
                    self.assertEqual(arts, [" / ".join(tags["artist"])])
                if tags.get("track"):
                    self.assertEqual(int(frames["TRCK"][0].split("/")[0]), tags["track"])
                if tags.get("year"):
                    y = frames.get("TDRC", frames.get("TYER"))[0]
                    self.assertEqual(int(y[:4]), tags["year"])
                if f.get("pic"):
                    self.assertIsNotNone(pic_seen)
                    off, ln, img = pic_seen
                    self.assertEqual((off, ln), (pic["offset"], pic["length"]))
                    want = sc._picture(f["pic"])[0]
                    self.assertEqual(img, want)
                    stored = data[off:off + ln]
                    self.assertEqual(unsync_undo(stored) if pic["coding"] == 1 else stored, want)
                if st.get("unsync"):
                    self.assertTrue(flags & 0x80)
            n, ok = mp3_frames(data, start, end)
            self.assertTrue(ok, p)
            self.assertEqual(n, f["frames"] + (1 if st["xing"] else 0), p)

    def test_flac_own_reader(self):
        for p, (f, data, ms, pic, written) in self.rows.items():
            if f["kind"] != "flac":
                continue
            r = read_flac(data)
            self.assertEqual(r["rate"], 44100)
            self.assertEqual(r["total"], f["seconds"] * 44100)
            self.assertTrue(r["frames_ok"], p)
            self.assertEqual(r["frames"], (f["seconds"] * 44100 + 4095) // 4096)
            m = multimap(r["comments"])
            if f["tags"].get("title"):
                self.assertEqual(m["TITLE"], [f["tags"]["title"]])
            self.assertEqual(m.get("ARTIST", []), f["tags"].get("artist") or [])
            self.assertEqual(m.get("GENRE", []), f["tags"].get("genre") or [])
            if f.get("pic"):
                self.assertEqual(r["pictures"][0], (pic["offset"], pic["length"]))
                self.assertEqual(data[pic["offset"]:pic["offset"] + pic["length"]], sc._picture(f["pic"])[0])

    def test_opus_own_reader(self):
        for p, (f, data, ms, pic, written) in self.rows.items():
            if f["kind"] != "opus":
                continue
            pages, packets = read_ogg(data)
            self.assertTrue(all(pg["crc_ok"] for pg in pages), p)
            self.assertEqual(pages[0]["flags"], 2)
            self.assertEqual(pages[-1]["flags"] & 4, 4)
            self.assertEqual(packets[0][:8], b"OpusHead")
            self.assertEqual(packets[1][:8], b"OpusTags")
            m = multimap(vorbis_list(packets[1][8:]))
            if f["tags"].get("title"):
                self.assertEqual(m["TITLE"], [f["tags"]["title"]])
            self.assertEqual(pages[-1]["granule"] - 312, f["seconds"] * 48000)
            self.assertTrue(all(pk == sc.SILENCE_STEREO for pk in packets[2:]))
            if f.get("pic"):
                block = base64.b64decode(m["METADATA_BLOCK_PICTURE"][0])
                self.assertTrue(block.endswith(sc._picture(f["pic"])[0]))
                raw = data[pic["offset"]:pic["offset"] + 16]
                self.assertEqual(raw, m["METADATA_BLOCK_PICTURE"][0][:16].encode())

    def test_ffprobe(self):
        ff = shutil.which("ffprobe")
        if not ff:
            self.skipTest("ffprobe isn't installed: the repo's parsers check the stubs instead (test_repo_parsers)")
        for p, (f, data, ms, pic, written) in self.rows.items():
            r = subprocess.run([ff, "-v", "error", "-show_format", "-show_streams", "-of", "json", p],
                               capture_output=True, text=True, encoding="utf-8")
            self.assertEqual(r.returncode, 0, f"{p}: {r.stderr}")
            j = json.loads(r.stdout)
            codecs = [s["codec_name"] for s in j["streams"] if s.get("codec_type") == "audio"]
            self.assertEqual(codecs, [{"mp3": "mp3", "flac": "flac", "opus": "opus"}[f["kind"]]], p)
            tags = {k.lower(): v for k, v in (j["format"].get("tags") or {}).items()}
            if f["kind"] == "opus":
                tags.update({k.lower(): v for s in j["streams"] for k, v in (s.get("tags") or {}).items()})
            t = f["tags"]
            if t.get("title") and not (f["kind"] == "mp3" and f["style"]["v1only"]):
                self.assertEqual(tags.get("title"), t["title"], p)
            self.assertGreater(float(j["format"].get("duration", 0)), 0.3, p)

    def test_repo_parsers(self):
        exe, how = build_probe()
        if not exe:
            self.skipTest(f"the host harness couldn't be built: {how}")
        out = run_probe(exe, list(self.rows))
        for p, (f, data, ms, pic, written) in self.rows.items():
            r = out[p]
            self.assertEqual(r["bytes"], len(data))
            if f["kind"] == "mp3":
                self.assertTrue(r["chained"], f"{p}: {r}")
                self.assertEqual(r["framesWalked"], f["frames"] + (1 if f["style"]["xing"] else 0), p)
                self.assertEqual(r["v1"], bool(written.get("v1")), p)
                if f["style"]["xing"]:
                    self.assertTrue(r["lame"]["header"], p)
                    self.assertEqual(r["lame"]["frames"], f["frames"], p)
                    self.assertEqual(r["lame"]["lame"], f["style"]["lame"], p)
                    self.assertEqual(r["durationMs"], ms, p)
                else:
                    self.assertEqual(r["durationMs"], 0, p)  # a plain CBR file: the firmware estimates
            elif f["kind"] == "flac":
                self.assertEqual(r["durationMs"], ms, p)
            else:
                self.assertEqual(r["open"].lower(), "ok", f"{p}: {r}")
                self.assertEqual(r["channels"], 2)
                self.assertEqual(r["durationMs"], ms, p)
            if "tagscan" in r:
                check_tagscan(self, p, f, pic, r["tagscan"])
        print(f"\n  the repo's parsers ({how}): {len(self.rows)} stubs read", file=sys.stderr)


# ---------------------------------------------------------------------------
# The host harness (tools/synthcard_probe.cpp)
# ---------------------------------------------------------------------------

_PROBE = {}


def include_closure(entry, skip=()):
    """The lib/core sources `entry` needs: each #include "X.h" with a
    lib/core/X.cpp, and theirs."""
    todo, seen, cpp = [entry], set(), []
    while todo:
        f = todo.pop()
        if f in seen:
            continue
        seen.add(f)
        text = Path(f).read_text(encoding="utf-8", errors="replace")
        for inc in re.findall(r'^\s*#include\s+"([^"]+)"', text, re.M):
            if inc in skip:
                continue
            h = CORE / inc
            if h.exists():
                todo.append(str(h))
                # X.cpp, and the companions that define X.h's tables (NameKeyTables.cpp).
                for c in [h.with_suffix(".cpp")] + sorted(CORE.glob(h.stem + "?*.cpp")):
                    if not c.exists() or str(c) in seen:
                        continue
                    if c != h.with_suffix(".cpp") and f'#include "{inc}"' not in c.read_text(
                            encoding="utf-8", errors="replace"):
                        continue
                    todo.append(str(c))
                    cpp.append(str(c))
    return sorted(set(cpp))


def build_probe():
    """(exe, how) or (None, why): with TagScan when it builds, else without."""
    if "exe" in _PROBE:
        return _PROBE["exe"], _PROBE["how"]
    gxx = shutil.which("g++")
    if not gxx:
        _PROBE.update(exe=None, how="no g++ on PATH")
        return None, _PROBE["how"]
    entry = str(ROOT / "tools" / "synthcard_probe.cpp")
    tries = []
    if (CORE / "TagScan.cpp").exists():
        tries.append(("with lib/core/TagScan (N6)", ["-DSYNTHCARD_TAGSCAN"], ()))
    tries.append(("without TagScan: the MP3, FLAC and Opus readers", [], ("TagScan.h",)))
    why = []
    for how, defs, skip in tries:
        srcs = include_closure(entry, skip)
        h = hashlib.sha256()
        for s in [entry] + srcs:
            h.update(Path(s).read_bytes())
        h.update(" ".join(defs).encode())
        exe = os.path.join(tempfile.gettempdir(), f"synthcard_probe_{h.hexdigest()[:12]}.exe")
        if not os.path.exists(exe):
            cmd = [gxx, "-std=gnu++17", "-O1", "-w", "-I", str(CORE), "-I", str(ROOT / "src")] + defs + \
                [entry] + srcs + ["-o", exe]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0:
                why.append(f"{how}: {r.stderr.strip().splitlines()[:3]}")
                continue
        _PROBE.update(exe=exe, how=how + (f" ({'; '.join(why)})" if why else ""))
        return exe, _PROBE["how"]
    _PROBE.update(exe=None, how="; ".join(why))
    return None, _PROBE["how"]


def run_probe(exe, paths):
    r = subprocess.run([exe], input="\n".join(paths) + "\n", capture_output=True, text=True, encoding="utf-8",
                       timeout=300)
    if r.returncode != 0:
        raise AssertionError(f"synthcard_probe failed: {r.stderr[:500]}")
    out = {}
    for line in r.stdout.splitlines():
        j = json.loads(line)
        out[j["path"]] = j
    return out


def check_tagscan(t, p, f, pic, r):
    """N6's record against what the stub carries (the fields part 5 decides
    the same whatever the reader: the title, the album, the numbers, the
    picture's anchor)."""
    tags, st = f["tags"], f["style"]
    if not tags:
        return
    t.assertEqual(r["result"], "ok", f"{p}: {r}")
    v1only = f["kind"] == "mp3" and st["v1only"]
    want_title = (tags.get("title") or "")
    if v1only:
        want_title = want_title.encode("latin-1", "replace")[:30].decode("latin-1").rstrip("\0 ")
    if want_title:
        t.assertEqual(r.get("title", ""), want_title, p)
    if tags.get("album") and not v1only:
        t.assertEqual(r.get("album", ""), tags["album"], p)
    if tags.get("track"):
        t.assertEqual(r["track"], tags["track"], p)
    if tags.get("year") and not v1only:
        t.assertEqual(r["year"], tags["year"], p)
    if tags.get("disc") and not v1only:
        t.assertEqual(r["disc"], tags["disc"], p)
    if tags.get("artist") and not v1only:
        got = r.get("artist", "").replace("\x1f", " / ")
        t.assertTrue(got.startswith(tags["artist"][0]), f"{p}: artist {got!r}, {tags['artist']!r} written")
    if pic and not v1only:
        t.assertTrue(r["hasPicture"], f"{p}: no picture")
        t.assertEqual((r["picOffset"], r["picLength"]), (pic["offset"], pic["length"]), p)


# ---------------------------------------------------------------------------
# Writing a tree, and --copy-to on a fake drive
# ---------------------------------------------------------------------------


class FakeProbe(sc.DriveProbe):
    def __init__(self, root, kind="removable", fs="FAT32", free=10 ** 10, system="C:\\"):
        self.root, self.kind, self.fs, self.free, self.sys = root, kind, fs, free, system

    def removable(self, root):
        return False

    def system_drive(self):
        return self.sys

    def info(self, root):
        return {"type": self.kind, "label": "TESTCARD", "fs": self.fs, "size": 32 * 10 ** 9, "free": self.free,
                "cluster": 32768, "readable": True}

    def target(self, root, rel):
        return os.path.join(self.root, *rel.split("/"))


class TreeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="synthcard_tree_")
        cls.out = os.path.join(cls.tmp, "tree")
        cls.plan, cls.summ = sc.build(cls.out, 400, 5, 2.0, progress=False)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(sc.long_path(cls.tmp), ignore_errors=True)

    def test_tree_written(self):
        audio = [f for f in self.plan.files if f["kind"] in sc.AUDIO]
        for f in self.plan.files[:200]:
            self.assertTrue(os.path.isfile(sc.long_path(os.path.join(self.out, *f["rel"].split("/")))), f["rel"])
        with open(os.path.join(self.out, "synthcard-files.jsonl"), encoding="utf-8") as fh:
            rows = [json.loads(x) for x in fh]
        self.assertEqual(len(rows), len(audio))
        with open(os.path.join(self.out, sc.MARKER), encoding="utf-8") as fh:
            j = json.load(fh)
        self.assertEqual(j["audio_files"], 400)
        self.assertGreater(j["bytes"], 0)
        self.assertIn("32K", j["card"])
        # The file times are the plan's (FAT keeps 2 s steps).
        f = audio[0]
        st = os.stat(sc.long_path(os.path.join(self.out, *f["rel"].split("/"))))
        self.assertEqual(int(st.st_mtime), f["mtime"])
        self.assertEqual(f["mtime"] % 2, 0)

    def test_rebuild_same_bytes(self):
        out2 = os.path.join(self.tmp, "tree2")
        sc.build(out2, 400, 5, 2.0, progress=False)
        a = Path(self.out, "synthcard-files.jsonl").read_bytes()
        b = Path(out2, "synthcard-files.jsonl").read_bytes()
        self.assertEqual(a, b)

    def test_out_dir_refusals(self):
        other = os.path.join(self.tmp, "someone_elses")
        os.makedirs(other)
        open(os.path.join(other, "keep.txt"), "w").close()
        with self.assertRaises(SystemExit):
            sc.check_out_dir(other)

        class Removable(FakeProbe):
            def removable(self, root):
                return True
        with self.assertRaises(SystemExit):
            sc.check_out_dir(os.path.join(self.tmp, "new"), Removable(self.tmp))

    def copy(self, probe, yes):
        buf = io.StringIO()
        try:
            rc = sc.copy_to(self.out, "E:\\", yes, probe, buf)
        except SystemExit as e:
            return "refused", str(e), buf.getvalue()
        return rc, "", buf.getvalue()

    def test_copy_refusals(self):
        card = os.path.join(self.tmp, "card_refusals")
        os.makedirs(card)
        for probe, word in ((FakeProbe(card, kind="fixed"), "removable"), (FakeProbe(card, fs="exFAT"), "FAT32"),
                            (FakeProbe(card, fs="NTFS"), "FAT32"), (FakeProbe(card, free=1000), "free"),
                            (FakeProbe(card, system="E:\\"), "system")):
            rc, why, said = self.copy(probe, True)
            self.assertEqual(rc, "refused", said)
            self.assertIn(word, why)
            self.assertIn("TESTCARD", said + why if word != "system" else "TESTCARD")
            self.assertEqual(os.listdir(card), [])
        os.makedirs(os.path.join(card, "music"))
        rc, why, said = self.copy(FakeProbe(card), True)
        self.assertEqual(rc, "refused")
        self.assertIn("music", why)
        self.assertEqual(os.listdir(os.path.join(card, "music")), [])
        with self.assertRaises(SystemExit):
            sc.copy_to(self.out, "E:\\music", True, FakeProbe(card), io.StringIO())

    def test_copy_refuses_a_card_that_isnt_empty(self):
        # Anything at the root but Windows' own System Volume Information: refused, nothing written.
        for what in ("DCIM", "photo.jpg", ".hidden", "$RECYCLE.BIN"):
            card = os.path.join(self.tmp, "card_full_" + what.strip(".$"))
            os.makedirs(card)
            p = os.path.join(card, what)
            if "." in what[1:]:
                Path(p).write_bytes(b"keep me")
            else:
                os.makedirs(p)
            rc, why, said = self.copy(FakeProbe(card), True)
            self.assertEqual(rc, "refused", said)
            self.assertIn("isn't empty", why)
            self.assertIn(what, why)
            self.assertEqual(os.listdir(card), [what])
        card = os.path.join(self.tmp, "card_svi")
        os.makedirs(os.path.join(card, "System Volume Information"))
        rc, why, said = self.copy(FakeProbe(card), False)
        self.assertEqual(rc, 2, why)
        self.assertEqual(os.listdir(card), ["System Volume Information"])

    def test_copy_needs_room_for_the_folders_too(self):
        # A folder's entries take whole clusters, more than one when they don't fit (/music's 1,600-odd do not
        # at 32 KB): with 512 B clusters, room for the files and one cluster a folder isn't enough.
        class SmallClusters(FakeProbe):
            def info(self, root):
                return dict(FakeProbe.info(self, root), cluster=512)
        card = os.path.join(self.tmp, "card_room")
        os.makedirs(card)
        ops = sc.copy_plan(self.out)
        files = sum((sz + 511) // 512 * 512 for k, _, sz in ops if k == "file")
        dirs = sum(1 for k, _, _ in ops if k == "dir")
        rc, why, said = self.copy(SmallClusters(card, free=files + dirs * 512), True)
        self.assertEqual(rc, "refused", said)
        self.assertIn("free", why)
        self.assertEqual(os.listdir(card), [])

    def test_copy_needs_yes_then_copies_in_order(self):
        card = os.path.join(self.tmp, "card_ok")
        os.makedirs(card)
        rc, why, said = self.copy(FakeProbe(card), False)
        self.assertEqual(rc, 2)
        self.assertIn('label "TESTCARD"', said)
        self.assertIn("FAT32", said)
        self.assertIn("--yes", said)
        self.assertEqual(os.listdir(card), [])
        rc, why, said = self.copy(FakeProbe(card), True)
        self.assertEqual(rc, 0, why)
        self.assertIn("copied", said)
        ops = sc.copy_plan(self.out)
        files = [r for k, r, _ in ops if k == "file"]
        self.assertEqual(files[0], "SYNTHCARD.TXT")
        for rel in files:
            self.assertTrue(os.path.isfile(sc.long_path(os.path.join(card, *rel.split("/")))), rel)
        # The artist folders made in NTFS's name order: the order the probes' positions assume.
        artists = [r.split("/")[1] for k, r, _ in ops if k == "dir" and r.count("/") == 1]
        self.assertEqual(artists, sorted(artists, key=sc.ntfs_key))
        self.assertEqual(len(artists), self.summ["artists"])
        # The tree's bytes and times, kept.
        f = [x for x in self.plan.files if x["kind"] == "mp3"][0]
        a = sc.long_path(os.path.join(self.out, *f["rel"].split("/")))
        b = sc.long_path(os.path.join(card, *f["rel"].split("/")))
        self.assertEqual(Path(a).read_bytes(), Path(b).read_bytes())
        self.assertEqual(int(os.stat(a).st_mtime), int(os.stat(b).st_mtime))


if __name__ == "__main__":
    unittest.main()
