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
--check` fails if a file here differs from what it makes, or if a file
here isn't one it makes. Writing them (`python tools/card_fixtures.py`)
deletes any such file, `.gitattributes` excepted, so keep work in progress
elsewhere. Made-up names only, synthetic embeddings.

- `vectors.json`: the vectors of 2.18 as data. `who` says which side a
  group binds when it isn't both.
- `libraries/*.json`: library descriptions, the input of a writer: every
  value a writer would otherwise choose is given (ids, times,
  generations). The tags descriptions' tag values are raw lists: each
  writer applies 2.3.6 and sets TRUNCATED; flags, `known` and the enums are
  as given. The AutoDJ descriptions' titles and artists are instead an MPTG
  record's values as stored, 2.3.6 already applied: an MPDJ writer cuts,
  replaces and drops nothing in them (AUTODJ-INDEX.md 13.2). Integers above
  2^53 are hex strings. A manifest names its companions by file; their
  generation, length, header CRC and (MPDJ) selection signature come from
  the golden companion. A manifest's `servers` are its SRVS entries in
  order (server i is entry i - 1), each key computed from the URL given
  (`serverUrl`, `parentUrl`: 2.5.1), 0 where none is; a ledger's `server`
  is ORIG's server index, 0 (the root's own server) when absent (2.5.5,
  2.6.6).
- `golden/`: what a writer MUST produce from each description, byte for
  byte (2.17, item 2). The MPTH pixels are the synthetic picture the
  description names: the scaling filter isn't pinned.
- `hardening/`: files broken in one way each, with valid CRCs (2.17, item
  4), at least one for every check of 2.4.3, so a reader that skips a
  check fails on its file. `index.json` lists each with the reader's uses
  (`all`: every section; `device`: MPTG's FOLD, RECS and STRS, MPDJ's
  DJRF, DJPH and STRS, MSMF's COMP, LIBR and STRS: the device never reads
  ORIG, OSTR or SRVS), whether it must read as absent or present, the
  check it breaks, and the player's reason code. A `present` file with
  `same` reads as the same records as that golden file (MPDJ: every row's
  filter record and used slot entries, every DJPH hash's row, the five
  strings). An MPDJ file's `slot` says how one slot reads when the device
  reads it alone (`damaged`: its CRC fails; `broken`: a good CRC over a
  bad list).

The AutoDJ index (MPDJ v2) is specified apart, in docs/AUTODJ-INDEX.md;
its fixtures (section 13 there) come from `tools/mpdj.py`, which
card_fixtures.py imports. `libraries/autodj-mid.f32` holds the vectors
that `autodj-mid.json` and `autodj-mid-default.json` name (raw
little-endian f32, 16 a row, in the descriptions' row order).

The player's tests: `test/test_card_contract` (the vectors),
`test/test_card_files` (the goldens, round trips, the hardening files,
truncation at every byte and every flipped bit), `test/test_card_autodj`
(the AutoDJ index's own rules) and `test/test_name_key` (the keys the
AutoDJ descriptions give).
