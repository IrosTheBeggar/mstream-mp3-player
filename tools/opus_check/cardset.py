#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""The Opus bench card set for the device gate (docs/OPUS.md, M0): /bench/opus/ on the card.

  cardset.py --out DIR --research DIR --ffmpeg PATH [--flac FILE]... [--ytdl FILE]

Builds DIR/bench/opus/ with short FAT32-safe names and a README.txt naming each
file, its source and its purpose:
  - the mStream-argument transcodes (mStream's exact ffmpeg arguments,
    src/api/transcode.js: `-vn -f opus -acodec libopus -ab <rate> pipe:1`) of real
    tracks at 96k, 128k and 192k, from the FLACs given (--flac);
  - the research's files for the frame sizes (2.5, 10, 60, 120 ms), mono 64k, hybrid
    32k, SILK 16k, and the 1.9 MB picture tag (--research: the scratchpad's opus/);
  - a 510k CBR file, cut from the research's continuous PCM;
  - a 14 min file (that PCM four times over) at 128k, for the 10 min plays of G2 and G8;
  - a 4-track gapless album cut from that same PCM (sample-exact cuts at 44.1 kHz,
    each part encoded with mStream's arguments at 128k) in album/;
  - a fuzz file: random packets in all 32 TOC configurations x stereo x codes 0-3,
    some malformed, in real Ogg pages with CRCs, spanning pages, an EOS trim;
  - a yt-dlp file (--ytdl: a .opus yt-dlp made, stream-copied from YouTube).
--ffmpeg should be mStream's own (mStream/bin/ffmpeg/ffmpeg.exe) for the transcodes.
"""
import argparse
import os
import random
import shutil
import struct
import subprocess
import sys
from pathlib import Path

# ---- Ogg (RFC 3533): the CRC and a small muxer, for the fuzz file ----

_CRC = []
for _i in range(256):
    _r = _i << 24
    for _ in range(8):
        _r = ((_r << 1) ^ 0x04C11DB7) if _r & 0x80000000 else (_r << 1)
    _CRC.append(_r & 0xFFFFFFFF)


def ogg_crc(data):
    crc = 0
    for b in data:
        crc = ((crc << 8) & 0xFFFFFFFF) ^ _CRC[((crc >> 24) ^ b) & 0xFF]
    return crc


class OggMuxer:
    """Packets into pages: a page flushes at ~`page_bytes` of body or 255 segments; a
    packet past the page's end continues on the next (the continued flag). The
    granule is the one of the last packet completing on the page (-1: none)."""

    def __init__(self, serial, page_bytes=4096):
        self.serial = serial
        self.page_bytes = page_bytes
        self.seq = 0
        self.out = bytearray()
        self.segs = []
        self.body = bytearray()
        self.granule = -1
        self.continued = False
        self.bos = True
        self.pending_continued = False

    def _flush(self, eos=False):
        flags = (1 if self.continued else 0) | (2 if self.bos else 0) | (4 if eos else 0)
        hdr = bytearray(b"OggS" + bytes([0, flags]) + struct.pack("<qIII", self.granule, self.serial, self.seq, 0))
        hdr += bytes([len(self.segs)]) + bytes(self.segs)
        page = hdr + self.body
        crc = ogg_crc(page)
        page[22:26] = struct.pack("<I", crc)
        self.last_page = len(self.out)
        self.out += page
        self.seq += 1
        self.bos = False
        self.segs = []
        self.body = bytearray()
        self.granule = -1
        self.continued = self.pending_continued
        self.pending_continued = False

    def add(self, packet, granule_after, flush=False):
        n = len(packet)
        at = 0
        lacing = [255] * (n // 255) + [n % 255]
        for i, lv in enumerate(lacing):
            last = i == len(lacing) - 1
            self.segs.append(lv)
            self.body += packet[at:at + lv]
            at += lv
            if last:
                self.granule = granule_after
            if len(self.segs) == 255 or (len(self.body) >= self.page_bytes and last):
                self.pending_continued = not last
                self._flush()
        if flush and self.segs:
            self._flush()

    def finish(self, eos_granule):
        """The EOS page is the last packet's page, as ffmpeg writes it (an empty EOS page
        after it would carry the trim where no packet is): the page waiting is flushed
        as EOS, or the page already written is re-flagged and re-summed."""
        if self.segs:
            self.granule = eos_granule
            self._flush(eos=True)
            return bytes(self.out)
        page = self.out[self.last_page:]
        page[5] |= 4
        page[6:14] = struct.pack("<q", eos_granule)
        page[22:26] = b"\0\0\0\0"
        page[22:26] = struct.pack("<I", ogg_crc(page))
        self.out[self.last_page:] = page
        return bytes(self.out)


def opus_head(channels=2, pre_skip=312, gain=0):
    return b"OpusHead" + bytes([1, channels]) + struct.pack("<HIhB", pre_skip, 48000, gain, 0)


def opus_tags(vendor, comments):
    out = b"OpusTags" + struct.pack("<I", len(vendor)) + vendor.encode() + struct.pack("<I", len(comments))
    for c in comments:
        out += struct.pack("<I", len(c)) + c.encode()
    return out


def toc_samples(config):
    if config >= 16:
        return 120 << (config & 3)
    if config >= 12:
        return 960 if config & 1 else 480
    n = config & 3
    return 2880 if n == 3 else 480 << n


def size_bytes(s):
    return bytes([s]) if s < 252 else bytes([252 + (s - 252) % 4, (s - 252) // 4])


def random_frame(rng):
    r = rng.random()
    if r < 0.05:
        return b""
    if r < 0.1:
        s = 1275
    elif r < 0.3:
        s = rng.randint(1, 20)
    else:
        s = rng.randint(20, 400)
    return bytes(rng.getrandbits(8) for _ in range(s))


def fuzz_packet(rng, config, stereo, code):
    """A packet in the framing of `code` (RFC 6716 section 3.2) around random frames;
    returns (bytes, samples)."""
    toc = (config << 3) | (stereo << 2) | code
    fs = toc_samples(config)
    if code == 0:
        return bytes([toc]) + random_frame(rng), fs
    if code == 1:
        f = random_frame(rng)
        return bytes([toc]) + f + f, 2 * fs
    if code == 2:
        a, b = random_frame(rng), random_frame(rng)
        return bytes([toc]) + size_bytes(len(a)) + a + b, 2 * fs
    m = rng.randint(1, min(48, 5760 // fs))
    vbr = rng.random() < 0.5
    pad = rng.random() < 0.3
    frames = [random_frame(rng) for _ in range(m)]
    if not vbr:
        frames = [frames[0]] * m
    out = bytes([toc, m | (0x80 if vbr else 0) | (0x40 if pad else 0)])
    if pad:
        p = rng.choice([0, 1, 37, 254, 300])
        out += b"\xff" * (p // 254) + bytes([p % 254])
        padding = b"\0" * p
    else:
        padding = b""
    if vbr:
        for f in frames[:-1]:
            out += size_bytes(len(f))
    return out + b"".join(frames) + padding, m * fs


def malformed_packet(rng):
    kind = rng.randint(0, 3)
    if kind == 0:
        return b""  # no TOC at all
    if kind == 1:
        return bytes([(20 << 3) | 3, 0])  # code 3 with a count of 0
    if kind == 2:
        return bytes([(28 << 3) | 2, 200, 1, 2, 3])  # code 2 whose first frame is longer than the packet
    return bytes([(0 << 3) | 3, 49])  # 49 x 60 ms: over 120 ms


def make_fuzz(path, sweeps=8, seed=0xC0FFEE):
    rng = random.Random(seed)
    mux = OggMuxer(0xF0220000)
    mux.add(opus_head(), 0, flush=True)
    mux.add(opus_tags("cardset fuzz", ["TITLE=fuzz: random packets in every TOC configuration"]), 0, flush=True)
    total = 0  # the granule: every packet's samples so far, the pre-skip's among them (g0 = 0)
    packets = 0
    # The first audio page holds four ordinary 20 ms CELT packets on its own:
    # the reader refuses a file whose first audio page carries frames under
    # 10 ms (docs/OPUS.md, M2), and the random sweeps below would put one
    # there one time in two. Every configuration still follows.
    for i in range(4):
        p, samples = fuzz_packet(rng, 31, 1, 0)  # (config 31: CELT fullband, 20 ms)
        total += samples
        mux.add(p, total, flush=i == 3)
        packets += 1
    for _ in range(sweeps):
        order = [(c, s, k) for c in range(32) for s in range(2) for k in range(4)]
        rng.shuffle(order)
        for config, stereo, code in order:
            if rng.random() < 0.04:
                mux.add(malformed_packet(rng), total)
                packets += 1
            p, samples = fuzz_packet(rng, config, stereo, code)
            total += samples
            mux.add(p, total)
            packets += 1
    # The EOS page's granule trims the last 100 samples (and more than the
    # pre-skip is kept).
    data = mux.finish(total - 100)
    path.write_bytes(data)
    return packets, total, len(data)


# ---- the files ----

def run(cmd, out_path=None):
    print("  " + " ".join(str(c) for c in cmd[:12]) + (" ..." if len(cmd) > 12 else ""))
    if out_path:
        with open(out_path, "wb") as f:
            r = subprocess.run(cmd, stdout=f, stderr=subprocess.PIPE)
    else:
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode != 0:
        sys.exit(f"failed: {' '.join(str(c) for c in cmd)}\n{r.stderr.decode(errors='replace')[-2000:]}")


def mstream_transcode(ffmpeg, src, bitrate, out, extra_in=()):
    # mStream src/api/transcode.js spawnTranscode(): exactly its arguments, to stdout.
    run([ffmpeg, "-v", "error", "-y", *extra_in, "-i", str(src), "-vn", "-f", "opus", "-acodec", "libopus", "-ab",
         bitrate, "pipe:1"], out)


def damage_one_page(src, dst):
    """A copy of `src` with one byte in the body of the page spanning the file's middle
    flipped (XOR 0x5A): that page's CRC fails, the reader steps over it, the gap plan
    sizes what it held from the next page's granule and the generator fills it, so the
    track keeps its exact length (docs/OPUS.md, M2's review). The resync after a damaged
    page was the longest step a play made (tens of KB of reads in one next() call: 55 and
    139 ms measured on these two copies) until OPUS.md 8.11 made it steps of a 4 KB chunk
    or an 8 KB slice, so a play of such a file to its end checks that every step of a
    damaged file fits the pass budget (8.9, G6).
    Returns (the page's offset, the byte's)."""
    data = bytearray(src.read_bytes())
    mid = len(data) // 2
    at = 0
    while True:
        if data[at:at + 4] != b"OggS":
            sys.exit(f"{src}: no page header at {at}")
        nsegs = data[at + 26]
        body = at + 27 + nsegs
        end = body + sum(data[at + 27:body])
        if end > mid or end >= len(data):
            break
        at = end
    byte = body + (end - body) // 2
    data[byte] ^= 0x5A
    dst.write_bytes(data)
    return at, byte


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="the card's root: DIR/bench/opus/ is written")
    ap.add_argument("--research", required=True, help="the research scratchpad's opus/ folder")
    ap.add_argument("--ffmpeg", required=True, help="ffmpeg (mStream's own for the transcodes)")
    ap.add_argument("--flac", action="append", default=[], help="a real track for the mStream transcodes (first: 96/128/192k; others: 128k)")
    ap.add_argument("--ytdl", help="a yt-dlp-made .opus to include")
    ap.add_argument("--work", help="scratch directory (default: DIR/.work)")
    args = ap.parse_args()

    out = Path(args.out) / "bench" / "opus"
    album = out / "album"
    out.mkdir(parents=True, exist_ok=True)
    album.mkdir(parents=True, exist_ok=True)
    work = Path(args.work) if args.work else Path(args.out) / ".work"
    work.mkdir(parents=True, exist_ok=True)
    research = Path(args.research)
    media = research / "fit" / "media"
    files = research / "integration" / "files"
    pcm = media / "killers.pcm"  # s16le, 44.1 kHz, stereo, 209.92 s (the research's source track, decoded)
    readme = []

    def note(name, source, purpose):
        readme.append((name, source, purpose))

    # 1. mStream-argument transcodes of real tracks.
    for i, flac in enumerate(args.flac):
        stem = Path(flac).stem
        rates = ["96k", "128k", "192k"] if i == 0 else ["128k"]
        for r in rates:
            name = f"ms{r}.opus" if i == 0 else f"ms{r}_{i + 1}.opus"
            mstream_transcode(args.ffmpeg, flac, r, out / name)
            note(name, f"mStream transcode (its exact ffmpeg arguments, -ab {r}) of {stem}",
                 "G1 decode + convert at 128k and 96k (the gate's files), G9 the length (3:30: G2 and G8's 10 min "
                 "play is long128k.opus)")

    # 2. The research's files, renamed.
    copies = [
        ("f2p5.opus", media / "killers_128k_f2.5.opus", "the research's killers_128k_f2.5.opus (2.5 ms frames, 128k)",
         "G1/G6: 48 decode calls per 120 ms; the costliest frame size (not realtime at 160 MHz, the research expects)"),
        ("f10.opus", media / "killers_128k_f10.opus", "the research's killers_128k_f10.opus (10 ms frames)", "G1/G6"),
        ("f60.opus", files / "f60.opus", "the research's f60.opus (60 ms packets: 3 frames each, 128k)", "G1/G6: multi-frame packets"),
        ("f120.opus", files / "f120.opus", "the research's f120.opus (120 ms packets: 6 frames each, 128k)",
         "G6: the longest pass must stay under 30 ms (OPUS.md 8.11; 20 ms at M0) though a packet is 120 ms (6 decode calls)"),
        ("mono64k.opus", media / "mono_music_64k.opus", "the research's mono_music_64k.opus (mono, 64k)",
         "the 1-channel decoder and the L = R expansion; G1 mono"),
        ("hyb32k.opus", media / "voice_hyb_st_32k_f20.opus", "the research's voice_hyb_st_32k_f20.opus (hybrid SILK + CELT, stereo 32k)",
         "G1/G4: the hybrid path's stack"),
        ("silk16k.opus", media / "voice_wb_mono_16k_f20.opus", "the research's voice_wb_mono_16k_f20.opus (VoIP, SILK wideband, mono 16k)",
         "G1/G4: the SILK path's stack"),
        ("pic.opus", files / "withpic.opus", "the research's withpic.opus (a 1.9 MB METADATA_BLOCK_PICTURE tag)",
         "the open skips the tag by page headers (24 KB of reads): the open time in the [opus] open line"),
    ]
    for name, src, source, purpose in copies:
        if not src.exists():
            sys.exit(f"missing {src}")
        shutil.copyfile(src, out / name)
        note(name, source, purpose)

    # 3. 510k CBR, 60 s, from the continuous PCM.
    if not pcm.exists():
        sys.exit(f"missing {pcm}")
    run([args.ffmpeg, "-v", "error", "-y", "-f", "s16le", "-ar", "44100", "-ac", "2", "-i", str(pcm), "-t", "60", "-vn",
         "-f", "opus", "-c:a", "libopus", "-b:a", "510k", "-vbr", "off", "pipe:1"], out / "cbr510k.opus")
    note("cbr510k.opus", "the research's killers.pcm (its source track, 16-bit 44.1 kHz) encoded by ffmpeg's libopus at 510k CBR, 60 s",
         "G1/G4: the heaviest CELT packets (1,276 B each)")

    # 3b. A play of 10 min or more for G2 and G8 (the longest real track is
    # 3:30, and Rf plays one file with nothing after it): the PCM four
    # times over, 839.7 s, as one mStream transcode at 128k. -stream_loop
    # is an input option, so the output side stays mStream's exactly.
    mstream_transcode(args.ffmpeg, pcm, "128k", out / "long128k.opus",
                      extra_in=["-stream_loop", "3", "-f", "s16le", "-ar", "44100", "-ac", "2"])
    note("long128k.opus", "the research's killers.pcm four times over (the source track x 4: 839.7 s, 40,304,640 samples at 48 kHz "
         "once trimmed), mStream's transcode arguments at 128k",
         "G2 (load over 10 min) and G8 (10 min at 160 MHz): one Rf plays the 14 min through; G9 the length of a long file")

    # 4. The gapless album: 4 contiguous parts of the PCM, cut on sample boundaries.
    frame_bytes = 4
    size = pcm.stat().st_size
    frames = size // frame_bytes
    part_frames = frames // 4
    with open(pcm, "rb") as f:
        for i in range(4):
            n = part_frames if i < 3 else frames - 3 * part_frames
            data = f.read(n * frame_bytes)
            part = work / f"album{i + 1}.pcm"
            part.write_bytes(data)
            name = f"{i + 1:02d}.opus"
            mstream_transcode(args.ffmpeg, part, "128k", album / name, extra_in=["-f", "s16le", "-ar", "44100", "-ac", "2"])
            note(f"album/{name}", f"part {i + 1} of 4 of the research's killers.pcm (the source track, continuous; cut at sample "
                 f"{i * part_frames} of 44.1 kHz), mStream's transcode arguments at 128k",
                 "G3 the gapless album (joins need the library index: M2); until then each part benches and plays on its own")

    # 5. The fuzz file.
    packets, total, nbytes = make_fuzz(out / "fuzz.opus")
    note("fuzz.opus", f"made here: {packets} random packets in every TOC configuration x stereo x code 0-3, ~4 % malformed, "
         f"{total / 48000:.1f} s of granules, {nbytes} B",
         "G4: the decode stack's high-water on hostile input (>= 1,536 B free); no crash. LOUD NOISE if ever played: bench only "
         "(b</bench/opus/fuzz.opus>). ffmpeg's decoder gives up on it early, so opus_check.py reports a length mismatch by design")

    # 5b. Damaged copies of two real files, one page's CRC wrong in the
    # middle of each: the resync after it was the longest step a play made
    # (M2: the scan and the page it found in one read), and is steps since
    # OPUS.md 8.11, so G6 plays them to their end to see every step fit the
    # budget.
    for name in ("ms128k.opus", "cbr510k.opus"):
        src = out / name
        if not src.exists():
            continue
        dst = out / f"{Path(name).stem}_dmg.opus"
        page, byte = damage_one_page(src, dst)
        note(dst.name, f"{name} with byte {byte} flipped (in the body of the page at {page}, the file's middle): that page's "
             "CRC fails and the reader steps over it",
             "G6 with a damaged page: Rf to its end; `[opus] end:` must say `gaps 1 (1 filled: N samples)` and `exactly the "
             "length`, its `longest step` within the 15,000 us budget (the resync's scan is a 4 KB chunk a step and the "
             "page it finds a slice a step since OPUS.md 8.11; M2 measured 55 and 139 ms here, one read each), "
             "underruns +0, the track ends on its own")

    # 6. A yt-dlp file.
    if args.ytdl:
        shutil.copyfile(args.ytdl, out / "ytdl.opus")
        note("ytdl.opus", f"a yt-dlp download ({Path(args.ytdl).name}): YouTube's Opus stream-copied into Ogg by ffmpeg",
             "G1/G9: the other real-world source of .opus files (20 ms packets, ~130 kb/s)")

    # README.txt
    lines = ["Opus bench card set for the Core2 (docs/OPUS.md, milestone M0). Copy this folder to the card as /bench/opus/.",
             "",
             "No file here is listed by the library: the console plays and benches them by path:",
             "  b</bench/opus/ms128k.opus>   the bench (decode, then decode + convert): no sound",
             "  Rf</bench/opus/ms128k.opus>  plays it on its own, in silent mode (z) only",
             "  O / Ol / Oi / Oh             where the decoder's state goes (gate G1's A/B)",
             "The MP3/FLAC baselines (One More Time, Stronger) are under /music already.",
             "",
             "File | Source | Purpose", "---- | ------ | -------"]
    for name, source, purpose in readme:
        lines.append(f"{name} | {source} | {purpose}")
    total_bytes = sum(p.stat().st_size for p in out.rglob("*") if p.is_file())
    lines += ["", f"{len(readme)} files, {total_bytes / 1e6:.1f} MB in all."]
    (out / "README.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"{out}: {len(readme)} files, {total_bytes / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
