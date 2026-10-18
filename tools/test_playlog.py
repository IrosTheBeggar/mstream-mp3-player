# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""Tests for tools/playlog.py (the play log, docs/METADATA.md 2.20, and
MSMF's ACKS, 2.5.6).

    python -m unittest discover -s tools -p "test_playlog.py"

- The slot: 80 bytes, its fields at 2.20.3's offsets, its CRC over the
  first 76; the reader's verdicts (a short tail, no magic, a newer major,
  a bad CRC, an unknown kind) and the enums' unknown values.
- The ids: core2-<12 hex>-<16 hex>-<decimal>, at most 46 characters.
- The log: each play once whatever file holds it, twins counted, two
  slots with one key both dropped, the order by (deviceId, epoch, seq),
  the files' order irrelevant.
- The device's model: the seq at boot (NVS behind or ahead of the card),
  a new epoch after an NVS erase, the rotation under the cap, its drop
  note, the trim by ACKS.
- The reader's rules: dating by the next dated play of a boot (the
  uptime wrapping), a play's file through the ledger (size, time, the
  skew), the posted mark held by a play still waiting.
- ACKS: its order (the epoch unsigned), its checks.
- The shared fixtures (test/fixtures/card/plays, vectors.json's plays
  group) as card_fixtures.py makes them, read back here.
"""
import contextlib
import io
import json
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import card_fixtures as cf  # noqa: E402
import playlog as pl  # noqa: E402

FIXTURES = Path(__file__).resolve().parent.parent / "test" / "fixtures" / "card"
DEV = "00:00:5e:00:53:01"
EPOCH = 0x1122334455667788


def play(seq, **kw):
    s = {"kind": 1, "deviceId": DEV, "epoch": EPOCH, "seq": seq, "outcome": 1, "source": 1, "clock": 1, "pauseCount": 0,
         "startedAt": 1792100000000 + seq * 1000, "bootSeq": 1, "uptimeMs": seq * 1000, "playedMs": 1000,
         "durationMs": 2000, "pathHash": 0xDA70B312760188D8, "size": 1000, "fatTime": 0x5D4773D5, "generation": 42}
    s.update(kw)
    return s


class SlotTests(unittest.TestCase):
    def test_layout(self):
        b = pl.encode(play(0x01020304, pauseCount=0x0506, epoch=0x1112131415161718, startedAt=0x2122232425262728,
                           bootSeq=0x31323334, uptimeMs=0x41424344, playedMs=0x51525354, durationMs=0x61626364,
                           pathHash=0x7172737475767778, size=0x81828384, fatTime=0x91929394, generation=0xA1A2A3A4,
                           outcome=2, source=3, clock=2))
        self.assertEqual(len(b), pl.SLOT)
        self.assertEqual(b[0:4], b"MPPL")
        self.assertEqual(struct.unpack_from("<I", b, 0)[0], 0x4C50504D)
        self.assertEqual(tuple(b[4:8]), (1, 1, 2, 3))  # major, kind, outcome, source
        self.assertEqual(struct.unpack_from("<IHBB", b, 8), (0x01020304, 0x0506, 2, 0))
        self.assertEqual(struct.unpack_from("<Q", b, 16)[0], 0x1112131415161718)
        self.assertEqual(b[24:30].hex(), "00005e005301")
        self.assertEqual(b[30:32], bytes(2))
        self.assertEqual(struct.unpack_from("<QIIII", b, 32),
                         (0x2122232425262728, 0x31323334, 0x41424344, 0x51525354, 0x61626364))
        self.assertEqual(struct.unpack_from("<QIII", b, 56), (0x7172737475767778, 0x81828384, 0x91929394, 0xA1A2A3A4))
        self.assertEqual(struct.unpack_from("<I", b, 76)[0], cf.crc(b[:76]))
        why, s = pl.decode(b)
        self.assertEqual(why, "ok")
        self.assertEqual((s["seq"], s["pathHash"], s["deviceId"]), (0x01020304, 0x7172737475767778, "00005e005301"))

    def test_round_trip(self):
        p = play(7)
        why, s = pl.decode(pl.encode(p))
        self.assertEqual(why, "ok")
        self.assertEqual({k: s[k] for k in pl.PLAY_FIELDS if k != "deviceId"},
                         {k: p[k] for k in pl.PLAY_FIELDS if k != "deviceId"})
        note = pl.encode({"kind": 2, "deviceId": DEV, "epoch": EPOCH, "dropped": 12, "bootSeq": 3, "uptimeMs": 9,
                          "startedAt": 5, "clock": 1})
        why, n = pl.decode(note)
        self.assertEqual((why, n["dropped"], n["bootSeq"]), ("ok", 12, 3))
        self.assertEqual(note[6:16], bytes(8) + b"\x01\x00", "outcome, source, seq, pauseCount 0; the clock")
        self.assertEqual(note[52:76], bytes(24), "a drop note's play fields are 0")

    def test_verdicts(self):
        good = pl.encode(play(1))
        self.assertEqual(pl.decode(good[:79])[0], "short")
        self.assertEqual(pl.decode(bytes(80))[0], "magic")
        torn = bytearray(good)
        torn[40] ^= 0x10
        self.assertEqual(pl.decode(bytes(torn))[0], "crc")
        newer = bytearray(good)
        newer[4] = 2
        self.assertEqual(pl.decode(bytes(newer))[0], "newer")
        self.assertEqual(pl.decode(pl.encode(play(1, kind=9)))[0], "kind")
        # Every flipped bit makes the slot not "ok".
        for i in range(80):
            for bit in range(8):
                b = bytearray(good)
                b[i] ^= 1 << bit
                self.assertNotEqual(pl.decode(bytes(b))[0], "ok", (i, bit))

    def test_enums(self):
        self.assertEqual([pl.outcome_name(v) for v in (0, 1, 2, 3, 4, 255)],
                         ["stopped", "completed", "skipped", "stopped", "stopped", "stopped"])
        self.assertEqual([pl.source_name(v) for v in (0, 1, 2, 3, 4)],
                         ["other", "manual", "shuffle", "autodj", "other"])

    def test_ids(self):
        self.assertEqual(pl.play_id(play(42)), "core2-00005e005301-1122334455667788-42")
        longest = pl.play_id({"deviceId": "ff:ff:ff:ff:ff:ff", "epoch": (1 << 64) - 1, "seq": (1 << 32) - 1})
        self.assertEqual(len(longest), 46)
        self.assertEqual(pl.play_id({"deviceId": "00005E005301", "epoch": 1, "seq": 1}),
                         "core2-00005e005301-0000000000000001-1")

    def test_caps(self):
        self.assertLessEqual(pl.FILE_SLOTS * pl.SLOT, 512 * 1024)
        self.assertGreater((pl.FILE_SLOTS + 1) * pl.SLOT, 512 * 1024)
        self.assertLessEqual(2 * pl.FILE_SLOTS * pl.SLOT, pl.LOG_BYTES)


class LogTests(unittest.TestCase):
    def test_once_each_in_order(self):
        a, b = pl.encode(play(2)), pl.encode(play(1))
        other = pl.encode(play(1, deviceId="00:00:5e:00:53:00"))
        files = {"plays.jnl": a + b, "plays.1.jnl": b + other, "plays.3.jnl": a}
        log = pl.read_log(files)
        self.assertEqual([pl.play_id(p) for p in log["plays"]], ["core2-00005e005300-1122334455667788-1",
                                                                  "core2-00005e005301-1122334455667788-1",
                                                                  "core2-00005e005301-1122334455667788-2"])
        self.assertEqual(log["counts"]["twins"], 2)
        # The files' names and order don't matter.
        again = pl.read_log({"plays.1.jnl": a + b, "plays.jnl": b + other, "plays.2.jnl": a})
        self.assertEqual(again, log)

    def test_conflict_drops_both(self):
        log = pl.read_log({"plays.jnl": pl.encode(play(1)) + pl.encode(play(1, playedMs=5)) +
                           pl.encode(play(1, playedMs=6)) + pl.encode(play(2))})
        self.assertEqual([p["seq"] for p in log["plays"]], [2])
        self.assertEqual(log["counts"]["conflicts"], 1)

    def test_skips_and_counts(self):
        data = (pl.encode(play(1)) + bytes(80) + pl.encode(play(0)) + pl.encode(play(2, epoch=0)) +
                pl.encode(play(3, kind=7)) + pl.encode(play(4)) + b"\x01\x02")
        log = pl.read_log({"plays.jnl": data})
        self.assertEqual([p["seq"] for p in log["plays"]], [1, 4])
        c = log["counts"]
        self.assertEqual((c["slots"], c["damaged"], c["unusable"], c["unknownKinds"], c["tail"]), (6, 1, 2, 1, 1))

    def test_boot_seq(self):
        plays = pl.read_log({"plays.jnl": pl.encode(play(5)) + pl.encode(play(9, epoch=7))})["plays"]
        self.assertEqual(pl.boot_seq(0, plays, DEV, EPOCH), 6)
        self.assertEqual(pl.boot_seq(8, plays, DEV, EPOCH), 9)
        self.assertEqual(pl.boot_seq(0, plays, DEV, 7), 10)
        self.assertEqual(pl.boot_seq(2, plays, "00:00:5e:00:53:02", EPOCH), 3)

    def test_dates(self):
        plays = pl.read_log({"plays.jnl": b"".join(pl.encode(p) for p in [
            play(1, startedAt=0, clock=0, uptimeMs=0xFFFFF000),
            play(2, startedAt=0, clock=0, uptimeMs=0xFFFFF800),
            play(3, startedAt=1792100000000, clock=2, uptimeMs=0x1000),
            play(4, startedAt=1792100500000, clock=2, uptimeMs=0x2000),
            play(5, startedAt=0, clock=0, bootSeq=2, uptimeMs=50),
            play(6, startedAt=1792200000000, bootSeq=3, uptimeMs=60)])})["plays"]
        d = pl.dates(plays)
        by = {k[2]: v for k, v in d.items()}
        self.assertEqual(by[1], 1792100000000 - 0x2000)
        self.assertEqual(by[2], 1792100000000 - 0x1800)
        self.assertEqual(by[3], 1792100000000)
        self.assertIsNone(by[5], "another boot's dated play doesn't date it")
        self.assertEqual(by[6], 1792200000000)


class DeviceTests(unittest.TestCase):
    def test_rotation_and_the_drop_note(self):
        d = pl.Device(DEV, [EPOCH], cap=3)
        d.boot(clock=1)
        for i in range(7):
            d.play(1, 1000, 0x5D4773D5, 42, 1000, 2000, 1, 1, 1000 * i, 1792100000000 + 1000 * i)
        # 3 + 3 + 1, the first file's three lost at the second rotation.
        log = pl.read_log(d.files)
        self.assertEqual([p["seq"] for p in log["plays"]], [4, 5, 6, 7])
        self.assertEqual(log["counts"]["dropped"], 3)
        first = pl.decode(d.files["plays.jnl"][:80])[1]
        self.assertEqual((first["kind"], first["dropped"], first["uptimeMs"]), (2, 3, 6000))
        self.assertEqual(len(d.files["plays.1.jnl"]), 3 * 80)
        self.assertEqual([o for o in d.ops if o[0] != "play"],
                         [("epoch", "%016x" % EPOCH), ("rename", "plays.jnl", "plays.1.jnl"),
                          ("delete", "plays.1.jnl", "rotation"), ("rename", "plays.jnl", "plays.1.jnl"), ("note", 3)])

    def test_acknowledged_plays_go_without_a_note(self):
        d = pl.Device(DEV, [EPOCH], cap=3)
        d.boot(clock=1)
        acks = [(pl.device_bytes(DEV).hex(), EPOCH, 3)]
        for i in range(7):
            d.play(1, 1000, 0x5D4773D5, 42, 1000, 2000, 1, 1, 1000 * i, 1792100000000 + 1000 * i, acks=acks)
        self.assertEqual(pl.read_log(d.files)["counts"]["dropped"], 0)
        # A boot with ACKS covering a whole file deletes it.
        d.boot(clock=1, acks=[(pl.device_bytes(DEV).hex(), EPOCH, 6)])
        self.assertEqual(sorted(d.files), ["plays.jnl"])
        self.assertEqual([p["seq"] for p in pl.read_log(d.files)["plays"]], [7])

    def test_seq_at_boot_and_a_new_epoch(self):
        d = pl.Device(DEV, [EPOCH, 99], cap=10)
        d.boot()
        d.play(1, 1000, 0x5D4773D5, 42, 1000, 2000, 1, 1, 10)
        d.play(1, 1000, 0x5D4773D5, 42, 1000, 2000, 1, 1, 20)
        d.nvs["seq"] = 0                      # NVS behind the card (its write lost)
        d.boot()
        self.assertEqual(d.play(1, 1000, 0x5D4773D5, 42, 1000, 2000, 1, 1, 30)["seq"], 3)
        self.assertEqual(d.nvs["boots"], 2)
        d.erase_nvs()
        d.boot()
        s = d.play(1, 1000, 0x5D4773D5, 42, 1000, 2000, 1, 1, 40)
        self.assertEqual((s["epoch"], s["seq"], s["bootSeq"]), (99, 1, 1))
        self.assertEqual(len(pl.read_log(d.files)["plays"]), 4, "the new epoch's seq 1 is another play")
        # No clock: startedAt 0 whatever the caller says.
        d.clock = 0
        self.assertEqual(d.play(1, 1000, 0x5D4773D5, 42, 1000, 2000, 1, 1, 50, 1792100000000)["startedAt"], 0)


class ResolveTests(unittest.TestCase):
    LEDGER = [{"pathHash": 0xDA70B312760188D8, "size": 1000, "fatTime": 0x5D4773D5, "serverPath": "Music/a.mp3",
               "server": 0},
              {"pathHash": 0x1111, "size": 5, "fatTime": 0x5D4773D5, "serverPath": "Music/b.mp3", "server": 2},
              {"pathHash": 0x2222, "size": 5, "fatTime": 0x5D4773D5, "serverPath": "Music/c.mp3", "server": 0},
              {"pathHash": 0x2222, "size": 5, "fatTime": 0x5D4773D5, "serverPath": "Music/d.mp3", "server": 0}]

    def resolve(self, slots, skew=0, posted=None):
        plays = pl.read_log({"plays.jnl": b"".join(pl.encode(s) for s in slots)})["plays"]
        return plays, pl.resolve(plays, self.LEDGER, skew, posted)

    def test_verdicts(self):
        hour = cf.fat_from_wall(cf.fat_wall(0x5D4773D5) + 3600)
        _, got = self.resolve([play(1), play(2, size=1001), play(3, fatTime=hour), play(4, pathHash=0x3333),
                               play(5, pathHash=0x2222, size=5), play(6, startedAt=0, clock=0, bootSeq=9),
                               play(7, pathHash=0x1111, size=5, durationMs=0), play(8, fatTime=0)])
        self.assertEqual([e["send"] for e in got], [True, False, False, False, False, False, True, False])
        self.assertEqual(got[0]["filePath"], "Music/a.mp3")
        self.assertEqual((got[6]["server"], "durationMs" in got[6]), (2, False))
        self.assertIn("listener", got[3]["why"])
        self.assertIn("share", got[4]["why"])
        self.assertIn("clock", got[5]["why"])
        _, skewed = self.resolve([play(3, fatTime=hour)], skew=3600)
        self.assertTrue(skewed[0]["send"])
        _, after = self.resolve([play(1), play(2)], posted={(pl.device_bytes(DEV).hex(), EPOCH): 1})
        self.assertEqual([e["id"] for e in after], ["core2-00005e005301-1122334455667788-2"])

    def test_the_mark_waits(self):
        plays, got = self.resolve([play(1), play(2, size=9), play(3), play(5), play(6)])
        key = (pl.device_bytes(DEV).hex(), EPOCH)
        ids = [e["id"] for e in got]
        self.assertEqual(pl.next_mark(plays, got, {}, []), {}, "nothing settled: no mark")
        self.assertEqual(pl.next_mark(plays, got, {}, [ids[0]]), {key: 2}, "2 isn't sent: settled")
        self.assertEqual(pl.next_mark(plays, got, {}, [ids[0], ids[3]]), {key: 2}, "3 still waits")
        self.assertEqual(pl.next_mark(plays, got, {}, [ids[0], ids[2], ids[3], ids[4]]), {key: 6},
                         "4 isn't on the card: no wait")


class AcksTests(unittest.TestCase):
    def test_order_and_checks(self):
        rows = pl.acks_bytes([("00:00:5e:00:53:02", 1, 3), (DEV, 0xF000000000000000, 2), (DEV, 0x7000000000000000, 1)])
        got = pl.read_acks(rows, 3, 24)
        self.assertEqual(got, [("00005e005301", 0x7000000000000000, 1), ("00005e005301", 0xF000000000000000, 2),
                               ("00005e005302", 1, 3)])
        self.assertEqual(rows[:24].hex(), "00005e005301" + "0000" + "0000000000000070" + "01000000" + "00000000")
        self.assertIsNone(pl.read_acks(rows[24:48] + rows[:24] + rows[48:], 3, 24))
        self.assertIsNone(pl.read_acks(rows, 3, 16))
        self.assertIsNone(pl.read_acks(rows[:48], 3, 24))
        self.assertIsNone(pl.read_acks(rows[:24] + rows[:24], 2, 24), "a (deviceId, epoch) twice")
        with self.assertRaises(AssertionError):
            pl.acks_bytes([(DEV, 1, 1), (DEV, 1, 2)])


class FixtureTests(unittest.TestCase):
    """test/fixtures/card/plays and vectors.json's plays group, as
    card_fixtures.py makes them (its --check keeps them frozen)."""

    def test_goldens_are_the_script(self):
        golden = {p.name: p.read_bytes() for p in (FIXTURES / "golden").iterdir()}
        snaps, _ = cf.run_plays(json.loads((FIXTURES / "plays" / "log.json").read_text(encoding="utf-8")))
        for snap, files in snaps.items():
            for name, data in files.items():
                self.assertEqual((FIXTURES / "plays" / "golden" / snap / name).read_bytes(), data, (snap, name))
        self.assertEqual(cf.plays_script(golden), json.loads((FIXTURES / "plays" / "log.json").read_text("utf-8")))

    def test_reader_cases(self):
        index = json.loads((FIXTURES / "plays" / "index.json").read_text(encoding="utf-8"))
        self.assertGreaterEqual(len(index), 12)
        for e in index:
            files = {name: (FIXTURES / "plays" / path).read_bytes() for name, path in e["files"].items()}
            log = pl.read_log(files)
            self.assertEqual([pl.play_id(p) for p in log["plays"]], e["ids"], e["case"])
            self.assertEqual(log["counts"], e["counts"], e["case"])
            when = {pl.play_id(p): ms for p in log["plays"] for k, ms in pl.dates(log["plays"]).items()
                    if k == pl.key(p)}
            for i, ms in e.get("dates", {}).items():
                self.assertEqual(when[i], ms, (e["case"], i))
            for i, names in e.get("names", {}).items():
                p = next(p for p in log["plays"] if pl.play_id(p) == i)
                self.assertEqual([pl.outcome_name(p["outcome"]), pl.source_name(p["source"])], names)

    def test_vectors(self):
        golden = {p.name: p.read_bytes() for p in (FIXTURES / "golden").iterdir()}
        v = json.loads((FIXTURES / "vectors.json").read_text(encoding="utf-8"))
        bad = []
        cf.check_plays(v["plays"], golden, lambda ok, what: ok or bad.append(what))
        self.assertEqual(bad, [])
        hardening = {p.name: p.read_bytes() for p in (FIXTURES / "hardening").iterdir()}
        for e in v["plays"]["acks"]["hardening"]:
            want = None if e["acks"] is None else [(a["deviceId"], cf.u64(a["epoch"]), a["seq"]) for a in e["acks"]]
            self.assertEqual(cf.root_acks(hardening[e["file"]]), want, e["file"])
        index = json.loads((FIXTURES / "hardening" / "index.json").read_text(encoding="utf-8"))
        for e in index:
            if e["file"].startswith("msmf-acks-"):
                self.assertEqual((e["expect"], e["why"]), ("present", "ok"), e["file"])

    def test_command_line(self):
        out = io.StringIO()
        paths = [str(FIXTURES / "plays" / "golden" / "end" / n) for n in ("plays.jnl", "plays.1.jnl")]
        with contextlib.redirect_stdout(out):
            self.assertEqual(pl.main(["read"] + paths), 0)
            self.assertEqual(pl.main(["dump", paths[0]]), 0)
        lines = [json.loads(line) for line in out.getvalue().splitlines()]
        self.assertEqual(lines[5]["counts"]["dropped"], 4)
        self.assertEqual(lines[0]["id"], "core2-00005e005301-f0e1d2c3b4a59687-5")


if __name__ == "__main__":
    unittest.main()
