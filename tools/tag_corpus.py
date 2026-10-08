#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""The tag reader's synthetic parity corpus (docs/METADATA.md 2.17, item 3).

Writes test/fixtures/tags/: small crafted MP3, FLAC and Opus files, each one
or a few of part 5's reading rules (multi-value frames, ID3v1 filling a blank
ID3v2 field, APE beside ID3v1, bad UTF-8, odd UTF-16, ISO-8859-1's 0x80-0x9F,
v2.4 unsynchronisation, sizes that aren't syncsafe, a FLAC with a front
ID3v2, Opus R128 gains, pictures behind large frames, an Opus picture across
pages, a compressed and an encrypted APIC, 2.18's number strings in every
numeric field), and anchors.json: where each embedded picture's bytes are
(2.6.4's anchor, as the record names it) and the FNV-1a 64 of its image data.

The records the files must give are expected.json, made by the reference
reader (tools/tagref: lofty 0.25 with mStream's selection rules and part 5),
not here: the player's TagScan matching them is two readers agreeing.

    python tools/tag_corpus.py          write the corpus
    python tools/tag_corpus.py --check  fail if a file on disk differs

Every name in the files is made up. The audio is a few silent frames; the
pictures are a few bytes that only start like a JPEG or a PNG.
"""
import argparse
import base64
import json
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "test" / "fixtures" / "tags"

# ---------------------------------------------------------------------------
# Bytes
# ---------------------------------------------------------------------------


def be32(v):
    return struct.pack(">I", v)


def le32(v):
    return struct.pack("<I", v)


def syncsafe(n):
    return bytes([(n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F])


def unsync(b):
    """ID3 unsynchronisation: a 00 after every FF (and before nothing else)."""
    out = bytearray()
    for i, x in enumerate(b):
        out.append(x)
        if x == 0xFF and (i + 1 == len(b) or b[i + 1] == 0 or b[i + 1] >= 0xE0):
            out.append(0)
    return bytes(out)


def jpeg(seed, size=96):
    """Bytes that start like a JPEG (FF D8 FF E0) and end like one; between,
    lowercase letters (no NUL, no frame id, no MPEG sync)."""
    body = bytes(0x61 + (seed * 31 + i * 7) % 26 for i in range(max(size - 6, 0)))
    return b"\xFF\xD8\xFF\xE0" + body + b"\xFF\xD9"


def jpeg_ff(seed, size=96):
    """A JPEG-ish picture with FF bytes that unsynchronisation must stuff
    (an FF before 00, and before E0 and up) and one it needn't."""
    body = bytearray()
    for i in range(max(size - 6, 0)):
        k = i % 11
        body.append({3: 0xFF, 4: 0x00, 7: 0xFF, 8: 0xE1, 9: 0xFF, 10: 0x41}.get(k, 0x61 + (seed + i * 5) % 26))
    return b"\xFF\xD8\xFF\xE0" + bytes(body) + b"\xFF\xD9"


def png(seed, size=96):
    body = bytes(0x61 + (seed * 7 + i * 3) % 26 for i in range(max(size - 8, 0)))
    return b"\x89PNG\r\n\x1a\n" + body


def digest(data):
    """FNV-1a 64 of the image data (2.3.3's function), as hex."""
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return "%016x" % h


# ---------------------------------------------------------------------------
# MPEG audio: MPEG-1 Layer III, 32 kHz, 32 kbps, stereo: 144-byte frames.
# A Xing header frame claiming 1000 frames (36 s) and 4000 bytes (under the
# 4 KB below which the device never scales a length down), with LAME's
# extension (delay 576, padding 576: the device's trim, 36 ms, is inside
# 2.17's 100 ms), then two silent frames.
# ---------------------------------------------------------------------------
FRAME = 144
HDR = b"\xFF\xFB\x18\x00"


def crc16_lame(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def mp3_audio(xing=True, frames=2, lame=True):
    out = bytearray()
    if xing:
        f = bytearray(FRAME)
        f[0:4] = HDR
        x = 4 + 32
        f[x:x + 4] = b"Xing"
        f[x + 4:x + 8] = be32(3)
        f[x + 8:x + 12] = be32(1000)
        f[x + 12:x + 16] = be32(4000)
        if lame:
            l = x + 16
            f[l:l + 9] = b"LAME3.100"
            # +21: delay and padding, 12 bits each.
            delay, padding = 576, 576
            f[l + 21] = delay >> 4
            f[l + 22] = ((delay & 0xF) << 4) | (padding >> 8)
            f[l + 23] = padding & 0xFF
            crc = crc16_lame(bytes(f[:l + 34]))
            f[l + 34] = crc >> 8
            f[l + 35] = crc & 0xFF
        out += f
    for _ in range(frames):
        f = bytearray(FRAME)
        f[0:4] = HDR
        out += f
    return bytes(out)


def mp3_mpeg2(frames=40):
    """MPEG-2 Layer III, 16 kHz, 8 kbps, mono: 36-byte frames, no Xing."""
    out = bytearray()
    for _ in range(frames):
        f = bytearray(36)
        f[0:4] = b"\xFF\xF3\x18\xC0"
        out += f
    return bytes(out)


# ---------------------------------------------------------------------------
# ID3v2
# ---------------------------------------------------------------------------


def enc_text(enc, s, terminate=False):
    """Text in ID3v2 encoding `enc` (0 Latin-1, 1 UTF-16 with BOM, 2 UTF-16BE,
    3 UTF-8). `s` may be bytes (taken as they are)."""
    if isinstance(s, bytes):
        b = s
    elif enc == 0:
        b = s.encode("latin-1")
    elif enc == 1:
        b = b"\xFF\xFE" + s.encode("utf-16-le")
    elif enc == 2:
        b = s.encode("utf-16-be")
    else:
        b = s.encode("utf-8")
    if terminate:
        b += b"\x00\x00" if enc in (1, 2) else b"\x00"
    return b


def text_frame(enc, *values):
    """An ID3v2 text frame's body: values joined with the encoding's NUL."""
    sep = b"\x00\x00" if enc in (1, 2) else b"\x00"
    parts = []
    for i, v in enumerate(values):
        if enc == 1 and i > 0 and isinstance(v, str):
            parts.append(b"\xFF\xFE" + v.encode("utf-16-le"))
        else:
            parts.append(enc_text(enc, v))
    return bytes([enc]) + sep.join(parts)


def txxx(enc, desc, *values):
    sep = b"\x00\x00" if enc in (1, 2) else b"\x00"
    body = bytes([enc]) + enc_text(enc, desc, terminate=True)
    parts = []
    for i, v in enumerate(values):
        if enc == 1 and isinstance(v, str):
            parts.append(b"\xFF\xFE" + v.encode("utf-16-le"))
        else:
            parts.append(enc_text(enc, v))
    return body + sep.join(parts)


def apic(ver, ptype, mime, data, desc="", enc=0):
    if ver == 2:
        fmt = {"image/jpeg": b"JPG", "image/png": b"PNG"}.get(mime, mime.encode()[:3])
        head = bytes([enc]) + fmt + bytes([ptype]) + enc_text(enc, desc, terminate=True)
    else:
        head = bytes([enc]) + mime.encode("latin-1") + b"\x00" + bytes([ptype]) + enc_text(enc, desc, terminate=True)
    return head, data


def ufid(owner, ident):
    return owner.encode("latin-1") + b"\x00" + ident.encode("latin-1")


class Tag:
    """An ID3v2 tag being built, tracking where each picture's data lands."""

    def __init__(self, ver, flags=0):
        self.ver = ver
        self.flags = flags
        self.frames = []  # (header, body, picture: (offset in body, stored length, data, coding) or None)

    def frame(self, fid, body, fflags=0, size=None, plain_size=False, picture=None, stored_picture=None):
        """`body` is the frame's stored content; `picture` (head, data) for an
        APIC: body is then head + data. `stored_picture` (offset in body,
        stored length, data): a picture the frame itself stores
        unsynchronised (picCoding 1). `size` overrides the size field."""
        pic = None
        if picture is not None:
            head, data = picture
            pic = (len(head), len(data), data, 0)
            body = head + data
        elif stored_picture is not None:
            o, n, d = stored_picture
            pic = (o, n, d, 1)
        n = len(body) if size is None else size
        if self.ver == 2:
            h = fid.encode() + bytes([(n >> 16) & 0xFF, (n >> 8) & 0xFF, n & 0xFF])
        elif self.ver == 3:
            h = fid.encode() + be32(n) + struct.pack(">H", fflags)
        else:
            h = fid.encode() + (be32(n) if plain_size else syncsafe(n)) + struct.pack(">H", fflags)
        self.frames.append((h, body, pic))
        return self

    def raw(self, data):
        """Bytes as they are (junk, a broken frame)."""
        self.frames.append((b"", data, None))
        return self

    def build(self, padding=64, start=0, unsync_tag=False, ext=None):
        """The tag's bytes and its pictures [(offset in file, stored length,
        coding, data)]. `ext`: an extended header's bytes."""
        body = bytearray()
        pics = []
        if ext:
            body += ext
        for h, b, pic in self.frames:
            fstart = len(body) + len(h)
            body += h + b
            if pic is not None:
                pics.append((fstart + pic[0], pic[1], pic[2], pic[3]))
        body += bytes(padding)
        if unsync_tag:
            # v2.2/2.3: the whole body unsynchronised; the pictures' stored
            # bytes are found again by mapping offsets through.
            mapped = bytearray()
            index = []
            for i, x in enumerate(body):
                index.append(len(mapped))
                mapped.append(x)
                if x == 0xFF and (i + 1 == len(body) or body[i + 1] == 0 or body[i + 1] >= 0xE0):
                    mapped.append(0)
            index.append(len(mapped))
            pics = [(index[o], index[o + n] - index[o], d, 1) for o, n, d, _ in pics]
            body = mapped
        head = b"ID3" + bytes([self.ver, 0, self.flags]) + syncsafe(len(body))
        out = head + bytes(body)
        return out, [{"offset": start + 10 + o, "length": n, "coding": c, "fnv": digest(d)} for o, n, d, c in pics]


def id3v1(title="", artist="", album="", year="", comment="", track=None, genre=255):
    def field(s, n):
        b = s.encode("latin-1") if isinstance(s, str) else s
        return (b + bytes(n))[:n]

    out = b"TAG" + field(title, 30) + field(artist, 30) + field(album, 30) + field(year, 4)
    if track is None:
        out += field(comment, 30)
    else:
        out += field(comment, 28) + b"\x00" + bytes([track])
    return out + bytes([genre])


def ape(items, header=True):
    """An APEv2 tag: items (key, value bytes, type). Returns (bytes, pictures
    relative to the tag's start)."""
    body = bytearray()
    pics = []
    for key, value, typ in items:
        start = len(body) + 8 + len(key) + 1
        body += le32(len(value)) + le32(typ << 1) + key.encode("ascii") + b"\x00" + value
        if typ == 1 and key.lower().startswith("cover art"):
            nul = value.index(b"\x00")
            pics.append((start + nul + 1, len(value) - nul - 1, value[nul + 1:]))
    size = len(body) + 32
    flags = 0xA0000000 if header else 0x80000000
    hdr = b"APETAGEX" + le32(2000) + le32(size) + le32(len(items)) + le32(flags | 0x20000000) + bytes(8)
    foot = b"APETAGEX" + le32(2000) + le32(size) + le32(len(items)) + le32(flags & ~0x20000000) + bytes(8)
    pre = hdr if header else b""
    return pre + bytes(body) + foot, [(len(pre) + o, n, d) for o, n, d in pics]


# ---------------------------------------------------------------------------
# FLAC
# ---------------------------------------------------------------------------


def streaminfo(rate=44100, channels=2, bps=16, total=44100 * 30):
    b = bytearray()
    b += struct.pack(">HH", 4096, 4096)
    b += bytes(6)  # min/max frame size: unknown
    v = (rate << 44) | ((channels - 1) << 41) | ((bps - 1) << 36) | total
    b += struct.pack(">Q", v)
    b += bytes(16)  # MD5
    return bytes(b)


def vorbis_comment(items, vendor="corpus"):
    out = le32(len(vendor)) + vendor.encode() + le32(len(items))
    for it in items:
        b = it if isinstance(it, bytes) else it.encode("utf-8")
        out += le32(len(b)) + b
    return out


def flac_picture(ptype, mime, data, desc="", dims=(1, 1, 24, 0)):
    m = mime.encode()
    d = desc.encode()
    head = be32(ptype) + be32(len(m)) + m + be32(len(d)) + d + b"".join(be32(x) for x in dims) + be32(len(data))
    return head, data


def flac(blocks, id3=b""):
    """blocks: (type, content) or (type, (head, data)) for a picture. Returns
    the bytes and the pictures."""
    out = bytearray(id3)
    out += b"fLaC"
    pics = []
    allb = [(0, streaminfo())] + list(blocks)
    for i, (typ, content) in enumerate(allb):
        last = i == len(allb) - 1
        if isinstance(content, tuple):
            head, data = content
            body = head + data
            pics.append((len(out) + 4 + len(head), len(data), data))
        else:
            body = content
        out += bytes([(0x80 if last else 0) | typ]) + struct.pack(">I", len(body))[1:] + body
    out += b"\xFF\xF8" + bytes(30)  # where a frame would start
    return bytes(out), [{"offset": o, "length": n, "coding": 0, "fnv": digest(d)} for o, n, d in pics]


# ---------------------------------------------------------------------------
# Ogg Opus
# ---------------------------------------------------------------------------


def ogg_crc(data):
    crc = 0
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc


class Ogg:
    def __init__(self, serial=0x1234ABCD):
        self.serial = serial
        self.seq = 0
        self.out = bytearray()

    def page(self, segments, body, granule, flags=0):
        h = bytearray(b"OggS" + bytes([0, flags]) + struct.pack("<qII", granule, self.serial, self.seq) + bytes(4))
        h += bytes([len(segments)]) + bytes(segments)
        page = bytearray(h + body)
        crc = ogg_crc(bytes(page))
        page[22:26] = le32(crc)
        self.seq += 1
        start = len(self.out)
        self.out += page
        return start + len(h)  # where the body starts

    def packet(self, data, granule, max_body=255 * 255, first_flags=0, segs_per_page=255):
        """One packet over as many pages as it takes. Returns [(file offset,
        packet offset, length)] of each page's piece."""
        pieces = []
        pos = 0
        cont = False
        lacing = []
        n = len(data)
        while True:
            seg = []
            take = 0
            while len(seg) < segs_per_page and take < max_body:
                left = n - pos - take
                if left >= 255:
                    seg.append(255)
                    take += 255
                else:
                    seg.append(left)
                    take += left
                    break
            done = seg[-1] < 255
            flags = (1 if cont else 0) | (first_flags if not cont else 0)
            at = self.page(seg, data[pos:pos + take], granule if done else -1, flags)
            pieces.append((at, pos, take))
            pos += take
            cont = True
            if done:
                return pieces


def opus_head(preskip=312, channels=2):
    return b"OpusHead" + bytes([1, channels]) + struct.pack("<HIhB", preskip, 48000, 0, 0)


def opus_file(comments, vendor="corpus", segs_per_page=255, seconds=5, preskip=312):
    """An Opus file: OpusHead, OpusTags (comments: str or bytes), one audio
    page. Returns the bytes and, per comment, the file offset of its value's
    first byte (for pictures)."""
    o = Ogg()
    o.page([19], opus_head(preskip), 0, flags=2)
    tags = bytearray(b"OpusTags" + le32(len(vendor)) + vendor.encode() + le32(len(comments)))
    value_at = []
    for c in comments:
        b = c if isinstance(c, bytes) else c.encode("utf-8")
        tags += le32(len(b))
        eq = b.index(b"=")
        value_at.append(len(tags) + eq + 1)
        tags += b
    pieces = o.packet(bytes(tags), 0, segs_per_page=segs_per_page)

    def file_offset(packet_offset):
        for at, start, take in pieces:
            if start <= packet_offset < start + take:
                return at + packet_offset - start
        at, start, take = pieces[-1]
        return at + take  # an empty value at the packet's end

    # One audio packet (a silent CELT frame's TOC and a few bytes), its
    # page's granule the length.
    o.page([3], b"\xF8\xFF\xFE", 48000 * seconds + preskip, flags=4)
    return bytes(o.out), [file_offset(v) for v in value_at]


# ---------------------------------------------------------------------------
# The corpus
# ---------------------------------------------------------------------------
CORPUS = {}  # name -> (bytes, pictures, note)


def add(name, data, pics, note):
    CORPUS[name] = (data, pics, note)


def mp3(name, note, tag=None, v1=None, ape_tag=None, audio=None, pre=b"", tags=None, tag_kw=None):
    """An MP3: `pre` bytes, the ID3v2 tags, the audio, an APE tag, an ID3v1."""
    out = bytearray(pre)
    pics = []
    for t in (tags if tags is not None else ([tag] if tag else [])):
        b, p = t.build(start=len(out), **(tag_kw or {}))
        out += b
        pics += p
    out += audio if audio is not None else mp3_audio()
    if ape_tag:
        b, p = ape_tag
        base = len(out)
        out += b
        pics += [{"offset": base + o, "length": n, "coding": 3, "fnv": digest(d)} for o, n, d in p]
    if v1:
        out += v1
    add(name, bytes(out), pics, note)


def build():
    CORPUS.clear()
    MB_ALBUM = "0f9e8d7c-6b5a-4938-8271-605f4e3d2c1b"
    MB_REC = "1a2b3c4d-5e6f-4a7b-8c9d-0e1f2a3b4c5d"

    # ---- ID3v2.4, every field ----
    t = Tag(4)
    t.frame("TIT2", text_frame(3, "Night Ferry"))
    t.frame("TPE1", text_frame(3, "Alpha Quartet"))
    t.frame("TALB", text_frame(3, "Harbour Lights"))
    t.frame("TPE2", text_frame(3, "Alpha Quartet"))
    t.frame("TCON", text_frame(3, "Ambient"))
    t.frame("TCOM", text_frame(3, "Bea Example"))
    t.frame("TRCK", text_frame(3, "3/12"))
    t.frame("TPOS", text_frame(3, "1/2"))
    t.frame("TDRC", text_frame(3, "2004-05-01"))
    t.frame("TBPM", text_frame(3, "120.5"))
    t.frame("TKEY", text_frame(3, "Am"))
    t.frame("TCMP", text_frame(3, "1"))
    t.frame("TSOT", text_frame(3, "Night Ferry, The"))
    t.frame("TSOP", text_frame(3, "Quartet, Alpha"))
    t.frame("TSOA", text_frame(3, "Harbour Lights"))
    t.frame("TSO2", text_frame(3, "Quartet, Alpha"))
    t.frame("TXXX", txxx(3, "MusicBrainz Album Id", MB_ALBUM))
    t.frame("UFID", ufid("http://musicbrainz.org", MB_REC))
    t.frame("TXXX", txxx(3, "REPLAYGAIN_TRACK_GAIN", "-6.785 dB"))
    t.frame("TXXX", txxx(3, "REPLAYGAIN_ALBUM_GAIN", "1.005 dB"))
    t.frame("TXXX", txxx(3, "REPLAYGAIN_TRACK_PEAK", "0.988567"))
    t.frame("TXXX", txxx(3, "REPLAYGAIN_ALBUM_PEAK", "7"))
    t.frame("APIC", None, picture=apic(4, 3, "image/jpeg", jpeg(1)))
    mp3("v24_full.mp3", "ID3v2.4, UTF-8, every field of 5.3; 2.18's gains and peaks", t)

    # ---- ID3v2.3, UTF-16 with a BOM, TYER + TDAT ----
    t = Tag(3)
    t.frame("TIT2", text_frame(1, "Café Lumière"))
    t.frame("TPE1", text_frame(1, "Bravo Band"))
    t.frame("TALB", text_frame(1, "Second Light"))
    t.frame("TYER", text_frame(0, "1999"))
    t.frame("TDAT", text_frame(0, "0105"))
    t.frame("TRCK", text_frame(0, "7"))
    t.frame("TCON", text_frame(0, "(17)Rock"))
    t.frame("APIC", None, picture=apic(3, 3, "image/png", png(2), desc="front", enc=1))
    mp3("v23_utf16.mp3", "ID3v2.3, UTF-16 with a BOM; TYER with TDAT; TCON '(17)Rock'", t)

    # ---- ID3v2.2 ----
    t = Tag(2)
    t.frame("TT2", text_frame(0, "Old Format"))
    t.frame("TP1", text_frame(0, "Charlie Trio"))
    t.frame("TAL", text_frame(0, "Twenty Two"))
    t.frame("TYE", text_frame(0, "1987"))
    t.frame("TRK", text_frame(0, "02/09"))
    t.frame("TCO", text_frame(0, "(4)(17)"))
    t.frame("PIC", None, picture=apic(2, 3, "image/jpeg", jpeg(3)))
    mp3("v22.mp3", "ID3v2.2 three-letter ids; TCON '(4)(17)'; a PIC", t)

    # ---- several values ----
    t = Tag(4)
    t.frame("TIT2", text_frame(3, "", "Many Values"))
    t.frame("TPE1", text_frame(3, "Delta One", "Echo Two", "Delta One"))
    t.frame("TPE2", text_frame(3, "Delta One", "Echo Two"))
    t.frame("TCOM", text_frame(3, "Fox Writer", "Golf Writer"))
    t.frame("TCON", text_frame(3, "Rock", "Pop", "Rock"))
    mp3("multivalue.mp3", "NUL-separated values: lists, a repeat dropped, the first non-empty title", t)

    # ---- UTF-16 several values, each with its BOM ----
    t = Tag(3)
    t.frame("TPE1", text_frame(1, "Hotel", "India"))
    t.frame("TIT2", text_frame(2, "Big Endian"))
    mp3("utf16_values.mp3", "UTF-16 values each with a BOM; UTF-16BE (in a v2.3 tag)", t)

    # ---- ID3v1 filling a blank ID3v2 field ----
    t = Tag(3)
    t.frame("TIT2", text_frame(0, "   "))
    t.frame("TPE1", text_frame(0, "Juliet Singer"))
    v1 = id3v1("Filled Title", "Other Artist", "Filled Album", "1994", track=6, genre=13)
    mp3("v1_fill.mp3", "ID3v1 fills a blank title, the absent album, year, track, genre; not the artist", t, v1=v1)

    # ---- ID3v1 alone ----
    v1 = id3v1("Plain Old Tag   ", "Kilo Artist", "Lima Album", "1987", track=5, genre=17)
    mp3("v1_only.mp3", "ID3v1 alone, padded with spaces: trimmed", v1=v1)

    v1 = id3v1("Year Too Small", "Kilo Artist", "", "0999", genre=191)
    mp3("v1_year.mp3", "ID3v1: year 0999 is no year (lofty writes it 999); genre 191; no track", v1=v1)

    # ---- APE ----
    cover = jpeg(4)
    back = png(5)
    a = ape([
        ("Title", "Mike Tune".encode(), 0),
        ("Artist", "November\x00Oscar".encode(), 0),
        ("Album", "Papa Album".encode(), 0),
        ("Album Artist", "Quebec".encode(), 0),
        ("ALBUMARTIST", "Quebec".encode(), 0),
        ("Genre", "Jazz".encode(), 0),
        ("Composer", "Romeo".encode(), 0),
        ("Track", "4/9".encode(), 0),
        ("Disc", " 2 / 3".encode(), 0),
        ("Year", "2001-02-03".encode(), 0),
        ("Compilation", "1".encode(), 0),
        ("REPLAYGAIN_TRACK_GAIN", "-6.5 DB".encode(), 0),
        ("REPLAYGAIN_TRACK_PEAK", "0.5".encode(), 0),
        ("MUSICBRAINZ_ALBUMID", (" " + MB_ALBUM + " ").encode(), 0),
        ("Cover Art (Back)", b"back.png\x00" + back, 1),
        ("Cover Art (Front)", b"front.jpg\x00" + cover, 1),
        ("Title", "Mike Tune Again".encode(), 0),
    ])
    mp3("ape_only.mp3", "APEv2 alone: a repeated key's last item; values on NUL; 'Track' 4/9; covers", ape_tag=a)

    a = ape([("Title", "From APE".encode(), 0), ("Artist", "Sierra".encode(), 0)])
    v1 = id3v1("From ID3v1", "Tango", "Uniform")
    mp3("ape_v1.mp3", "APE beside ID3v1, no ID3v2: ID3v1 is the tag (lofty's order)", ape_tag=a, v1=v1)

    a = ape([("cover art (front)", b"x.jpg\x00" + jpeg(6), 1), ("Title", "Lower Key".encode(), 0)])
    mp3("ape_lower_key.mp3", "an APE cover under a lowercase key (mStream reads it)", ape_tag=a)

    # ---- text repairs ----
    t = Tag(4)
    t.frame("TIT2", b"\x03" + b"Caf\xe9 \xf0\x9f\x8e Bad")
    t.frame("TALB", b"\x03" + b"\xc3\xa9t\xc3\xa9 \xed\xa0\x80 \xe0\x80\xaf")
    mp3("bad_utf8.mp3", "UTF-8 that isn't: one U+FFFD per maximal invalid subpart", t)

    t = Tag(3)
    t.frame("TIT2", b"\x01\xff\xfe" + "Odd".encode("utf-16-le") + b"\x41")
    t.frame("TPE1", b"\x02" + "Even".encode("utf-16-be") + b"\x42")
    mp3("odd_utf16.mp3", "an odd-length UTF-16 frame's stray byte: U+FFFD (with a BOM, and big-endian)", t)

    # Files lofty fails on: the reference record is UNREADABLE, and the
    # device reads them itself (2.9); its reading is pinned in the test.
    t = Tag(3)
    t.frame("TIT2", text_frame(0, "Lone Surrogate"))
    t.frame("TPE1", b"\x01\xff\xfe" + "A".encode("utf-16-le") + b"\x00\xd8" + "B".encode("utf-16-le"))
    mp3("lofty_fails_surrogate.mp3", "an unpaired surrogate: lofty fails the file; U+FFFD here", t)

    t = Tag(3)
    t.frame("TIT2", b"\x01" + "No BOM".encode("utf-16-le"))
    mp3("lofty_fails_nobom.mp3", "UTF-16 with no BOM: lofty fails the file; little-endian here", t)

    t = Tag(3)
    t.frame("TIT2", b"\x00" + bytes([0x80, 0x93, 0x9f, 0x20, 0xe9, 0x41]))
    t.frame("TPE1", b"\x00Zulu\x00\x00")
    mp3("latin1.mp3", "ISO-8859-1 bytes 0x80-0x9F stay U+0080-U+009F; trailing NULs trimmed", t)

    t = Tag(4)
    t.frame("TIT2", text_frame(3, "Emoji \U0001F3B5"))
    t.frame("TPE1", text_frame(1, "Surrogate \U0001F3B5 Pair"))
    t.frame("TALB", text_frame(3, "Tab\there\x01x"))
    mp3("unicode.mp3", "a code point past the BMP in UTF-8 and UTF-16; control characters to spaces", t)

    # ---- unsynchronisation ----
    t = Tag(4, flags=0x80)
    t.frame("TIT2", unsync(b"\x00" + bytes([0x41, 0xFF, 0xE5, 0x42, 0xFF])))
    t.frame("TPE1", unsync(text_frame(0, "Victor")))
    data = jpeg_ff(7)
    head, _ = apic(4, 3, "image/jpeg", data)
    stored = unsync(head + data)
    t.frame("APIC", stored, fflags=0)
    mp3("v24_unsync_tag.mp3", "v2.4 tag-level unsynchronisation, undone frame by frame; the picture's coding 1", t)
    # The picture's anchor: the stored bytes after the stored head.
    b, _ = t.build()
    head_stored = unsync(head)
    at = b.index(stored) + len(head_stored)
    CORPUS["v24_unsync_tag.mp3"][1].append(
        {"offset": at, "length": len(stored) - len(head_stored), "coding": 1, "fnv": digest(data)})

    t = Tag(4)
    data = jpeg_ff(8)
    head, _ = apic(4, 3, "image/jpeg", data)
    stored = unsync(head + data)
    t.frame("TIT2", unsync(b"\x00" + bytes([0x57, 0xFF, 0xE8, 0x58])), fflags=0x0002)
    t.frame("APIC", syncsafe(len(head + data)) + stored, fflags=0x0003)
    t.frame("TALB", text_frame(3, "Whiskey"))
    b, _ = t.build()
    at = b.index(stored) + len(unsync(head))
    mp3("v24_frame_unsync.mp3", "v2.4 frame-level unsynchronisation with a data length indicator", t)
    CORPUS["v24_frame_unsync.mp3"][1].append(
        {"offset": at, "length": len(stored) - len(unsync(head)), "coding": 1, "fnv": digest(data)})

    t = Tag(3, flags=0x80)
    data = jpeg_ff(9)
    t.frame("TIT2", text_frame(0, "X-Ray \xff\xe9 Unsync \xff"))
    t.frame("APIC", None, picture=apic(3, 3, "image/jpeg", data))
    t.frame("TALB", text_frame(0, "After Picture"))
    mp3("v23_unsync.mp3", "v2.3 whole-tag unsynchronisation: read through; the picture's stored length", t,
        tag_kw={"unsync_tag": True})

    # ---- sizes ----
    t = Tag(4)
    long_txxx = txxx(3, "COMMENTARY", "y" * 200)
    t.frame("TXXX", long_txxx, plain_size=True)
    t.frame("TIT2", text_frame(3, "Yankee After Plain Size"))
    data = jpeg(10, 300)
    t.frame("APIC", None, plain_size=True, picture=apic(4, 3, "image/jpeg", data))
    t.frame("TALB", text_frame(3, "Zulu Album"))
    mp3("nonsyncsafe.mp3", "v2.4 sizes written as plain integers: re-read where they land on a frame", t)

    t = Tag(3)
    t.frame("APIC", None, picture=apic(3, 3, "image/jpeg", jpeg(11, 20000)))
    t.frame("TIT2", text_frame(0, "Behind The Picture"))
    t.frame("TPE1", text_frame(0, "Alpha Two"))
    mp3("picture_first.mp3", "a 20 KB picture before the text frames (5% of MP3s)", t)

    t = Tag(3)
    t.frame("TIT2", text_frame(0, "Cut Short"))
    t.frame("TALB", text_frame(0, "Album Runs Past"), size=4000)
    mp3("frame_overrun.mp3", "a frame running past the tag's end: cut there", t, tag_kw={"padding": 0})

    # ---- pictures ----
    t = Tag(3)
    raw = jpeg(12, 200)
    comp = zlib.compress(raw)
    t.frame("APIC", be32(len(apic(3, 3, "image/jpeg", raw)[0] + raw)) +
            zlib.compress(apic(3, 3, "image/jpeg", raw)[0] + raw), fflags=0x0080)
    t.frame("APIC", None, picture=apic(3, 4, "image/png", png(13)))
    t.frame("TIT2", text_frame(0, "Compressed Front"))
    mp3("compressed_apic.mp3", "a compressed APIC (front) is passed over: the back cover is elected", t)
    _ = comp

    t = Tag(4)
    enc_body = b"\x80" + syncsafe(150) + bytes(range(150))
    t.frame("APIC", enc_body, fflags=0x0005)
    t.frame("APIC", None, picture=apic(4, 0, "image/jpeg", jpeg(14)))
    t.frame("APIC", None, picture=apic(4, 6, "image/jpeg", jpeg(15)))
    t.frame("TIT2", text_frame(3, "Encrypted Front"))
    mp3("encrypted_apic.mp3", "an encrypted APIC is passed over; no front: the first picture", t)

    t = Tag(3)
    t.frame("APIC", None, picture=apic(3, 0, "image/jpeg", jpeg(16)))
    t.frame("APIC", None, picture=apic(3, 4, "image/png", png(17)))
    t.frame("APIC", None, picture=apic(3, 3, "IMAGE/JPG", jpeg(18)))
    t.frame("APIC", None, picture=apic(3, 3, "image/png", png(19)))
    t.frame("APIC", apic(3, 3, "image/jpeg", b"")[0])
    t.frame("TIT2", text_frame(0, "Front Third"))
    mp3("apic_election.mp3", "the first front cover among several pictures; an empty one unseen", t)

    # ---- tags and frames that repeat ----
    t1 = Tag(3)
    t1.frame("TIT2", text_frame(0, "First Tag Title"))
    t1.frame("TALB", text_frame(0, "First Tag Album"))
    t1.frame("TPE1", text_frame(0, "First Tag Artist"))
    t2 = Tag(4)
    t2.frame("TIT2", text_frame(3, "Second Tag Title"))
    t2.frame("TPE1", text_frame(3, ""))
    mp3("two_tags.mp3", "two ID3v2 tags: frame by frame, the later replacing (an empty one too)", tags=[t1, t2])

    t = Tag(3)
    t.frame("TIT2", text_frame(0, "One"))
    t.frame("TIT2", text_frame(0, "Two"))
    t.frame("TALB", text_frame(0, "Kept Album"))
    t.frame("TALB", text_frame(0, ""))
    t.frame("TXXX", txxx(0, "replaygain_track_gain", "-3 dB"))
    t.frame("TXXX", txxx(0, "REPLAYGAIN_TRACK_GAIN", "-5 dB"))
    t.frame("TXXX", txxx(0, "replaygain_track_gain", "-4 dB"))
    mp3("dup_frames.mp3", "a repeated frame replaces; an empty one doesn't; TXXX by its exact description", t)

    t = Tag(4)
    t.frame("TXXX", txxx(3, "ALBUM ARTIST", "   "))
    t.frame("TXXX", txxx(3, "AlbumArtist", "Bravo Two", "Charlie Two"))
    t.frame("TIT2", text_frame(3, "No TPE2"))
    mp3("txxx_albumartist.mp3", "no TPE2: the first TXXX album artist that isn't blank, whole", t)

    # ---- numbers and dates ----
    for i, (trck, tpos, note) in enumerate([
        ("07/12", " 1 / 2 ", "TRCK '07/12', TPOS ' 1 / 2 ' (trimmed)"),
        ("5/x", "A1", "TRCK '5/x' and TPOS 'A1': none"),
        ("5\x00abc", "+3", "TRCK '5\\0abc': lofty's map makes 5 the total; TPOS '+3' (Rust's u32)"),
        ("0", "99999", "TRCK '0' (none, though lofty has one: no ID3v1 fill); TPOS 99999 saturates"),
    ]):
        t = Tag(4)
        t.frame("TIT2", text_frame(3, "Numbers %d" % i))
        t.frame("TRCK", text_frame(3, trck))
        t.frame("TPOS", text_frame(3, tpos))
        v1 = id3v1("", "", "", "", track=9) if i == 3 else None
        mp3("trck_%d.mp3" % i, note, t, v1=v1)

    for i, (ver, frames, note) in enumerate([
        (4, [("TDRC", "2010-13-01")], "TDRC month 13: fails lofty's verify(), no year"),
        (4, [("TDRC", "  1999")], "TDRC after spaces"),
        (4, [("TDRC", "2004/05/06")], "TDRC with '/': lofty's timestamp parse fails, no year"),
        (3, [("TYER", "2004-05-01")], "v2.3 TYER with a month: lofty drops it, no year"),
        (3, [("TYER", "1998"), ("TDAT", "3113")], "v2.3 TYER with a TDAT of month 13: no year"),
        (4, [("TDRC", "20040501")], "TDRC with no separators"),
        (4, [("TDRC", "2015-07"), ("TDRC", "2016")], "two TDRC frames: the first in the list"),
    ]):
        t = Tag(ver)
        t.frame("TIT2", text_frame(0, "Date %d" % i))
        for fid, s in frames:
            t.frame(fid, text_frame(0, s))
        mp3("date_%d.mp3" % i, note, t)

    # ---- number strings (2.18) in the ID3v2 fields ----
    for i, (gain, peak, bpm, key, comp, note) in enumerate([
        ("-6.5 DB", "-0.5", "19.5", "  f#m ", "true", "gain -6.5 DB, a negative peak, BPM 19.5, key 'f#m', 'true'"),
        ("-400 dB", "7", "300.5", "Bbm", "FALSE", "gain -400 dB, peak 7 saturates, BPM 300.5, 'FALSE'"),
        ("inf", "1e2", "0x78", "Amaj", "yes", "inf, 1e2, 0x78, 'yes' (not said)"),
        ("1e2", "0.0001", "120 BPM", "nope", "0", "1e2, 120 BPM, an unknown key, '0'"),
    ]):
        t = Tag(4)
        t.frame("TIT2", text_frame(3, "Rules %d" % i))
        t.frame("TXXX", txxx(3, "REPLAYGAIN_TRACK_GAIN", gain))
        t.frame("TXXX", txxx(3, "REPLAYGAIN_ALBUM_PEAK", peak))
        t.frame("TBPM", text_frame(3, bpm))
        t.frame("TKEY", text_frame(3, key))
        t.frame("TCMP", text_frame(3, comp))
        mp3("numbers_%d.mp3" % i, note, t)

    for i, (tcon, note) in enumerate([
        ("(17)\x00Pop", "'(17)\\0Pop': a NUL-ended segment whole"),
        ("RX\x00CR\x00255", "RX, CR, a number past the table"),
        ("((I think)Rock", "a bracketed refinement"),
        ("\x00\x00Folk\x00\x00", "empty values and trailing NULs"),
    ]):
        t = Tag(4)
        t.frame("TIT2", text_frame(3, "Genre %d" % i))
        t.frame("TCON", text_frame(3, tcon))
        mp3("tcon_%d.mp3" % i, note, t)

    # ---- the extended header ----
    t = Tag(3, flags=0x40)
    t.frame("TIT2", text_frame(0, "Hidden By Header"))
    v1 = id3v1("From V1 Instead", "Alpha V1")
    mp3("v23_exthdr.mp3", "a v2.3 extended header: lofty reads no frame; ID3v1 fills", t, v1=v1,
        tag_kw={"ext": be32(6) + bytes(2) + be32(0)})

    t = Tag(4, flags=0x40)
    t.frame("TIT2", text_frame(3, "Read Past Header"))
    mp3("v24_exthdr.mp3", "a v2.4 extended header", t, tag_kw={"ext": syncsafe(6) + b"\x01\x00"})

    # ---- where the tag is ----
    t = Tag(3)
    t.frame("TIT2", text_frame(0, "In The Junk"))
    mp3("junk_id3.mp3", "an ID3v2 tag after junk (lofty's max_junk_bytes)", t, pre=bytes([0x55] * 100))

    t = Tag(3)
    t.frame("TIT2", text_frame(0, "After Zeros"))
    mp3("leading_zeros.mp3", "zero bytes before the tag (lofty skips them)", t, pre=bytes(37))

    # ---- URL frames ----
    t = Tag(3)
    t.frame("WOAR", b"\x00http://a.example")
    t.frame("TIT2", text_frame(0, "After A URL"))
    mp3("url_nul.mp3", "a URL frame with a NUL in front: the repair pass drops it", t)

    t1 = Tag(3)
    t1.frame("TIT2", text_frame(0, "First"))
    t2 = Tag(3)
    t2.frame("WOAR", b"http://b.example\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00")
    t2.frame("TALB", text_frame(0, "Lost After URL"))
    mp3("url_misread.mp3", "lofty reads a URL to its NUL and the rest as the next header", tags=[t1, t2])

    # ---- edges ----
    t = Tag(4)
    t.frame("TIT2", text_frame(3, "T" * 300))
    t.frame("TPE1", text_frame(3, *[("%d" % k) * 204 for k in range(5)]))
    t.frame("TCOM", text_frame(3, *(["c%02d" % k for k in range(20)])))
    mp3("long_values.mp3", "a 300-byte title cut; five 204-byte artists keep four; 20 composers keep 16", t)

    mp3("no_tags.mp3", "no tag at all: NO_TAGS")
    t = Tag(4)
    mp3("empty_id3.mp3", "an ID3v2 tag with only padding: NO_TAGS", t)
    mp3("cbr.mp3", "no Xing header: the length from the bitrate", audio=mp3_audio(xing=False, frames=40))
    t = Tag(4)
    t.frame("TIT2", text_frame(3, "MPEG Two"))
    mp3("mpeg2.mp3", "MPEG-2 Layer III", t, audio=mp3_mpeg2())

    # ---- FLAC ----
    items = [
        "TITLE=Lantern Song", "ARTIST=Mike Duo", "ARTIST=November", "ALBUM=Quiet Roads",
        "ALBUMARTIST=Mike Duo", "ALBUM ARTIST=Mike Duo", "GENRE=Folk", "GENRE=Acoustic", "COMPOSER=Oscar Pen",
        "DATE=2003-04-05", "TRACKNUMBER=3/12", "DISCNUMBER=1/2", "BPM=96", "INITIALKEY=8B",
        "COMPILATION=TRUE", "REPLAYGAIN_TRACK_GAIN=-6.785 dB", "REPLAYGAIN_ALBUM_GAIN=1.005 dB",
        "REPLAYGAIN_TRACK_PEAK=0.988567", "REPLAYGAIN_ALBUM_PEAK=1", "MUSICBRAINZ_ALBUMID=" + MB_ALBUM,
        "MUSICBRAINZ_TRACKID=" + MB_REC, "TITLESORT=Lantern Song", "ARTISTSORT=Duo, Mike",
        "ALBUMSORT=Quiet Roads", "ALBUMARTISTSORT=Duo, Mike",
    ]
    b, p = flac([(4, vorbis_comment(items)), (6, flac_picture(3, "image/jpeg", jpeg(20)))])
    add("flac_full.flac", b, p, "FLAC: every field; ALBUMARTIST and ALBUM ARTIST the same value once")

    t = Tag(3)
    t.frame("TIT2", text_frame(0, "From Front ID3"))
    tb, _ = t.build()
    b, p = flac([(4, vorbis_comment(["TITLE=From Comments"]))], id3=tb)
    add("flac_id3_and_comments.flac", b, p, "a FLAC with a front ID3v2 and comments: the comments")

    t = Tag(3)
    t.frame("TIT2", text_frame(0, "Only Front ID3"))
    t.frame("TPE1", text_frame(0, "Papa Front"))
    tb, _ = t.build()
    b, p = flac([(1, bytes(10))], id3=tb)
    add("flac_id3_only.flac", b, p, "a FLAC with a front ID3v2 and no comment block: the ID3v2")

    t = Tag(3)
    t.frame("TIT2", text_frame(0, "Hidden By Picture"))
    tb, _ = t.build()
    b, p = flac([(6, flac_picture(0, "image/png", png(21)))], id3=tb)
    add("flac_id3_picture.flac", b, p, "a FLAC with a front ID3v2 and a PICTURE block: no title")

    items = [b"TITLE=Bad \xff UTF-8", b"TITLE=Good Title", b"ARTIST=Quebec\x00\x00", b"ALBUM=A\x00B"]
    b, p = flac([(4, vorbis_comment(items))])
    add("flac_text.flac", b, p, "a value that isn't UTF-8 is dropped; trailing NULs trimmed; an inner NUL kept")

    for i, (items, note) in enumerate([
        (["TRACKNUMBER= 3", "TRACKTOTAL=12", "DISCNUMBER=x/2"], "TRACKNUMBER ' 3' (parse_num_of), 'x/2'"),
        (["TRACKNUMBER=4", "TRACKNUMBER=5/10", "TOTALTRACKS=11"], "a later TRACKNUMBER replaces"),
        (["TRACKTOTAL=8", "TRACKNUMBER=2/9", "DISCNUMBER=2", "DISCTOTAL=x", "TOTALDISCS=4"],
         "TRACKNUMBER's total replaces TRACKTOTAL; DISCTOTAL 'x' first"),
        (["YEAR=1999", "DATE=2005"], "YEAR before DATE"),
        (["DATE=　 2005-x"], "DATE after Unicode whitespace"),
        (["DATE=199"], "DATE with three digits: none"),
    ]):
        b, p = flac([(4, vorbis_comment(["TITLE=Vorbis %d" % i] + items))])
        add("flac_numbers_%d.flac" % i, b, p, note)

    for i, (vals, note) in enumerate([
        (["REPLAYGAIN_TRACK_GAIN=-6.5 DB", "REPLAYGAIN_ALBUM_GAIN=-400 dB", "BPM=19.5",
          "REPLAYGAIN_TRACK_PEAK=-0.5", "KEY=f#m"], "2.18: -6.5 DB, -400 dB, 19.5; KEY"),
        (["REPLAYGAIN_TRACK_GAIN=inf", "REPLAYGAIN_ALBUM_GAIN=1e2", "BPM=300.5", "REPLAYGAIN_ALBUM_PEAK=7"],
         "2.18: inf, 1e2, 300.5, peak 7"),
        (["BPM=0x78", "BPM=120", "INITIALKEY=Unknown", "KEY=Am"], "2.18: 0x78 (the first BPM); the first key"),
        (["BPM=120 BPM", "COMPILATION=0"], "2.18: 120 BPM; COMPILATION 0"),
    ]):
        b, p = flac([(4, vorbis_comment(["TITLE=Rules %d" % i] + vals))])
        add("flac_rules_%d.flac" % i, b, p, note)

    mbp_data = jpeg(22)
    head, data = flac_picture(3, "image/jpeg", mbp_data)
    mbp = base64.b64encode(head + data).decode()
    block_head, block_data = flac_picture(3, "image/png", png(23))
    comment = vorbis_comment(["TITLE=Two Pictures", "METADATA_BLOCK_PICTURE=" + mbp])
    b, p = flac([(6, (block_head, block_data)), (4, comment)])
    # The comment picture's anchor: its value's first base64 character.
    at = b.index(mbp.encode())
    p.append({"offset": at, "length": len(mbp), "coding": 2, "fnv": digest(mbp_data)})
    add("flac_pictures.flac", b, p, "a comment picture before a PICTURE block, whatever the file order")

    b, p = flac([(4, vorbis_comment(["TITLE=First Block", "ARTIST=Romeo"])),
                 (4, vorbis_comment(["TITLE=Second Block"]))])
    add("flac_two_comments.flac", b, p, "two comment blocks: the second (lofty, relaxed)")

    # ---- Opus ----
    b, offs = opus_file(["TITLE=Opus Track", "ARTIST=Sierra Voice", "ALBUM=Ogg Album", "DATE=2021",
                         "TRACKNUMBER=2", "R128_TRACK_GAIN=-1312", "R128_ALBUM_GAIN=32",
                         "REPLAYGAIN_TRACK_GAIN=-9.00 dB", "REPLAYGAIN_TRACK_PEAK=0.9"])
    add("opus_basic.opus", b, [], "Opus: R128 gains (the REPLAYGAIN gain ignored); a peak")

    b, offs = opus_file(["TITLE=R128 Edges", "R128_TRACK_GAIN=0", "R128_ALBUM_GAIN=-5888"])
    add("opus_r128.opus", b, [], "R128 0 and -5888")

    pic = jpeg(24, 30000)
    head, data = flac_picture(3, "image/jpeg", pic)
    mbp = base64.b64encode(head + data).decode()
    b, offs = opus_file(["TITLE=Picture Across Pages", "METADATA_BLOCK_PICTURE=" + mbp, "ARTIST=Tango Ogg"],
                        segs_per_page=40)
    add("opus_picture.opus", b, [{"offset": offs[1], "length": len(mbp), "coding": 2, "fnv": digest(pic)}],
        "an Opus picture across several pages; a comment after it")

    long_title = "TITLE=" + "Long Ogg Title " * 40
    b, offs = opus_file([long_title, "ARTIST=Uniform", "ALBUM=Split Across"], segs_per_page=2)
    add("opus_pages.opus", b, [], "OpusTags across pages of two segments: values split by page headers")


GITATTRIBUTES = """# The files are compared byte for byte (the audio, anchors.json, the
# README, expected.json): never convert their line ends.
* -text
"""

README = """# Tag reader parity corpus

SPDX-License-Identifier: CC0-1.0. To the extent possible under law, the
authors have waived all copyright and related or neighboring rights to the
files in this folder (the CC0 1.0 Universal dedication,
https://creativecommons.org/publicdomain/zero/1.0/), so mstream-terminal can
copy them (docs/METADATA.md part 7, U18, proposed). The player's code stays
GPL-3.0-or-later.

docs/METADATA.md 2.17, item 3: synthetic audio files, each a few of part 5's
reading rules, and the record each must give.

- The files: made by `tools/tag_corpus.py` (`--check` fails if one here
  differs). Every name in them is made up; the audio is a few silent
  frames, the pictures a few bytes that only start like a JPEG or a PNG.
- `anchors.json`: per file, where each embedded picture's bytes are (2.6.4's
  anchor: offset, stored length, picCoding) and the FNV-1a 64 of its image data.
- `expected.json`: the record each file gives, made by the reference reader
  `tools/tagref` (lofty 0.25, mStream's rust-parser selection rules, part 5
  and 2.3.6), with the elected picture's anchor from anchors.json. A reader
  of the contract matches it field for field, the length within 100 ms
  (lofty doesn't trim the MP3 encoder delay; the device does).

The player's test: `test/test_tag_scan` (TagScan against expected.json,
then mutated copies of every file for the fuzz pass).

Where the device's reader knowingly differs from lofty (all rare, and none
of these in this corpus): a compressed ID3v2 text frame (lofty inflates it),
the repair pass's fallback for a tag with no padding to grow into, COVERART,
an APE tag at the head of an MP3, a Lyrics3 block before ID3v1, the base64
of an Opus picture past its head, a frame no field comes from repeated
within one tag (lofty's list may replace it; the device only counts it), and
a Vorbis value whose bytes stop being UTF-8 after its first 4 KB. Then the
files lofty fails on (the two `lofty_fails_*` here): the reference reads them
as UNREADABLE and the device reads them itself (2.9).
"""


def outputs():
    build()
    files = {name: data for name, (data, _, _) in CORPUS.items()}
    anchors = {name: {"note": note, "pictures": pics} for name, (_, pics, note) in sorted(CORPUS.items())}
    files["anchors.json"] = (json.dumps(anchors, indent=1, sort_keys=True) + "\n").encode()
    files["README.md"] = README.encode()
    files[".gitattributes"] = GITATTRIBUTES.encode()
    return files


# ---------------------------------------------------------------------------
# A random corpus, for a differential run against the reference reader (not
# kept: `--random N --out DIR`, then tools/tagref --dir DIR, then the player's
# reader over the same files). Every feature of the fixed corpus, mixed at
# random, minus the reader's known differences (a compressed text frame).
# ---------------------------------------------------------------------------
VOCAB = [
    "Alpha", "Bravo Charlie", "", " ", "  padded  ", "Tab\there", "ctl\x01x", "Café", "Ünïcödé",
    "日本語", "note \U0001F3B5", "x" * 300, "A/B", "3", "03", "3/12", " 3 / 12 ", "5/x", "A1", "+3",
    "0", "99999", "-1", "1999", "2004-05-01", "2010-13-01", "  2001", "1959.", "2004/05", "20040501", "2004-5",
    "120.5", "19.5", "300.5", "0x78", "120 BPM", "-6.785 dB", "1.005 dB", "inf", "1e2", "-400 dB", "0.988567", "7",
    "-0.5", "Am", "8B", "f#m", "Unknown", "1", "true", "TRUE", "false", "yes", "(17)", "(17)Rock", "(4)(17)", "RX",
    "CR", "((I think)x", "0f9e8d7c-6b5a-4938-8271-605f4e3d2c1b", " 1a2b3c4d-5e6f-4a7b-8c9d-0e1f2a3b4c5d ",
    "ÿé latin", "\u0080\u0093", "  　 ", "Delta One", "Echo Two",
]
V2_IDS = ["TT2", "TP1", "TAL", "TP2", "TCO", "TCM", "TST", "TSP", "TSA", "TS2", "TRK", "TPA", "TYE", "TBP", "TKE",
          "TCP", "TRD"]
V3_IDS = ["TIT2", "TPE1", "TALB", "TPE2", "TCON", "TCOM", "TSOT", "TSOP", "TSOA", "TSO2", "TRCK", "TPOS", "TYER",
          "TDAT", "TIME", "TDRC", "TBPM", "TKEY", "TCMP", "TLEN"]
V4_IDS = ["TIT2", "TPE1", "TALB", "TPE2", "TCON", "TCOM", "TSOT", "TSOP", "TSOA", "TSO2", "TRCK", "TPOS", "TDRC",
          "TBPM", "TKEY", "TCMP", "TYER", "TLEN"]
TXXX_DESC = ["REPLAYGAIN_TRACK_GAIN", "replaygain_track_gain", "REPLAYGAIN_ALBUM_GAIN", "REPLAYGAIN_TRACK_PEAK",
             "REPLAYGAIN_ALBUM_PEAK", "MusicBrainz Album Id", "MUSICBRAINZ ALBUM ID", "ALBUMARTIST", "ALBUM ARTIST",
             "AlbumArtist", "COMMENTARY", ""]
VORBIS_KEYS = ["TITLE", "ARTIST", "ALBUM", "ALBUMARTIST", "ALBUM ARTIST", "GENRE", "COMPOSER", "DATE", "YEAR",
               "TRACKNUMBER", "TRACKTOTAL", "TOTALTRACKS", "DISCNUMBER", "DISCTOTAL", "TOTALDISCS", "BPM",
               "INITIALKEY", "KEY", "COMPILATION", "REPLAYGAIN_TRACK_GAIN", "REPLAYGAIN_ALBUM_GAIN",
               "REPLAYGAIN_TRACK_PEAK", "REPLAYGAIN_ALBUM_PEAK", "R128_TRACK_GAIN", "R128_ALBUM_GAIN",
               "MUSICBRAINZ_ALBUMID", "MUSICBRAINZ_TRACKID", "TITLESORT", "ARTISTSORT", "ALBUMSORT",
               "ALBUMARTISTSORT", "title", "Artist", "COMMENT", "LYRICS"]
APE_KEYS = ["Title", "Artist", "Album", "Album Artist", "ALBUMARTIST", "Genre", "Composer", "Year", "Track", "Disc",
            "Compilation", "REPLAYGAIN_TRACK_GAIN", "REPLAYGAIN_TRACK_PEAK", "MUSICBRAINZ_ALBUMID", "TITLE",
            "Comment"]


def rand_values(rng, k=None):
    k = k if k is not None else rng.choice([1, 1, 1, 2, 3])
    vals = []
    for _ in range(k):
        v = rng.choice(VOCAB)
        if rng.random() < 0.1:
            v = rng.choice(["-1312", "32", "0", "-5888", "+12", "40000"])
        vals.append(v)
    return vals


def rand_text(rng, ver, vals):
    """An ID3v2 text frame's body for these values in a random encoding."""
    encs = [0, 1] if ver == 2 else [0, 1, 2, 3]
    enc = rng.choice(encs)
    if enc == 0 and any(ord(c) > 0xFF for v in vals for c in v):
        enc = 1 if ver == 2 else 3
    return text_frame(enc, *vals)


def rand_tag(rng, ver, first, pics_seed):
    """A random ID3v2 tag: (Tag, build keywords)."""
    flags = 0
    unsync4 = ver == 4 and rng.random() < 0.12
    if unsync4:
        flags |= 0x80
    whole3 = ver == 3 and rng.random() < 0.12
    if whole3:
        flags |= 0x80
    t = Tag(ver, flags)
    ids = V2_IDS if ver == 2 else (V3_IDS if ver == 3 else V4_IDS)
    for _ in range(rng.randint(0, 10)):
        r = rng.random()
        fflags = 0
        plain = ver == 4 and rng.random() < 0.15
        frame_unsync = ver == 4 and (unsync4 or rng.random() < 0.1)
        if r < 0.55:
            fid = rng.choice(ids)
            body = rand_text(rng, ver, rand_values(rng))
            hostile = rng.random()
            if hostile < 0.06 and ver != 2:
                body = b"\x03" + rng.choice([b"Bad \xe9 utf", b"\xf0\x9f\x8e x", b"ok\xed\xa0\x80", b"\xc3"])
            elif hostile < 0.1:
                body = b"\x01\xff\xfe" + "Odd".encode("utf-16-le") + b"\x41"
            if frame_unsync:
                body = unsync(body)
                fflags |= 0x0002
            t.frame(fid, body, fflags=fflags, plain_size=plain)
        elif r < 0.72 and ver != 2:
            enc = rng.choice([0, 1, 3])
            desc = rng.choice(TXXX_DESC)
            vals = rand_values(rng)
            if enc == 0 and any(ord(c) > 0xFF for v in vals + [desc] for c in v):
                enc = 3
            body = txxx(enc, desc, *vals)
            if frame_unsync:
                body = unsync(body)
                fflags |= 0x0002
            t.frame("TXXX", body, fflags=fflags, plain_size=plain)
        elif r < 0.78 and ver != 2:
            owner = rng.choice(["http://musicbrainz.org", "other.example"])
            ident = rng.choice(["1a2b3c4d-5e6f-4a7b-8c9d-0e1f2a3b4c5d", " 0f9e8d7c ", "", "x\x00y"])
            t.frame("UFID", ufid(owner, ident), plain_size=plain)
        elif r < 0.92:
            pics_seed[0] += 1
            seed = pics_seed[0]
            ptype = rng.choice([0, 3, 3, 4, 6])
            mime = rng.choice(["image/jpeg", "image/png", "IMAGE/JPG", "image/gif", ""])
            data = (jpeg if rng.random() < 0.6 else png)(seed % 25 + 1, rng.choice([0, 40, 96, 300]) + seed % 7)
            if rng.random() < 0.1:
                data = b""
            if ver == 2:
                mime = rng.choice(["image/jpeg", "image/png"])
            head, data = apic(ver, ptype, mime, data, desc=rng.choice(["", "front", "cöver"]),
                              enc=rng.choice([0, 1]) if ver != 2 else 0)
            fid = "PIC" if ver == 2 else "APIC"
            if frame_unsync:
                stored = unsync(head + data)
                hs = len(unsync(head))
                t.frame(fid, stored, fflags=0x0002, plain_size=plain,
                        stored_picture=(hs, len(stored) - hs, data) if data else None)
            elif ver != 2 and not unsync4 and rng.random() < 0.08:
                # Compressed: passed over by the election (5.3).
                comp = (be32(len(head + data)) if ver == 3 else syncsafe(len(head + data))) + zlib.compress(head + data)
                t.frame(fid, comp, fflags=0x0080 if ver == 3 else 0x0009)
            elif data:
                t.frame(fid, None, plain_size=plain, picture=(head, data))
            else:
                t.frame(fid, head, plain_size=plain)
        else:
            fid = "COM" if ver == 2 else "COMM"
            t.frame(fid, b"\x00eng\x00" + b"filler " * rng.randint(0, 40), fflags=0x0002 if unsync4 else 0,
                    plain_size=plain)
    if first and ver == 3 and rng.random() < 0.05:
        t.frame("WOAR", b"\x00http://url.example")
    kw = {"padding": rng.choice([16, 32, 64, 200]), "unsync_tag": whole3}
    return t, kw


def rand_mp3(rng, pics_seed):
    tags = []
    kws = []
    out = bytearray()
    where = rng.random()
    if where < 0.04:
        out += bytes(rng.randint(1, 40))  # zeros before the tag: lofty skips them
    elif where < 0.08:
        out += bytes([0x55]) * rng.randint(1, 600)  # junk: the tag is looked for in it
    if rng.random() < 0.8:
        t, kw = rand_tag(rng, rng.choice([2, 3, 3, 4, 4]), True, pics_seed)
        tags.append(t)
        kws.append(kw)
        if rng.random() < 0.1:
            t, kw = rand_tag(rng, rng.choice([3, 4]), False, pics_seed)
            tags.append(t)
            kws.append(kw)
    pics = []
    for t, kw in zip(tags, kws):
        b, p = t.build(start=len(out), **kw)
        out += b
        pics += p
    out += mp3_audio(xing=rng.random() < 0.8, frames=rng.choice([2, 2, 12]))
    if rng.random() < 0.15:
        items = []
        for _ in range(rng.randint(0, 6)):
            key = rng.choice(APE_KEYS)
            items.append((key, "\x00".join(rand_values(rng)).encode(), 0))
        if rng.random() < 0.3:
            pics_seed[0] += 1
            items.append((rng.choice(["Cover Art (Front)", "cover art (back)"]), b"c.jpg\x00" + jpeg(pics_seed[0] % 25 + 1), 1))
        a, p = ape(items)
        base = len(out)
        out += a
        pics += [{"offset": base + o, "length": n, "coding": 3, "fnv": digest(d)} for o, n, d in p]
    if rng.random() < 0.4:
        def v1s():
            v = rng.choice(VOCAB)
            return v.encode("latin-1", "replace")[:30] if v else b""
        out += id3v1(v1s(), v1s(), v1s(), rng.choice(["1999", "0999", "20x1", "    ", "2010"]),
                     track=rng.choice([None, 1, 7, 0]), genre=rng.choice([0, 13, 17, 191, 192, 255]))
    return bytes(out), pics


def rand_vorbis(rng, opus):
    items = []
    for _ in range(rng.randint(0, 12)):
        key = rng.choice(VORBIS_KEYS)
        val = rng.choice(rand_values(rng, 1))
        b = (key + "=" + val).encode("utf-8")
        if rng.random() < 0.05:
            b += b"\xff"  # not UTF-8: lofty drops the value
        if rng.random() < 0.05:
            b += b"\x00\x00"
        items.append(b)
    return items


def rand_flac(rng, pics_seed):
    id3 = b""
    if rng.random() < 0.2:
        t = Tag(3)
        t.frame("TIT2", text_frame(0, "Front ID3"))
        t.frame("TPE1", text_frame(0, rng.choice(["Front Artist", ""])))
        id3, _ = t.build()
    blocks = []
    mbp_anchors = []
    if rng.random() < 0.85:
        items = rand_vorbis(rng, False)
        if rng.random() < 0.15:
            pics_seed[0] += 1
            data = jpeg(pics_seed[0] % 25 + 1, rng.choice([40, 96, 200]))
            head, d = flac_picture(rng.choice([0, 3, 4]), rng.choice(["image/jpeg", "image/png", ""]), data)
            mbp = base64.b64encode(head + d).decode()
            items.insert(rng.randint(0, len(items)), ("METADATA_BLOCK_PICTURE=" + mbp).encode())
            mbp_anchors.append((mbp, data))
        blocks.append((4, vorbis_comment(items)))
    for _ in range(rng.choice([0, 0, 1, 2])):
        pics_seed[0] += 1
        data = (jpeg if rng.random() < 0.5 else png)(pics_seed[0] % 25 + 1, rng.choice([40, 96]))
        blocks.insert(rng.randint(0, len(blocks)), (6, flac_picture(rng.choice([0, 3, 4, 6]),
                                                                    rng.choice(["image/jpeg", "image/png"]), data)))
    if rng.random() < 0.3:
        blocks.insert(rng.randint(0, len(blocks)), (1, bytes(rng.randint(0, 50))))
    if rng.random() < 0.05:
        blocks.append((4, vorbis_comment(rand_vorbis(rng, False))))
    b, p = flac(blocks, id3=id3)
    for mbp, data in mbp_anchors:
        p.append({"offset": b.index(mbp.encode()), "length": len(mbp), "coding": 2, "fnv": digest(data)})
    return b, p


def rand_opus(rng, pics_seed):
    items = rand_vorbis(rng, True)
    pic = None
    if rng.random() < 0.15:
        pics_seed[0] += 1
        data = jpeg(pics_seed[0] % 25 + 1, rng.choice([96, 2000, 9000]))
        head, d = flac_picture(rng.choice([0, 3]), "image/jpeg", data)
        mbp = base64.b64encode(head + d).decode()
        at = rng.randint(0, len(items))
        items.insert(at, ("METADATA_BLOCK_PICTURE=" + mbp).encode())
        pic = (at, mbp, data)
    b, offs = opus_file(items, segs_per_page=rng.choice([2, 5, 40, 255]), seconds=rng.choice([1, 5, 200]))
    pics = []
    if pic:
        at, mbp, data = pic
        pics.append({"offset": offs[at], "length": len(mbp), "coding": 2, "fnv": digest(data)})
    return b, pics


def random_corpus(n, seed):
    import random
    rng = random.Random(seed)
    pics_seed = [0]
    out = {}
    for i in range(n):
        kind = rng.choice(["mp3"] * 6 + ["flac"] * 2 + ["opus"] * 2)
        if kind == "mp3":
            data, pics = rand_mp3(rng, pics_seed)
        elif kind == "flac":
            data, pics = rand_flac(rng, pics_seed)
        else:
            data, pics = rand_opus(rng, pics_seed)
        # Two pictures with one hash can't be told apart by anchors.json.
        hashes = [p["fnv"] for p in pics]
        if len(hashes) != len(set(hashes)):
            continue
        out["r%05d.%s" % (i, kind)] = (data, pics, "random %d" % i)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="fail if a file on disk differs")
    ap.add_argument("--random", type=int, default=0, help="write N random files to --out instead")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", type=Path)
    args = ap.parse_args()
    if args.random:
        corpus = random_corpus(args.random, args.seed)
        args.out.mkdir(parents=True, exist_ok=True)
        for name, (data, _, _) in corpus.items():
            (args.out / name).write_bytes(data)
        anchors = {name: {"note": note, "pictures": pics} for name, (_, pics, note) in sorted(corpus.items())}
        (args.out / "anchors.json").write_text(json.dumps(anchors, indent=1, sort_keys=True) + "\n")
        print("wrote %d random files to %s" % (len(corpus), args.out))
        return
    files = outputs()
    bad = 0
    for name, data in sorted(files.items()):
        path = OUT / name
        if args.check:
            if not path.exists() or path.read_bytes() != data:
                print("differs: %s" % path.relative_to(ROOT))
                bad += 1
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
    if args.check:
        known = set(files) | {"expected.json"}
        for p in OUT.iterdir():
            if p.name not in known:
                print("stray: %s" % p.relative_to(ROOT))
                bad += 1
        if bad:
            sys.exit(1)
        print("tag corpus: %d files match" % len(files))
    else:
        print("wrote %d files to %s" % (len(files), OUT.relative_to(ROOT)))


if __name__ == "__main__":
    main()
