#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Generate test audio for the player's LittleFS image (data/music/).

Makes tones of known pitch (to check playback speed and channel order, and the
48 kHz / hi-res paths) and, optionally, short excerpts of real tracks, sized to
fit the 3.8 MB filesystem partition (its size is read from partitions.csv;
a real library goes on the SD card). Needs ffmpeg; if none is on PATH it uses
the portable build from the imageio-ffmpeg package:

    python -m venv .venv-tools
    .venv-tools/Scripts/pip install imageio-ffmpeg          (Windows; bin/ elsewhere)
    .venv-tools/Scripts/python tools/make_test_audio.py --mp3 SONG.mp3 --flac SONG.flac

Then write it to the Core2's flash with:  pio run -e core2 -t uploadfs
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "data" / "music"


def partition_bytes(label: str = "spiffs") -> int:
    """The size of a partition in partitions.csv (the one uploadfs writes)."""
    for line in (ROOT / "partitions.csv").read_text(encoding="utf-8").splitlines():
        fields = [f.strip() for f in line.split(",")]
        if not line.lstrip().startswith("#") and len(fields) >= 5 and fields[0] == label:
            return int(fields[4], 0)
    sys.exit(f"No '{label}' partition in partitions.csv.")


BUDGET_BYTES = int(partition_bytes() * 0.9)  # leave room for LittleFS metadata

# ffmpeg's sine source is 1/8 of full scale, i.e. about -18 dBFS: loud enough
# to hear, safe at any volume setting.
TONES = [
    # name, frequency, sample rate, seconds, extra output args
    ("01_tone_440Hz_44k.mp3", 440, 44100, 20, ["-ac", "2", "-c:a", "libmp3lame", "-b:a", "128k"]),
    ("02_tone_1kHz_44k.flac", 1000, 44100, 20, ["-ac", "2", "-c:a", "flac", "-sample_fmt", "s16"]),
    ("03_tone_left_only_44k.mp3", 440, 44100, 10,
     ["-af", "pan=stereo|c0=c0|c1=0*c0", "-c:a", "libmp3lame", "-b:a", "128k"]),
    ("04_tone_440Hz_48k.mp3", 440, 48000, 10, ["-ac", "2", "-c:a", "libmp3lame", "-b:a", "128k"]),
    ("05_tone_1kHz_96k_24bit.flac", 1000, 96000, 5,
     ["-ac", "2", "-c:a", "flac", "-sample_fmt", "s32", "-bits_per_raw_sample", "24"]),
]


def find_ffmpeg() -> str:
    exe = shutil.which("ffmpeg")
    if exe:
        return exe
    try:
        import imageio_ffmpeg  # type: ignore
    except ImportError:
        sys.exit("No ffmpeg on PATH and imageio-ffmpeg isn't installed (see this file's docstring).")
    return imageio_ffmpeg.get_ffmpeg_exe()


def run(ffmpeg: str, args: list) -> None:
    subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", *args], check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--mp3", type=Path, help="MP3 to excerpt (first 30 s, tags kept)")
    parser.add_argument("--flac", type=Path, help="FLAC to excerpt (first 8 s, tags kept)")
    opts = parser.parse_args()

    ffmpeg = find_ffmpeg()
    OUT.mkdir(parents=True, exist_ok=True)
    for old in OUT.iterdir():
        if old.is_file():
            old.unlink()

    for name, freq, rate, seconds, out_args in TONES:
        run(ffmpeg, ["-f", "lavfi", "-i", f"sine=frequency={freq}:sample_rate={rate}:duration={seconds}",
                     "-metadata", f"title={Path(name).stem}", *out_args, str(OUT / name)])

    # Excerpts: audio stream only (drops embedded cover art), tags kept.
    if opts.mp3:
        run(ffmpeg, ["-i", str(opts.mp3), "-t", "30", "-map", "0:a", "-map_metadata", "0",
                     "-c", "copy", str(OUT / "10_excerpt.mp3")])
    if opts.flac:
        run(ffmpeg, ["-i", str(opts.flac), "-t", "8", "-map", "0:a", "-map_metadata", "0",
                     "-c:a", "flac", str(OUT / "11_excerpt.flac")])

    total = 0
    for f in sorted(OUT.iterdir()):
        total += f.stat().st_size
        print(f"{f.stat().st_size / 1024:8.0f} KB  {f.name}")
    print(f"{total / 1024:8.0f} KB  total (budget {BUDGET_BYTES / 1024:.0f} KB)")
    if total > BUDGET_BYTES:
        sys.exit("Too big for the LittleFS partition: use shorter or lower-bitrate excerpts.")


if __name__ == "__main__":
    main()
