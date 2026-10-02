#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""The USB visualizer's reference sender: makes a Core2's crab dance to audio on this computer.

It speaks the protocol in docs/USB-VISUALIZER.md: the Core2's own beat-tracker
front end runs here (a float32 port of lib/core/HopFrontEnd, bit-exact on the
golden file), and the low and mid band energy of each 512-frame hop goes down
the serial port as '@h' lines, with the heard frame ('@c') ten times a second.
Nothing is heard unless --play asks for it; the Core2 pauses its own player.

    python tools/usb_viz.py --port COM3 --click 120                 silent: a click track's beats, 60 s
    python tools/usb_viz.py --port COM3 --wav a.wav --wav b.wav      silent; an epoch per file
    python tools/usb_viz.py --port COM3 --file song.flac --play      plays it here too (ffmpeg for MP3/FLAC)
    python tools/usb_viz.py --port COM3 --click 120off --measure --expect "lock<=4,bpm<=0.5,med<=10,p95<=25"
    python tools/usb_viz.py --dry-run --fast --click 120 --seconds 2  the lines on stdout, no port
    python tools/usb_viz.py --selftest                               the port against the golden file
    python tools/usb_viz.py --score run.jsonl                         score a --log of an earlier run

Close `pio monitor` first: the sender needs the port, and shows the Core2's
log lines itself. It opens the port with DTR and RTS low (no reset), and only
ever writes whole '@' lines: never a bare byte the console could take as a key.
Needs pyserial (not for --dry-run or --selftest). Tests:
python -m unittest discover -s tools -p "test_usb_viz.py"
"""
import argparse
import array
import bisect
import json
import math
import os
import queue
import random
import re
import secrets
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GOLDEN = ROOT / "test" / "test_hop_feed" / "hop_golden.h"

PROTO = 1                  # the protocol version this sender speaks
BYTE_S = 10 / 115200  # a byte on the wire at 115200 baud, 8N1
RATES = (44100, 48000)     # the rates an epoch may have
HOP = 512                  # frames per hop (BeatTracker::Config::hop)
DECIMATION = 8
LOWPASS_HZ = 150.0
MAX_LINE = 255             # bytes from the '@' to the last byte before '\n'
MAX_ENERGY = 1e4
CORE2_BRIDGES = ((0x1A86, 0x55D4), (0x10C4, 0xEA60))  # CH9102, CP2104

# ---- float32, as the firmware computes ----

_F32 = struct.Struct("<f")


def f32(x: float) -> float:
    """x rounded to the nearest float32 (what a C++ float holds)."""
    return _F32.unpack(_F32.pack(x))[0]


def lround(x: float) -> int:
    """C's llround: halves away from zero (Python's round() goes to even)."""
    return int(math.floor(x + 0.5)) if x >= 0 else -int(math.floor(-x + 0.5))


# ---- ClickGen: lib/core/ClickGen, the same samples and beat frames ----


class ClickGen:
    """A click track with a known beat grid: a port of lib/core/ClickGen.

    Every operation is rounded to float32 as the C++ does, so the samples are
    the firmware's to the bit (the golden file's FNV-1a checks it). Beat k is
    at offset + llround(k * 60 * rate / bpm) frames."""

    OFF_BEATS = 0.37      # "off" tracks: the first beat at 0.37 of a period
    CLICK_MS = 15.0
    UNACCENTED_DB = -4.0
    TICK_LEVEL = 0.35

    def __init__(self, bpm: float, offset_beats: float, rate: int, frames: int, level_dbfs: float = -12.0):
        self.bpm = float(bpm)
        self.offset_beats = offset_beats
        self.rate = rate
        self.frames = frames
        self.level_dbfs = level_dbfs
        self.period = 60.0 * rate / self.bpm
        self.offset = lround(f32(offset_beats) * self.period)

    @staticmethod
    def parse(name: str):
        """'120', 'click120', '120off', 'click120off' -> (bpm, offset_beats), or None."""
        m = re.fullmatch(r"(?:tone:)?(?:click)?(\d+)(off)?", name.strip())
        if not m:
            return None
        bpm = int(m.group(1))
        if bpm < 30 or bpm > 300:
            return None
        return float(bpm), ClickGen.OFF_BEATS if m.group(2) else 0.0

    def beat_frame(self, k: int) -> int:
        return self.offset + lround(k * 60.0 * self.rate / self.bpm)

    def beats(self) -> list:
        out, k = [], 0
        while self.beat_frame(k) < self.frames:
            out.append(self.beat_frame(k))
            k += 1
        return out

    def _shape(self) -> list:
        """One click before normalising (ClickGen::shape), frame by frame."""
        rate = self.rate
        n = int(f32(f32(rate * f32(self.CLICK_MS)) / f32(1000.0)))
        two_pi, pi = f32(6.283185307), f32(3.141592654)
        k004, k005, k0005, k0008 = f32(0.004), f32(0.005), f32(0.0005), f32(0.0008)
        tick_w = f32(two_pi * 2000.0)
        tick_level = f32(self.TICK_LEVEL)
        out, phase = [], 0.0
        for i in range(n):
            t = f32(i / rate)
            freq = f32(60.0 + f32(40.0 * f32(math.exp(f32(-t / k004)))))
            phase = f32(phase + f32(f32(two_pi * freq) / rate))
            attack = f32(t / k0005) if t < k0005 else 1.0
            window = f32(0.5 + f32(0.5 * f32(math.cos(f32(f32(pi * i) / n)))))
            thump = f32(f32(math.sin(phase)) * f32(math.exp(f32(-t / k005))))
            tick = f32(f32(tick_level * f32(math.sin(f32(tick_w * t)))) * f32(math.exp(f32(-t / k0008))))
            out.append(f32(f32(attack * window) * f32(thump + tick)))
        return out

    def stereo(self) -> array.array:
        """The whole track, interleaved int16 stereo (both channels the same)."""
        shape = self._shape()
        peak = max(abs(v) for v in shape) or 1.0
        level = f32(f32(10.0 ** f32(self.level_dbfs / 20.0)) / peak)
        soft = f32(level * f32(10.0 ** f32(self.UNACCENTED_DB / 20.0)))
        accented = [lround(f32(32767.0 * f32(v * level))) for v in shape]
        unaccented = [lround(f32(32767.0 * f32(v * soft))) for v in shape]
        mono = array.array("h", bytes(2 * self.frames))
        k = 0
        while self.beat_frame(k) < self.frames:
            start, nxt = self.beat_frame(k), self.beat_frame(k + 1)
            click = accented if k % 4 == 0 else unaccented
            n = min(len(click), nxt - start, self.frames - start)  # a new beat cuts a click short
            mono[start:start + n] = array.array("h", click[:n])
            k += 1
        out = array.array("h", bytes(4 * self.frames))
        out[0::2] = mono
        out[1::2] = mono
        return out


def fnv1a_mono(stereo: array.array) -> int:
    """FNV-1a (32 bit) over the left channel's int16 samples, little-endian."""
    left = array.array("h", stereo[0::2])
    if sys.byteorder != "little":
        left.byteswap()
    h = 0x811C9DC5
    for byte in left.tobytes():
        h = ((h ^ byte) * 0x01000193) & 0xFFFFFFFF
    return h


# ---- the front end: lib/core/HopFrontEnd ----

_KPI = 3.14159265358979  # the C++ constant, as written there


def front_end_coefficients(rate: int):
    """(dc_pole, lp1, lp2) as HopFrontEnd::begin() makes them: lpN is (b0, b1, b2, a1, a2),
    worked out in double and kept as float32."""
    # 2.0f * float(kPi) * kDcHz * decimation / sampleRate, all float.
    dc_pole = f32(1.0 - f32(f32(f32(f32(2.0 * f32(_KPI)) * 5.0) * DECIMATION) / rate))
    fs = f32(rate / DECIMATION)
    filters = []
    for q in (f32(0.5412), f32(1.3066)):  # fourth-order Butterworth as two biquads
        w0 = 2.0 * _KPI * LOWPASS_HZ / fs
        alpha = math.sin(w0) / (2.0 * q)
        c = math.cos(w0)
        a0 = 1.0 + alpha
        b0 = f32((1.0 - c) / 2.0 / a0)
        filters.append((b0, f32((1.0 - c) / a0), b0, f32(-2.0 * c / a0), f32((1.0 - alpha) / a0)))
    return dc_pole, filters[0], filters[1]


class HopFrontEnd:
    """BeatTracker's step 1 (docs/USB-VISUALIZER.md "The front end"), every
    operation rounded to float32 in the C++ order: bit-exact with the firmware
    on the golden file (an ESP32 that fuses a multiply-add may differ by an
    ulp; the tracker doesn't care). About 4 % of a core at 44.1 kHz."""

    def __init__(self, rate: int):
        if rate not in RATES:
            raise ValueError(f"the front end runs at 44100 or 48000 Hz, not {rate}")
        self.rate = rate
        self.dc_pole, self.lp1, self.lp2 = front_end_coefficients(rate)
        self.scale = f32(1.0 / (32768.0 * DECIMATION))
        self.hop_len = HOP // DECIMATION
        self.reset()

    def reset(self):
        """All state zero: the next sample starts a hop."""
        self.decim_sum = 0
        self.decim_count = 0
        self.fill = 0
        self.low = self.mid = 0.0
        self.dc_x = self.dc_y = 0.0
        self.z = [0.0, 0.0, 0.0, 0.0]

    def process(self, mono) -> list:
        """int16 mono samples (any iterable), contiguous with what came before.
        Returns the (low, mid) energy of each hop completed."""
        F = f32
        b0, b1, b2, a1, a2 = self.lp1
        c0, c1, c2, d1, d2 = self.lp2
        dc_pole, scale, hop_len = self.dc_pole, self.scale, self.hop_len
        z1, z2, z3, z4 = self.z
        s, n, fill = self.decim_sum, self.decim_count, self.fill
        low, mid, dx, dy = self.low, self.mid, self.dc_x, self.dc_y
        hops = []
        for v in mono:
            s += v
            n += 1
            if n < DECIMATION:
                continue
            x = F(s * scale)
            s = n = 0
            d = F(F(x - dx) + F(dc_pole * dy))  # the DC blocker
            dx, dy = x, d
            y1 = F(F(b0 * d) + z1)  # biquad 1, transposed direct form II
            z1 = F(F(F(b1 * d) - F(a1 * y1)) + z2)
            z2 = F(F(b2 * d) - F(a2 * y1))
            y = F(F(c0 * y1) + z3)  # biquad 2
            z3 = F(F(F(c1 * y1) - F(d1 * y)) + z4)
            z4 = F(F(c2 * y1) - F(d2 * y))
            m = F(d - y)  # above the low band
            low = F(low + F(y * y))
            mid = F(mid + F(m * m))
            fill += 1
            if fill == hop_len:
                hops.append((low, mid))
                low = mid = 0.0
                fill = 0
        self.z = [z1, z2, z3, z4]
        self.decim_sum, self.decim_count, self.fill = s, n, fill
        self.low, self.mid, self.dc_x, self.dc_y = low, mid, dx, dy
        return hops


def mono_of(stereo: array.array, start: int, end: int) -> list:
    """Frames [start, end) of interleaved int16 stereo as the firmware's mono:
    (left + right) >> 1, an arithmetic shift (DanceMode::feed)."""
    left = stereo[2 * start:2 * end:2]
    right = stereo[2 * start + 1:2 * end:2]
    return [(a + b) >> 1 for a, b in zip(left, right)]


# ---- the golden file ----


def read_golden(path: Path = GOLDEN) -> list:
    """test/test_hop_feed/hop_golden.h as a list of tracks:
    {name, rate, bpm, offset, frames, mono_fnv1a, dc_pole, lp1, lp2, beats, hops}."""
    tracks, cur = [], None
    for line in path.read_text(encoding="utf-8").splitlines():
        m = re.match(r"// track (\d+): (.*)$", line)
        if m:
            kv = dict(item.split("=", 1) for item in m.group(2).split())
            cur = {
                "name": kv["name"], "rate": int(kv["rate"]), "bpm": float(kv["bpm"]),
                "offset": float(kv["offset"]), "frames": int(kv["frames"]),
                "mono_fnv1a": int(kv["mono_fnv1a"], 16), "dc_pole": float(kv["dc_pole"]),
                "lp1": tuple(float(v) for v in kv["lp1"].split(",")),
                "lp2": tuple(float(v) for v in kv["lp2"].split(",")),
                "hop_count": int(kv["hops"]), "beats": [], "hops": [],
            }
            tracks.append(cur)
            continue
        if cur is None:
            continue
        m = re.match(r"\s*\{([-+0-9.eE]+)f, ([-+0-9.eE]+)f\},\s*// \d+", line)
        if m:
            cur["hops"].append((float(m.group(1)), float(m.group(2))))
            continue
        m = re.match(r"inline constexpr uint32_t kBeats\d+\[\] = \{(.*)\};", line)
        if m:
            cur["beats"] = [int(v) for v in m.group(1).split(",") if v.strip()]
    return tracks


# ---- the lines ----

_F32_FIELD = re.compile(r"-?[0-9]+(\.[0-9]*)?([eE][-+]?[0-9]+)?")


def encode_line(text: str) -> bytes:
    """The only way a line reaches the port. Refuses anything the Core2's
    console could take as a key: it must start with '@', be printable ASCII
    (0x20-0x7E) and at most 255 bytes; the '\\n' is added here."""
    if not isinstance(text, str) or not text.startswith("@") or len(text) < 2:
        raise ValueError(f"not a host line: {text!r}")
    if len(text) > MAX_LINE:
        raise ValueError(f"host line over {MAX_LINE} bytes: {text[:40]!r}...")
    for ch in text:
        if not " " <= ch <= "~":
            raise ValueError(f"host line with byte {ord(ch):#04x}: {text!r}")
    return text.encode("ascii") + b"\n"


def fmt_energy(e: float) -> str:
    """An energy as an f32 field: '%.9g' of the float32, which strtof reads
    back to the same bits, so the Core2's tracker is the one fed from audio,
    bit for bit (the protocol asks for at least 6 digits and allows '0' under
    1e-9; sending those exactly too is what keeps it bit for bit). Never
    negative or over 1e4."""
    if not e > 0.0:  # (also NaN)
        return "0"
    return "%.9g" % f32(min(e, MAX_ENERGY))


def fmt_prior(bpm: float) -> str:
    return "0" if not bpm else "%.6g" % bpm


def line_hello(session: str, features: str = "viz") -> str:
    return f"@hello {PROTO} {session} {features}"


def line_epoch(epoch: int, rate: int, prior: float) -> str:
    return f"@e {epoch} {rate} {fmt_prior(prior)}"


def line_hop(epoch: int, hop: int, low: float, mid: float) -> str:
    return f"@h {epoch} {hop} {fmt_energy(low)} {fmt_energy(mid)}"


def line_clock(epoch: int, heard: int, playing: bool) -> str:
    return f"@c {epoch} {heard} {1 if playing else 0}"


def new_session_id() -> str:
    return secrets.token_hex(4)  # 8 of [0-9a-f]: a token of 1-16 [0-9A-Za-z]


# ---- audio sources ----


class Source:
    """One run of audio: interleaved int16 stereo at 44.1 or 48 kHz, with its
    beats (source frames) when they are known."""

    def __init__(self, name: str, rate: int, pcm: array.array, beats=None, bpm: float = 0.0):
        self.name = name
        self.rate = rate
        self.pcm = pcm
        self.frames = len(pcm) // 2
        self.beats = beats      # sorted source frames, or None
        self.bpm = bpm          # the truth's tempo, 0 if unknown
        self.wav_path = None    # --play: the file handed to winsound

    def period(self) -> float:
        if self.bpm:
            return 60.0 * self.rate / self.bpm
        if self.beats and len(self.beats) > 2:
            gaps = sorted(b - a for a, b in zip(self.beats, self.beats[1:]))
            return float(gaps[len(gaps) // 2])
        return 0.0


def click_source(name: str, rate: int, seconds: float) -> Source:
    spec = ClickGen.parse(name)
    if spec is None:
        raise SystemExit(f"--click {name}: want a tempo 30-300, e.g. 120 or 120off")
    gen = ClickGen(spec[0], spec[1], rate, int(round(seconds * rate)))
    label = f"click{int(spec[0])}{'off' if spec[1] else ''}@{rate}"
    return Source(label, rate, gen.stereo(), gen.beats(), gen.bpm)


def find_ffmpeg():
    """ffmpeg on PATH, or imageio-ffmpeg's (imported, or in the repo's
    .venv-tools as tools/make_test_audio.py suggests); None if neither."""
    exe = shutil.which("ffmpeg")
    if exe:
        return exe
    try:
        import imageio_ffmpeg  # type: ignore

        return imageio_ffmpeg.get_ffmpeg_exe()
    except Exception:
        pass
    for venv in (ROOT / ".venv-tools", ROOT.parent.parent.parent / ".venv-tools"):
        hits = sorted(venv.glob("**/imageio_ffmpeg/binaries/ffmpeg*"))
        hits = [h for h in hits if h.is_file() and not h.name.endswith(".md")]
        if hits:
            return str(hits[0])
    return None


def read_wav(path: Path):
    """A 16- or 24-bit PCM WAV at 44.1 or 48 kHz as (rate, int16 stereo), or
    None for anything else (ffmpeg can convert those)."""
    with wave.open(str(path), "rb") as w:
        rate, channels, width, n = w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()
        if rate not in RATES or channels not in (1, 2) or width not in (2, 3):
            return None
        data = w.readframes(n)
    if width == 3:  # 24-bit: keep the top 16 bits
        out = bytearray(len(data) // 3 * 2)
        out[0::2] = data[1::3]
        out[1::2] = data[2::3]
        data = bytes(out)
    pcm = array.array("h")
    pcm.frombytes(data)
    if sys.byteorder != "little":
        pcm.byteswap()
    if channels == 1:
        stereo = array.array("h", bytes(4 * len(pcm)))
        stereo[0::2] = pcm
        stereo[1::2] = pcm
        pcm = stereo
    return rate, pcm


def decode_with_ffmpeg(path: Path, ffmpeg: str):
    """Any file ffmpeg reads, as (rate, int16 stereo): 48 kHz if the source is
    a multiple of it, else 44.1 kHz."""
    probe = subprocess.run([ffmpeg, "-hide_banner", "-i", str(path)], capture_output=True, text=True,
                           errors="replace")
    m = re.search(r"Audio:.*?(\d+) Hz", probe.stderr)
    src_rate = int(m.group(1)) if m else 44100
    rate = 48000 if src_rate % 48000 == 0 else 44100
    run = subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-i", str(path), "-vn", "-f", "s16le",
                          "-acodec", "pcm_s16le", "-ac", "2", "-ar", str(rate), "-"], capture_output=True)
    if run.returncode != 0:
        raise SystemExit(f"ffmpeg couldn't decode {path}: {run.stderr.decode(errors='replace').strip()}")
    pcm = array.array("h")
    pcm.frombytes(run.stdout[: len(run.stdout) // 4 * 4])
    if sys.byteorder != "little":
        pcm.byteswap()
    return rate, pcm


def file_source(path: Path, seconds=None, beats_json=None) -> Source:
    if not path.is_file():
        raise SystemExit(f"No such file: {path}")
    got = None
    if path.suffix.lower() == ".wav":
        try:
            got = read_wav(path)
        except (wave.Error, EOFError) as e:
            print(f"usb_viz: {path.name}: the wave module can't read it ({e}); trying ffmpeg", file=sys.stderr)
    if got is None:
        ffmpeg = find_ffmpeg()
        if ffmpeg is None:
            raise SystemExit(f"{path.name}: only a 16- or 24-bit PCM WAV at 44.1 or 48 kHz works without ffmpeg, "
                             "and there is none on PATH or in an imageio-ffmpeg install "
                             "(pip install imageio-ffmpeg, or convert the file first)")
        got = decode_with_ffmpeg(path, ffmpeg)
    rate, pcm = got
    if seconds:
        pcm = pcm[: 2 * int(seconds * rate)]
    beats = None
    if beats_json:
        data = json.loads(Path(beats_json).read_text(encoding="utf-8"))
        times = data["beats"] if isinstance(data, dict) else data
        beats = sorted(lround(float(t) * rate) for t in times)
    return Source(path.name, rate, pcm, beats)


def write_wav(path: Path, src: Source, gain_db: float):
    """The source at gain_db, for winsound to play."""
    gain = 10.0 ** (gain_db / 20.0)
    pcm = array.array("h", (max(-32768, min(32767, lround(v * gain))) for v in src.pcm))
    if sys.byteorder != "little":
        pcm.byteswap()
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(src.rate)
        w.writeframes(pcm.tobytes())


# ---- clocks and ports ----


class RealClock:
    def now(self) -> float:
        return time.perf_counter()

    def sleep(self, seconds: float):
        if seconds > 0:
            time.sleep(seconds)


class VirtualClock:
    """For --dry-run --fast and the tests: sleeping only moves the time on."""

    def __init__(self, start: float = 0.0):
        self.t = start

    def now(self) -> float:
        return self.t

    def sleep(self, seconds: float):
        self.t += max(seconds, 0.0)


class SerialPort:
    """The Core2's serial port, opened with DTR and RTS low and never touched
    after (the board's auto-reset pulls EN or GPIO0 when they differ). A
    reader thread splits what comes back into lines, stamped on arrival.

    On arrival: read(n) with a timeout returns only when n bytes came or the
    timeout ran out (pyserial's Windows backend: ReadTotalTimeoutConstant,
    no interval timeout; POSIX loops the same way), so read(4096) at a few
    hundred bytes a second ended every read on the 50 ms timeout and
    stamped each line up to 50 ms late. It reads what is waiting, or one
    byte (which returns as that byte comes), and stamps a line by when its
    first byte came: the Core2 printed it then, and the rest of it took
    10 bits a byte at 115200 baud on the wire."""

    def __init__(self, name: str, clock, serial_factory=None):
        if serial_factory is None:
            try:
                import serial  # type: ignore
            except ImportError:
                raise SystemExit("pyserial isn't installed: pip install pyserial")
            serial_factory = serial.Serial
        self.clock = clock
        s = serial_factory()
        s.port = name
        s.baudrate = 115200
        s.timeout = 0.05
        s.dtr = False  # before open(): pyserial applies them as it opens
        s.rts = False
        s.open()
        self.serial = s
        self.lines = queue.Queue()
        self.running = True
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def _read(self):
        buf = b""
        first = None  # when the line under way began to come in
        while self.running:
            try:
                data = self.serial.read(max(1, self.serial.in_waiting))
            except Exception as e:  # unplugged
                self.lines.put((self.clock.now(), f"usb_viz: the port failed: {e}"))
                self.lines.put((self.clock.now(), None))
                return
            if not data:
                continue
            now = self.clock.now()
            while data:
                if first is None:
                    # This read's first byte came at most its own length
                    # on the wire before it returned.
                    first = now - len(data) * BYTE_S
                cut = data.find(b"\n")
                if cut < 0:
                    buf += data
                    break
                raw, data = buf + data[:cut], data[cut + 1:]
                buf = b""
                self.lines.put((first, raw.decode("utf-8", "backslashreplace").rstrip("\r")))
                first = None if not data else now - len(data) * BYTE_S

    def write(self, data: bytes):
        self.serial.write(data)

    def poll(self) -> list:
        out = []
        while True:
            try:
                out.append(self.lines.get_nowait())
            except queue.Empty:
                return out

    def close(self):
        self.running = False
        try:
            self.serial.flush()
        except Exception:
            pass
        self.thread.join(timeout=0.5)
        self.serial.close()


class FakeCore2:
    """A stand-in for the firmware's session logic (lib/core/HostLink), for
    --dry-run and the tests: '@ok' to a hello, '@bye ok' to a bye, '@err 3' to
    data with no session. It checks every line's grammar and the hop numbering
    and keeps what it saw (`lines`, `problems`). The tests script it further
    (`ignore_hellos`, `busy_hellos`, `say_at`, `reboot_at`, `boot_hellos`,
    `banner_delay`, `decline_at`)."""

    _GRAMMAR = {
        "hello": re.compile(r"@hello \d+ [0-9A-Za-z]{1,16} [a-z]+(,[a-z]+)*"),
        "bye": re.compile(r"@bye"),
        "e": re.compile(r"@e \d+ (44100|48000) " + _F32_FIELD.pattern),
        "h": re.compile(r"@h \d+ \d+ " + _F32_FIELD.pattern + " " + _F32_FIELD.pattern),
        "c": re.compile(r"@c \d+ -?\d+ [01]"),
        "log": re.compile(r"@log [012]"),
    }

    def __init__(self, fw: str = "dry-run"):
        self.fw = fw
        self.session = None
        self.epoch = None
        self.next_hop = None
        self.lines = []          # (t, text) for every line received
        self.problems = []
        self.ignore_hellos = 0   # boot: the first n hellos are lost
        self.busy_hellos = 0     # then n are answered '@err 4 hello ui'
        self.reboot_at = None    # a time: the board resets then (session lost)
        self.boot_hellos = 0     # hellos lost after that reset (the boot)
        self.banner_delay = 0.0  # the banner this long after the ROM lines (0: with them)
        self.decline_at = None   # a time: the user ends it on the Core2
        self.say_at = []         # (t, line) to say unprompted

    def receive(self, t: float, data: bytes) -> list:
        """One write from the sender; returns the lines it answers with."""
        out = self.tick(t)
        if not data.endswith(b"\n") or not data.startswith(b"@") or data.count(b"\n") != 1:
            self.problems.append(f"not one '@' line: {data!r}")
            return out
        text = data[:-1].decode("ascii")
        self.lines.append((t, text))
        verb = text[1:].split(" ", 1)[0]
        grammar = self._GRAMMAR.get(verb)
        if grammar is None or not grammar.fullmatch(text):
            self.problems.append(f"bad line: {text}")
            return out + [f"@err 1 {verb if grammar else '-'}"]
        f = text.split()
        if verb == "hello":
            if self.ignore_hellos > 0:
                self.ignore_hellos -= 1
                return out
            if self.busy_hellos > 0 and self.session is None:
                self.busy_hellos -= 1
                return out + ["@err 4 hello ui"]
            if self.session != f[2]:
                self.epoch = self.next_hop = None
            self.session = f[2]
            return out + [f"@ok {PROTO} {f[2]} {self.fw} viz,log"]
        if verb == "bye":
            if self.session is None:
                return out
            self.session = None
            return out + ["@bye ok"]
        if self.session is None:
            return out + [f"@err 3 {verb}"]
        if verb == "e":
            if self.epoch != int(f[1]):
                self.epoch, self.next_hop = int(f[1]), None
        elif verb in ("h", "c"):
            if self.epoch is None:
                return out + [f"@err 9 {verb}"]
            if verb == "h" and int(f[1]) == self.epoch:
                hop = int(f[2])
                if self.next_hop is not None and hop != self.next_hop:
                    self.problems.append(f"hop {hop} after {self.next_hop - 1}")
                self.next_hop = hop + 1
        return out

    def tick(self, t: float) -> list:
        """Unprompted lines due by t."""
        out = []
        if self.reboot_at is not None and t >= self.reboot_at:
            self.reboot_at = None
            self.session = self.epoch = self.next_hop = None
            self.ignore_hellos = self.boot_hellos
            out += ["ets Jul 29 2019 12:21:46", "", "rst:0x1 (POWERON_RESET),boot:0x17 (SPI_FAST_FLASH_BOOT)"]
            banner = "mstream-mp3-player v0.0.0-test (commit 0000000, fake), ELF 0000"
            if self.banner_delay > 0:
                self.say_at = sorted(self.say_at + [(t + self.banner_delay, banner)], key=lambda x: x[0])
            else:
                out.append(banner)
        if self.decline_at is not None and t >= self.decline_at and self.session is not None:
            self.decline_at = None
            self.session = None
            out += ["@bye user", "[viz] off (a touch): ..."]
        while self.say_at and self.say_at[0][0] <= t:
            out.append(self.say_at.pop(0)[1])
        return out


class DryRunPort:
    """--dry-run: every line written goes to `out` (stdout), and a FakeCore2
    answers so the session runs as it would."""

    def __init__(self, clock, device: FakeCore2 = None, out=None):
        self.clock = clock
        self.device = device or FakeCore2()
        self.out = out
        self.pending = []

    def write(self, data: bytes):
        if self.out is not None:
            self.out.write(data.decode("ascii"))
        t = self.clock.now()
        self.pending += [(t, line) for line in self.device.receive(t, data)]

    def poll(self) -> list:
        t = self.clock.now()
        self.pending += [(t, line) for line in self.device.tick(t)]
        out, self.pending = self.pending, []
        return out

    def close(self):
        pass


# ---- the player: what plays, and when ----


class Player:
    """The sources in turn, an epoch each (and a new one on a seek). The play
    position is a clock anchored when an epoch starts or playback resumes:
    epoch frame `anchor_pos` at time `anchor_t`, counting at the rate while
    playing. `heard` is the position less the output delay (--offset-ms)."""

    def __init__(self, sources: list, offset_ms: float = 0.0):
        self.sources = sources
        self.offset_ms = offset_ms
        self.index = -1
        self.epoch = 0
        self.src = None
        self.start = 0           # the source frame at epoch frame 0
        self.playing = False
        self.anchor_t = 0.0
        self.anchor_pos = 0.0
        self.next_hop = 0        # the next hop to compute
        self.fe = None
        self.started = False

    @property
    def rate(self) -> int:
        return self.src.rate

    def epoch_frames(self) -> int:
        return self.src.frames - self.start

    def position(self, t: float) -> float:
        """Epoch frames played by t (never past the epoch's end)."""
        if not self.playing:
            return self.anchor_pos
        return min(self.anchor_pos + (t - self.anchor_t) * self.rate, float(self.epoch_frames()))

    def heard(self, t: float) -> int:
        return lround(self.position(t) - self.offset_ms * self.rate / 1000.0)

    def new_epoch(self, t: float, index: int, start: int):
        if self.src is None or self.sources[index].rate != self.src.rate:
            self.fe = HopFrontEnd(self.sources[index].rate)
        self.index = index
        self.src = self.sources[index]
        self.epoch += 1
        self.start = start
        self.anchor_t, self.anchor_pos = t, 0.0
        self.playing = True
        self.started = True
        self.next_hop = 0
        self.fe.reset()

    def pause(self, t: float):
        if self.playing:
            self.anchor_pos = self.position(t)
            self.playing = False

    def resume(self, t: float):
        if not self.playing:
            self.anchor_t = t
            self.playing = True

    def end_time(self) -> float:
        """When this epoch's last frame has played (while playing)."""
        return self.anchor_t + (self.epoch_frames() - self.anchor_pos) / self.rate

    def compute_hop(self) -> tuple:
        """The next hop's energies (its audio through the front end)."""
        a = self.start + self.next_hop * HOP
        hops = self.fe.process(mono_of(self.src.pcm, a, a + HOP))
        self.next_hop += 1
        return hops[0]

    def skip_hops(self, n: int):
        """Audio lost before the front end (--gap-at): those hops aren't
        computed, and the filters start again after them."""
        self.next_hop += n
        self.fe.reset()

    def truth(self) -> dict:
        """This epoch's beats in epoch frames, for the log and --measure."""
        src = self.src
        period = src.period()
        beats = None
        if src.beats is not None:
            # (One beat before the epoch too, so a frame near 0 has its nearest.)
            beats = [b - self.start for b in src.beats if b - self.start >= -period]
        bpm = src.bpm or (60.0 * src.rate / period if period else 0.0)
        return {"kind": "epoch", "epoch": self.epoch, "rate": src.rate, "source": src.name, "start": self.start,
                "frames": self.epoch_frames(), "bpm": bpm, "period": period, "beats": beats}


# ---- the sender ----

HELLO, STREAM = "hello", "stream"
REBOOT_SIGNS = ("ets ", "rst:", "mstream-mp3-player ")
# The ROM's lines as the board resets, and the firmware's banner: Serial is
# up by the time it prints (it starts in M5.begin(), and SerialConsole::
# begin() has checked for a line under way just before). Nothing is written
# from a ROM line to the banner: a line the Core2 starts to read halfway
# could otherwise reach its console as keys (it drops such tails, but a
# sender shouldn't make them).
ROM_SIGNS = ("ets ", "rst:")
BANNER = "mstream-mp3-player "
BOOT_WAIT = 5.0  # s from a ROM line with no banner: not this firmware, or stuck; hello anyway


class Sender:
    """The session and the stream (docs/USB-VISUALIZER.md "Sessions"):
    @hello every 1 s until @ok, then @e, the hops and @c; a new session on
    @err 3, a reboot banner or @bye timeout; it stops for good on @bye user
    or @err 8 (the user on the Core2 wins); @bye on the way out."""

    def __init__(self, opts, sources: list, port, clock, out=None):
        self.o = opts
        self.port = port
        self.clock = clock
        self.out = out if out is not None else sys.stdout
        self.player = Player(sources, opts.offset_ms)
        self.rng = random.Random(opts.seed)
        self.state = HELLO
        self.session = new_session_id()
        self.hello_sent = False
        # First, a moment's listening: opening the port may have reset the
        # board (its ROM lines come within this), and nothing is written
        # while it boots (ROM_SIGNS).
        self.next_hello = clock.now() + opts.listen_first
        self.booting_until = None  # a ROM line was seen: quiet until the banner, or this
        self.hello_since = None
        self.busy_said = set()
        self.next_clock = 0.0
        self.result = None
        self.message = ""
        self.bye_acked = False
        self.fw = None
        self.entries = []        # the run's record (--log, --measure)
        self.log_file = open(opts.log, "w", encoding="utf-8") if opts.log else None
        self.t0 = clock.now()
        self.run_t = None        # when playback started (the faults' time base)
        self.faults = self._faults()
        self.held = []           # --jitter-ms: (release time, bytes), in order
        self.counts = {"h": 0, "c": 0, "e": 0, "dropped": 0, "errs": 0, "sessions": 0}
        self.sound = None        # --play: winsound
        self.tmpdir = None

    # -- records and messages --

    def say(self, text: str):
        print(f"usb_viz: {text}", file=self.out, flush=True)

    def record(self, entry: dict):
        entry.setdefault("t", round(self.clock.now() - self.t0, 4))
        self.entries.append(entry)
        if self.log_file:
            self.log_file.write(json.dumps(entry) + "\n")

    def send(self, text: str, now: float):
        data = encode_line(text)  # raises before anything unguarded is written
        if self.o.log:
            self.record({"kind": "tx", "text": text, "t": round(now - self.t0, 4)})
        if self.o.jitter_ms > 0 and not text.startswith(("@hello", "@bye")):
            last = self.held[-1][0] if self.held else now
            self.held.append((max(last, now + self.rng.uniform(0, self.o.jitter_ms / 1000.0)), data))
            return
        self.flush_held(float("inf"))
        self.port.write(data)

    def flush_held(self, now: float):
        while self.held and self.held[0][0] <= now:
            self.port.write(self.held.pop(0)[1])

    # -- the faults (each tests one rule) --

    def _faults(self) -> list:
        o, out = self.o, []
        if o.pause_at:
            at, length = (float(v) for v in o.pause_at.split(":"))
            out += [(at, "pause", None), (at + length, "resume", None)]
        if o.seek_at:
            parts = o.seek_at.split(":")
            out.append((float(parts[0]), "seek", float(parts[1]) if len(parts) > 1 else None))
        if o.gap_at:
            out.append((float(o.gap_at), "gap", None))
        return sorted(out, key=lambda f: f[0])

    def run_faults(self, now: float):
        p = self.player
        while self.faults and now - self.run_t >= self.faults[0][0]:
            _, kind, arg = self.faults.pop(0)
            if kind == "pause":
                p.pause(now)
                self.say(f"paused at epoch frame {int(p.anchor_pos)} (the same epoch carries on after)")
            elif kind == "resume":
                p.resume(now)
                self.say("resumed")
            elif kind == "seek":
                pos = p.start + int(p.position(now))
                to = int(arg * p.rate) if arg is not None else pos + 10 * p.rate
                to = max(0, min(to, p.src.frames - HOP))
                self.start_epoch(now, p.index, to, f"seek to {to / p.rate:.2f} s")
            elif kind == "gap":
                n = int(0.5 * p.rate) // HOP
                p.skip_hops(n)
                self.say(f"dropped 0.5 s of audio before the front end: hops {p.next_hop - n}-{p.next_hop - 1} "
                         "not sent, the filters reset")

    # -- the session --

    def on_line(self, t: float, text):
        if text is None:  # the port failed (unplugged)
            self.stop(1, "the serial port went away")
            return
        p = self.player
        entry = {"kind": "rx", "text": text, "t": round(t - self.t0, 4)}
        if self.state == STREAM and p.started:
            entry["epoch"], entry["heard"] = p.epoch, p.heard(t)
        if self.o.log or self.o.measure:
            self.record(entry)
        if not text.startswith("@"):
            if not self.o.quiet and text:
                print(f"core2| {text}", file=self.out, flush=True)
            if text.startswith(ROM_SIGNS):
                if self.booting_until is None:
                    self.say("the Core2 is booting: quiet until its banner")
                self.booting_until = t + BOOT_WAIT
            elif text.startswith(BANNER) and self.booting_until is not None:
                self.booting_until = None
                self.next_hello = min(self.next_hello, t)  # it reads from the first byte now
            if self.state == STREAM and text.startswith(REBOOT_SIGNS):
                self.new_session(t, "the Core2 rebooted")
            return
        f = text.split()
        verb = f[0]
        if verb == "@ok":
            if self.state == HELLO and len(f) >= 3 and f[2] == self.session:
                self.established(t, f)
            return
        if verb == "@bye":
            why = f[1] if len(f) > 1 else ""
            if why == "ok":
                self.bye_acked = True
            elif why == "user":
                self.stop(3, "Stopped on the Core2 (a touch or a button there). Not retrying: run it again to "
                             "start over.")
            elif why == "dance":
                self.stop(3, "The Core2's Dance tab went away; stopping.")
            elif why == "timeout" and self.state == STREAM:
                self.new_session(t, "the Core2 heard nothing for 3 s")
            return
        if verb == "@err":
            self.on_error(t, f)

    def on_error(self, t: float, f: list):
        code = int(f[1]) if len(f) > 1 and f[1].isdigit() else 0
        about = f[2] if len(f) > 2 else "-"
        detail = f[3] if len(f) > 3 else ""
        self.counts["errs"] += 1
        if code == 4 and about == "hello":
            if detail == "dance":
                self.stop(1, "The Core2 can't dance (no PSRAM for the dancer).")
            elif detail not in self.busy_said:
                self.busy_said.add(detail)
                self.say(f"the Core2 is busy ({detail}); asking again every second")
        elif code == 2:
            self.stop(1, f"The Core2 speaks protocol {detail or '?'}, this sender {PROTO}: update one side.")
        elif code == 7 and about == "hello":
            self.stop(1, "The Core2's firmware has no visualizer: update it.")
        elif code == 8:
            self.stop(3, "Stopped on the Core2 (it declines until this sender stops). Not retrying.")
        elif code == 3 and self.state == STREAM:
            self.new_session(t, "the Core2 has no session (it rebooted or timed out)")
        elif code == 9 and self.state == STREAM:
            self.send_epoch(t)
        else:
            self.say(f"the Core2 refused a line: {' '.join(f)} (a bug in this sender)")

    def new_session(self, t: float, why: str):
        self.say(f"{why}: starting a new session")
        self.state = HELLO
        self.session = new_session_id()
        self.next_hello = t
        self.hello_since = None

    def established(self, arrived: float, f: list):
        t = self.clock.now()  # (the play clock starts now, not when the line came in)
        self.state = STREAM
        self.counts["sessions"] += 1
        self.fw = f[3] if len(f) > 3 else "?"
        caps = f[4] if len(f) > 4 else ""
        self.say(f"Core2 {self.fw} (protocol {f[1]}, caps {caps}): dancing to this computer, session {self.session}")
        if self.o.measure:
            self.send(f"@log {2 if self.o.flash else 1}", t)
        if not self.player.started:
            self.run_t = t
            self.start_epoch(t, 0, 0, None)
        else:
            # The same epoch again. Hops for audio already played while there
            # was no session are skipped, not sent in a burst: the first hop
            # sent restarts the Core2's tracker anyway.
            p = self.player
            behind = int(p.position(t)) // HOP - p.next_hop
            if behind > 0:
                p.skip_hops(behind)
            self.send_epoch(t)
        self.next_clock = t

    def start_epoch(self, t: float, index: int, start: int, why):
        p = self.player
        p.new_epoch(t, index, start)
        self.record(p.truth())
        if why:
            self.say(f"epoch {p.epoch}: {why}")
        else:
            self.say(f"epoch {p.epoch}: {p.src.name}, {p.epoch_frames() / p.rate:.1f} s at {p.rate} Hz")
        if self.o.play:
            self.play_audio(t, p.src, start)
        self.send_epoch(t)
        self.next_clock = t

    def send_epoch(self, t: float):
        p = self.player
        self.send(line_epoch(p.epoch, p.rate, self.o.prior), t)
        self.counts["e"] += 1

    def stop(self, code: int, message: str):
        if self.result is None:
            self.result = code
            self.message = message

    # -- --play --

    def play_audio(self, t: float, src: Source, start: int):
        import winsound  # type: ignore

        if start != 0:
            raise RuntimeError("--play can't seek")
        if src.wav_path is None:
            if self.tmpdir is None:
                self.tmpdir = tempfile.TemporaryDirectory(prefix="usb_viz_")
            src.wav_path = Path(self.tmpdir.name) / f"{len(os.listdir(self.tmpdir.name))}.wav"
            write_wav(src.wav_path, src, self.o.gain_db)
        winsound.PlaySound(str(src.wav_path), winsound.SND_FILENAME | winsound.SND_ASYNC | winsound.SND_NODEFAULT)
        self.sound = winsound
        # Anchored when the call returns; the output's own delay is --offset-ms.
        p = self.player
        p.anchor_t = self.clock.now()

    def stop_audio(self):
        if self.sound:
            self.sound.PlaySound(None, 0)
            self.sound = None

    # -- the stream --

    def stream(self, now: float) -> bool:
        """One pass: faults, the epoch's end, the hops due, @c. True when
        everything has played."""
        p = self.player
        self.run_faults(now)
        while p.playing and now >= p.end_time():
            if p.index + 1 >= len(p.sources):
                self.send(line_clock(p.epoch, p.heard(now), False), now)
                return True
            # The next source starts the moment this one ends.
            t_end = p.end_time()
            self.start_epoch(t_end if not self.o.play else now, p.index + 1, 0, None)
        # The hops, in bursts of --batch frames, once they are --lead-ms ahead of the position.
        lead = self.o.lead_ms * p.rate / 1000.0
        avail = int((p.position(now) + lead) // self.o.batch) * self.o.batch
        avail = min(avail, p.epoch_frames())
        while (p.next_hop + 1) * HOP <= avail:
            hop = p.next_hop
            low, mid = p.compute_hop()
            if self.o.drop and self.rng.random() < self.o.drop:
                self.counts["dropped"] += 1
                continue
            self.send(line_hop(p.epoch, hop, low, mid), now)
            self.counts["h"] += 1
        if now >= self.next_clock:
            self.send(line_clock(p.epoch, p.heard(now), p.playing), now)
            self.counts["c"] += 1
            self.next_clock += 1.0 / self.o.clock_hz
            if self.next_clock < now:  # fell behind (a stall): from now on
                self.next_clock = now + 1.0 / self.o.clock_hz
        return False

    def run(self) -> int:
        try:
            self._loop()
        except KeyboardInterrupt:
            self.say("Ctrl-C: saying bye")
            if self.result is None:
                self.result = 0
        finally:
            self.finish()
        return self.result if self.result is not None else 0

    def _loop(self):
        tick = 0.005
        while True:
            now = self.clock.now()
            for t, text in self.port.poll():
                self.on_line(t, text)
                if self.result is not None:
                    return
            if self.state == HELLO:
                if self.hello_since is None:
                    self.hello_since = now
                if now - self.hello_since > self.o.hello_timeout:
                    self.stop(1, f"No answer from the Core2 in {self.o.hello_timeout:.0f} s. Is it running this "
                                 "firmware (v0.5.0 or later with the visualizer), and is pio monitor closed?")
                    return
                if self.booting_until is not None and now >= self.booting_until:
                    self.say(f"no banner {BOOT_WAIT:.0f} s after the Core2's ROM lines: asking anyway")
                    self.booting_until = None
                if now >= self.next_hello and self.booting_until is None:
                    self.send(line_hello(self.session), now)
                    self.hello_sent = True
                    self.next_hello = now + 1.0
            elif self.state == STREAM:
                if self.stream(now):
                    self.stop(0, "")
                    return
            self.flush_held(now)
            self.clock.sleep(tick)

    def finish(self):
        self.stop_audio()
        try:
            self.flush_held(float("inf"))
            if self.hello_sent and self.result != 3:
                now = self.clock.now()
                self.send("@bye", now)
                # The answer, if it comes (the port is free when it does).
                deadline = now + 1.0
                while not self.bye_acked and self.clock.now() < deadline:
                    for t, text in self.port.poll():
                        if text is not None and text.startswith("@bye ok"):
                            self.bye_acked = True
                        elif text is not None and (self.o.log or self.o.measure):
                            self.record({"kind": "rx", "text": text, "t": round(t - self.t0, 4)})
                        if text is not None and not text.startswith("@") and text and not self.o.quiet:
                            print(f"core2| {text}", file=self.out, flush=True)
                    self.clock.sleep(0.01)
        except Exception as e:  # the port went away: nothing to say bye on
            self.say(f"couldn't say bye: {e}")
        finally:
            try:
                self.port.close()
            except Exception:
                pass
            if self.tmpdir is not None:
                self.tmpdir.cleanup()
        c = self.counts
        dropped = f", left out {c['dropped']} hops (--drop)" if c["dropped"] else ""
        self.say(f"sent {c['h']} hops, {c['c']} clocks, {c['e']} epoch lines in {c['sessions']} session(s)"
                 f"{dropped}; {c['errs']} @err replies")
        if self.message:
            self.say(self.message)
        if self.log_file:
            self.log_file.close()


# ---- scoring a run (--measure, --score) ----

_RESET = re.compile(r"\[dance\] tracker reset \(computer: epoch (\d+),")
# (The epoch in [dance] locked and [beat] lines: firmware since the reset
# lines' rate limit spared an epoch's first hop. Older lines lack it.)
_LOCKED = re.compile(r"\[dance\] locked: ([0-9.]+) BPM, ([0-9.]+) s after the reset"
                     r"(?: \(computer: epoch (\d+)\))?")
_BEAT = re.compile(r"\[beat\] #-?\d+ next at -?[0-9.]+s \((?:epoch (\d+), )?frame (-?[0-9.]+), hop \d+\) "
                   r"bpm=([0-9.]+) conf=[0-9.]+( locked)?")
_FLASH = re.compile(r"\[flash\] #-?\d+ heard=(-?\d+) aim=-?\d+")
_STATS = re.compile(r"\| viz .*?hops=(\d+) gaps=(\d+) dup=(\d+) stale=(\d+) bad=(\d+) errs=(\d+) "
                    r"\| clock \w+ age=\d+ms snaps=(\d+) slew=[-+0-9.]+% spread=([0-9.]+)ms")


def percentile(values: list, p: float) -> float:
    """The p-th percentile (0-100), linear between the nearest ranks."""
    v = sorted(values)
    if not v:
        return float("nan")
    k = (len(v) - 1) * p / 100.0
    lo, hi = int(math.floor(k)), int(math.ceil(k))
    return v[lo] + (v[hi] - v[lo]) * (k - lo)


def phase_error(frame: float, beats: list, period: float) -> float:
    """Frames from the nearest true beat, folded into +-half the true period
    (as BeatTracker::Grid::errorAgainst() does)."""
    i = bisect.bisect_left(beats, frame)
    near = [beats[j] for j in (i - 1, i) if 0 <= j < len(beats)]
    d = min((frame - b for b in near), key=abs)
    return d - period * math.floor(d / period + 0.5)


def octave_error(bpm: float, truth: float) -> float:
    """The tempo's error in percent, octave-folded (87 against 174 is 0)."""
    if bpm <= 0 or truth <= 0:
        return float("nan")
    r = bpm / truth
    while r >= math.sqrt(2.0):
        r /= 2.0
    while r < math.sqrt(0.5):
        r *= 2.0
    return abs(r - 1.0) * 100.0


def score(entries: list) -> list:
    """Per epoch, from a run's record: the lock time and tempo, the phase
    error of every [beat] line from the lock on, the [flash] lines against
    this computer's clock at arrival, and the last [dance] line's counters."""
    epochs, order = {}, []
    device_epoch = None
    for e in entries:
        if e.get("kind") == "epoch":
            ep = {"truth": e, "lock": None, "lock_bpm": None, "errors": [], "bpms": [], "flash": [], "resets": 0,
                  "stats": None}
            epochs[e["epoch"]] = ep
            order.append(e["epoch"])
            continue
        if e.get("kind") != "rx":
            continue
        text = e["text"]
        m = _RESET.search(text)
        if m:
            device_epoch = int(m.group(1))
            if device_epoch in epochs:
                epochs[device_epoch]["resets"] += 1
            continue
        lock, beat = _LOCKED.search(text), _BEAT.search(text)
        # Whose line: the epoch it names, else the last reset's, else this
        # computer's at its arrival.
        named = (lock and lock.group(3)) or (beat and beat.group(1))
        ep = epochs.get(int(named) if named else device_epoch if device_epoch is not None else e.get("epoch"))
        if ep is None:
            continue
        truth = ep["truth"]
        if lock:
            if ep["lock"] is None:
                ep["lock_bpm"], ep["lock"] = float(lock.group(1)), float(lock.group(2))
            continue
        if beat:
            if beat.group(4) and truth.get("beats") and truth.get("period"):
                frame = float(beat.group(2))
                ep["errors"].append(phase_error(frame, truth["beats"], truth["period"]) * 1000.0 / truth["rate"])
                ep["bpms"].append(float(beat.group(3)))
            continue
        m = _FLASH.search(text)
        if m and e.get("epoch") == truth["epoch"] and "heard" in e:
            ep["flash"].append((int(m.group(1)) - e["heard"]) * 1000.0 / truth["rate"])
            continue
        m = _STATS.search(text)
        if m:
            ep["stats"] = dict(zip(("hops", "gaps", "dup", "stale", "bad", "errs", "snaps"),
                                   (int(v) for v in m.groups()[:7])))
            ep["stats"]["spread"] = float(m.group(8))
    rows = []
    for n in order:
        ep = epochs[n]
        t = ep["truth"]
        absd = [abs(v) for v in ep["errors"]]
        bpm = percentile(ep["bpms"], 50) if ep["bpms"] else (ep["lock_bpm"] or float("nan"))
        rows.append({
            "epoch": n, "source": t["source"], "rate": t["rate"], "lock": ep["lock"], "bpm": bpm,
            "bpm_err": octave_error(bpm, t.get("bpm") or 0) if bpm == bpm else float("nan"),
            "beats": len(absd), "med": percentile(absd, 50), "p95": percentile(absd, 95),
            "mean": sum(ep["errors"]) / len(absd) if absd else float("nan"),
            "flash_med": percentile(ep["flash"], 50),
            "flash_spread": percentile(ep["flash"], 95) - percentile(ep["flash"], 5),
            "flashes": len(ep["flash"]), "resets": ep["resets"], "stats": ep["stats"],
        })
    return rows


def format_table(rows: list) -> str:
    def f(v, spec):
        return "-" if v is None or v != v else format(v, spec)

    head = ("epoch", "source", "lock s", "bpm", "bpm err%", "beats", "med ms", "p95 ms", "mean ms", "flash med",
            "flash 5-95", "resets", "gaps", "bad")
    lines = ["  ".join(head)]
    for r in rows:
        s = r["stats"] or {}
        lines.append("  ".join([
            str(r["epoch"]), r["source"], f(r["lock"], ".2f"), f(r["bpm"], ".2f"), f(r["bpm_err"], ".2f"),
            str(r["beats"]), f(r["med"], ".1f"), f(r["p95"], ".1f"), f(r["mean"], "+.1f"),
            f(r["flash_med"], "+.1f") if r["flashes"] else "-", f(r["flash_spread"], ".1f") if r["flashes"] else "-",
            str(r["resets"]), str(s.get("gaps", "-")), str(s.get("bad", "-")),
        ]))
    return "\n".join(lines)


_EXPECT_KEYS = {"lock": "lock", "bpm": "bpm_err", "med": "med", "p95": "p95", "spread": "flash_spread"}


def check_expect(rows: list, expect: str) -> list:
    """'lock<=4,bpm<=0.5,med<=10,p95<=25' against every epoch with a truth;
    returns the failures (a missing value fails)."""
    fails = []
    for item in filter(None, (s.strip() for s in expect.split(","))):
        m = re.fullmatch(r"(\w+)\s*<=\s*([0-9.]+)", item)
        if not m or m.group(1) not in _EXPECT_KEYS:
            raise SystemExit(f"--expect: can't read '{item}' (keys: {', '.join(_EXPECT_KEYS)}; form key<=number)")
        key, limit = _EXPECT_KEYS[m.group(1)], float(m.group(2))
        for r in rows:
            v = r[key]
            if v is None or v != v or v > limit:
                fails.append(f"epoch {r['epoch']} ({r['source']}): {m.group(1)}={v} (want <= {limit:g})")
    return fails


# ---- --selftest ----


def selftest(out=None) -> bool:
    """The port against test/test_hop_feed/hop_golden.h, and the line guard."""
    out = out if out is not None else sys.stdout
    ok = True

    def report(good: bool, what: str):
        nonlocal ok
        ok = ok and good
        print(f"  {'ok  ' if good else 'FAIL'} {what}", file=out)

    print(f"selftest against {GOLDEN.relative_to(ROOT)}:", file=out)
    for g in read_golden():
        name = f"{g['name']} at {g['rate']} Hz"
        dc, lp1, lp2 = front_end_coefficients(g["rate"])
        same = f32(dc) == f32(g["dc_pole"]) and all(f32(a) == f32(b) for a, b in zip(lp1 + lp2, g["lp1"] + g["lp2"]))
        report(same, f"{name}: the DC pole and biquad coefficients (as float32)")
        gen = ClickGen(g["bpm"], g["offset"], g["rate"], g["frames"])
        report(gen.beats() == g["beats"], f"{name}: the click's beat frames ({len(g['beats'])})")
        pcm = gen.stereo()
        report(fnv1a_mono(pcm) == g["mono_fnv1a"], f"{name}: the click's samples (FNV-1a {fnv1a_mono(pcm):#010x})")
        fe = HopFrontEnd(g["rate"])
        hops = fe.process(mono_of(pcm, 0, g["frames"]))
        worst, exact = 0.0, 0
        for (a, b), (ga, gb) in zip(hops, g["hops"]):
            exact += f32(a) == f32(ga) and f32(b) == f32(gb)
            for v, w in ((a, ga), (b, gb)):
                if abs(v - w) > 1e-12:
                    worst = max(worst, abs(v - w) / abs(w))
        report(len(hops) == len(g["hops"]) and worst <= 1e-5,
               f"{name}: {len(hops)} hops within 1e-5 relative (or 1e-12 absolute): worst {worst:.2g}, "
               f"{exact} bit-exact")
        # Through the text the line carries: every energy back to the same float32.
        same = all(f32(float(fmt_energy(v))) == f32(v) for h in hops for v in h)
        report(same, f"{name}: the energies through '%.9g' read back to the same float32")
    refused = ["", "@", "h 1 2 3 4", "x@hello", "@e 1 44100 0\n", "@e 1\r", "@c 1 2\t1", "@h \x00", "@café",
               "@" + "x" * 255, " @bye"]
    report(all(_refuses(s) for s in refused), f"the line guard refuses {len(refused)} lines that aren't '@' lines")
    report(encode_line("@" + "x" * 254) == b"@" + b"x" * 254 + b"\n", "the line guard passes a 255-byte line")
    print("selftest: " + ("passed" if ok else "FAILED"), file=out)
    return ok


def _refuses(text: str) -> bool:
    try:
        encode_line(text)
    except ValueError:
        return True
    return False


# ---- the command line ----


def pick_port(name: str) -> str:
    """'auto': the one port with a Core2's USB bridge (CH9102 or CP2104)."""
    if name != "auto":
        return name
    try:
        from serial.tools import list_ports  # type: ignore
    except ImportError:
        raise SystemExit("pyserial isn't installed: pip install pyserial")
    hits = [p.device for p in list_ports.comports() if (p.vid, p.pid) in CORE2_BRIDGES]
    if len(hits) != 1:
        raise SystemExit(f"--port auto: {len(hits)} ports with a Core2's USB bridge ({', '.join(hits) or 'none'}); "
                         "name one with --port")
    return hits[0]


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 epilog="docs/USB-VISUALIZER.md has the protocol and the device test plan.")
    src = ap.add_argument_group("what plays (an epoch each, in this order: clicks, then files)")
    src.add_argument("--click", action="append", default=[], metavar="BPM",
                     help="a click track (ClickGen's): 120, or 120off (the first beat 0.37 of a beat late)")
    src.add_argument("--rate", type=int, default=44100, choices=RATES, help="the click tracks' rate")
    src.add_argument("--wav", "--file", dest="files", action="append", default=[], type=Path, metavar="FILE",
                     help="a 16/24-bit WAV at 44.1 or 48 kHz; anything else (MP3, FLAC) through ffmpeg")
    src.add_argument("--beats", action="append", default=[], metavar="JSON",
                     help="the truth for the --wav of the same position: a list of beat times in seconds")
    src.add_argument("--seconds", type=float, help="each source's length (clicks: default 60)")
    src.add_argument("--prior", type=float, default=0.0, help="the BPM prior sent with each epoch (0: none)")
    link = ap.add_argument_group("the link")
    link.add_argument("--port", help="the Core2's serial port (COM3), or 'auto' (the one Core2 bridge)")
    link.add_argument("--dry-run", action="store_true", help="no port: the lines go to stdout, a fake Core2 answers")
    link.add_argument("--fast", action="store_true", help="with --dry-run: virtual time, as fast as it computes")
    link.add_argument("--quiet", action="store_true", help="don't show the Core2's log lines")
    link.add_argument("--log", metavar="FILE", help="record the run (lines both ways, the truth) as JSON lines")
    out = ap.add_argument_group("sound and timing")
    out.add_argument("--silent", action="store_true", help="play nothing here (the default)")
    out.add_argument("--play", action="store_true", help="play the audio on this computer (Windows), from the start")
    out.add_argument("--gain-db", type=float, default=-12.0, help="--play's level (default -12 dB)")
    out.add_argument("--offset-ms", type=float, default=0.0,
                     help="this computer's output delay: heard = played - offset (positive: the crab later)")
    out.add_argument("--lead-ms", type=float, default=150.0, help="how far the hops run ahead of the play position")
    out.add_argument("--batch", type=int, default=2048, help="hops go out in bursts of this many frames")
    out.add_argument("--clock-hz", type=float, default=10.0, help="@c lines a second (5-20)")
    tests = ap.add_argument_group("measuring and faults")
    tests.add_argument("--measure", action="store_true", help="ask for the Core2's [beat] log and score it")
    tests.add_argument("--flash", action="store_true", help="with --measure: [flash] lines too (the heard clock)")
    tests.add_argument("--expect", help="with --measure: thresholds for the exit status, "
                                        "'lock<=4,bpm<=0.5,med<=10,p95<=25' (also spread<=: the flashes' 5-95 %%)")
    tests.add_argument("--score", metavar="FILE", help="score a --log file and exit")
    tests.add_argument("--pause-at", metavar="S:LEN", help="pause S s into the run for LEN s (the same epoch)")
    tests.add_argument("--seek-at", metavar="S[:TO]", help="seek S s into the run, to TO s (default: 10 s on)")
    tests.add_argument("--gap-at", type=float, metavar="S", help="lose 0.5 s of audio before the front end at S s")
    tests.add_argument("--drop", type=float, default=0.0, metavar="P", help="leave out that share of the hops")
    tests.add_argument("--jitter-ms", type=float, default=0.0, metavar="N", help="hold each line back 0-N ms")
    tests.add_argument("--seed", type=int, default=1, help="for --drop and --jitter-ms")
    tests.add_argument("--hello-timeout", type=float, default=20.0, help=argparse.SUPPRESS)
    tests.add_argument("--listen-first", type=float, default=0.3, help=argparse.SUPPRESS)
    tests.add_argument("--selftest", action="store_true", help="check the port against the golden file and exit")
    return ap


def build_sources(opts) -> list:
    seconds = opts.seconds
    sources = [click_source(c, opts.rate, seconds or 60.0) for c in opts.click]
    for i, path in enumerate(opts.files):
        sources.append(file_source(path, seconds, opts.beats[i] if i < len(opts.beats) else None))
    for s in sources:
        if s.frames < HOP:
            raise SystemExit(f"{s.name}: too short")
    return sources


def main(argv=None) -> int:
    for stream in (sys.stdout, sys.stderr):  # (a Core2 line a redirected console can't encode mustn't stop it)
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(errors="replace")
    ap = build_parser()
    opts = ap.parse_args(argv)
    if opts.selftest:
        return 0 if selftest() else 1
    if opts.score:
        entries = [json.loads(line) for line in Path(opts.score).read_text(encoding="utf-8").splitlines() if line]
        rows = score(entries)
        print(format_table(rows))
        fails = check_expect(rows, opts.expect) if opts.expect else []
        for f in fails:
            print(f"usb_viz: FAIL {f}")
        return 1 if fails else 0
    if opts.play and opts.silent:
        ap.error("--play or --silent, not both")
    if opts.play and (opts.pause_at or opts.seek_at):
        ap.error("--play can't pause or seek (winsound plays a file from its start); use them silent")
    if opts.play and sys.platform != "win32":
        ap.error("--play uses winsound: Windows only")
    if opts.fast and not opts.dry_run:
        ap.error("--fast is for --dry-run")
    if not 5.0 <= opts.clock_hz <= 20.0:
        ap.error("--clock-hz: 5 to 20")
    if opts.batch <= 0 or opts.batch % HOP:
        ap.error(f"--batch: a multiple of {HOP}")
    if opts.prior and not 30.0 <= opts.prior <= 300.0:
        ap.error("--prior: 30 to 300 BPM, or 0")
    if (opts.flash or opts.expect) and not opts.measure:
        ap.error("--flash and --expect go with --measure")
    if not opts.click and not opts.files:
        ap.error("nothing to play: --click BPM or --wav FILE")
    if not opts.dry_run and not opts.port:
        ap.error("--port COM3 (or --port auto), or --dry-run")
    sources = build_sources(opts)
    if opts.play:
        print(f"usb_viz: --play: this computer plays the audio at {opts.gain_db:+.0f} dB. Turn the volume down "
              "first.", flush=True)
    clock = VirtualClock() if opts.fast else RealClock()
    if opts.dry_run:
        port = DryRunPort(clock, out=sys.stdout)
    else:
        port = SerialPort(pick_port(opts.port), clock)
    # (In a dry run stdout carries only the lines; the rest goes to stderr.)
    sender = Sender(opts, sources, port, clock, out=sys.stderr if opts.dry_run else sys.stdout)
    code = sender.run()
    if opts.measure:
        rows = score(sender.entries)
        print(format_table(rows), file=sender.out)
        if opts.expect:
            fails = check_expect(rows, opts.expect)
            for f in fails:
                print(f"usb_viz: FAIL {f}", file=sender.out)
            if fails and code == 0:
                code = 1
    return code


if __name__ == "__main__":
    sys.exit(main())
