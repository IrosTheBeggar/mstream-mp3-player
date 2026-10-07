# Card contract fixtures

SPDX-License-Identifier: CC0-1.0. To the extent possible under law, the
authors have waived all copyright and related or neighboring rights to the
files in this folder (the CC0 1.0 Universal dedication,
https://creativecommons.org/publicdomain/zero/1.0/), so the player,
mstream-terminal and mStream can all copy them (docs/METADATA.md part 7,
U18, proposed). The player's code stays GPL-3.0-or-later.

The shared fixtures of docs/METADATA.md 2.17, made by
`tools/card_fixtures.py` (a second implementation of the contract, apart
from `lib/core/CardContract*`) and frozen: `python tools/card_fixtures.py
--check` fails if a file here differs from what it makes. Made-up names
only, synthetic embeddings.

- `vectors.json`: the vectors of 2.18 as data. `who` says which side a
  group binds when it isn't both.
- `libraries/*.json`: library descriptions, the input of a writer: every
  value a writer would otherwise choose is given (ids, times,
  generations). Tag values are raw lists: each writer applies 2.3.6 and
  sets TRUNCATED; flags, `known` and the enums are as given. Integers above
  2^53 are hex strings. A manifest names its companions by file; their
  generation, length, header CRC and (MPDJ) selection signature come from
  the golden companion.
- `golden/`: what a writer MUST produce from each description, byte for
  byte (2.17, item 2). The MPTH pixels are the synthetic picture the
  description names: the scaling filter isn't pinned.
- `hardening/`: files broken in one way each, with valid CRCs (2.17, item
  4), at least one for every check of 2.4.3, so a reader that skips a
  check fails on its file. `index.json` lists each with the reader's uses
  (`all`: every section; `device`: FOLD, RECS and STRS only), whether it
  must read as absent or present, the check it breaks, and the player's
  reason code. A `present` file with `same` reads as the same records as
  that golden file.

The player's tests: `test/test_card_contract` (the vectors) and
`test/test_card_files` (the goldens, round trips, the hardening files,
truncation at every byte and every flipped bit).
