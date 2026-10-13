# Auto DJ engine fixtures

SPDX-License-Identifier: CC0-1.0. To the extent possible under law, the
authors have waived all copyright and related or neighboring rights to the
files in this folder (the CC0 1.0 Universal dedication,
https://creativecommons.org/publicdomain/zero/1.0/). The player's code
stays GPL-3.0-or-later.

Made by `tools/autodj_ref.py`, the pure-Python reference of the Auto DJ
engine's pick rule, and frozen: `python tools/autodj_ref.py --check` fails
if a file here differs from what it makes. `test/test_autodj_engine`
replays every session on `lib/core/AutoDjEngine` and must get every pick,
every prefetch read and the counters exactly; `tools/test_autodj_ref.py`
tests the reference itself.

`sessions.json`:
- `rng`: PCG32 (pcg32_srandom_r(seed, stream), XSH RR) outputs and
  `below(n)` draws (Lemire's method), the engine's one stream.
- `tables`: `golden` names a file in `test/fixtures/card/golden/` (read
  through `mpdj::Table`); otherwise a synthetic table's rows, k, selHead,
  selFloor and each row's used slot entries as hex, 6 digits an entry (the
  row, then the level).
- `sessions`: each a table (none: no index), the maps (`trackToRow`,
  `rowToTrack`, 65535 none), the filter words (bpm | camelot << 8 |
  artist16 << 16; BPM_ANALYSED and "present" in bits 13 and 15), the
  library's track count, the faults the slot reader injects by row
  (`damaged`: the CRC always fails; `flaky`: on the first read only;
  `io`: the first read fails, Why::Io; `broken`: a good CRC over a bad
  list; a `faults` event replaces them), the events,
  and the counters at the end. Events: `new` (a session: its PCG32 seed
  and seed track, 4294967295 none), `start` (a track starts: `dj`, Auto
  DJ's pick), `left` (a track leaves: ms played, a natural end or not),
  `mode` (Anchor changed), `reopen` (the file reopened), `renumber` (the
  library rebuilt: the by-id cooldown cleared), `index` (`on` 0: the index
  away from now, the engine given no maps and no slots; 1: back), `prefetch` (one loop pass
  in mode `m`: the row it reads, 65535 none, and the read's result),
  `pick` and `surprise` (the settings, the playing track, and every field
  of the result, a field left out being 0).

Synthetic data only: no real names, no embeddings.
