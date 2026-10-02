# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Tests for tools/usb_viz.py (the USB visualizer's reference sender).

    python -m unittest discover -s tools -p "test_usb_viz.py"

No port, no sound: the session runs in virtual time against usb_viz's
FakeCore2, which checks every line's grammar and the hop numbering.
"""
import array
import contextlib
import io
import math
import re
import sys
import tempfile
import unittest
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import usb_viz as uv  # noqa: E402

LINE_STREAM = re.compile(rb"(@[ -~]{1,255}\n)*")


class RecordingPort(uv.DryRunPort):
    """A dry-run port that keeps every byte written, with its time."""

    def __init__(self, clock, device=None):
        super().__init__(clock, device or uv.FakeCore2(fw="v9.9.9-test"))
        self.data = b""
        self.writes = []

    def write(self, data: bytes):
        self.data += data
        self.writes.append((self.clock.now(), data.decode("ascii").rstrip("\n")))
        super().write(data)


def run_sender(args, device=None, start=100.0):
    """The sender in virtual time: (exit code, port, sender, its messages)."""
    opts = uv.build_parser().parse_args(["--dry-run", "--fast", "--quiet"] + args)
    clock = uv.VirtualClock(start)
    port = RecordingPort(clock, device)
    out = io.StringIO()
    sender = uv.Sender(opts, uv.build_sources(opts), port, clock, out=out)
    code = sender.run()
    return code, port, sender, out.getvalue()


def sent(port, verb):
    return [(t, line) for t, line in port.writes if line.split(" ", 1)[0] == verb]


class FrontEndTest(unittest.TestCase):
    def test_golden_file_bit_exact(self):
        tracks = uv.read_golden()
        self.assertEqual([t["rate"] for t in tracks], [44100, 48000])
        for g in tracks:
            with self.subTest(rate=g["rate"]):
                dc, lp1, lp2 = uv.front_end_coefficients(g["rate"])
                self.assertEqual(uv.f32(dc), uv.f32(g["dc_pole"]))
                self.assertEqual([uv.f32(v) for v in lp1 + lp2], [uv.f32(v) for v in g["lp1"] + g["lp2"]])
                gen = uv.ClickGen(g["bpm"], g["offset"], g["rate"], g["frames"])
                self.assertEqual(gen.beats(), g["beats"])
                pcm = gen.stereo()
                self.assertEqual(uv.fnv1a_mono(pcm), g["mono_fnv1a"])
                hops = uv.HopFrontEnd(g["rate"]).process(uv.mono_of(pcm, 0, g["frames"]))
                self.assertEqual(len(hops), g["hop_count"])
                # The stated tolerance is the spec's (1e-5 relative, 1e-12 absolute);
                # this port meets it with every hop bit-exact as float32.
                for i, ((a, b), (ga, gb)) in enumerate(zip(hops, g["hops"])):
                    for v, w in ((a, ga), (b, gb)):
                        self.assertTrue(abs(v - w) <= 1e-12 or abs(v - w) <= 1e-5 * abs(w), (i, v, w))
                        self.assertEqual(uv.f32(v), uv.f32(w), i)

    def test_chunking_doesnt_matter(self):
        gen = uv.ClickGen(128, 0.37, 44100, 44100 * 3)
        mono = uv.mono_of(gen.stereo(), 0, gen.frames)
        whole = uv.HopFrontEnd(44100).process(mono)
        fe = uv.HopFrontEnd(44100)
        pieces, i = [], 0
        for n in (1, 7, 777, 512, 3, 2048, 10**9):
            pieces += fe.process(mono[i:i + n])
            i += n
        self.assertEqual(pieces, whole)

    def test_reset(self):
        gen = uv.ClickGen(120, 0.0, 48000, 48000)
        mono = uv.mono_of(gen.stereo(), 0, gen.frames)
        fe = uv.HopFrontEnd(48000)
        first = fe.process(mono)
        fe.reset()
        self.assertEqual(fe.process(mono), first)

    def test_mono_is_the_firmware_mix(self):
        pcm = array.array("h", [-1, 0, 3, 4, -32768, -32768, 32767, 32767, -3, 0])
        self.assertEqual(uv.mono_of(pcm, 0, 5), [-1, 3, -32768, 32767, -2])  # (L + R) >> 1, floored
        self.assertEqual(uv.mono_of(pcm, 1, 3), [3, -32768])

    def test_rates(self):
        with self.assertRaises(ValueError):
            uv.HopFrontEnd(22050)

    def test_click_names(self):
        self.assertEqual(uv.ClickGen.parse("120"), (120.0, 0.0))
        self.assertEqual(uv.ClickGen.parse("click120off"), (120.0, 0.37))
        self.assertEqual(uv.ClickGen.parse("tone:click90"), (90.0, 0.0))
        self.assertIsNone(uv.ClickGen.parse("29"))
        self.assertIsNone(uv.ClickGen.parse("301"))
        self.assertIsNone(uv.ClickGen.parse("12x"))
        gen = uv.ClickGen(120, 0.37, 44100, 44100 * 2)
        self.assertEqual(gen.beats(), [8159, 30209, 52259, 74309])  # llround(0.37f * 22050) = 8159

    def test_lround_halves_away_from_zero(self):
        self.assertEqual([uv.lround(v) for v in (0.5, 1.5, 2.5, -0.5, -2.5, 2.4999)], [1, 2, 3, -1, -3, 2])

    def test_selftest_passes(self):
        out = io.StringIO()
        self.assertTrue(uv.selftest(out))
        self.assertIn("selftest: passed", out.getvalue())


class LineTest(unittest.TestCase):
    def test_guard_refuses_what_isnt_an_at_line(self):
        for bad in ("", "@", "e 1 44100 0", "x@hello", " @bye", "@e 1 44100 0\n", "@c 1\r2 1", "@c\t1", "@\x7f",
                    "@\x00", "@café", "@" + "x" * 255, None, b"@bye"):
            with self.subTest(line=bad), self.assertRaises(ValueError):
                uv.encode_line(bad)

    def test_guard_passes_a_line(self):
        self.assertEqual(uv.encode_line("@bye"), b"@bye\n")
        self.assertEqual(len(uv.encode_line("@" + "~" * 254)), 256)

    def test_formats(self):
        self.assertEqual(uv.line_hello("7f3a"), "@hello 1 7f3a viz")
        self.assertEqual(uv.line_epoch(2, 48000, 0), "@e 2 48000 0")
        self.assertEqual(uv.line_epoch(2, 44100, 87.0), "@e 2 44100 87")
        self.assertEqual(uv.line_epoch(2, 44100, 87.5), "@e 2 44100 87.5")
        self.assertEqual(uv.line_hop(1, 3, 0.0036861975677311420, 2.564430906204507e-05),
                         "@h 1 3 0.00368619757 2.56443091e-05")
        self.assertEqual(uv.line_clock(1, -6615, True), "@c 1 -6615 1")
        self.assertEqual(uv.line_clock(4, 1323000, False), "@c 4 1323000 0")

    def test_energy_field(self):
        grammar = re.compile(uv._F32_FIELD.pattern)
        for e in (0.0, 1e-10, -1.0, float("nan"), float("inf"), 2e4, 1e4, 1.49884555e-15, 0.0291152764, 64.0,
                  3.0e-9, 1234.5678):
            text = uv.fmt_energy(e)
            with self.subTest(e=e, text=text):
                self.assertTrue(grammar.fullmatch(text))
                v = float(text)
                self.assertTrue(0.0 <= v <= 1e4)
                if 0.0 < e <= 1e4:
                    self.assertEqual(uv.f32(v), uv.f32(e))  # the same float32 at the other end
                else:
                    self.assertIn(text, ("0", "10000"))
        self.assertEqual(uv.fmt_energy(1e-10), "1.00000001e-10")  # tiny, but exact: the tracker stays bit for bit
        self.assertEqual(uv.fmt_energy(1e-50), "0")  # under a float32's least: 0 there too

    def test_session_ids(self):
        ids = {uv.new_session_id() for _ in range(50)}
        self.assertEqual(len(ids), 50)
        self.assertTrue(all(re.fullmatch(r"[0-9A-Za-z]{1,16}", s) for s in ids))


class SessionTest(unittest.TestCase):
    def assert_clean(self, port):
        self.assertTrue(LINE_STREAM.fullmatch(port.data), "a byte outside an '@' line")
        self.assertEqual(port.device.problems, [])

    def test_a_whole_run(self):
        code, port, sender, _ = run_sender(["--click", "120", "--seconds", "3"])
        self.assertEqual(code, 0)
        self.assert_clean(port)
        lines = [line for _, line in port.writes]
        self.assertTrue(lines[0].startswith("@hello 1 "))
        self.assertEqual(lines[1], "@e 1 44100 0")  # before any @h or @c
        self.assertEqual(lines[-1], "@bye")
        hops = [int(l.split()[2]) for l in lines if l.startswith("@h ")]
        self.assertEqual(hops, list(range(3 * 44100 // 512)))
        self.assertTrue(sender.bye_acked)

    def test_hops_match_the_front_end(self):
        _, port, _, _ = run_sender(["--click", "120", "--seconds", "4"])
        g = uv.read_golden()[0]
        got = [tuple(float(v) for v in l.split()[3:5]) for _, l in sent(port, "@h")]
        self.assertEqual(len(got), g["hop_count"])
        for (a, b), (ga, gb) in zip(got, g["hops"]):
            self.assertEqual((uv.f32(a), uv.f32(b)), (uv.f32(ga), uv.f32(gb)))  # exactly the golden file's

    def test_clock_lines(self):
        _, port, _, _ = run_sender(["--click", "120", "--seconds", "3", "--offset-ms", "150"])
        clocks = sent(port, "@c")
        t0 = clocks[0][0]
        self.assertAlmostEqual(len(clocks), 31, delta=1)  # 10 Hz, and one at the end
        for t, line in clocks[:-1]:
            _, epoch, heard, playing = line.split()
            self.assertEqual(playing, "1")
            # heard = played - 150 ms (the anchor is the first @c's pass, within a tick)
            self.assertAlmostEqual(int(heard), (t - t0) * 44100 - 6615, delta=0.006 * 44100)
        gaps = [b[0] - a[0] for a, b in zip(clocks, clocks[1:-1])]
        self.assertTrue(all(0.095 <= g <= 0.106 for g in gaps), gaps)
        self.assertEqual(clocks[-1][1], f"@c 1 {3 * 44100 - 6615} 0")

    def test_hops_lead_the_play_position(self):
        _, port, _, _ = run_sender(["--click", "120", "--seconds", "3"])
        t0 = sent(port, "@e")[0][0]
        for t, line in sent(port, "@h"):
            hop = int(line.split()[2])
            played = (t - t0) * 44100
            self.assertLessEqual((hop + 1) * 512, played + 0.150 * 44100 + 1)  # never more than the lead ahead
            self.assertGreaterEqual((hop + 1) * 512, played - 2048 - 0.006 * 44100)  # nor a batch behind
        bursts = {}
        for t, _ in sent(port, "@h"):
            bursts[t] = bursts.get(t, 0) + 1
        self.assertLessEqual(max(bursts.values()), 16)

    def test_hello_until_ok(self):
        dev = uv.FakeCore2()
        dev.ignore_hellos = 2  # booting: lost
        dev.busy_hellos = 2    # then '@err 4 hello ui'
        code, port, _, msgs = run_sender(["--click", "120", "--seconds", "1"], dev)
        self.assertEqual(code, 0)
        hellos = sent(port, "@hello")
        self.assertEqual(len(hellos), 5)
        self.assertEqual(len({l for _, l in hellos}), 1)  # the same session id
        for a, b in zip(hellos, hellos[1:]):
            self.assertAlmostEqual(b[0] - a[0], 1.0, delta=0.006)
        self.assertEqual(msgs.count("busy (ui)"), 1)  # said once
        self.assertLess(port.writes.index(hellos[-1]), port.writes.index(sent(port, "@e")[0]))
        self.assert_clean(port)

    def test_gives_up_without_an_answer(self):
        dev = uv.FakeCore2()
        dev.ignore_hellos = 10**6
        code, port, _, msgs = run_sender(["--click", "120", "--hello-timeout", "3"], dev)
        self.assertEqual(code, 1)
        self.assertIn(len(sent(port, "@hello")), (3, 4))  # every second for 3 s
        self.assertIn("No answer from the Core2", msgs)
        self.assertEqual(sent(port, "@h"), [])

    def test_reboot_mid_stream(self):
        dev = uv.FakeCore2()
        dev.reboot_at = 102.0  # 2 s in: the ROM lines and the banner, the session gone
        dev.boot_hellos = 2    # and the next two hellos lost while it boots
        code, port, _, msgs = run_sender(["--click", "120", "--seconds", "8"], dev)
        self.assertEqual(code, 0)
        self.assert_clean(port)
        hellos = sent(port, "@hello")
        self.assertEqual(len(hellos), 4)
        self.assertEqual(len({l for _, l in hellos[1:]}), 1)  # one new session id, retried
        self.assertNotEqual(hellos[0][1], hellos[1][1])
        self.assertIn("rebooted", msgs)
        t_ok = hellos[-1][0]
        after = [(t, l) for t, l in port.writes if t >= t_ok][1:]
        self.assertEqual(after[0][1], "@e 1 44100 0")  # the same epoch, then its hop numbering
        hops = [(t, int(l.split()[2])) for t, l in after if l.startswith("@h ")]
        played = (t_ok - sent(port, "@e")[0][0]) * 44100  # since the play clock started
        # The hops of the 2 s without a session are skipped, not sent in a burst.
        self.assertGreaterEqual(hops[0][1] * 512, played - 512)
        self.assertLessEqual(sum(1 for t, _ in hops if t == hops[0][0]), 16)

    def test_listens_before_the_first_hello(self):
        code, port, _, _ = run_sender(["--click", "120", "--seconds", "1"])
        self.assertEqual(code, 0)
        self.assertGreaterEqual(port.writes[0][0], 100.3 - 1e-9)  # opening the port may have reset it
        self.assertTrue(port.writes[0][1].startswith("@hello "))

    def test_quiet_while_the_core2_boots(self):
        # The board resets before it answers (power-on, a reset on open):
        # nothing is written from its ROM lines until its banner, so no
        # line is under way when its Serial starts.
        for state, reboot in (("hello", 100.2), ("stream", 102.0)):
            dev = uv.FakeCore2()
            dev.reboot_at = reboot
            dev.banner_delay = 1.5
            if state == "hello":
                dev.ignore_hellos = 10 ** 6  # not answering yet: it's about to reset
                dev.boot_hellos = 0          # (the reset clears it)
            code, port, _, msgs = run_sender(["--click", "120", "--seconds", "4"], dev)
            self.assertEqual(code, 0, state)
            self.assert_clean(port)
            rom = [t for t, l in port.writes if reboot - 0.01 <= t]  # the device ticks on the sender's poll
            quiet = [(t, l) for t, l in port.writes if reboot + 0.01 < t < reboot + 1.5 - 1e-9]
            self.assertEqual(quiet, [], state)
            hellos = [t for t, l in sent(port, "@hello") if t > reboot]
            self.assertTrue(hellos, state)
            self.assertAlmostEqual(hellos[0], reboot + 1.5, delta=0.02)  # at the banner
            self.assertIn("booting", msgs)
            self.assertTrue(rom)

    def test_no_banner_asks_anyway(self):
        dev = uv.FakeCore2()
        dev.say_at = [(100.1, "ets Jul 29 2019 12:21:46")]  # a ROM line and nothing after
        code, port, _, msgs = run_sender(["--click", "120", "--seconds", "1"], dev)
        self.assertEqual(code, 0)
        self.assertAlmostEqual(port.writes[0][0], 100.1 + uv.BOOT_WAIT, delta=0.02)
        self.assertIn("asking anyway", msgs)

    def test_err_3_starts_over(self):
        dev = uv.FakeCore2()
        dev.say_at = [(101.0, "@err 3 h")]
        _, port, _, _ = run_sender(["--click", "120", "--seconds", "2"], dev)
        self.assertEqual(len({l for _, l in sent(port, "@hello")}), 2)

    def test_err_9_resends_the_epoch(self):
        dev = uv.FakeCore2()
        dev.say_at = [(101.0, "@err 9 c")]
        _, port, _, _ = run_sender(["--click", "120", "--seconds", "2"], dev)
        self.assertEqual([l for _, l in sent(port, "@e")], ["@e 1 44100 0"] * 2)
        self.assertEqual(len(sent(port, "@hello")), 1)

    def test_the_user_wins(self):
        dev = uv.FakeCore2()
        dev.decline_at = 101.0
        code, port, _, msgs = run_sender(["--click", "120", "--seconds", "5"], dev)
        self.assertEqual(code, 3)
        self.assertIn("Stopped on the Core2", msgs)
        self.assertTrue(all(t <= 101.006 for t, _ in port.writes))  # nothing after, not even @bye
        self.assertEqual(sent(port, "@bye"), [])

    def test_declined_err_8(self):
        dev = uv.FakeCore2()
        dev.say_at = [(100.5, "@err 8 h")]
        code, port, _, _ = run_sender(["--click", "120", "--seconds", "5"], dev)
        self.assertEqual(code, 3)
        self.assertTrue(all(t <= 100.506 for t, _ in port.writes))

    def test_version_and_feature_refusals(self):
        for reply, text in (("@err 2 hello 2-3", "protocol 2-3"), ("@err 7 hello", "no visualizer"),
                            ("@err 4 hello dance", "can't dance")):
            dev = uv.FakeCore2()
            dev.ignore_hellos = 10**6
            dev.say_at = [(100.2, reply)]
            code, port, _, msgs = run_sender(["--click", "120"], dev)
            with self.subTest(reply=reply):
                self.assertEqual(code, 1)
                self.assertIn(text, msgs)
                self.assertEqual(sent(port, "@e"), [])

    def test_pause_keeps_the_epoch(self):
        _, port, _, _ = run_sender(["--click", "120", "--seconds", "3", "--pause-at", "1:0.5"])
        self.assert_clean(port)  # the hops carry on with no gap
        self.assertEqual(len(sent(port, "@e")), 1)
        paused = [l for _, l in sent(port, "@c") if l.endswith(" 0")][:-1]
        self.assertAlmostEqual(len(paused), 5, delta=1)
        self.assertEqual(len({l for l in paused}), 1)  # heard stays put
        frozen = int(paused[0].split()[2])
        self.assertAlmostEqual(frozen, 44100, delta=0.006 * 44100)
        self.assertEqual(len(sent(port, "@h")), 3 * 44100 // 512)

    def test_seek_is_a_new_epoch(self):
        code, port, sender, _ = run_sender(["--click", "120", "--seconds", "4", "--seek-at", "1:3"])
        self.assertEqual(code, 0)
        self.assert_clean(port)
        self.assertEqual([l for _, l in sent(port, "@e")], ["@e 1 44100 0", "@e 2 44100 0"])
        ep2 = [l for _, l in sent(port, "@h") if l.startswith("@h 2 ")]
        self.assertEqual([int(l.split()[2]) for l in ep2], list(range(44100 // 512)))  # 1 s left after 3 s
        truth = [e for e in sender.entries if e["kind"] == "epoch"]
        self.assertEqual(truth[1]["start"], 3 * 44100)
        self.assertEqual(truth[1]["beats"][:3], [-22050, 0, 22050])  # (one before the epoch, for the nearest)

    def test_gap_skips_hop_numbers(self):
        _, port, _, _ = run_sender(["--click", "120", "--seconds", "3", "--gap-at", "1"])
        hops = [int(l.split()[2]) for _, l in sent(port, "@h")]
        jumps = [(a, b) for a, b in zip(hops, hops[1:]) if b != a + 1]
        self.assertEqual(len(jumps), 1)
        self.assertEqual(jumps[0][1] - jumps[0][0] - 1, int(0.5 * 44100) // 512)
        self.assertEqual(len(port.device.problems), 1)  # the fake Core2 sees the gap

    def test_drop_and_jitter(self):
        _, port, sender, _ = run_sender(["--click", "120", "--seconds", "3", "--drop", "0.1", "--jitter-ms", "30"])
        self.assertTrue(LINE_STREAM.fullmatch(port.data))
        self.assertGreater(sender.counts["dropped"], 5)
        self.assertEqual(len(sent(port, "@h")) + sender.counts["dropped"], 3 * 44100 // 512)
        hops = [int(l.split()[2]) for _, l in sent(port, "@h")]
        self.assertEqual(hops, sorted(hops))  # held back, never reordered

    def test_sources_in_turn(self):
        code, port, sender, _ = run_sender(["--click", "120", "--click", "90off", "--rate", "48000", "--seconds", "2",
                                            "--prior", "120"])
        self.assertEqual(code, 0)
        self.assert_clean(port)
        self.assertEqual([l for _, l in sent(port, "@e")], ["@e 1 48000 120", "@e 2 48000 120"])
        t_e2 = sent(port, "@e")[1][0]
        t_e1 = sent(port, "@e")[0][0]
        self.assertAlmostEqual(t_e2 - t_e1, 2.0, delta=0.006)
        self.assertEqual([e["source"] for e in sender.entries if e["kind"] == "epoch"],
                         ["click120@48000", "click90off@48000"])

    def test_measure_asks_for_the_beat_log(self):
        _, port, _, _ = run_sender(["--click", "120", "--seconds", "1", "--measure", "--flash"])
        lines = [l for _, l in port.writes]
        self.assertEqual(lines[1:3], ["@log 2", "@e 1 44100 0"])

    def test_ctrl_c_says_bye(self):
        opts = uv.build_parser().parse_args(["--dry-run", "--fast", "--quiet", "--click", "120"])
        clock = uv.VirtualClock(0.0)
        port = RecordingPort(clock)

        class Interrupting(uv.Sender):
            def stream(self, now):
                if now > 1.0:
                    raise KeyboardInterrupt
                return super().stream(now)

        sender = Interrupting(opts, uv.build_sources(opts), port, clock, out=io.StringIO())
        self.assertEqual(sender.run(), 0)
        self.assertEqual(port.writes[-1][1], "@bye")
        self.assertTrue(sender.bye_acked)

    def test_main_dry_run_stdout_is_only_lines(self):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = uv.main(["--dry-run", "--fast", "--click", "120off", "--seconds", "1"])
        self.assertEqual(code, 0)
        self.assertTrue(LINE_STREAM.fullmatch(out.getvalue().encode("ascii")))
        self.assertIn("usb_viz:", err.getvalue())

    def test_argument_checks(self):
        for args in (["--dry-run", "--click", "120", "--play", "--seek-at", "1"], ["--click", "120"],
                     ["--dry-run"], ["--dry-run", "--click", "120", "--clock-hz", "30"],
                     ["--dry-run", "--click", "120", "--prior", "20"], ["--dry-run", "--click", "120", "--expect", "x"],
                     ["--click", "120", "--port", "COM3", "--fast"]):
            with self.subTest(args=args), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as e:
                uv.main(args)
            self.assertEqual(e.exception.code, 2)


class PortTest(unittest.TestCase):
    def test_dtr_and_rts_low_before_open(self):
        events = []

        class FakeSerial:
            in_waiting = 0

            def __setattr__(self, name, value):
                events.append((name, value))
                object.__setattr__(self, name, value)

            def open(self):
                events.append(("open", None))

            def read(self, n):
                import time
                time.sleep(0.005)
                return b""

            def write(self, data):
                events.append(("write", data))

            def flush(self):
                pass

            def close(self):
                events.append(("close", None))

        port = uv.SerialPort("COM9", uv.RealClock(), serial_factory=FakeSerial)
        port.write(uv.encode_line("@bye"))
        port.close()
        names = [n for n, _ in events]
        self.assertEqual(dict(events[:names.index("open")]),
                         {"port": "COM9", "baudrate": 115200, "timeout": 0.05, "dtr": False, "rts": False})
        self.assertEqual([n for n in names[names.index("open"):] if n in ("dtr", "rts")], [])  # never touched after
        self.assertIn(("write", b"@bye\n"), events)

    def test_reader_splits_lines(self):
        chunks = [b"[dance] on (cr", b"ab)\r\n@ok 1 ab v1 viz,log\n", b"partial"]

        class FakeSerial:
            in_waiting = 0

            def open(self):
                pass

            def read(self, n):
                import time
                time.sleep(0.002)
                return chunks.pop(0) if chunks else b""

            def flush(self):
                pass

            def close(self):
                pass

        port = uv.SerialPort("COM9", uv.RealClock(), serial_factory=FakeSerial)
        import time
        deadline = time.time() + 2
        got = []
        while len(got) < 2 and time.time() < deadline:
            got += [text for _, text in port.poll()]
            time.sleep(0.01)
        port.close()
        self.assertEqual(got, ["[dance] on (crab)", "@ok 1 ab v1 viz,log"])


class ArrivalClock:
    """Virtual time for the reader thread alone (it is the only one that
    moves it)."""

    def __init__(self):
        self.t = 0.0

    def now(self):
        return self.t


class BatchingSerial:
    """pyserial's read(n) with a timeout, as on Windows (ReadTotalTimeout-
    Constant, no interval timeout): it returns when n bytes have come or the
    timeout ran out, whichever is first. `lines`: (when the Core2 printed
    it, text); its bytes go over the wire from then (or once the line
    before is out) at 115200 baud."""

    def __init__(self, clock, lines):
        self.clock = clock
        self.timeout = 0.05
        self.arrivals = []  # (time the byte is in, byte)
        self.started = []  # when each line's first byte went on the wire
        free = 0.0
        for printed, text in lines:
            data = (text + "\r\n").encode()
            start = max(printed, free)  # (behind the line before, if it's still going out)
            self.started.append(start)
            for k in range(len(data)):
                self.arrivals.append((start + (k + 1) * uv.BYTE_S, data[k:k + 1]))
            free = start + len(data) * uv.BYTE_S
        self.reads = 0

    @property
    def in_waiting(self):
        return sum(1 for t, _ in self.arrivals if t <= self.clock.t)

    def read(self, n):
        self.reads += 1
        deadline = self.clock.t + self.timeout
        if len(self.arrivals) >= n and self.arrivals[n - 1][0] <= deadline:
            self.clock.t = max(self.clock.t, self.arrivals[n - 1][0])  # the n-th byte is in
        else:
            self.clock.t = deadline
        k = min(n, self.in_waiting)
        out, self.arrivals = b"".join(b for _, b in self.arrivals[:k]), self.arrivals[k:]
        return out

    def open(self):
        pass

    def write(self, data):
        pass

    def flush(self):
        pass

    def close(self):
        pass


class ArrivalTest(unittest.TestCase):
    """Lines stamped when they came, not when a read's timeout ran out
    (that put [flash] up to 50 ms late: --measure --flash's spread)."""

    def stamps(self, lines):
        clock = ArrivalClock()
        fake = BatchingSerial(clock, lines)
        self.started = fake.started
        port = uv.SerialPort("COM9", clock, serial_factory=lambda: fake)
        import time
        deadline = time.time() + 5
        got = []
        while len(got) < len(lines) and time.time() < deadline:
            got += port.poll()
            time.sleep(0.005)
        port.close()
        return got

    def test_lines_stamped_when_they_came(self):
        lines = [(0.1003, "[flash] #12 heard=264600 aim=330750"),
                 (0.2317, "[beat] #13 next at 6.500s (epoch 1, frame 286650.0, hop 551) bpm=120.00 conf=0.83 locked"),
                 (0.2318, "[flash] #13 heard=286651 aim=352801"),
                 (0.4441, "@bye timeout")]
        got = self.stamps(lines)
        self.assertEqual([text for _, text in got], [text for _, text in lines])
        for start, (stamp, _) in zip(self.started, got):
            self.assertAlmostEqual(stamp, start, delta=0.0005)
        self.assertGreater(self.started[2], lines[2][0] + 0.005)  # (the [flash] waited for the [beat])

    def test_the_fake_batches_like_windows(self):
        # (A read of 4096 waits out its timeout: the old reader's 50 ms.)
        clock = ArrivalClock()
        fake = BatchingSerial(clock, [(0.01, "[flash] #1 heard=0 aim=0")])
        self.assertEqual(fake.read(4096), b"[flash] #1 heard=0 aim=0\r\n")
        self.assertAlmostEqual(clock.t, 0.05)


class SourceTest(unittest.TestCase):
    def write(self, path, rate, channels, width, frames):
        with wave.open(str(path), "wb") as w:
            w.setnchannels(channels)
            w.setsampwidth(width)
            w.setframerate(rate)
            w.writeframes(frames)

    def test_wav_16_24_and_mono(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "a.wav"
            self.write(p, 48000, 2, 2, array.array("h", [1, -2, 300, -400]).tobytes())
            self.assertEqual(uv.read_wav(p), (48000, array.array("h", [1, -2, 300, -400])))
            self.write(p, 44100, 1, 2, array.array("h", [5, -6]).tobytes())
            self.assertEqual(uv.read_wav(p), (44100, array.array("h", [5, 5, -6, -6])))
            # 24-bit: the top 16 bits (0x123456 -> 0x1234; -1 -> -1)
            self.write(p, 44100, 2, 3, bytes([0x56, 0x34, 0x12, 0xFF, 0xFF, 0xFF]))
            self.assertEqual(uv.read_wav(p), (44100, array.array("h", [0x1234, -1])))
            self.write(p, 22050, 2, 2, bytes(8))
            self.assertIsNone(uv.read_wav(p))  # another rate: ffmpeg's job

    def test_other_formats_need_ffmpeg(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "c.mp3"
            p.write_bytes(b"ID3")
            real = uv.find_ffmpeg
            uv.find_ffmpeg = lambda: None
            try:
                with self.assertRaises(SystemExit) as e:
                    uv.file_source(p)
            finally:
                uv.find_ffmpeg = real
            self.assertIn("without ffmpeg", str(e.exception))

    @unittest.skipIf(uv.find_ffmpeg() is None, "no ffmpeg here")
    def test_flac_through_ffmpeg(self):
        with tempfile.TemporaryDirectory() as d:
            gen = uv.ClickGen(120, 0.0, 48000, 48000)
            pcm = gen.stereo()
            wav, flac = Path(d) / "c.wav", Path(d) / "c.flac"
            self.write(wav, 48000, 2, 2, pcm.tobytes())
            uv.subprocess.run([uv.find_ffmpeg(), "-hide_banner", "-loglevel", "error", "-i", str(wav), str(flac)],
                              check=True)
            src = uv.file_source(flac)
            self.assertEqual((src.rate, src.frames), (48000, 48000))
            self.assertEqual(src.pcm, pcm)  # lossless: the same samples

    def test_write_wav_for_play(self):
        with tempfile.TemporaryDirectory() as d:
            src = uv.Source("x", 44100, array.array("h", [1000, -1000, 32767, -32768]))
            p = Path(d) / "x.wav"
            uv.write_wav(p, src, 20 * math.log10(0.5))
            self.assertEqual(uv.read_wav(p), (44100, array.array("h", [500, -500, 16384, -16384])))

    def test_file_source_with_beats(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "b.wav"
            self.write(p, 44100, 2, 2, bytes(4 * 44100))
            beats = Path(d) / "b.json"
            beats.write_text("[0.5, 1.0, 1.5]")
            src = uv.file_source(p, None, str(beats))
            self.assertEqual((src.rate, src.frames, src.beats), (44100, 44100, [22050, 44100, 66150]))
            self.assertEqual(src.period(), 22050.0)
            short = uv.file_source(p, 0.5)
            self.assertEqual(short.frames, 22050)


class ScoreTest(unittest.TestCase):
    def entries(self, error_ms=1.0, bpm=120.0, lock=2.7):
        rate = 44100
        gen = uv.ClickGen(120, 0.37, rate, rate * 20)
        e = [{"kind": "epoch", "epoch": 1, "rate": rate, "source": "click120off@44100", "start": 0,
              "frames": rate * 20, "bpm": 120.0, "period": gen.period, "beats": gen.beats()}]
        rx = lambda text, **kw: dict({"kind": "rx", "text": text, "t": 0.0}, **kw)
        e.append(rx("[dance] tracker reset (computer: epoch 1, its first hop) at hop 0, 44100 Hz"))
        e.append(rx("[beat] #3 next at 1.5s (frame 74309.0, hop 100) bpm=118.00 conf=0.20"))  # not locked: unscored
        e.append(rx(f"[dance] locked: {bpm:.2f} BPM, {lock:.2f} s after the reset"))
        for k, b in enumerate(gen.beats()[6:30]):
            frame = b + (error_ms if k % 2 else -error_ms) * rate / 1000.0
            e.append(rx(f"[beat] #{k + 6} next at {frame / rate:.3f}s (frame {frame:.1f}, hop {k}) bpm={bpm:.2f} "
                        f"conf=0.80 locked"))
            e.append(rx(f"[flash] #{k + 6} heard={int(b) + 88} aim={int(b) + 1500}", epoch=1, heard=int(b)))
        e.append(rx("[dance] on skin=crab fps=30.0/30 (dance) draw=9.0ms push=12.0ms | bpm=120.00 conf=0.80 locked "
                    "lock_after=2.70s err=n/a (the computer measures it) | viz epoch=1 44100Hz prior=0 hops=1722 "
                    "gaps=0 dup=0 stale=0 bad=0 errs=0 | clock ok age=43ms snaps=1 slew=+0.4% spread=6.0ms "
                    "offset=+0ms | tracker=1.30% resets=1 lost=0 | ram=60K min=50K"))
        return e

    def test_score(self):
        rows = uv.score(self.entries())
        self.assertEqual(len(rows), 1)
        r = rows[0]
        self.assertEqual((r["lock"], r["beats"], r["resets"]), (2.7, 24, 1))
        self.assertAlmostEqual(r["med"], 1.0, places=2)
        self.assertAlmostEqual(r["p95"], 1.0, places=2)
        self.assertAlmostEqual(r["bpm_err"], 0.0)
        self.assertAlmostEqual(r["flash_med"], 88 * 1000 / 44100, places=3)
        self.assertEqual(r["stats"]["gaps"], 0)
        self.assertEqual(r["stats"]["snaps"], 1)
        self.assertIn("click120off@44100", uv.format_table(rows))
        self.assertEqual(uv.check_expect(rows, "lock<=4,bpm<=0.5,med<=10,p95<=25,spread<=10"), [])
        self.assertEqual(len(uv.check_expect(rows, "lock<=2,med<=0.5")), 2)

    def test_score_half_tempo_and_misses(self):
        r = uv.score(self.entries(bpm=60.0))[0]
        self.assertAlmostEqual(r["bpm_err"], 0.0)  # octave-folded
        r = uv.score(self.entries(error_ms=30.0, lock=5.0))[0]
        self.assertEqual(len(uv.check_expect([r], "lock<=4,p95<=25")), 2)

    def test_no_lock_fails_expect(self):
        e = [x for x in self.entries() if "locked" not in x.get("text", "") or "conf" not in x.get("text", "")]
        e = [x for x in e if "[dance] locked" not in x.get("text", "")]
        self.assertTrue(uv.check_expect(uv.score(e), "lock<=4"))

    def test_phase_error_folds(self):
        beats = [0, 22050, 44100]
        self.assertAlmostEqual(uv.phase_error(22050 + 441, beats, 22050), 441)
        self.assertAlmostEqual(uv.phase_error(22050 - 441, beats, 22050), -441)
        self.assertAlmostEqual(uv.phase_error(66150 + 10, beats, 22050), 10)  # past the list: folded by the period
        self.assertAlmostEqual(uv.octave_error(87.0, 174.0), 0.0)
        self.assertAlmostEqual(uv.octave_error(121.2, 120.0), 1.0)

    def test_score_uses_the_epoch_a_line_names(self):
        # A gap's reset at 19.5 s, then a seek: epoch 2 starts 0.5 s later.
        # (Firmware before the fix rate-limited that reset line away.) The
        # lock and beats name epoch 2 and are scored against its truth.
        rate = 44100
        gen = uv.ClickGen(120, 0.0, rate, rate * 40)
        rx = lambda text, **kw: dict({"kind": "rx", "text": text, "t": 0.0}, **kw)
        truth = lambda n, start: {"kind": "epoch", "epoch": n, "rate": rate, "source": f"click120@{rate}",
                                  "start": start, "frames": rate * 20, "bpm": 120.0, "period": gen.period,
                                  "beats": [b - start for b in gen.beats() if b >= start]}
        shift = int(7.3 * rate)
        e = [truth(1, 0)]
        e.append(rx("[dance] tracker reset (computer: epoch 1, its first hop) at hop 0, 44100 Hz"))
        e.append(rx("[dance] locked: 120.00 BPM, 2.60 s after the reset (computer: epoch 1)"))
        e.append(rx("[dance] tracker reset (computer: epoch 1, a gap in its hops) at hop 1679, 44100 Hz"))
        e.append(truth(2, shift))
        e.append(rx("[dance] locked: 120.00 BPM, 2.90 s after the reset (computer: epoch 2)", epoch=2))
        for k, b in enumerate(truth(2, shift)["beats"][6:20]):
            frame = b + 0.5 * rate / 1000.0
            e.append(rx(f"[beat] #{k} next at {frame / rate:.3f}s (epoch 2, frame {frame:.1f}, hop {k}) "
                        f"bpm=120.00 conf=0.80 locked", epoch=2))
        rows = {r["epoch"]: r for r in uv.score(e)}
        self.assertEqual(rows[1]["lock"], 2.6)
        self.assertEqual(rows[1]["beats"], 0)
        self.assertEqual(rows[2]["lock"], 2.9)
        self.assertEqual(rows[2]["beats"], 14)
        self.assertAlmostEqual(rows[2]["med"], 0.5, places=2)
        # Lines without an epoch (older firmware) still go by the last reset.
        old = [x if not x.get("text", "").startswith(("[beat]", "[dance] locked")) else dict(x, text=re.sub(r" \(computer: epoch \d+\)|epoch \d+, ", "",
                                                                    x["text"])) for x in e]
        rows = {r["epoch"]: r for r in uv.score(old)}
        self.assertEqual(rows[2]["lock"], None)  # what the reviewer saw: credited to epoch 1

    def test_score_reads_its_own_log(self):
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "run.jsonl"
            code, _, _, _ = run_sender(["--click", "120", "--seconds", "2", "--log", str(log)])
            self.assertEqual(code, 0)
            lines = log.read_text().splitlines()
            kinds = {uv.json.loads(l)["kind"] for l in lines}
            self.assertEqual(kinds, {"epoch", "tx", "rx"})
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(uv.main(["--score", str(log)]), 0)
            self.assertIn("click120@44100", out.getvalue())


if __name__ == "__main__":
    unittest.main()
