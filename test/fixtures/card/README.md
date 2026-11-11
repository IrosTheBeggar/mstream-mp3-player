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
  DJRF, DJPH and STRS, MSMF's COMP, LIBR and STRS, MPWF's frame: the device
  never reads ORIG, OSTR or SRVS, and a slot of DJSL or WVSL only when it
  needs one), whether it must read as absent or present, the check it
  breaks, and the player's reason code. A `present` file with
  `same` reads as the same records as that golden file (MPDJ: every row's
  filter record and used slot entries, every DJPH hash's row, the five
  strings; MPWF: every slot's hash, length and bars). An MPDJ file's `slot`
  says how one slot reads when the device reads it alone (`damaged`: its
  CRC fails; `broken`: a good CRC over a bad list). An MPWF file's `slot` is
  one lookup as the device makes it: the hash, what it reads (`hit`, `miss`,
  `damaged`, `broken`) and how many slots it reads.
- `moods.json`, `golden/tags-moods.bin` and the hardening files
  `mptg-moods-longer`, `mptg-tail-invalid` and `mptg-stride-76`: mood
  filters (docs/MOOD-FILTERS.md 5.3, 5.4; METADATA 2.6.4's mood tail and
  3.4.3's mood word), made with `tools/moodfilter.py`, which
  card_fixtures.py imports. A tags description's record `scores` are
  mStream's eight mood scores in `moods.json`'s `heads` order (null where a
  head has none), or null for no server row: each writer derives the
  record's tail from them (RECS rows of 80 bytes, minor 1). An MPTG file's
  `same` covers its records' scores too. In `moods.json`: `values` (a
  score's value byte and bucket), `levels` (each level's code in a filter,
  its `need` and `under`), `bars` (its `on` and `mirror`), `predicates` (the
  eleven of MOOD-FILTERS.md 4.1, each a one-group filter's bits at
  Standard), `rows` (scores, the tail a writer makes of them, the word a
  builder makes of the tail, and `holds`: for each level, `strict`,
  `standard` and `loose`, the predicates that hold, in `predicates` order;
  any other fails), `tails` (whether a reader takes each, and its word),
  `filters` (a filter's bits, what they read as, and the rows it
  `matches`, by index from 0) and `albums` (rows as one album's tracks
  under a filter: `pass` of `analysed`, and whether 2 × pass > analysed).
  `tools/test_mood_vectors.py` checks every `holds` and `matches` against
  mStream's own predicate, run by SQLite.

The AutoDJ index (MPDJ v2) is specified apart, in docs/AUTODJ-INDEX.md;
its fixtures (section 13 there) come from `tools/mpdj.py`, which
card_fixtures.py imports. `libraries/autodj-mid.f32` holds the vectors
that `autodj-mid.json` and `autodj-mid-default.json` name (raw
little-endian f32, 16 a row, in the descriptions' row order).

The waveform index (MPWF v1) is specified apart, in
docs/WAVEFORM-INDEX.md; its fixtures (section 13 there) come from
`tools/mpwf.py`, which card_fixtures.py imports. A description's tracks
hold the server's peak bars as given, and a writer computes the bars.
`golden/manifest-waves.bin` is a root that names all three companions. The
`msmf-waves-*` hardening files are roots that read whatever their MPWF row
holds (`vectors.json`'s `mpwf.rootRow` says what a reader takes from each),
but for the two whose name breaks a string rule, as any kind's would.

The play log (docs/METADATA.md 2.20) has its fixtures in `plays/`, made
with `tools/playlog.py`, its reference reader and writer, which
card_fixtures.py imports. `plays/log.json` is one device's script on the
fixture card (every value a writer would otherwise choose given; a cap of
4 slots a file, so the rotation shows). `plays/golden/<snapshot>/` holds
the log's files at each of its snapshots: what a writer MUST make of the
script, byte for byte. `plays/index.json` lists the reader's cases, each
a card's log files (a golden one by its path, a case's own in
`plays/read/<case>/`) with what a reader takes from them: the plays' ids
in order, the counts of what it skipped, and where given the plays'
starts and what their enums read as. `golden/manifest-acks.bin` is a root
with ACKS (2.5.6); the `msmf-acks-*` hardening files are roots that
read, whatever their ACKS holds (`vectors.json`'s `plays.acks` says what
a reader that uses ACKS takes from each). So a reader takes ACKS outside
its container's check of known sections, whose stride and length rules
would make the root absent: it finds ACKS by its type in the directory
and checks it on its own.

The listening stats (docs/LISTENING-STATS.md: `/.player/stats.bin` and
its base file, 5.2-5.6) have their fixtures in `stats/`, made with
`tools/statsref.py`, their reference, which card_fixtures.py imports.
`stats/index.json` lists the scenarios. Each is a card's `/.player` before
its first boot (the log's files, and a `stats.bin` and its base where an
earlier boot left them), the booting device (`device`: its deviceId and
epoch, which only the boot rules use: the stats fold every device's
plays) and its boots in turn. A boot gives the files it starts from (`in`)
and leaves (`out`), each name mapped to the folder that holds its bytes (a
file is stored once, in the first folder with those bytes; files changed
between two boots, a play the device wrote, are named in `added` and
stored in `<boot>-in`); its inputs (`acks`: the root's ACKS rows; `now`:
the board's clock at the save in Unix ms, null while unset, and `today`,
its UTC day counted from 2026-01-01; `commitTime`: the root's, Unix s;
`playing` and `safePoint`, for the compaction's rule; `saveFails`: a full
card; `unread`: the scan's card faults, each log file's name mapped to the
slots read before a read failed, or null for an open that failed); and
what a device MUST make of them: `out` byte for byte, what the read found
(`read`: loaded, absent, corrupt, newer or unread, and `why`, the check
that failed by CardContainer's name), the slots folded, met a second time,
and missing under ACKS (`folded`, `twice`, `gap`), whether the scan was
partial (`partial`: no save follows), whether `stats.bin` was written
(`saved`) and the base compacted, the files removed and the log files
trimmed, and, read back where it reads, HEAD and MARK (`stats`), the
shelves' lists (`lists`, by path hash, each its cap plus 16: `mostPlayed`
per window [hash, plays in it]; `newFinds` [hash, plays, firstAt, its
source]; `oldFavourites` [hash, plays, lastAt]; `recentlyPlayed` [hash,
plays, lastAt, lastOrd]; `notPlayed` by album) and the hub's numbers
(`view`). Each log file keeps one device and epoch's seqs in order, as a
device writes them (5.6's ranges rely on it). `stats/library.json` is the
made-up library behind Not played yet: its albums in A-Z order, each
track's path and hash. `stats/sessions.json` lists the sessions: players'
boots and plays in turn on one card (`in`, under `stats/sessions/`), the
play log written as tools/playlog.py's Device writes it and the stats kept
as a device that keeps them does (the live fold of each play written, the
rotation's save before plays.1.jnl leaves the log's names, `Hs`, the
orderly end's flush, another player's `stats.bin` refusing a session's
later saves, a 0.14.0 boot, NVS put back or erased, a log file deleted by
hand). Each player gives its deviceId, the epochs a fresh NVS draws and
its NVS as the session starts (null: erased); each step its own fields (a
boot's as a boot's above, and `stats` false for a 0.14.0 boot; a play's
slot fields, `now` at a save it brings, `saveFails`, and `ending` with the
budget `left` for the end's flush) and what a device MUST make of it: the
card after it (`out`), a boot's facts as above, a play's seq and its
rotation's save (`save`: null when none is asked, else whether it landed),
Hs's `ok`. `slots` and `lost`: the slots that count the card ever held and
those the stats lose (5.5's undercount, to the slot). Nothing but the
device reads these files: a mirror of this folder MAY leave `stats/` out
(docs/METADATA.md 2.17, item 10).

The player's tests: `test/test_card_contract` (the vectors),
`test/test_card_files` (the goldens, round trips, the hardening files,
truncation at every byte and every flipped bit), `test/test_card_autodj`
(the AutoDJ index's own rules), `test/test_card_waves` (the waveform
index's own rules) and `test/test_name_key` (the keys the AutoDJ
descriptions give). The play log's fixtures are for the
firmware's log and the transfer software's reader, neither built yet;
`tools/test_playlog.py` reads them today. The stats' fixtures are for the
firmware's stats (docs/LISTENING-STATS.md S1-S2), not built yet;
`tools/test_statsref.py` checks them today.
